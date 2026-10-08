#!/usr/bin/env python3
"""
make_icon.py -- OpenTricky.ico, the launcher's own icon (nothing of the game).

The picture is the launcher's "OT" badge, the one shown in the side bar when
no disc image is set (ot_badge_ot in port/launcher/ot_theme.c, drawn in a
46 dp box by ui/launcher.rcss ".side .otb"): a rounded square filled with a
diagonal blue gradient (0x3AA0FF top left -> 0x1450B8 bottom right, corner
radius 13/46), with "OT" in Bullet SmallCaps, 20/46 em, white, centred, the
baseline at 26/46 (the RmlUi line box of the 46 dp badge, measured on a
capture of the launcher). Each size is drawn 8x oversampled and reduced with
a box filter: straight alpha, clean edges.

    python make_icon.py [--out OpenTricky.ico] [--png <folder>]

Needs Pillow. The .ico is committed: this script only has to run again when
the badge changes.
"""
import argparse
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "ui", "fonts", "bullet-smallcaps.otf")
SIZES = [256, 128, 64, 48, 32, 16]
OVERSAMPLE = 8
C0 = (0x3A, 0xA0, 0xFF)   # top left (ot_theme.c: 0xFF3AA0FF)
C1 = (0x14, 0x50, 0xB8)   # bottom right (0xFF1450B8)


def gradient(n):
    """The badge fill: mix(C0, C1, (x + y) / (2 n - 2)), as ot_badge_ot."""
    im = Image.new("RGBA", (n, n))
    px = im.load()
    d = 2.0 * n - 2
    for y in range(n):
        for x in range(n):
            t = (x + y) / d
            px[x, y] = (int(C0[0] + (C1[0] - C0[0]) * t + 0.5),
                        int(C0[1] + (C1[1] - C0[1]) * t + 0.5),
                        int(C0[2] + (C1[2] - C0[2]) * t + 0.5), 255)
    return im


def badge(size):
    n = size * OVERSAMPLE
    im = gradient(n)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, n - 1, n - 1), radius=n * 13.0 / 46.0, fill=255)
    im.putalpha(mask)
    font = ImageFont.truetype(FONT, int(round(n * 20.0 / 46.0)))
    ImageDraw.Draw(im).text((n / 2.0, n * 26.0 / 46.0), "OT", font=font, fill=(255, 255, 255, 255), anchor="ms")
    return im.resize((size, size), Image.BOX)


def main():
    ap = argparse.ArgumentParser(description="OpenTricky.ico from the launcher's OT badge")
    ap.add_argument("--out", default=os.path.join(HERE, "OpenTricky.ico"))
    ap.add_argument("--png", help="also write OpenTricky-<size>.png files in this folder")
    ap.add_argument("--size", type=int, help="write a single PNG of this size to --png and stop (checks)")
    a = ap.parse_args()
    if a.size:
        os.makedirs(a.png, exist_ok=True)
        badge(a.size).save(os.path.join(a.png, "OpenTricky-%d.png" % a.size))
        return
    frames = [badge(s) for s in SIZES]
    if a.png:
        os.makedirs(a.png, exist_ok=True)
        for s, im in zip(SIZES, frames):
            im.save(os.path.join(a.png, "OpenTricky-%d.png" % s))
    frames[0].save(a.out, format="ICO", sizes=[(s, s) for s in SIZES], append_images=frames[1:])
    print("%s: %d bytes, sizes %s" % (a.out, os.path.getsize(a.out), SIZES))


if __name__ == "__main__":
    main()
