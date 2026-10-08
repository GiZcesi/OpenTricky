/*
 * hud_anchor -- race HUD in its own proportions, at the size chosen (fork). See hud_anchor.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "recomp/recomp_types.h"
#include "hud_anchor.h"
#include "aspect.h"

void sub_000C6730(void);    /* HUD_DrawRaceOverlay (thiscall, ret 20) */
void sub_000C44E0(void);    /* HUD_DrawWorldSpaceMarkers (thiscall, ret 20) */
void sub_000C9BB0(void);    /* HUD object, vtable 0x19BE20 [0] (thiscall, ret) */
void sub_000C9D70(void);    /* HUD object, vtable 0x19BE20 [1] (thiscall, ret) */
void pgraph_d3d11_set_hud_scale(float kx, float ky);   /* nv2a_pgraph_d3d11.h */
void pgraph_d3d11_set_box(float k);                    /* nv2a_pgraph_d3d11.h */
void d3d8_SetPresentBoxShape(double shape);           /* d3d8_xbox.h */
extern int g_aspect_menus43;                          /* aspect.h */
void pgraph_d3d11_set_box_race2d(float k2d);           /* nv2a_pgraph_d3d11.h */
void pgraph_d3d11_set_box_frame(float kboxed);          /* nv2a_pgraph_d3d11.h */
void sub_000D2D80(void);    /* finish banner overlay, vtable 0x19C600 [1] (thiscall, ret) */

#define GFX_GLOBAL      0x001BA7A0u     /* the GfxContext (RenderFrame's this) */
#define GFX_VIEWS       0x196018u       /* gfx: first view */
#define VIEW_STRIDE     0xC8C0u
#define VIEW_COUNT      6
#define VIEW_LIST_END   0x808u          /* view: end of its records */
#define VIEW_LIST       0x8C0u          /* view: first record (0x18 bytes each) */
#define VB_2D           0x1580Cu        /* gfx: 2D vertex buffer, per frame buffer */
#define VB_2D_ALT       0x15820u        /* the world markers' other one (gfx+0x20 < 0) */
#define GFX_FRAME_BUF   0x196014u       /* gfx: frame buffer index */
#define VT_RACE_QUADS   0x001A2AB0u     /* node: +4 next, +0xA first vertex (short), 4 vertices */
#define VT_WORLD_QUADS  0x001A2AC8u     /* node: +4 next, +8 count, +0xC first vertex */

int g_hud_anchor_on;
int g_panel_on;                     /* XBOX_FIX_PANEL_EDGES: the panels' bars edge to edge */
int g_box_on;
int g_race2d_on;
static int   s_race2d;              /* XBOX_FIX_RACE2D: 0 off, 1 on (default), 2 + banner as race HUD */
static int   s_mode = HUD_SHAPE_PROPORTIONAL;
static int   s_size = 100;
static int   s_trlog;               /* XBOX_HUD_TRLOG=N: the elements of every Nth list */
static int   s_diag;                /* XBOX_HUD_DIAG=1 (diagnostic) */
static int   s_banner_watch;        /* the 0xD2D80 hook looks at what the banner draws */
static int   s_panel_log;           /* XBOX_PANEL_LOG=1 (diagnostic) */

static const char *const k_mode_name[HUD_SHAPE_COUNT] = { "Proportional", "Xbox" };
static const int k_size[HUD_SIZE_COUNT] = { 100, 90, 85, 80, 70 };

const char *hud_anchor_mode_name(int mode)
{
    return (mode >= 0 && mode < HUD_SHAPE_COUNT) ? k_mode_name[mode] : k_mode_name[0];
}

int hud_anchor_mode_parse(const char *s, int fallback)
{
    int i;
    if (!s || !s[0]) return fallback;
    for (i = 0; i < HUD_SHAPE_COUNT; i++)
        if (!_stricmp(s, k_mode_name[i])) return i;
    if (!_stricmp(s, "auto") || !_stricmp(s, "on") || !strcmp(s, "1")) return HUD_SHAPE_PROPORTIONAL;
    if (!_stricmp(s, "stretched") || !_stricmp(s, "off") || !strcmp(s, "0")) return HUD_SHAPE_XBOX;
    return fallback;
}

int hud_anchor_size_value(int index)
{
    return k_size[index >= 0 && index < HUD_SIZE_COUNT ? index : 0];
}

int hud_anchor_size_parse(const char *s, int fallback)
{
    int v, i;
    if (!s || !s[0]) return fallback;
    v = atoi(s);
    for (i = 0; i < HUD_SIZE_COUNT; i++)
        if (k_size[i] == v) return i;
    return fallback;
}

