/*
 * drawdist -- draw distance Original / Far / Max. See drawdist.h.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "recomp/recomp_types.h"
#include "drawdist.h"
#include "terrain_seams.h"
#include "lightmap_map.h"
#include "../../xboxrecomp/src/kernel/xbox_logstats.h"

void d3d8_VeilSetCullFn(int (*fn)(float *out));     /* xboxrecomp d3d8_veil */
void sub_000DE230(void);    /* TerrainNode_TickVisibilityUpdate (thiscall, ret) */
void sub_000FFB50(void);    /* SceneView_RenderPass */
void sub_000F8F10(void);    /* BoardMesh_DrawAttachedPatches */
void sub_000F8BA0(void);    /* terrain patch re-tessellation into a cache slot */
void sub_000DFE90(void);    /* FogVolume_DrawCloudPuffs (thiscall, ret 16) -- log only */
void sub_000E6670(void);    /* particle emitter render (thiscall) -- log only */
void sub_000FF260(void);    /* add one terrain patch to this frame's draw lists (thiscall, ret 16) */
void sub_001013D0(void);    /* add one draw item to this frame's view command list (thiscall, ret 32) */

#define APP_GLOBAL   0x001E3C7Cu
#define CELL_COUNT   0x001FAF88u + 0x2C4u    /* list 0x1FAF88+0x3C, count at +0x288 */
#define GRID_PTR     0x001FAF8Cu
#define FAR_MUL      0x001C0834u
#define PATCH_LOD    0x001BD160u
#define PATCH_SLOTS  0x001BD158u
#define FOG_FAR      0x0019CFCCu              /* fog volumes: none drawn beyond (20000) */
#define FOG_NEAR     0x0019CFD0u              /* ... fade start (12500); the draw ignores the fade */
#define LIST_SAFE    150                      /* of 162 entries */
#define CELL         10000.0f                 /* the title's divisor (0xDE28C), not the grid's own */

/* The terrain patches of a frame go into a node pool of the renderer:
 * 0x14000 bytes allocated once (0x104B0F), two buffers of 0xA000 = 2560
 * nodes of 16 bytes ([r+0x18E5C8], [r+0x18E5CC]). Two cursors share a buffer,
 * reset at each frame (0xFF972): one going up from its start (patches drawn
 * as they are, 0xFF362 -- no bound at all) and one going down from its top
 * (patches re-tessellated, 0xFF2F8 -- bounded by the patch cache only). The
 * title's own distances stay below; Far / Max can ask for more, the cursors
 * then cross, nodes go into two lists at once and two of them can point at
 * each other: BoardMesh_DrawAttachedPatches then loops for ever. */
#define NODE_UP      0x18E594u                /* renderer: next node going up */
#define NODE_DOWN    0x18E598u                /* renderer: next node going down */
#define NODE_POOL    2560                     /* nodes in one buffer */
#define NODE_MARGIN  160                      /* run-time guard: cut the radius at POOL - MARGIN */

/* The same kind of limit for the draw items: each object drawn
 * (0x1013D0) takes a 0x60-byte record from a per-frame pool ([r+0x18E5A8],
 * two buffers of 1700 at [r+0x18E5E0] / [r+0x18E5E4], no bound) and a block
 * of 0xE0 bytes counted by [r+0x1A788] in an array of 4 sets of 1700 inside
 * the renderer (shared with 0x1017E0, see pumplag.c). Past 1700 the records
 * run into the other buffer and the blocks into the next set (the last set:
 * into the renderer's per-frame pool pointers, r+0x18E390..). Far / Max
 * reach 1597 at Elysium; the title's own distances stay under 700. */
#define ITEM_CUR     0x18E5A8u                /* renderer: next draw item record */
#define ITEM_BUF     0x18E5E0u                /* renderer: the two buffers */
#define ITEM_SIZE    0x60u
#define ITEM_COUNT   0x1A788u                 /* renderer: blocks taken this frame (pumplag.c CTX_SLOT) */
#define ITEM_POOL    1700
#define ITEM_MARGIN  (ITEM_POOL / 16)         /* same ratio as the node guard (160 / 2560) */

