/*
 * strmwalk -- bounded block search in the EA stream reader. See strmwalk.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include "recomp/recomp_types.h"
#include "strmwalk.h"

/* The hook below is the translated body line for line: same register names. */
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi

void sub_0014F197(void);    /* release current block, tail (translated body) */
void sub_00164300(void);    /* RtlEnterCriticalSection wrapper (arg on the stack, caller pops) */
void sub_00164310(void);    /* RtlLeaveCriticalSection wrapper (same) */

#define STRM_RING_LO    0x38u   /* STRM: first byte of the ring allocation */
#define STRM_BASE       0x3Cu   /* ... where the walk restarts after a 0xFFFFFFFF block */
#define STRM_RING_HI    0x40u   /* ... end of the ring */
#define STRM_TAIL       0x5Cu   /* ... oldest block not yet released (log only) */
#define STRM_PARSE      0x60u   /* ... parse pointer: blocks below it are tagged and counted */
#define STRM_READ       0x64u   /* ... end of the data read (log only) */

static int s_fix = -1;          /* XBOX_FIX_STRMWALK: 0 original, 1 ring + parse pointer, 2 ring only */
static int s_lock = -1;         /* XBOX_FIX_STRMLOCK: 1 wait for the reader before reading [H+0xC], 0 original order */

/* The reader (0x14EAC0, file thread) adds a block's size to [H+8] and, when
 * the channel was empty, then stores the block in [H+0xC] -- both inside the
 * STRM lock (S+4). 0x14F170 reads [H+8] without the lock and 0x14F197 reads
 * [H+0xC] right after, still without it. On the one-core Xbox the main thread
 * could never run between the reader's two stores; on several cores it can,
 * and then sees the new count with the old block: 0xDEADC0ED for a fresh
 * handle, or the block it has just consumed, which it then consumes a second
 * time (the count drifts below zero, then wraps: the end-of-video freeze).
 * Taking and releasing that lock here waits for a reader still between its
 * two stores, so [H+0xC] matches the count 0x14F170 saw. The lock is the
 * guest's own (host CRITICAL_SECTION, recursive) and 0x14F197 takes it a few
 * instructions later anyway: no new lock, no new lock order. ebx (handle) and
 * the stack are left as they were; eax/ecx/edx are not live at 0x14F197. */
static void strm_lock_barrier(uint32_t strm)
{
    PUSH32(esp, strm + 4);
    g_seh_ebp = strm; PUSH32(esp, 0); sub_00164300();
    PUSH32(esp, strm + 4);
    g_seh_ebp = strm; PUSH32(esp, 0); sub_00164310();
    esp = esp + 8;
    g_seh_ebp = strm;
}
static volatile LONG s_giveups;

static void giveup_log(int why, uint32_t h, uint32_t s, uint32_t count, uint32_t start,
                       uint32_t at, uint32_t steps, uint32_t lo, uint32_t hi)
{
    static const char *const reason[] = { "?", "left the ring", "block of size 0", "second wrap", "more steps than the ring holds",
                                          "reached the parse pointer" };
    LONG n = InterlockedIncrement(&s_giveups);
    if (n <= 16 || (n & 255) == 0)
        fprintf(stderr, "[STRMWALK] #%ld t%lu walk given up (%s): handle 0x%08X channel %u pending 0x%08X -> 0, "
                "start 0x%08X, at 0x%08X after %u steps; ring 0x%08X..0x%08X tail 0x%08X parse 0x%08X read 0x%08X\n",
                (long)n, (unsigned long)GetCurrentThreadId(), reason[why], h, MEM32(h + 4u), count, start, at, steps,
                lo, hi, MEM32(s + STRM_TAIL), MEM32(s + STRM_PARSE), MEM32(s + STRM_READ));
}

static volatile LONG s_notyet;

static void notyet_log(uint32_t h, uint32_t s, uint32_t blk)
{
    LONG n = InterlockedIncrement(&s_notyet);
    if (n <= 16 || (n & 255) == 0)
        fprintf(stderr, "[STRMWALK] not yet #%ld t%lu: handle 0x%08X channel %u pending 0x%08X, current block 0x%08X "
                "outside the ring 0x%08X..0x%08X -> no block\n", (long)n, (unsigned long)GetCurrentThreadId(), h,
                MEM32(h + 4u), MEM32(h + 8u), blk, MEM32(s + STRM_RING_LO), MEM32(s + STRM_RING_HI));
}

/* 0x14F197, reached by a tail jump from 0x14F170 (ebx = handle; on the stack:
 * the caller's ebp, ebx, ecx slot holding the STRM, return address, handle).
 * The translated body, line for line, plus the bounds of the walk. */
