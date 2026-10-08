#!/usr/bin/env python3
"""The trick tutorial's pad and gold markers for the modern button styles.

Sources, from "8th Gen Console Vector Gamepad Collection" by TheHoodieGuy02
(https://opengameart.org/node/97182), CC0 (src/Readme.txt):
src/dualshock4.svg (PlayStation style) and src/xboxone.svg (Xbox style).
Each pad keeps its own shape and proportions (no logo in either drawing),
restyled black: anthracite body, light outline (seen on the game's night
skies). Each gold marker is drawn here from the outline of its button in the
same drawing: a gold ring around the button over a dark see-through fill,
the look of the game's own lesson markers. A shoulder button drawn behind
another (L2 behind L1, LT behind LB) is ringed only where it shows above the
front one's marker, so that the two never overlap.

Output, 4x (one texel of the game's 256 x 256 atlas = 4 x 4 pixels), next to
this script, per pad (prefix ps_ or xb_): <p>lesson_pad.png and
<p>lesson_{ring,l1,l2,dpad_v,dpad_h}.png (l1 / l2 = the left front / back
shoulder; the right-hand shoulders and the other D-pad arms are the same
pictures mirrored by the shapes' UVs), and <p>lesson.txt: the pictures'
places in the composed atlas and each marker's corner on the pad (texels),
the table nv2a_btnicons.c carries.

    python make_pads.py          (skia-python, numpy, Pillow)

The build only reads the PNG files.
"""
import os
import re
import xml.etree.ElementTree as ET

import numpy as np
import skia
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
NS = "{http://www.w3.org/2000/svg}"
X4 = 4
RING_IN, RING_OUT = 0.5, 2.5     # texels around the button: inside edge, outside edge of the gold ring
GOLD = (214, 160, 32, 255)
GOLD_EDGE = (120, 84, 10, 255)
FILL = (40, 32, 20, 120)         # see-through dark fill inside the ring
ATLAS_ROWS = 180                 # the Duke's rows of the trick HUD atlas: pad and markers fit there

# Per pad: the drawing, its restyle (CSS), its width in texels (= the game's
# units: the shape is drawn at its size), and where its buttons are: an
# element path (ids, then the index of the drawn child) per marker.
PADS = {
    "ps": dict(
        svg="dualshock4.svg", width=240,
        restyle={".fil1 {fill:#4D4D4D}": ".fil1 {fill:#2E2F33}",
                 ".fil2 {fill:#333333}": ".fil2 {fill:#222326}",
                 ".fil4 {fill:#1A1A1A}": ".fil4 {fill:#151618}",
                 ".str0 {stroke:black;": ".str0 {stroke:#A3A6AE;"},
        # (front, back) shoulders: the two drawn parts of each shoulder group, the upper one behind
        shoulders=(["Left_x0020_Shoulder", 0], ["Left_x0020_Shoulder", 1],
                   ["Right_x0020_Shoulder", 0], ["Right_x0020_Shoulder", 1]),
        faces={"cross": ["Cross", 0], "circle": ["Circle", 0], "square": ["Square", 0], "triangle": ["Triangle", 0]},
        arms={"up": ["D-pad_x0020_Up", 0], "down": ["D-pad_x0020_Down", 0],
              "right": ["D-pad_x0020_Right", 0], "left": ["D-pad_x0020_Left", 0]},
    ),
    "xb": dict(
        svg="xboxone.svg", width=206,
        restyle={".fil2 {fill:#4D4D4D}": ".fil2 {fill:#2E2F33}",
                 ".fil1 {fill:#1A1A1A}": ".fil1 {fill:#151618}",
                 ".str1 {stroke:black;": ".str1 {stroke:#A3A6AE;"},
        shoulders=(["Left_x0020_Shoulder"], ["Left_x0020_Trigger"],
                   ["Right_x0020_Shoulder"], ["Right_x0020_Trigger"]),
        faces={"cross": ["A", 0], "circle": ["B", 0], "square": ["X", 0], "triangle": ["Y", 0]},
        arms="D-pad",            # one cross: its arms are cut out of it
    ),
}
# marker names in the order of the title's table 0x1AA6C8 (nv2a_btnicons.c mark[])
ROWS = ["L2", "R2", "cross", "circle", "square", "triangle", "L1", "R1", "left", "right", "down", "up"]

ET.register_namespace("", "http://www.w3.org/2000/svg")
ET.register_namespace("xlink", "http://www.w3.org/1999/xlink")