int g_drawdist_level;
static int s_log;
static int s_fx = 1;                /* also push the fog volume distance (XBOX_DRAW_DISTANCE_FX) */
static int s_nodeguard = 1;         /* node pool guards (XBOX_DRAW_DISTANCE_GUARD=0: off, tests only) */
static int s_nodecap = NODE_POOL;   /* nodes the guards allow (XBOX_DRAW_DISTANCE_NODECAP, tests only) */
static int s_pool = NODE_POOL;      /* nodes in one buffer (XBOX_DRAW_DISTANCE_NODESHRINK, tests only) */
static int s_itemroom = ITEM_POOL;  /* draw items left per frame (XBOX_DRAW_DISTANCE_ITEMSHRINK, tests only) */
static float s_mul;

static struct {
    uint32_t lvl, cfg;
    float o60, o64, w60, w64;       /* the title's radii, and what we wrote */
    float cap60, cap64;             /* radius caps for this level's grid */
    float ofar, wfar, otl, wtl;     /* far multiplier, terrain patch distance */
    int   far_set, tl_set;
    float ofogf, ofogn, wfogf;      /* fog volume distance: the title's, what we wrote */
    int   fog_set;
    int   cut;                      /* run-time guard: cells taken off the radius */
    unsigned rp_since_tick;         /* SceneView_RenderPass calls since the last terrain tick */
    unsigned max_cells, cuts, overflow, ticks;
    unsigned pool_slots, max_claims, reuse, calls;
    unsigned long long claims;
    unsigned max_cmds;              /* view command list (2048) use */
    unsigned node_win;              /* patch nodes asked for since the last terrain tick */
    unsigned node_max, node_over, node_cuts, node_drops, node_drop_ticks;
    unsigned drop_tick;             /* last terrain tick with a drop */
    unsigned item_win;              /* draw items asked for since the last terrain tick */
    unsigned item_max, item_over, item_cuts, item_drops, item_drop_ticks, item_drop_tick;
} s;

/* Log only: the renderer's other per-frame pools, same scheme as the
 * node pool: [r+cur] the cursor, [r+base] / [r+base+4] the two buffers,
 * reset at each frame (0xFF972). Used = cursor - start of its buffer. */
static const struct { uint32_t cur, base; unsigned elem; const char *name; } k_pool[] = {
    { 0x18E59Cu, 0x18E5D0u, 16, "D0" },  { 0x18E5A0u, 0x18E5D8u, 12, "D8" },
    { 0x18E5A8u, 0x18E5E0u, 96, "E0" },  { 0x18E5ACu, 0x18E5E8u, 32, "E8" },
    { 0x18E5A4u, 0x18E5F8u, 28, "F8" },  { 0x18E5B4u, 0x18E600u, 16, "600" },
    { 0x18E5B8u, 0x18E608u, 16, "608" }, { 0x18E5BCu, 0x18E610u, 80, "610" },
    { 0x18E5C0u, 0x18E618u, 80, "618" }, { 0x18E5C4u, 0x18E620u, 48, "clouds" },
};
#define NPOOL (sizeof k_pool / sizeof k_pool[0])
static unsigned s_pool_max[NPOOL], s_pool_cap[NPOOL];

static void pools_sample(void)
{
    uint32_t g = MEM32(APP_GLOBAL), r = g ? MEM32(g + 0x720u) : 0;
    unsigned i;
    if (!r) return;
    for (i = 0; i < NPOOL; i++) {
        uint32_t cur = MEM32(r + k_pool[i].cur), b0 = MEM32(r + k_pool[i].base), b1 = MEM32(r + k_pool[i].base + 4u);
        uint32_t lo = b0 < b1 ? b0 : b1, hi = b0 < b1 ? b1 : b0, st;
        unsigned used;
        if (!lo || hi == lo || cur < lo) continue;
        st = cur >= hi ? hi : lo;
        used = (cur - st) / k_pool[i].elem;
        s_pool_cap[i] = (hi - lo) / k_pool[i].elem;
        if (used > s_pool_max[i] && used < 1000000u) s_pool_max[i] = used;
    }
}

/* Log only: where effects start being drawn. An object (fog volume, emitter)
 * drawn now and not in the last FX_GAP terrain ticks "appears"; its distance
 * to the camera goes into a histogram of FX_BIN-unit bins. */
