/*
 * drawdist -- draw distance Original / Far / Max. See drawdist.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "recomp/recomp_types.h"
#include "drawdist.h"

void sub_000DE230(void);    /* TerrainNode_TickVisibilityUpdate (thiscall, ret) */
void sub_000FFB50(void);    /* SceneView_RenderPass */
void sub_000F8F10(void);    /* BoardMesh_DrawAttachedPatches */
void sub_000F8BA0(void);    /* terrain patch re-tessellation into a cache slot */

#define APP_GLOBAL   0x001E3C7Cu
#define CELL_COUNT   0x001FAF88u + 0x2C4u    /* list 0x1FAF88+0x3C, count at +0x288 */
#define GRID_PTR     0x001FAF8Cu
#define FAR_MUL      0x001C0834u
#define PATCH_LOD    0x001BD160u
#define PATCH_SLOTS  0x001BD158u
#define LIST_SAFE    150                      /* of 162 entries */
#define CELL         10000.0f                 /* the title's divisor (0xDE28C), not the grid's own */

int g_drawdist_level;
static int s_log;
static float s_mul;

static struct {
    uint32_t lvl, cfg;
    float o60, o64, w60, w64;       /* the title's radii, and what we wrote */
    float cap60, cap64;             /* radius caps for this level's grid */
    float ofar, wfar, otl, wtl;     /* far multiplier, terrain patch distance */
    int   far_set, tl_set;
    int   cut;                      /* run-time guard: cells taken off the radius */
    unsigned rp_since_tick;         /* SceneView_RenderPass calls since the last terrain tick */
    unsigned max_cells, cuts, overflow, ticks;
    unsigned pool_slots, max_claims, reuse, calls;
    unsigned long long claims;
} s;

static float rdf(uint32_t va) { float f; uint32_t u = MEM32(va); memcpy(&f, &u, 4); return f; }
static void  wrf(uint32_t va, float f) { uint32_t u; memcpy(&u, &f, 4); MEM32(va) = u; }

/* Cells the title takes for radius R: k = int((R - 1) / 10000) + 1 (0xDE27D). */
static int cells_of(float r) { return (int)((r - 1.0f) / CELL) + 1; }

/* Most non-empty cells in any (2k+1)^2 window of the grid (windows centred
 * outside the grid are subsets of ones centred on its edge). */
static int worst_window(int k)
{
    uint32_t g = MEM32(GRID_PTR), tab;
    int w, h, x, y, best = 0;
    if (!g) return 0;
    w = (int)MEM32(g + 0x2Cu); h = (int)MEM32(g + 0x30u); tab = MEM32(g + 0x4Cu);
    if (w <= 0 || h <= 0 || w > 512 || h > 512 || !tab) return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int n = 0, yy, xx;
            for (yy = y - k < 0 ? 0 : y - k; yy <= y + k && yy < h; yy++)
                for (xx = x - k < 0 ? 0 : x - k; xx <= x + k && xx < w; xx++)
                    n += MEM32(tab + 4u * (uint32_t)(yy * w + xx)) != 0;
            if (n > best) best = n;
        }
    return best;
}

/* Largest radius in [orig, orig * mul] whose worst window fits the list. */
static float radius_cap(float orig, int *worst_out)
{
    float want = orig * s_mul;
    int k = cells_of(want), k0 = cells_of(orig), wv = 0;
    for (; k > k0; k--) {
        wv = worst_window(k);
        if (wv <= LIST_SAFE) break;
    }
    if (k <= k0) { wv = worst_window(k0); *worst_out = wv; return orig; }
    *worst_out = wv;
    return want < k * CELL ? want : k * CELL;
}

static void level_report(const char *why)
{
    if (!s.ticks) return;
    fprintf(stderr, "[DRAWDIST] %s: %u ticks, cells max %u / 162, guard cuts %u, overflow %u; "
            "patch cache %u slots: claims max %u per call, slot reused within a call %u (%u calls)\n",
            why, s.ticks, s.max_cells, s.cuts, s.overflow, s.pool_slots, s.max_claims, s.reuse, s.calls);
    fflush(stderr);
}

static void new_level(uint32_t lvl, uint32_t cfg)
{
    int w60 = 0, w64 = 0;
    level_report("level end");
    s.max_cells = s.cuts = s.overflow = s.ticks = s.max_claims = s.reuse = s.calls = 0;
    s.claims = 0; s.cut = 0;
    s.lvl = lvl; s.cfg = cfg;
    s.o60 = rdf(cfg + 0x60u); s.o64 = rdf(cfg + 0x64u);
    if (g_drawdist_level) {
        s.cap60 = radius_cap(s.o60, &w60);
        s.cap64 = radius_cap(s.o64, &w64);
    } else {
        s.cap60 = s.o60; s.cap64 = s.o64;
        w60 = worst_window(cells_of(s.o60)); w64 = worst_window(cells_of(s.o64));
    }
    s.w60 = s.cap60; s.w64 = s.cap64;
    {
        uint32_t g = MEM32(GRID_PTR);
        fprintf(stderr, "[DRAWDIST] level: %s x%.1f, grid %ux%u, radius %.0f -> %.0f (worst %d cells), "
                "split screen %.0f -> %.0f (worst %d)\n",
                g_drawdist_level == 2 ? "Max" : g_drawdist_level == 1 ? "Far" : "Original", s_mul,
                g ? MEM32(g + 0x2Cu) : 0u, g ? MEM32(g + 0x30u) : 0u,
                s.o60, s.cap60, w60, s.o64, s.cap64, w64);
        fflush(stderr);
    }
}

