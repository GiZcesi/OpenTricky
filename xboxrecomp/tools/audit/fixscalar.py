#!/usr/bin/env python3
"""Implement the remaining scalar instructions left as `TODO` comments.

What is left after the x87, string and SIMD passes splits three ways, and the
value of this pass is as much in the classification as in the rewrites: a
defect report that lists 57 holes when 23 of them are correct and 16 are not
code at all is a report nobody can act on.

**Real, and implemented here**

    bsf   r32, r/m32     find-first-set. Both sites are two-instruction leaf
                         helpers -- `bsf eax, src; ret` -- so with the
                         instruction dropped they returned whatever happened to
                         be in eax. This is the same shape as the dropped `bsf`
                         that corrupted every mid-sized heap allocation.
    rcr   r32, 1         rotate-right-through-carry. All 8 sites are the second
                         half of a `shr rN,1 ; rcr rM,1` pair -- a 64-bit shift
                         inside the CRT's division helper. The carry the `shr`
                         produces was discarded, so the pair is rewritten
                         together.
    xlatb                AL <- [EBX + AL], a 16-entry table lookup.
    pushal / popal       save and restore all eight general registers. The pair
                         is symmetric, so dropping both kept `esp` consistent
                         and made the hole invisible -- but any register the
                         function clobbers in between was never restored.
    cmpxchg m32, r32     interlocked compare-and-exchange. The store never
                         happened, and the `jne` after it was translated as a
                         re-read of the memory it was supposed to have written,
                         so the loop could never make progress. Both halves are
                         rewritten together.

**Correct as no-ops, and now marked as such**

    prefetcht0 prefetchnta sfence wbinvd hlt ldmxcsr

These are cache and ordering hints, or -- for `ldmxcsr` -- an SSE control word
this runtime does not model. Emitting nothing is the right translation; leaving
them as `TODO` makes 23 correct translations look like holes.

**Not code**

`scasb`, `cmpsd`, `insb`, `insd`, `outsd`, `arpl`, `aas`, `aam`, `lcall` and
`pushfd` sit in one region where the disassembler walked into a float table --
the surrounding "code" sets `edi = 0xBF800000` (-1.0f) and reads
`MEM32(0x3E938D1E)`. Left alone deliberately; the fix there is a function
boundary, not an opcode.

    fixscalar.py --dry-run
    fixscalar.py
"""

import argparse
import ssxpaths
import glob
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GEN = os.path.normpath(
    ssxpaths.GEN)

BSF_REG = re.compile(r"^(?P<ind>\s*)/\* TODO: bsf (?P<dst>e[a-z]{2}), (?P<src>e[a-z]{2}) \*/\s*$")
BSF_MEM = re.compile(r"^(?P<ind>\s*)/\* TODO: bsf (?P<dst>e[a-z]{2}), "
                     r"dword ptr \[(?P<addr>[^\]]+)\] \*/\s*$")
RCR = re.compile(r"^(?P<ind>\s*)/\* TODO: rcr (?P<dst>e[a-z]{2}), 1 \*/\s*$")
SHR = re.compile(r"^\s*(?P<reg>e[a-z]{2}) = (?P=reg) >> 1;\s*$")
XLATB = re.compile(r"^(?P<ind>\s*)/\* TODO: xlatb\s*\*/\s*$")
CMPXCHG = re.compile(r"^(?P<ind>\s*)/\* TODO: cmpxchg dword ptr \[(?P<addr>[^\]]+)\], "
                     r"(?P<src>e[a-z]{2}) \*/\s*$")
NOOP = re.compile(r"^(?P<ind>\s*)/\* TODO: (?P<mn>prefetcht0|prefetcht1|prefetcht2"
                  r"|prefetchnta|sfence|lfence|mfence|wbinvd|hlt|ldmxcsr|int)\b[^*]*\*/\s*$")
FNSTCW = re.compile(r"^(?P<ind>\s*)/\* fnstcw word ptr \[(?P<addr>[^\]]+)\]"
                    r" - store FPU control word \*/\s*$")
FLDCW = re.compile(r"^(?P<ind>\s*)/\* fldcw word ptr \[(?P<addr>[^\]]+)\]"
                   r" - load FPU control word \*/\s*$")
PUSHAL = re.compile(r"^(?P<ind>\s*)/\* TODO: pushal\s*\*/\s*$")
POPAL = re.compile(r"^(?P<ind>\s*)/\* TODO: popal\s*\*/\s*$")

NOOP_WHY = {
    "prefetcht0":  "cache hint",
    "prefetcht1":  "cache hint",
    "prefetcht2":  "cache hint",
    "prefetchnta": "cache hint",
    "sfence":      "store ordering; the host is already ordered here",
    "lfence":      "load ordering; the host is already ordered here",
    "mfence":      "memory ordering; the host is already ordered here",
    "wbinvd":      "cache writeback/invalidate",
    "hlt":         "halt until interrupt",
    "ldmxcsr":     "SSE control word is not modelled",
    "int":         "debugger service trap; the int3 beside it already breaks",
}


