/*
 * aspect -- display shapes wider than 16:9 (fork). See aspect.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "recomp/recomp_types.h"
#include "aspect.h"

void sub_000F9EA0(void);    /* Renderer_SetScreenMode (thiscall, ret 4) */
void sub_0017770D(void);    /* Matrix_BuildPerspectiveProjection (cdecl, ret 20) */
void d3d8_SetPointZoom(float zoom);     /* d3d8_xbox.h */

int g_aspect_hook_on;
int g_aspect_fov = ASPECT_FOV_NOSTRETCH;
static uint32_t s_xscale_bits;      /* (4/3) / A as a float */

static const char *const k_fov_name[ASPECT_FOV_COUNT] = { "NoStretch", "Balanced", "Full" };

const char *aspect_fov_name(int mode)
{
    return (mode >= 0 && mode < ASPECT_FOV_COUNT) ? k_fov_name[mode] : k_fov_name[0];
}

int aspect_fov_parse(const char *s, int fallback)
{
    int i;
    if (!s || !s[0]) return fallback;
    for (i = 0; i < ASPECT_FOV_COUNT; i++)
        if (!_stricmp(s, k_fov_name[i])) return i;
    if (!_stricmp(s, "hor+")) return ASPECT_FOV_FULL;
    return fallback;
}

void aspect_set_fov(int chosen)
{
    g_aspect_fov = aspect_fov_parse(getenv("XBOX_WIDE_FOV"), chosen);
    if (g_aspect_fov < 0 || g_aspect_fov >= ASPECT_FOV_COUNT) g_aspect_fov = ASPECT_FOV_NOSTRETCH;
}

int aspect_init(double a)
{
    float k;
    if (!(a > 4.0 / 3.0 + 0.005)) return 0;             /* 4:3 */
    if (fabs(a - 16.0 / 9.0) < 0.005) return 1;          /* 16:9: the title's own 0.75 */
    k = (float)((4.0 / 3.0) / a);
    memcpy(&s_xscale_bits, &k, 4);
    g_aspect_hook_on = 1;
    printf("Aspect:     %.4f, renderer x-scale %.4f, field of view %s\n",
           a, k, aspect_fov_name(g_aspect_fov));
    return 1;
}

double aspect_from_env(unsigned render_w, unsigned render_h)
{
    const char *e = getenv("XBOX_ASPECT");
    unsigned a, b;
    double v;
    if (!e || !e[0]) return 0.0;
    if (!_stricmp(e, "auto"))
        return (render_w && render_h) ? (double)render_w / (double)render_h : 0.0;
    if (sscanf(e, "%u:%u", &a, &b) == 2 && a && b) {
        /* 21:9 and 32:9 are names more than ratios: the resolution's own
         * shape when it is close (3440x1440 is 2.389, 2560x1080 2.370). */
        v = (double)a / (double)b;
        if (render_w && render_h && fabs((double)render_w / render_h - v) < 0.1)
            v = (double)render_w / (double)render_h;
        return v;
    }
    v = atof(e);
    return v > 1.0 ? v : 0.0;
}

/* Renderer_SetScreenMode(mode): mode 2 (widescreen) gets the wider x-scale.
 * The title's body runs first, unchanged; ecx is left as it leaves it. */
void hook_aspect_000F9EA0(void)
{
    uint32_t self = g_ecx, mode = MEM32(g_esp + 4u);
    sub_000F9EA0();
    if (mode == 2u && g_aspect_hook_on) MEM32(self + 0x53Cu) = s_xscale_bits;
}

/* Matrix_BuildPerspectiveProjection(out, fov, aspect, near, far), fov the
 * full vertical angle in radians. Only an aspect wider than 16:9 is touched
 * (the views the x-scale above widened): the fov argument is narrowed in
 * place, then the title's body builds the matrix. Pass 13 routes the direct
 * calls here too, so with the hook off this is a plain call. */
void hook_fov_0017770D(void)
{
    float fov = MEMF(g_esp + 8u), a = MEMF(g_esp + 12u);
    if (g_aspect_hook_on && g_aspect_fov != ASPECT_FOV_FULL &&
        a > 16.0f / 9.0f + 0.01f && fov > 0.0f && fov < 3.1f) {
        double t = tan(0.5 * (double)fov), t2;
        if (g_aspect_fov == ASPECT_FOV_NOSTRETCH) {
            t2 = t * (16.0 / 9.0) / a;      /* the 16:9 horizontal tangent */
        } else {
            double h = 0.5 * (atan(t * 16.0 / 9.0) + atan(t * a));
            t2 = tan(h) / a;                /* horizontal half-angle halfway */
        }
        MEMF(g_esp + 8u) = (float)(2.0 * atan(t2));
        /* The title sizes particles from their distance only: scale them
         * with the scene. The views here all share the screen's
         * aspect; the last one built is the one being drawn. */
        d3d8_SetPointZoom((float)(t / t2));
    }
    sub_0017770D();
}

void (*aspect_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x000F9EA0u) return hook_aspect_000F9EA0;
    if (xbox_va == 0x0017770Du) return hook_fov_0017770D;
    return 0;
}
