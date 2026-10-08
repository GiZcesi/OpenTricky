#!/usr/bin/env python3
"""Configure Controller (options menu) for the modern button styles: the
picture of the pad with its legend lines, and the labels' layout.

The screen shows shape 61 of the front end's atlas (fe_1, "xbox" entry, its
texels (0,0)-(218,171)) at (245,100), 0.9 x its width by its height (196.2 x
171 units); the labels are separate texts placed by the title's tables
(common 0x1BA0C8, Default 0x1BA1B8, Pro 0x1BA1E8: {x, y, flags, string},
from the pad's corner, flags 8 = right-aligned). The legend lines are in the
picture: they run from the picture's edge (next to their label) to their
button. Layout units below are the screen's, from the pad's corner.

Output, next to this script, 4x the atlas texels (872 x 684), the picture
squeezed back by 1 / 0.9 in width: ps_config.png (DualShock 4, PlayStation
style), xb_config.png (Xbox One, modern style). The labels' tables, which
match these lines, are LABELS below; --c prints them for port/src/ps2legend.c
(k_cfg_layout), the PS2 style's too (its picture: port/assets/btnicons/ps2/ps2_config.png).

    python make_config.py [--c]  (Pillow; the pads from make_pads.py)
"""
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
X4 = 4
TEX_W, TEX_H = 218, 171          # shape 61's texels
SQUEEZE = 0.9                    # screen width / texel width (196.2 / 218)
LINE = (205, 205, 212, 255)
LINE_W = 2.0                     # texels

# Per style: the pad (make_pads.py picture, its width in units, its corner),
# and each legend line, from the picture's edge to its button (units).
STYLES = {
    "ps": dict(pad="ps_lesson_pad.png", width=196.2, at=(0, 15), lines=[
        [(33, 12), (37.5, 16.5)],                              # L2 (label above the pad)
        [(158, 12), (157.5, 16.5)],                            # R2
        [(88, 148), (88, 74), (62, 50.5)],                     # Share (label under the pad)
        [(107, 148), (107, 74), (134.5, 50.5)],                # Options
        [(196, 45.5), (170, 45.5), (166.2, 47.5)],             # triangle
        [(196, 84), (172, 84), (166, 79.5)],                   # cross
        [(196, 107), (150, 107), (150, 75), (147.5, 69.5)],    # square
        [(196, 125.5), (129.2, 125.5), (129.2, 108.5)],        # right stick
        [(0, 40), (20, 40), (31, 50)],                         # D-pad up (two labels)
        [(0, 76.5), (12, 76.5), (21.5, 65)],                   # D-pad left (two labels)
        [(0, 110), (36, 110), (51.5, 98.5)],                   # left stick (crouch/brake ...)
        [(0, 144.4), (66.9, 144.4), (66.9, 108.5)],            # left stick (turn ...)
    ]),
    "xb": dict(pad="xb_lesson_pad.png", width=196.2, at=None, lines=[
        [(28, 8), (35.5, 12.5)],                               # LT
        [(0, 36.5), (22, 36.5), (36.5, 44)],                   # left stick (crouch/brake ...)
        [(0, 79.5), (24, 79.5), (37, 62.5)],                   # left stick (turn ...)
        [(0, 117.5), (46, 117.5), (62, 92.5)],                 # D-pad left
        [(0, 154), (58, 154), (71, 100.5)],                    # D-pad down
        [(85, 163), (95, 152), (95, 63), (87.5, 56)],          # View
        [(112, 163), (101, 152), (101, 63), (109, 56.5)],      # Menu
        [(168, 8), (160, 12.5)],                               # RT
        [(196, 28.5), (172, 28.5), (165, 24)],                 # RB
        [(196, 44), (166, 44), (156.5, 40.5)],                 # Y
        [(196, 66.5), (176, 66.5), (169.5, 55.5)],             # B
        [(196, 85.5), (163, 85.5), (154, 71)],                 # A
        [(196, 106.5), (141, 106.5), (141, 75), (137, 59.5)],  # X
        [(196, 136.5), (122.6, 136.5), (122.6, 106)],          # right stick
    ]),
}


# The labels, per style: the 21 rows of the three tables (common 15, Default
# 3, Pro 3) as (text, x, y, right-aligned, string index). "jump",
# "crouch/brake" and "turn/late spin" are the strings 0xC94, 0xC8A, 0xC8C
# rewritten (ps2legend.c). Rows the style does not use are hidden (string 0xE02 is
# empty; far off the screen). The Default preset's stick labels sit where the
# Pro preset's first label of each pair sits: common to both presets, so that
# the Pro preset's three own rows suffice. The three styles show the PS2
# game's words (the same control layout).
HIDE = ("", -2000.0, 0.0, 0, 0xE02)
Q = 196.2 / 256 / 0.8            # the PS2 game's units -> the PS2 picture's place here


def ps2(x, y):
    return round(x * Q, 2), round(y * Q + 15, 2)


