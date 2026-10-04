#!/usr/bin/env python3
"""Restore the operands of x87 arithmetic the lifter collapsed to one form.

For 3,626 sites the generator emitted every `fadd`/`fsub`/`fmul`/`fdiv` as the
*popping two-register* form, whatever the instruction actually was:

    fp_st1() -= fp_top(); fp_pop(); /* fsub */

The annotation carries only a mnemonic, so the operand is gone from the
generated source entirely.  Two things are wrong at once:

  * the operand.  `fsub DWORD PTR [ebp+0xc]` subtracts a value in memory from
    ST(0); the emitted code subtracts ST(0) from ST(1) instead.
  * the stack.  The non-popping mnemonics (`fadd`, `fsub`, `fmul`, `fdiv` --
    2,488 of the sites) never pop.  Every one of them shortens the x87 stack by
    one, so every later `st(N)` in that function refers to the wrong slot.

`Matrix_BuildOrthographicProjection` is a worked example: it builds the 2D
projection used for screen-space text, and its `fsub DWORD PTR [ebp+0xc]` --
the `right - left` term -- became `ST1 - ST0` with a spurious pop.

The operand cannot be recovered from the generated source, so this tool goes
back to the game's own bytes.  For each translated function it disassembles the
original instruction range named in the function's `Original:` header, lists the
x87 arithmetic in order, lists the corresponding sites in the generated body in
order, and pairs them.  A function is rewritten only when the two sequences
agree in length *and* mnemonic at every position; anything else is reported and
left untouched, because a mispaired operand is worse than a missing one.

    fixfpmem.py --dry-run          show every rewrite without applying
    fixfpmem.py --report           only summarise what pairs and what does not
    fixfpmem.py                    apply

Operand semantics are Intel's: `FSUB dst,src` is `dst = dst - src` and
`FSUBR dst,src` is `dst = src - dst`, so the one-operand `fsub st(i)` spelling
means `st, st(i)` (the stack top is the destination) while the popping `fsubp
st(i)` means `st(i), st`.
"""

import argparse
import ssxpaths
import glob
import io
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GEN = os.path.normpath(
    ssxpaths.GEN)
DEFAULT_XBE = os.path.normpath(
    ssxpaths.XBE)

FUNC = re.compile(r"^void (?P<name>[A-Za-z_][A-Za-z0-9_]*)\(void\)$")
ORIGIN = re.compile(r"^\s*\*\s*Original:\s*0x(?P<start>[0-9A-Fa-f]{8})\s*-\s*"
                    r"0x(?P<end>[0-9A-Fa-f]{8})")

# The collapsed form: two registers, always popping, mnemonic-only comment.
AMBIG = re.compile(r"^(?P<ind>\s*)fp_st1\(\) (?P<op>[-+*/])= fp_top\(\); "
                   r"fp_pop\(\); /\* (?P<mn>f[a-z]+) \*/\s*$")

# Any generated line that is an x87 arithmetic site, collapsed or not.  Used to
# build the ordered list that gets paired against the disassembly.
ARITH_MN = ("fadd", "faddp", "fsub", "fsubp", "fsubr", "fsubrp",
            "fmul", "fmulp", "fdiv", "fdivp", "fdivr", "fdivrp",
            "fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr")
# Longest mnemonic first: an alternation with "fadd" before "faddp" would
# match the shorter one and silently mis-pair every popping site.
SITE = re.compile(r"/\* (?:FPU: )?(?P<mn>"
                  + "|".join(sorted(ARITH_MN, key=len, reverse=True))
                  + r")(?![a-z])(?P<rest>(?:[^*]|\*(?!/))*)\*/")

ST0 = "fp_top()"
ST1 = "fp_st1()"


def st(i):
    if i == 0:
        return ST0
    if i == 1:
        return ST1
    return "_fp_stack[(_fp_top + %d) & 7]" % i


def xbe_sections(data):
    base = struct.unpack_from("<I", data, 0x104)[0]
    count = struct.unpack_from("<I", data, 0x11C)[0]
    table = struct.unpack_from("<I", data, 0x120)[0] - base
    out = []
    for i in range(count):
        off = table + i * 0x38
        _f, va, vsize, raw, _r = struct.unpack_from("<IIIII", data, off)
        out.append((va, vsize, raw))
    return out


def va_to_raw(sections, va):
    for s_va, s_size, s_raw in sections:
        if s_va <= va < s_va + s_size:
            return s_raw + (va - s_va)
    return None


