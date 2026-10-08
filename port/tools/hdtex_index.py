#!/usr/bin/env python3
"""Build the HD texture index from a community texture pack and your disc.

The pack is a texture replacement pack for the PS2 version: DDS files named
by the emulator's own hashes. Nothing ties them to the Xbox textures, so every
texture of the Xbox disc is matched to a pack file by its picture:

  1. every SHPX texture bank of the disc is read (loose .xsh, .xsh inside the
     c0fb .big archives of the tracks and in char/texxbx.big) and each texture
     decoded (DXT1/3/5, 32-bit, 565, 4444, 8-bit palettised);
  2. a 16x16 thumbnail of each Xbox texture and of each pack DDS is compared by
     1 - normalised correlation of the alpha-weighted colours (blind to the PS2
     colour range, which is half the Xbox one): a pair is kept under 0.06;
  3. bump maps (_B) and lightmaps (_L), which only the Xbox has, are never
     replaced; textures of one flat colour gain nothing and are skipped;
  4. the colour factors (RGB x1 or x2, alpha x1 or x2) that bring the pack file
     back to the Xbox colours are chosen per texture; a pair still more than 25
     levels off after them is the same drawing in other colours: rejected.

Output, one line per key (sorted, no picture data, only hashes and file names):
  <fnv64 of the Xbox level-0 bytes> <w>x<h> <rgb factor><alpha factor> <3d|menu> <pack file>
The key is FNV-1a 64 over 8-byte words of the level-0 bytes as the game holds
them in memory (uncompressed textures in linear and in swizzled order, the
game swizzles some at load). "menu" = a picture of textures/ (menus,
interface, loading cards); the game leaves those off unless asked.

usage:
  python port/tools/hdtex_index.py --pack <pack folder> --disc <iso or extracted folder>
         [--out port/assets/hdtex_index.txt] [--report <json>] [--cache <npz>] [--jobs N]
The pack folder may be any folder above the .dds files. Needs numpy and Pillow.
Matching ~3 200 disc textures with ~4 000 pack files takes a few minutes.
"""
import argparse, collections, hashlib, json, os, re, struct, sys, time
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

THUMB = 16
MATCH = 0.06          # 1 - correlation under which a pair is the same picture
COLOUR_MAX = 25.0     # mean error (levels) left after the colour factors
VERSION = 1


# ── disc ─────────────────────────────────────────────────────────────────────

class Xiso:
    """Files of an Xbox disc image (XDVDFS), by path relative to its root."""
    BASES = (0, 0x18300000, 0x0FD90000, 0x02080000, 0x1FB20000)

    def __init__(self, path):
        self.f = open(path, 'rb')
        for base in self.BASES:
            self.f.seek(base + 32 * 2048)
            d = self.f.read(2048)
            if d[:20] == b'MICROSOFT*XBOX*MEDIA' and d[-20:] == b'MICROSOFT*XBOX*MEDIA':
                self.base = base
                root, size = struct.unpack_from('<II', d, 20)
                self.files = {}
                self._walk(root, size, '')
                return
        raise SystemExit('%s: not an Xbox disc image' % path)

    def _read(self, sector, size):
        self.f.seek(self.base + sector * 2048)
        return self.f.read(size)

    def _walk(self, sector, size, prefix):
        if not size:
            return
        d = self._read(sector, size)
        todo = [0]
        while todo:
            o = todo.pop() * 4
            if o + 14 > len(d) or d[o:o + 4] == b'\xff\xff\xff\xff':
                continue
            left, right, sec, sz, attr, n = struct.unpack_from('<HHIIBB', d, o)
            name = d[o + 14:o + 14 + n].decode('latin1')
            if left: todo.append(left)
            if right: todo.append(right)
            p = prefix + name
            if attr & 0x10:
                self._walk(sec, sz, p + '/')
            else:
                self.files[p.lower()] = (p, sec, sz)

    def walk(self):
        for key in sorted(self.files):
            p, sec, sz = self.files[key]
            yield p, (lambda sec=sec, sz=sz: self._read(sec, sz))