#define FX_GAP  8u
#define FX_BIN  2500.0f
#define FX_BINS 40
enum { FX_FOG, FX_EMIT, FX_KINDS };
static const char *const fx_name[FX_KINDS] = { "fog volumes", "emitters" };
static struct {
    uint32_t key[4096];
    unsigned seen[4096];
    unsigned hist[FX_BINS + 1];
    unsigned appear, calls;
    float dmax;
} fx[FX_KINDS];

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
    fprintf(stderr, "[DRAWDIST] %s: patch nodes max %u / %d (ticks over %u), node guard cuts %u, "
            "patches dropped %u in %u ticks%s\n",
            why, s.node_max, s_pool, s.node_over, s.node_cuts, s.node_drops, s.node_drop_ticks,
            s_nodeguard ? "" : " (guards off)");
    fprintf(stderr, "[DRAWDIST] %s: draw items max %u / %d (ticks over %u), item guard cuts %u, "
            "items dropped %u in %u ticks%s\n",
            why, s.item_max, ITEM_POOL, s.item_over, s.item_cuts, s.item_drops, s.item_drop_ticks,
            s_itemroom != ITEM_POOL ? " (TEST: pool shrunk)" : "");
    if (s_log) {
        int k, b;
        unsigned i;
        fprintf(stderr, "[DRAWDIST] %s: frame pools max / size:", why);
        for (i = 0; i < NPOOL; i++) fprintf(stderr, " %s %u/%u", k_pool[i].name, s_pool_max[i], s_pool_cap[i]);
        fputc('\n', stderr);
        fprintf(stderr, "[DRAWDIST] %s: view commands max %u / 2048, fog cut %.0f\n",
                why, s.max_cmds, rdf(FOG_FAR));
        for (k = 0; k < FX_KINDS; k++) {
            fprintf(stderr, "[DRAWDIST] %s: %s drawn %u, appearances %u, farthest drawn %.0f; appear at (x%.0f):",
                    why, fx_name[k], fx[k].calls, fx[k].appear, fx[k].dmax, FX_BIN);
            for (b = 0; b <= FX_BINS; b++) if (fx[k].hist[b]) fprintf(stderr, " %d:%u", b, fx[k].hist[b]);
            fputc('\n', stderr);
        }
    }
    xbox_log_flush();
}

static void new_level(uint32_t lvl, uint32_t cfg)
{
    int w60 = 0, w64 = 0;
    level_report("level end");
    s.max_cells = s.cuts = s.overflow = s.ticks = s.max_claims = s.reuse = s.calls = 0;
    s.claims = 0; s.cut = 0; s.max_cmds = 0;
    s.item_win = s.item_max = s.item_over = s.item_cuts = s.item_drops = s.item_drop_ticks = 0;
    s.item_drop_tick = 0;
    s.node_win = s.node_max = s.node_over = s.node_cuts = s.node_drops = s.node_drop_ticks = 0;
    s.drop_tick = 0;
    memset(s_pool_max, 0, sizeof s_pool_max);
    memset(fx, 0, sizeof fx);
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
        xbox_log_flush();
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
    if (s_fx) {
        /* Fog volumes (FogMan_RenderFogVolumes 0xDFA00) come from the visible
         * cells but are cut at FOG_FAR from the camera -- a hard cut, the
         * fade it computes is not used by the draw. Push it out by the radius
         * actually applied, so the title's fog / scenery ratio holds. */
        float k = s.o60 > 0.0f ? s.w60 / s.o60 : 1.0f;
        if (!s.fog_set) { s.ofogf = rdf(FOG_FAR); s.ofogn = rdf(FOG_NEAR); s.fog_set = 1; }
        if (k < 1.0f) k = 1.0f;
        s.wfogf = s.ofogf * k;
        wrf(FOG_FAR, s.wfogf); wrf(FOG_NEAR, s.ofogn * k);
    }
}

