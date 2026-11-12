#!/usr/bin/env python3
"""Fork pass: tell the runtime which halo each visibility test belongs to.

Every visibility test of the title (the sun's flare, the lights' halos: lamps,
ground glows, coloured lights) is issued by one method, sub_001000E0, the only
caller of D3DDevice_BeginVisibilityTest (0x00169720) and EndVisibilityTest
(0x00169750). Its test number is not the halo's: it is the next value of a
counter that the frame start sets to 0 or 0x800 (the image's parity, 0x000FFAFF)
and each test moves on by one, kept in the test's object at +8:

    0x0010015B  mov edx, [ebx+0x18E630] ; mov [esi+8], edx
                mov esi, [ebx+0x18E630] ; ... ; inc esi ; mov [ebx+0x18E630], esi

The report of number n is at page[n >> 8] + (n & 0xFF) * 16 (0x00168FA0): an
address is "the k-th test of the images of one parity", whichever halo that is.

The object esi is not the halo either: it is a visibility rectangle that the
halo takes anew each image from defVisibilityRectList (128 rectangles of 32
bytes, two halves of 64 used in turn by image parity, the second 0x800 bytes
after the first: sub_00104C50, 0x00104F12, 0x000FF972), in the order the halos
ask (sub_00100EA0 / sub_00100EF2: the next rectangle of the image's half). The
halo itself is the object that asks: it passes its own rectangle data at +0x24
and keeps the rectangle it got (+0x35C or +0xD0: 0x000E10E7, 0x000E1157,
0x000E2094).

The runtime (xbox_memory_layout.c, above 60 only) hands a halo whose report is
still pending its own newest count; for that it must know whose test each
number is. This pass adds two calls, which record (rectangle, halo) and
(number, rectangle) and change nothing the title reads or writes:

    0x00100F3D (sub_00100EF2: eax the rectangle given out, ebx the halo's data
    pointer, ecx the render context):
        if (g_fix_occlwait) xbox_occl_rect(eax, ebx, ecx);
    0x0010015B (sub_001000E0, right after the number is stored in the rectangle):
        if (g_fix_occlwait) xbox_occl_emit(edx, esi);

The switch is the one of the wait (fix_occlwait.py, port/src/main.c).

    fix_occlemit.py GEN_DIR [--dry-run]
Idempotent. Stops if the code at either site changed shape.
"""
import glob, io, os, sys

TAG = "/* fork_occlemit */"
NL = "\n"
FUNC = "void sub_001000E0(void)" + NL + "{" + NL
SITE = ("loc_0010015B: ;" + NL +
        "    RECOMP_LOC(0x0010015B);" + NL +
        "    edx = MEM32(ebx + 0x18E630);" + NL +
        "    MEM32(esi + 8) = edx;" + NL +
        "    esi = MEM32(ebx + 0x18E630);" + NL +
        "    eax = MEM32(ebx);" + NL +
        "    esi++;" + NL +
        "    MEM32(ebx + 0x18E630) = esi;" + NL)
HOOK = ("    if (g_fix_occlwait) xbox_occl_emit(edx, esi); " + TAG +
        " /* this test's number is the rectangle's (esi) until its next test */" + NL)
NEW = SITE.replace("    MEM32(esi + 8) = edx;" + NL, "    MEM32(esi + 8) = edx;" + NL + HOOK)
FUNC2 = "void sub_00100EF2(void)" + NL + "{" + NL
SITE2 = ("    MEM32(esp + 0x1C) = eax;" + NL +
         "    MEM32(eax + 8) = 0xFFFFFFFFu;" + NL +
         "    if (TEST_Z(edx, edx)) goto loc_0010105A; /* je: equal / zero */" + NL)
HOOK2 = ("    if (g_fix_occlwait) xbox_occl_rect(eax, ebx, ecx); " + TAG +
         " /* the rectangle given out (eax) is this halo's (ebx: its data, +0x24) for this image */" + NL)
NEW2 = SITE2.replace("    if (TEST_Z(edx, edx))", HOOK2 + "    if (TEST_Z(edx, edx))")
DECL = ("extern int g_fix_occlwait; " + TAG + NL +
        "extern void xbox_occl_emit(uint32_t number, uint32_t rect); " + TAG + NL +
        "extern void xbox_occl_rect(uint32_t rect, uint32_t halo, uint32_t ctx); " + TAG + NL)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    gen, dry = args[0], "--dry-run" in sys.argv
    total = done = 0
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", newline="").read()
        if FUNC not in src and FUNC2 not in src:
            continue
        if HOOK in src or HOOK2 in src:
            if not (HOOK in src and HOOK2 in src):
                sys.exit("fix_occlemit: %s: one site done, not the other" % path)
            done += 1
            print("%s: already done" % os.path.basename(path))
            continue
        if not (FUNC in src and FUNC2 in src):
            sys.exit("fix_occlemit: %s: sub_001000E0 and sub_00100EF2 not in the same file" % path)
        new = src
        for func, site, repl, label in ((FUNC, SITE, NEW, "0x0010015B"), (FUNC2, SITE2, NEW2, "0x00100F3D")):
            if new.count(site) != 1:
                sys.exit("fix_occlemit: %s: the code at %s changed shape" % (path, label))
            f0 = new.index(func)
            f1 = new.find(NL + "void sub_", f0 + len(func))
            if not (f0 < new.index(site) < (f1 if f1 >= 0 else len(new))):
                sys.exit("fix_occlemit: %s: %s not in its function" % (path, label))
            new = new.replace(site, repl)
        if DECL not in new:
            first = new.index(NL + "void sub_")
            new = new[:first + 1] + DECL + new[first + 1:]
        total += 1
        print("%s: each rectangle's halo (0x00100F3D) and each test's rectangle (0x0010015B) recorded"
              % os.path.basename(path))
        if not dry:
            io.open(path, "w", encoding="utf-8", newline="").write(new)
    if total + done != 1:
        # SSX has exactly one file holding both: recomp_0006.c.
        sys.exit("fix_occlemit: expected 1 file, found %d" % (total + done))
    print("fix_occlemit: %d file%s" % (total + done, " (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
