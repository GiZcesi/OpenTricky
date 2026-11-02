#!/usr/bin/env python3
"""Fork pass: the wait for a visibility test's report asleep on the report's status.

The title reads each visibility test (LensFX's sun, the lights' tests) through
Render_ReadVisibilityTestResult (sub_000FBEA6), which rereads the report through
D3DDevice_GetVisibilityTestResult (0x00169010) while it answers "incomplete"
(0x88760828: the status word, +0xC of the 16-byte report, reads 0xFFFFFFFF), at
most 0x100000 times, with nothing in between:

    0x000FBEAE  push edi ; mov edi, 0x100000
    0x000FBEB4  mov ecx, [esi+8] ; push 0 ; lea eax, [esp+0x14] ; push eax ; push ecx ; call 0x00169010
    0x000FBEC4  cmp eax, 0x88760828 ; jne 0x000FBED0
    0x000FBECB  dec edi ; jg 0x000FBEB4

Here the GPU is the pump thread, which writes the status when the test has been
counted (pgraph_d3d11_poll_reports): until then the game thread turns.

This pass keeps the loop, its reads, its bound and its exit test exactly as they
are, and adds a call before the decrement every 1024 rounds, guarded by the
runtime switch g_fix_occlwait (on always; XBOX_FIX_OCCLWAIT=0 with the developer tools, port/src/main.c):

    if (g_fix_occlwait && !(edi & 0x3FFu)) edi = xbox_occl_wait(&MEM32(<status>), edi);

<status> is the word 0x00169010 reads, computed the same way from the test's
handle [esi+8]: MEM32(MEM32(0x1776C0) + (h >> 8) * 4 + 0x48C) + (h & 0xFF) * 16 + 0xC.
xbox_occl_wait (xbox_memory_layout.c) sleeps on that word until the pump writes
it and returns the rounds the bare loop would have left by then, so the bound of
0x100000 rounds is reached when the bare loop reaches it. With the switch at 0 the
loop is the original one.

    fix_occlwait.py GEN_DIR [--dry-run]
Idempotent.
"""
import glob, io, os, sys

FUNC = "void sub_000FBEA6(void)\n{\n"
# The loop, as lifted: the bound, the handle read, the call, the exit test, the decrement.
LOOP = ("loc_000FBEAE: ;\n"
        "    RECOMP_LOC(0x000FBEAE);\n"
        "    PUSH32(esp, edi);\n"
        "    edi = 0x100000;\n"
        "\n"
        "loc_000FBEB4: ;\n"
        "    RECOMP_LOC(0x000FBEB4);\n"
        "    ecx = MEM32(esi + 8);\n"
        "    PUSH32(esp, 0);\n"
        "    eax = esp + 0x14;\n"
        "    PUSH32(esp, eax);\n"
        "    PUSH32(esp, ecx);\n"
        "    g_seh_ebp = ebp; PUSH32(esp, 0); sub_00169010(); /* call 0x00169010 */\n"
        "\n"
        "loc_000FBEC4: ;\n"
        "    RECOMP_LOC(0x000FBEC4);\n"
        "    if (CMP_NE(eax, 0x88760828u)) goto loc_000FBED0; /* jne: not equal / not zero */\n"
        "\n")
SITE = ("loc_000FBECB: ;\n"
        "    RECOMP_LOC(0x000FBECB);\n"
        "    edi--;\n"
        "    if (CMP_G(edi & edi, 0)) goto loc_000FBEB4; /* jg: greater (signed >) */\n")
