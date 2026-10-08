/*
 * perf_hooks -- game thread zones for XBOX_PERF.
 * See xboxrecomp/src/kernel/xbox_perf.h.
 *
 * Main loop 0xAA1A0: N ticks (InGameState vt+0x14 = 0xAD4A0), one render
 * (vt+0x18 = 0xAB610), then the wait on the frame event (0xB2750). Each hook
 * times the call and chains to the next hook of recomp_lookup_manual
 * (ticktrace, then fps_cap) or to the original function. Without
 * XBOX_PERF=1, no hook is handed out.
 */
#include <stdint.h>
#include "recomp/recomp_types.h"
#include "../../xboxrecomp/src/kernel/xbox_perf.h"
#include "ticktrace.h"
#include "fps_cap.h"

void sub_000AD4A0(void);
void sub_000AB610(void);
void sub_000B2750(void);

typedef void (*fn_t)(void);

static fn_t next_hook(unsigned va)
{
    fn_t f = 0;
    if (g_ticktrace_on) f = ticktrace_lookup(va);
    if (!f && g_fps_cap_on) f = fps_cap_lookup(va);
    return f;
}

static int s_nticks;

#define ZONE(z, va, orig)                                   \
    do {                                                    \
        fn_t nx = next_hook(va);                            \
        int prev = perf_zone_enter(z);                      \
        double t0 = perf_now();                             \
        if (nx) nx(); else orig();                          \
        perf_zone_leave(z, prev, perf_now() - t0);          \
    } while (0)

static void hook_tick(void)  { ZONE(PZ_TICK, 0x000AD4A0u, sub_000AD4A0); s_nticks++; }
static void hook_wait(void)  { ZONE(PZ_FWAIT, 0x000B2750u, sub_000B2750); }
static void hook_render(void)
{
    ZONE(PZ_RENDER, 0x000AB610u, sub_000AB610);
    perf_game_frame_end(s_nticks);
    s_nticks = 0;
}

void (*perf_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x000AD4A0u) return hook_tick;
    if (xbox_va == 0x000AB610u) return hook_render;
    if (xbox_va == 0x000B2750u) return hook_wait;
    return 0;
}
