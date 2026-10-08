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
void sub_00075DE0(void);    /* Camera_ComputeDesiredEye (thiscall, ret 0x10) */
void sub_000759F0(void);    /* camera collision check (cdecl: eye*, pivot*, k) */
void sub_00078DE0(void);    /* Camera_Update (thiscall, ret) */
void d3d8_SetPointZoom(float zoom);     /* d3d8_xbox.h */

int g_aspect_hook_on;
int g_aspect_fov = ASPECT_FOV_NOSTRETCH;
int g_aspect_camera = 1;
int g_aspect_menus43 = ASPECT_MENUS_DEFAULT == ASPECT_MENUS_43;   /* aspect_set_menus */
static uint32_t s_xscale_bits;      /* (4/3) / A as a float */
static uint32_t s_renderer;         /* Renderer_SetScreenMode's this, in mode 2 */
static int      s_boxed;            /* this frame shows the title's own 16:9 (aspect_frame) */
static float    s_cam_zoom = 1.0f;  /* A / (16/9) in NoStretch, else 1 */
static int      s_cam_log;         /* XBOX_WIDE_CAMERA_LOG=1 */

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

static const char *const k_menus_name[ASPECT_MENUS_COUNT] = { "16:9", "4:3" };

const char *aspect_menus_name(int mode)
{
    return (mode >= 0 && mode < ASPECT_MENUS_COUNT) ? k_menus_name[mode] : k_menus_name[ASPECT_MENUS_DEFAULT];
}

int aspect_menus_parse(const char *s, int fallback)
{
    if (!s || !s[0]) return fallback;
    if (!strcmp(s, "43") || !strcmp(s, "4:3") || !_stricmp(s, "original")) return ASPECT_MENUS_43;
    if (!strcmp(s, "169") || !strcmp(s, "16:9") || !_stricmp(s, "xbox")) return ASPECT_MENUS_169;
    if (!strcmp(s, "0")) return ASPECT_MENUS_169;       /* no frame (hud_anchor.c): as before */
    return fallback;
}

void aspect_set_menus(int chosen)
{
    int m = aspect_menus_parse(getenv("XBOX_WIDE_MENUS"), chosen);
    g_aspect_menus43 = m == ASPECT_MENUS_43;
}

void aspect_set_fov(int chosen)
{
    g_aspect_fov = aspect_fov_parse(getenv("XBOX_WIDE_FOV"), chosen);
    if (g_aspect_fov < 0 || g_aspect_fov >= ASPECT_FOV_COUNT) g_aspect_fov = ASPECT_FOV_NOSTRETCH;
}

int aspect_init(double a)
{
    const char *e = getenv("XBOX_WIDE_CAMERA_LOG");
    float k;
    s_cam_log = e && e[0] == '1';
    if (!(a > 4.0 / 3.0 + 0.005)) return 0;             /* 4:3 */
    if (fabs(a - 16.0 / 9.0) < 0.005 && !g_aspect_menus43) return 1;   /* 16:9: the title's own 0.75 */
    k = fabs(a - 16.0 / 9.0) < 0.005 ? 0.75f : (float)((4.0 / 3.0) / a);
    memcpy(&s_xscale_bits, &k, 4);
    g_aspect_hook_on = 1;
    e = getenv("XBOX_WIDE_CAMERA");
    if (e && e[0] == '0') g_aspect_camera = 0;
    if (g_aspect_camera && g_aspect_fov == ASPECT_FOV_NOSTRETCH) s_cam_zoom = (float)(a * 9.0 / 16.0);
    printf("Aspect:     %.4f, renderer x-scale %.4f, field of view %s, follow camera x%.3f\n",
           a, k, aspect_fov_name(g_aspect_fov), s_cam_zoom);
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

/* The framed image's x-scale: the title's 16:9 (0.75f), or its 4:3 (1.0f)
 * with the menus at 4:3 (aspect_set_menus). */
static uint32_t boxed_bits(void)
{
    return g_aspect_menus43 ? 0x3F800000u : 0x3F400000u;
}

/* Renderer_SetScreenMode(mode): mode 2 (widescreen) gets the wider x-scale.
 * The title's body runs first, unchanged; ecx is left as it leaves it. */
void hook_aspect_000F9EA0(void)
{
    uint32_t self = g_ecx, mode = MEM32(g_esp + 4u);
    sub_000F9EA0();
    if (mode == 2u && g_aspect_hook_on) {
        s_renderer = self;
        MEM32(self + 0x53Cu) = s_boxed ? boxed_bits() : s_xscale_bits;    /* 0.75f: 16:9 */
    } else if (self == s_renderer) {
        s_renderer = 0;
    }
}

/* Frames shown in the 16:9 frame (menus, loading screens; hud_anchor.c
 * decides) get the title's own 16:9 x-scale back, the others the wide one.
 * Called before each RenderFrame, which builds its projections from it. */
void aspect_frame(int boxed)
{
    s_boxed = boxed;
    if (g_aspect_hook_on && s_renderer >= 0x1000u)
        MEM32(s_renderer + 0x53Cu) = boxed ? boxed_bits() : s_xscale_bits;
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
    } else if (g_aspect_hook_on && s_boxed) {
        d3d8_SetPointZoom(1.0f);            /* a 16:9 frame: the title's own sizes */
    }
    sub_0017770D();
}

