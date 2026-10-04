#!/usr/bin/env python3
"""Turn fixfpret.py's 620 float-return hand-offs into run-time checked ones.

fixfpret.py inserted `fp_push(g_x87_st0);` after calls whose ST(0) result the
caller consumes. That was right while every function had its own `_fp_stack`.
Since part 179 the x87 stack is one per thread, so a callee's result is
already on it -- and `g_x87_st0` is only the value of the callee's *last
fp_push*, not its ST(0). The angle wrapper sub_0001E600 ends
`fld x; fld 2pi; fmulp; fsubr [a]`: its last push is the constant, so every
caller received pi instead of the wrapped angle.

Removing the hand-offs outright broke the frontend UI, so some
callees evidently do leave nothing. This pass makes each site measure it:

    _fpc = g_fp_top; ... call ...        (x87 depth before the call)
    X87_RET(_fpc, 0xSITE);               (replaces the hand-off)

`x87_ret` (recomp_types.h) compares the depth after the call with `_fpc`;
XBOX_X87_RET picks the behaviour and 0x100 logs each site's first outcomes.

    fpretcheck.py --dry-run
    fpretcheck.py
"""
import argparse, glob, io, os, re, sys
import ssxpaths

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GEN = os.path.normpath(
    ssxpaths.GEN)
CALL = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\(\); /\* call|RECOMP_ICALL_SAFE")
HANDOFF = "fp_push(g_x87_st0); /* value returned in ST(0) by the call above */"
LOC = re.compile(r"RECOMP_LOC\((0x[0-9A-Fa-f]+)\)")
FUNC = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \*]*\b([A-Za-z_][A-Za-z0-9_]*)\(void\)\s*$")
DECL = "    int _fpc = 0; /* x87 depth before a float-returning call (X87_RET) */"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default=DEFAULT_GEN)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    total = funcs = 0
    for path in sorted(glob.glob(os.path.join(args.gen, "*.c"))):
        text = io.open(path, encoding="utf-8", errors="surrogateescape").read()
        if HANDOFF not in text:
            continue
        lines = text.split("\n")
        decl_at = set()
        for j, l in enumerate(lines):
            if HANDOFF not in l:
                continue
            k = j - 1
            while k > j - 14 and not CALL.search(lines[k]):
                if lines[k].startswith("}"):
                    sys.exit("%s:%d: call not in the same function" % (path, j + 1))
                k -= 1
            if not CALL.search(lines[k]):
                sys.exit("%s:%d: no call above the hand-off" % (path, j + 1))
            site = None
            for m in range(j, k, -1):
                mm = LOC.search(lines[m])
                if mm:
                    site = mm.group(1)
                    break
            site = site or "0x%08X" % 0
            ind = re.match(r"^(\s*)", lines[k]).group(1)
            if "_fpc = g_fp_top;" not in lines[k]:
                lines[k] = ind + "_fpc = g_fp_top; " + lines[k][len(ind):]
            lines[j] = lines[j].replace(
                HANDOFF, "X87_RET(_fpc, %s); /* value returned in ST(0) by the call above */" % site)
            f = k
            while f >= 0 and not FUNC.match(lines[f]):
                f -= 1
            if f < 0 or lines[f + 1].strip() != "{":
                sys.exit("%s:%d: cannot find the function head" % (path, j + 1))
            decl_at.add(f + 1)
            total += 1
        for f in sorted(decl_at, reverse=True):
            if lines[f + 1] != DECL:
                lines.insert(f + 1, DECL)
                funcs += 1
        if not args.dry_run:
            io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write("\n".join(lines))
    print("%s %d hand-off(s) in %d function(s)"
          % ("would check" if args.dry_run else "checked", total, funcs))


if __name__ == "__main__":
    main()