/* After it: the list it built. */
static void check(void)
{
    unsigned n = MEM32(CELL_COUNT);
    if (n > s.max_cells) s.max_cells = n;
    if (n > 162) {
        if (!s.overflow++) { fprintf(stderr, "[DRAWDIST] OVERFLOW: %u cells\n", n); xbox_log_flush(); }
    }
    if (g_drawdist_level && n >= LIST_SAFE && s.w60 > s.o60) {
        s.cut++; s.cuts++;
        fprintf(stderr, "[DRAWDIST] guard: %u cells, radius cut by one cell (%d)\n", n, s.cut);
        xbox_log_flush();
    } else if (g_drawdist_level && s_nodeguard && s.node_win >= (unsigned)(s_nodecap - NODE_MARGIN)
               && s.w60 > s.o60) {
        /* the frame's patches came close to the node pool: one cell less */
        s.cut++; s.node_cuts++;
        fprintf(stderr, "[DRAWDIST] node guard: %u patch nodes of %d, radius cut by one cell (%d) at tick %u\n",
                s.node_win, s_nodecap, s.cut, s.ticks);
        xbox_log_flush();
    } else if (g_drawdist_level && s_nodeguard && s.item_win >= (unsigned)(ITEM_POOL - ITEM_MARGIN)
               && s.w60 > s.o60) {
        /* the frame's draw items came close to their pool: one cell less */
        s.cut++; s.item_cuts++;
        fprintf(stderr, "[DRAWDIST] item guard: %u draw items of %d, radius cut by one cell (%d) at tick %u\n",
                s.item_win, ITEM_POOL, s.cut, s.ticks);
        xbox_log_flush();
    }
    if (s.item_win > s.item_max) s.item_max = s.item_win;
    if (s.item_win > (unsigned)ITEM_POOL) s.item_over++;
    s.item_win = 0;
    if (s.node_win > s.node_max) s.node_max = s.node_win;
    if (s.node_win > (unsigned)s_pool) s.node_over++;
    s.node_win = 0;
    if (s_log) pools_sample();
    if ((s_log || xbox_log_stats()) && s.ticks % (s_log ? 600u : 1800u) == 0)
        level_report("running");        /* tests end by a kill; XBOX_LOG_STATS=1 or XBOX_DRAW_DISTANCE_LOG */
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
    if (s_log) pools_sample();
    if (s_log) {
        uint32_t view = g_ecx, g = MEM32(APP_GLOBAL), gfx = g ? MEM32(g + 0x720u) : 0;
        int32_t n = (int32_t)(MEM32(view + 0x808u) - view - 0x8C0u) / 0x18;
        (void)gfx;
        if (n > (int32_t)s.max_cmds && n < 100000) s.max_cmds = (unsigned)n;
    }
    if (g_drawdist_level) {
        float f = rdf(FAR_MUL);
        int racing = s.rp_since_tick < 400u;
        if (!s.far_set || f != s.wfar) { s.ofar = f; s.far_set = 1; }   /* the title's own value */
        s.wfar = racing ? s.ofar * s_mul : s.ofar;
        if (!racing && s.tl_set && rdf(PATCH_LOD) == s.wtl) { wrf(PATCH_LOD, s.otl); s.tl_set = 0; }
        if (!racing && s.fog_set && rdf(FOG_FAR) != s.ofogf) { wrf(FOG_FAR, s.ofogf); wrf(FOG_NEAR, s.ofogn); }
        if (f != s.wfar) wrf(FAR_MUL, s.wfar);
        if (s.rp_since_tick < 0xFFFFFFFFu) s.rp_since_tick++;
    }
    sub_000FFB50();
}

/* SceneView: one terrain patch for this frame's draw lists (ecx = renderer).
 * Nodes left = (down cursor + 16 - up cursor) / 16. With none left the patch
 * is not added this frame (Far / Max only): no node is ever handed out twice. */
void hook_drawdist_000FF260(void)
{
    uint32_t r = g_ecx;
    int32_t left;
    unsigned want;
    if (s_pool != NODE_POOL && MEM32(r + NODE_DOWN) - MEM32(r + NODE_UP) == 0x9FF0u)
        /* tests only: first patch of the frame, the pool made s_pool nodes long */
        MEM32(r + NODE_DOWN) = MEM32(r + NODE_UP) + 16u * (uint32_t)(s_pool - 1);
    left = ((int32_t)(MEM32(r + NODE_DOWN) - MEM32(r + NODE_UP)) + 16) / 16;
    want = (unsigned)(s_pool - left + 1);
    if (want > s.node_win) s.node_win = want;
    if (g_drawdist_level && s_nodeguard && left <= s_pool - s_nodecap) {
        if (!s.node_drops++) {
            fprintf(stderr, "[DRAWDIST] node guard: patch node pool full (%d), patch not drawn this frame (tick %u)\n",
                    s_nodecap, s.ticks);
            xbox_log_flush();
        }
        if (s.drop_tick != s.ticks || !s.node_drop_ticks) { s.drop_tick = s.ticks; s.node_drop_ticks++; }
        g_esp += 4u + 16u;          /* ret 16 */
        return;
    }
    sub_000FF260();
}

