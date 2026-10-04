#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# OpenTricky -- build the game from your own SSX Tricky (USA) Xbox disc image.
#
#   ./build.sh "path/to/SSX Tricky (USA).iso"
#
# Run from an MSYS2 UCRT64 shell (or use build.bat, which opens one for you).
# Result: dist/OpenTricky/ -- "SSX Tricky.exe" and the DLLs it needs.
# Nothing from the game is ever added to the repository: the extracted files
# (game_files/) and the translated code (port/src/recomp/gen/) are git-ignored.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

ISO="${1:-}"
if [ -z "$ISO" ] && [ ! -f game_files/default.xbe ]; then
    echo "usage: ./build.sh \"path/to/SSX Tricky (USA).iso\""
    exit 2
fi

echo "== [0/4] checking the tools"
missing=""
for t in gcc cmake mingw32-make python objdump; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
    echo "missing:$missing"
    echo "In an MSYS2 UCRT64 shell:"
    echo "  pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake make \\"
    echo "            mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone \\"
    echo "            mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow"
    exit 1
fi
python -c "import capstone, numpy, PIL" 2>/dev/null || {
    echo "Python needs capstone, numpy and pillow:"
    echo "  pacman -S --needed mingw-w64-ucrt-x86_64-python-capstone mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow"
    exit 1
}

if [ -n "$ISO" ]; then
    echo "== [1/4] reading your disc image"
    python port/tools/extract_iso.py "$ISO" game_files
else
    echo "== [1/4] game_files/default.xbe already there"
fi

echo "== [2/4] translating the game (about 3 minutes)"
mkdir -p build
PYTHON=python bash port/tools/fork_regen.sh

echo "== [3/4] compiling (5 to 10 minutes)"
# Source paths are recorded relative to this folder: no user name in the exe.
WROOT="$(cygpath -m "$ROOT" 2>/dev/null || echo "$ROOT")"
if [ ! -f port/build/CMakeCache.txt ]; then
    cmake -S port -B port/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="-O3 -g -DNDEBUG" \
        -DCMAKE_C_FLAGS="-Wno-error=int-conversion -ffile-prefix-map=$WROOT/="
fi
cmake --build port/build -j "$(nproc)"

echo "== [4/4] dist/OpenTricky"
EXE="port/build/SSX Tricky.exe"
OUT="dist/OpenTricky"
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$EXE" "$OUT/"
# Every DLL the game imports that does not ship with Windows (from MSYS2).
UCRT="$(dirname "$(command -v gcc)")"
todo=("$EXE"); seen=" "
while [ ${#todo[@]} -gt 0 ]; do
    f="${todo[0]}"; todo=("${todo[@]:1}")
    for d in $(objdump -p "$f" | sed -n 's/^\s*DLL Name: //p'); do
        k="$(echo "$d" | tr 'A-Z' 'a-z')"
        case "$seen" in *" $k "*) continue ;; esac
        seen="$seen$k "
        if [ -f "$UCRT/$d" ]; then
            cp "$UCRT/$d" "$OUT/"
            todo+=("$UCRT/$d")
        fi
    done
done
ls "$OUT"
echo
echo "Done. Start \"$OUT/SSX Tricky.exe\" and pick the same disc image in the launcher."
echo "This build contains code translated from your copy of the game: please don't share it."
