#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# OpenTricky -- build the game from your own SSX Tricky (USA) Xbox disc image.
#
#   ./build.sh "path/to/SSX Tricky (USA).iso"
#   OT_RELEASE=1 ./build.sh ...   a release build: the version without "-dev"
#                                 (then python port/tools/make_release.py)
#
# Run from an MSYS2 UCRT64 shell (or use build.bat, which opens one for you).
# Result: dist/OpenTricky/ -- "OpenTricky.exe" (the launcher, with its ui/ folder
# and THIRD-PARTY-LICENSES.txt), "SSX Tricky.exe" and the DLLs it needs.
# The extracted files (game_files/) and the translated code
# (port/src/recomp/gen/) are git-ignored.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

ISO="${1:-}"
if [ -z "$ISO" ] && [ ! -f game_files/default.xbe ]; then
    echo "usage: ./build.sh \"path/to/SSX Tricky (USA).iso\""
    exit 2
fi

echo "== [0/5] checking the tools"
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
    echo "== [1/5] reading your disc image"
    python port/tools/extract_iso.py "$ISO" game_files
else
    echo "== [1/5] game_files/default.xbe already there"
fi

echo "== [2/5] translating the game (about 3 minutes)"
mkdir -p build
PYTHON=python bash port/tools/fork_regen.sh

echo "== [3/5] compiling the game (5 to 10 minutes)"
# Source paths are recorded relative to this folder: no user name in the exe.
WROOT="$(cygpath -m "$ROOT" 2>/dev/null || echo "$ROOT")"
# Release or not (OT_RELEASE=1): switching recompiles everything once.
REL=OFF; [ "${OT_RELEASE:-0}" = "1" ] && REL=ON
if [ ! -f port/build/CMakeCache.txt ]; then
    cmake -S port -B port/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="-O3 -g -DNDEBUG" \
        -DCMAKE_C_FLAGS="-Wno-error=int-conversion -ffile-prefix-map=$WROOT/=" -DOT_RELEASE=$REL
else
    cmake -S port -B port/build -DOT_RELEASE=$REL >/dev/null
fi
cmake --build port/build -j "$(nproc)"

echo "== [4/5] compiling the launcher, OpenTricky.exe (about 2 minutes the first time)"
# Its libraries (SDL3, FreeType, RmlUi) are built from port/third_party.
if [ ! -f port/build-launcher/CMakeCache.txt ]; then
    cmake -S port/launcher/app -B port/build-launcher -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-ffile-prefix-map=$WROOT/=" -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$WROOT/=" -DOT_RELEASE=$REL
else
    cmake -S port/launcher/app -B port/build-launcher -DOT_RELEASE=$REL >/dev/null
fi
cmake --build port/build-launcher -j "$(nproc)"

echo "== [5/5] dist/OpenTricky"
EXE="port/build/SSX Tricky.exe"
OUT="dist/OpenTricky"
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$EXE" "$OUT/"
# The launcher: one static exe, its interface files and the licences.
cp port/build-launcher/OpenTricky.exe port/build-launcher/THIRD-PARTY-LICENSES.txt "$OUT/"
cp -r port/build-launcher/ui "$OUT/"
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
echo "Done. Start \"$OUT/OpenTricky.exe\" (the launcher), choose the same disc image"
echo "(or drop it on its window), then PLAY. \"SSX Tricky.exe\" then starts the game directly."
echo "This build contains code translated from your copy of the game: please don't share it."