/* SceneView: one draw item for this frame (ecx = renderer). 0x1013D0 has one
 * direct caller, already routed to hook_plag_001013D0 (pumplag.c), which
 * calls this in place of the title's function. Taken so far = the larger of
 * the records used in the current buffer and the block count ([r+0x1A788]).
 * With the pool full the item is not drawn this frame (Far / Max only;
 * Original: the title's function, as is). */
void drawdist_item(void)
{
    uint32_t r = g_ecx, cur = MEM32(r + ITEM_CUR), b0 = MEM32(r + ITEM_BUF), b1 = MEM32(r + ITEM_BUF + 4u);
    uint32_t lo = b0 < b1 ? b0 : b1, hi = b0 < b1 ? b1 : b0, st = cur >= hi ? hi : lo;
    unsigned used, cnt;
    if (!g_drawdist_level && !s_log) { sub_001013D0(); return; }
    if (s_itemroom != ITEM_POOL && (cur == lo || cur == hi)) {
        /* tests only: first item of the frame, the pool made s_itemroom items long */
        uint32_t skip = (uint32_t)(ITEM_POOL - s_itemroom);
        MEM32(r + ITEM_CUR) = cur = cur + skip * ITEM_SIZE;
        MEM32(r + ITEM_COUNT) = MEM32(r + ITEM_COUNT) + skip;
    }
    used = cur >= st ? (cur - st) / ITEM_SIZE : 0;
    cnt = MEM32(r + ITEM_COUNT);
    if (cnt > used && cnt < 100000u) used = cnt;
    if (used + 1u > s.item_win) s.item_win = used + 1u;
    if (g_drawdist_level && s_nodeguard && used >= (unsigned)ITEM_POOL) {
        if (!s.item_drops++) {
            fprintf(stderr, "[DRAWDIST] item guard: draw item pool full (%d), item not drawn this frame (tick %u)\n",
                    ITEM_POOL, s.ticks);
            xbox_log_flush();
        }
        if (s.item_drop_tick != s.ticks || !s.item_drop_ticks) { s.item_drop_tick = s.ticks; s.item_drop_ticks++; }
        g_esp += 4u + 32u;          /* ret 32 */
        return;
    }
    sub_001013D0();
}

static uint32_t s_slot_va[4096];
static unsigned s_slot_stamp[4096];
static unsigned s_claims_call;

void hook_drawdist_000F8F10(void)
{
    /* the lighting map (lightmap_map.h): the renderer, arg 1, holds the level's texture table */
    if (g_lmap_armed) lightmap_map_renderer(MEM32(g_esp + 4u));
    if (!g_drawdist_level && !s_log) { sub_000F8F10(); return; }
    s.calls++; s_claims_call = 0;
    s.pool_slots = MEM32(PATCH_SLOTS);
    sub_000F8F10();
    if (s_claims_call > s.max_claims) s.max_claims = s_claims_call;
}

void hook_drawdist_000F8BA0(void)
{
    uint32_t obj = g_ecx, slot = MEM32(g_esp + 4u), patch = MEM32(g_esp + 8u);
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
    /* texture seams between patches (terrain_seams.h): which patch this slot holds */
    if (g_terrain_seams) terrain_seams_note(obj, slot, patch);
    /* the lighting map seen from above (lightmap_map.h): which patch this slot holds */
    if (g_lmap_armed) lightmap_map_note(obj, slot, patch);
}

