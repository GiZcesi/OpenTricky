/*
 * weather -- Graphics.Weather (XBOX_WEATHER): the snowfall without the fog
 * the Xbox version adds to it, as in the PS2 version. See weather.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "recomp/recomp_types.h"
#include "weather.h"

void sub_000FD3E0(void);    /* per-track fog and light setup (thiscall: ecx = gfx, no arg) */
void sub_000DEE40(void);    /* SnowFallMan_RenderSnowfall (thiscall: ecx = snowfall, no arg) */

#define GAME_PTR      0x1E3C7Cu   /* the game object; +0x72C = the level */
#define GAME_LEVEL    0x72Cu
#define LVL_TRACK     0x2Cu       /* TrackInfo; +0x54 = its weather chance p */
#define TRACK_CHANCE  0x54u
#define LVL_WEATHER   0x78u       /* byte: the weather drawn at load */
#define LVL_SNOW      0x7Cu       /* float: the snow amount drawn at load (logged) */
#define TRACK_INDEX   0x1DEC90u
#define GAME_MODE     0x1DEC94u

static int s_ps2;           /* XBOX_WEATHER=ps2 */
static int s_noted;         /* ps2: a snowfall left out was logged since the last fog setup */

static uint32_t level(void)
{
    uint32_t g = MEM32(GAME_PTR);
    return g ? MEM32(g + GAME_LEVEL) : 0u;
}

static float track_chance(uint32_t lvl)
{
    uint32_t t = MEM32(lvl + LVL_TRACK);
    return t ? MEMF(t + TRACK_CHANCE) : 0.0f;
}

/* 0xFD3E0, the fog and light setup of the level (thiscall, no argument, ret),
 * after LoadLevel has drawn the weather and the snow amount. PS2: run with
 * the weather byte read as 0 -- the title's clear-weather values -- then the
 * byte is put back for the snowfall. */
void hook_weather_000FD3E0(void);   /* called by the generated code (fix_manual_hook_calls) */
void hook_weather_000FD3E0(void)
{
    uint32_t gfx = g_ecx, lvl = level();
    uint8_t w = lvl ? MEM8(lvl + LVL_WEATHER) : 0;
    if (!s_ps2 || !w) {
        sub_000FD3E0();
    } else {
        MEM8(lvl + LVL_WEATHER) = 0;
        sub_000FD3E0();
        MEM8(lvl + LVL_WEATHER) = w;
    }
    s_noted = 0;
    if (w && gfx >= 0x10000u) {
        float p = track_chance(lvl);
        fprintf(stderr, "[WEATHER] track %u mode %u: weather on (chance %.2f), snow %.4f (%s); fog density %.3g,"
                " shadow direction (%g, %g, %g), w %g\n",
                (unsigned)MEM32(TRACK_INDEX), (unsigned)MEM32(GAME_MODE), (double)p, (double)MEMF(lvl + LVL_SNOW),
                !s_ps2 ? "original" : p > 0.0f ? "ps2" : "ps2: no snow, an Xbox extra draw",
                (double)MEMF(gfx + 0x15934u), (double)MEMF(0x1C0AA0u), (double)MEMF(0x1C0AA4u),
                (double)MEMF(0x1C0AA8u), (double)MEMF(0x1C0954u));
    }
}

/* 0xDEE40, the snowfall render (thiscall, no argument, ret), handed out with
 * ps2 only. On a track whose own chance is 0: the title's no-weather path
 * (it returns at once). */
void hook_weather_000DEE40(void);
void hook_weather_000DEE40(void)
{
    uint32_t lvl = level();
    uint8_t w = lvl ? MEM8(lvl + LVL_WEATHER) : 0;
    if (!s_ps2 || !w || track_chance(lvl) != 0.0f) {
        sub_000DEE40();
        return;
    }
    if (!s_noted) {
        s_noted = 1;
        fprintf(stderr, "[WEATHER] track %u: snowfall left out (ps2: chance 0, an Xbox extra draw)\n",
                (unsigned)MEM32(TRACK_INDEX));
    }
    MEM8(lvl + LVL_WEATHER) = 0;
    sub_000DEE40();
    MEM8(lvl + LVL_WEATHER) = w;
}

int weather_ps2(void)
{
    return s_ps2;
}

void weather_init(void)
{
    const char *x = getenv("XBOX_WEATHER");
    s_ps2 = x && !strcmp(x, "ps2");
    fprintf(stderr, "[WEATHER] XBOX_WEATHER=%s\n", s_ps2 ? "ps2" : "original");
}

void (*weather_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x000FD3E0u) return hook_weather_000FD3E0;   /* direct calls: always routed */
    if (s_ps2 && xbox_va == 0x000DEE40u) return hook_weather_000DEE40;   /* virtual call: ps2 only */
    return 0;
}