void hud_anchor_set(double shape, int mode, int size)
{
    const char *e = getenv("XBOX_HUD_TRLOG");
    float kx, ky;
    s_mode = hud_anchor_mode_parse(getenv("XBOX_HUD_SHAPE"), mode);
    if (s_mode < 0 || s_mode >= HUD_SHAPE_COUNT) s_mode = HUD_SHAPE_PROPORTIONAL;
    s_size = hud_anchor_size_value(hud_anchor_size_parse(getenv("XBOX_HUD_SIZE"), size));
    s_trlog = e ? atoi(e) : 0;
    e = getenv("XBOX_HUD_DIAG");
    s_diag = e ? atoi(e) : 0;
    if (!(shape > 1.0)) shape = 4.0 / 3.0;
    ky = (float)s_size / 100.0f;
    kx = shape > 4.0 / 3.0 + 0.005 ? (float)((4.0 / 3.0) / shape) * ky : ky;
    g_hud_anchor_on = s_mode == HUD_SHAPE_PROPORTIONAL && (kx < 0.999f || ky < 0.999f);
    pgraph_d3d11_set_hud_scale(g_hud_anchor_on ? kx : 0.0f, g_hud_anchor_on ? ky : 0.0f);
    e = getenv("XBOX_WIDE_MENUS");
    {   /* menus at 4:3 (aspect_set_menus): the menus whole at 4:3 from 16:9 up */
        /* the 4:3 frame needs the title's 4:3 x-scale out of a race (aspect.c) */
        int m43 = g_aspect_menus43 && g_aspect_hook_on;
        double fr = m43 ? 4.0 / 3.0 : 16.0 / 9.0;
        g_box_on = shape > fr + 0.005 && !(e && e[0] == '0');
        d3d8_SetPresentBoxShape(fr);
        pgraph_d3d11_set_box(g_box_on ? (float)(fr / shape) : 0.0f);
        pgraph_d3d11_set_box_frame(m43 ? 1.0f : 0.75f);     /* the panels' content in a framed image */
    }
    /* The race's own 2D outside the race HUD (finish banner, pause, end
     * screens) at the title's 4:3 proportions, like the race HUD: on any
     * screen wider than 4:3, 16:9 included, with the proportional HUD. */
    e = getenv("XBOX_FIX_RACE2D");
    s_race2d = e && e[0] ? atoi(e) : 1;
    if (s_race2d < 0 || s_race2d > 2) s_race2d = 1;
    g_race2d_on = s_race2d != 0 && s_mode == HUD_SHAPE_PROPORTIONAL && shape > 4.0 / 3.0 + 0.005;
    pgraph_d3d11_set_box_race2d(g_race2d_on ? (float)((4.0 / 3.0) / shape) : 0.0f);
    /* The panels (dark backing, gold bars) reach the screen's edges: wider
     * than 4:3, their content at the 4:3 proportions, with the race's 2D
     * above; at 4:3 (the title's 2D over the whole screen), their content
     * as drawn. */
    e = getenv("XBOX_FIX_PANEL_EDGES");
    g_panel_on = !(e && e[0] == '0') && (g_race2d_on || !(shape > 4.0 / 3.0 + 0.005));
    e = getenv("XBOX_PANEL_LOG");
    s_panel_log = e ? atoi(e) : 0;
    if (g_panel_on)
        printf("Panels:     edge to edge, their content %s\n",
               g_race2d_on ? "at 4:3 proportions" : "as drawn");
    if (g_box_on)
        printf("Menus:      in a centred %s frame, x %.4f\n", g_aspect_menus43 && g_aspect_hook_on ? "4:3" : "16:9",
               (g_aspect_menus43 && g_aspect_hook_on ? 4.0 / 3.0 : 16.0 / 9.0) / shape);
    e = getenv("XBOX_HUD_BANNER_SHOTS");
    s_banner_watch = g_race2d_on || s_trlog || s_diag || (e && e[0]);
    if (g_race2d_on)
        printf("Race 2D:    at 4:3 proportions, x %.4f%s\n", (4.0 / 3.0) / shape,
               s_race2d == 2 ? ", finish banner with the race HUD" : "");
    if (g_hud_anchor_on)
        printf("HUD:        proportional, size %d%%, x %.4f y %.4f\n", s_size, kx, ky);
    else
        printf("HUD:        %s\n", s_mode == HUD_SHAPE_XBOX
               ? "like the Xbox (stretched to the screen)" : "as drawn by the title (4:3, 100%)");
}

/* ── Which records are the race HUD (ticks) ──────────────────────────
 *
 * The HUD functions do not draw: they append records (object + state block)
 * to the list of the HUD's orthographic view, drawn by the next RenderFrame
 * after the list is sorted (records move, their object stays).
 * So the object is the key. The lists come in two buffers drawn in turn,
 * their objects from a per-frame arena: a mark is good for its buffer's
 * current fill only. A fill starts when the list is empty or shorter than
 * the last time it was seen. Game thread only (ticks and RenderFrame). */
#define NBUF 16
static struct { uint32_t base, last_end, gen, calls, calls_at_draw, drawn_frame; } s_buf[NBUF];
static unsigned s_nbuf, s_gen, s_seg;

typedef struct { float x, y, w, h; } View;      /* the player's view, 640x480 pixels */
static const View k_full = { 0.0f, 0.0f, 640.0f, 480.0f };

#define NTAG 4096u              /* power of two */
static struct {
    uint32_t obj, base, gen;
    View     view;
    uint32_t seg;               /* the HUD call that appended it */
    uint32_t frame, tag, tag_y; /* the render frame the tags were worked out for */
} s_tag[NTAG];
static unsigned long long s_marked, s_found, s_full, s_elements, s_stale;
static unsigned s_lists, s_frame = 1;
static double   s_work_ms;          /* time spent working lists out */

static unsigned buf_index(uint32_t base)
{
    unsigned i;
    for (i = 0; i < s_nbuf; i++) if (s_buf[i].base == base) return i;
    if (s_nbuf < NBUF) {
        s_buf[s_nbuf].last_end = s_buf[s_nbuf].gen = 0;
        s_buf[s_nbuf].calls = s_buf[s_nbuf].calls_at_draw = s_buf[s_nbuf].drawn_frame = 0;
        s_buf[s_nbuf].base = base;
        return s_nbuf++;
    }
    return NBUF;                /* more lists than the title has: unmarked */
}

static unsigned slot_of(uint32_t obj) { return (unsigned)(((obj >> 3) * 2654435761u) >> 20) & (NTAG - 1u); }

static int mark_live(unsigned h)
{
    unsigned b;
    if (!s_tag[h].obj) return 0;
    b = buf_index(s_tag[h].base);
    return b < NBUF && s_buf[b].gen == s_tag[h].gen;
}

static void mark_put(uint32_t obj, uint32_t base, uint32_t gen, const View *v, uint32_t seg)
{
    unsigned h = slot_of(obj), i;
    for (i = 0; i < 16u; i++, h = (h + 1u) & (NTAG - 1u)) {
        if (!s_tag[h].obj || s_tag[h].obj == obj || !mark_live(h)) {
            s_tag[h].obj = obj; s_tag[h].base = base; s_tag[h].gen = gen; s_tag[h].view = *v; s_tag[h].seg = seg;
            s_tag[h].frame = 0; s_tag[h].tag = s_tag[h].tag_y = 0;
            s_marked++;
            return;
        }
    }
    s_full++;
}

/* The live mark of obj in the list at base, or -1. */
static int mark_find(uint32_t obj, uint32_t base)
{
    unsigned h = slot_of(obj), i;
    if (!obj) return -1;
    for (i = 0; i < 16u; i++, h = (h + 1u) & (NTAG - 1u)) {
        if (!s_tag[h].obj) return -1;
        if (s_tag[h].obj == obj)
            return s_tag[h].base == base && mark_live(h) ? (int)h : -1;
    }
    return -1;
}

