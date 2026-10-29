#!/usr/bin/env python3
"""Fork pass: mark the block of 0xC44E0 that draws the Tricky gauge.

HUD_DrawWorldSpaceMarkers sub_000C44E0 draws, per player's view, the Tricky
gauge in one block of its code, from 0x000C4648 to 0x000C4F6E, each part under
its bit of the HUD's flags (HUD + 0xFC): 0x1000 the rings and the rail, 0x2000
the head (the ball), 0x8000 the logo's six letters, 0x4000 the marks under the
rail, 0x80 the S under the rail's foot (picture 0x18, while the gauge state's
byte +0xA1 is set). The block is entered at 0x000C4648 only (from the code above
it) and left at 0x000C4F6E only (its last part falls through, every skip jumps
there).

The pass calls hud_anchor_gauge(1) at the block's entry and hud_anchor_gauge(0)
at its exit (port/src/hud_anchor.c): the HUD records appended in between are the
gauge, one element of the race HUD, as the game draws it.

    fix_hudgauge.py GEN_DIR [--dry-run]
Idempotent. Stops if the code around the two labels changed shape.
"""
import glob, io, os, re, sys

TAG = "/* fork_hudgauge */"
DECL = "extern void hud_anchor_gauge(int begin); " + TAG + "\n"
FUNC = "void sub_000C44E0(void)\n{\n"
SITES = (("loc_000C4648: ;\n", "    hud_anchor_gauge(1); " + TAG + " /* the Tricky gauge's block starts */\n"),
         ("loc_000C4F6E: ;\n", "    hud_anchor_gauge(0); " + TAG + " /* ... and ends */\n"))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    gen, dry = args[0], "--dry-run" in sys.argv
    hits = []
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        if "void sub_000C44E0(void)" in src:
            hits.append((path, src))
    if len(hits) != 1:
        sys.exit("fix_hudgauge: expected sub_000C44E0 in exactly 1 file, found %d" % len(hits))
    path, src = hits[0]
    nl = "\r\n" if "\r\n" in src else "\n"
    func, decl = FUNC.replace("\n", nl), DECL.replace("\n", nl)
    start = src.index(func)
    end = src.index(nl + "}" + nl, start)
    body = src[start:end]
    if TAG in body:
        print("%s: already patched" % os.path.basename(path))
        return
    lab_in, lab_out = SITES[0][0].replace("\n", nl), SITES[1][0].replace("\n", nl)
    if body.count(lab_in) != 1 or body.count(lab_out) != 1:
        sys.exit("fix_hudgauge: the two labels are not in sub_000C44E0 once each: stop rather than guess")
    i_in, i_out = body.index(lab_in), body.index(lab_out)
    block, rest = body[i_in:i_out], body[:i_in] + body[i_out:]
    # entered at 0xC4648 only, left at 0xC4F6E only
    inside = set(re.findall(r"loc_(000C4[0-9A-F]{3}): ;", block))
    jumps_in = [t for t in re.findall(r"goto loc_([0-9A-F]{8});", rest) if t in inside and t != "000C4648"]
    jumps_out = [t for t in re.findall(r"goto loc_([0-9A-F]{8});", block) if t not in inside and t != "000C4F6E"]
    if jumps_in or jumps_out or "goto loc_000C4F6E;" in rest:
        sys.exit("fix_hudgauge: the gauge block is entered or left elsewhere (%s / %s): stop rather than guess"
                 % (jumps_in, jumps_out))
    new_body = body
    for lab, call in SITES:
        lab, call = lab.replace("\n", nl), call.replace("\n", nl)
        new_body = new_body.replace(lab, lab + call, 1)
    new = src[:start] + new_body + src[end:]
    first = new.index(nl + "void sub_")
    new = new[:first + len(nl)] + decl + new[first + len(nl):]
    print("%s: sub_000C44E0 gauge block 0xC4648-0xC4F6E marked (%d labels inside)" % (os.path.basename(path), len(inside)))
    if not dry:
        io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    print("fix_hudgauge: 2 sites%s" % (" (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