def load_text(cfg):
    text = open(os.path.join(HERE, "src", cfg["svg"]), encoding="utf-8").read()
    for a, b in cfg["restyle"].items():
        assert a in text, a
        text = text.replace(a, b)
    return text


def tree(text):
    root = ET.fromstring(text)
    css = {n: [tuple(x.strip() for x in d.split(":", 1)) for d in b.split(";") if ":" in d]
           for n, b in re.findall(r"\.(\w+)\s*\{([^}]*)\}", text)}
    for e in root.iter():                        # skia's SVGDOM ignores <style>
        for c in (e.get("class") or "").split():
            for k, v in css.get(c, []):
                e.set(k, v)
        e.attrib.pop("class", None)
    return root


def find(root, idd):
    for e in root.iter():
        if e.get("id") == idd:
            return e
    raise KeyError(idd)


def only(root, keep):
    """Drop every drawn element but those in keep (their ancestors stay)."""
    def prune(e):
        hit = False
        for c in list(e):
            tag = c.tag.replace(NS, "")
            if tag in ("defs", "style", "metadata"):
                continue
            if c in keep:
                hit = True
            elif prune(c):
                hit = True
            else:
                e.remove(c)
        return hit
    prune(root)
    return root


def render(root, m, w, h):
    s = skia.Surface(w, h)
    cv = s.getCanvas()
    cv.clear(skia.ColorTRANSPARENT)
    data = ET.tostring(root)                     # kept alive while skia reads it
    dom = skia.SVGDOM.MakeFromStream(skia.MemoryStream(data, True))
    assert dom is not None, "svg not read"
    dom.setContainerSize(skia.Size(2048, 2048))
    cv.save(); cv.concat(m); dom.render(cv); cv.restore()
    return s.makeImageSnapshot().toarray(colorType=skia.kRGBA_8888_ColorType, alphaType=skia.kUnpremul_AlphaType)


def dilate(mask, r):
    """Disc dilation, r in pixels."""
    from math import ceil
    n = int(ceil(r))
    out = np.zeros_like(mask)
    ys, xs = np.mgrid[-n:n + 1, -n:n + 1]
    for dy, dx in zip(ys.ravel(), xs.ravel()):
        if dx * dx + dy * dy <= r * r:
            out |= np.roll(np.roll(mask, dy, 0), dx, 1)
    return out


