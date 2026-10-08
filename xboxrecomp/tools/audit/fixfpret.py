#!/usr/bin/env python3
"""Recover floating-point return values that are dropped across calls.

MSVC returns a float or double in ST(0): the callee loads the value and
returns, leaving it in a register the caller reads. There is no push at the
call site, because the value is already on the x87 stack.

The generated code gives every function its **own** `_fp_stack` array, torn
down on return. So a callee's returned value never reaches its caller: the
caller's `fp_top()` reads whatever was left in its own local stack, which is
usually stale and often zero.

`recomp_types.h` already mirrors every `fp_push` into a thread-local
`g_x87_st0` for exactly this reason -- but only *orphan* call sites, the ones
with no local FP stack at all, were changed to read it. Any function that uses
the FPU elsewhere kept its local stack and still reads the wrong value. There
are 620 such sites.

The cost is not subtle. `sub_000FB5A0` initialises a scene view's clip planes
from three camera getters called through a vtable:

    call [edx+0x1cc] ; fstp [edi+0x810]      near
    call [eax+0x1d0] ; fstp [edi+0x814]      far

Both stored zero, so `zfar - znear` was zero, so the orthographic projection's
`1/(zfar-znear)` was **+inf**, and that infinity multiplied into the z of every
screen-space vertex the title drew -- which is why the glyph quads, correct in
position, size, colour and texture, were discarded by the rasteriser.

The fix inserts `fp_push(g_x87_st0);` before the consuming statement, so the
returned value enters the caller's stack exactly as the hardware would have
left it in ST(0).

    fixfpret.py --dry-run
    fixfpret.py

An earlier attempt removed all 620 insertions on the theory that the shared x87
stack already holds the callee's ST(0). It does not for every
callee: the frontend UI vanished and x87 top drifted by -2 M. They were
restored. The open question is which callees leave no value on the shared
stack -- until that is answered, keep this pass's output.

A site is only rewritten when the path from the call to the consumer is
unambiguous: no intervening push, call, branch, or return, and no label that
anything jumps to -- because a join point means ST(0) could have come from
another path entirely.
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

CALL = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\(\); /\* call|RECOMP_ICALL_SAFE")
PUSH = re.compile(r"\bfp_push\(")
USE = re.compile(r"\bfp_top\(\)|\bfp_st1\(\)")
STOP = re.compile(r"\bgoto \b|\bif \(|\breturn;")
LABEL = re.compile(r"^(loc_[0-9A-Fa-f]+): ;")
WINDOW = 12


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default=DEFAULT_GEN)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.gen, "*.c")))
    if not files:
        sys.exit("no generated sources under %s" % args.gen)

    fixed = skipped = 0
    for path in files:
        text = io.open(path, encoding="utf-8", errors="surrogateescape").read()
        lines = text.split("\n")
        targets = set(re.findall(r"goto (loc_[0-9A-Fa-f]+)", text))
        targets |= set(re.findall(r"(loc_[0-9A-Fa-f]+)\(\);", text))

        inserts = []
        i = 0
        while i < len(lines):
            if CALL.search(lines[i]):
                j = i + 1
                ok = True
                while j < len(lines) and j < i + WINDOW:
                    l = lines[j]
                    m = LABEL.match(l.strip())
                    if m and m.group(1) in targets:
                        ok = False
                        break
                    if PUSH.search(l) or CALL.search(l) or STOP.search(l):
                        ok = False
                        break
                    if USE.search(l):
                        break
                    j += 1
                else:
                    ok = False
                if ok and j < len(lines) and USE.search(lines[j]):
                    inserts.append(j)
                    fixed += 1
                elif not ok:
                    skipped += 1
            i += 1

        if inserts and not args.dry_run:
            for j in reversed(inserts):
                ind = re.match(r"^(\s*)", lines[j]).group(1)
                lines[j] = (ind + "fp_push(g_x87_st0); /* value returned in ST(0) "
                            "by the call above */\n" + lines[j])
            io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                    newline="").write("\n".join(lines))
        elif inserts:
            for j in inserts[:4]:
                print("%s:%d  %s" % (os.path.basename(path), j + 1,
                                     lines[j].strip()[:70]))

    print("\n%s %d site(s); %d call(s) left alone (ambiguous path)"
          % ("would rewrite" if args.dry_run else "rewrote", fixed, skipped))


if __name__ == "__main__":
    main()