/* The race HUD is two methods of the HUD object (vtable 0x19BE20): 0xC9BB0
 * and 0xC9D70. Each loops over the players' views and calls, per view, the
 * draw function with the view's rectangle -- HUD_DrawWorldSpaceMarkers
 * 0xC44E0 and HUD_DrawRaceOverlay 0xC6730 -- plus a few panels of its own
 * (the Tricky gauge, the progress bar's marks). The methods mark all they
 * append; a record appended by an inner call takes that call's view, the
 * others the view of the inner call that follows them in the list (the
 * panels of a view are drawn just before its draw function). */
static void hud_call(void (*body)(void), int has_view)
{
    uint32_t gfx = MEM32(GFX_GLOBAL), e0[VIEW_COUNT], base[VIEW_COUNT], views, r;
    View vw = k_full;
    uint32_t seg;
    int i;

    if ((!g_hud_anchor_on && !g_box_on && !g_panel_on) || !gfx) { body(); return; }   /* the panels leave the race HUD out */
    if (has_view) {
        /* (rider, y, x, width, height): the player's view in 640x480 */
        vw.y = MEMF(g_esp + 8u);
        vw.x = MEMF(g_esp + 12u);
        vw.w = MEMF(g_esp + 16u);
        vw.h = MEMF(g_esp + 20u);
        if (!(vw.w > 1.0f && vw.w <= 2048.0f && vw.x >= 0.0f && vw.x < 2048.0f &&
              vw.h > 1.0f && vw.h <= 2048.0f && vw.y >= 0.0f && vw.y < 2048.0f)) vw = k_full;
    }
    seg = ++s_seg;
    views = MEM32(gfx + GFX_VIEWS);
    for (i = 0; i < VIEW_COUNT; i++) {
        unsigned b;
        uint32_t v = views + (uint32_t)i * VIEW_STRIDE;
        base[i] = v + VIEW_LIST;
        e0[i] = MEM32(v + VIEW_LIST_END);
        if (e0[i] < base[i] || (b = buf_index(base[i])) >= NBUF) continue;
        if (e0[i] == base[i] || e0[i] < s_buf[b].last_end) s_buf[b].gen = ++s_gen;   /* a new fill */
        s_buf[b].last_end = e0[i];
        s_buf[b].calls++;
    }
    body();
    for (i = 0; i < VIEW_COUNT; i++) {
        uint32_t e1 = MEM32(views + (uint32_t)i * VIEW_STRIDE + VIEW_LIST_END);
        unsigned b;
        if (e1 <= e0[i] || e0[i] < base[i] || (b = buf_index(base[i])) >= NBUF) continue;
        s_buf[b].last_end = e1;
        if (has_view) {
            for (r = e0[i]; r < e1; r += 0x18u)
                if (MEM32(r)) mark_put(MEM32(r), base[i], s_buf[b].gen, &vw, seg);
        } else {
            /* the method: what its inner calls did not mark takes the view
             * of the next marked record */
            View next = k_full;
            for (r = e1; r > e0[i]; ) {
                int h;
                r -= 0x18u;
                if (!MEM32(r)) continue;
                h = mark_find(MEM32(r), base[i]);
                if (h >= 0) { next = s_tag[h].view; continue; }
                mark_put(MEM32(r), base[i], s_buf[b].gen, &next, seg);
            }
        }
    }
}

void hook_hud_000C6730(void) { hud_call(sub_000C6730, 1); }
void hook_hud_000C44E0(void) { hud_call(sub_000C44E0, 1); }
void hook_hud_000C9BB0(void) { hud_call(sub_000C9BB0, 0); }
void hook_hud_000C9D70(void) { hud_call(sub_000C9D70, 0); }

/* The finish banner (FINISH / TIME UP, "1st PLACE", the time, the medal):
 * method [1] of an overlay object of its own (vtable 0x19C600, the same
 * interface as the HUD's 0x19BE20), not of the HUD, so its records are not
 * the race HUD's: drawn with the rest of the race's 2D (4:3 proportions,
 * full size, centred). XBOX_FIX_RACE2D=2 makes it part of the race HUD
 * instead (HUD size, anchored), to compare. The hook also says when it
 * first draws, which ties the banner on screen to this function. */
static unsigned long long s_banner_draws;
void d3d8_RequestScreenshot(const wchar_t *path);
int  d3d8_ScreenshotPending(void);
unsigned d3d8_PresentSeq(void);

/* XBOX_HUD_BANNER_SHOTS=<path prefix> (tests): internal screenshots at the
 * banner's Nth draws (XBOX_HUD_BANNER_AT=N,N,..., default 20,60,120,240),
 * the same moment of the banner on every run, whatever the screen shape.
 * The game waits (tests only) for the previous shot to be taken. */
static void banner_shot(unsigned long long draw)
{
    static int init, n;
    static unsigned at[16];
    static wchar_t pre[MAX_PATH];
    wchar_t path[MAX_PATH];
    DWORD w0;
    int i;
    if (!init) {
        const char *p = getenv("XBOX_HUD_BANNER_SHOTS"), *a = getenv("XBOX_HUD_BANNER_AT");
        init = 1;
        if (!p || !*p) return;
        MultiByteToWideChar(CP_ACP, 0, p, -1, pre, MAX_PATH);
        if (!a || !*a) a = "20,60,120,240";
        while (*a && n < 16) {
            at[n++] = (unsigned)strtoul(a, (char **)&a, 10);
            while (*a == ',' || *a == ' ') a++;
        }
    }
    for (i = 0; i < n; i++) {
        if (at[i] != draw) continue;
        w0 = GetTickCount();
        while (d3d8_ScreenshotPending() && GetTickCount() - w0 < 3000u) Sleep(1);
        _snwprintf(path, MAX_PATH, L"%lsbanner_d%04u.png", pre, at[i]);
        path[MAX_PATH - 1] = 0;
        d3d8_RequestScreenshot(path);
        fprintf(stderr, "[HUD] banner shot at draw %u, frame %u, present %u\n", at[i], s_frame, d3d8_PresentSeq());
        return;
    }
}

static uint32_t view_records(void)
{
    uint32_t gfx = MEM32(GFX_GLOBAL), views, v, e, n = 0;
    int i;
    if (!gfx || (views = MEM32(gfx + GFX_VIEWS)) < 0x1000u) return 0;
    for (i = 0; i < VIEW_COUNT; i++) {
        v = views + (uint32_t)i * VIEW_STRIDE;
        e = MEM32(v + VIEW_LIST_END);
        if (e > v + VIEW_LIST) n += (e - v - VIEW_LIST) / 0x18u;
    }
    return n;
}

