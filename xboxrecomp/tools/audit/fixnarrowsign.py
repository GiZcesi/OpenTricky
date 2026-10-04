#!/usr/bin/env python3
"""Restore the sign of 8- and 16-bit flag results in the generated tree.

An x86 byte or word operation keeps its sign in bit 7 / bit 15. The lifter
emitted those conditions with 32-bit semantics:

    CMP_GE(LO8(eax) & LO8(eax), 0)        test al, al; jge   -- `&` promotes to
                                                              int, so RECOMP_SEXT
                                                              (sizeof-based) never
                                                              sign-extends
    CMP_L(MEM8(esi + 4), 0x80)            cmp byte [esi+4], 0x80; jl -- 0x80 is
                                                              -128 as a byte, 128
                                                              as a C int
    ((int32_t)LO8(eax) < 0)               sub al, 1; js      -- always false

The first one sent every AI rider in a race down the human-player branch of
Race_SpawnRidersAndLoadAssets, which left the player without a controller and
paused the race with "controller disconnected". The lifter now
narrows these (lifter._make_condition); apply_lifter_diff.py carried most of
the change into gen/, and this pass fixes what it could not reach -- sites an
older pass had already rewritten, and flag snapshots.

    fixnarrowsign.py [--dry-run]
"""
import argparse, glob, io, os, re
import ssxpaths

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = ssxpaths.GEN

ARG = r"[^()]*(?:\([^()]*\)[^()]*)*"          # one level of nested parentheses
N8 = r"(?:LO8|HI8|MEM8)\(" + ARG + r"\)|\(\(uint8_t\)_fs[ab]\)"
N16 = r"(?:LO16|MEM16)\(" + ARG + r"\)|\(\(uint16_t\)_fs[ab]\)"
SIGNED = r"CMP_(?:L|GE|LE|G)"

RULES = []
for n, ut, st, lo, hi in ((N8, "uint8_t", "int8_t", 0x80, 0xFF), (N16, "uint16_t", "int16_t", 0x8000, 0xFFFF)):
    # test: CMP_x(A & B, 0) with narrow A and B
    RULES.append((re.compile(r"(" + SIGNED + r")\(((?:" + n + r")) & ((?:" + n + r")), 0\)"),
                  lambda m, ut=ut: "%s((%s)(%s & %s), 0)" % (m.group(1), ut, m.group(2), m.group(3)),
                  "test %s in a signed compare" % ut))
    # cmp: narrow operand against an immediate with the sign bit set
    RULES.append((re.compile(r"(" + SIGNED + r")\(((?:" + n + r")), (0x[0-9A-Fa-f]+)\)"),
                  lambda m, ut=ut, lo=lo, hi=hi: (m.group(0) if not lo <= int(m.group(3), 16) <= hi
                                                  else "%s(%s, (%s)%s)" % (m.group(1), m.group(2), ut, m.group(3))),
                  "cmp %s against a negative immediate" % ut))
    # post-op results and test/cmp js forms cast to int32_t
    RULES.append((re.compile(r"\(int32_t\)(\((?:" + n + r")(?: [&-] (?:" + n + r"|0x[0-9A-Fa-f]+))?\)|(?:" + n + r"))"),
                  lambda m, st=st: "(%s)%s" % (st, m.group(1)),
                  "(int32_t) of a %s value" % ut))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default=GEN)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    counts, shown = {}, {}
    for path in sorted(glob.glob(os.path.join(args.gen, "recomp_*.c"))):
        text = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        new = text
        for rx, fn, why in RULES:
            def sub(m, fn=fn, why=why):
                r = fn(m)
                if r != m.group(0):
                    counts[why] = counts.get(why, 0) + 1
                    if args.dry_run and shown.get(why, 0) < 2:
                        shown[why] = shown.get(why, 0) + 1
                        print("%s\n  - %s\n  + %s" % (os.path.basename(path), m.group(0), r))
                return r
            new = rx.sub(sub, new)
        if new != text and not args.dry_run:
            io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    for why, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print("%5d  %s" % (n, why))


if __name__ == "__main__":
    main()
