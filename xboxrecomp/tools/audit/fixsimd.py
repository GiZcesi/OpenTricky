#!/usr/bin/env python3
"""Implement the SSE instructions the lifter left as `TODO` comments.

Most of the packed-float set is translated. A handful of forms are not, and
they are annotation only:

    /* TODO: movntps xmmword ptr [edi + eax*4], xmm0 */

`movntps` is a **store**. Thirty-nine of them write nothing at all, so whatever
the routine computed never reaches memory -- the same failure mode that broke
the CRT's `memcpy` once already. They cluster in exactly the places that shape
suggests: `sub_00178164` is a 4x4 transpose built out of `movlhps`/`movhlps`
followed by four non-temporal stores, and every one of those eight instructions
is a comment, so the whole routine is a no-op that returns whatever was in the
destination buffer.

Handled here (register-register unless noted):

    movntps  m128, xmm     store, the only memory form in the tree
    movlhps  xmm, xmm      dst high 64 <- src low 64
    movhlps  xmm, xmm      dst low 64  <- src high 64
    andnps   xmm, xmm      dst = (NOT dst) AND src
    cmpnltps xmm, xmm      per lane: dst >= src ? all-ones : zero
    cmpnleps xmm, xmm      per lane: dst >  src ? all-ones : zero

    fixsimd.py --dry-run
    fixsimd.py

The two compares are the negated forms, so on hardware they also answer true
for unordered (NaN) operands. This maps them to `>=` and `>`, which differs only
when a lane is NaN; the title's vector code does not produce NaNs on these
paths, and modelling the unordered case would need a NaN-aware compare the
runtime does not have. Recorded here rather than left implicit.

MMX is handled too, against the lane model added to `recomp_types.h`:

    movntq    m64, mm        store
    paddb/w/d mm, mm         packed add
    paddusb   mm, mm         unsigned saturating byte add
    psrld/psrlq/psllq mm, i  shift by immediate
    punpcklwd/punpckhwd      word interleave
    packuswb  mm, mm         signed words to saturated unsigned bytes
    pmaddwd   mm, mm         four signed products, summed in adjacent pairs
    cvtpi2ps  xmm, m64       two signed dwords to the low two float lanes
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

STORE = re.compile(r"^(?P<ind>\s*)/\* TODO: movntps xmmword ptr \[(?P<addr>[^\]]+)\],"
                   r"\s*(?P<src>xmm[0-7]) \*/\s*$")
RR = re.compile(r"^(?P<ind>\s*)/\* TODO: (?P<mn>movlhps|movhlps|andnps|cmpnltps|cmpnleps)"
                r"\s+(?P<dst>xmm[0-7]),\s*(?P<src>xmm[0-7]) \*/\s*$")

MM_STORE = re.compile(r"^(?P<ind>\s*)/\* TODO: movntq qword ptr \[(?P<addr>[^\]]+)\],"
                      r"\s*(?P<src>mm[0-7]) \*/\s*$")
MM_RR = re.compile(r"^(?P<ind>\s*)/\* TODO: (?P<mn>paddb|paddw|paddd|paddusb"
                   r"|punpcklwd|punpckhwd|packuswb|pmaddwd)"
                   r"\s+(?P<dst>mm[0-7]),\s*(?P<src>mm[0-7]) \*/\s*$")
MM_SHIFT = re.compile(r"^(?P<ind>\s*)/\* TODO: (?P<mn>psrld|psrlq|psllq|pslld)"
                      r"\s+(?P<dst>mm[0-7]),\s*(?P<imm>0x[0-9a-f]+|\d+) \*/\s*$")
MM_CVT = re.compile(r"^(?P<ind>\s*)/\* TODO: cvtpi2ps\s+(?P<dst>xmm[0-7]),"
                    r"\s*qword ptr \[(?P<addr>[^\]]+)\] \*/\s*$")

RR_FORM = {
    "movlhps":  "XMM_MOVLHPS(%(d)s, %(s)s);",
    "movhlps":  "XMM_MOVHLPS(%(d)s, %(s)s);",
    "andnps":   "XMM_ANDNPS(%(d)s, %(s)s);",
    "cmpnltps": "XMM_CMPPS(%(d)s, %(s)s, >=);",
    "cmpnleps": "XMM_CMPPS(%(d)s, %(s)s, >);",
}


def addr_expr(text):
    """objdump's `edi + eax*4 + 0x10` as the generator spells it."""
    body = text.replace("*1", "").strip()
    if not re.match(r"^[a-z0-9_+\-*x ]+$", body):
        return None
    body = re.sub(r"\s*\+\s*", " + ", body)
    body = re.sub(r"(?<=[a-z0-9])\s*-\s*", " - ", body)
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
    for path in files:
        lines = io.open(path, encoding="utf-8",
                        errors="surrogateescape").read().split("\n")
        dirty = False
        for i, line in enumerate(lines):
            m = STORE.match(line)
            if m:
                addr = addr_expr(m.group("addr"))
                if addr is None:
                    skipped.append("%s:%d unparsed address %r"
                                   % (os.path.basename(path), i + 1, m.group("addr")))
                    continue
                new = "%sMEMX(%s) = %s.x; /* movntps */" % (
                    m.group("ind"), addr, m.group("src"))
                counts["movntps"] = counts.get("movntps", 0) + 1
            elif MM_STORE.match(line):
                m = MM_STORE.match(line)
                addr = addr_expr(m.group("addr"))
                if addr is None:
                    skipped.append("%s:%d unparsed address %r"
                                   % (os.path.basename(path), i + 1, m.group("addr")))
                    continue
                new = "%sMEM64(%s) = %s; /* movntq */" % (
                    m.group("ind"), addr, m.group("src"))
                counts["movntq"] = counts.get("movntq", 0) + 1
            elif MM_RR.match(line):
                m = MM_RR.match(line)
                mn, d, sr = m.group("mn"), m.group("dst"), m.group("src")
                new = "%s%s = mm_%s(%s, %s); /* %s */" % (m.group("ind"), d, mn, d, sr, mn)
                counts[mn] = counts.get(mn, 0) + 1
            elif MM_SHIFT.match(line):
                m = MM_SHIFT.match(line)
                mn, d, imm = m.group("mn"), m.group("dst"), m.group("imm")
                new = "%s%s = mm_%s(%s, %s); /* %s */" % (m.group("ind"), d, mn, d, imm, mn)
                counts[mn] = counts.get(mn, 0) + 1
            elif MM_CVT.match(line):
                m = MM_CVT.match(line)
                addr = addr_expr(m.group("addr"))
                if addr is None:
                    skipped.append("%s:%d unparsed address %r"
                                   % (os.path.basename(path), i + 1, m.group("addr")))
                    continue
                new = "%sMM_CVTPI2PS(%s, MEM64(%s)); /* cvtpi2ps */" % (
                    m.group("ind"), m.group("dst"), addr)
                counts["cvtpi2ps"] = counts.get("cvtpi2ps", 0) + 1
            else:
                m = RR.match(line)
                if not m:
                    continue
                mn = m.group("mn")
                new = "%s%s /* %s */" % (
                    m.group("ind"),
                    RR_FORM[mn] % {"d": m.group("dst"), "s": m.group("src")}, mn)
                counts[mn] = counts.get(mn, 0) + 1
            if args.dry_run:
                print("%s:%d" % (os.path.basename(path), i + 1))
                print("  -%s" % line.strip())
                print("  +%s" % new.strip())
            else:
                lines[i] = new
                dirty = True
        if dirty and not args.dry_run:
            io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write("\n".join(lines))

    total = sum(counts.values())
    print("\n%s %d site(s)" % ("would rewrite" if args.dry_run else "rewrote", total))
    for mn in sorted(counts, key=lambda k: -counts[k]):
        print("    %-10s %3d" % (mn, counts[mn]))
    if skipped:
        print("\nleft alone:")
        for w in skipped:
            print("    %s" % w)


if __name__ == "__main__":
    main()
