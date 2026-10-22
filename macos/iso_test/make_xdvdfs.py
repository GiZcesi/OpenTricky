#!/usr/bin/env python3
"""Build a tiny XDVDFS image for tests: no game data, just the structure the reader walks.

    make_xdvdfs.py out.iso

Layout: descriptor at sector 32, root directory at 33 (DEFAULT.XBE, DATA, ZZ_LAST.TXT), DATA at 34
(HELLO.TXT, BIG.BIN, SUB), SUB at 35 (DEEP.TXT), files from sector 36. Directories are binary trees
ordered by upper-case name, as on a real disc."""
import struct, sys

SECTOR = 2048

def pack_dir(entries):
    entries = sorted(entries, key=lambda e: e[0].upper())
    n = len(entries)
    # records are written root-first (the reader starts at offset 0): pre-order of a balanced BST
    pre = []
    links = {}
    def build(lo, hi):
        if lo > hi: return None
        mid = (lo + hi) // 2
        idx = len(pre); pre.append(mid)
        l = build(lo, mid - 1)
        r = build(mid + 1, hi)
        links[mid] = (l, r)
        return mid
    build(0, n - 1)
    offs = {}
    pos = 0
    for mid in pre:
        offs[mid] = pos // 4
        pos += (14 + len(entries[mid][0]) + 3) & ~3
    out = bytearray()
    for mid in pre:
        name, sector, size, is_dir = entries[mid]
        l, r = links[mid]
        rec = struct.pack('<HHIIBB', offs[l] if l is not None else 0, offs[r] if r is not None else 0,
                          sector, size, 0x10 if is_dir else 0x20, len(name)) + name.encode('ascii')
        rec += b'\xff' * (((len(rec) + 3) & ~3) - len(rec))
        out += rec
    out += b'\xff' * ((-len(out)) % SECTOR if len(out) % SECTOR else 0)
    if len(out) == 0: out = bytearray(b'\xff' * SECTOR)
    return bytes(out)

def main(path):
    files = {}
    files['DEFAULT.XBE'] = b'XBEH' + bytes(range(256)) * 3
    files['HELLO.TXT'] = b'hello from the disc\n'
    files['BIG.BIN'] = bytes((i * 7 + 3) & 0xFF for i in range(5000))
    files['DEEP.TXT'] = b'deep\n'
    files['ZZ_LAST.TXT'] = b'last\n'
    # sectors
    DESC, ROOT, DATA, SUB = 32, 33, 34, 35
    nxt = 36
    place = {}
    for name in ['DEFAULT.XBE', 'HELLO.TXT', 'BIG.BIN', 'DEEP.TXT', 'ZZ_LAST.TXT']:
        place[name] = nxt
        nxt += (len(files[name]) + SECTOR - 1) // SECTOR
    sub = pack_dir([('deep.txt', place['DEEP.TXT'], len(files['DEEP.TXT']), False)])
    img = bytearray(nxt * SECTOR)
    def put(sec, b): img[sec * SECTOR: sec * SECTOR + len(b)] = b
    # directory entries carry their extent size, so the parents are built after their children
    data = pack_dir([('hello.txt', place['HELLO.TXT'], len(files['HELLO.TXT']), False),
                     ('big.bin', place['BIG.BIN'], len(files['BIG.BIN']), False),
                     ('sub', SUB, len(sub), True)])
    root = pack_dir([('default.xbe', place['DEFAULT.XBE'], len(files['DEFAULT.XBE']), False),
                     ('data', DATA, len(data), True),
                     ('zz_last.txt', place['ZZ_LAST.TXT'], len(files['ZZ_LAST.TXT']), False)])
    desc = bytearray(SECTOR)
    desc[0:20] = b'MICROSOFT*XBOX*MEDIA'
    desc[20:28] = struct.pack('<II', ROOT, len(root))
    desc[0x7EC:0x7EC + 20] = b'MICROSOFT*XBOX*MEDIA'
    put(DESC, desc); put(ROOT, root); put(DATA, data); put(SUB, sub)
    for name, sec in place.items(): put(sec, files[name])
    open(path, 'wb').write(img)

if __name__ == '__main__':
    main(sys.argv[1])