def addr_expr(text):
    body = text.replace("*1", "").strip()
    if not re.match(r"^[a-z0-9_+\-*x ]+$", body):
        return None
    body = re.sub(r"\s*\+\s*", " + ", body)
    return body.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default=DEFAULT_GEN)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.gen, "*.c")))
    if not files:
        sys.exit("no generated sources under %s" % args.gen)

    counts, skipped = {}, []

    def bump(k):
        counts[k] = counts.get(k, 0) + 1

    for path in files:
        lines = io.open(path, encoding="utf-8",
                        errors="surrogateescape").read().split("\n")
        out = list(lines)
        dirty = False
        for i, line in enumerate(lines):
            new = None

            m = BSF_REG.match(line) or BSF_MEM.match(line)
            if m:
                d = m.group("dst")
                src = (m.group("src") if "src" in m.groupdict() and m.groupdict().get("src")
                       else None)
                if src is None:
                    a = addr_expr(m.group("addr"))
                    if a is None:
                        skipped.append("%s:%d bsf address" % (os.path.basename(path), i + 1))
                        continue
                    src = "MEM32(%s)" % a
                # Hardware leaves the destination alone when the source is zero.
                new = ("%s{ uint32_t _s = (uint32_t)(%s); if (_s) %s = "
                       "(uint32_t)__builtin_ctz(_s); } /* bsf */"
                       % (m.group("ind"), src, d))
                bump("bsf")

            elif RCR.match(line):
                m = RCR.match(line)
                sm = SHR.match(lines[i - 1]) if i else None
                if not sm:
                    skipped.append("%s:%d rcr without its shr"
                                   % (os.path.basename(path), i + 1))
                    continue
                hi, lo = sm.group("reg"), m.group("dst")
                out[i - 1] = ("%s{ uint32_t _cf = %s & 1u; %s >>= 1;"
                              % (m.group("ind"), hi, hi))
                new = ("%s  %s = (%s >> 1) | (_cf << 31); } /* shr %s,1 ; rcr %s,1 */"
                       % (m.group("ind"), lo, lo, hi, lo))
                bump("rcr")

            elif XLATB.match(line):
                m = XLATB.match(line)
                new = "%sSET_LO8(eax, MEM8(ebx + LO8(eax))); /* xlatb */" % m.group("ind")
                bump("xlatb")

            elif CMPXCHG.match(line):
                m = CMPXCHG.match(line)
                a = addr_expr(m.group("addr"))
                if a is None:
                    skipped.append("%s:%d cmpxchg address" % (os.path.basename(path), i + 1))
                    continue
                new = ("%s{ uint32_t _d = MEM32(%s);"
                       " if (_d == eax) { MEM32(%s) = %s; g_str_ne = 0; }"
                       " else { eax = _d; g_str_ne = 1; } } /* cmpxchg */"
                       % (m.group("ind"), a, a, m.group("src")))
                bump("cmpxchg")
                # The jne that follows tests ZF from the exchange, not a
                # re-read of the memory it was meant to have written.
                nxt = lines[i + 1] if i + 1 < len(lines) else ""
                if "goto" in nxt and "jne" in nxt and "!=" in nxt:
                    head = nxt[:nxt.index("if (")]
                    tail = nxt[nxt.index("goto"):]
                    out[i + 1] = "%sif (g_str_ne) %s" % (head, tail)
                    bump("cmpxchg-branch")

            elif FNSTCW.match(line):
                m = FNSTCW.match(line)
                a = addr_expr(m.group("addr"))
                if a is None:
                    skipped.append("fnstcw address"); continue
                new = "%sMEM16(%s) = g_x87_cw; /* fnstcw */" % (m.group("ind"), a)
                bump("fnstcw")

            elif FLDCW.match(line):
                m = FLDCW.match(line)
                a = addr_expr(m.group("addr"))
                if a is None:
                    skipped.append("fldcw address"); continue
                new = "%sg_x87_cw = (uint16_t)MEM16(%s); /* fldcw */" % (m.group("ind"), a)
                bump("fldcw")

            elif PUSHAL.match(line):
                m = PUSHAL.match(line)
                # PUSHA order: EAX ECX EDX EBX ESP(original) EBP ESI EDI.
                new = ("%s{ uint32_t _sp = esp; PUSH32(esp, eax); PUSH32(esp, ecx);"
                       " PUSH32(esp, edx); PUSH32(esp, ebx); PUSH32(esp, _sp);"
                       " PUSH32(esp, ebp); PUSH32(esp, esi); PUSH32(esp, edi); }"
                       " /* pushal */" % m.group("ind"))
                bump("pushal")

            elif POPAL.match(line):
                m = POPAL.match(line)
                # POPA restores everything but ESP, which is discarded.
                new = ("%s{ uint32_t _dead; POP32(esp, edi); POP32(esp, esi);"
                       " POP32(esp, ebp); POP32(esp, _dead); POP32(esp, ebx);"
                       " POP32(esp, edx); POP32(esp, ecx); POP32(esp, eax); (void)_dead; }"
                       " /* popal */" % m.group("ind"))
                bump("popal")

            elif NOOP.match(line):
                m = NOOP.match(line)
                mn = m.group("mn")
                new = "%s(void)0; /* %s -- %s, nothing to model */" % (
                    m.group("ind"), mn, NOOP_WHY[mn])
                bump(mn)

            if new is None:
                continue
            if args.dry_run:
                print("%s:%d  %s" % (os.path.basename(path), i + 1, line.strip()[:52]))
                print("        -> %s" % new.strip()[:96])
            else:
                out[i] = new
                dirty = True
        if dirty and not args.dry_run:
            io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write("\n".join(out))

    total = sum(counts.values())
    print("\n%s %d site(s)" % ("would rewrite" if args.dry_run else "rewrote", total))
    for k in sorted(counts, key=lambda x: -counts[x]):
        print("    %-16s %3d" % (k, counts[k]))
    if skipped:
        print("\nleft alone:")
        for w in skipped:
            print("    %s" % w)


if __name__ == "__main__":
    main()