void hook_hud_000D2D80(void)
{
    uint32_t n0, n1;
    if (!s_banner_watch) { sub_000D2D80(); return; }
    n0 = view_records();
    if (s_race2d == 2 && g_race2d_on) hud_call(sub_000D2D80, 0);
    else sub_000D2D80();
    n1 = view_records();
    if (n1 <= n0) return;
    if (!s_banner_draws || (s_trlog > 0 && (s_banner_draws % (unsigned)s_trlog) == 0u))
        fprintf(stderr, "[HUD] finish banner (0xD2D80) draws %u records, frame %u, present %u, draw %llu\n",
                n1 - n0, s_frame, d3d8_PresentSeq(), s_banner_draws + 1u);
    s_banner_draws++;
    banner_shot(s_banner_draws);
}

/* ── Elements and their anchor (RenderFrame) ─────────────────────────
 *
 * Each record is one quad (a glyph, its drop shadow, a bar segment, an icon)
 * or a short chain of them, taken from the frame's 2D vertex buffer: x, y in
 * 640x480 pixels at the start of each 32-byte vertex. The vertices
 * are final only when RenderFrame draws the list (some are written after the
 * HUD functions), so the first HUD record drawn from a list works out the
 * whole list: records close to each other -- overlapping rows, a gap under
 * a glyph's height -- make one element (a string with its shadow, a number
 * with its label), and the element goes to one anchor: the left edge, the
 * right edge or the centre of the player's view by the third its centre
 * falls in, and the same for the top, the middle or the bottom. An element
 * nearly as wide as the view (wipes, fades) keeps the stretch. */
#define MAX_REC 512

typedef struct { float x0, x1, y0, y1; } Box;

static int box_valid(const Box *b) { return b->x0 <= b->x1 && b->y0 <= b->y1; }

static int data_ok(uint32_t va, uint32_t bytes)
{
    return va >= 0x80000000u && va + bytes >= va && va + bytes < 0x88000000u;
}

/* Box of the quads of one record, read from one of the 2D vertex buffers;
 * empty if a vertex is missing or off the 640x480 screen by far. */
static Box box_from(uint32_t gfx, uint32_t obj, uint32_t vt, uint32_t vb_slot)
{
    Box b = { 1e30f, -1e30f, 1e30f, -1e30f }, none = b;
    uint32_t node, vb, data, first, n, k, hops;
    vb = MEM32(gfx + vb_slot + 4u * MEM32(gfx + GFX_FRAME_BUF));
    if (vb < 0x1000u) return none;
    data = MEM32(vb + 4u) | 0x80000000u;
    for (node = obj, hops = 0; node && hops < 64u; node = MEM32(node + 4u), hops++) {
        if (MEM32(node) != vt) break;
        if (vt == VT_RACE_QUADS) { first = (uint32_t)(int32_t)(int16_t)MEM16(node + 0xAu); n = 4; }
        else { first = MEM32(node + 0xCu); n = MEM32(node + 8u); if (n > 64u) n = 64u; }
        if (!data_ok(data + first * 0x20u, n * 0x20u)) return none;
        for (k = 0; k < n; k++) {
            float x = MEMF(data + (first + k) * 0x20u), y = MEMF(data + (first + k) * 0x20u + 4u);
            if (!(x > -128.0f && x < 768.0f && y > -128.0f && y < 608.0f)) return none;
            if (x < b.x0) b.x0 = x;
            if (x > b.x1) b.x1 = x;
            if (y < b.y0) b.y0 = y;
            if (y > b.y1) b.y1 = y;
        }
    }
    return b;
}

/* The race panels draw from the 2D buffer; the world markers from it or
 * from a second one (the title picks by gfx+0x20, a state that changes while
 * the frame is drawn): the buffer whose vertices make sense wins. */
static Box record_box(uint32_t gfx, uint32_t obj)
{
    Box b = { 1e30f, -1e30f, 1e30f, -1e30f };
    uint32_t vt;
    if (!obj) return b;
    vt = MEM32(obj);
    if (vt != VT_RACE_QUADS && vt != VT_WORLD_QUADS) return b;
    b = box_from(gfx, obj, vt, VB_2D);
    if (!box_valid(&b) && vt == VT_WORLD_QUADS) b = box_from(gfx, obj, vt, VB_2D_ALT);
    return b;
}

static int near_box(const Box *e, const Box *q)
{
    /* the gap allowed between two glyphs of a string: from the lower of the
     * two heights, 6 to 16 pixels (a tall bar must not reach across) */
    float hq = q->y1 - q->y0, he = e->y1 - e->y0, h = hq < he ? hq : he;
    float gap = h * 0.6f < 6.0f ? 6.0f : h * 0.6f > 16.0f ? 16.0f : h * 0.6f;
    if (q->y0 > e->y1 + 2.0f || q->y1 < e->y0 - 2.0f) return 0;        /* not on its rows */
    return q->x0 <= e->x1 + gap && q->x1 >= e->x0 - gap;
}

/* Two elements join when the same HUD call drew them, the union stays well
 * under the view's width (nothing chains across the screen) and
 *   - they are text on the same rows, a glyph's gap apart (both text-high:
 *     a tall bar must not pull its neighbours in), or
 *   - they are stacked: overlapping columns, a small gap apart (a bar and
 *     its end caps, the Tricky gauge and its logo), so that a panel keeps
 *     one anchor when the HUD is made smaller. */
static int joinable(const Box *a, const Box *b, float vw)
{
    float x0 = a->x0 < b->x0 ? a->x0 : b->x0, x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    float gy = a->y0 > b->y1 ? a->y0 - b->y1 : b->y0 > a->y1 ? b->y0 - a->y1 : 0.0f;
    if (x1 - x0 > 0.6f * vw) return 0;
    if (a->x0 <= b->x1 + 2.0f && b->x0 <= a->x1 + 2.0f && gy <= 24.0f) return 1;      /* stacked */
    if (a->y1 - a->y0 > 64.0f || b->y1 - b->y0 > 64.0f) return 0;
    return near_box(a, b) || near_box(b, a);
}

/* Anchor of a coordinate range [lo, hi] in the view's [o, o + len]: the
 * start, the end or the middle, by the third its centre falls in. */
