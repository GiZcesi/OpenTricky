#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Package dist/OpenTricky (made by build.sh) as a release zip.

    OT_RELEASE=1 ./build.sh "path/to/SSX Tricky (USA).iso"
    python port/tools/make_release.py [label]      # e.g. 0.1.4

    dist/OpenTricky-<label>-win64/
        OpenTricky.exe    the launcher, with its ui/ folder and
        ui/               THIRD-PARTY-LICENSES.txt
        THIRD-PARTY-LICENSES.txt
        SSX Tricky.exe    (+ the DLLs build.sh copied next to it)
        README.txt        short, in English
        LICENSE.txt       GPL-3.0-only (xboxrecomp's MIT and xemu's LGPL notices
                          are in the source repository)
    dist/OpenTricky-<label>-win64.zip

No settings file: the launcher writes settings.ini at the first start, with
the defaults of the settings registry (port/src/settings.c).

The label defaults to the version of port/src/version.h and must start with
it. Refused: executables that still say "-dev" (build.sh without
OT_RELEASE=1), a "What's new" block (port/launcher/app/whatsnew.txt) of
another version or marked "draft:", and anything else in the folder: no disc
image, no save, no log, no settings file, no absolute path in the text files.

The executable contains code translated from the game. Whether it may be
distributed is the publisher's decision -- this script only prepares the zip.
"""
import datetime, os, re, shutil, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "dist", "OpenTricky")
EXE = "SSX Tricky.exe"
LAUNCHER = "OpenTricky.exe"
LICENCES = "THIRD-PARTY-LICENSES.txt"

# The README of the zip: the same text as the README of the project's release
# zips, for this zip's layout (the game beside the launcher, not in a bin
# folder) and with its LICENSE.txt.
TITLE = "OpenTricky {version} -- SSX Tricky on PC (static recompilation of the Xbox game)"
README = """
Start
  Double-click "OpenTricky.exe", the launcher. The first time, choose your
  own copy of SSX Tricky (USA) for Xbox as a disc image (.iso or .xiso), or
  drop it on the launcher's window: the launcher remembers it. PLAY starts
  the game; Settings has the display, graphics, audio, controls and other
  options (settings.ini, written by the launcher beside it).
  The game itself is beside the launcher: "SSX Tricky.exe" also starts it
  directly, with the same settings.
  With Settings > Advanced > Skip launcher next time on, the game starts
  directly; to see the launcher again, hold Shift (or Back / Select on a
  controller) while you start OpenTricky.exe.

Coming from 0.1.2, 0.1.3 or 0.1.4
  Let the launcher update you: your saves, settings and disc image are kept (a
  preset takes its new values). If no update shows up, the 0.1.4 launcher
  has Check now (Settings > Advanced > Updates); a 0.1.2 or 0.1.3 launcher
  checks GitHub once a day: close it, delete update-cache.txt next to
  settings.ini, and open it again.

Coming from 0.1.1
  Install this version fresh: unzip it into a new, empty folder.
  - Saves in Documents\\My Games\\SSX Tricky\\Saves stay where they are. If
    your saves were inside the old folder (a Saves folder with portable.txt,
    or an hdd folder), copy that folder (and portable.txt) into the new
    folder.
  - Do not copy settings.ini: the new defaults are recommended. The launcher
    asks for your disc image again the first time; if you had chosen a save
    folder, choose it again in Settings > Game > Save folder.
  - Then delete the old folder.
  A shortcut to the old "SSX Tricky.exe" (desktop, Steam, Windows graphics
  settings) must be made again: point it to "OpenTricky.exe" (or to the new
  "SSX Tricky.exe").

