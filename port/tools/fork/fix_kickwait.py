#!/usr/bin/env python3
"""Fork pass: no busy wait on the NV2A write-combine flush bit.

The title's D3D runtime flushes the GPU's write-combine buffer before handing it
work: it sets NV_PFB_WBC_FLUSH (bit 16) of NV_PFB_WBC (PFB block 0x100000 +
0x410, i.e. [GPU+0x100410]) and spins until the bit reads back clear:

    0x0016B410  flush only                       (3 callers)
    0x0016B45E  flush, then write the new PUT    (the push-buffer kick)

The host has no write-combine buffer to flush, and nothing models the bit: it is
plain memory that the PFIFO pump thread clears at the top of each of its passes
(xbox_memory_layout.c), i.e. only after it has translated the whole batch it was
working on. So every kick made the game thread wait for the translator: the game
and the pump ran one after the other (~55 % of the game thread in this
loop). xemu reads NV_PFB_WBC as 0 ("Flush not pending", hw/xbox/nv2a/pfb.c), so
the title never waits there.

This guards each such wait with the runtime switch g_fix_kickwait
(XBOX_FIX_KICKWAIT, default 1, port/src/main.c): on, the loop is not taken, as
on xemu; 0, the original wait. What protects the data the title rewrites is
unchanged: GET, the fences and the frame throttle are still published by the
pump only after translating (xbox_memory_layout.c, "ack late").

    fix_kickwait.py GEN_DIR [--dry-run]
Idempotent.
"""
import glob, io, os, re, sys

WAIT = re.compile(r"^(?P<ind>[ \t]*)if \(TEST_NZ\(MEM32\((?P<reg>e[a-d]x) \+ 0x100410\), 0x10000\)\) "
                  r"goto (?P<lbl>loc_[0-9A-F]{8});(?P<rest>.*)$", re.M)
DECL = "extern int g_fix_kickwait; /* fork_kickwait */\n"
GUARD = "%(ind)sif (TEST_NZ(MEM32(%(reg)s + 0x100410), 0x10000) && !g_fix_kickwait) goto %(lbl)s;%(rest)s " \
        "/* fork_kickwait: NV_PFB_WBC flush, read as done (xemu) */"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    gen, dry = sys.argv[1], "--dry-run" in sys.argv
    total = 0
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", newline="").read()
        sites = WAIT.findall(src)
        done = src.count("&& !g_fix_kickwait) goto")
        total += done
        if done:
            print("%s: %d wait(s) already guarded" % (os.path.basename(path), done))
        if not sites:
            continue
        new = WAIT.sub(lambda m: GUARD % m.groupdict(), src)
        if DECL not in new:
            first = new.index("\nvoid sub_")
            new = new[:first + 1] + DECL + new[first + 1:]
        total += len(sites)
        print("%s: %d wait(s) guarded (%s)" % (os.path.basename(path), len(sites),
                                               ", ".join(s[2] for s in sites)))
        if not dry:
            io.open(path, "w", encoding="utf-8", newline="").write(new)
    if total != 2:
        # SSX has exactly two: 0x16B410 and 0x16B45E. Anything else means the
        # generated code changed shape: stop rather than guess.
        sys.exit("fix_kickwait: expected 2 sites, found %d" % total)
    print("fix_kickwait: %d sites%s" % (total, " (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