def disassemble(data, sections, start, end):
    raw = va_to_raw(sections, start)
    if raw is None:
        return []
    tmp = os.path.join(HERE, ".fpmem_tmp.bin")
    io.open(tmp, "wb").write(data[raw:raw + (end - start)])
    try:
        out = subprocess.run(
            ["objdump", "-D", "-b", "binary", "-m", "i386", "-M", "intel",
             "--adjust-vma=0x%X" % start, tmp],
            capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass
    insns = []
    for line in out.split("\n"):
        m = re.match(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(\S+)\s*(.*)$", line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    return insns


def addr_expr(text):
    """objdump's `DWORD PTR [esp+0x24]` -> the generator's `esp + 0x24`."""
    m = re.match(r"^(?:BYTE|WORD|DWORD|QWORD|TBYTE) PTR (.+)$", text)
    if not m:
        return None, None
    body = m.group(1)
    size = text.split(" PTR ")[0].strip().lower()
    body = re.sub(r"^ds:", "", body)
    if body.startswith("[") and body.endswith("]"):
        body = body[1:-1]
    else:
        # A bare `ds:0x1a9f34` with no brackets is a plain absolute address.
        if not re.match(r"^0x[0-9a-fA-F]+$", body):
            return None, None
    body = body.replace("*1", "")
    # Reject anything with a segment override or an unexpected token: a wrong
    # address here would be silent, so refuse rather than guess.
    if re.search(r"[a-z]{2}:", body):
        return None, None
    if not re.match(r"^[a-z0-9_+\-*x ]+$", body):
        return None, None
    body = re.sub(r"\s*\+\s*", " + ", body)
    body = re.sub(r"\s*-\s*", " - ", body)
    return size, body.strip()


def read_expr(size, addr, integer):
    if integer:
        if size == "dword":
            return "(double)SMEM32(%s)" % addr
        if size == "word":
            return "(double)SMEM16(%s)" % addr
        return None
    if size == "dword":
        return "(double)MEMF(%s)" % addr
    if size == "qword":
        return "MEMD(%s)" % addr
    return None


OPS = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/",
       "fiadd": "+", "fisub": "-", "fimul": "*", "fidiv": "/"}


def build(mn, ops):
    """C statement for a disassembled x87 arithmetic instruction."""
    base = mn
    popping = base.endswith("p") and base not in ("fiadd",)
    if popping:
        base = base[:-1]
    reverse = base.endswith("r")          # FSUBR/FDIVR: dst = src OP dst
    core = base[:-1] if reverse else base
    integer = core.startswith("fi")
    sym = OPS.get(core)
    if sym is None:
        return None, "unknown mnemonic '%s'" % mn

    # register operands
    m = re.match(r"^st,\s*st\((\d)\)$", ops)
    if m:
        i = int(m.group(1))
        if popping:
            return None, "unexpected popping form with st,st(i)"
        expr = ("%s = %s %s %s;" % (ST0, st(i), sym, ST0) if reverse
                else "%s = %s %s %s;" % (ST0, ST0, sym, st(i)))
        return expr, None
    m = re.match(r"^st\((\d)\),\s*st$", ops)
    if m:
        i = int(m.group(1))
        expr = ("%s = %s %s %s;" % (st(i), ST0, sym, st(i)) if reverse
                else "%s = %s %s %s;" % (st(i), st(i), sym, ST0))
        if popping:
            expr += " fp_pop();"
        return expr, None
    m = re.match(r"^st\((\d)\)$", ops)
    if m:
        i = int(m.group(1))
        if popping:
            expr = ("%s = %s %s %s; fp_pop();" % (st(i), ST0, sym, st(i)) if reverse
                    else "%s = %s %s %s; fp_pop();" % (st(i), st(i), sym, ST0))
        else:
            expr = ("%s = %s %s %s;" % (ST0, st(i), sym, ST0) if reverse
                    else "%s = %s %s %s;" % (ST0, ST0, sym, st(i)))
        return expr, None

    # memory operand
    size, addr = addr_expr(ops)
    if addr is None:
        return None, "unparsed operand '%s'" % ops
    r = read_expr(size, addr, integer)
    if r is None:
        return None, "unsupported operand size '%s'" % size
    if popping:
        return None, "memory operand on a popping mnemonic '%s'" % mn
    expr = ("%s = %s %s %s;" % (ST0, r, sym, ST0) if reverse
            else "%s = %s %s %s;" % (ST0, ST0, sym, r))
    return expr, None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default=DEFAULT_GEN)
    ap.add_argument("--xbe", default=DEFAULT_XBE)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--revert", action="store_true",
                    help="put the collapsed form back, for bisecting a "
                         "regression against this pass")
    ap.add_argument("--function", default="", help="restrict to one function")
    args = ap.parse_args()

    if not os.path.exists(args.xbe):
        sys.exit("no XBE at %s" % args.xbe)
    data = io.open(args.xbe, "rb").read()
    sections = xbe_sections(data)

    n_sites = n_fixed = 0
    unpaired = []
    failures = {}

    # recomp_recovered.c holds the bodies recover_batch.py added; it was left
    # out of this glob, so none of them ever got their operands back.
    for path in sorted(glob.glob(os.path.join(args.gen, "recomp_0*.c")) +
                       glob.glob(os.path.join(args.gen, "recomp_recovered.c"))):
        lines = io.open(path, encoding="utf-8",
                        errors="surrogateescape").read().split("\n")
        changed = False
        i, origin = 0, None
        while i < len(lines):
            m = ORIGIN.match(lines[i])
            if m:
                origin = (int(m.group("start"), 16), int(m.group("end"), 16))
                i += 1
                continue
            fm = FUNC.match(lines[i])
            if not fm:
                i += 1
                continue
            name, start_line = fm.group("name"), i
            end_line = i
            while end_line < len(lines) and lines[end_line] != "}":
                end_line += 1
            if args.function and name != args.function:
                i = end_line
                continue

            # every arithmetic site in this function, in order
            sites = []
            for k in range(start_line, end_line):
                sm = SITE.search(lines[k])
                if sm:
                    sites.append((k, sm.group("mn")))
            ambig = [k for k, _ in sites if AMBIG.match(lines[k])]
            # A header covers only the function right after it: without this
            # reset a function with no header was paired against the previous
            # function's bytes.
            this_origin, origin = origin, None
            if this_origin is None or (not ambig and not args.revert):
                i = end_line
                continue
            n_sites += len(ambig)

            insns = [(a, mn, ops) for a, mn, ops in
                     disassemble(data, sections, *this_origin) if mn in ARITH_MN]

            if len(insns) != len(sites) or \
               any(mn != imn for (_k, mn), (_a, imn, _o) in zip(sites, insns)):
                unpaired.append((name, len(sites), len(insns)))
                i = end_line
                continue

            if args.revert:
                for (k, mn), (_a, imn, ops) in zip(sites, insns):
                    code, why = build(imn, ops)
                    if code is None:
                        continue
                    ind = re.match(r"^(\s*)", lines[k]).group(1)
                    produced = "%s%s /* %s %s */" % (ind, code, imn, ops)
                    if lines[k] != produced:
                        continue
                    sym = OPS.get(imn[1:] if imn.startswith("fi") and False else
                                  (imn[:-1] if imn.endswith("p") else imn).rstrip("r"))
                    if sym is None:
                        continue
                    lines[k] = "%sfp_st1() %s= fp_top(); fp_pop(); /* %s */" % (
                        ind, sym, imn)
                    changed = True
                    n_fixed += 1
                i = end_line
                continue

            for (k, mn), (_a, imn, ops) in zip(sites, insns):
                if not AMBIG.match(lines[k]):
                    continue
                code, why = build(imn, ops)
                if code is None:
                    failures.setdefault(why, []).append(name)
                    continue
                ind = AMBIG.match(lines[k]).group("ind")
                new = "%s%s /* %s %s */" % (ind, code, imn, ops)
                if args.dry_run or args.report:
                    if args.dry_run:
                        print("%s  %s" % (name, lines[k].strip()))
                        print("%s  -> %s" % (" " * len(name), new.strip()))
                else:
                    lines[k] = new
                    changed = True
                n_fixed += 1
            i = end_line

        if changed and not (args.dry_run or args.report):
            io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write("\n".join(lines))

    print("\ncollapsed x87 arithmetic sites: %d" % n_sites)
    print("%s: %d" % ("would rewrite" if (args.dry_run or args.report) else "rewrote",
                      n_fixed))
    if unpaired:
        tot = sum(a for _n, a, _b in unpaired)
        print("\nleft alone -- generated and disassembled sequences disagree "
              "in %d function(s) (%d sites):" % (len(unpaired), tot))
        for nm, a, b in unpaired[:12]:
            print("    %-42s gen=%d xbe=%d" % (nm, a, b))
        if len(unpaired) > 12:
            print("    ... and %d more" % (len(unpaired) - 12))
    if failures:
        print("\nleft alone -- no safe rewrite:")
        for why, where in sorted(failures.items()):
            print("    %d x %s   e.g. %s" % (len(where), why, where[0]))


if __name__ == "__main__":
    main()
