#!/usr/bin/env python3
"""Fork pass: bounded walk of the rider lists in sub_0002F800 (rank catch-up).

Every 6 ticks of a race, sub_0002F800(race) sorts the riders and splits them on
rider+0x44C (1 = human Player, 0 = AI) into two lists on its stack frame:

    esp+0xCC  Player list (4 slots)      esp+0xDC  AI list (6 slots)

then walks the AI list up to [race+0x80] (the AI count fixed when the race was
built, 5 in a 1-player race) and, for each AI, takes its target from the
Player list (pointer [esp+0x38], from esp+0xCC, one slot per [race+0x88] /
[race+0x7C] AIs), without checking how many entries either list holds:

    0x0002FD50  mov edx, [esp+ebx*4+0xDC]       ; AI list slot ebx
                mov ecx, [edi+edx*4+0xC4]       ; race->rider[edx]
                mov eax, [esp+0x38]
                mov eax, [eax]                  ; Player list slot
                mov esi, [edi+eax*4+0xC4]       ; race->rider[eax]
                mov [ecx+0x130], eax            ; AI target = that index

If, at that call, the flags disagree with the counts the race was built with,
the walk reads a slot it never wrote: leftover stack bytes (0x3F800000 in the
crash seen on the AI side), so the rider index is ~1e9 and the read lands
far from the race object: access violation (one Garibaldi run), or, with the RAM mirrors
mapped, a wild read and a wrong target. AI side: fewer riders with flag 0 than
[race+0x80] (seen once). Player side: fewer riders with flag 1 than
[race+0x7C] (never seen). The original code has the same unchecked
walk; why the counts disagreed in that run is not known.

With the runtime switch g_fix_riderlist (XBOX_FIX_RIDERLIST, default 1,
port/src/main.c), this pass:
  1. fills the 6 AI-list and the 4 Player-list slots with 0xFFFFFFFF before the
     split (0x0002FB83), so an unwritten slot is recognisable instead of being
     stack garbage;
  2. after the split (0x0002FBC5) stores how many entries each list received
     (g_riderlist_ai_n / g_riderlist_pl_n) and, if they differ from
     [race+0x80] / [race+0x7C], reports it (fork_riderlist_note, kind 0);
  3. at the top of the walk body (0x0002FD50, before any x87 push), leaves
     through the function's own epilogue (loc_0002FFA0) when the AI slot or the
     Player slot does not hold a valid rider index (>= [race+0x88]), and reports
     it (kind 1 = AI side, 2 = Player side).
fork_riderlist_note (main.c) writes one "[RIDERLIST] ..." line on stderr per
kind and per race, so the "stable" campaign sees it in run.log.
When the counts agree (every run measured) nothing changes: the slots filled
are all overwritten before they are read, no report is made.
0 = the original code (no fill, no count, no guard, no report).

    fix_riderlist.py GEN_DIR [--dry-run]
Idempotent.
"""
import glob, io, os, sys

TAG = "/* fork_riderlist */"
V2 = "fork_riderlist_note"   # marker of this version; the first one had TAG only
DECL = ("extern int g_fix_riderlist; extern uint32_t g_riderlist_ai_n, g_riderlist_pl_n; "
        "void fork_riderlist_note(uint32_t kind, uint32_t race, uint32_t tick, uint32_t riders, "
        "uint32_t ai_exp, uint32_t pl_exp, uint32_t at); " + TAG + "\n")

FILL_AT = ("loc_0002FB83: ;\n    RECOMP_LOC(0x0002FB83);\n")
FILL = ("    if (g_fix_riderlist) { MEM32(esp + 0xDC) = 0xFFFFFFFFu; MEM32(esp + 0xE0) = 0xFFFFFFFFu; "
        "MEM32(esp + 0xE4) = 0xFFFFFFFFu; MEM32(esp + 0xE8) = 0xFFFFFFFFu; MEM32(esp + 0xEC) = 0xFFFFFFFFu; "
        "MEM32(esp + 0xF0) = 0xFFFFFFFFu; "
        "MEM32(esp + 0xCC) = 0xFFFFFFFFu; MEM32(esp + 0xD0) = 0xFFFFFFFFu; MEM32(esp + 0xD4) = 0xFFFFFFFFu; "
        "MEM32(esp + 0xD8) = 0xFFFFFFFFu; } " + TAG + " /* AI and Player lists: unwritten slots recognisable */\n")

