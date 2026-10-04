#!/usr/bin/env bash
# Translate SSX Tricky's XBE into C: the base xboxrecomp pass.
#
#   bash port/tools/regen.sh            # into port/src/recomp/gen/ (must be empty)
#   bash port/tools/regen.sh OUTDIR     # anywhere else, e.g. to diff against gen/
#
# EXPERIMENTAL. This runs the recompiler over every function the project knows
# (port/src/seeds.json, 12,581 entry points). The tree the port is built from
# also carries fix-up passes (xboxrecomp/tools/audit/fix*.py), recovered
# functions and hand-verified bodies that this script does not reproduce yet;
# see docs/building.md. It refuses to overwrite a populated gen/ -- regenerating
# in place throws those fixes away.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PORT="$ROOT/port"
TK="$ROOT/xboxrecomp"
XBE="$ROOT/game_files/default.xbe"
WORK="$ROOT/build/regen"
OUT="${1:-$PORT/src/recomp/gen}"
PY="${PYTHON:-python}"

[ -f "$XBE" ] || { echo "No game_files/default.xbe: unpack your own disc first (docs/building.md)"; exit 1; }
if [ -z "${1:-}" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then
    echo "$OUT is not empty. Regenerating in place would discard its fixes;"
    echo "pass an output directory instead, e.g.  bash port/tools/regen.sh build/gen_new"
    exit 1
fi
mkdir -p "$WORK" "$OUT"
OUT="$(cd "$OUT" && pwd)"      # absolute: the steps below run from xboxrecomp/
cd "$TK"

echo "[1/4] XBE analysis"
"$PY" -m tools.xbe_parser "$XBE" --json "$WORK/analysis.json" --quiet
echo "[2/4] disassembly (seeded)"
"$PY" -m tools.disasm "$XBE" -o "$WORK/disasm" --text-only --force \
    --analysis-json "$WORK/analysis.json" --seed-functions "$PORT/src/seeds.json"
echo "[3/4] function identification"
"$PY" -m tools.func_id "$XBE" --functions "$WORK/disasm/functions.json" \
    --xrefs "$WORK/disasm/xrefs.json" --strings "$WORK/disasm/strings.json" \
    --output "$WORK/funcid"
echo "[4/4] translation"
"$PY" -m tools.recomp "$XBE" --all --split 1000 --gen-dir "$OUT" -o "$WORK/recomp" \
    --disasm-dir "$WORK/disasm" --func-id-dir "$WORK/funcid" --skip-binary-check
echo "done: $OUT"