/* Follow camera. The title's camera runs as is (same simulation as at
 * 16:9); only the eye it leaves for the view is moved back, between two
 * ticks:
 *   0x75DE0  Camera_ComputeDesiredEye (thiscall, ret 0x10: out = cam+0x47C,
 *            targets, ...): a follow camera (params [this+0x11C], flag +0xC8,
 *            target targets[[p+4]]) ;
 *   0x759F0  its collision check (cdecl: eye*, pivot*, k): ground under the
 *            eye, then the segment pivot -> eye against the scene (walls,
 *            tunnels) ; 0x75DE0 is its only caller. The pivot sits on the
 *            rider's body ;
 *   0x78DE0  Camera_Update (thiscall): the eye [cam+0x42C] trails the
 *            desired one ; the view (0xABD30) and the rest read it.
 *   0xABD30  builds view i (InGameState+0xB0+0x80i: eye, then the view
 *            matrix at +0x10, rows of a row-vector matrix, so the forward
 *            axis is its third column) for the camera [st+0xA0+0x80i].
 * After 0x78DE0 the eye goes back along the forward axis by (z-1) times the
 * pivot's depth, then through 0x759F0 again (pivot and k from this tick),
 * and stays in [cam+0x42C] until the title's next update, which gets its own
 * eye back. Everything at the pivot's depth keeps its size and its place on
 * screen under the NoStretch zoom z = A / (16/9): the rider is framed as at
 * 16:9, lag included; only the background is narrower. */
typedef struct {
    uint32_t cam, k;
    int      follow, have_pivot, shown_valid;
    float    pivot[3], sim[3], shown[3];
} CamSlot;
static CamSlot  s_cs[4];
static CamSlot *s_cs_cur;               /* while 0x75DE0 runs */

static CamSlot *cam_slot(uint32_t cam)
{
    int i;
    for (i = 0; i < 4; i++) if (s_cs[i].cam == cam) return &s_cs[i];
    for (i = 0; i < 4; i++) if (!s_cs[i].cam) { s_cs[i].cam = cam; return &s_cs[i]; }
    return 0;
}

/* The title's eye back, unless something wrote the eye since (a cut). */
static void cam_restore(CamSlot *c)
{
    int i;
    if (!c->shown_valid) return;
    c->shown_valid = 0;
    for (i = 0; i < 3; i++) if (MEMF(c->cam + 0x42Cu + 4u * i) != c->shown[i]) return;
    for (i = 0; i < 3; i++) MEMF(c->cam + 0x42Cu + 4u * i) = c->sim[i];
}

void hook_cam_00075DE0(void)
{
    uint32_t p = MEM32(g_ecx + 0x11Cu), out = MEM32(g_esp + 4u), list = MEM32(g_esp + 8u);
    CamSlot *c;
    if (!(s_cam_zoom > 1.001f) || out < 0x10000u || !(c = cam_slot(out - 0x47Cu))) {
        sub_00075DE0();
        return;
    }
    c->follow = p >= 0x10000u && MEM32(p + 0xC8u) && MEM32(p) && list >= 0x10000u &&
                MEM32(list + (MEM32(p + 4u) << 5)) >= 0x10000u;
    c->have_pivot = 0;
    s_cs_cur = c->follow ? c : 0;
    sub_00075DE0();
    s_cs_cur = 0;
}

void hook_cam_000759F0(void)
{
    CamSlot *c = s_cs_cur;
    if (c) {
        uint32_t pv = MEM32(g_esp + 8u);
        int i;
        for (i = 0; i < 3; i++) c->pivot[i] = MEMF(pv + 4u * i);
        c->k = MEM32(g_esp + 12u);
        c->have_pivot = 1;
    }
    sub_000759F0();
}