# eax = [race+0x88] here; the split loop ran iff eax > 0, leaving ebx / esi one past
# the last AI / Player entry written.
COUNT_AT = ("loc_0002FBC5: ;\n    RECOMP_LOC(0x0002FBC5);\n")
COUNT = ("    if (g_fix_riderlist && (int32_t)eax > 0) { g_riderlist_ai_n = (ebx - (esp + 0xDC)) >> 2; "
         "g_riderlist_pl_n = (esi - (esp + 0xCC)) >> 2; "
         "if (g_riderlist_ai_n != MEM32(edi + 0x80) || g_riderlist_pl_n != MEM32(edi + 0x7C)) "
         "fork_riderlist_note(0, edi, MEM32(edi + 0x18), eax, MEM32(edi + 0x80), MEM32(edi + 0x7C), 0); } "
         + TAG + " /* list sizes vs the race's counts */\n")

GUARD_AT = ("loc_0002FD50: ;\n    RECOMP_LOC(0x0002FD50);\n")
GUARD = ("    if (g_fix_riderlist && (MEM32(esp + ebx * 4 + 0xDC) >= MEM32(edi + 0x88) || "
         "MEM32(MEM32(esp + 0x38)) >= MEM32(edi + 0x88))) { "
         "fork_riderlist_note(MEM32(esp + ebx * 4 + 0xDC) >= MEM32(edi + 0x88) ? 1u : 2u, edi, MEM32(edi + 0x18), "
         "MEM32(edi + 0x88), MEM32(edi + 0x80), MEM32(edi + 0x7C), ebx); goto loc_0002FFA0; } " + TAG +
         " /* AI or Player list shorter than the race's counts: stop */\n")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    gen, dry = sys.argv[1], "--dry-run" in sys.argv
    hits = []
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        src = io.open(path, encoding="utf-8", newline="").read()
        if "void sub_0002F800(void)" not in src:
            continue
        hits.append(path)
        if TAG in src:
            if V2 not in src:
                sys.exit("fix_riderlist: %s holds an older version of this pass: regenerate gen/" % path)
            print("%s: already patched" % os.path.basename(path))
            continue
        nl = "\r\n" if "\r\n" in src else "\n"
        fill_at, count_at, guard_at = (s.replace("\n", nl) for s in (FILL_AT, COUNT_AT, GUARD_AT))
        if (src.count(fill_at) != 1 or src.count(count_at) != 1 or src.count(guard_at) != 1
                or "loc_0002FFA0: ;" not in src):
            sys.exit("fix_riderlist: sub_0002F800 changed shape in %s: stop rather than guess" % path)
        new = src.replace(fill_at, fill_at + FILL.replace("\n", nl), 1)
        new = new.replace(count_at, count_at + COUNT.replace("\n", nl), 1)
        new = new.replace(guard_at, guard_at + GUARD.replace("\n", nl), 1)
        first = new.index("\nvoid sub_")
        new = new[:first + 1] + DECL.replace("\n", nl) + new[first + 1:]
        print("%s: list fill at 0x2FB83, sizes at 0x2FBC5, bound at 0x2FD50 (AI + Player)" % os.path.basename(path))
        if not dry:
            io.open(path, "w", encoding="utf-8", newline="").write(new)
    if len(hits) != 1:
        sys.exit("fix_riderlist: expected sub_0002F800 in exactly 1 file, found %d" % len(hits))
    print("fix_riderlist: 3 sites%s" % (" (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