static float anchor_of(float lo, float hi, float o, float len)
{
    float c = 0.5f * (lo + hi);
    if (c < o + len / 3.0f) return o;
    if (c > o + 2.0f * len / 3.0f) return o + len;
    return o + 0.5f * len;
}

static uint32_t q16(float v)
{
    long q = lroundf(v * 16.0f);
    return q < 0 ? 0u : q > 0x7FFF ? 0x7FFFu : (uint32_t)q;
}

static int uf_root(int *p, int i) { while (p[i] != i) i = p[i] = p[p[i]]; return i; }

static void work_out_list(uint32_t gfx, uint32_t base, uint32_t end)
{
    static Box rb[MAX_REC], eb[MAX_REC];
    static int slot[MAX_REC], par[MAX_REC];
    uint32_t r;
    int n = 0, i, j, ne = 0, changed;

    for (r = base; r < end && n < MAX_REC; r += 0x18u) {
        int h = mark_find(MEM32(r), base);
        if (h < 0) continue;
        s_tag[h].frame = s_frame;
        s_tag[h].tag = s_tag[h].tag_y = 0;
        rb[n] = record_box(gfx, MEM32(r));
        if (!box_valid(&rb[n])) continue;
        if (rb[n].x1 - rb[n].x0 >= 0.9f * s_tag[h].view.w) continue;  /* full width (fades, wipes): stretched */
        slot[n] = h;
        par[n] = n;
        n++;
    }
    /* Elements: records joined by nearness, until nothing joins any more
     * (a string's shadow and its glyphs, from either end). */
    for (i = 0; i < n; i++) eb[i] = rb[i];
    do {
        changed = 0;
        for (i = 0; i < n; i++) {
            int ri = uf_root(par, i);
            for (j = i + 1; j < n; j++) {
                int rj = uf_root(par, j);
                if (ri == rj || s_tag[slot[ri]].seg != s_tag[slot[rj]].seg ||
                    !joinable(&eb[ri], &eb[rj], s_tag[slot[i]].view.w)) continue;
                par[rj] = ri;
                if (eb[rj].x0 < eb[ri].x0) eb[ri].x0 = eb[rj].x0;
                if (eb[rj].x1 > eb[ri].x1) eb[ri].x1 = eb[rj].x1;
                if (eb[rj].y0 < eb[ri].y0) eb[ri].y0 = eb[rj].y0;
                if (eb[rj].y1 > eb[ri].y1) eb[ri].y1 = eb[rj].y1;
                changed = 1;
            }
        }
    } while (changed);
    for (i = 0; i < n; i++) {
        int ri = uf_root(par, i);
        const View *v = &s_tag[slot[ri]].view;
        float ax = anchor_of(eb[ri].x0, eb[ri].x1, v->x, v->w);
        float ay = anchor_of(eb[ri].y0, eb[ri].y1, v->y, v->h);
        s_tag[slot[i]].tag   = (HUD_TAG_MAGIC << 16) | 0x8000u | q16(ax);
        s_tag[slot[i]].tag_y = (HUD_TAG_MAGIC_Y << 16) | q16(ay);
        if (ri != i) continue;
        ne++;
        if (s_trlog > 0 && (s_lists % (unsigned)s_trlog) == 0u)
            fprintf(stderr, "[HUD] frame %u element %.1f..%.1f x %.1f..%.1f -> anchor %.0f, %.0f\n", s_frame,
                    eb[i].x0, eb[i].x1, eb[i].y0, eb[i].y1, ax, ay);
    }
    s_lists++;
    s_elements += (unsigned long long)ne;
}

/* ── The 16:9 frame ───────────────────────────────────────────────────
 *
 * A race on screen (from the start grid on: race, pause, end screens) keeps
 * the wide 3D; its menus and panels (anything in the HUD view that is not
 * the race HUD) are drawn in a centred 16:9 frame over it. Everything else
 * (front end, loading screens, videos, the riders' intro scenes before the
 * start grid) is shown whole in a centred 16:9
 * frame with black bars: the title renders it with its own 16:9 x-scale and
 * the host presents it at 16:9. A frame tag per RenderFrame tells the
 * translator which of the two the frame is. */
static int race_state(void)
{
    uint32_t app = MEM32(0x001E3C7Cu), lvl, race, st;
    if (app < 0x1000u || (lvl = MEM32(app + 0x72Cu)) < 0x1000u) return -1;
    race = MEM32(lvl + 0x1Cu);
    if (race < 0x1000u) return -1;
    st = MEM32(race + 0x1Cu);       /* RaceState_SetState 0x2CE10: 1 PreRace, 2 StartRace, 3 Countdown, 4 Race, 5 EndRace... */
    return st > 0xFFFFu ? 0xFFFF : (int)st;
}

static int frame_in_race(int st)
{
    return st >= 2;                 /* not before: the level loading, the riders' intro scenes (PreRace) */
}

static int s_frame_wide;            /* this RenderFrame is a race on screen */
static int s_frame_state = -1;      /* its race state, -1 out of a level */

uint32_t hud_anchor_frame(void)
{
    int wide;
    s_frame++;
    s_frame_wide = 0;
    s_frame_state = -1;
    if (!g_box_on && !g_race2d_on) {
        if (g_panel_on) s_frame_state = race_state();   /* 4:3: the panels only */
        return 0;
    }
    s_frame_state = race_state();
    s_frame_wide = wide = frame_in_race(s_frame_state);
    if (g_box_on) aspect_frame(!wide);  /* 16:9 or 16:10: the race's 2D only, the image as before */
    if (s_diag) {
        static uint32_t last[4];
        uint32_t app = MEM32(0x001E3C7Cu), st = 0, lvl = 0, race = 0, rs = 0, cur[4];
        if (app >= 0x1000u) { st = MEM32(app + 4u); lvl = MEM32(app + 0x72Cu); }
        if (lvl >= 0x1000u) race = MEM32(lvl + 0x1Cu);
        if (race >= 0x1000u) rs = MEM32(race + 0x1Cu);
        cur[0] = st >= 0x1000u ? MEM32(st) : 0; cur[1] = lvl != 0; cur[2] = race != 0; cur[3] = rs;
        if (memcmp(cur, last, sizeof cur)) {
            fprintf(stderr, "[HUDDIAG] frame %u state vt %08X level %u race %u race state %u -> %s\n",
                    s_frame, cur[0], cur[1], cur[2], cur[3], wide ? "wide" : "16:9 frame");
            memcpy(last, cur, sizeof cur);
        }
    }
    return (HUD_TAG_MAGIC_BOX << 16) | (wide ? HUD_BOX_FRAME_WIDE : HUD_BOX_FRAME_BOXED);
}