static void fx_seen(int k, uint32_t obj, uint32_t lo, uint32_t hi)
{
    uint32_t g = MEM32(APP_GLOBAL), lvl = g ? MEM32(g + 0x72Cu) : 0, cam;
    unsigned h = (obj >> 4) & 4095u, i, last = 0, found = 0;
    float dx, dy, dz, d;
    if (!lvl) return;
    cam = lvl + 0xB0u + (MEM32(lvl + 0x290u) << 7);
    dx = 0.5f * (rdf(lo) + rdf(hi)) - rdf(cam);
    dy = 0.5f * (rdf(lo + 4u) + rdf(hi + 4u)) - rdf(cam + 4u);
    dz = 0.5f * (rdf(lo + 8u) + rdf(hi + 8u)) - rdf(cam + 8u);
    d = sqrtf(dx * dx + dy * dy + dz * dz);
    fx[k].calls++;
    if (d > fx[k].dmax) fx[k].dmax = d;
    for (i = 0; i < 4096; i++, h = (h + 1) & 4095u) {
        if (!fx[k].key[h] || fx[k].key[h] == obj) { found = fx[k].key[h] == obj; break; }
    }
    if (i == 4096) return;
    last = fx[k].seen[h];
    if (!found || s.ticks - last > FX_GAP) {
        int b = (int)(d / FX_BIN);
        fx[k].appear++;
        fx[k].hist[b < FX_BINS ? b : FX_BINS]++;
        if (s_log > 1) { fprintf(stderr, "[DRAWDIST] appear: %s at %.0f\n", fx_name[k], d); xbox_log_flush(); }
    }
    fx[k].key[h] = obj; fx[k].seen[h] = s.ticks;
}

/* FogVolume_DrawCloudPuffs: one fog volume that passed the distance test;
 * arg 1 is the cell sub-object, bounds at +0x44 / +0x50 (centre as 0xDFA00). */
void hook_drawdist_000DFE90(void)
{
    uint32_t sub = MEM32(g_esp + 4u);
    if (s_log) fx_seen(FX_FOG, sub, sub + 0x44u, sub + 0x50u);
    sub_000DFE90();
}

/* Particle emitter render: bounds at +0x150 / +0x160 (its frustum test). */
void hook_drawdist_000E6670(void)
{
    uint32_t em = g_ecx;
    if (s_log) fx_seen(FX_EMIT, em, em + 0x150u, em + 0x160u);
    sub_000E6670();
}

static void at_exit(void) { level_report("exit"); }

/* Fork: the distance veil's cell term (d3d8_veil.h) -- the cell window the
 * title draws this tick (TerrainNode_TickVisibilityUpdate 0xDE230): radius =
 * track record +0x60 (+0x64 split screen, as written above in Far / Max),
 * k = int((radius - 1) / 10000) + 1 cells around the cell of the view's
 * position (lvl+0xB0 + view*0x80), cell size and origin from the grid
 * (bounds 0x30380, indices clamped to the grid as in 0x1420B0).
 * out: k cells (the distance always drawn), cell size, window edges x min /
 * max, y min / max relative to the view, radius, and the race's own tick
 * (race+0x18, as the race bench logs it; -1 without a race): the haze's
 * noise follows it, the same in every run of a replay; the track's number
 * (track [0x1DEC90]: the haze's look of each track), -1 if not one. 0 out
 * of a level.
 * Read on the pump thread. */