def disc_files(disc):
    """(path under data/, reader) of every file, sorted, from an ISO or an extracted folder."""
    if os.path.isfile(disc):
        for p, rd in Xiso(disc).walk():
            if p.lower().startswith('data/'):
                yield p[5:], rd
        return
    root = os.path.join(disc, 'data') if os.path.isdir(os.path.join(disc, 'data')) else disc
    out = []
    for r, _, fs in os.walk(root):
        for f in fs:
            out.append(os.path.relpath(os.path.join(r, f), root).replace('\\', '/'))
    for p in sorted(out, key=str.lower):
        yield p, (lambda p=p: open(os.path.join(root, p), 'rb').read())


def refpack(src):
    b0 = src[0]
    p = 2
    if b0 & 0x01: p += 4 if b0 & 0x80 else 3
    n = 4 if b0 & 0x80 else 3
    size = int.from_bytes(src[p:p + n], 'big'); p += n
    out = bytearray()
    while True:
        c = src[p]
        if c < 0x80:
            c1 = src[p + 1]; p += 2
            lit, ln, off = c & 3, ((c >> 2) & 7) + 3, ((c & 0x60) << 3) + c1 + 1
        elif c < 0xC0:
            c1, c2 = src[p + 1], src[p + 2]; p += 3
            lit, ln, off = c1 >> 6, (c & 0x3F) + 4, ((c1 & 0x3F) << 8) + c2 + 1
        elif c < 0xE0:
            c1, c2, c3 = src[p + 1], src[p + 2], src[p + 3]; p += 4
            lit, ln, off = c & 3, ((c & 0x0C) << 6) + c3 + 5, ((c & 0x10) << 12) + (c1 << 8) + c2 + 1
        elif c < 0xFC:
            lit = ((c & 0x1F) << 2) + 4; p += 1
            out += src[p:p + lit]; p += lit
            continue
        else:
            lit = c & 3; p += 1
            out += src[p:p + lit]
            break
        out += src[p:p + lit]; p += lit
        for _ in range(ln):
            out.append(out[-off])
    if len(out) != size:
        raise ValueError('refpack size')
    return bytes(out)


def big_entries(d):
    """(name, bytes) of the .xsh files of an EA archive (BIGF or c0fb)."""
    if d[:4] == b'BIGF':                                   # char/texxbx.big
        n, = struct.unpack_from('>I', d, 8); q = 16
        for _ in range(n):
            o, sz = struct.unpack_from('>II', d, q); q += 8
            e = d.index(b'\0', q); nm = d[q:e].decode('latin1'); q = e + 1
            if nm.endswith('.xsh'):
                raw = d[o:o + sz]
                yield nm, refpack(raw) if raw[:2] == b'\x10\xfb' else raw
    elif d[:2] == b'\xc0\xfb':
        n = struct.unpack_from('>H', d, 4)[0]; p = 6
        toc = []
        for _ in range(n):
            off = int.from_bytes(d[p:p + 3], 'big'); size = int.from_bytes(d[p + 3:p + 6], 'big'); p += 6
            e = d.index(b'\0', p); toc.append((d[p:e].decode('latin1'), off, size)); p = e + 1
        for name, off, size in toc:
            if name.endswith('.xsh'):
                blob = d[off:off + size]
                yield name, refpack(blob) if len(blob) > 2 and blob[1] == 0xFB else blob


def banks(disc):
    """(bank name, bytes) of every SHPX bank on the disc."""
    for p, rd in disc_files(disc):
        low = p.lower()
        if low.endswith('.xsh'):
            yield p, rd()
        elif low.endswith('.big') and not low.startswith(('audio/', 'video/')):
            d = rd()
            try:
                for nm, b in big_entries(d):
                    yield p + ':' + nm, b
            except (ValueError, IndexError, struct.error):
                continue


def entries(d):
    """(index, offset, format, w, h) of each texture of a SHPX bank."""
    if d[:4] != b'SHPX':
        return
    n, = struct.unpack_from('<I', d, 8)
    for i in range(n):
        if 0x18 + i * 8 > len(d): break
        off, = struct.unpack_from('<I', d, 0x14 + i * 8)
        if off + 16 > len(d): continue
        hdr, w, h = struct.unpack_from('<IHH', d, off)
        yield i, off, hdr & 0xFF, w, h


