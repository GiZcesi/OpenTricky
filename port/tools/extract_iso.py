#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Copy the few files the build needs out of your own SSX Tricky (USA) Xbox
disc image, into game_files/ (ignored by git).

    extract_iso.py "path/to/SSX Tricky.iso" game_files

The build needs:
    default.xbe                    the game's executable, translated to C
    data/textures/hudgame.xsh      the application icon (optional)
    data/textures/loading.xsh      the application icon (optional)

Reads XDVDFS (the Xbox disc file system) directly: an extract-xiso image or a
full disc dump (the game partition is then found at its usual offset). No
other tool needed.
"""
import os, struct, sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Where the game partition starts: xiso, then the usual full-dump offsets.
PARTITIONS = (0, 0x18300000, 0x0FD90000, 0x02080000)
WANTED = ("default.xbe", "data/textures/hudgame.xsh", "data/textures/loading.xsh")
REQUIRED = ("default.xbe",)


def find_partition(f):
    for base in PARTITIONS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    return None


def read_dir(f, base, sector, size):
    """Directory table -> {lower-case name: (sector, size, is_dir)}."""
    if size == 0:
        return {}
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    out = {}
    todo = [0]
    while todo:
        off = todo.pop()
        if off + 14 > len(table):
            continue
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue                                  # padding
        name = table[off + 14:off + 14 + nlen].decode("ascii", "replace")
        out[name.lower()] = (start, length, bool(attr & 0x10))
        if left:
            todo.append(left * 4)
        if right:
            todo.append(right * 4)
    return out


def lookup(f, base, root, path):
    sector, size = root
    entry = None
    for part in path.lower().split("/"):
        entries = read_dir(f, base, sector, size)
        if part not in entries:
            return None
        entry = entries[part]
        sector, size = entry[0], entry[1]
    return entry


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    iso, out = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        base = find_partition(f)
        if base is None:
            sys.exit("%s: not an Xbox disc image (no XDVDFS header found)" % iso)
        f.seek(base + 32 * SECTOR + len(MAGIC))
        root = struct.unpack("<II", f.read(8))
        for path in WANTED:
            e = lookup(f, base, root, path)
            if e is None or e[2]:
                if path in REQUIRED:
                    sys.exit("%s: %s not found -- is this SSX Tricky (USA) for Xbox?" % (iso, path))
                print("   (not found, skipped: %s)" % path)
                continue
            dst = os.path.join(out, *path.split("/"))
            os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
            f.seek(base + e[0] * SECTOR)
            left = e[1]
            with open(dst, "wb") as g:
                while left:
                    chunk = f.read(min(left, 1 << 20))
                    if not chunk:
                        sys.exit("%s: truncated image" % iso)
                    g.write(chunk)
                    left -= len(chunk)
            print("   %s (%d bytes)" % (path, e[1]))


if __name__ == "__main__":
    main()
