/*
 * riderlod -- riders always drawn with their most detailed model set.
 * See riderlod.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include "recomp/recomp_types.h"
#include "riderlod.h"

void sub_0003DD70(void);    /* rider level-of-detail pick (thiscall: ecx = rider, no arg) */

#define RIDER_LOD      0x408u   /* level drawn (read by the rider draw 0x44FD0) */
#define RIDER_LOD_ALT  0x40Cu   /* written with it, read by nothing else */

static int s_max = 1;           /* XBOX_RIDER_LOD_MAX */
static int s_log;               /* XBOX_RIDER_LOD_LOG */
static unsigned s_picks[4];     /* original picks: 0, 1, 2, other */
static uint32_t s_rider[16];    /* log: last original pick per rider */
static int32_t s_last[16];

unsigned d3d8_PresentSeq(void);

void hook_riderlod_0003DD70(void);   /* called by the generated code (fix_manual_hook_calls) */
void hook_riderlod_0003DD70(void)
{
    uint32_t rider = g_ecx;
    sub_0003DD70();
    if (rider < 0x10000u) return;
    if (s_log) {
        int32_t lv = (int32_t)MEM32(rider + RIDER_LOD);
        unsigned n;
        int i;
        s_picks[lv >= 0 && lv <= 2 ? lv : 3]++;
        for (i = 0; i < 16 && s_rider[i] && s_rider[i] != rider; i++) {}
        if (i < 16) {
            if (s_rider[i] == rider && s_last[i] != lv)
                fprintf(stderr, "[RIDERLOD] present %u rider %08X level %d -> %d\n",
                        d3d8_PresentSeq(), rider, (int)s_last[i], (int)lv);
            s_rider[i] = rider; s_last[i] = lv;
        }
        n = s_picks[0] + s_picks[1] + s_picks[2] + s_picks[3];
        if (n % 6000u == 0)
            fprintf(stderr, "[RIDERLOD] original picks: level 0 %u, 1 %u, 2 %u, other %u (max %d)\n",
                    s_picks[0], s_picks[1], s_picks[2], s_picks[3], s_max);
    }
    if (!s_max) return;
    if ((int32_t)MEM32(rider + RIDER_LOD) > 0) MEM32(rider + RIDER_LOD) = 0;
    if ((int32_t)MEM32(rider + RIDER_LOD_ALT) > 0) MEM32(rider + RIDER_LOD_ALT) = 0;
}

void riderlod_set(int on)
{
    s_max = on ? 1 : 0;
}

void riderlod_init(void)
{
    const char *x = getenv("XBOX_RIDER_LOD_MAX");
    s_max = !(x && x[0] == '0');
    x = getenv("XBOX_RIDER_LOD_LOG");
    s_log = x && x[0] == '1';
    fprintf(stderr, "[RIDERLOD] XBOX_RIDER_LOD_MAX=%d\n", s_max);
}

void (*riderlod_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x0003DD70u) return hook_riderlod_0003DD70;   /* direct call: always routed */
    return 0;
}
