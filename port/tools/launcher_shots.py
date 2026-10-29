#!/usr/bin/env python3
"""
launcher_shots.py -- pictures of every screen and state of the launcher
(OpenTricky.exe), to compare two builds pixel by pixel.

  launcher_shots.py run <launcher dir> <out dir> --iso <disc image> [--only a,b] [--list]
      Copies the launcher folder (OpenTricky.exe, ui\\, ...) into <out dir>\\_run,
      then starts it once per case with a test script (--script, ending with
      shot:<file>): <out dir>\\<case>.png and <case>.log. Each case starts from
      no settings.ini and no update cache; the update check reads a local file
      (--updates-json), never the network. Fixtures (a wrong disc image, a
      crash folder, release lists) go to _fixtures beside <out dir>, shared
      by the runs that are compared. The window is never brought to
      the front; no controller is opened (the controller cases push SDL events).

  launcher_shots.py compare <dir A> <dir B> [--out <dir>] [--sheet]
      Pixels that differ between the two runs, case by case (0 = identical).
      --sheet also writes <out>\\<case>.png (A | B side by side, with labels)
      and <out>\\index.html, the board for a look by eye.

Exit code of compare: 0 when every common case is identical, 1 otherwise.
A mouse moved over the window during a run hides the keyboard / controller
focus of that case (as for a player): run it again with --only.
Needs Pillow (compare only).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

TRACK = "3"

# name: (window size, extra arguments, script before the shot, wait frames before the shot)
# @MISSING / @WRONG / @NEW / @OLD / @CRASH are replaced by fixture paths.
CASES = {
    # Home, a verified disc image
    "home":              ("1280x800", ["--iso", "@ISO"], "", 20),
    "home-kbd":          ("1280x800", ["--iso", "@ISO"], "k:down", 20),
    "home-kbd-settings": ("1280x800", ["--iso", "@ISO"], "k:down,k:right", 20),
    "home-kbd-notes":    ("1280x800", ["--iso", "@ISO"], "k:down,k:right,k:right", 20),
    "home-kbd-foot":     ("1280x800", ["--iso", "@ISO"], "k:down,k:down", 20),
    "home-kbd-side":     ("1280x800", ["--iso", "@ISO"], "k:down,k:left", 20),
    "home-pad":          ("1280x800", ["--iso", "@ISO"], "p:down", 20),
    "home-pad-ps2":      ("1280x800", ["--iso", "@ISO", "@PS2"], "p:down", 20),
    "home-latest":       ("1280x800", ["--iso", "@ISO", "--updates-json", "@OLD"], "", 40),
    "home-update":       ("1280x800", ["--iso", "@ISO", "--updates-json", "@NEW"], "", 40),
    "home-update-notes": ("1280x800", ["--iso", "@ISO", "--updates-json", "@NEW"], "wait:30,click:ver", 30),
    "home-wide":         ("1920x1080", ["--iso", "@ISO"], "", 20),
    "home-ultrawide":    ("2560x1080", ["--iso", "@ISO"], "", 20),
    # Home, no good disc image
    "nodisc":            ("1280x800", [], "", 20),
    "nodisc-pad":        ("1280x800", [], "p:down", 20),
    "nodisc-kbd-pill":   ("1280x800", [], "k:down,k:up", 20),
    "nodisc-missing":    ("1280x800", ["--iso", "@MISSING"], "", 20),
    "nodisc-wrong":      ("1280x800", ["--iso", "@WRONG"], "", 20),
    # Settings, each tab (the first press shows the focus)
    "set-display":       ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "0"], "", 20),
    "set-display-kbd":   ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "0"], "k:down,k:down", 20),
    "set-graphics-kbd":  ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "1"], "k:down,k:down", 20),
    "set-audio-kbd":     ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "2"], "k:down", 20),
    "set-controls-kbd":  ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "3"], "k:down,k:down", 20),
    "set-controls-cell": ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "3"],
                          "k:down,k:down,k:down,k:down,k:down", 20),
    "set-game-kbd":      ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "4"], "k:down", 20),
    "set-advanced-kbd":  ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "5"], "k:down,k:down", 20),
    "set-advanced-pad":  ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "5"], "p:down,p:down", 20),
    "set-changed":       ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "1"], "k:down,k:right,wait:10,k:down", 20),
    "set-nodisc":        ("1280x800", ["--page", "settings", "--tab", "0"], "", 20),
    "set-wide":          ("1920x1080", ["--iso", "@ISO", "--page", "settings", "--tab", "1"], "k:down", 20),
    # Dialogs
    "dlg-about":         ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "5"], "click:r1002,wait:10,k:down", 20),
    "dlg-report":        ("1280x800", ["--iso", "@ISO"], "click:nav-bug,wait:10,k:down", 20),
    "dlg-capture":       ("1280x800", ["--iso", "@ISO", "--page", "settings", "--tab", "3"], "wait:10,click:k0", 10),
    "dlg-crash":         ("1280x800", ["--iso", "@ISO", "--crash-test", "@CRASH"], "k:down", 20),
    # Find my disc image (the disc image's folder searched as if beside the launcher)
    "find-auto":         ("1280x800", ["--find-root", "@ISODIR"], "", 20),
    "find-list":         ("1280x800", ["--iso", "@ISO", "--find-root", "@ISODIR"], "find", 20),
    "find-list-pad":     ("1280x800", ["--iso", "@ISO", "--find-root", "@ISODIR"], "find,wait:10,p:down", 20),
    "find-deck":         ("1280x800", ["--find-root", "@ISODIR", "--iso", "@MISSING"], "find,wait:10,p:down", 20),
}


def no_activate_startup(env):
    """Windows: the launcher's first window opens without activation (SW_SHOWNOACTIVATE),
    unless XBOX_NO_ACTIVATE=0. Elsewhere: None."""
    if os.name != "nt" or env.get("XBOX_NO_ACTIVATE", "1") in ("", "0"):
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 4                      # SW_SHOWNOACTIVATE
    return si


def fixtures(out):
    """Files the cases point to, made once beside the output folder (shared
    by the runs compared: the crash screen shows its folder's date)."""
    fx = os.path.join(os.path.dirname(out), "_fixtures")
    os.makedirs(fx, exist_ok=True)
    wrong = os.path.join(fx, "not-a-disc.iso")
    if not os.path.exists(wrong):
        with open(wrong, "wb") as f:
            f.write(b"\0" * (1 << 20))
    crash = os.path.join(fx, "crash-2026-10-14_120000")
    os.makedirs(crash, exist_ok=True)

    def rel(tag):
        return [{"tag_name": tag, "name": "OpenTricky " + tag[1:], "draft": False, "prerelease": True,
                 "html_url": "https://github.com/GiZcesi/OpenTricky/releases/tag/" + tag,
                 "body": "<!-- launcher\nversion: %s\ntitle: A test release\ncard: snowdream\n"
                         "fixes: First fixed thing\nfixes: Second fixed thing\nnew: A new thing\n-->\nNotes." % tag[1:].split("-")[0],
                 "assets": []}]
    paths = {}
    for name, tag in (("new", "v9.9.9-unstable"), ("old", "v0.0.1-unstable")):
        p = os.path.join(fx, name + ".json")
        with open(p, "w", encoding="utf-8") as f:
            json.dump(rel(tag), f)
        paths[name] = p
    return {"@MISSING": os.path.join(fx, "moved", "SSX Tricky.iso"), "@WRONG": wrong,
            "@NOCHECK": os.path.join(fx, "no-answer.json"),     # missing: the check fails quietly
            "@NEW": paths["new"], "@OLD": paths["old"], "@CRASH": crash}


def run(args):
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    fx = fixtures(out)
    fx["@ISO"] = args.iso
    fx["@ISODIR"] = os.path.dirname(os.path.abspath(args.iso))
    rundir = os.path.join(out, "_run")
    if os.path.isdir(rundir):
        shutil.rmtree(rundir)
    shutil.copytree(args.launcher, rundir, ignore=shutil.ignore_patterns("CMakeFiles", "*.a", "*.cmake", "rmlui", "sdl3",
                                                                          "freetype", "generated", "Makefile", "CMakeCache.txt",
                                                                          "settings.ini", "*.log", "update-cache.txt"))
    exe = os.path.join(rundir, "OpenTricky.exe")
    env = dict(os.environ)
    env.update({"SDL_XINPUT_ENABLED": "0", "SDL_JOYSTICK_XINPUT": "0", "SDL_JOYSTICK_RAWINPUT": "0", "SDL_JOYSTICK_WGI": "0",
                "SDL_JOYSTICK_HIDAPI": "0", "SDL_JOYSTICK_DIRECTINPUT": "0", "SDL_JOYSTICK_GAMEINPUT": "0",
                "SDL_WINDOW_ACTIVATE_WHEN_SHOWN": "0", "XBOX_MUTE": "1"})
    # Never the focus of the window in front: the launcher (and a game it
    # starts) is shown without activation, behind it. XBOX_NO_ACTIVATE=0 to get it back.
    env.setdefault("XBOX_NO_ACTIVATE", "1")
    for k in ("XBOX_TEST_DECK_ROOT", "SteamDeck", "SteamGamepadUI", "GAMESCOPE_WAYLAND_DISPLAY", "OT_LAUNCHER_FULLSCREEN"):
        env.pop(k, None)
    only = set(args.only.split(",")) if args.only else None
    failed = 0
    for name, (size, extra, script, wait) in CASES.items():
        if only and name not in only:
            continue
        for f in ("settings.ini", "settings.ini.new", "update-cache.txt"):
            p = os.path.join(rundir, f)
            if os.path.exists(p):
                os.remove(p)
        a = []
        for x in extra:
            if x == "@PS2":       # the PS2 button style: a settings.ini with it
                with open(os.path.join(rundir, "settings.ini"), "w") as f:
                    f.write("[Meta]\nVersion=1\n[Controls]\nButtonStyle=ps2\n")
                continue
            a.append(fx.get(x, x))
        if "--updates-json" not in a:       # never the network: the pill stays "v<version>"
            a += ["--updates-json", fx["@NOCHECK"]]
        shot = os.path.join(out, name + ".png")
        # the launcher draws ~200 frames a second under a script: the waits
        # outlast the 0.12 s colour transitions
        steps = ["wait:40"] + ([script] if script else []) + ["wait:%d" % (wait + 40), "shot:" + shot]
        cmd = [exe, "--track", TRACK, "--size", size, "--no-skip", "--log", os.path.join(out, name + ".log"),
               "--script", ",".join(steps)] + a
        if os.path.exists(shot):
            os.remove(shot)
        t0 = time.time()
        try:
            rc = subprocess.run(cmd, env=env, cwd=rundir, timeout=90, startupinfo=no_activate_startup(env)).returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
        ok = os.path.exists(shot)
        failed += not ok
        print("%-20s %s rc=%s %.1fs" % (name, "ok" if ok else "NO PICTURE", rc, time.time() - t0), flush=True)
    return 1 if failed else 0


def compare(args):
    from PIL import Image, ImageChops, ImageDraw
    a_dir, b_dir = args.a, args.b
    names = sorted(n[:-4] for n in os.listdir(a_dir) if n.endswith(".png") and os.path.exists(os.path.join(b_dir, n)))
    out = args.out or os.path.join(b_dir, "_compare")
    if args.sheet:
        os.makedirs(out, exist_ok=True)
    rows, bad = [], 0
    for n in names:
        a = Image.open(os.path.join(a_dir, n + ".png")).convert("RGB")
        b = Image.open(os.path.join(b_dir, n + ".png")).convert("RGB")
        if a.size != b.size:
            diff = -1
        else:
            d = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v else 0)
            diff = sum(d.histogram()[1:])
        bad += diff != 0
        rows.append((n, diff))
        print("%-20s %s" % (n, "identical" if diff == 0 else "size differs" if diff < 0 else "%d pixels differ" % diff))
        if args.sheet:
            gap, head = 16, 34
            sheet = Image.new("RGB", (a.width + b.width + gap, max(a.height, b.height) + head), (24, 26, 32))
            sheet.paste(a, (0, head))
            sheet.paste(b, (a.width + gap, head))
            dr = ImageDraw.Draw(sheet)
            dr.text((8, 10), "%s  -  %s" % (n, args.label_a), fill=(230, 230, 230))
            dr.text((a.width + gap + 8, 10), "%s  -  %s" % (n, args.label_b), fill=(230, 230, 230))
            sheet.save(os.path.join(out, n + ".png"))
    print("%d case(s), %d differ" % (len(rows), bad))
    if args.sheet:
        with open(os.path.join(out, "index.html"), "w", encoding="utf-8") as f:
            f.write("<!doctype html><meta charset=utf-8><title>Launcher: %s / %s</title>"
                    "<style>body{background:#111;color:#ddd;font:14px sans-serif;margin:16px}img{max-width:100%%;display:block;margin:6px 0 28px}"
                    "</style><h1>%s (left) / %s (right)</h1>\n" % (args.label_a, args.label_b, args.label_a, args.label_b))
            for n, diff in rows:
                f.write("<h2>%s &mdash; %s</h2><img src='%s.png' loading=lazy>\n" %
                        (n, "identical" if diff == 0 else "%d pixels differ" % diff if diff > 0 else "size differs", n))
    return 1 if bad else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("launcher")
    r.add_argument("out")
    r.add_argument("--iso", required=True)
    r.add_argument("--only")
    c = sub.add_parser("compare")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--out")
    c.add_argument("--sheet", action="store_true")
    c.add_argument("--label-a", default="before")
    c.add_argument("--label-b", default="after")
    sub.add_parser("list")
    args = p.parse_args()
    if args.cmd == "list":
        for n, (size, extra, script, wait) in CASES.items():
            print("%-20s %-9s %s | %s" % (n, size, " ".join(extra), script))
        return 0
    return run(args) if args.cmd == "run" else compare(args)


if __name__ == "__main__":
    sys.exit(main())
