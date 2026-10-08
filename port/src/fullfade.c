/*
 * fullfade -- full-screen fades reach the edges of the screen (fork).
 * See fullfade.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "recomp/recomp_types.h"
#include "fullfade.h"
#include "pass_tags.h"

void sub_001009D0(void);    /* world-quads node: draw (thiscall node, arg gfx, ret 4) */

#define SAFE_MARGINS    0x001C0840u     /* left, top, +width, +height added by 0xFE940 */
#define DRAW_SKIP       0x001EAC24u     /* bit 1: the method draws nothing */
#define GFX_VIEW        0x196004u       /* gfx: view being drawn */
#define VIEW_FOV        0x80Cu          /* view: field of view, <= 0 = orthographic */
#define GFX_VB_SELECT   0x20u           /* gfx: short, < 0 = the second 2D buffer */
#define VB_2D           0x1580Cu        /* gfx: 2D vertex buffer, per frame buffer */
#define VB_2D_ALT       0x15820u        /* gfx: the second one */
#define GFX_FRAME_BUF   0x196014u       /* gfx: frame buffer index */
#define VTX             0x20u           /* bytes per 2D vertex: x, y, z, w, colour, u, v */
#define EDGE_PAD        4.0f            /* guest pixels past each edge */

int g_fullfade_on = 1;
static unsigned long long s_widened, s_unframed;
static DWORD s_last_tick;

static float rdf(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return f; }
static void  wrf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }

static int is_edge(float v, float a, float b) { return fabsf(v - a) < 0.01f || fabsf(v - b) < 0.01f; }

/* One node of 4 vertices: a full 640x480 quad with one texture coordinate
 * moves to the widened rectangle. */
static int widen(uint32_t q, float l, float t, float r, float b)
{
    float u0 = rdf(q + 0x14u), v0 = rdf(q + 0x18u);
    int k, nx0 = 0, ny0 = 0;
    for (k = 0; k < 4; k++) {
        uint32_t p = q + (uint32_t)k * VTX;
        float x = rdf(p), y = rdf(p + 4u);
        if (!is_edge(x, 0.0f, 640.0f) || !is_edge(y, 0.0f, 480.0f)) return 0;
        if (rdf(p + 0x14u) != u0 || rdf(p + 0x18u) != v0) return 0;
        nx0 += fabsf(x) < 0.01f;
        ny0 += fabsf(y) < 0.01f;
    }
    if (nx0 != 2 || ny0 != 2) return 0;         /* the four corners, not a line */
    for (k = 0; k < 4; k++) {
        uint32_t p = q + (uint32_t)k * VTX;
        wrf(p, fabsf(rdf(p)) < 0.01f ? l : r);
        wrf(p + 4u, fabsf(rdf(p + 4u)) < 0.01f ? t : b);
    }
    s_widened++;
    {   /* one line per run of frames with such a quad (one fade), not per draw */
        DWORD now = GetTickCount();
        if (!s_last_tick || now - s_last_tick > 1000u) {
            fprintf(stderr, "[FULLFADE] full-screen quad widened to %.0f..%.0f x %.0f..%.0f "
                    "(colour %08X, %llu so far, %llu unframed)\n", l, r, t, b, MEM32(q + 0x10u),
                    s_widened, s_unframed);
            fflush(stderr);
        }
        s_last_tick = now;
    }
    return 1;
}

void hook_fullfade_001009D0(void)
{
    uint32_t ecx0 = g_ecx, node = g_ecx, gfx = MEM32(g_esp + 4u), v, vb, data, hops;
    float l, t, r, b;
    int wide = 0;

    if (!gfx || (MEM8(DRAW_SKIP) & 2u)) goto call;
    v = MEM32(gfx + GFX_VIEW);
    if (!v || v == 0xFFFFFFFFu || rdf(v + VIEW_FOV) > 0.0f) goto call;
    vb = MEM32(gfx + ((int16_t)MEM16(gfx + GFX_VB_SELECT) < 0 ? VB_2D_ALT : VB_2D)
               + 4u * MEM32(gfx + GFX_FRAME_BUF));
    if (vb < 0x1000u) goto call;
    data = MEM32(vb + 4u) | 0x80000000u;
    l = rdf(SAFE_MARGINS);
    t = rdf(SAFE_MARGINS + 4u);
    r = 640.0f + l + rdf(SAFE_MARGINS + 8u);
    b = 480.0f + t + rdf(SAFE_MARGINS + 12u);
    if (l > 0.0f) l = 0.0f;
    if (t > 0.0f) t = 0.0f;
    if (r < 640.0f) r = 640.0f;
    if (b < 480.0f) b = 480.0f;
    /* A few pixels past the edges: the translator keeps the NV2A half-pixel
     * viewport offset (0.531) in guest pixels, so at a high resolution an
     * exact quad leaves its first column and row uncovered (3 px at 3440x1440).
     * What lies outside the screen is clipped. */
    l -= EDGE_PAD; t -= EDGE_PAD; r += EDGE_PAD; b += EDGE_PAD;
    for (hops = 0; node && hops < 64u; node = MEM32(node + 4u), hops++) {
        uint32_t first = MEM32(node + 0xCu), q = data + first * VTX;
        if (MEM32(node + 8u) != 4u || first > 0x10000u) continue;
        if (q < 0x80000000u || q + 4u * VTX >= 0x88000000u) continue;
        wide |= widen(q, l, t, r, b);
    }
    /* And drawn stretched, not in the 16:9 / 4:3 frame of the race's
     * menus that its record's tag may have asked for (trick tutorial). */
    if (wide && pass_tags_draw_stretched()) s_unframed++;
call:
    g_ecx = ecx0;               /* the node, for the title's method */
    sub_001009D0();
}

void fullfade_init(void)
{
    const char *e = getenv("XBOX_FULLSCREEN_FADES");
    g_fullfade_on = !(e && (e[0] == '0' || !_stricmp(e, "off") || !_stricmp(e, "xbox")));
    fprintf(stderr, "[FULLFADE] XBOX_FULLSCREEN_FADES=%s\n",
            g_fullfade_on ? "1 (full-screen fades reach the screen edges)" : "0 (like the Xbox)");
}

void (*fullfade_lookup(unsigned int xbox_va))(void)
{
    if (!g_fullfade_on) return 0;
    if (xbox_va == 0x001009D0u) return hook_fullfade_001009D0;
    return 0;
}