# The status read of 0x00169010 that <status> mirrors (the handle is its first argument).
CALLEE = ("void sub_00169010(void)\n{\n")
CALLEE_BODY = ("    eax = MEM32(esp + 4);\n"
               "    edx = MEM32(0x1776C0);\n"
               "    ecx = eax;\n"
               "    ecx = ecx >> 8;\n"
               "    eax = eax & 0xFF;\n"
               "    PUSH32(esp, esi);\n"
               "    esi = MEM32(edx + ecx * 4 + 0x48C);\n"
               "    eax = eax << 4;\n"
               "    ecx = MEM32(eax + esi + 0xC);\n"
               "    eax = eax + esi;\n"
               "    (void)0; /* cmp ecx, 0xFFFFFFFFu - flags set for next jcc */\n"
               "    POP32(esp, esi);\n"
               "    if (CMP_NE(ecx, 0xFFFFFFFFu)) { g_seh_ebp = ebp; sub_00169043(); return; } /* jne: not equal / not zero */\n"
               "\n"
               "loc_0016903B: ;\n"
               "    RECOMP_LOC(0x0016903B);\n"
               "    eax = 0x88760828u;\n")
STATUS = "&MEM32(MEM32(MEM32(0x1776C0) + (MEM32(esi + 8) >> 8) * 4 + 0x48C) + ((MEM32(esi + 8) & 0xFF) << 4) + 0xC)"
GUARD = ("loc_000FBECB: ;\n"
         "    RECOMP_LOC(0x000FBECB);\n"
         "    if (g_fix_occlwait && !(edi & 0x3FFu)) edi = xbox_occl_wait(" + STATUS + ", edi);"
         " /* fork_occlwait: asleep on the report's status, the bound kept at the loop's pace */\n"
         "    edi--;\n"
         "    if (CMP_G(edi & edi, 0)) goto loc_000FBEB4; /* jg: greater (signed >) */\n")
DECL = ("extern int g_fix_occlwait; /* fork_occlwait */\n"
        "extern uint32_t xbox_occl_wait(volatile uint32_t *status, uint32_t rounds); /* fork_occlwait */\n")
DONE = "if (g_fix_occlwait && "


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    gen, dry = sys.argv[1], "--dry-run" in sys.argv
    total = callee = 0
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", newline="").read()
        if CALLEE in src:
            c0 = src.index(CALLEE)
            c1 = src.find("\nvoid sub_", c0 + len(CALLEE))
            body = src[c0:c1 if c1 >= 0 else len(src)]
            if body.count(CALLEE_BODY) != 1:
                sys.exit("fix_occlwait: %s: sub_00169010 changed shape (the status read)" % path)
            callee += 1
        done = src.count(DONE)
        total += done
        if done:
            print("%s: %d wait(s) already guarded" % (os.path.basename(path), done))
        if FUNC not in src or done:
            continue
        # Shape checks: the exact loop and site, in the function that holds them,
        # and no other jump to the site's label. Anything else: stop rather than guess.
        if src.count(LOOP + SITE) != 1:
            sys.exit("fix_occlwait: %s: loop at 0x000FBEB4 changed shape" % path)
        if "goto loc_000FBECB;" in src or src.count("goto loc_000FBEB4;") != 1:
            sys.exit("fix_occlwait: %s: the loop's labels have other jumps" % path)
        f0 = src.index(FUNC)
        f1 = src.find("\nvoid sub_", f0 + len(FUNC))
        if not (f0 < src.index(LOOP + SITE) < (f1 if f1 >= 0 else len(src))):
            sys.exit("fix_occlwait: %s: loop not in sub_000FBEA6" % path)
        new = src.replace(LOOP + SITE, LOOP + GUARD)
        if DECL not in new:
            first = new.index("\nvoid sub_")
            new = new[:first + 1] + DECL + new[first + 1:]
        total += 1
        print("%s: 1 wait guarded (loc_000FBECB)" % os.path.basename(path))
        if not dry:
            io.open(path, "w", encoding="utf-8", newline="").write(new)
    if callee != 1:
        sys.exit("fix_occlwait: expected sub_00169010 once, found %d" % callee)
    if total != 1:
        # SSX has exactly one: 0x000FBECB.
        sys.exit("fix_occlwait: expected 1 site, found %d" % total)
    print("fix_occlwait: %d site%s" % (total, " (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
