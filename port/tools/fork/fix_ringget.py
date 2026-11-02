#!/usr/bin/env python3
"""Fork pass: the GPU's read position in the push buffer's own address space.

The title's D3D runtime keeps the GPU from reading commands it is overwriting:
before handing out room in the push-buffer ring, MakeSpace (0x16B680, its tail
lifted as sub_0016B6B3) reads the GPU's read pointer -- NV_USER DMA_GET,
[[ctx+0x2304]+0x44], a physical address -- turns it into a CPU address with
`or eax, 0x80000000` (the console maps contiguous memory at 0x80000000 +
physical) and compares it with the ring's base and end ([ctx+0x10],
[ctx+0x14]; ctx = 0x174B30). If the GPU is inside the room asked for, it waits
(segment fences, then a spin on DMA_GET).

In the recomp the ring lives at its physical address (base 0x03DF8000, end
0x03FF8000, seen at run time), so `GET | 0x80000000` is never inside
[base, end): the runtime takes the fallback register (NV_PFIFO_CACHE1_DMA_
SUBROUTINE, never written here), ORs 0x80000000 again, and both checks find
the GPU "out of the way". The ring has no flow control at all: when the pump
thread (xboxrecomp/src/nv2a/nv2a_live_pb.c, the "GPU") falls more than one
lap (2 MB) behind -- two-player split screen at Draw distance Max writes about
1 MB of commands per frame -- the title overwrites commands the pump has not
translated yet. The pump then reads index data as methods; one of them,
NV097_SET_COLOR_MASK, which the title never sets during a race, keeps a value
made of two vertex indices: false colours and images of one colour.

This pass replaces the five `| 0x80000000` of MakeSpace (the wrap wait at
0x16B700, the room check at 0x16B725 / 0x16B73F, the spin at 0x16B7DF /
0x16B7F9) by the conversion into the ring's own address space,
fork_ring_va(): the low 28 bits (the physical address) with the high nibble of
the ring's base. On the console that is exactly `| 0x80000000`; here it is the
address the ring really has. The pump publishes DMA_GET only after translating
up to it (xbox_memory_layout.c, "ack late"; and nv2a_live_pb.c translates a
batch even when the write pointer stands past the segment limit), so the
runtime's waits hold: the title waits for the pump instead of writing over
commands it has not read.

The same conversion in sub_0016B2F0, sub_0016B320 and sub_0016B4AA is left as
it is. Those readers feed other waits of the runtime; converted there too, the
title froze in its first menu (freeze report of 31/10 15:07: the game
thread in KeWaitForSingleObject, not studied further). Left alone, they keep
seeing the GPU as never behind, as in every build so far.

    fix_ringget.py GEN_DIR [--dry-run]
Idempotent. Stops if the number of sites differs from 5.
"""
import glob, io, os, re, sys

TAG = "/* fork_ringget */"
DECL = ("static inline uint32_t fork_ring_va(uint32_t a) { return (a & 0x0FFFFFFFu) | "
        "(MEM32(0x174B40u) & 0xF0000000u); } " + TAG + "\n")
FUNCS = {"sub_0016B6B3": 5}
OLD = re.compile(r"^(?P<ind>[ \t]*)(?P<r>e[a-d]x) = (?P=r) \| 0x80000000u;(?P<eol>\r?\n)", re.M)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    gen, dry = args[0], "--dry-run" in sys.argv
    files = {}
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        for fn in FUNCS:
            if "void %s(void)" % fn in src:
                files.setdefault(path, src)
    if not files:
        sys.exit("fix_ringget: none of %s found" % ", ".join(FUNCS))
    total, done = 0, 0
    for path, src in files.items():
        nl = "\r\n" if "\r\n" in src else "\n"
        new = src
        for fn, want in FUNCS.items():
            head = "void %s(void)%s{%s" % (fn, nl, nl)
            if head not in new:
                continue
            start = new.index(head)
            end = new.index(nl + "}" + nl, start)
            body = new[start:end]
            if TAG in body:
                done += body.count("fork_ring_va(")
                continue
            sites = OLD.findall(body)
            if len(sites) != want:
                sys.exit("fix_ringget: %s has %d `| 0x80000000` sites, %d expected: stop rather than guess"
                         % (fn, len(sites), want))
            body = OLD.sub(lambda m: "%s%s = fork_ring_va(%s); %s /* GPU read pointer in the ring's address "
                           "space, not | 0x80000000 */%s" % (m.group("ind"), m.group("r"), m.group("r"), TAG,
                                                            m.group("eol")), body)
            new = new[:start] + body + new[end:]
            total += want
            print("%s: %s, %d site(s)" % (os.path.basename(path), fn, want))
        if new != src:
            if DECL.replace("\n", nl) not in new:
                first = new.index(nl + "void sub_")
                new = new[:first + len(nl)] + DECL.replace("\n", nl) + new[first + len(nl):]
            if not dry:
                io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
    if total + done != 5:
        sys.exit("fix_ringget: %d sites (patched now %d, already %d), 5 expected" % (total + done, total, done))
    print("fix_ringget: 5 sites%s%s" % (" (already patched)" if done == 5 else "", " (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
