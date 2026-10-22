#!/usr/bin/env python3
"""Write port/assets/resources.txt: the names of the Windows build's executable resources (port/src/ssx_recomp.rc.in) and
the files they come from, relative to port/assets (read at run time by xboxrecomp/src/platform/ot_assets.c).
Run:  python3 macos/launcher/make_resources.py"""
import os, re, glob
root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
assets = os.path.join(root, "port", "assets")
sub = {"@SSX_BTNICONS@": "btnicons/juliocacko", "@SSX_BTNICONS_DRAWN@": "btnicons/drawn", "@SSX_BTNICONS_PADS@": "btnicons/pads",
       "@SSX_HDTEX_INDEX@": "hdtex_index.txt"}
lines = []
for ln in open(os.path.join(root, "port", "src", "ssx_recomp.rc.in"), encoding="utf-8", errors="replace"):
    m = re.match(r'^(\w+)\s+RCDATA\s+"([^"]+)"', ln)
    if not m: continue
    name, path = m.groups()
    for k, v in sub.items(): path = path.replace(k, v)
    if "@" in path: continue
    if os.path.isfile(os.path.join(assets, path)): lines.append("%s %s" % (name, path))
    else: print("missing:", name, path)
for f in sorted(glob.glob(os.path.join(assets, "btnicons", "ps2", "ps2_*.png"))):
    lines.append("%s %s" % (os.path.splitext(os.path.basename(f))[0].upper(), os.path.relpath(f, assets)))
open(os.path.join(assets, "resources.txt"), "w").write("\n".join(lines) + "\n")
print("wrote %d resources" % len(lines))
