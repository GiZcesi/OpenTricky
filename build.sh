#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# OpenTricky -- build the game from your own SSX Tricky (USA) Xbox disc image.
#
#   ./build.sh "path/to/SSX Tricky (USA).iso"
#   OT_RELEASE=1 ./build.sh ...   a release build: the version without "-dev"
#                                 (then python port/tools/make_release.py)
#
# Run from an MSYS2 UCRT64 shell (or use build.bat, which opens one for you).
# On macOS (Darwin): headless bring-up only, see MACOS_PORT_PLAN.md.
# Result: dist/OpenTricky/ -- "OpenTricky.exe" (the launcher, with its ui/ folder
# and THIRD-PARTY-LICENSES.txt), "SSX Tricky.exe" and the DLLs it needs.
# The extracted files (game_files/) and the translated code
# (port/src/recomp/gen/) are git-ignored.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# Host differences: the MSYS2 UCRT64 toolchain on Windows, Homebrew + Ninja on macOS.
if [ "$(uname -s)" = "Darwin" ]; then
    GEN="Ninja"; MAKE_TOOL="ninja"; NPROC="$(sysctl -n hw.ncpu)"
    # a virtualenv with capstone, numpy and pillow: here, or beside the repository, else python3
    if [ -z "${PYTHON:-}" ]; then
        for v in "$ROOT/.venv" "$ROOT/../.venv"; do [ -x "$v/bin/python" ] && PYTHON="$v/bin/python" && break; done
        PYTHON="${PYTHON:-python3}"
    fi
    export PYTHON
    # OT_GPU=0 builds the null renderer instead (movies only, nothing drawn)
    OT_GPU_OPT="-DOT_GPU=ON"; [ "${OT_GPU:-1}" = "0" ] && OT_GPU_OPT="-DOT_GPU=OFF"
    OT_HEADLESS_OPT="-DOT_HEADLESS=ON $OT_GPU_OPT -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl"
    # The translation passes read GNU objdump's output (Apple's is LLVM's: no `-b binary`).
    # brew install binutils; it is keg-only, so expose just objdump to the passes.
    GNU_OBJDUMP="$(command -v gobjdump || true)"
    [ -z "$GNU_OBJDUMP" ] && [ -x /opt/homebrew/opt/binutils/bin/objdump ] && GNU_OBJDUMP=/opt/homebrew/opt/binutils/bin/objdump
    if [ -n "$GNU_OBJDUMP" ]; then
        mkdir -p build/gnu-bin && ln -sf "$GNU_OBJDUMP" build/gnu-bin/objdump
        export PATH="$ROOT/build/gnu-bin:$PATH"
    else
        echo "missing: GNU objdump -- brew install binutils"; exit 1
    fi
else
    GEN="MinGW Makefiles"; MAKE_TOOL="mingw32-make"; NPROC="$(nproc)"
    PYTHON="${PYTHON:-python}"; export PYTHON
    OT_HEADLESS_OPT=""
fi

ISO="${1:-}"
if [ -z "$ISO" ] && [ ! -f game_files/default.xbe ]; then
    echo "usage: ./build.sh \"path/to/SSX Tricky (USA).iso\""
    exit 2
fi

echo "== [0/5] checking the tools"
missing=""
for t in cmake "$MAKE_TOOL" "$PYTHON"; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
    echo "missing:$missing"
    if [ "$(uname -s)" = "Darwin" ]; then echo "macOS: brew install cmake ninja binutils; python -m venv .venv && .venv/bin/pip install capstone numpy pillow"; exit 1; fi
    echo "In an MSYS2 UCRT64 shell:"
    echo "  pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake make \\"
    echo "            mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone \\"
    echo "            mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow"
    exit 1
fi
"$PYTHON" -c "import capstone, numpy, PIL" 2>/dev/null || {
    echo "Python needs capstone, numpy and pillow:"
    echo "  pacman -S --needed mingw-w64-ucrt-x86_64-python-capstone mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow"
    exit 1
}

if [ -n "$ISO" ]; then
    echo "== [1/5] reading your disc image"
    "$PYTHON" port/tools/extract_iso.py "$ISO" game_files
else
    echo "== [1/5] game_files/default.xbe already there"
fi

echo "== [2/5] translating the game (about 3 minutes)"
mkdir -p build
bash port/tools/fork_regen.sh

echo "== [3/5] compiling the game (5 to 10 minutes)"
# Source paths are recorded relative to this folder: no user name in the exe.
WROOT="$(cygpath -m "$ROOT" 2>/dev/null || echo "$ROOT")"
# Release or not (OT_RELEASE=1): switching recompiles everything once.
REL=OFF; [ "${OT_RELEASE:-0}" = "1" ] && REL=ON
if [ ! -f port/build/CMakeCache.txt ]; then
    cmake -S port -B port/build -G "$GEN" -DCMAKE_BUILD_TYPE=Release $OT_HEADLESS_OPT \
        -DCMAKE_C_FLAGS_RELEASE="-O3 -g -DNDEBUG" \
        -DCMAKE_C_FLAGS="-Wno-error=int-conversion -ffile-prefix-map=$WROOT/=" -DOT_RELEASE=$REL
else
    cmake -S port -B port/build -DOT_RELEASE=$REL >/dev/null
fi
cmake --build port/build -j "$NPROC"

if [ "$(uname -s)" = "Darwin" ]; then
    # Headless bring-up: no launcher, no DLL bundling (steps 4 and 5).
    mkdir -p dist/headless && cp "port/build/SSX Tricky" dist/headless/
    echo "Done (macOS: a window with the movies and the first 3D: untextured, no sound): dist/headless/SSX Tricky"
    echo "Run it from an empty folder:  \"dist/headless/SSX Tricky\" --direct <your .iso>"
    echo "XBOX_HEADLESS=1 runs without a window; XBOX_DUMP_FB=<folder> saves the movie frames as PPM pictures."
    echo "(needs: brew install sdl3). This build contains code"
    echo "translated from your copy of the game: please don't share it."
    exit 0
fi

echo "== [4/5] compiling the launcher, OpenTricky.exe (about 2 minutes the first time)"
# Its libraries (SDL3, FreeType, RmlUi) are built from port/third_party.
if [ ! -f port/build-launcher/CMakeCache.txt ]; then
    cmake -S port/launcher/app -B port/build-launcher -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-ffile-prefix-map=$WROOT/=" -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$WROOT/=" -DOT_RELEASE=$REL
else
    cmake -S port/launcher/app -B port/build-launcher -DOT_RELEASE=$REL >/dev/null
fi
cmake --build port/build-launcher -j "$NPROC"

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