def make(prefix, cfg):
    text = load_text(cfg)
    full = render(tree(text), skia.Matrix(), 2048, 2048)
    ys, xs = np.nonzero(full[..., 3] > 8)
    bx0, by0, bx1, by1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
    pad_w = cfg["width"]
    k = pad_w * X4 / (bx1 - bx0)                 # svg -> pad pixels, one scale (the pad's own shape)
    pad_h = int(np.ceil((by1 - by0) * k / X4))
    m = skia.Matrix(); m.setAll(k, 0, -bx0 * k, 0, k, -by0 * k, 0, 0, 1)
    W, H = pad_w * X4, pad_h * X4
    pad = render(tree(text), m, W, H)
    Image.fromarray(pad, "RGBA").save(os.path.join(HERE, "%s_lesson_pad.png" % prefix), optimize=True)

    mg = 4 * X4                                  # room around a button in the canvas
    big = skia.Matrix(); big.setAll(k, 0, -bx0 * k + mg, 0, k, -by0 * k + mg, 0, 0, 1)

    def mask(path):
        """the button's own shape (the element, or its drawn child), pad pixels + mg."""
        r = tree(text)
        e = r
        for step in path:
            e = find(e, step) if isinstance(step, str) else [c for c in e if c.tag.replace(NS, "") != "title"][step]
        return render(only(r, {e}), big, W + 2 * mg, H + 2 * mg)[..., 3] > 96

    def marker(msk, clip=None):
        inner = dilate(msk, RING_IN * X4)
        outer = dilate(msk, RING_OUT * X4)
        edge = outer & ~dilate(msk, (RING_OUT - 0.5) * X4)
        if clip is not None:                     # kept off another marker's place
            inner &= ~clip; outer &= ~clip; edge &= ~clip
        ys_, xs_ = np.nonzero(outer)
        x0, y0 = (xs_.min() // X4) * X4, (ys_.min() // X4) * X4      # whole texels
        x1, y1 = -(-(xs_.max() + 1) // X4) * X4, -(-(ys_.max() + 1) // X4) * X4
        img = np.zeros(outer.shape + (4,), np.uint8)
        img[inner] = FILL
        img[outer & ~inner] = GOLD
        img[edge] = GOLD_EDGE
        img = img[y0:y1, x0:x1]
        return img, ((x0 - mg) / X4, (y0 - mg) / X4)

    def back_marker(back, front):
        """a back shoulder's marker: around the part of it seen above the front
        one's marker; when too little of it shows there (the Xbox triggers),
        around the part seen above the front button, kept off the front
        marker's place"""
        vis = back & ~dilate(front, (2 * RING_OUT + 0.5) * X4)
        if vis.sum() >= (4 * X4) ** 2:
            return marker(vis)
        return marker(back & ~front, clip=dilate(front, RING_OUT * X4))

    def front_back(a, b):
        """(front, back): the upper of the two is behind"""
        return (a, b) if np.nonzero(a)[0].min() > np.nonzero(b)[0].min() else (b, a)

    pics, pos = {}, {}
    lf, lb = front_back(mask(cfg["shoulders"][0]), mask(cfg["shoulders"][1]))
    rf, rb = front_back(mask(cfg["shoulders"][2]), mask(cfg["shoulders"][3]))
    pics["l1"], pos["L1"] = marker(lf)
    pics["l2"], pos["L2"] = back_marker(lb, lf)
    pos["R1"] = marker(rf)[1]
    pos["R2"] = back_marker(rb, rf)[1]
    for j, (n, path) in enumerate(cfg["faces"].items()):
        img, pos[n] = marker(mask(path))
        if j == 0:
            pics["ring"] = img
    if isinstance(cfg["arms"], str):             # one cross: cut its arms off its centre square
        cross = mask([cfg["arms"]])
        cy, cx = np.nonzero(cross)
        y0, y1, x0, x1 = cy.min(), cy.max(), cx.min(), cx.max()
        top = cross[y0:y0 + (y1 - y0) // 5]
        ax = np.nonzero(top.any(0))[0]            # columns of the vertical arm
        left = cross[:, x0:x0 + (x1 - x0) // 5]
        ay = np.nonzero(left.any(1))[0]           # rows of the horizontal arm
        yy, xx = np.mgrid[0:cross.shape[0], 0:cross.shape[1]]
        arms = {"up": cross & (yy < ay.min()), "down": cross & (yy > ay.max()),
                "right": cross & (xx > ax.max()), "left": cross & (xx < ax.min())}
    else:
        arms = {n: mask(p) for n, p in cfg["arms"].items()}
    pics["dpad_v"], pos["up"] = marker(arms["up"])
    pos["down"] = marker(arms["down"])[1]
    pics["dpad_h"], pos["right"] = marker(arms["right"])
    pos["left"] = marker(arms["left"])[1]

    # places in the composed atlas: the pad at the corner, the markers in a row under it
    at = {"pad": (0, 0, pad_w, pad_h)}
    x, y = 0, pad_h + 2
    for n in ("ring", "l1", "l2", "dpad_v", "dpad_h"):
        w, h = pics[n].shape[1] // X4, pics[n].shape[0] // X4
        at[n] = (x, y, w, h)
        x += w + 2
    assert x <= 256 and y + max(p.shape[0] // X4 for p in pics.values()) <= ATLAS_ROWS, (x, y)
    for n, img in pics.items():
        Image.fromarray(img, "RGBA").save(os.path.join(HERE, "%s_lesson_%s.png" % (prefix, n)), optimize=True)
    order = ("pad", "ring", "l2", "l1", "dpad_v", "dpad_h")   # nv2a_btnicons.c L_PAD ... L_DPAD_H
    with open(os.path.join(HERE, "%s_lesson.txt" % prefix), "w") as f:
        f.write("pad %d x %d texels\n" % (pad_w, pad_h))
        for n in order:
            f.write("at %-7s %d %d %d x %d\n" % ((n,) + at[n]))
        for n in ROWS:
            f.write("marker %-8s %g %g\n" % (n, pos[n][0], pos[n][1]))
        f.write("C at:   %s\n" % ", ".join("{ %d, %d, %d, %d }" % at[n] for n in order))
        f.write("C mark: %s\n" % ", ".join("{ %g, %g }" % pos[n] for n in ROWS))
    print(prefix, open(os.path.join(HERE, "%s_lesson.txt" % prefix)).read())


def main():
    for prefix, cfg in PADS.items():
        make(prefix, cfg)


if __name__ == "__main__":
    main()
