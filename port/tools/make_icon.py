#!/usr/bin/env python3
"""The executable's icon, made at build time from the player's
own game files -- never stored in the repository.

    make_icon.py --game-files ../game_files --fallback src/ssx_recomp.ico \
                 --out build/ssx_app.ico [--pick garibaldi|flocon|medal|logo]

Reads one picture from the disc's textures (the Garibaldi track badge by
default), scales it uniformly into a square at 256, 128, 64,
48, 32, 24 and 16 px, and writes a .ico (PNG entries) to --out, in the build
directory only. If the game files are missing or anything fails, --fallback
(an original icon drawn for this fork, in the repository) is copied instead,
so a build without the game still has an icon. Pure Python: zlib, struct.

Pictures (SHPX entries of the disc's texture archives):
    garibaldi data/textures/hudgame.xsh entry "map1", 32-bit, box (0,0)-(61,61)
    flocon  data/textures/hudgame.xsh  entry "hud1", 32-bit, box (0,0)-(62,55)
    medal   data/textures/hudgame.xsh  entry "map4", 32-bit, the gold hexagon
    logo    data/textures/loading.xsh  entry "loa2", DXT3,   the SSX Tricky logo
"""
import argparse, os, shutil, struct, sys, zlib

SIZES = (256, 128, 64, 48, 32, 24, 16)
PICKS = {
    'garibaldi': ('data/textures/hudgame.xsh', 'map1', (0, 0, 61, 61), None),
    'flocon': ('data/textures/hudgame.xsh', 'hud1', (0, 0, 62, 55), None),
    'medal':  ('data/textures/hudgame.xsh', 'map4', (2, 0, 81, 59),
               [(1, 28), (17, 1), (59, 1), (72, 28), (59, 55), (17, 55)]),
    'logo':   ('data/textures/loading.xsh', 'loa2', (2, 2, 254, 148), None),
}


def shpx_entry(data, name):
    """(fmt, w, h, pixel offset) of a 4-character entry of an SHPX bank."""
    if data[:4] != b'SHPX':
        raise ValueError('not an SHPX bank')
    count, = struct.unpack_from('<I', data, 8)
    for i in range(count):
        if data[0x10 + i * 8:0x14 + i * 8] == name.encode():
            off, = struct.unpack_from('<I', data, 0x14 + i * 8)
            hdr, w, h = struct.unpack_from('<IHH', data, off)
            return hdr & 0xFF, w, h, off + 16
    raise KeyError(name)


