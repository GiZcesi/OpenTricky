#!/usr/bin/env bash
# Rebuild port/src/recomp/gen/ from zero in one command.
#
#   bash port/tools/fork_regen.sh            # regen -> link hookups -> passes -> gen/
#   bash port/tools/fork_regen.sh --upto N   # stop after pass N (bisecting)
#   bash port/tools/fork_regen.sh --only N   # apply only pass N to the existing gen/
#                                            # (measuring one pass at a time)
#   bash port/tools/fork_regen.sh --skip fixfpmem,fixsimd   # leave passes out (ablation)
# Options combine. Each run writes build/fork_regen_manifest.txt (seeds, functions,
# stubs, passes applied).
#
# Steps:
#   1. the four steps of port/tools/regen.sh into build/gen_fresh, seeded with
#      port/src/seeds.json + port/tools/fork/extra_seeds.txt (deterministic, ~1.5 min)
#   2. copy into port/src/recomp/gen/ (the previous gen/ is moved to build/gen.prev)
#   3. link hookups: fork shim (g_last_loc, g_main_loc,
#      recomp_dispatch_init) and the 8 XInput bodies renamed *_lifted
#   3b. hand-written bodies (one: 0x001788B2)
#   4. the upstream post-passes listed in PASSES below, in order, each one once,
#      then the fork's own passes (port/tools/fork/*.py)
#
# Every change to gen/ is made by this script; nothing in gen/ is edited by hand.
# Needs the MSYS2 UCRT64 python (capstone): PATH=/c/msys64/ucrt64/bin:$PATH
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GEN="$ROOT/port/src/recomp/gen"
FRESH="$ROOT/build/gen_fresh"
AUDIT="$ROOT/xboxrecomp/tools/audit"
FORK="$ROOT/port/tools/fork"
PY="${PYTHON:-python}"
UPTO=999; ONLY=0; SKIP=""
while [ $# -gt 0 ]; do
    case "$1" in
        --upto) UPTO="$2"; shift 2 ;;
        --only) ONLY="$2"; shift 2 ;;
        --skip) SKIP=",$2,"; shift 2 ;;
        *) echo "unknown option: $1"; exit 2 ;;
    esac
done
MANIFEST="$ROOT/build/fork_regen_manifest.txt"

if [ "$ONLY" = 0 ]; then

echo "== [1] regen"
# Same four steps as port/tools/regen.sh, with one difference: the seed list is
# port/src/seeds.json plus port/tools/fork/extra_seeds.txt (one address per line,
# '#' comments) -- functions upstream recovered after the first pass
# (recover_batch.py) that are not in seeds.json.
rm -rf "$FRESH"
WORK="$ROOT/build/regen"
mkdir -p "$WORK"
SEEDS="$WORK/seeds_fork.json"
"$PY" - "$ROOT/port/src/seeds.json" "$FORK/extra_seeds.txt" "$SEEDS" <<'EOF'
import json, sys
seeds = json.load(open(sys.argv[1]))
have = {int(s["start"], 16) for s in seeds}
extra = []
for line in open(sys.argv[2]):
    line = line.split("#")[0].strip()
    if line and int(line, 16) not in have:
        have.add(int(line, 16))
        extra.append({"start": "0x%08X" % int(line, 16)})
json.dump(seeds + extra, open(sys.argv[3], "w"), indent=0)
print("   seeds: %d + %d extra" % (len(seeds), len(extra)))
EOF
(
    cd "$ROOT/xboxrecomp"
    XBE="$ROOT/game_files/default.xbe"
    "$PY" -m tools.xbe_parser "$XBE" --json "$WORK/analysis.json" --quiet
    "$PY" -m tools.disasm "$XBE" -o "$WORK/disasm" --text-only --force \
        --analysis-json "$WORK/analysis.json" --seed-functions "$SEEDS"
    "$PY" -m tools.func_id "$XBE" --functions "$WORK/disasm/functions.json" \
        --xrefs "$WORK/disasm/xrefs.json" --strings "$WORK/disasm/strings.json" \
        --output "$WORK/funcid"
    mkdir -p "$FRESH"
    "$PY" -m tools.recomp "$XBE" --all --split 1000 --gen-dir "$FRESH" -o "$WORK/recomp" \
        --disasm-dir "$WORK/disasm" --func-id-dir "$WORK/funcid" --skip-binary-check
) > "$ROOT/build/fork_regen_regen.log" 2>&1 \
    || { tail -20 "$ROOT/build/fork_regen_regen.log"; exit 1; }
grep -E "functions \(|unresolved call targets" "$ROOT/build/fork_regen_regen.log" | head -5 | sed 's/^/   /' || true

echo "== [2] copy into gen/"
if [ -d "$GEN" ]; then rm -rf "$ROOT/build/gen.prev"; mv "$GEN" "$ROOT/build/gen.prev"; fi
mkdir -p "$(dirname "$GEN")"
cp -r "$FRESH" "$GEN"