/* 0x759F0 on (eye, pivot) from here: both copied below the guest stack. */
static void cam_collide(float eye[3], const float pivot[3], uint32_t k)
{
    uint32_t sv[7] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_seh_ebp };
    uint32_t esp0 = g_esp, buf = (g_esp - 0x40u) & ~0xFu;
    int i;
    for (i = 0; i < 3; i++) { MEMF(buf + 4u * i) = eye[i]; MEMF(buf + 0x10u + 4u * i) = pivot[i]; }
    MEMF(buf + 0xCu) = 1.0f; MEMF(buf + 0x1Cu) = 1.0f;
    g_esp = buf;
    g_esp -= 4; MEM32(g_esp) = k;
    g_esp -= 4; MEM32(g_esp) = buf + 0x10u;
    g_esp -= 4; MEM32(g_esp) = buf;
    g_esp -= 4; MEM32(g_esp) = 0;          /* dummy return address */
    sub_000759F0();
    for (i = 0; i < 3; i++) eye[i] = MEMF(buf + 4u * i);
    g_esp = esp0;
    g_eax = sv[0]; g_ecx = sv[1]; g_edx = sv[2]; g_ebx = sv[3]; g_esi = sv[4]; g_edi = sv[5]; g_seh_ebp = sv[6];
}

/* Forward axis of the view built last tick for this camera; 0 if none. */
static int cam_forward(uint32_t cam, float f[3])
{
    uint32_t app = MEM32(0x001E3C7Cu), st = app >= 0x1000u ? MEM32(app + 4u) : 0, n, i, m;
    if (st < 0x1000u) return 0;
    n = MEM32(st + 0x298u);
    for (i = 0; i < n && i < 4u; i++) {
        if (MEM32(st + 0xA0u + 0x80u * i) != cam) continue;
        m = st + 0xC0u + 0x80u * i;
        f[0] = MEMF(m + 0x08u); f[1] = MEMF(m + 0x18u); f[2] = MEMF(m + 0x28u);
        return fabsf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2] - 1.0f) < 0.01f;
    }
    return 0;
}

void hook_cam_00078DE0(void)
{
    uint32_t cam = g_ecx;
    CamSlot *c = s_cam_zoom > 1.001f ? cam_slot(cam) : 0;
    float e[3], f[3], dp = 0.0f, back = 0.0f;
    int i, axis;
    if (c) cam_restore(c);
    sub_00078DE0();
    if (!c || !c->follow || !c->have_pivot) return;
    c->follow = 0;                      /* a fresh 0x75DE0 each tick */
    for (i = 0; i < 3; i++) c->sim[i] = MEMF(cam + 0x42Cu + 4u * i);
    axis = cam_forward(cam, f);
    if (axis) {
        for (i = 0; i < 3; i++) dp += (c->pivot[i] - c->sim[i]) * f[i];
        axis = dp > 1.0f;
    }
    if (axis) {
        back = (s_cam_zoom - 1.0f) * dp;
        for (i = 0; i < 3; i++) e[i] = c->sim[i] - f[i] * back;
    } else {                            /* no view yet: scale about the pivot */
        for (i = 0; i < 3; i++) e[i] = c->pivot[i] + (c->sim[i] - c->pivot[i]) * s_cam_zoom;
    }
    cam_collide(e, c->pivot, c->k);
    for (i = 0; i < 3; i++) MEMF(cam + 0x42Cu + 4u * i) = c->shown[i] = e[i];
    c->shown_valid = 1;
    if (s_cam_log > 0) {
        static unsigned n;
        if (n++ % 15u == 0u) {
            float mv = 0.0f;
            for (i = 0; i < 3; i++) mv += (e[i] - c->sim[i]) * (e[i] - c->sim[i]);
            printf("CAMLOG z=%.3f axis=%d pivot_depth=%.1f back=%.1f moved=%.1f\n",
                   s_cam_zoom, axis, dp, back, sqrtf(mv));
        }
    }
}

void (*aspect_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x000F9EA0u) return hook_aspect_000F9EA0;
    if (xbox_va == 0x0017770Du) return hook_fov_0017770D;
    if (xbox_va == 0x00075DE0u) return hook_cam_00075DE0;
    if (xbox_va == 0x000759F0u) return hook_cam_000759F0;
    if (xbox_va == 0x00078DE0u) return hook_cam_00078DE0;
    return 0;
}