LABELS = {
    "ps2": [("pause",) + ps2(118, 125) + (0, 0xC90), ("reset", round(75 * Q + 23.75, 2), ps2(0, 145)[1], 8, 0xC8F),
            ("grab board",) + ps2(40, -20) + (8, 0xC91), ("grab board",) + ps2(160, -20) + (0, 0xC91),
            ("jump",) + ps2(210, 64) + (0, 0xC94), ("tweak/boost",) + ps2(210, 88) + (0, 0xC95),
            ("shove",) + ps2(210, 107) + (0, 0xC8E), ("crouch/brake",) + ps2(-10, 10) + (8, 0xC8A),
            ("prewind flips",) + ps2(-10, 26) + (8, 0xC8B), ("turn/late spin",) + ps2(-10, 48) + (8, 0xC8C),
            ("prewind spins",) + ps2(-10, 64) + (8, 0xC8D), ("crouch/brake",) + ps2(-45, 85) + (8, 0xDFD),
            ("turn",) + ps2(-36, 120) + (8, 0xDFF), HIDE, HIDE,
            ("no function",) + ps2(210, 24) + (0, 0xC92), HIDE, HIDE,
            ("combat cam",) + ps2(210, 24) + (0, 0xE09), ("prewind flips",) + ps2(-45, 101) + (8, 0xDFE),
            ("prewind spins",) + ps2(-36, 136) + (8, 0xE00)],
    "ps": [("pause", 106.7, 150, 0, 0xC90), ("reset", 89.75, 150, 8, 0xC8F),
           ("grab board", 38.3, -4.2, 8, 0xC91), ("grab board", 153.3, -4.2, 0, 0xC91),
           ("jump", 201.2, 76.3, 0, 0xC94), ("tweak/boost", 201.2, 99.3, 0, 0xC95), ("shove", 201.2, 117.5, 0, 0xC8E),
           ("crouch/brake", -9.6, 24.6, 8, 0xC8A), ("prewind flips", -9.6, 39.9, 8, 0xC8B),
           ("turn/late spin", -9.6, 61, 8, 0xC8C), ("prewind spins", -9.6, 76.3, 8, 0xC8D),
           ("crouch/brake", -9.6, 94.4, 8, 0xDFD), ("turn", -9.6, 128.7, 8, 0xDFF), HIDE, HIDE,
           ("no function", 201.2, 38, 0, 0xC92), HIDE, HIDE,
           ("combat cam", 201.2, 38, 0, 0xE09), ("prewind flips", -9.6, 109.7, 8, 0xDFE), ("prewind spins", -9.6, 144, 8, 0xE00)],
    "xb": [("pause", 105, 165, 0, 0xC90), ("reset", 95, 165, 8, 0xC8F),
           ("grab board", 30, -8, 8, 0xC91), ("grab board", 166, -8, 0, 0xC91), ("grab board", 200, 21, 0, 0xE08),
           ("jump", 200, 78, 0, 0xC94), ("tweak/boost", 200, 99, 0, 0xC95), ("no function", 200, 59, 0, 0xC93),
           ("shove", 200, 129, 0, 0xC8E), ("crouch/brake", -5, 138, 8, 0xC8A), ("prewind flips", -5, 155, 8, 0xC8B),
           ("turn/late spin", -5, 102, 8, 0xC8C), ("prewind spins", -5, 118, 8, 0xC8D),
           ("crouch/brake", -5, 21, 8, 0xDFD), ("turn", -5, 64, 8, 0xDFF),
           ("no function", 200, 41, 0, 0xC92), HIDE, HIDE,
           ("combat cam", 200, 41, 0, 0xE09), ("prewind flips", -5, 37, 8, 0xDFE), ("prewind spins", -5, 80, 8, 0xE00)],
}
for _k, _v in LABELS.items():
    assert len(_v) == 21, _k


def labels(style, preset):
    """the visible labels of a preset: common rows + that preset's rows"""
    rows = LABELS[style][:15] + (LABELS[style][15:18] if preset == "Default" else LABELS[style][18:21])
    return [r for r in rows if r[0]]


def c_tables():
    for st in ("ps2", "ps", "xb"):
        print("    { /* %s */" % st)
        for i, (t, x, y, f, idx) in enumerate(LABELS[st]):
            print("        { %8.2ff, %8.2ff, %u, 0x%03X },%s" % (x, y, f, idx, "   /* %s */" % t if t else "   /* (hidden) */"))
        print("    },")


def make(prefix, cfg):
    pad = Image.open(os.path.join(HERE, cfg["pad"])).convert("RGBA")
    pw, ph = pad.width / X4, pad.height / X4               # pad texels = units at scale 1
    s = cfg["width"] / pw
    at = cfg["at"] or (0, (TEX_H - ph * s) / 2)
    W, H = TEX_W * X4, TEX_H * X4

    def px(u, v):                                          # units -> picture pixels
        return u / SQUEEZE * X4, v * X4
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    x0, y0 = px(*at)
    x1, y1 = px(at[0] + pw * s, at[1] + ph * s)
    out.alpha_composite(pad.resize((round(x1 - x0), round(y1 - y0)), Image.LANCZOS), (round(x0), round(y0)))
    # legend lines, drawn 4x larger again and reduced: smooth edges
    big = Image.new("RGBA", (W * 4, H * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    for pts in cfg["lines"]:
        d.line([(x * 4, y * 4) for x, y in (px(u, v) for u, v in pts)], fill=LINE,
               width=round(LINE_W * X4 * 4), joint="curve")
    out.alpha_composite(big.resize((W, H), Image.LANCZOS))
    out.save(os.path.join(HERE, "%s_config.png" % prefix), optimize=True)
    print("%s_config.png %dx%d, pad at %s units, scale %.4f" % (prefix, W, H, at, s))


if __name__ == "__main__":
    import sys
    if "--c" in sys.argv:
        c_tables()
    else:
        for p, c in STYLES.items():
            make(p, c)