/* A quad over the whole width that touches the top or the bottom (fades,
 * wipes, letterbox bars): stretched over the wide screen, not framed. */
static int full_width(uint32_t obj)
{
    Box b = record_box(MEM32(GFX_GLOBAL), obj);
    return box_valid(&b) && b.x0 <= 2.0f && b.x1 >= 638.0f && (b.y0 <= 2.0f || b.y1 >= 478.0f);
}

/* ── Panels edge to edge ──────────────────────────────────────────────
 *
 * The title's panels (tutorial and lessons, the Uber Trick and "Single
 * Event Race" cards before the start, pause and its options, the replay's
 * help, Top 5 and results) are a dark backing between gold bars drawn from
 * x = 10 to 632 of the 640x480 screen, their content inside. Stretched on
 * a wide screen they nearly reach its edges; at the 4:3 proportions of the
 * race's 2D they stop at the 4:3 columns. Here, wider than 4:3, their
 * content keeps the 4:3 proportions, centred, and the pieces that make the
 * panel's two ends -- the backing, the straight runs of the bars -- reach
 * the screen's edges: the vertices at the panel's left edge go to the
 * screen's left edge, those at its right edge to the right one, all the
 * others where the 4:3 proportions put them (a bar's curved middle, drawn
 * as quads of its own, keeps its shape).
 *
 * What the title draws: the backing (dark, CC181818) and each bar as two
 * halves (one quad each: a straight run and half of the curve, the whole
 * texture across it) are world quads from x = -20 to 660 meeting at 320,
 * which the title's 2D transform puts at 10..632 on screen; the text and
 * icons are race quads in screen pixels.
 *
 * Which records: in a level (the panels before the start grid too), the
 * records of a list of the HUD view that are not the race HUD, read from
 * their quads:
 *   - an end piece is at least PANEL_MIN_W wide and is a world quad that
 *     starts at x0 = -20 or ends at x1 = 660 (+- PANEL_TOL), or a race quad
 *     (not a full-width fade or letterbox: x0 > 2, x1 < 638) that starts at
 *     x0 = 10 or ends at x1 = 632 ;
 *   - a list holds a panel when a left end piece and a right end piece
 *     (or one piece that is both) share rows: the two halves of one bar, or
 *     the backing. A quad that only touches one side (text, a picture
 *     against the left edge) or a list without both ends is left alone.
 * The translator draws the end pieces on the CPU path: the vertices at the
 * panel's screen edges go to the screen's, and take the attributes of their
 * triangle's plane there (the texture keeps its 4:3 density: the curve its
 * shape, the straight run goes on, the texture being clamped).
 * In a race (state 2 on), only the end pieces change: the rest of the
 * race's 2D is already at the 4:3 proportions. Before the start grid
 * (states 0 and 1: the cards), a list with a panel gets its other records
 * at the 4:3 proportions as well (the same panels in and before a race
 * look the same); a list without one stays as the title draws it.
 * At 4:3 the title's 2D fills the screen: only the end pieces change (their
 * outer vertices to the screen's edges), the race HUD marked to be left out.
 * Game thread only (RenderFrame). */
#define PANEL_EDGE_L    10.0f
#define PANEL_EDGE_R    632.0f
#define PANEL_OBJ_L     (-20.0f)        /* the same edges in a panel quad's own x (world quads) */
#define PANEL_OBJ_R     660.0f
#define PANEL_TOL       4.0f
#define PANEL_MIN_W     48.0f
#define PANEL_SNAP      3.0f            /* a vertex this close to an edge goes to the screen's */
#define PANEL_PIECES    32
#define PANEL_LISTS     8

typedef struct {
    uint32_t base, frame;
    int      found, n;
    float    l, r;                      /* the panel's edges */
    uint32_t piece[PANEL_PIECES];
} PanelList;
static PanelList s_panel[PANEL_LISTS];
static unsigned  s_panel_next;

static int rows_meet(const Box *a, const Box *b)
{
    return a->y0 <= b->y1 && b->y0 <= a->y1;
}

static void panel_log(const PanelList *p, int nrec, int unread, uint32_t unread_vt,
                      const Box *lb, int nl, const Box *rb, int nr)
{
    static struct { uint32_t base; int found, n, nl, nr, nrec, state; } last[PANEL_LISTS];
    unsigned i, slot = PANEL_LISTS;
    int k;
    for (i = 0; i < PANEL_LISTS && slot == PANEL_LISTS; i++)
        if (last[i].base == p->base) slot = i;
    for (i = 0; i < PANEL_LISTS && slot == PANEL_LISTS; i++)
        if (!last[i].base) slot = i;
    if (slot == PANEL_LISTS) slot = 0;
    if (last[slot].base == p->base && last[slot].found == p->found && last[slot].n == p->n &&
        last[slot].nl == nl && last[slot].nr == nr && last[slot].nrec == nrec &&
        last[slot].state == s_frame_state) return;
    last[slot].base = p->base; last[slot].found = p->found; last[slot].n = p->n;
    last[slot].nl = nl; last[slot].nr = nr; last[slot].nrec = nrec; last[slot].state = s_frame_state;
    fprintf(stderr, "[PANEL] frame %u race state %d list %08X: records %d (unread %d, vt %08X), "
            "left ends %d, right ends %d -> %s %.1f..%.1f, pieces %d\n",
            s_frame, s_frame_state, p->base, nrec, unread, unread_vt, nl, nr,
            p->found ? "panel" : "no panel", p->l, p->r, p->n);
    for (k = 0; k < nl; k++)
        fprintf(stderr, "[PANEL]   left  %.1f..%.1f x %.1f..%.1f\n", lb[k].x0, lb[k].x1, lb[k].y0, lb[k].y1);
    for (k = 0; k < nr; k++)
        fprintf(stderr, "[PANEL]   right %.1f..%.1f x %.1f..%.1f\n", rb[k].x0, rb[k].x1, rb[k].y0, rb[k].y1);
}

