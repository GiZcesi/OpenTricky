#!/usr/bin/env python3
"""
instrument_labels.py -- put a RECOMP_LOC() check on every generated label.

WHY
---
Instrumenting a label used to mean editing a generated .c file and rebuilding:
52 s for one touched file, ~90 s for a full probe cycle, and the site had to be
guessed right *before* paying that cost.  With RECOMP_LOC() at every label,
probes are armed at runtime (XBOX_PROBE, or the diag server's `probe` command)
and cost nothing to re-aim.

Measured: 69,683 labels instrumented, no measurable runtime cost when nothing
is armed (draws 7,688 vs a 7,730 baseline, inside run-to-run variance).

Idempotent -- a file that already includes recomp_probe.h is left alone, so it
is safe to run after every splice.  recover_batch.py and relift.py call it
automatically; run it by hand only after hand-editing a generated file.

    python instrument_labels.py              # all generated files
    python instrument_labels.py recomp_0003.c
"""

import io, re, sys, os
import ssxpaths

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
GEN  = ssxpaths.GEN
LABEL = re.compile(r"^(loc_([0-9A-Fa-f]{8})): ;[ \t]*$")

def instrument(path):
    src = io.open(path, encoding="utf-8", errors="surrogateescape").read()
    have_include = "recomp_probe.h" in src
    lines = src.split("\n")
    out, n = [], 0
    for i, ln in enumerate(lines):
        out.append(ln)
        m = LABEL.match(ln)
        if m:
            nxt = lines[i + 1] if i + 1 < len(lines) else ""
            if "RECOMP_LOC(" not in nxt:
                out.append("    RECOMP_LOC(0x%s);" % m.group(2).upper())
                n += 1
    if n == 0 and have_include:
        return 0, "up to date"
    body = "\n".join(out)
    # The include goes after recomp_funcs.h, which is the first include in every
    # generated file and sits below #define RECOMP_GENERATED_CODE, so the
    # register macros are already visible.
    if not have_include:
        body = body.replace('#include "recomp_funcs.h"',
                            '#include "recomp_funcs.h"\n#include "../recomp_probe.h"', 1)
    io.open(path, "w", encoding="utf-8", errors="surrogateescape").write(body)
    return n, "ok"

targets = sys.argv[1:] or sorted(f for f in os.listdir(GEN) if f.endswith(".c"))
total = 0
for f in targets:
    p = os.path.join(GEN, f)
    n, msg = instrument(p)
    total += n
    print("  %-28s %6d  %s" % (f, n, msg))
print("total labels instrumented:", total)