echo "== [3] link hookups"
cp "$FORK/recomp_fork_shim.c" "$GEN/"
# port/src/xapi_input_hle.c replaces these 8 XInput bodies; upstream renames the
# translated ones <name>_lifted (xapi_input_hle.c:11-13). The file number varies.
for n in sub_0017FF2C sub_00180916 sub_0018098B sub_00180997 sub_00180B89 sub_00180BFA sub_00180C59 sub_00180C7B; do
    f=$(grep -l -E "^void ${n}\(void\)"$'\r'"?\$" "$GEN"/recomp_*.c)
    sed -i -E "s/^void ${n}\(void\)(\r?)\$/void ${n}_lifted(void)\1/" "$f"
    grep -q -E "^void ${n}_lifted\(void\)" "$f" || { echo "rename $n failed"; exit 1; }
done

echo "== [3b] hand-written bodies (checked against the XBE bytes)"
# 0x001788B2 is `ret 0x20` (c2 20 00), the epilogue of sub_00178164 (XGRPH). The
# instruction straddles the end of XGRPH's raw data (raw ends at 0x001788B4, the
# imm16 high byte is in the zero-filled tail), so tools.disasm never decodes it
# and the regen leaves an empty "not detected" stub: sub_00178164 then returns
# without popping its 8 arguments (esp 0x24 off at every caller).
# Lifter convention for `ret N`: esp += 4 + N.
STUBS="$GEN/recomp_stubs_unresolved.c"
sed -i -E 's|^void sub_001788B2\(void\) \{ /\* 0x001788B2: not detected \*/ \}|void sub_001788B2(void) { esp += 0x24; return; /* ret 0x20 -- hand-written, fork_regen.sh [3b] */ }|' "$STUBS"
grep -q 'sub_001788B2(void) { esp += 0x24' "$STUBS" || { echo "hand body 0x001788B2 not applied"; exit 1; }

{
    echo "seeds: $(grep -c '^0x' "$FORK/extra_seeds.txt") extra (port/tools/fork/extra_seeds.txt) + port/src/seeds.json"
    grep -E "functions \(|unresolved call targets" "$ROOT/build/fork_regen_regen.log" | sed 's/^ */regen: /' || true
    echo "hand-written bodies: 0x001788B2"
} > "$MANIFEST"

fi

echo "== [4] passes"
# One entry per pass: "name|command". Applied in this order, each exactly once.
# Order: the historical order of the upstream parts, then
# dependencies. Tools write their own backups next to the files (.bak_*), which
# CMake does not compile.
PASSES=(
  "instrument_labels|$PY instrument_labels.py"
  "fixfpbranch|$PY fixfpbranch.py"
  "fixrepstr|$PY fixrepstr.py"
  "fixscalar|$PY fixscalar.py"
  "fixsimd|$PY fixsimd.py"
  "fixsplitflags|$PY fixsplitflags.py"
  "fixicallsaves|$PY fixicallsaves.py \"$GEN\" --apply"
  "fixnarrowsign|$PY fixnarrowsign.py"
  "fixfpret|$PY fixfpret.py"
  "fpretcheck|$PY fpretcheck.py"
  "fixfpmem|$PY fixfpmem.py"
  "fork_icall_esp_target|$PY \"$FORK/fix_icall_esp_target.py\" \"$GEN\""
  "fork_manual_hook_calls|$PY \"$FORK/fix_manual_hook_calls.py\" \"$GEN\" \"$ROOT/port/src\""
  "fork_fpu_cmp_split|$PY \"$FORK/fix_fpu_cmp_split.py\" \"$GEN\""
  "fork_std_scasb|$PY \"$FORK/fix_std_scasb.py\" \"$GEN\""
  "fork_kickwait|$PY \"$FORK/fix_kickwait.py\" \"$GEN\""
)
i=0
for p in "${PASSES[@]}"; do
    i=$((i + 1))
    [ "$i" -gt "$UPTO" ] && { echo "   (stopped after pass $UPTO)"; break; }
    [ "$ONLY" != 0 ] && [ "$i" != "$ONLY" ] && continue
    name="${p%%|*}"; cmd="${p#*|}"
    case "$SKIP" in *",$name,"*) echo "   [$i] $name -- skipped (--skip)"; echo "pass $i $name: SKIPPED" >> "$MANIFEST"; continue ;; esac
    echo "   [$i] $name"
    (cd "$AUDIT" && eval "$cmd") > "$ROOT/build/fork_regen_pass$i.log" 2>&1 \
        || { echo "pass $i ($name) failed:"; tail -20 "$ROOT/build/fork_regen_pass$i.log"; exit 1; }
    tail -3 "$ROOT/build/fork_regen_pass$i.log" | sed 's/^/       /'
    if [ "$ONLY" != 0 ]; then echo "then --only pass $i $name" >> "$MANIFEST"; else echo "pass $i $name" >> "$MANIFEST"; fi
done
[ "$UPTO" -lt "${#PASSES[@]}" ] && echo "(stopped after pass $UPTO: --upto)" >> "$MANIFEST"
echo "done: $GEN"