void hook_strm_0014F197(void)
{
    uint32_t ebp, s, lo, hi, steps = 0, max_steps, wraps = 0;
    int why = 0;

    ebp = g_seh_ebp; /* inherit caller's frame (ebp = STRM) */
    if (s_lock > 0) strm_lock_barrier(ebp);
    if (s_fix <= 0) { sub_0014F197(); return; }

    eax = MEM32(ebx + 0xC);
    /* The reader counts a channel's first block ([H+8] += size) and only then
     * stores it as the current block ([H+0xC]), both under the STRM lock, but
     * 0x14F170 reads [H+8] without the lock: on several cores it can see the
     * count before the block (seen: [H+0xC] still 0xDEADC0ED, the fill of a
     * new handle). Not a block yet: answer "no block", as 0x14F170 does when
     * the count is 0, and touch nothing -- the caller asks again. */
    if (eax < MEM32(ebp + STRM_RING_LO) || eax > MEM32(ebp + STRM_RING_HI) - 8u) {
        notyet_log(ebx, ebp, eax);
        POP32(esp, ebp);
        eax = 0;
        POP32(esp, ebx);
        POP32(esp, ecx);
        esp += 4; return; /* ret */
    }
    PUSH32(esp, esi);
    esi = MEM32(eax + 4);
    PUSH32(esp, edi);
    ebp = ebp + 4;                      /* the STRM's critical section */
    esi = esi & 0xFFFFFF;
    PUSH32(esp, ebp);
    MEM32(esp + 0x1C) = eax;            /* start block, in the handle argument slot */
    MEM32(eax + 4) = esi;               /* clear its channel tag */
    g_seh_ebp = ebp; PUSH32(esp, 0); sub_00164300();

    edi = MEM32(ebx + 8);
    edi = edi - esi;
    PUSH32(esp, ebp);
    MEM32(ebx + 8) = edi;
    g_seh_ebp = ebp; PUSH32(esp, 0); sub_00164310();

    esp = esp + 8;
    if (CMP_LE(edi & edi, 0)) goto done;

    s = ebp - 4;
    lo = MEM32(s + STRM_RING_LO);
    hi = MEM32(s + STRM_RING_HI);
    max_steps = hi > lo ? (hi - lo) / 8u : 0;

    eax = MEM32(esp + 0x18);
    edx = MEM32(ebx + 4);
    ecx = esi + eax;
    if (ecx < lo || ecx > hi - 8u) { why = 1; goto giveup; }
    if (s_fix == 1 && ecx == MEM32(s + STRM_PARSE)) { why = 5; goto giveup; }
    eax = MEM32(ecx + 4);
    esi = eax;
    edx = edx << 0x18;
    esi = esi & 0xFF000000u;
    if (CMP_EQ(esi, edx)) goto found;

next:
    if (++steps > max_steps) { why = 4; goto giveup; }
    if (CMP_NE(MEM32(ecx), 0xFFFFFFFFu)) goto step;
    if (++wraps > 1) { why = 3; goto giveup; }
    ecx = MEM32(esp + 0x10);
    ecx = MEM32(ecx + 0x3C);
    goto check;

step:
    eax = eax & 0xFFFFFF;
    if (eax == 0) { why = 2; goto giveup; }
    ecx = ecx + eax;

check:
    if (ecx < lo || ecx > hi - 8u) { why = 1; goto giveup; }
    if (s_fix == 1 && ecx == MEM32(s + STRM_PARSE)) { why = 5; goto giveup; }
    eax = MEM32(ecx + 4);
    esi = eax;
    esi = esi & 0xFF000000u;
    if (CMP_NE(esi, edx)) goto next;

found:
    MEM32(ebx + 0xC) = ecx;
    goto done;

giveup:
    giveup_log(why, ebx, s, MEM32(ebx + 8), MEM32(esp + 0x18), ecx, steps, lo, hi);
    PUSH32(esp, ebp);
    g_seh_ebp = ebp; PUSH32(esp, 0); sub_00164300();
    MEM32(ebx + 8) = 0;
    PUSH32(esp, ebp);
    g_seh_ebp = ebp; PUSH32(esp, 0); sub_00164310();
    esp = esp + 8;

done:
    eax = MEM32(esp + 0x18);
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ebp);
    POP32(esp, ebx);
    POP32(esp, ecx);
    esp += 4; return; /* ret */
}

void strmwalk_init(void)
{
    const char *x = getenv("XBOX_FIX_STRMWALK");
    s_fix = !x || !x[0] ? 2 : (x[0] == '0' ? 0 : x[0] == '1' ? 1 : 2);
    x = getenv("XBOX_FIX_STRMLOCK");
    s_lock = !x || !x[0] ? 1 : (x[0] == '0' ? 0 : 1);
    fprintf(stderr, "[STRMWALK] XBOX_FIX_STRMWALK=%d XBOX_FIX_STRMLOCK=%d\n", s_fix, s_lock);
}

void (*strmwalk_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x0014F197u) return hook_strm_0014F197;   /* direct tail call: always routed */
    return 0;
}