def decode(data, name):
    """RGBA rows (list of bytearrays) of an entry: 0x7D 32-bit BGRA, 0x61 DXT3."""
    fmt, w, h, o = shpx_entry(data, name)
    px = [bytearray(w * 4) for _ in range(h)]
    if fmt == 0x7D:
        for y in range(h):
            row = data[o + y * w * 4:o + (y + 1) * w * 4]
            for x in range(w):
                b, g, r, a = row[x * 4:x * 4 + 4]
                px[y][x * 4:x * 4 + 4] = bytes((r, g, b, a))
        return w, h, px
    if fmt == 0x61:
        def c565(c):
            return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
        bw = (w + 3) // 4
        for by in range(0, h, 4):
            for bx in range(0, w, 4):
                b = o + ((by // 4) * bw + bx // 4) * 16
                c0, c1, bits = struct.unpack_from('<HHI', data, b + 8)
                p0, p1 = c565(c0), c565(c1)
                pal = [p0, p1, tuple((2 * a + c) // 3 for a, c in zip(p0, p1)), tuple((a + 2 * c) // 3 for a, c in zip(p0, p1))]
                for k in range(16):
                    x, y = bx + (k & 3), by + (k >> 2)
                    if x < w and y < h:
                        a = ((data[b + k // 2] >> ((k & 1) * 4)) & 15) * 17
                        px[y][x * 4:x * 4 + 4] = bytes(pal[(bits >> (2 * k)) & 3] + (a,))
        return w, h, px
    raise ValueError('format 0x%02X' % fmt)


def inside(poly, x, y):
    n, c = len(poly), False
    for i in range(n):
        (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % n]
        if (y0 > y) != (y1 > y) and x < (x1 - x0) * (y - y0) / (y1 - y0) + x0:
            c = not c
    return c


def crop(img, box, poly):
    w, h, px = img
    x0, y0, x1, y1 = box
    rows = []
    for y in range(y0, y1):
        r = bytearray(px[y][x0 * 4:x1 * 4])
        if poly:
            for x in range(x1 - x0):
                if not inside(poly, x + 0.5, y - y0 + 0.5):
                    r[x * 4 + 3] = 0
        rows.append(r)
    return x1 - x0, y1 - y0, rows


def premul_sample(img, fx, fy):
    """Bilinear sample, premultiplied (no dark fringes), outside = transparent."""
    w, h, px = img
    x0, y0 = int(fx // 1), int(fy // 1)
    tx, ty = fx - x0, fy - y0
    acc = [0.0, 0.0, 0.0, 0.0]
    for (xx, yy, wt) in ((x0, y0, (1 - tx) * (1 - ty)), (x0 + 1, y0, tx * (1 - ty)),
                         (x0, y0 + 1, (1 - tx) * ty), (x0 + 1, y0 + 1, tx * ty)):
        if wt and 0 <= xx < w and 0 <= yy < h:
            r, g, b, a = px[yy][xx * 4:xx * 4 + 4]
            acc[0] += wt * r * a; acc[1] += wt * g * a; acc[2] += wt * b * a; acc[3] += wt * a
    return acc


def square(img, size):
    """Uniform scale into size x size, centred: supersampled (4 x 4) so both
    up- and down-scaling stay smooth."""
    w, h, _ = img
    inner = size * 0.92 if size >= 32 else size
    k = min(inner / w, inner / h)
    dw, dh = w * k, h * k
    ox, oy = (size - dw) / 2, (size - dh) / 2
    out = []
    n = 2 if size >= 64 else 4
    for y in range(size):
        row = bytearray(size * 4)
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(n):
                for sx in range(n):
                    u = (x + (sx + 0.5) / n - ox) / k - 0.5
                    v = (y + (sy + 0.5) / n - oy) / k - 0.5
                    s = premul_sample(img, u, v)
                    for c in range(4):
                        acc[c] += s[c]
            a = acc[3] / (n * n)
            if a > 0.5:
                row[x * 4:x * 4 + 4] = bytes((min(255, round(acc[0] / acc[3])), min(255, round(acc[1] / acc[3])),
                                              min(255, round(acc[2] / acc[3])), min(255, round(a))))
        out.append(row)
    return out


def png(size, rows):
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def ico(images):
    head = struct.pack('<HHH', 0, 1, len(images))
    off = 6 + 16 * len(images)
    dirs, blobs = b'', b''
    for size, data in images:
        dirs += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), off)
        blobs += data
        off += len(data)
    return head + dirs + blobs


def make(game_files, pick):
    path, name, box, poly = PICKS[pick]
    data = open(os.path.join(game_files, path), 'rb').read()
    img = crop(decode(data, name), box, poly)
    return ico([(s, png(s, square(img, s))) for s in SIZES])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game-files', required=True)
    ap.add_argument('--fallback', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--pick', default='garibaldi', choices=sorted(PICKS))
    a = ap.parse_args()
    try:
        blob = make(a.game_files, a.pick)
        tmp = a.out + '.tmp'
        open(tmp, 'wb').write(blob)
        os.replace(tmp, a.out)
        print('make_icon: %s from the game files (%s)' % (a.out, a.pick))
    except Exception as e:                      # no game files, or anything else: the fork's own icon
        shutil.copyfile(a.fallback, a.out)
        print('make_icon: game files not usable (%s): %s' % (e, a.fallback), file=sys.stderr)


if __name__ == '__main__':
    main()
