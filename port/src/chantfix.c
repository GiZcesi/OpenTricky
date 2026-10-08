/*
 * chantfix -- crowd chant names one past the end of each list. See chantfix.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include "recomp/recomp_types.h"
#include "chantfix.h"

void sub_0011EE20(void);    /* chant.inf reader (thiscall: ecx = manager, 1 arg, ret 4) */

#define CHANT_NAME       0x40u      /* bytes per name */
#define CHANT_SLOTS      20u        /* slots per list */
#define CHANT_CHARS      12u        /* characters with their own list */
#define CHANT_CHAR_BASE  0x0004u    /* first character list */
#define CHANT_GEN_BASE   0x3C04u    /* the general list */
#define CHANT_CHAR_COUNT 0x4104u    /* CHARCHANTBANKS */
#define CHANT_GEN_COUNT  0x4108u    /* GENCHANTBANKS */

static int s_fix = -1;              /* XBOX_FIX_CHANT: 1 fill slot `count`, 0 original data */

/* A name with its terminating zero inside the slot. */
static int name_ok(uint32_t va)
{
    uint32_t k;
    for (k = 0; k < CHANT_NAME; k++)
        if (!MEM8(va + k)) return k > 0;
    return 0;
}

/* Slot `count` of the list at `base` gets slot `count - 1`, when it is one of
 * the list's own slots and holds no terminated name. Returns 1 if written. */
static int fill_one_past(uint32_t base, uint32_t count)
{
    uint32_t last, past, k;
    if (count == 0u || count >= CHANT_SLOTS) return 0;
    last = base + (count - 1u) * CHANT_NAME;
    past = base + count * CHANT_NAME;
    if (!name_ok(last) || name_ok(past)) return 0;
    for (k = 0; k < CHANT_NAME; k += 4) MEM32(past + k) = MEM32(last + k);
    return 1;
}

void hook_chant_0011EE20(void);   /* called by the generated code (fix_manual_hook_calls) */
void hook_chant_0011EE20(void)
{
    uint32_t mgr = g_ecx, c, n = 0;
    sub_0011EE20();
    if (s_fix != 1 || mgr < 0x1000u) return;
    {
        uint32_t keep_eax = g_eax;          /* the reader's result, untouched */
        uint32_t ccount = MEM32(mgr + CHANT_CHAR_COUNT), gcount = MEM32(mgr + CHANT_GEN_COUNT);
        for (c = 0; c < CHANT_CHARS; c++)
            n += (uint32_t)fill_one_past(mgr + CHANT_CHAR_BASE + c * CHANT_SLOTS * CHANT_NAME, ccount);
        n += (uint32_t)fill_one_past(mgr + CHANT_GEN_BASE, gcount);
        g_eax = keep_eax;
        fprintf(stderr, "[CHANTFIX] chant names: %u lists closed (character banks %u, general banks %u)\n",
                n, ccount, gcount);
    }
}

void chantfix_init(void)
{
    const char *x = getenv("XBOX_FIX_CHANT");
    s_fix = !x || !x[0] ? 1 : (x[0] == '0' ? 0 : 1);
    fprintf(stderr, "[CHANTFIX] XBOX_FIX_CHANT=%d\n", s_fix);
}

void (*chantfix_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x0011EE20u) return hook_chant_0011EE20;   /* direct call: always routed */
    return 0;
}