In the game
  F12          screenshot (Screenshots folder beside OpenTricky.exe, or
               Documents\\My Games\\SSX Tricky\\Screenshots when the folder
               of OpenTricky.exe can't be written)
  Shift+F12    a burst of screenshots
  Alt+Enter    window / fullscreen (F11 too)
  Alt+F4       quit

Saves
  Documents\\My Games\\SSX Tricky\\Saves
  To keep everything in this folder instead (a portable copy), create an
  empty file named portable.txt beside "OpenTricky.exe": saves then go to
  its Saves folder. Settings > Game > Save folder can also point to any
  folder.

Problems
  If the game closes unexpectedly, the launcher offers a bug report: a zip on
  your Desktop with the logs and settings (never your disc image or saves).
  Nothing is sent automatically.

OpenTricky continues SSX Tricky PC by MatiasRiveraC, built on xboxrecomp by
sp00nznet. SSX and SSX Tricky are trademarks of Electronic Arts; this is an
unofficial fan project, not affiliated with EA or Microsoft.
Licences: LICENSE.txt (OpenTricky, GPL-3.0-only) and THIRD-PARTY-LICENSES.txt
  (Settings > Advanced > About OpenTricky > Licences).

Version {version} ({stamp})
"""


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)


def read_version():
    text = open(os.path.join(ROOT, "port", "src", "version.h"), encoding="utf-8").read()
    n = [re.search(r"#define OT_VERSION_%s\s+(\d+)" % k, text) for k in ("MAJOR", "MINOR", "PATCH")]
    if not all(n):
        sys.exit("port/src/version.h: OT_VERSION_MAJOR / MINOR / PATCH not found")
    return ".".join(m.group(1) for m in n)


def check_whatsnew(version):
    """The launcher's What's new block (built into OpenTricky.exe)."""
    text = open(os.path.join(ROOT, "port", "launcher", "app", "whatsnew.txt"), encoding="utf-8").read()
    a = text.find("<!-- launcher")
    b = text.find("-->", a) if a >= 0 else -1
    if a < 0 or b < 0:
        return ["whatsnew.txt: no <!-- launcher --> block"]
    keys = {}
    for line in text[a + len("<!-- launcher"):b].splitlines():
        k, sep, v = line.partition(":")
        if sep and v.strip():
            keys.setdefault(k.strip().lower(), v.strip())
    bad = []
    if keys.get("version") != version:
        bad.append("whatsnew.txt: version %r, the release is %s" % (keys.get("version"), version))
    if "draft" in keys:
        bad.append("whatsnew.txt: the What's new block is still a draft (%s)" % keys["draft"])
    return bad


def check_exe(path, version):
    data = open(path, "rb").read()
    dev = version + "-dev"
    if dev.encode() in data or dev.encode("utf-16-le") in data:
        return ["%s says %s: build with OT_RELEASE=1 ./build.sh" % (os.path.basename(path), dev)]
    if version.encode() not in data:
        return ["%s: version %s not found inside" % (os.path.basename(path), version)]
    return []


def check_clean(dst):
    allowed = {EXE.lower(), LAUNCHER.lower(), LICENCES.lower(), "readme.txt", "license.txt"}
    bad = []
    for root, dirs, files in os.walk(dst):
        in_ui = os.path.relpath(root, dst).replace("\\", "/").split("/")[0] == "ui"
        if root == dst:
            bad += ["folder %s" % d for d in dirs if d != "ui"]
        for f in files:
            k = f.lower()
            if k.endswith((".ini", ".iso", ".xiso", ".log", ".zip", ".sav", ".dmp")) or k == "portable.txt":
                bad.append("file %s" % f)
            elif not in_ui and k not in allowed and not k.endswith(".dll"):
                bad.append("file %s" % f)
            if k.endswith((".txt", ".rml", ".rcss")) and k != "license.txt":
                text = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
                for m in re.finditer(r"(?<![A-Za-z])[A-Za-z]:[\\/]|\\\\[A-Za-z]|/c/", text):
                    bad.append("absolute path in %s: %r" % (f, text[max(0, m.start() - 10):m.end() + 20]))
    return bad


def main():
    if len(sys.argv) > 2 or (len(sys.argv) == 2 and sys.argv[1].startswith("-")):
        sys.exit(__doc__)
    version = read_version()
    label = sys.argv[1] if len(sys.argv) == 2 else version
    if label != version and not label.startswith(version + "-"):
        sys.exit("label %s: the version is %s (port/src/version.h)" % (label, version))
    for f in (EXE, LAUNCHER, LICENCES, "ui"):
        if not os.path.exists(os.path.join(SRC, f)):
            sys.exit("%s not found: run build.sh first" % os.path.join(SRC, f))
    bad = check_whatsnew(version) + check_exe(os.path.join(SRC, EXE), version) \
        + check_exe(os.path.join(SRC, LAUNCHER), version)
    if bad:
        sys.exit("release refused:\n  " + "\n  ".join(bad))

    name = "OpenTricky-%s-win64" % label
    dst = os.path.join(ROOT, "dist", name)
    if os.path.exists(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    for f in os.listdir(SRC):
        if f in (EXE, LAUNCHER, LICENCES) or f.lower().endswith(".dll"):
            shutil.copy2(os.path.join(SRC, f), dst)
    shutil.copytree(os.path.join(SRC, "ui"), os.path.join(dst, "ui"))
    title = TITLE.format(version=label)
    write(os.path.join(dst, "README.txt"), title + "\n" + "=" * len(title) + "\n" +
          README.format(version=label, stamp=datetime.date.today().isoformat()))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(dst, "LICENSE.txt"))

    bad = check_clean(dst)
    if bad:
        sys.exit("release refused:\n  " + "\n  ".join(bad))
    z = dst + ".zip"
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for root, dirs, files in os.walk(dst):
            dirs.sort()
            for f in sorted(files):
                full = os.path.join(root, f)
                rel = os.path.relpath(full, dst)
                zf.write(full, os.path.join(name, rel))
                print("  %10d  %s" % (os.path.getsize(full), rel))
    print("zip: %s (%.1f MB)" % (z, os.path.getsize(z) / 1e6))


if __name__ == "__main__":
    main()
