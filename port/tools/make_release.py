#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package dist/OpenTricky (made by build.sh) as a release zip.

    python port/tools/make_release.py 0.1.0-unstable

    dist/OpenTricky-<version>-win64/
        SSX Tricky.exe    (+ the DLLs build.sh copied next to it)
        SSX Tricky.ini    neutral: no disc image, automatic save folder,
                          every other setting at the launcher's default
        README.txt        short, in English
        LICENSE.txt       GPL-3.0-only (xboxrecomp's MIT and xemu's LGPL notices
                          are in the source repository)
    dist/OpenTricky-<version>-win64.zip

Refuses to package anything else: no disc image, no save, no log, no
absolute path in the text files.

The executable contains code translated from the game. Whether it may be
distributed is the publisher's decision -- this script only prepares the zip.
"""
import datetime, os, re, shutil, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "dist", "OpenTricky")
EXE = "SSX Tricky.exe"

INI = r"""; SSX Tricky (OpenTricky) -- settings, written by the launcher's SETTINGS.
; Safe to edit by hand while the game is closed. Missing settings take the
; launcher's defaults.

[Game]
; Your own SSX Tricky (USA) Xbox disc image (.iso). The launcher asks for it
; at the first start.
DiscImage=
; Empty = automatic: Documents\My Games\SSX Tricky\Saves, or Saves\ next to
; the game when a file named portable.txt is there.
SaveFolder=
"""

README = """OpenTricky {version} -- SSX Tricky (Xbox) for Windows
=========================================================

A recompiled PC port of SSX Tricky for the original Xbox. Not an emulator.
UNSTABLE release: see "Known issues" on the project page.
This download is the executable only: no disc image, game data, music or
video. You need your own SSX Tricky (USA) Xbox disc
image (.iso).

Start
  Double-click "SSX Tricky.exe". The first time, choose your disc image:
  the launcher remembers it. PLAY starts the game; SETTINGS has the video,
  audio, controls and advanced options; QUIT (or Esc) closes the launcher.
  Out of the box the game looks like the Xbox (4:3, 60 FPS, no extras).

In the game
  Alt+Enter / F11   window / fullscreen
  F12               screenshot (Screenshots folder next to the game)
  Alt+F4            quit
  An Xbox-style controller is used if one is plugged in.

Saves
  Documents\\My Games\\SSX Tricky\\Saves
  For a portable copy, create an empty file named portable.txt next to
  "SSX Tricky.exe": saves then go to its Saves folder.

Problems
  SETTINGS > ADVANCED > Log file writes "SSX Tricky.log" next to the game.
  Attach it to a bug report: https://github.com/GiZcesi/OpenTricky/issues

OpenTricky continues SSX Tricky PC by MatiasRiveraC, built on xboxrecomp by
sp00nznet. SSX and SSX Tricky are trademarks of Electronic Arts; this is an
unofficial fan project, not affiliated with EA or Microsoft.

Version {version} ({stamp})
"""


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)


def check_clean(dst):
    allowed = {EXE.lower(), "ssx tricky.ini", "readme.txt", "license.txt"}
    bad = []
    for root, dirs, files in os.walk(dst):
        bad += ["folder %s" % d for d in dirs]
        for f in files:
            k = f.lower()
            if k not in allowed and not k.endswith(".dll"):
                bad.append("file %s" % f)
            if k.endswith((".ini", ".txt")) and k != "license.txt":
                text = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
                for m in re.finditer(r"(?<![A-Za-z])[A-Za-z]:[\\/]|\\\\[A-Za-z]|/c/", text):
                    bad.append("absolute path in %s: %r" % (f, text[max(0, m.start() - 10):m.end() + 20]))
    return bad


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    version = sys.argv[1]
    if not os.path.isfile(os.path.join(SRC, EXE)):
        sys.exit("%s not found: run build.sh first" % os.path.join(SRC, EXE))
    name = "OpenTricky-%s-win64" % version
    dst = os.path.join(ROOT, "dist", name)
    if os.path.exists(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    for f in os.listdir(SRC):
        if f == EXE or f.lower().endswith(".dll"):
            shutil.copy2(os.path.join(SRC, f), dst)
    write(os.path.join(dst, "SSX Tricky.ini"), INI)
    write(os.path.join(dst, "README.txt"),
          README.format(version=version, stamp=datetime.date.today().isoformat()))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(dst, "LICENSE.txt"))

    bad = check_clean(dst)
    if bad:
        sys.exit("release refused:\n  " + "\n  ".join(bad))
    z = dst + ".zip"
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for f in sorted(os.listdir(dst)):
            zf.write(os.path.join(dst, f), os.path.join(name, f))
    for f in sorted(os.listdir(dst)):
        print("  %10d  %s" % (os.path.getsize(os.path.join(dst, f)), f))
    print("zip: %s (%.1f MB)" % (z, os.path.getsize(z) / 1e6))


if __name__ == "__main__":
    main()
