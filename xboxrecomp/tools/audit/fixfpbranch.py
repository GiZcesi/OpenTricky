#!/usr/bin/env python3
"""Restore x87 comparison branches, using the XBE to recover what was dropped.

An x87 comparison in compiled code is four instructions:

    fcomp  dword ptr [esp+0x10]     compare ST(0) with memory, pop
    fnstsw ax                       copy the FPU status word into AH
    test   ah, 0x5                  set PF from a mask of C0/C2/C3
    jp     0xbb2ca                  branch on parity

The recompiler drops three of them.  `fnstsw` becomes a bare comment, the
`test` vanishes entirely, and the branch is emitted as:

    if (1 /* jp after test - parity */) goto loc_000BB2CA;

Always taken.  780 of those in this build, in both directions, which is
self-contradictory -- and it is why fixing `fnstsw` on its own makes things
worse rather than better: the branch stays unconditional, while the status word
it does not read starts affecting every *other* branch.

The mask is the missing piece.  It is not in the generated source at all, so it
has to come from the original bytes: this tool disassembles each affected
function out of the XBE, pairs each `jp`/`jnp` with the `test ah, IMM` in front
of it, and rewrites the branch to evaluate parity properly.

    fixfpbranch.py --dry-run     report what would change
    fixfpbranch.py               apply
    fixfpbranch.py --revert      restore from the backups it wrote

Functions whose branch count does not line up between the XBE and the generated
body are skipped and listed, never guessed at.  A wrong branch is worse than an
unconditional one, because it looks deliberate.
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
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DEFAULT_GEN = ssxpaths.GEN
DEFAULT_XBE = ssxpaths.XBE
SUFFIX = ".bak_fpbranch"

FUNC = re.compile(r"^void (?P<name>\w+)\(void\)$")
ORIGIN = re.compile(r"^\s*\*\s*Original:\s*0x(?P<start>[0-9A-Fa-f]{8})\s*-\s*"
                    r"0x(?P<end>[0-9A-Fa-f]{8})")
PARITY = re.compile(r"^(?P<ind>\s*)if \(1 /\* (?P<mn>jn?p) after test - parity \*/\)"
                    r"(?P<tail>.*)$")


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
    tmp = os.path.join(HERE, ".fpbranch_tmp.bin")
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


# Mnemonics that provably leave EFLAGS alone, so a `test ah, IMM` before one
# of them is still live at a later `jp`/`jnp`.
#
# The first version of this pairing looked back a fixed three instructions and
# gave up otherwise, which lost every site where the compiler scheduled SSE
# work into the gap -- 34 of them, including 15 in
# Rider_ResolveTerrainContactPhysics:
#
#     test   ah,0x5
#     movaps xmm0,XMMWORD PTR [esp+0x10]
#     subps  xmm0,xmm1
#     movaps XMMWORD PTR [esp+0x40],xmm0
#     jp     0x2b92a
#
# The list is an allow-list on purpose: an unrecognised mnemonic stops the
# search rather than being assumed harmless, so a new opcode can cost a pairing
# but can never produce a wrong one. Note what is deliberately absent --
# `comiss`/`ucomiss`/`fcomi` all write EFLAGS and must stop the walk.
FLAG_SAFE = frozenset("""
nop lea
mov movb movw movl movzx movsx movsxd
push pop
movaps movups movss movsd movdqa movdqu movq movd
movlps movhps movlpd movhpd movntps movntdq movnti
addps addss addpd addsd subps subss subpd subsd
mulps mulss mulpd mulsd divps divss divpd divsd
sqrtps sqrtss rcpps rcpss rsqrtps rsqrtss
maxps maxss minps minss
andps andnps orps xorps andpd andnpd orpd xorpd
pand pandn por pxor paddd psubd paddw psubw paddb psubb
shufps shufpd unpcklps unpckhps unpcklpd unpckhpd pshufd pshufw
cvtps2pd cvtpd2ps cvtdq2ps cvtps2dq cvttps2dq cvtsi2ss cvtsi2sd
cvtss2sd cvtsd2ss cvttss2si cvttsd2si
fld fld1 fldz fldpi fst fstp fild fist fistp fxch fchs fabs
fadd faddp fsub fsubp fsubr fsubrp fmul fmulp fdiv fdivp fdivr fdivrp
fnstsw fnstcw fldcw fwait
prefetchnta prefetcht0 prefetcht1 prefetcht2
""".split())


def parity_branches(insns):
    """Each jp/jnp, paired with the mask of the `test ah, IMM` that feeds it.

    Walks back over instructions that cannot disturb EFLAGS. Anything else --
    another flag writer, a branch, a call -- ends the search and the site is
    reported unpaired rather than guessed at."""
    out = []
    for i, (addr, mn, args) in enumerate(insns):
        if mn not in ("jp", "jnp"):
            continue
        mask = None
        for k in range(i - 1, max(-1, i - 24), -1):
            _a, m2, a2 = insns[k]
            t = re.match(r"^ah,(0x[0-9a-f]+|\d+)$", a2.replace(" ", ""))
            if m2 == "test" and t:
                mask = int(t.group(1), 16 if t.group(1).startswith("0x") else 10)
                break
            if m2 not in FLAG_SAFE:
                break
        out.append((addr, mn, mask))
    return out


def process(gen_dir, xbe_path, dry_run):
    data = io.open(xbe_path, "rb").read()
    sections = xbe_sections(data)

    fixed = skipped = 0
    skips = []
    for path in sorted(glob.glob(os.path.join(gen_dir, "recomp_0*.c"))):
        lines = io.open(path, encoding="utf-8", errors="surrogateescape").read().split("\n")
        changed = False
        i, origin = 0, None
        while i < len(lines):
            m = ORIGIN.match(lines[i])
            if m:
                origin = (int(m.group("start"), 16), int(m.group("end"), 16))
                i += 1
                continue
            m = FUNC.match(lines[i])
            if not m:
                i += 1
                continue
            name, start_line = m.group("name"), i
            end_line = i
            while end_line < len(lines) and lines[end_line] != "}":
                end_line += 1

            sites = [k for k in range(start_line, end_line) if PARITY.match(lines[k])]
            if not sites or origin is None:
                i = end_line
                continue

            branches = parity_branches(disassemble(data, sections, *origin))
            if len(branches) != len(sites):
                skipped += len(sites)
                skips.append((name, len(sites), len(branches)))
                i = end_line
                continue

            ok = True
            for k, (_addr, mn, mask) in zip(sites, branches):
                pm = PARITY.match(lines[k])
                if mask is None or pm.group("mn") != mn:
                    ok = False
                    break
            if not ok:
                skipped += len(sites)
                skips.append((name, len(sites), -1))
                i = end_line
                continue

            for k, (addr, mn, mask) in zip(sites, branches):
                pm = PARITY.match(lines[k])
                cond = "FPU_PARITY(0x%02X)" % mask
                if mn == "jnp":
                    cond = "!" + cond
                lines[k] = ("%sif (%s)%s   /* %s after test ah,0x%02X -- the test and "
                            "the fnstsw were both dropped, so this was emitted as "
                            "if(1) */"
                            % (pm.group("ind"), cond, pm.group("tail"), mn, mask))
                fixed += 1
                changed = True
            i = end_line

        if changed and not dry_run:
            io.open(path + SUFFIX, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write("\n".join(
                        io.open(path, encoding="utf-8",
                                errors="surrogateescape").read().split("\n")))
            io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write("\n".join(lines))

    print("parity branches rewritten : %d" % fixed)
    print("skipped (no clean pairing): %d" % skipped)
    for name, want, got in skips[:12]:
        print("   %-34s %d site(s), %s branch(es) in the XBE"
              % (name, want, "?" if got < 0 else got))
    if len(skips) > 12:
        print("   ... and %d more function(s)" % (len(skips) - 12))
    if dry_run:
        print("\n(dry run -- nothing written)")
    return 0


def revert(gen_dir):
    n = 0
    for bak in glob.glob(os.path.join(gen_dir, "*" + SUFFIX)):
        target = bak[:-len(SUFFIX)]
        io.open(target, "w", encoding="utf-8", errors="surrogateescape",
                newline="").write(io.open(bak, encoding="utf-8",
                                          errors="surrogateescape").read())
        os.remove(bak)
        n += 1
    print("reverted %d file(s)" % n)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default=DEFAULT_GEN)
    ap.add_argument("--xbe", default=DEFAULT_XBE)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--revert", action="store_true")
    args = ap.parse_args()
    if args.revert:
        return revert(args.gen)
    return process(args.gen, args.xbe, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