/* XBOX_PANEL_LOG=2 (diagnostic): the vertices of one record (x, y, colour, u, v). */
static void panel_dump_vertices(uint32_t gfx, uint32_t obj)
{
    uint32_t vt = MEM32(obj), slot, vb, data, node, first, n, k, hops;
    for (slot = 0; slot < 2; slot++) {
        vb = MEM32(gfx + (slot ? VB_2D_ALT : VB_2D) + 4u * MEM32(gfx + GFX_FRAME_BUF));
        if (vb < 0x1000u) continue;
        data = MEM32(vb + 4u) | 0x80000000u;
        for (node = obj, hops = 0; node && hops < 16u; node = MEM32(node + 4u), hops++) {
            if (MEM32(node) != vt) break;
            if (vt == VT_RACE_QUADS) { first = (uint32_t)(int32_t)(int16_t)MEM16(node + 0xAu); n = 4; }
            else { first = MEM32(node + 0xCu); n = MEM32(node + 8u); if (n > 16u) n = 16u; }
            if (!data_ok(data + first * 0x20u, n * 0x20u)) break;
            for (k = 0; k < n; k++) {
                uint32_t a = data + (first + k) * 0x20u;
                fprintf(stderr, "[PANEL]     vb%u node %u v%u: x %.1f y %.1f c %08X uv %.4f %.4f\n", slot, hops, k,
                        MEMF(a), MEMF(a + 4u), MEM32(a + 16u), MEMF(a + 20u), MEMF(a + 24u));
            }
        }
    }
}

/* The panel of the list at base this frame (worked out at its first
 * record drawn: the vertices are final then). */
static const PanelList *panel_of_list(uint32_t base, uint32_t end)
{
    static Box lb[PANEL_PIECES], rb[PANEL_PIECES];
    static uint32_t lo[PANEL_PIECES], ro[PANEL_PIECES];
    uint32_t gfx = MEM32(GFX_GLOBAL), r, unread_vt = 0;
    PanelList *p = 0;
    unsigned i;
    int nl = 0, nr = 0, nrec = 0, unread = 0, a, b;
    int dump = s_panel_log >= 2 && (s_frame % 300u) == 0u;     /* XBOX_PANEL_LOG=2: every record, every 300th frame */

    for (i = 0; i < PANEL_LISTS; i++)
        if (s_panel[i].base == base) { p = &s_panel[i]; break; }
    if (!p) {
        p = &s_panel[s_panel_next++ % PANEL_LISTS];
        p->base = base;
        p->frame = 0;
    }
    if (p->frame == s_frame) return p;
    p->frame = s_frame;
    p->found = p->n = 0;
    p->l = PANEL_EDGE_L;
    p->r = PANEL_EDGE_R;
    for (r = base; r < end && nrec < MAX_REC; r += 0x18u, nrec++) {
        uint32_t obj = MEM32(r);
        Box q;
        int left, right;
        if (mark_find(obj, base) >= 0) {                    /* the race HUD */
            if (s_panel_log && obj >= 0x1000u && MEM32(obj) == VT_WORLD_QUADS) {
                q = record_box(gfx, obj);
                if (box_valid(&q) && (fabsf(q.x0 - PANEL_OBJ_L) <= PANEL_TOL || fabsf(q.x1 - PANEL_OBJ_R) <= PANEL_TOL))
                    fprintf(stderr, "[PANEL] frame %u list %08X obj %08X: a race HUD record looks like an end piece "
                            "(%.1f..%.1f x %.1f..%.1f)\n", s_frame, base, obj, q.x0, q.x1, q.y0, q.y1);
            }
            continue;
        }
        q = record_box(gfx, obj);
        if (dump)
            fprintf(stderr, "[PANEL] frame %u state %d list %08X rec %d obj %08X vt %08X box %.1f..%.1f x %.1f..%.1f\n",
                    s_frame, s_frame_state, base, nrec, obj, obj >= 0x1000u ? MEM32(obj) : 0u, q.x0, q.x1, q.y0, q.y1);
        if (dump && box_valid(&q) && q.x1 - q.x0 >= 200.0f) panel_dump_vertices(gfx, obj);
        if (!box_valid(&q)) {
            if (obj >= 0x1000u) { unread++; unread_vt = MEM32(obj); }
            continue;
        }
        if (MEM32(obj) == VT_WORLD_QUADS) {
            /* the title's panel quads: from x = -20 to 660 (the bars' two
             * halves meet at 320), which its 2D transform puts at 10..632 */
            left = q.x1 - q.x0 >= PANEL_MIN_W && fabsf(q.x0 - PANEL_OBJ_L) <= PANEL_TOL;
            right = q.x1 - q.x0 >= PANEL_MIN_W && fabsf(q.x1 - PANEL_OBJ_R) <= PANEL_TOL;
            if (!left && !right) {
                /* A world quad is drawn from one of two 2D buffers, picked
                 * when it is drawn (gfx+0x20): record_box reads the first
                 * one, and its stale vertices at the same place can look
                 * valid -- the backing, drawn from the second one, was then
                 * missed now and then. The second buffer's box counts too. */
                Box q2 = box_from(gfx, obj, VT_WORLD_QUADS, VB_2D_ALT);
                if (box_valid(&q2) && q2.x1 - q2.x0 >= PANEL_MIN_W) {
                    left = fabsf(q2.x0 - PANEL_OBJ_L) <= PANEL_TOL;
                    right = fabsf(q2.x1 - PANEL_OBJ_R) <= PANEL_TOL;
                    if (left || right) {
                        if (s_panel_log >= 2)
                            fprintf(stderr, "[PANEL] frame %u list %08X obj %08X: end piece read from the second 2D buffer "
                                    "(%.1f..%.1f x %.1f..%.1f; first buffer %.1f..%.1f x %.1f..%.1f)\n", s_frame, base, obj,
                                    q2.x0, q2.x1, q2.y0, q2.y1, q.x0, q.x1, q.y0, q.y1);
                        q = q2;
                    }
                }
            }
        } else {
            if (q.x1 - q.x0 < PANEL_MIN_W) continue;
            if (q.x0 <= 2.0f || q.x1 >= 638.0f) continue;   /* fades, letterbox */
            left = fabsf(q.x0 - PANEL_EDGE_L) <= PANEL_TOL;
            right = fabsf(q.x1 - PANEL_EDGE_R) <= PANEL_TOL;
        }
        if (left && nl < PANEL_PIECES) { lb[nl] = q; lo[nl++] = obj; }
        if (right && nr < PANEL_PIECES) { rb[nr] = q; ro[nr++] = obj; }
    }
    for (a = 0; a < nl && !p->found; a++)
        for (b = 0; b < nr; b++)
            if (rows_meet(&lb[a], &rb[b])) { p->found = 1; break; }
    if (p->found) {
        /* the edges on screen: the race quads' own, else the title's 10..632 */
        p->l = PANEL_EDGE_L + PANEL_TOL;
        p->r = PANEL_EDGE_R - PANEL_TOL;
        for (a = 0; a < nl; a++) {
            if (MEM32(lo[a]) != VT_WORLD_QUADS && lb[a].x0 < p->l) p->l = lb[a].x0;
            p->piece[p->n++] = lo[a];
        }
        for (b = 0; b < nr; b++) {
            int dup = 0;
            if (MEM32(ro[b]) != VT_WORLD_QUADS && rb[b].x1 > p->r) p->r = rb[b].x1;
            for (a = 0; a < nl; a++) if (lo[a] == ro[b]) dup = 1;
            if (!dup && p->n < PANEL_PIECES) p->piece[p->n++] = ro[b];
        }
    }
    if (s_panel_log && (nl || nr || p->found))
        panel_log(p, nrec, unread, unread_vt, lb, nl, rb, nr);
    return p;
}

