"""PlayStation-style D-pad drawings for the button icons (nv2a_btnicons.c).

Drawn for this project from plain geometric shapes (no picture copied from
anywhere): four separate dark arms pointing at the centre, each with a dark
outline (it reads on snow) and a thin lighter edge; the active arm is white
with a dark arrow. 128 x 128 RGBA, like the JulioCacko drawings beside them.

Writes, next to this script:
  ps_dpad_up.png, ps_dpad_down.png, ps_dpad_left.png, ps_dpad_right.png
  ps_dpad_full.png   (no active arm: the Duke's big D-pad disc)

Needs: pip install skia-python numpy pillow. Run: python draw_ps_dpad.py
The PNGs are committed, so a build never runs this.
"""
import os

import skia
from PIL import Image

OUT = os.path.dirname(os.path.abspath(__file__))
N = 128
C = 64.0

ARM, EDGE, OUTLINE, ACTIVE, ARROW = 0x2A2A2E, 0x6A6A72, 0x0C0C0E, 0xFFFFFF, 0x2C2C30
WIDTH, INNER, OUTER, OUTLINE_W = 46.0, 6.0, 62.0, 4.0


def col(h):
    return skia.Color((h >> 16) & 255, (h >> 8) & 255, h & 255, 255)


def paint(c, stroke=0.0):
    p = skia.Paint(AntiAlias=True, Color=c)
    if stroke:
        p.setStyle(skia.Paint.kStroke_Style)
        p.setStrokeWidth(stroke)
        p.setStrokeJoin(skia.Paint.kRound_Join)
    return p


def rotated(p, deg):
    m = skia.Matrix()
    m.setRotate(deg, C, C)
    p.transform(m)
    return p


def arm_path(deg):
    """The up arm (outer end rounded, inner end pointing at the centre), rotated."""
    w, r0, r1 = WIDTH, INNER, OUTER
    p = skia.Path()
    p.moveTo(C - w / 2, C - r1 + 8)
    p.quadTo(C - w / 2, C - r1, C - w / 2 + 8, C - r1)
    p.lineTo(C + w / 2 - 8, C - r1)
    p.quadTo(C + w / 2, C - r1, C + w / 2, C - r1 + 8)
    p.lineTo(C + w / 2, C - r0 - w / 2 + 4)
    p.lineTo(C, C - r0)
    p.lineTo(C - w / 2, C - r0 - w / 2 + 4)
    p.close()
    return rotated(p, deg)


def dpad(active):
    s = skia.Surface(N, N)
    cv = s.getCanvas()
    cv.clear(skia.ColorTRANSPARENT)
    arms = {'up': 0, 'right': 90, 'down': 180, 'left': 270}
    for k, deg in arms.items():
        p = arm_path(deg)
        cv.drawPath(p, paint(col(OUTLINE), OUTLINE_W + 4))
        cv.drawPath(p, paint(col(ACTIVE if k == active else ARM)))
        if k != active:
            cv.drawPath(p, paint(col(EDGE), 2.0))
    if active:
        a = skia.Path()
        a.moveTo(C - 11, C - 34)
        a.lineTo(C + 11, C - 34)
        a.lineTo(C, C - 48)
        a.close()
        cv.drawPath(rotated(a, arms[active]), paint(col(ARROW)))
    return s


def save(s, name):
    im = s.makeImageSnapshot().toarray(colorType=skia.kRGBA_8888_ColorType, alphaType=skia.kUnpremul_AlphaType)
    path = os.path.join(OUT, name)
    Image.fromarray(im).save(path, optimize=True)
    print(path)


if __name__ == '__main__':
    for k in ('up', 'down', 'left', 'right'):
        save(dpad(k), 'ps_dpad_%s.png' % k)
    save(dpad(None), 'ps_dpad_full.png')
