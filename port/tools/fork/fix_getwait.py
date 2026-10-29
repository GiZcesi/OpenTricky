#!/usr/bin/env python3
"""Fork pass: no bare busy wait on the push-buffer GET pointer.

The title's D3D runtime makes room in its push buffer (sub_0016B590, the
"wait for ring-buffer space" of xbox_memory_layout.c) by rereading GET until
the GPU has consumed enough:

    0x0016B644  mov ecx, [edx] ; mov esi, edi ; sub esi, ecx ; cmp eax, esi ; jb 0x0016B644

Here the "GPU" is the PFIFO pump thread, which publishes GET only after it has
translated the batch (ack late). Lifted as is, the loop rereads GET with nothing
in between: at a 60 fps cap it was more than half of the game thread's time, a
whole core kept busy for nothing.

This pass keeps the loop, its reads and its exit test exactly as they are, and
adds a call between two reads, guarded by the runtime switch g_fix_getwait
(XBOX_FIX_GETWAIT, default 1, port/src/main.c):

    if (CMP_B(eax, esi)) { if (g_fix_getwait) xbox_pfifo_get_wait(&MEM32(edx), ecx, _fork_getwait++);
                           goto loc_0016B644; }

xbox_pfifo_get_wait (xbox_memory_layout.c) spins with `pause` for a while, then
sleeps on the GET cell until the pump stores a new value. With the switch at 0
the loop is the original one. Only the time the title takes to see the new GET
changes, not what it sees: the same moment-only change as fix_kickwait.py.

    fix_getwait.py GEN_DIR [--dry-run]
Idempotent.
"""
import glob, io, os, re, sys

FUNC = "void sub_0016B590(void)\n{\n"
LOOP = re.compile(
    r"^(?P<ind>[ \t]*)if \(CMP_B\(eax, esi\)\) goto (?P<lbl>loc_0016B644);(?P<rest>.*)$", re.M)
# The body the guard relies on: GET read into ecx through the pointer in edx.
BODY = ("loc_0016B644: ;\n"
        "    RECOMP_LOC(0x0016B644);\n"
        "    ecx = MEM32(edx);\n"
        "    esi = edi;\n"
        "    esi = esi - ecx;\n"
        "    if (CMP_B(eax, esi)) goto loc_0016B644;")
DECL = ("extern int g_fix_getwait; /* fork_getwait */\n"
        "extern void xbox_pfifo_get_wait(volatile uint32_t *get, uint32_t seen, uint32_t spins);"
        " /* fork_getwait */\n")
VAR = "    uint32_t _fork_getwait = 0; /* fork_getwait: rounds of the GET wait */\n"
GUARD = ("%(ind)sif (CMP_B(eax, esi)) { if (g_fix_getwait) xbox_pfifo_get_wait(&MEM32(edx), ecx, "
         "_fork_getwait++); goto %(lbl)s; }%(rest)s /* fork_getwait: pause, then sleep until the "
         "pump publishes GET */")
DONE = "if (g_fix_getwait) xbox_pfifo_get_wait("


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    gen, dry = sys.argv[1], "--dry-run" in sys.argv
    total = 0
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", newline="").read()
        done = src.count(DONE)
        total += done
        if done:
            print("%s: %d wait(s) already guarded" % (os.path.basename(path), done))
        if FUNC not in src:
            continue
        sites = LOOP.findall(src)
        if not sites:
            continue
        # Shape checks: the exact body, one jump to the label (the loop's own),
        # and the function that holds it. Anything else: stop rather than guess.
        if src.count(BODY) != 1:
            sys.exit("fix_getwait: %s: loop body at 0x0016B644 changed shape" % path)
        if src.count("goto loc_0016B644;") != 1:
            sys.exit("fix_getwait: %s: loc_0016B644 has other jumps" % path)
        f0 = src.index(FUNC)
        f1 = src.find("\nvoid sub_", f0 + len(FUNC))
        if not (f0 < src.index(BODY) < (f1 if f1 >= 0 else len(src))):
            sys.exit("fix_getwait: %s: loop not in sub_0016B590" % path)
        new = LOOP.sub(lambda m: GUARD % m.groupdict(), src)
        cut = new.index(FUNC) + len(FUNC)
        new = new[:cut] + VAR + new[cut:]
        if DECL not in new:
            first = new.index("\nvoid sub_")
            new = new[:first + 1] + DECL + new[first + 1:]
        total += len(sites)
        print("%s: %d wait(s) guarded (%s)" % (os.path.basename(path), len(sites),
                                               ", ".join(s[1] for s in sites)))
        if not dry:
            io.open(path, "w", encoding="utf-8", newline="").write(new)
    if total != 1:
        # SSX has exactly one: 0x0016B644.
        sys.exit("fix_getwait: expected 1 site, found %d" % total)
    print("fix_getwait: %d site%s" % (total, " (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