static int panel_piece(const PanelList *p, uint32_t obj)
{
    int i;
    for (i = 0; i < p->n; i++) if (p->piece[i] == obj) return 1;
    return 0;
}

static uint32_t q14(float v)
{
    long q = lroundf(v * 16.0f);
    return q < 0 ? 0u : q > 0x3FFF ? 0x3FFFu : (uint32_t)q;
}

/* The panel tags of a record of a list holding a panel; 0 = none (the
 * record keeps the tags it had before). */
static uint32_t panel_tag(uint32_t obj, uint32_t view, uint32_t *tag_y)
{
    const PanelList *p;
    uint32_t base = view + VIEW_LIST;
    if (s_panel_log >= 3 && (s_frame % 300u) == 0u)
        fprintf(stderr, "[PANEL] tag frame %u state %d view %08X obj %08X\n", s_frame, s_frame_state, view, obj);
    if (!g_panel_on || s_frame_state < 0) return 0;
    p = panel_of_list(base, MEM32(view + VIEW_LIST_END));
    if (!p->found) return 0;
    if (panel_piece(p, obj)) {
        /* at 4:3 (no race 2D), x as drawn and the ends to the screen's edges */
        *tag_y = (HUD_TAG_MAGIC_PANEL_R << 16) | q14(p->r - PANEL_SNAP);
        return (HUD_TAG_MAGIC_PANEL << 16) | (s_frame_wide ? HUD_PANEL_WIDE_ONLY : 0u) |
               q14(p->l + PANEL_SNAP);
    }
    if (!g_race2d_on || s_frame_wide || full_width(obj)) return 0;
    *tag_y = HUD_TAG_MAGIC_PANEL_R << 16;               /* before the race: 4:3 proportions */
    return (HUD_TAG_MAGIC_PANEL << 16) | HUD_PANEL_SCALE_ONLY;
}

/* The HUD lists are rebuilt by every RenderFrame, extra renders included
 * (the HUD methods run in the render): a list drawn again with no HUD call
 * since its last draw holds no race HUD, whatever objects it reuses (the
 * per-frame arena hands the same addresses out again, so the marks of the
 * last race frame would otherwise match the end screens' records, one frame
 * in two). */
static void list_drawn(uint32_t base)
{
    unsigned b = buf_index(base);
    if (b >= NBUF || s_buf[b].drawn_frame == s_frame) return;
    s_buf[b].drawn_frame = s_frame;
    if (s_buf[b].calls == s_buf[b].calls_at_draw) {
        s_buf[b].gen = ++s_gen;             /* no HUD in this fill: its marks are gone */
        s_stale++;
    }
    s_buf[b].calls_at_draw = s_buf[b].calls;
}

uint32_t hud_anchor_tag(uint32_t obj, uint32_t view, uint32_t *tag_y)
{
    uint32_t base = view + VIEW_LIST;
    int h;
    *tag_y = 0;
    list_drawn(base);
    h = mark_find(obj, base);
    if (h < 0) {
        uint32_t t = panel_tag(obj, view, tag_y);
        if (t) return t;
        /* without the 16:9 frame, only the race's own 2D is tagged: out of
         * a race the push buffer stays the title's */
        if (!g_box_on && !(g_race2d_on && s_frame_wide)) return 0;
        return (HUD_TAG_MAGIC_BOX << 16) | (full_width(obj) ? HUD_BOX_OFF : HUD_BOX_ON);
    }
    if (!g_hud_anchor_on && !g_box_on) return 0;     /* 4:3 at 100 %: marked for the panels only */
    if (!g_hud_anchor_on) return (HUD_TAG_MAGIC_BOX << 16) | HUD_BOX_OFF;     /* race HUD like the Xbox */
    if (s_tag[h].frame != s_frame) {
        static LARGE_INTEGER f;
        LARGE_INTEGER t0, t1;
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        work_out_list(MEM32(GFX_GLOBAL), base, MEM32(view + VIEW_LIST_END));
        QueryPerformanceCounter(&t1);
        s_work_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    }
    if ((++s_found % 100000u) == 1u)
        fprintf(stderr, "[HUD] elements %llu, records marked %llu, drawn %llu, table full %llu, lists without HUD %llu ; %.4f ms per list\n",
                s_elements, s_marked, s_found, s_full, s_stale, s_lists ? s_work_ms / (double)s_lists : 0.0);
    if (s_tag[h].frame != s_frame) return 0;
    *tag_y = s_tag[h].tag_y;
    return s_tag[h].tag;
}

void (*hud_anchor_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x000C6730u) return hook_hud_000C6730;
    if (xbox_va == 0x000C44E0u) return hook_hud_000C44E0;
    if (xbox_va == 0x000C9BB0u) return hook_hud_000C9BB0;
    if (xbox_va == 0x000C9D70u) return hook_hud_000C9D70;
    if (xbox_va == 0x000D2D80u) return hook_hud_000D2D80;
    return 0;
}
