/* pacing.c -- see pacing.h. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>
#include "recomp/recomp_types.h"
#include "pacing.h"

extern void sub_000AC9B0(void);     /* InGameState_LoadLevel (thiscall) */

static double cfg_min_load_ms(void)
{
    static double v = -1.0;
    if (v < 0.0) { const char *e = getenv("OT_MIN_LOAD_MS"); v = (e && e[0]) ? atof(e) : 0.0; if (v < 0.0) v = 0.0; if (v > 30000.0) v = 30000.0; }
    return v;
}

static double cfg_slow(void)
{
    static double v = -1.0;
    if (v < 0.0) { const char *e = getenv("OT_COUNTDOWN_SLOW"); v = (e && e[0]) ? atof(e) : 1.0; if (!(v >= 1.0)) v = 1.0; if (v > 4.0) v = 4.0; }
    return v;
}

int ot_race_state(void)
{
    uint32_t app = MEM32(0x001E3C7Cu), lvl, race, st;
    if (app < 0x1000u || (lvl = MEM32(app + 0x72Cu)) < 0x1000u) return -1;
    race = MEM32(lvl + 0x1Cu);
    if (race < 0x1000u) return -1;
    st = MEM32(race + 0x1Cu);
    return st > 0xFFFFu ? 0xFFFF : (int)st;
}

double pacing_tick_scale(void)      /* called from the 60 Hz tick thread */
{
    static int last = -2, shown;
    static double t0;
    int st;
    if (!g_xbox_mem_offset) return 1.0;      /* the guest window is not mapped yet (this is also called before main): nothing to read */
    st = ot_race_state();
    if (st != last && shown < 60) {        /* a log of the race states with wall-clock times: how long each lasted */
        LARGE_INTEGER f, c; double now;
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
        now = (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
        if (last != -2) fprintf(stderr, "[PACING] race state %d -> %d after %.0f ms\n", last, st, now - t0);
        t0 = now; last = st; shown++;
    }
    double sl = cfg_slow();
    return (sl > 1.0 && st == 3) ? sl : 1.0;
}

static void hook_AC9B0(void)
{
    uint32_t ecx = g_ecx;
    LARGE_INTEGER f, a, b;
    double ms;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    g_ecx = ecx;
    sub_000AC9B0();
    QueryPerformanceCounter(&b);
    ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    {
        static int told;
        double total = ms < cfg_min_load_ms() ? cfg_min_load_ms() : ms;
        if (told++ < 20) fprintf(stderr, "[PACING] level load took %.0f ms, screen held for %.0f ms in all (minimum %.0f)\n", ms, total, cfg_min_load_ms());
    }
    if (ms < cfg_min_load_ms()) Sleep((DWORD)(cfg_min_load_ms() - ms));
}

void (*pacing_lookup(uint32_t xbox_va))(void)
{
    if (xbox_va == 0x000AC9B0u && cfg_min_load_ms() > 0.0 && !getenv("XBOX_NET")) return hook_AC9B0;     /* the netplay prototype hooks the same function */
    return 0;
}

/* the guest clocks follow the countdown slowdown (xbox_guest_clock_adjust in kernel_hal.c) */
extern double (*xbox_clock_scale_hook)(void);
__attribute__((constructor)) static void pacing_install_clock_hook(void) { if (cfg_slow() > 1.0) xbox_clock_scale_hook = pacing_tick_scale; }     /* only when the option is on: otherwise the clocks are untouched */