/* Before the title's tick: this level's values, then the guard's cut. */
static void prepare(void)
{
    uint32_t g = MEM32(APP_GLOBAL), lvl = g ? MEM32(g + 0x72Cu) : 0, cfg;
    float r60, r64, tl;
    if (!lvl || !(cfg = MEM32(lvl + 0x2Cu))) return;
    r60 = rdf(cfg + 0x60u); r64 = rdf(cfg + 0x64u);
    if (lvl != s.lvl || cfg != s.cfg || r60 != s.w60 || r64 != s.w64) new_level(lvl, cfg);
    s.ticks++;
    if (!g_drawdist_level) return;
    s.w60 = s.cap60 - (float)s.cut * CELL; if (s.w60 < s.o60) s.w60 = s.o60;
    s.w64 = s.cap64 - (float)s.cut * CELL; if (s.w64 < s.o64) s.w64 = s.o64;
    wrf(cfg + 0x60u, s.w60); wrf(cfg + 0x64u, s.w64);
    tl = rdf(PATCH_LOD);
    if (!s.tl_set || tl != s.wtl) { s.otl = tl; s.wtl = tl * s_mul; s.tl_set = 1; }
    wrf(PATCH_LOD, s.wtl);
}

/* After it: the list it built. */
static void check(void)
{
    unsigned n = MEM32(CELL_COUNT);
    if (n > s.max_cells) s.max_cells = n;
    if (n > 162) {
        if (!s.overflow++) { fprintf(stderr, "[DRAWDIST] OVERFLOW: %u cells\n", n); fflush(stderr); }
    }
    if (g_drawdist_level && n >= LIST_SAFE && s.w60 > s.o60) {
        s.cut++; s.cuts++;
        fprintf(stderr, "[DRAWDIST] guard: %u cells, radius cut by one cell (%d)\n", n, s.cut);
        fflush(stderr);
    }
    if (s.ticks % 1800u == 0) level_report("running");     /* ~30 s; tests end by a kill */
}

void hook_drawdist_000DE230(void)
{
    if (!g_drawdist_level && !s_log) { sub_000DE230(); return; }
    s.rp_since_tick = 0;
    prepare();
    sub_000DE230();
    check();
}

/* Far plane: [0x1C0834] times the multiplier while a race runs (the terrain
 * tick ran lately -- it also runs in pause), the title's own value otherwise;
 * the terrain patch distance goes back too, for the front end. */
void hook_drawdist_000FFB50(void)
{
    if (g_drawdist_level) {
        float f = rdf(FAR_MUL);
        int racing = s.rp_since_tick < 400u;
        if (!s.far_set || f != s.wfar) { s.ofar = f; s.far_set = 1; }   /* the title's own value */
        s.wfar = racing ? s.ofar * s_mul : s.ofar;
        if (!racing && s.tl_set && rdf(PATCH_LOD) == s.wtl) { wrf(PATCH_LOD, s.otl); s.tl_set = 0; }
        if (f != s.wfar) wrf(FAR_MUL, s.wfar);
        if (s.rp_since_tick < 0xFFFFFFFFu) s.rp_since_tick++;
    }
    sub_000FFB50();
}

static uint32_t s_slot_va[4096];
static unsigned s_slot_stamp[4096];
static unsigned s_claims_call;

void hook_drawdist_000F8F10(void)
{
    if (!g_drawdist_level && !s_log) { sub_000F8F10(); return; }
    s.calls++; s_claims_call = 0;
    s.pool_slots = MEM32(PATCH_SLOTS);
    sub_000F8F10();
    if (s_claims_call > s.max_claims) s.max_claims = s_claims_call;
}

void hook_drawdist_000F8BA0(void)
{
    if (g_drawdist_level || s_log) {
        uint32_t vb = MEM32(g_esp + 4u);
        unsigned h = (vb / 48u) & 4095u, i;
        s_claims_call++; s.claims++;
        for (i = 0; i < 4096; i++, h = (h + 1) & 4095u) {
            if (s_slot_stamp[h] != s.calls) { s_slot_stamp[h] = s.calls; s_slot_va[h] = vb; break; }
            if (s_slot_va[h] == vb) { s.reuse++; break; }
        }
    }
    sub_000F8BA0();
}

static void at_exit(void) { level_report("exit"); }

void drawdist_init(void)
{
    const char *e = getenv("XBOX_DRAW_DISTANCE");
    int v = 0;
    if (e && e[0]) {
        if (!_stricmp(e, "far") || !strcmp(e, "1")) v = 1;
        else if (!_stricmp(e, "max") || !strcmp(e, "2")) v = 2;
    }
    g_drawdist_level = v;
    s_mul = v == 2 ? 2.0f : v == 1 ? 1.5f : 1.0f;
    e = getenv("XBOX_DRAW_DISTANCE_LOG");
    s_log = e && e[0] == '1';
    if (v || s_log) atexit(at_exit);
    fprintf(stderr, "[DRAWDIST] XBOX_DRAW_DISTANCE=%s%s\n",
            v == 2 ? "max (x2)" : v == 1 ? "far (x1.5)" : "original", s_log ? ", log" : "");
}

void (*drawdist_lookup(unsigned int xbox_va))(void)
{
    if (!g_drawdist_level && !s_log) return 0;
    if (xbox_va == 0x000DE230u) return hook_drawdist_000DE230;
    if (xbox_va == 0x000FFB50u) return hook_drawdist_000FFB50;
    if (xbox_va == 0x000F8F10u) return hook_drawdist_000F8F10;
    if (xbox_va == 0x000F8BA0u) return hook_drawdist_000F8BA0;
    return 0;
}
