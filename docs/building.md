# Building OpenTricky from source

OpenTricky is built from two parts:

- **The translated game.** SSX Tricky's Xbox executable (`default.xbe`) is turned into C by
  [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), then corrected by a series of fix-up passes.
  This code is generated from **your own copy of the game** and is never stored in this repository.
  It lands in `port/src/recomp/gen/`, which git ignores.
- **Everything else**: the Xbox runtime (kernel, GPU, audio, input) in `xboxrecomp/src/`, and the PC side
  (launcher, settings, controls, interpolation, ultrawide…) in `port/src/`.

## What you need

| | |
|---|---|
| **MSYS2** | [msys2.org](https://www.msys2.org/), installed in `C:\msys64` (or set `MSYS2_ROOT`). |
| **Packages** | In the **UCRT64** shell: `pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake make mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow` |
| **The game** | A disc image of your own **SSX Tricky (USA) Xbox** disc. |

## One command

```bat
build.bat "C:\path\to\SSX Tricky (USA).iso"
```

or, from the UCRT64 shell:

```bash
./build.sh "/c/path/to/SSX Tricky (USA).iso"
```

It runs four steps:

1. **Reads your disc image.** `port/tools/extract_iso.py` copies `default.xbe` and two texture files (used for the program icon) into `game_files/`.
   Nothing else is extracted. The game reads your disc image directly when you play.
2. **Translates the game.** `port/tools/fork_regen.sh` translates every known function (`port/src/seeds.json`, plus
   `port/tools/fork/extra_seeds.txt`), links the hand-written pieces, then applies the fix-up passes in order:
   the upstream ones in `xboxrecomp/tools/audit/fix*.py`, then OpenTricky's in `port/tools/fork/`.
   The output is the same byte for byte from one run to the next. Every change to `gen/` is made by this script; nothing in it is edited by hand.
3. **Compiles** with CMake and GCC (`-O3 -g`). A full build takes a few minutes on a fast PC.
4. **Copies** `SSX Tricky.exe` and the MSYS2 DLLs it needs into `dist/OpenTricky/`.

Run it again after pulling changes. The `.iso` argument can be left out once `game_files/default.xbe` exists.

> [!CAUTION]
> The build contains code translated from your copy of the game. Please don't share it: point people to the releases instead.

## By hand

```bash
python port/tools/extract_iso.py "/c/path/to/game.iso" game_files
bash port/tools/fork_regen.sh
cmake -S port -B port/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS_RELEASE="-O3 -g -DNDEBUG" -DCMAKE_C_FLAGS="-Wno-error=int-conversion -ffile-prefix-map=$(cygpath -m "$PWD")/="
cmake --build port/build -j
```

`fork_regen.sh` has a few options for working on the translation:
- `--upto N` stops after pass N;
- `--only N` applies a single pass;
- `--skip name,…` leaves passes out.

`-g` keeps line information in the optimised build, which is what lets `addr2line` and the crash reporter name a generated line.

## Running without the launcher

```bash
"port/build/SSX Tricky.exe" --direct "/c/path/to/game.iso"
```

`--direct` starts straight away at 640×480, with the save folder `hdd/` next to the executable.
Settings can also be given as `XBOX_*` environment variables, which win over `SSX Tricky.ini`.
The diagnostic switches (`XBOX_FPS_LOG`, `XBOX_PROFILE`, `XBOX_NV2A_DRAWLOG`, …) are listed in `xboxrecomp/tools/audit/README.md`.