static int veil_cull(float *out)
{
    uint32_t g = MEM32(APP_GLOBAL), lvl = g ? MEM32(g + 0x72Cu) : 0, cfg = lvl ? MEM32(lvl + 0x2Cu) : 0;
    uint32_t grid = MEM32(GRID_PTR), view, pos;
    float r, x0, y0, cs, px, py;
    int w, h, k, cx, cy, ix0, ix1, iy0, iy1;
    if (!cfg || !grid) return 0;
    r = rdf(cfg + ((int32_t)MEM32(lvl + 0x298u) > 1 ? 0x64u : 0x60u));
    if (!(r > 1000.0f && r < 1e6f)) return 0;
    x0 = rdf(grid + 4u); y0 = rdf(grid + 8u); cs = rdf(grid + 0x28u);
    w = (int)MEM32(grid + 0x2Cu); h = (int)MEM32(grid + 0x30u);
    if (!(cs > 100.0f && cs < 1e6f) || w <= 0 || h <= 0 || w > 512 || h > 512) return 0;
    view = MEM32(lvl + 0x290u);
    if (view > 3u) return 0;
    pos = lvl + 0xB0u + (view << 7);
    px = rdf(pos); py = rdf(pos + 4u);
    if (!(fabsf(px) < 1e8f && fabsf(py) < 1e8f)) return 0;
    k = (int)(r - 1.0f) / 10000 + 1;
    cx = (int)((px - x0 + cs) / cs) - 1;
    cy = (int)((py - y0 + cs) / cs) - 1;
    ix0 = cx - k < 0 ? 0 : cx - k;  ix1 = cx + k > w - 1 ? w - 1 : cx + k;
    iy0 = cy - k < 0 ? 0 : cy - k;  iy1 = cy + k > h - 1 ? h - 1 : cy + k;
    out[0] = (float)k * cs;
    out[1] = cs;
    out[2] = x0 + (float)ix0 * cs - px;
    out[3] = x0 + (float)(ix1 + 1) * cs - px;
    out[4] = y0 + (float)iy0 * cs - py;
    out[5] = y0 + (float)(iy1 + 1) * cs - py;
    out[6] = r;
    {
        uint32_t race = MEM32(lvl + 0x1Cu);
        out[7] = race ? (float)(MEM32(race + 0x18u) & 0xFFFFu) : -1.0f;
    }
    {   /* the track (the haze's look of each track, XBOX_VEIL_TRACKS) */
        uint32_t t = MEM32(0x001DEC90u);
        out[8] = t < 64u ? (float)t : -1.0f;
    }
    return 1;
}

void drawdist_init(void)
{
    const char *e = getenv("XBOX_DRAW_DISTANCE");
    int v = 0;
    if (e && e[0]) {
        if (!_stricmp(e, "far") || !strcmp(e, "1")) v = 1;
        else if (!_stricmp(e, "max") || !strcmp(e, "2")) v = 2;
    }
    g_drawdist_level = v;
    d3d8_VeilSetCullFn(veil_cull);
    s_mul = v == 2 ? 2.0f : v == 1 ? 1.5f : 1.0f;
    e = getenv("XBOX_DRAW_DISTANCE_LOG");
    s_log = e && e[0] >= '1' && e[0] <= '2' ? e[0] - '0' : 0;
    e = getenv("XBOX_DRAW_DISTANCE_FX");
    s_fx = !(e && e[0] == '0');
    if (v || s_log) atexit(at_exit);
    fprintf(stderr, "[DRAWDIST] XBOX_DRAW_DISTANCE=%s%s%s%s",
            v == 2 ? "max (x2)" : v == 1 ? "far (x1.5)" : "original", s_log ? ", log" : "",
            v && !s_fx ? ", fog volume distance left alone" : "",
            v && !s_nodeguard ? ", node pool guards OFF" : "");
    if (v && s_pool != NODE_POOL) fprintf(stderr, ", TEST: node pool shrunk to %d", s_pool);
    if (v && s_itemroom != ITEM_POOL) fprintf(stderr, ", TEST: draw item pool shrunk to %d", s_itemroom);
    if (v && s_nodeguard && s_nodecap != s_pool) fprintf(stderr, ", node pool taken as %d", s_nodecap);
    fputc('\n', stderr);
}

void (*drawdist_lookup(unsigned int xbox_va))(void)
{
    /* the terrain seam options (terrain_seams.h) need the patch hook alone */
    if (!g_drawdist_level && !s_log)
        return xbox_va == 0x000F8BA0u && (g_terrain_seams || g_lmap_armed) ? hook_drawdist_000F8BA0 :
               xbox_va == 0x000F8F10u && g_lmap_armed ? hook_drawdist_000F8F10 : 0;
    if (xbox_va == 0x000DE230u) return hook_drawdist_000DE230;
    if (xbox_va == 0x000FFB50u) return hook_drawdist_000FFB50;
    if (xbox_va == 0x000F8F10u) return hook_drawdist_000F8F10;
    if (xbox_va == 0x000F8BA0u) return hook_drawdist_000F8BA0;
    if (xbox_va == 0x000FF260u) return hook_drawdist_000FF260;
    if (!s_log) return 0;
    if (xbox_va == 0x000DFE90u) return hook_drawdist_000DFE90;
    if (xbox_va == 0x000E6670u) return hook_drawdist_000E6670;
    return 0;
}