def rgb565(c):
    return np.stack([(c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1)


def dxt(d, o, w, h, kind):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    bs = 8 if kind == 1 else 16
    raw = np.frombuffer(d, np.uint8, bw * bh * bs, o).reshape(bh * bw, bs)
    col = raw[:, bs - 8:]
    c0 = col[:, 0].astype(np.uint32) | (col[:, 1].astype(np.uint32) << 8)
    c1 = col[:, 2].astype(np.uint32) | (col[:, 3].astype(np.uint32) << 8)
    bits = col[:, 4:8].astype(np.uint32)
    bits = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
    p0, p1 = rgb565(c0).astype(np.int32), rgb565(c1).astype(np.int32)
    four = (c0 > c1) | (kind != 1)
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)
    idx = (bits[:, None] >> (2 * np.arange(16))) & 3
    rgb = np.take_along_axis(pal, idx[:, :, None].repeat(3, 2), 1)
    if kind == 1:
        a = np.where((~four[:, None]) & (idx == 3), 0, 255)
    elif kind == 3:
        ab = raw[:, :8]
        a = np.stack([(ab[:, k // 2] >> (4 * (k & 1))) & 15 for k in range(16)], 1).astype(np.int32) * 17
    else:
        a = np.full((len(raw), 16), 255, np.int32)
    out = np.concatenate([rgb, a[:, :, None]], 2).astype(np.uint8)
    out = out.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return out[:h, :w]


def decode(d, off, fmt, w, h):
    """RGBA (h, w, 4) of a disc texture, or None (DXT5 decodes its colour only)."""
    o = off + 16
    if fmt == 0x7D and o + w * h * 4 <= len(d):
        return np.frombuffer(d, np.uint8, w * h * 4, o).reshape(h, w, 4)[:, :, [2, 1, 0, 3]]
    if fmt in (0x78, 0x6D) and o + w * h * 2 <= len(d):
        c = np.frombuffer(d, '<u2', w * h, o).reshape(h, w).astype(np.int32)
        if fmt == 0x78:
            return np.concatenate([rgb565(c), np.full((h, w, 1), 255)], 2).astype(np.uint8)
        return (np.stack([(c >> 8) & 15, (c >> 4) & 15, c & 15, c >> 12], -1) * 17).astype(np.uint8)
    if fmt == 0x60: return dxt(d, o, w, h, 1)
    if fmt == 0x61: return dxt(d, o, w, h, 3)
    if fmt == 0x62: return dxt(d, o, w, h, 5)
    if fmt == 0x7B and o + w * h + 16 + 1024 <= len(d):
        idx = np.frombuffer(d, np.uint8, w * h, o).reshape(h, w)
        pal = np.frombuffer(d, np.uint8, 1024, o + w * h + 16).reshape(256, 4)[:, [2, 1, 0, 3]]
        return pal[idx]
    return None


def morton(w, h):
    ys, xs = np.mgrid[0:h, 0:w]
    res = np.zeros((h, w), np.int64)
    wb, hb = w.bit_length() - 1, h.bit_length() - 1
    i = j = ob = 0
    while i < wb or j < hb:
        if i < wb: res |= ((xs >> i) & 1) << ob; ob += 1; i += 1
        if j < hb: res |= ((ys >> j) & 1) << ob; ob += 1; j += 1
    return res


def level0(d, off, fmt, w, h):
    """Level-0 bytes as the game holds them: DXT as stored; 32 / 16 bits linear and swizzled."""
    o = off + 16
    if fmt == 0x60: return [d[o:o + ((w + 3) // 4) * ((h + 3) // 4) * 8]]
    if fmt in (0x61, 0x62): return [d[o:o + ((w + 3) // 4) * ((h + 3) // 4) * 16]]
    bpp = {0x7D: 4, 0x78: 2, 0x6D: 2}.get(fmt)
    if not bpp or o + w * h * bpp > len(d): return []
    lin = np.frombuffer(d, np.uint8, w * h * bpp, o).reshape(h * w, bpp)
    swz = np.empty_like(lin); swz[morton(w, h).reshape(-1)] = lin
    return [lin.tobytes(), swz.tobytes()]


def fnv64(b):
    """FNV-1a 64 over 8-byte words then the tail bytes (as nv2a_hdtex.c)."""
    h = 0xcbf29ce484222325
    n = len(b) // 8
    for v in np.frombuffer(b, '<u8', n).tolist():
        h = ((h ^ v) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    for v in b[n * 8:]:
        h = ((h ^ v) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def thumb(a):
    im = Image.fromarray(np.ascontiguousarray(a)).convert('RGBA')
    return np.asarray(im.resize((THUMB, THUMB), Image.BOX), np.uint8)


def category(bank):
    """'menu' for the pictures of textures/, 'skip' for Xbox-only maps, else '3d'."""
    if bank.lower().startswith('textures/'):
        return 'menu'
    m = re.search(r'/(\w+)\.xsh$', bank)
    if m and m.group(1).endswith(('_B', '_L')) and 'models/' in bank:
        return 'skip'
    return '3d'


def disc_textures(disc):
    """Groups of identical disc pictures: {md5: {'thumb', 'names', 'keys', 'cat', 'w', 'h'}}."""
    groups = collections.OrderedDict()
    nb = nt = 0
    for b, d in banks(disc):
        nb += 1
        for i, off, fmt, w, h in entries(d):
            nt += 1
            try:
                a = decode(d, off, fmt, w, h)
                keys = level0(d, off, fmt, w, h)
            except (ValueError, IndexError):
                continue
            if a is None or w < 4 or h < 4:
                continue
            md5 = hashlib.md5(a.tobytes()).hexdigest()
            c = category(b)
            g = groups.get(md5)
            if g is None:                                 # the group's category is its first one's
                g = groups[md5] = {'thumb': thumb(a), 'names': [], 'keys': [], 'cat': c, 'w': w, 'h': h}
            g['names'].append('%s#%d' % (b, i))
            if c != 'skip':                               # each key keeps its own bank's category
                g['keys'] += [(fnv64(k), w, h, c) for k in keys]
    return groups, nb, nt


# ── pack ─────────────────────────────────────────────────────────────────────

def find_dds(pack):
    out = []
    for r, _, fs in os.walk(pack):
        for f in fs:
            if f.lower().endswith('.dds'):
                out.append(os.path.join(r, f))
    if not out:
        raise SystemExit('%s: no .dds file in it' % pack)
    top = os.path.commonpath([os.path.dirname(p) for p in out])
    return top, sorted(os.path.relpath(p, top).replace('\\', '/') for p in out)


def pack_thumb(path):
    try:
        im = Image.open(path)
        return thumb(np.asarray(im.convert('RGBA')))
    except Exception as e:                                # unreadable file: never matched
        print('  cannot read %s: %s' % (path, e), file=sys.stderr)
        return None


def feat(t):
    t = t.astype(np.float32)
    f = (t[..., :3] * t[..., 3:] / 255).reshape(len(t), -1)
    f = f - f.mean(1, keepdims=True)
    return f / (np.linalg.norm(f, axis=1, keepdims=True) + 1e-3)


def colour_factors(xt, pt):
    """(error, rgb factor, alpha factor) bringing the pack thumbnail closest to the Xbox one."""
    best = None
    xt = xt.astype(float)
    for kr in (1, 2):
        for ka in (1, 2):
            q = pt.astype(float)
            q[..., :3] = np.minimum(q[..., :3] * kr, 255)
            q[..., 3] = np.minimum(q[..., 3] * ka, 255)
            e = np.abs(xt[..., :3] - q[..., :3]).mean() + np.abs(xt[..., 3] - q[..., 3]).mean()
            if best is None or e < best[0]:
                best = (e, kr, ka)
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pack', required=True, help='pack folder (any folder above the .dds files)')
    ap.add_argument('--disc', required=True, help='your disc image (.iso) or its extracted folder')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'hdtex_index.txt'))
    ap.add_argument('--report', help='write matches, rejections and ambiguities as JSON')
    ap.add_argument('--cache', help='npz of the pack thumbnails, reused when the file list is the same')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    t0 = time.time()

    print('disc: reading textures ...')
    groups, nbanks, ntex = disc_textures(a.disc)
    print('  %d banks, %d textures, %d distinct pictures (%.0f s)' % (nbanks, ntex, len(groups), time.time() - t0))

    top, files = find_dds(a.pack)
    pt = None
    if a.cache and os.path.exists(a.cache):
        z = np.load(a.cache, allow_pickle=False)
        if list(z['names']) == files:
            pt, ok = z['thumbs'], z['ok']
            print('pack: %d thumbnails from %s' % (len(files), a.cache))
    if pt is None:
        print('pack: %d .dds files in %s, decoding (%d jobs) ...' % (len(files), top, a.jobs))
        with ProcessPoolExecutor(a.jobs) as ex:
            res = list(ex.map(pack_thumb, [os.path.join(top, f) for f in files], chunksize=16))
        ok = np.array([r is not None for r in res])
        pt = np.stack([r if r is not None else np.zeros((THUMB, THUMB, 4), np.uint8) for r in res])
        if a.cache:
            np.savez_compressed(a.cache, names=np.array(files), thumbs=pt, ok=ok)
        print('  done (%.0f s)' % (time.time() - t0))

    gl = list(groups.values())
    xt = np.stack([g['thumb'] for g in gl])
    D = 1 - feat(pt) @ feat(xt).T                         # (pack, xbox)
    D[~ok] = 9
    flat = (xt[..., :3].astype(np.float32) * xt[..., 3:] / 255).reshape(len(xt), -1).std(1) < 4

    lines, report = {}, {'matched': [], 'rejected': [], 'ambiguous': [], 'key_conflicts': []}
    stats = collections.Counter()
    P = feat(pt)
    for j, g in enumerate(gl):
        cat = g['cat']
        if cat == 'skip':
            stats['xbox-only maps (_B, _L)'] += 1; continue
        if flat[j]:
            stats['one flat colour'] += 1; continue
        stats['candidates ' + cat] += 1
        cand = np.where(D[:, j] < MATCH)[0]
        if not len(cand):
            stats['no match ' + cat] += 1; continue
        b = int(cand[np.argmin(D[cand, j])])
        others = [int(k) for k in cand if k != b and 1 - P[k] @ P[b] > MATCH]
        if others:
            report['ambiguous'].append([g['names'][0], files[b], files[others[0]]])
        err, kr, ka = colour_factors(xt[j], pt[b])
        if err > COLOUR_MAX:
            stats['rejected (colours) ' + cat] += 1
            report['rejected'].append([round(float(err), 1), g['names'][0], files[b]]); continue
        stats['matched ' + cat] += 1
        report['matched'].append([g['names'], files[b], kr, ka, round(float(D[b, j]), 4), round(float(err), 1)])
        for key, w, h, c in g['keys']:
            line = '%016x %dx%d %d%d %s %s' % (key, w, h, kr, ka, c, files[b])
            if key in lines and lines[key] != line:
                report['key_conflicts'].append([lines[key], line]); continue
            lines[key] = line

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', newline='\n') as f:
        f.write('# SSX Tricky HD texture index v%d (port/tools/hdtex_index.py) -- hashes and file names only, no picture\n' % VERSION)
        f.write('# pack: %d .dds files; disc: %d textures in %d banks\n' % (len(files), ntex, nbanks))
        f.write('# <fnv64 of the Xbox level-0 bytes> <w>x<h> <rgb factor><alpha factor> <3d|menu> <pack file>\n')
        for k in sorted(lines):
            f.write(lines[k] + '\n')
    used = len({l.rsplit(' ', 1)[1] for l in lines.values()})
    print('index: %s -- %d keys, %d pack files used, %d ambiguous, %d key conflicts (%.0f s)' %
          (a.out, len(lines), used, len(report['ambiguous']), len(report['key_conflicts']), time.time() - t0))
    for k in sorted(stats):
        print('  %-28s %5d' % (k, stats[k]))
    if a.report:
        report['stats'] = dict(stats)
        with open(a.report, 'w') as f:
            json.dump(report, f, indent=0)


if __name__ == '__main__':
    main()
