#!/usr/bin/env python3
"""Implement the repeated string instructions the lifter left as comments.

`rep movs` and `rep stos` are translated (939 sites). The comparing and
scanning forms are not: 16 sites are a bare annotation, and the branch that
consumes their result is hardcoded.

    /* repe cmpsb - string compare, ecx iterations */
    if (0 /* strings differed (repe cmpsb) */) goto loc_0007A0F0;

Two separate losses:

  * **the answer.** The branch is pinned, so one side is unreachable. Seven
    sites are pinned to "matched" and three to "differed", whatever the bytes
    actually are.
  * **the registers.** `repe cmps` and `repne scas` advance `esi`/`edi` and
    count `ecx` down, and callers use those afterwards. The four `repne scasb`
    sites are the CRT's `strlen` idiom --

        mov ecx, -1 / xor al, al / repne scasb / not ecx / dec ecx

    -- so with `ecx` never counted down, every length computed that way is
    derived from -1.

This rewrites each site into a faithful loop and repoints the branch at the
result. Semantics follow Intel: the compare forms stop at the first mismatch
and the scan stops on the first match, leaving the pointers one element past
the byte that ended the run, which is exactly what the `strlen` idiom depends
on.

    fixrepstr.py --dry-run
    fixrepstr.py

Forward direction (DF clear) is assumed, as it is for the already-translated
`rep movs`/`rep stos` sites; a `std` site is reported rather than rewritten.
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

OP = re.compile(r"^(?P<ind>\s*)/\* (?P<mn>repe cmpsb|repe cmpsd|repne scasb)"
                r" - string (compare|scan),[^*]*\*/\s*$")
BR = re.compile(r"^(?P<ind>\s*)if \((?P<pin>0|1) /\* strings (differed|matched)"
                r" \(repe cmpsb\) \*/\)(?P<tail>.*)$")

BODY = {
    # ZF after the run: set when the last pair compared equal.
    "repe cmpsb": """{ uint32_t _n = ecx; int _ne = 0;
%(i)s  while (_n) { uint32_t _a = MEM8(esi), _b = MEM8(edi);
%(i)s               esi += 1; edi += 1; _n--;
%(i)s               if (_a != _b) { _ne = 1; break; } }
%(i)s  ecx = _n; g_str_ne = _ne; } /* repe cmpsb */""",
    "repe cmpsd": """{ uint32_t _n = ecx; int _ne = 0;
%(i)s  while (_n) { uint32_t _a = MEM32(esi), _b = MEM32(edi);
%(i)s               esi += 4; edi += 4; _n--;
%(i)s               if (_a != _b) { _ne = 1; break; } }
%(i)s  ecx = _n; g_str_ne = _ne; } /* repe cmpsd */""",
    # ZF set when the byte was found; `strlen` reads the residual ecx.
    "repne scasb": """{ uint32_t _n = ecx; int _hit = 0;
%(i)s  while (_n) { uint32_t _b = MEM8(edi);
%(i)s               edi += 1; _n--;
%(i)s               if (_b == LO8(eax)) { _hit = 1; break; } }
%(i)s  ecx = _n; g_str_ne = !_hit; } /* repne scasb */""",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default=DEFAULT_GEN)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.gen, "*.c")))
    if not files:
        sys.exit("no generated sources under %s" % args.gen)

    ops = brs = 0
    unpinned = []

    for path in files:
        lines = io.open(path, encoding="utf-8",
                        errors="surrogateescape").read().split("\n")
        dirty = False
        for i, line in enumerate(lines):
            m = OP.match(line)
            if m:
                ind, mn = m.group("ind"), m.group("mn")
                new = ind + BODY[mn] % {"i": ind}
                if args.dry_run:
                    print("%s:%d  %s" % (os.path.basename(path), i + 1, mn))
                else:
                    lines[i] = new
                    dirty = True
                ops += 1
                continue
            m = BR.match(line)
            if m:
                ind, tail = m.group("ind"), m.group("tail")
                # The trailing annotation names the real instruction, so the
                # sense comes from the jcc rather than from the pinned literal.
                if "jne" in tail:
                    cond = "g_str_ne"
                elif "je" in tail:
                    cond = "!g_str_ne"
                else:
                    unpinned.append("%s:%d" % (os.path.basename(path), i + 1))
                    continue
                new = "%sif (%s)%s" % (ind, cond, tail)
                if args.dry_run:
                    print("%s:%d  %s -> %s" % (os.path.basename(path), i + 1,
                                               line.strip()[:48], cond))
                else:
                    lines[i] = new
                    dirty = True
                brs += 1
        if dirty and not args.dry_run:
            io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write("\n".join(lines))

    verb = "would rewrite" if args.dry_run else "rewrote"
    print("\n%s %d string operation(s) and %d branch(es)" % (verb, ops, brs))
    if unpinned:
        print("left alone -- branch sense not recoverable from the annotation:")
        for w in unpinned:
            print("    %s" % w)


if __name__ == "__main__":
    main()
