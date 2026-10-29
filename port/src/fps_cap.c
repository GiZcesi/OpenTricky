/*
 * fps_cap -- host frame pacing above 60. See fps_cap.h.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "recomp/recomp_types.h"
#include "fps_cap.h"
#include "hud_anchor.h"
#include "frame_clock.h"
#include "../../xboxrecomp/src/kernel/xbox_perf.h"

void sub_000B2750(void);    /* XBoxExecutionMan_WaitForFrameEvent (thiscall, ret) */
void sub_00151D07(void);    /* WaitForSingleObject (stdcall, ret 8) */
unsigned d3d8_PresentSeq(void);
double d3d8_display_rate(LONG *gen);       /* d3d8_sync.h: the game window's display, exact rate */
void d3d8_sync_set_pace(int fps);          /* d3d8_sync.h */

int g_fps_cap_on;
static int s_in_extra;          /* an extra render is in progress */
/* Fork: the race render (0xAB610) updates the ambient sound emitters (0x120CF0),
 * which stops every emitter the tick did not refresh since the previous update.
 * An extra render (no tick before it) stopped them all: the crowd, cheers and track
 * ambience restarted on the next tick with a new bank, evicting the previous one,
 * and none was ever heard. The generated call (fork pass fix_crowdemit.py) asks this. */
extern int g_fix_crowdemit;
int fps_cap_skip_emitters(void) { return g_fix_crowdemit && s_in_extra; }
/* Fork: the audio update (0x115EE0, called by every render) steps the mixer's volume
 * fades (0x10E280) by 1/60 of their length per call. Once they work (fix_fadeparity.py),
 * an extra render would step them again: skipped there, so a fade lasts as long as on
 * the console (one step per tick). */
extern int g_fix_fade;
int fps_cap_skip_fade(void) { return g_fix_fade && s_in_extra; }
static int s_dup_k;             /* DUP mode: index of the render after the normal one */
static int s_fix_hud;           /* XBOX_FIX_HUD_RATE (default 1): overlay animations at tick rate */

#define APP_GLOBAL      0x001E3C7Cu
#define TICK_MS         (1000.0 / 60.0)
#define WAIT_OBJ_0      0u
#define WAIT_TIMEOUT_   0x102u

static struct {
    int      cap, log, check, noskip, dup;
    double   period_ms;             /* 0 = no limit */
    DWORD    tid;
    double   freq;
    double   last_tick_ret;         /* return of the frame-event hook (~ tick start + render) */
    int      tick_fresh;            /* the event was seen signaled while waiting (tick produced ~ now) */
    int      from_wait;             /* the next tick follows a return of the frame wait */
    double   next_render;
    double   avg_render;
    int      grid;                  /* XBOX_FPS_CAP_GRID (default 1): continuous render grid */
    double   last_tick_sig;         /* time the last tick's event was seen signaled (or released, if unknown) */
    /* The instant each render shows (interpolation): its slot, see
     * sample_time. frame_t: the slot of the render being dispatched (0 =
     * none); render_slot / slot_due: the slot of the tick's render, given at
     * the release of the frame wait and taken by that render. */
    double   frame_t, render_slot;
    int      slot_due;
    unsigned dump_from;             /* XBOX_D3D_DUMP_FROM (duplicate log, T4) */            /* running average of an extra render (ms) */
    /* The limit follows the display ("Match screen", or a limit lowered to it
     * under VSync / Adaptive): see match_fps. */
    int      follow, follow_monitor;
    double   req_fps;               /* limit asked (0 = none / the display) */
    double   margin;                /* fraction under the display's exact rate */
    LONG     rate_gen;              /* d3d8_display_rate's count when last read */
} s;

static struct {
    unsigned long long rider_restored, rng_restored, hook_calls, ticks_ev, extra, extra_ok, refused, skip_tick, late;
    double   extra_ms, extra_max;
    unsigned present0;
    uint32_t race_tick0, race0;
    double   t0;
    /* state probe */
    unsigned long long snaps, d_rng, d_race, d_rider, d_audio, d_state, d_app;
    unsigned long long page_scans, page_changed;
} P, S;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / s.freq;
}

/* Duration of a render (normal or extra) in the running average that decides
 * whether an extra render fits before the tick. Every render updates it: if
 * only extra renders did, one stall (a 198 ms load) would freeze it above the
 * margin and stop extra renders for good. Durations > 50 ms (loads, stalls)
 * are ignored. */
#define RENDER_OUTLIER_MS 50.0
#define NV_MAX      4u
#define NR_MAX      8u
#define VIEW_OFF    0xB0u
#define VIEW_LEN    0x58u           /* +0xB0..+0x107 */
#define VIEW_STRIDE 0x80u
#define POS_OFF     0x170u
#define POSE_OFF    0x48B0u
#define POSE_N      21u
#define TELEPORT    400.0f          /* units per tick (race: ~30) */
#define CAM_CUT_COS 0.866f          /* camera rotation > 60 deg in one tick = cut */
#define BONE_CUT_COS 0.0f           /* bone turned by > 180 deg: quaternions, cos(half angle) < 0 -> 90 deg */

enum { SP_VIEW, SP_POS, SP_POSE };
typedef struct { uint32_t va, len, kind, off; } Span;
typedef struct {
    uint32_t st, race, n, nw;
    Span sp[NV_MAX + 2u * NR_MAX];
    float w[(NV_MAX * VIEW_LEN + NR_MAX * (0x10u + POSE_N * 0x40u)) / 4u];
} VS;

static struct {
    int on, log, synth;             /* XBOX_FPS_INTERP, XBOX_FPS_INTERP_LOG (number of renders logged) */
    VS prev, cur, pre, out;
    int ok;                         /* prev and cur valid, same layout */
    double t_tick;                  /* start of the last tick */
    double t_est;                   /* estimated production time of the last tick (game timer) */
    double ts;                      /* instant the last render showed (its slot, or its start) */
    int clock;                      /* XBOX_FPS_INTERP_CLOCK: 1 = timer-based tick time, 0 = consumption time */
    int mode;                       /* last interp_begin: 0 interpolated, 1 alpha >= 1, 2 memory != tick, 3 not applicable */
    float alpha;                    /* last alpha (before the >= 1 test) */
    float t;                        /* fraction used by the render in progress (written = 1) */
    int written;                    /* interpolated state in memory (render in progress) */
    unsigned long long n_interp, n_mismatch, n_alpha1, n_cut_cam, n_cut_rider, n_badrot, n_logged;
    uint32_t log_from;
} I;

static unsigned long long s_outliers;
static int s_measured;           /* duration already counted by hook_AB610 */
static void render_time(double d)
{
    if (d > RENDER_OUTLIER_MS) { s_outliers++; return; }
    s.avg_render = s.avg_render > 0.0 ? s.avg_render * 0.9 + d * 0.1 : d;
}

static uint32_t race_ptr(void)
{
    uint32_t app = MEM32(APP_GLOBAL), lvl;
    if (app < 0x1000u) return 0;
    lvl = MEM32(app + 0x72Cu);
    if (lvl < 0x1000u) return 0;
    return MEM32(lvl + 0x1Cu);
}

/* ── guest calls ────────────────────────────────────────────────── */

/* The game's WaitForSingleObject(handle, ms): 0 if signaled, 0x102 on timeout. */
static uint32_t guest_wait(uint32_t handle, uint32_t ms)
{
    uint32_t sv_ecx = g_ecx, sv_edx = g_edx, esp0 = g_esp, r;
    g_esp -= 4; MEM32(g_esp) = ms;
    g_esp -= 4; MEM32(g_esp) = handle;
    g_esp -= 4; MEM32(g_esp) = 0;          /* dummy return address */
    sub_00151D07();
    r = g_eax;
    g_esp = esp0;                          /* ret 8 popped everything; to be safe */
    g_ecx = sv_ecx; g_edx = sv_edx;
    return r;
}

/* state->vt+0x18 (render of the current state), as the loop at 0xAA283 does.
 * Returns al (1 = render done). */
static int guest_render(uint32_t app)
{
    uint32_t sv[7] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_seh_ebp };
    uint32_t esp0 = g_esp, state = MEM32(app + 4u), va;
    recomp_func_t fn;
    int al;
    if (state < 0x1000u) return 0;
    va = MEM32(MEM32(state) + 0x18u);
    fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) return 0;
    g_ecx = state;
    g_esi = app;                           /* as in the loop (esi = Application) */
    g_esp -= 4; MEM32(g_esp) = 0;          /* dummy return address */
    fn();
    al = (int)(g_eax & 0xFFu);
    if (g_esp != esp0) {
        static int warned;
        if (!warned++) fprintf(stderr, "[FPSCAP] stack unbalanced after the render: %d bytes\n", (int)(g_esp - esp0));
        g_esp = esp0;
    }
    g_eax = sv[0]; g_ecx = sv[1]; g_edx = sv[2]; g_ebx = sv[3]; g_esi = sv[4]; g_edi = sv[5]; g_seh_ebp = sv[6];
    return al;
}

/* ── state probe (XBOX_FPS_CAP_CHECK=1) ───────────────────────── */

/* Regions copied raw before / after each extra render; every changed word is
 * counted per (region, offset) to tell WHAT changes. */
enum { RG_RNG, RG_RACE, RG_RIDER, RG_AUDIO, RG_STATE, RG_APP, RG_N };
static const char *const s_rg_name[RG_N] = { "rng", "race", "riders", "audio", "game_state", "app" };
static const uint32_t s_rg_len[RG_N] = { 0x30, 0x400, 0x5A00, 0x400, 0x300, 0x800 };   /* game state: up to views +0xB0..+0x29F */
static uint32_t *s_rg_hist[RG_N];          /* count per word (riders: all indices together) */

typedef struct {
    uint32_t base[RG_N + 7];                /* RG_RIDER .. +7 : 8 riders */
    uint32_t *buf[RG_N + 7];
} Snap;

static int rg_of(int k) { return k >= RG_RIDER && k < RG_RIDER + 8 ? RG_RIDER : (k >= RG_RIDER + 8 ? k - 7 : k); }

static void snap_bases(Snap *o, uint32_t app)
{
    uint32_t race = race_ptr(), au = MEM32(0x001F82F4u), st = MEM32(app + 4u), i, n;
    memset(o->base, 0, sizeof o->base);
    o->base[RG_RNG] = 0x001FAD70u;
    if (race) {
        o->base[RG_RACE] = race;
        n = MEM32(race + 0x88u); if (n > 8) n = 8;
        for (i = 0; i < n; i++) {
            uint32_t r = MEM32(race + 0xC4u + 4u * i);
            if (r >= 0x1000u) o->base[RG_RIDER + i] = r;   /* whole Rider sub-object (+0x58E0 used) */
        }
    }
    if (au >= 0x1000u) o->base[RG_AUDIO + 7] = au;
    if (st >= 0x1000u) o->base[RG_STATE + 7] = st;
    o->base[RG_APP + 7] = app;
}

static void snap_copy(Snap *o)
{
    int k;
    for (k = 0; k < RG_N + 7; k++) {
        uint32_t len = s_rg_len[rg_of(k)];
        if (!o->buf[k]) o->buf[k] = (uint32_t *)malloc(len);
        if (o->base[k]) memcpy(o->buf[k], (const void *)XBOX_PTR(o->base[k]), len);
    }
}

/* Compares the copy with current memory; returns a mask of the changed regions. */
static unsigned snap_diff(const Snap *o)
{
    unsigned mask = 0;
    int k;
    for (k = 0; k < RG_N + 7; k++) {
        int rg = rg_of(k);
        uint32_t len = s_rg_len[rg], w;
        const uint32_t *now;
        if (!o->base[k]) continue;
        now = (const uint32_t *)XBOX_PTR(o->base[k]);
        if (!memcmp(now, o->buf[k], len)) continue;
        mask |= 1u << rg;
        for (w = 0; w < len / 4; w++)
            if (now[w] != o->buf[k][w]) {
                if (s_rg_hist[rg][w]++ < 6)   /* first 6 examples per word: value before -> after */
                    fprintf(stderr, "[FPSCAP] diff %s+%X (base %08X): %08X -> %08X\n",
                            s_rg_name[rg], w * 4, o->base[k], o->buf[k][w], now[w]);
            }
    }
    return mask;
}

static void hist_report(void)
{
    int rg;
    for (rg = 0; rg < RG_N; rg++) {
        uint32_t w, nw = s_rg_len[rg] / 4, shown = 0;
        fprintf(stderr, "[FPSCAP]   %s: changed words (offset:count)", s_rg_name[rg]);
        for (w = 0; w < nw && shown < 24; w++)
            if (s_rg_hist[rg][w]) { fprintf(stderr, " +%X:%u", w * 4, s_rg_hist[rg][w]); shown++; }
        fprintf(stderr, "%s\n", shown ? "" : " none");
    }
}

/* Render functions called by the extra renders (vt+0x18 of the current state). */
static struct { uint32_t va; unsigned long long n, ok; } s_rva[16];
static void rva_note(uint32_t va, int ok)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (s_rva[i].va == va || !s_rva[i].va) { s_rva[i].va = va; s_rva[i].n++; s_rva[i].ok += ok != 0; return; }
    }
}

/* Diff of guest memory pages (4 KB) around an extra render, sampled. */
#define PG_MAX (0x08000000u >> 12)
static uint64_t *s_pg;
static uint32_t s_pg_hits[PG_MAX];

static void pages_hash(uint64_t *out)
{
    uint32_t va = 0;
    while (va < 0x08000000u) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t p = (uintptr_t)va + g_xbox_mem_offset;
        uint32_t end;
        if (!VirtualQuery((void *)p, &mbi, sizeof mbi)) break;
        end = (uint32_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - g_xbox_mem_offset);
        if (end > 0x08000000u || end <= va) end = 0x08000000u;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (; va < end; va += 0x1000u) {
                const uint64_t *q = (const uint64_t *)((uintptr_t)va + g_xbox_mem_offset);
                uint64_t h = 1469598103934665603ull; int k;
                for (k = 0; k < 512; k++) { h ^= q[k]; h *= 1099511628211ull; }
                out[va >> 12] = h;
            }
        } else {
            for (; va < end; va += 0x1000u) out[va >> 12] = 0;
        }
    }
}

static void pages_report(void)
{
    uint32_t i, run = 0, start = 0, shown = 0;
    fprintf(stderr, "[FPSCAP] pages changed by the extra renders (%llu scans; page:count):", S.page_scans);
    for (i = 0; i <= PG_MAX; i++) {
        int hit = i < PG_MAX && s_pg_hits[i];
        if (hit && !run) { start = i; run = 1; }
        if (!hit && run) {
            if (shown++ < 60) fprintf(stderr, " %06X-%06X:%u", start << 12, (i << 12) - 1, s_pg_hits[start]);
            run = 0;
        }
    }
    fprintf(stderr, " (%u ranges)\n", shown);
}

/* ── HUD at tick rate (XBOX_FIX_HUD_RATE, default 1) ─────────────
 * The InGameState render draws the 19 OverlayManager panels (race HUD,
 * pause and in-race menus, banners), and several of their draw methods step
 * an animation on every call, with no time base: selected item pulse
 * (0xD06C0: globals 0x1BB25C / 0x1BB260; 0x56A60: 0x1AA7D4 / 0x1AA7D8),
 * list counter 0x1E9888 (0xD4DE0), finish banner fade (0xD2C20, panel
 * +0x58), rider marker sway (0xC44E0, race HUD panel). The original renders
 * once per tick, so they advance 60 steps per second; extra renders made
 * them run faster.
 * Fix: the panels and these globals are copied right before the normal
 * render (pre). Around each extra render, the current state (post, as the
 * normal render left it) is saved, pre is written, the render runs (it
 * repeats the very step of the normal render and draws the same HUD), and
 * post is written back: the next tick sees exactly the state after the
 * normal render, as at 60.
 * Words that look like pointers on either side (object created or replaced
 * by a render) are never swapped: the extra render sees the current pointer,
 * and a pointer it sets is kept (counted, logged).
 * Also: no extra render while the gameplay tick is frozen (pause and the
 * other blocking overlays, the game's own gate 0xAB170): the scene does not
 * move and the pause menu runs as at 60. Nor after a tick whose riders were
 * frozen (hud_anchor_tick_frozen): the tick that closes the pause has not
 * moved them, its image is the last one of the pause, and its extra renders
 * (the panel closed by then, the race HUD back) showed a race frozen
 * between it and the next tick.
 * XBOX_FIX_HUD_RATE=0: previous behavior. */
#define OM_VTABLE   0x0019CB24u     /* OverlayManager (OverlayManager_Construct 0xDD7D0) */
#define OM_PANELS   19u             /* panel pointers at +0x10..+0x58 */
#define HUD_GLOB    3u
#define HUD_RG_MAX  (OM_PANELS + HUD_GLOB)
#define HUD_WORDS   0x2800u         /* 0x9A7C bytes used */
#define RDATA_LO    0x001872E0u     /* .rdata: panel vtables */
#define RDATA_HI    0x001A9F68u

/* Each panel object, from its construction in 0xDD7D0 (slot order): size of
 * the allocation (len), and where the pointer the manager keeps lies in it
 * (off). That pointer is the panel base (0xCA8A0, 0x340 bytes); most panel
 * classes inherit it virtually, so the manager stores the virtual base
 * (object + vbptr + vbtable[1]), at the end of the object or after its first
 * base: the object, and the state its own draw methods step, start off bytes
 * before it. vbp: offset of that vbptr in the object (NO_VB: plain base,
 * off 0); used to check that the object is the one 0xDD7D0 built. */
#define NO_VB 0xFFFFFFFFu
static const struct { uint32_t len, off, vbp; } s_om[OM_PANELS] = {
    { 0x5F0, 0, NO_VB },   { 0x1314, 0x104, 0 }, { 0x450, 0x110, 0 },    { 0x590, 0x250, 0 },
    { 0x30, 0, NO_VB },    { 0x498, 0, NO_VB },  { 0x162C, 0x12EC, 0xE78 }, { 0x94, 0, NO_VB },
    { 0x28, 0, NO_VB },    { 0x11DC, 0xE9C, 0xE78 }, { 0x340, 0, NO_VB }, { 0x38C, 0x4C, 0 },
    { 0x24, 0, NO_VB },    { 0x590, 0x250, 0 },  { 0x340, 0, NO_VB },    { 0x11DC, 0xE9C, 0xE78 },
    { 0x12D4, 0xF94, 0xE78 }, { 0x11FC, 0xEBC, 0xE78 }, { 0x38C, 0x4C, 0 }
};
/* Globals stepped by panel draws: 0xD06C0 pulse speed / scale, 0x56A60
 * pulse speed / scale, 0xD4DE0 counter. */
static const uint32_t s_hud_glob[HUD_GLOB][2] = { { 0x001BB25Cu, 8 }, { 0x001AA7D4u, 8 }, { 0x001E9888u, 4 } };

typedef struct { uint32_t va, len, off, id; } HudRg;     /* off: word offset in the buffers; id: slot, or OM_PANELS + global */
typedef struct { uint32_t st, n, nw; HudRg rg[HUD_RG_MAX]; } HudLayout;

static struct {
    HudLayout l;
    uint32_t pre[HUD_WORDS], post[HUD_WORDS];
    uint32_t tick;                  /* race tick at the copy */
    int valid;                      /* pre is from the last normal render, no tick since */
    unsigned bad_slot;              /* slots whose object is not a panel (logged once) */
    unsigned long long swaps, words, ptr_kept, stale;
} H;

static int is_race_render(uint32_t st)
{
    return st >= 0x1000u && MEM32(st) >= 0x1000u && MEM32(MEM32(st) + 0x18u) == 0x000AB610u;
}

/* A guest object of len bytes: in the low memory, or in the contiguous memory
 * the title allocates its race objects from (0x80000000 up: the OverlayManager
 * and its panels are there; with the low range only, no panel was copied and
 * their animations ran at the render rate above 60). */
static int hud_obj(uint32_t p, uint32_t len)
{
    return (p >= 0x1000u && p < 0x08000000u - len) || (p >= 0x80000000u && p < 0x84000000u - len);
}

/* Panels of the race state st (only if it is InGameState), then the globals. */
static uint32_t hud_layout(HudLayout *l, uint32_t st)
{
    uint32_t i, off = 0, om = is_race_render(st) ? MEM32(st + 0x38u) : 0;
    l->st = st; l->n = 0;
    if (hud_obj(om, 0x60u) && MEM32(om) == OM_VTABLE) {
        for (i = 0; i < OM_PANELS; i++) {
            uint32_t p = MEM32(om + 0x10u + 4u * i), obj = p - s_om[i].off, vt = 0, vbo = 0;
            if (p >= s_om[i].off && hud_obj(obj, s_om[i].len)) {
                vt = MEM32(p);
                if (s_om[i].vbp != NO_VB) {
                    uint32_t vbt = MEM32(obj + s_om[i].vbp);
                    vbo = vbt >= RDATA_LO && vbt < RDATA_HI - 8u ? MEM32(vbt + 4u) : 0;
                }
            }
            /* every panel class shares the base methods +0x28 / +0x2C; a
             * virtual base must be where the object's vbtable puts it */
            if (vt < RDATA_LO || vt >= RDATA_HI || MEM32(vt + 0x28u) != 0x000C3770u || MEM32(vt + 0x2Cu) != 0x000C3780u
                || (s_om[i].vbp != NO_VB && vbo != s_om[i].off - s_om[i].vbp)) {
                if (p && !(H.bad_slot & (1u << i))) {
                    H.bad_slot |= 1u << i;
                    fprintf(stderr, "[FPSCAP] hud: slot %u (%08X, vt %08X, vb %X) is not a panel, skipped\n", i, p, vt, vbo);
                }
                continue;
            }
            l->rg[l->n].va = obj; l->rg[l->n].len = s_om[i].len; l->rg[l->n].off = off; l->rg[l->n].id = i;
            off += s_om[i].len / 4u; l->n++;
        }
    }
    for (i = 0; i < HUD_GLOB; i++) {
        l->rg[l->n].va = s_hud_glob[i][0]; l->rg[l->n].len = s_hud_glob[i][1]; l->rg[l->n].off = off; l->rg[l->n].id = OM_PANELS + i;
        off += s_hud_glob[i][1] / 4u; l->n++;
    }
    l->nw = off;
    return l->n;
}

static int hud_ptr(uint32_t v)
{
    return !(v & 3u) && ((v >= 0x10000u && v < 0x08000000u) || (v > 0x80000000u && v < 0x84000000u));
}
/* a -> b looks like a pointer change (set, cleared or replaced) */
static int hud_ptr_pair(uint32_t a, uint32_t b)
{
    return (a | b) && (!a || hud_ptr(a)) && (!b || hud_ptr(b));
}


/* Before the normal race render. */
static void hud_capture(uint32_t st)
{
    uint32_t k, race = race_ptr();
    H.valid = 0;
    if (!hud_layout(&H.l, st)) return;
    for (k = 0; k < H.l.n; k++) memcpy(&H.pre[H.l.rg[k].off], (const void *)XBOX_PTR(H.l.rg[k].va), H.l.rg[k].len);
    H.tick = race ? MEM32(race + 0x18u) : 0;
    H.valid = 1;
}

/* Before an extra render: post saved, pre written. 1 if done. */
static int hud_swap_in(uint32_t app)
{
    static HudLayout l;
    uint32_t k, w, race = race_ptr(), st = MEM32(app + 4u);
    if (!H.valid) return 0;
    if (st != H.l.st || (race ? MEM32(race + 0x18u) : 0) != H.tick || hud_layout(&l, st) != H.l.n
        || memcmp(l.rg, H.l.rg, l.n * sizeof l.rg[0])) {
        H.valid = 0; H.stale++;
        return 0;
    }
    /* every post first, before any pre is written: a post taken after an
     * overlapping region's pre was written would hold that pre (the panels
     * were once copied from their virtual base on, over the next panels: the
     * finish banner's fade-in was reset to 0 after every extra render) */
    for (k = 0; k < l.n; k++) memcpy(&H.post[l.rg[k].off], (const void *)XBOX_PTR(l.rg[k].va), l.rg[k].len);
    for (k = 0; k < l.n; k++) {
        const HudRg *g = &l.rg[k];
        uint32_t *m = (uint32_t *)XBOX_PTR(g->va), *pre = &H.pre[g->off], *post = &H.post[g->off];
        if (!memcmp(pre, post, g->len)) continue;
        for (w = 0; w < g->len / 4u; w++)
            if (pre[w] != post[w] && !hud_ptr_pair(pre[w], post[w])) { m[w] = pre[w]; H.words++; }
    }
    H.swaps++;
    return 1;
}

/* After the extra render: post written back. */
static void hud_swap_out(void)
{
    uint32_t k, w;
    for (k = 0; k < H.l.n; k++) {
        const HudRg *g = &H.l.rg[k];
        uint32_t *m = (uint32_t *)XBOX_PTR(g->va), *post = &H.post[g->off];
        if (!memcmp(m, post, g->len)) continue;
        for (w = 0; w < g->len / 4u; w++) {
            if (m[w] == post[w]) continue;
            if (hud_ptr_pair(post[w], m[w])) {
                if (H.ptr_kept++ < 8u)
                    fprintf(stderr, "[FPSCAP] hud: pointer set by an extra render kept, region %u +%X: %08X -> %08X\n",
                            g->id, w * 4u, post[w], m[w]);
                continue;
            }
            m[w] = post[w];
        }
    }
}

/* Probe (XBOX_FPS_CAP_CHECK=1): HUD regions before / after each extra
 * render (after the fix's write-back): renders that changed each region,
 * changed words. With XBOX_FIX_HUD_RATE=0 it lists what the render steps;
 * with the fix, what is left. */
static struct {
    HudLayout l;
    uint32_t buf[HUD_WORDS];
    uint32_t *hist[HUD_RG_MAX];
    unsigned long long renders[HUD_RG_MAX];
} HP;

static void hud_probe_copy(uint32_t app)
{
    uint32_t k;
    hud_layout(&HP.l, MEM32(app + 4u));
    for (k = 0; k < HP.l.n; k++) memcpy(&HP.buf[HP.l.rg[k].off], (const void *)XBOX_PTR(HP.l.rg[k].va), HP.l.rg[k].len);
}

static void hud_probe_diff(void)
{
    uint32_t k, w;
    for (k = 0; k < HP.l.n; k++) {
        const HudRg *g = &HP.l.rg[k];
        const uint32_t *now = (const uint32_t *)XBOX_PTR(g->va), *was = &HP.buf[g->off];
        if (!memcmp(now, was, g->len)) continue;
        if (!HP.hist[g->id]) HP.hist[g->id] = (uint32_t *)calloc(g->len / 4u, sizeof(uint32_t));
        HP.renders[g->id]++;
        for (w = 0; w < g->len / 4u; w++)
            if (now[w] != was[w] && HP.hist[g->id] && HP.hist[g->id][w]++ < 4u)
                fprintf(stderr, "[FPSCAP] diff hud%u+%X (base %08X): %08X -> %08X\n", g->id, w * 4u, g->va, was[w], now[w]);
    }
}

static void hud_probe_report(void)
{
    uint32_t id, w, shown = 0;
    fprintf(stderr, "[FPSCAP]   hud regions (0-18 = OverlayManager slot, 19-21 = globals 1BB25C 1AA7D4 1E9888): renders changed");
    for (id = 0; id < HUD_RG_MAX; id++) if (HP.renders[id]) fprintf(stderr, " %u:%llu", id, HP.renders[id]);
    fprintf(stderr, "; words (region+offset:count)");
    for (id = 0; id < HUD_RG_MAX; id++) {
        uint32_t len = id < OM_PANELS ? s_om[id].len : s_hud_glob[id - OM_PANELS][1];
        if (!HP.hist[id]) continue;
        for (w = 0; w < len / 4u && shown < 48u; w++)
            if (HP.hist[id][w]) { fprintf(stderr, " %u+%X:%u", id, w * 4u, HP.hist[id][w]); shown++; }
    }
    fputc('\n', stderr);
    fprintf(stderr, "[FPSCAP]   hud layout (region:object/len):");
    for (id = 0; id < HP.l.n; id++) fprintf(stderr, " %u:%08X/%X", HP.l.rg[id].id, HP.l.rg[id].va, HP.l.rg[id].len);
    fputc('\n', stderr);
}

/* GameState_ShouldSkipGameplayTick 0xAB170 (thiscall on InGameState, no
 * argument, reads only): nonzero while the gameplay tick is frozen (pause,
 * blocking overlay, HUD overlay 0x1E3E08, lesson). */
void sub_000AB170(void);
static int gameplay_frozen(uint32_t st)
{
    uint32_t sv[7] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_seh_ebp }, esp0 = g_esp;
    int r;
    g_ecx = st;
    g_esp -= 4; MEM32(g_esp) = 0;          /* fake return address */
    sub_000AB170();
    r = (g_eax & 0xFFu) != 0;
    g_esp = esp0;
    g_eax = sv[0]; g_ecx = sv[1]; g_edx = sv[2]; g_ebx = sv[3]; g_esi = sv[4]; g_edi = sv[5]; g_seh_ebp = sv[6];
    return r;
}

/* ── statistics ───────────────────────────────────────────────── */

static void report(int force)
{
    double t = now_ms(), el = (t - P.t0) / 1000.0;
    uint32_t race = race_ptr(), rt = race ? MEM32(race + 0x18u) : 0;
    double race_tps = -1.0;
    if (!s.log && !s.check) return;
    if (!force && el < 2.0) return;
    if (race && race == P.race0 && rt >= P.race_tick0) race_tps = (rt - P.race_tick0) / el;
    fprintf(stderr,
        "[FPSCAP] t=%.1f cap=%d presents/s=%.1f events/s=%.1f race_ticks/s=%.1f "
        "extra/s=%.1f (done %.1f refused %.1f) skipped_for_tick/s=%.1f late/s=%.1f "
        "extra_render_ms=%.2f (max %.2f, running avg %.2f) rng_restored=%llu",
        t / 1000.0, s.cap, (d3d8_PresentSeq() - P.present0) / el, P.ticks_ev / el, race_tps,
        P.extra / el, P.extra_ok / el, P.refused / el, P.skip_tick / el, P.late / el,
        P.extra_ok ? P.extra_ms / P.extra_ok : 0.0, P.extra_max, s.avg_render, P.rng_restored);
    if (s.check)
        fprintf(stderr, " | state[n=%llu rng=%llu race=%llu riders=%llu audio=%llu game_state=%llu app=%llu] pages[scans=%llu changed=%llu]",
            P.snaps, P.d_rng, P.d_race, P.d_rider, P.d_audio, P.d_state, P.d_app, P.page_scans, P.page_changed);
    if (I.on)
        fprintf(stderr, " | interp[n=%llu memory!=tick=%llu alpha>=1=%llu cam_cut=%llu rider_cut=%llu bones_not_interp=%llu riders_restored=%llu] stalls_ignored=%llu",
                I.n_interp, I.n_mismatch, I.n_alpha1, I.n_cut_cam, I.n_cut_rider, I.n_badrot, P.rider_restored, s_outliers);
    if (s_fix_hud)
        fprintf(stderr, " | hud[swaps=%llu words=%llu stale=%llu pointers_kept=%llu]", H.swaps, H.words, H.stale, H.ptr_kept);
    fprintf(stderr, " race_tick=%u state=%u\n", rt, race ? MEM32(race + 0x1Cu) : 0);
    fflush(stderr);
#define ACC(f) S.f += P.f
    ACC(rng_restored); ACC(rider_restored); ACC(hook_calls); ACC(ticks_ev); ACC(extra); ACC(extra_ok); ACC(refused); ACC(skip_tick); ACC(late);
    ACC(snaps); ACC(d_rng); ACC(d_race); ACC(d_rider); ACC(d_audio); ACC(d_state); ACC(d_app);
    ACC(page_scans); ACC(page_changed);
#undef ACC
    memset(&P, 0, sizeof P);
    P.t0 = t; P.present0 = d3d8_PresentSeq(); P.race0 = race; P.race_tick0 = rt;
    {
        static double last_cum;
        if (s.check && !force && t - last_cum > 30000.0) force = 1;
        if (force) last_cum = t;
    }
    if (s.check && force) {
        fprintf(stderr, "[FPSCAP] total: RNG drawn then restored in %llu extra renders\n", S.rng_restored);
        fprintf(stderr, "[FPSCAP] total: extra renders %llu (done %llu); differences rng=%llu race=%llu riders=%llu audio=%llu game_state=%llu app=%llu\n",
            S.extra, S.extra_ok, S.d_rng, S.d_race, S.d_rider, S.d_audio, S.d_state, S.d_app);
        hist_report();
        {
            int i;
            fprintf(stderr, "[FPSCAP]   render functions (va:calls/done)");
            for (i = 0; i < 16 && s_rva[i].va; i++) fprintf(stderr, " %06X:%llu/%llu", s_rva[i].va, s_rva[i].n, s_rva[i].ok);
            fputc('\n', stderr);
        }
        if (s_pg) pages_report();
        hud_probe_report();
    }
}

/* Fidelity: the InGameState render (0xAB610) counts down a frame skip
 * [state+0x6C] on every call; an extra render would consume it faster than
 * the original. No extra render while it is > 0. */
static int extra_allowed(uint32_t app)
{
    uint32_t st = MEM32(app + 4u), va;
    if (st < 0x1000u) return 0;
    va = MEM32(MEM32(st) + 0x18u);
    /* Allow list: race (InGameState 0xAB610) and menus (0x7CBD0), checked to
     * have no logic effect. The other states (boot, videos, loading) advance
     * the state machine in their render or block: no extra render, as in the
     * original. With the HUD fix, none either while the gameplay tick is
     * frozen (pause), nor after a tick that was (the one that closes the
     * pause): see the HUD section. */
    if (va == 0x000AB610u)
        return (int32_t)MEM32(st + 0x6Cu) <= 0 && !(s_fix_hud && (gameplay_frozen(st) || hud_anchor_tick_frozen()));
    return va == 0x0007CBD0u;
}

/* ── interpolation ────────────────────────────────────────────────
 * A render shows lerp(tick N-1, tick N, alpha), alpha = (the render's
 * instant - the time tick N was produced) / 16.67 ms: the normal render
 * (right after the tick) as well as the extra ones. The render's instant is
 * the one its image reaches the screen at (sample_time: its slot when frames
 * wait for a refresh, its start when they are shown at once), the tick's
 * time the frame timer's point of its exact grid (hook_AD4A0): so the
 * simulated time of two images in a row differs by the time between them on
 * the display. Measured from the render's start and from the moment the
 * frame wait saw the tick's event instead, the image after each tick jumped
 * ahead (6-7 ms of motion for a 4.17 ms refresh at 240 Hz) and the next one
 * fell back. Display is one tick late (+16.7 ms latency), no extrapolation.
 * State (measured):
 *   InGameState +0xB0 + i*0x80 (view i < [+0x298]): camera position (4 f),
 *     view matrix +0xC0 (rows 0-2 = rotation, row 3 = -c*R),
 *     projection +0x100 / +0x104;
 *   rider +0x170 (position, 4 f) and pose +0x48B0: 21 4x4 matrices in
 *     WORLD space (orthonormal rotation + translation).
 * All of it is written by the tick only; the render never writes it.
 * Before each render: memory must equal the copy of tick N (else no
 * interpolation); after: the copy of tick N is written back (bit exact). */

static uint32_t vs_layout(VS *v, uint32_t st)
{
    uint32_t race = race_ptr(), nv, nr, i, off = 0;
    v->n = 0; v->st = st; v->race = race;
    if (st < 0x1000u || !race) return 0;
    nv = MEM32(st + 0x298u); if (nv > NV_MAX) nv = NV_MAX;
    nr = MEM32(race + 0x88u); if (nr > NR_MAX) nr = NR_MAX;
    for (i = 0; i < nv; i++) {
        Span *p = &v->sp[v->n++];
        p->va = st + VIEW_OFF + i * VIEW_STRIDE; p->len = VIEW_LEN; p->kind = SP_VIEW; p->off = off; off += VIEW_LEN / 4u;
    }
    for (i = 0; i < nr; i++) {
        uint32_t r = MEM32(race + 0xC4u + 4u * i);
        Span *p;
        if (r < 0x1000u) continue;
        p = &v->sp[v->n++]; p->va = r + POS_OFF; p->len = 0x10u; p->kind = SP_POS; p->off = off; off += 4u;
        p = &v->sp[v->n++]; p->va = r + POSE_OFF; p->len = POSE_N * 0x40u; p->kind = SP_POSE; p->off = off; off += POSE_N * 16u;
    }
    v->nw = off;
    return v->n;
}

static void vs_read(VS *v)
{
    uint32_t k;
    for (k = 0; k < v->n; k++) memcpy(&v->w[v->sp[k].off], (const void *)XBOX_PTR(v->sp[k].va), v->sp[k].len);
}

static void vs_write(const VS *v)
{
    uint32_t k;
    for (k = 0; k < v->n; k++) memcpy((void *)XBOX_PTR(v->sp[k].va), &v->w[v->sp[k].off], v->sp[k].len);
}

static int vs_same_layout(const VS *a, const VS *b)
{
    uint32_t k;
    if (a->n != b->n || a->st != b->st || a->race != b->race) return 0;
    for (k = 0; k < a->n; k++) if (a->sp[k].va != b->sp[k].va) return 0;
    return 1;
}

static int vs_equals_mem(const VS *v)
{
    uint32_t k;
    for (k = 0; k < v->n; k++)
        if (memcmp(&v->w[v->sp[k].off], (const void *)XBOX_PTR(v->sp[k].va), v->sp[k].len)) return 0;
    return 1;
}

/* Quaternion of a 3x3 rotation matrix (rows m[0..2], stride 4). */
static void m2q(const float *m, float q[4])
{
    float tr = m[0] + m[5] + m[10], s;
    if (tr > 0.0f) {
        s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (m[6] - m[9]) / s; q[1] = (m[8] - m[2]) / s; q[2] = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        s = sqrtf(1.0f + m[0] - m[5] - m[10]) * 2.0f;
        q[3] = (m[6] - m[9]) / s; q[0] = 0.25f * s; q[1] = (m[4] + m[1]) / s; q[2] = (m[8] + m[2]) / s;
    } else if (m[5] > m[10]) {
        s = sqrtf(1.0f + m[5] - m[0] - m[10]) * 2.0f;
        q[3] = (m[8] - m[2]) / s; q[0] = (m[4] + m[1]) / s; q[1] = 0.25f * s; q[2] = (m[9] + m[6]) / s;
    } else {
        s = sqrtf(1.0f + m[10] - m[0] - m[5]) * 2.0f;
        q[3] = (m[1] - m[4]) / s; q[0] = (m[8] + m[2]) / s; q[1] = (m[9] + m[6]) / s; q[2] = 0.25f * s;
    }
}

static void q2m(const float q[4], float *m)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y);
}

/* Near-orthonormal 3x3 rotation (rows of m); returns +1 (proper), -1
 * (improper: the game's view matrix has determinant -1) or 0, and the row
 * norms. */
static int rot_ok(const float *m, float n[3])
{
    int i;
    float d;
    for (i = 0; i < 3; i++) {
        n[i] = sqrtf(m[4 * i] * m[4 * i] + m[4 * i + 1] * m[4 * i + 1] + m[4 * i + 2] * m[4 * i + 2]);
        if (!(n[i] > 1e-4f) || !isfinite(n[i])) return 0;
    }
    d = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    d /= n[0] * n[1] * n[2];
    return d > 0.98f && d < 1.02f ? 1 : (d < -0.98f && d > -1.02f ? -1 : 0);
}

/* Interpolated rotation (slerp), interpolated row norms. dot = cos(half angle). */
static int rot_lerp(const float *a, const float *b, float t, float *o, float min_dot)
{
    float na[3], nb[3], ua[12], ub[12], qa[4], qb[4], q[4], d, l;
    int i, j, sa = rot_ok(a, na), sb = rot_ok(b, nb);
    if (!sa || sa != sb) return 0;
    if (sa < 0) { na[2] = -na[2]; nb[2] = -nb[2]; }   /* row 2 flipped: proper rotation, restored at the end */
    for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) { ua[4 * i + j] = a[4 * i + j] / na[i]; ub[4 * i + j] = b[4 * i + j] / nb[i]; }
    m2q(ua, qa); m2q(ub, qb);
    d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0.0f) { d = -d; for (i = 0; i < 4; i++) qb[i] = -qb[i]; }
    if (d < min_dot) return -1;
    if (d > 0.9995f) {
        for (i = 0; i < 4; i++) q[i] = qa[i] + (qb[i] - qa[i]) * t;
    } else {
        float th = acosf(d), s = sinf(th), wa = sinf((1.0f - t) * th) / s, wb = sinf(t * th) / s;
        for (i = 0; i < 4; i++) q[i] = qa[i] * wa + qb[i] * wb;
    }
    l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (i = 0; i < 4; i++) q[i] /= l;
    q2m(q, o);
    for (i = 0; i < 3; i++) { float n = na[i] + (nb[i] - na[i]) * t; for (j = 0; j < 3; j++) o[4 * i + j] *= n; }
    return 1;
}

static float dist3(const float *a, const float *b)
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

/* View: block +0xB0 (22 f): [0..3] position, [4..19] view matrix, [20], [21] projection. */
static int view_lerp(const float *a, const float *b, float t, float *o)
{
    float ca[3], cb[3], c[3], *m = o + 4;
    const float *ma = a + 4, *mb = b + 4;
    int i, j, r;
    /* camera position from the matrix: c = -t*R^T */
    for (i = 0; i < 3; i++) {
        ca[i] = -(ma[12] * ma[4 * i] + ma[13] * ma[4 * i + 1] + ma[14] * ma[4 * i + 2]);
        cb[i] = -(mb[12] * mb[4 * i] + mb[13] * mb[4 * i + 1] + mb[14] * mb[4 * i + 2]);
    }
    if (!(dist3(ca, cb) < TELEPORT) || !(dist3(a, b) < TELEPORT)) return -1;
    r = rot_lerp(ma, mb, t, m, CAM_CUT_COS);
    if (r <= 0) return r;
    for (i = 0; i < 3; i++) c[i] = ca[i] + (cb[i] - ca[i]) * t;
    for (j = 0; j < 3; j++) m[12 + j] = -(c[0] * m[j] + c[1] * m[4 + j] + c[2] * m[8 + j]);
    for (i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * t;
    o[20] = a[20] + (b[20] - a[20]) * t;
    o[21] = a[21] + (b[21] - a[21]) * t;
    return 1;
}

/* Rider: position + 21 bones. Teleport (root bone) -> state N. */
static int rider_lerp(const float *pa, const float *pb, const float *ka, const float *kb, float t, float *po, float *ko)
{
    uint32_t b;
    int i;
    if (!(dist3(pa, pb) < TELEPORT) || !(dist3(ka + 12, kb + 12) < TELEPORT)) return -1;
    for (i = 0; i < 3; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
    for (b = 0; b < POSE_N; b++) {
        const float *x = ka + 16 * b, *y = kb + 16 * b;
        float *o = ko + 16 * b;
        if (rot_lerp(x, y, t, o, BONE_CUT_COS) <= 0) { I.n_badrot++; continue; }   /* stays at N */
        for (i = 0; i < 3; i++) o[12 + i] = x[12 + i] + (y[12 + i] - x[12 + i]) * t;
    }
    return 1;
}

/* Cut log (XBOX_FPS_CAP_LOG=1, first 40): race state and position jump. */
static void cut_note(const char *what, uint32_t va, const float *a, const float *b)
{
    static int n;
    uint32_t race = race_ptr();
    if (!s.log || n++ >= 40) return;
    fprintf(stderr, "[INTERP] cut %s %08X: state=%u tick=%u jump=%.1f units\n", what, va,
            race ? MEM32(race + 0x1Cu) : 0, race ? MEM32(race + 0x18u) : 0, dist3(a, b));
}

/* Writes the interpolated state before an InGameState render shown at the
 * instant ts (sample_time); 1 if written. */
static int interp_begin(uint32_t st, double ts)
{
    double a;
    float t;
    uint32_t k;
    VS *o = &I.out;
    I.written = 0;
    I.mode = 3; I.alpha = 0.0f; I.ts = ts;
    if (!I.on || !I.ok || I.cur.st != st || GetCurrentThreadId() != s.tid) return 0;
    if (!vs_equals_mem(&I.cur)) { I.n_mismatch++; I.mode = 2; return 0; }
    a = (ts - I.t_tick) / TICK_MS;
    if (I.synth && s.dup > 0) a = (double)s_dup_k / (s.dup + 1);   /* DUP test: synthetic time */
    I.alpha = (float)a;
    if (a >= 1.0) { I.n_alpha1++; I.mode = 1; return 0; }
    I.mode = 0;
    if (a < 0.0) a = 0.0;
    t = (float)a;
    I.t = t;
    memcpy(o, &I.cur, sizeof *o - sizeof o->w + I.cur.nw * sizeof(float));
    for (k = 0; k < I.cur.n; k++) {
        const Span *p = &I.cur.sp[k];
        const float *x = &I.prev.w[p->off], *y = &I.cur.w[p->off];
        if (p->kind == SP_VIEW) {
            if (view_lerp(x, y, t, &o->w[p->off]) <= 0) { I.n_cut_cam++; cut_note("camera", p->va, x, y); memcpy(&o->w[p->off], y, p->len); }
        } else if (p->kind == SP_POS) {
            const Span *q = &I.cur.sp[k + 1];
            if (rider_lerp(x, y, &I.prev.w[q->off], &I.cur.w[q->off], t, &o->w[p->off], &o->w[q->off]) < 0) {
                I.n_cut_rider++; cut_note("rider", p->va, x, y);
                memcpy(&o->w[p->off], y, p->len); memcpy(&o->w[q->off], &I.cur.w[q->off], q->len);
            }
            k++;
        }
    }
    vs_write(o);
    I.written = 1;
    I.n_interp++;
    if (I.log && I.n_logged < (unsigned long long)I.log) {
        uint32_t race = race_ptr();
        if (race && MEM32(race + 0x1Cu) == 4u && MEM32(race + 0x18u) >= I.log_from) {
            /* view 0, then rider 0: first position block and its pose (bone 0 = root) */
            static const float z[16];
            const float *v = &o->w[0], *r0 = z, *k0 = z;
            for (k = 0; k + 1 < I.cur.n; k++)
                if (I.cur.sp[k].kind == SP_POS) { r0 = &o->w[I.cur.sp[k].off]; k0 = &o->w[I.cur.sp[k + 1].off]; break; }
            I.n_logged++;
            fprintf(stderr, "[INTERP] n=%llu tick=%u extra=%d alpha=%.4f cam=%.4f,%.4f,%.4f r0=%.4f,%.4f,%.4f os0=%.4f,%.4f,%.4f\n",
                    I.n_logged, MEM32(race + 0x18u), s_in_extra, t, v[0], v[1], v[2], r0[0], r0[1], r0[2],
                    k0[12], k0[13], k0[14]);
        }
    }
    return 1;
}

/* How far the render in progress is behind tick N, in ticks: it shows
 * lerp(N-1, N, alpha), so 1 - alpha when interpolated, 0 otherwise. Game
 * thread, read by the speed blur at FRAME_END (speedfx.c). */
double fps_cap_render_lag(void)
{
    if (!I.written) return 0.0;
    return 1.0 - (I.alpha > 0.0f ? (double)I.alpha : 0.0);
}

/* Probe: the view matrix of view 0 at tick N (what the memory holds
 * between renders); 0 when unknown. */
int fps_cap_tick_view(float *m)
{
    uint32_t k;
    if (!I.ok) return 0;
    for (k = 0; k < I.cur.n; k++)
        if (I.cur.sp[k].kind == SP_VIEW) { memcpy(m, &I.cur.w[I.cur.sp[k].off + 4u], 16 * sizeof(float)); return 1; }
    return 0;
}

static void interp_end(void)
{
    if (!I.written) return;
    vs_write(&I.cur);       /* state of tick N, bit exact */
    I.written = 0;
}

/* ── Marker above a rider (XBOX_FIX_MARKER_INTERP, default 1) ─────
 * The relationship icon an AI rider shows above its head (the rival "!")
 * belongs to an indicator object of the rider (rider sub-object +0x3F8).
 * Its world anchor is computed by the tick, 0x43340 (called by the rider's
 * update, 0x455ED): per player slot (count +0x40, 1 or 2, stride 0x70),
 * anchor +0x60 (4 f) = attachment point (+0x130 of [[rider+0x42E8]+4]) +
 * root bone row 2 (rider +0x48D0) * 95.9. The rider's render (0x44FD0,
 * rider vtable slot 2) draws it with 0x43690, which projects that anchor
 * with the current view. Above 60 the camera and the rider are drawn at
 * lerp(N-1, N, t), the anchor stays at tick N: the icon steps at 60 Hz
 * against a smooth rider (ghosting).
 * Fix: the anchors of each indicator are kept for the last two ticks; around
 * 0x43690, in a render that interpolates (I.written), the anchor shown is
 * lerp(N-1, N, t), written back bit exact after the call. Nothing changes at
 * 60 (no interpolation) nor in the logic (the tick never sees the lerp). */
#define MK_MAX      16u
#define MK_SLOTS    2u
#define MK_COUNT    0x40u           /* indicator: number of player slots */
#define MK_ANCHOR   0x60u           /* indicator: anchor of slot 0 (4 f) */
#define MK_STRIDE   0x70u
static struct {
    int on;
    uint32_t seq;                   /* race ticks seen by hook_AD4A0 */
    int in_tick;
    struct { uint32_t ind, seq, n; int has_prev; float prev[MK_SLOTS][4], cur[MK_SLOTS][4]; } e[MK_MAX];
    unsigned long long n_tick, n_outside, n_full, n_lerp, n_cut, n_stale, n_kept;
} M;

static uint32_t mk_count(uint32_t ind)
{
    uint32_t n = MEM32(ind + MK_COUNT);
    return n > MK_SLOTS ? MK_SLOTS : n;
}

void sub_00043340(void);    /* indicator: anchors and fades (thiscall, rider; ret 4) */
void sub_00043690(void);    /* indicator: draw (thiscall) */

void hook_fps_00043340(void)
{
    uint32_t ind = g_ecx, k, i, slot = MK_MAX, n;
    sub_00043340();
    if (!M.on || !I.on || ind < 0x1000u) return;
    if (!M.in_tick) { M.n_outside++; return; }      /* only the tick's anchors are kept */
    M.n_tick++;
    for (i = 0; i < MK_MAX; i++) if (M.e[i].ind == ind) { slot = i; break; }
    for (i = 0; i < MK_MAX && slot == MK_MAX; i++)
        if (!M.e[i].ind || M.seq - M.e[i].seq > 2u) slot = i;      /* free, or not refreshed for 2 ticks */
    if (slot == MK_MAX) { M.n_full++; return; }
    n = mk_count(ind);
    if (M.e[slot].ind != ind || M.e[slot].n != n || (M.e[slot].seq != M.seq && M.e[slot].seq + 1u != M.seq)) {
        M.e[slot].ind = ind; M.e[slot].n = n; M.e[slot].has_prev = 0;
    } else if (M.e[slot].seq + 1u == M.seq) {
        memcpy(M.e[slot].prev, M.e[slot].cur, sizeof M.e[slot].prev);
        M.e[slot].has_prev = 1;
    }   /* same tick again: only cur is refreshed */
    M.e[slot].seq = M.seq;
    for (k = 0; k < n; k++) memcpy(M.e[slot].cur[k], (const void *)XBOX_PTR(ind + MK_ANCHOR + k * MK_STRIDE), 16);
}

void hook_fps_00043690(void)
{
    uint32_t ind = g_ecx, i, k;
    float save[MK_SLOTS][4], put[MK_SLOTS][4];
    int done[MK_SLOTS] = { 0 }, any = 0;
    if (M.on && I.written && ind >= 0x1000u) {
        for (i = 0; i < MK_MAX; i++) if (M.e[i].ind == ind) break;
        if (i < MK_MAX && M.e[i].seq == M.seq && M.e[i].has_prev && M.e[i].n == mk_count(ind)) {
            for (k = 0; k < M.e[i].n; k++) {
                float *m = (float *)XBOX_PTR(ind + MK_ANCHOR + k * MK_STRIDE);
                const float *a = M.e[i].prev[k], *b = M.e[i].cur[k];
                int j;
                if (memcmp(m, b, 16)) { M.n_stale++; continue; }     /* not the tick's anchor: left alone */
                if (!(dist3(a, b) < TELEPORT)) { M.n_cut++; continue; }
                for (j = 0; j < 4; j++) put[k][j] = a[j] + (b[j] - a[j]) * I.t;
                memcpy(save[k], m, 16);
                memcpy(m, put[k], 16);
                done[k] = 1; any = 1;
            }
            if (any) M.n_lerp++;
        }
    }
    sub_00043690();
    if (!any) return;
    for (k = 0; k < MK_SLOTS; k++) {
        float *m;
        if (!done[k]) continue;
        m = (float *)XBOX_PTR(ind + MK_ANCHOR + k * MK_STRIDE);
        if (memcmp(m, put[k], 16)) { M.n_kept++; continue; }      /* written by the draw: kept */
        memcpy(m, save[k], 16);
    }
}

/* ── Leg smoothing (XBOX_FPS_SMOOTH_LEGS=1; off by default) ────────
 * In a carve, the board suspension (rider +0x1F8, ground physics 0x27B63 /
 * 0x27D28) can oscillate at 12-20 Hz; the board node follows it (0x3BD83)
 * and the legs bend with it on every tick: the board and the legs shake
 * under the hips. Display only: each race render shows the legs and the
 * board (bones 14-20) relative to the root as the average of the last 4
 * ticks (with interpolation: the averages at tick N-1 and N, blended by
 * alpha). The root, the lean and the upper body are untouched.
 * Only while riding on the ground (+0x454 == 2 and +0x458 == 3): the history
 * restarts on any other state (jump, landing, fall, trick), on a teleport and
 * on a local step larger than 20 deg or 20 units in one tick.
 * The bones are written before the render and written back bit exact after
 * it; the logic never sees them. At 60 fps the render and tick hooks are
 * installed only for this (no interpolation, no extra frame). */
#define LEG_FIRST    14u
#define LEG_N        7u             /* bones 14..20 */
#define LEG_AVG      4u
#define LEG_HIST     (LEG_AVG + 1u)
#define LEG_STEP_COS 0.98481f       /* cos(10 deg) = local bone turned by more than 20 deg */
#define LEG_STEP_LEN 20.0f
#define LEG_STATE    0x454u
#define LEG_MODE     0x458u

typedef struct { float q[4], t[3]; } LegBone;
typedef struct {
    uint32_t va;                            /* rider */
    uint32_t n;                             /* valid ticks in h[], newest first */
    LegBone  h[LEG_HIST][LEG_N];
    float    raw[(1u + LEG_N) * 16u];       /* tick N as written by the tick: root, bones 14..20 */
    float    save[LEG_N * 16u];             /* bones before the render */
    int      written;
} LegRider;

static struct {
    int on;
    DWORD tid;                              /* thread running the ticks */
    LegRider r[NR_MAX];
    unsigned long long n_render, n_reset, n_skip;
} L;

/* Unit rows of a 4x4 matrix's rotation, or 0 (degenerate, scaled, mirrored). */
static int leg_rows(const float *m)
{
    float n[3];
    return rot_ok(m, n) == 1 && fabsf(n[0] - 1.0f) < 0.02f && fabsf(n[1] - 1.0f) < 0.02f && fabsf(n[2] - 1.0f) < 0.02f;
}

/* Bone relative to the root (row vectors: W = L * R): L = W * R^T. */
static void leg_local(const float *root, const float *w, LegBone *o)
{
    float m[16] = { 0 };
    int i, j, k;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            float a = 0.0f;
            for (k = 0; k < 3; k++) a += w[4 * i + k] * root[4 * j + k];
            m[4 * i + j] = a;
        }
    m2q(m, o->q);
    for (j = 0; j < 3; j++) {
        float a = 0.0f;
        for (k = 0; k < 3; k++) a += (w[12 + k] - root[12 + k]) * root[4 * j + k];
        o->t[j] = a;
    }
}

/* Average of h[from .. from+cnt-1] (quaternions aligned on h[from]). */
static void leg_mean(const LegRider *r, uint32_t b, uint32_t from, uint32_t cnt, LegBone *o)
{
    const float *q0 = r->h[from][b].q;
    float l;
    uint32_t k;
    int i;
    memset(o, 0, sizeof *o);
    for (k = from; k < from + cnt; k++) {
        const LegBone *x = &r->h[k][b];
        float sg = (x->q[0] * q0[0] + x->q[1] * q0[1] + x->q[2] * q0[2] + x->q[3] * q0[3]) < 0.0f ? -1.0f : 1.0f;
        for (i = 0; i < 4; i++) o->q[i] += sg * x->q[i];
        for (i = 0; i < 3; i++) o->t[i] += x->t[i];
    }
    l = sqrtf(o->q[0] * o->q[0] + o->q[1] * o->q[1] + o->q[2] * o->q[2] + o->q[3] * o->q[3]);
    for (i = 0; i < 4; i++) o->q[i] /= l;
    for (i = 0; i < 3; i++) o->t[i] /= (float)cnt;
}

/* After each race tick: history of the local leg pose of every rider. */
static void legs_tick(void)
{
    uint32_t race = race_ptr(), nr, i, b;
    if (!L.on) return;
    if (!L.tid) L.tid = GetCurrentThreadId();
    if (!race || MEM32(race + 0x1Cu) != 4u) { for (i = 0; i < NR_MAX; i++) L.r[i].n = 0; return; }
    nr = MEM32(race + 0x88u); if (nr > NR_MAX) nr = NR_MAX;
    for (i = 0; i < NR_MAX; i++) {
        LegRider *r = &L.r[i];
        uint32_t va = i < nr ? MEM32(race + 0xC4u + 4u * i) : 0;
        const float *pose;
        LegBone cur[LEG_N];
        int ok;
        if (va < 0x1000u) { r->va = 0; r->n = 0; continue; }
        if (va != r->va) { r->va = va; r->n = 0; }
        pose = (const float *)XBOX_PTR(va + POSE_OFF);
        ok = MEM32(va + LEG_STATE) == 2u && MEM32(va + LEG_MODE) == 3u && leg_rows(pose);
        for (b = 0; ok && b < LEG_N; b++) {
            const float *w = pose + 16u * (LEG_FIRST + b);
            if (!leg_rows(w)) { ok = 0; break; }
            leg_local(pose, w, &cur[b]);
            if (r->n) {
                const LegBone *p = &r->h[0][b];
                float d = fabsf(cur[b].q[0] * p->q[0] + cur[b].q[1] * p->q[1] + cur[b].q[2] * p->q[2] + cur[b].q[3] * p->q[3]);
                if (d < LEG_STEP_COS || dist3(cur[b].t, p->t) > LEG_STEP_LEN) { r->n = 0; L.n_reset++; }
            }
        }
        if (ok && r->n && dist3(pose + 12, r->raw + 12) > TELEPORT) { r->n = 0; L.n_reset++; }
        if (!ok) { if (r->n) L.n_reset++; r->n = 0; continue; }
        memmove(&r->h[1], &r->h[0], (LEG_HIST - 1u) * sizeof r->h[0]);
        memcpy(r->h[0], cur, sizeof cur);
        if (r->n < LEG_HIST) r->n++;
        memcpy(r->raw, pose, 16u * sizeof(float));
        memcpy(r->raw + 16, pose + 16u * LEG_FIRST, LEG_N * 16u * sizeof(float));
    }
}

/* Before a race render (after interp_begin): averaged legs on the root shown. */
static void legs_begin(void)
{
    uint32_t race = race_ptr(), nr, i, b;
    if (!L.on || !race || GetCurrentThreadId() != L.tid || MEM32(race + 0x1Cu) != 4u) return;
    nr = MEM32(race + 0x88u); if (nr > NR_MAX) nr = NR_MAX;
    for (i = 0; i < nr; i++) {
        LegRider *r = &L.r[i];
        float *pose;
        uint32_t cnt;
        float t = I.written ? I.alpha : 1.0f;
        if (r->n < 2u || r->va != MEM32(race + 0xC4u + 4u * i)) continue;
        pose = (float *)XBOX_PTR(r->va + POSE_OFF);
        /* without interpolation, memory must still be tick N */
        if (!I.written && (memcmp(pose, r->raw, 16u * sizeof(float))
                           || memcmp(pose + 16u * LEG_FIRST, r->raw + 16, LEG_N * 16u * sizeof(float)))) { L.n_skip++; continue; }
        if (!leg_rows(pose)) { L.n_skip++; continue; }
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        memcpy(r->save, pose + 16u * LEG_FIRST, sizeof r->save);
        cnt = r->n < LEG_AVG ? r->n : LEG_AVG;
        for (b = 0; b < LEG_N; b++) {
            LegBone a, o;
            float m[16] = { 0 }, *w = pose + 16u * (LEG_FIRST + b);
            int j, k, c;
            leg_mean(r, b, 0, cnt, &o);
            if (t < 1.0f) {     /* interpolated root: the average one tick earlier, blended */
                uint32_t c1 = r->n - 1u < LEG_AVG ? r->n - 1u : LEG_AVG;
                float d, l;
                leg_mean(r, b, 1, c1, &a);
                d = a.q[0] * o.q[0] + a.q[1] * o.q[1] + a.q[2] * o.q[2] + a.q[3] * o.q[3];
                for (k = 0; k < 4; k++) o.q[k] = (d < 0.0f ? -a.q[k] : a.q[k]) * (1.0f - t) + o.q[k] * t;
                l = sqrtf(o.q[0] * o.q[0] + o.q[1] * o.q[1] + o.q[2] * o.q[2] + o.q[3] * o.q[3]);
                for (k = 0; k < 4; k++) o.q[k] /= l;
                for (k = 0; k < 3; k++) o.t[k] = a.t[k] * (1.0f - t) + o.t[k] * t;
            }
            /* W = L * R (rotation rows, then translation) */
            q2m(o.q, m);
            for (j = 0; j < 3; j++)
                for (c = 0; c < 3; c++) {
                    float s2 = 0.0f;
                    for (k = 0; k < 3; k++) s2 += m[4 * j + k] * pose[4 * k + c];
                    w[4 * j + c] = s2;
                }
            for (c = 0; c < 3; c++) {
                float s2 = pose[12 + c];
                for (k = 0; k < 3; k++) s2 += o.t[k] * pose[4 * k + c];
                w[12 + c] = s2;
            }
        }
        r->written = 1;
        L.n_render++;
    }
}

/* After the render: the bones as they were before it, bit exact. */
static void legs_end(void)
{
    uint32_t i;
    if (!L.on) return;
    for (i = 0; i < NR_MAX; i++) {
        LegRider *r = &L.r[i];
        if (!r->written) continue;
        memcpy((void *)XBOX_PTR(r->va + POSE_OFF + 0x40u * LEG_FIRST), r->save, sizeof r->save);
        r->written = 0;
    }
}

/* Test XBOX_FPS_CAP_CHECK=3: the same extra render done again WITHOUT
 * interpolation from the same state; the rider words (and RNG) that differ
 * between the two renders are values written by the render that DEPEND on the
 * interpolated state. Used to judge the normal render (which keeps its
 * writes). Two Presents per extra render: measurement only. */
static uint32_t *s_dep_hist;
static unsigned long long s_dep_runs, s_dep_diff, s_dep_rng;
static void dep_experiment(uint32_t app, const uint32_t *rva, uint8_t (*rsv)[0x5A00], uint32_t nr, const uint32_t *rng)
{
    static uint8_t r1[NR_MAX][0x5A00];
    uint32_t i, w, rng1[12];
    int any = 0;
    if (!s_dep_hist) s_dep_hist = (uint32_t *)calloc(0x5A00 / 4, sizeof(uint32_t));
    for (i = 0; i < nr; i++) if (rva[i] >= 0x1000u) memcpy(r1[i], (const void *)XBOX_PTR(rva[i]), 0x5A00u);
    for (i = 0; i < 12; i++) { rng1[i] = MEM32(0x001FAD70u + 4u * i); MEM32(0x001FAD70u + 4u * i) = rng[i]; }
    for (i = 0; i < nr; i++) if (rva[i] >= 0x1000u) memcpy((void *)XBOX_PTR(rva[i]), rsv[i], 0x5A00u);
    I.on = 0;
    guest_render(app);
    I.on = 1;
    for (i = 0; i < 12; i++) if (MEM32(0x001FAD70u + 4u * i) != rng1[i]) { s_dep_rng++; break; }
    for (i = 0; i < nr; i++) {
        const uint32_t *a = (const uint32_t *)r1[i], *b;
        if (rva[i] < 0x1000u) continue;
        b = (const uint32_t *)XBOX_PTR(rva[i]);
        for (w = 0; w < 0x5A00u / 4u; w++) if (a[w] != b[w]) { s_dep_hist[w]++; any = 1; }
    }
    s_dep_runs++; s_dep_diff += any;
    if (s_dep_runs % 200u == 0u) {
        uint32_t shown = 0;
        fprintf(stderr, "[FPSCAP] dependence on the interpolation: %llu tries, %llu with different rider words, %llu with different RNG; words (offset:count)",
                s_dep_runs, s_dep_diff, s_dep_rng);
        for (w = 0; w < 0x5A00u / 4u && shown < 64; w++) if (s_dep_hist[w]) { fprintf(stderr, " +%X:%u", w * 4, s_dep_hist[w]); shown++; }
        fputc('\n', stderr);
    }
}

/* ── one extra render ─────────────────────────────────────────── */

static int extra_render(uint32_t app)
{
    static unsigned n;
    static Snap a;
    uint64_t *pg_before = NULL;
    double t0, d;
    int ok, sample = 0;

    if (s.check) {
        snap_bases(&a, app);
        snap_copy(&a);
        hud_probe_copy(app);
        sample = s_pg && (n++ % 500u) == 250u;
        if (sample) { pg_before = (uint64_t *)malloc(PG_MAX * sizeof(uint64_t)); if (pg_before) pages_hash(pg_before); else sample = 0; }
    }
    t0 = now_ms();
    {
        unsigned seq0 = d3d8_PresentSeq();
        if (s.check == 2) {             /* control: same duration, no render */
            double until = now_ms() + (s.avg_render > 0.0 ? s.avg_render : 8.0);
            while (now_ms() < until) Sleep(0);
            ok = 1;
        } else {
            /* Fidelity: the InGameState render draws from RNG A during the
             * intro fly-over (race state 1); an extra render would shift the
             * sequence the logic draws. RNG states A and B (6 + 6 words,
             * 0x1FAD70) restored as they were before the render. */
            uint32_t rng[12], i;
            static uint8_t rsv[NR_MAX][0x5A00];
            uint32_t rva[NR_MAX], nr = 0, race = race_ptr();
            int hud;
            for (i = 0; i < 12; i++) rng[i] = MEM32(0x001FAD70u + 4u * i);
            /* Interpolation: the render derives a few pose values in the
             * rider (+0x4DF0..) from the interpolated state; an extra render
             * does not exist in the original -> whole riders restored as they
             * were before (0x5A00 bytes each). */
            if (I.on && race && MEM32(MEM32(app + 4u)) && MEM32(MEM32(MEM32(app + 4u)) + 0x18u) == 0x000AB610u) {
                nr = MEM32(race + 0x88u); if (nr > NR_MAX) nr = NR_MAX;
                for (i = 0; i < nr; i++) {
                    rva[i] = MEM32(race + 0xC4u + 4u * i);
                    if (rva[i] >= 0x1000u) memcpy(rsv[i], (const void *)XBOX_PTR(rva[i]), 0x5A00u);
                }
            }
            s_measured = 0;
            /* HUD: this render repeats the normal render's step (HUD section). */
            hud = s_fix_hud && hud_swap_in(app);
            {
                int sv_in = s_in_extra;
                s_in_extra = 1;
                ok = guest_render(app);
                if (s.check == 3 && nr) dep_experiment(app, rva, rsv, nr, rng);
                s_in_extra = sv_in;
            }
            if (hud) hud_swap_out();
            for (i = 0; i < nr; i++)
                if (rva[i] >= 0x1000u && memcmp((const void *)XBOX_PTR(rva[i]), rsv[i], 0x5A00u)) {
                    memcpy((void *)XBOX_PTR(rva[i]), rsv[i], 0x5A00u);
                    P.rider_restored++;
                }
            for (i = 0; i < 12; i++)
                if (MEM32(0x001FAD70u + 4u * i) != rng[i]) break;
            if (i < 12) {
                P.rng_restored++;
                for (i = 0; i < 12; i++) MEM32(0x001FAD70u + 4u * i) = rng[i];
            }
        }
        /* T4: which Presents are duplicates, within the XBOX_D3D_DUMP_FROM window. */
        if (s.dump_from && seq0 + 1u >= s.dump_from && seq0 < s.dump_from + 64u)
            fprintf(stderr, "[FPSCAP] extra render: present %u -> %u (ok=%d)\n", seq0, d3d8_PresentSeq(), ok);
    }
    d = now_ms() - t0;
    P.extra++;
    if (ok) {
        if (!s_measured) render_time(d);   /* menus: render not measured by hook_AB610 */
        P.extra_ok++; P.extra_ms += d;
        if (d > P.extra_max) P.extra_max = d;
    } else {
        P.refused++;
    }
    if (s.check) {
        unsigned m = snap_diff(&a);
        hud_probe_diff();
        P.snaps++;
        rva_note(MEM32(MEM32(MEM32(app + 4u)) + 0x18u), ok);
        if (m & (1u << RG_RNG)) P.d_rng++;
        if (m & (1u << RG_RACE)) P.d_race++;
        if (m & (1u << RG_RIDER)) P.d_rider++;
        if (m & (1u << RG_AUDIO)) P.d_audio++;
        if (m & (1u << RG_STATE)) P.d_state++;
        if (m & (1u << RG_APP)) P.d_app++;
        if (sample) {
            uint32_t i2;
            pages_hash(s_pg);
            for (i2 = 0; i2 < PG_MAX; i2++)
                if (s_pg[i2] != pg_before[i2]) { s_pg_hits[i2]++; P.page_changed++; }
            P.page_scans++;
            free(pg_before);
        }
    }
    return ok;
}

/* ── test: XBOX_FPS_CAP_DUP=N ─────────────────────────────────────
 * After each successful InGameState render (0xAB610), N extra renders
 * without a tick, back to back. Same operation as the cap (render without a
 * tick), but forced: checks "0 difference over >= 10,000 race renders" on a
 * machine where the race render is longer than the tick (the wait at
 * 0xAA296 is then never reached). Same as XBOX_C019_EXTRA. */
void sub_000AB610(void);

/* Test (XBOX_FPS_CAP_STALL=s:ms): one simulated stall of ms in a normal
 * render, s seconds after start; shows the pacing recovers. */
static double s_stall_at, s_stall_ms, s_t0_run;
static void maybe_stall(void)
{
    if (s_stall_ms > 0.0 && !s_in_extra && now_ms() - s_t0_run >= s_stall_at * 1000.0) {
        fprintf(stderr, "[FPSCAP] simulated stall: %.0f ms\n", s_stall_ms);
        Sleep((DWORD)s_stall_ms);
        s_stall_ms = 0.0;
    }
}

/* Test (XBOX_FPS_INTERP_JANK=every:ms): one slow render (ms of busy wait
 * inside it) every `every` InGameState renders in race (state 4), normal or
 * extra; reproduces the stutters of a heavy scene. */
static int s_jank_every;
static double s_jank_ms;
static unsigned s_jank_n;

/* Trace (XBOX_FPS_INTERP_TRACE=file): one line per InGameState render on the
 * loop thread ("R"), per race tick ("T") and per injected stall ("J"); times
 * in ms since start. R: t_start t_end present tick race_state extra mode alpha
 * camera(3) rider0(3) camera_step(3) rider0_step(3) shown_at -- step = tick N -
 * tick N-1, shown_at = the instant alpha is measured at (sample_time). T:
 * t tick from_wait fresh tick_time old_clock seen -- tick_time = the time the
 * tick was produced (the alpha's origin), seen = when the frame wait saw it. */
static FILE *s_trace;
static unsigned s_trace_n;

static void trace_flush(void)
{
    if (s_trace && (++s_trace_n & 255u) == 0u) fflush(s_trace);
}

/* cam / rp: camera and rider 0 positions as the render saw them. */
static void trace_render(uint32_t race, double t0, double t1, const float *cam, const float *rp)
{
    uint32_t k;
    float cv[3] = { 0, 0, 0 }, rv[3] = { 0, 0, 0 };
    if (!race) return;
    if (I.ok) {
        int cvdone = 0, rvdone = 0;
        for (k = 0; k < I.cur.n; k++) {
            const Span *p = &I.cur.sp[k];
            float *dst = NULL;
            int i;
            if (p->kind == SP_VIEW && !cvdone) { dst = cv; cvdone = 1; }
            else if (p->kind == SP_POS && !rvdone) { dst = rv; rvdone = 1; }
            if (dst) for (i = 0; i < 3; i++) dst[i] = I.cur.w[p->off + i] - I.prev.w[p->off + i];
        }
    }
    fprintf(s_trace, "R %.3f %.3f %u %u %u %d %d %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.3f\n",
            t0 - s_t0_run, t1 - s_t0_run, d3d8_PresentSeq(), MEM32(race + 0x18u), MEM32(race + 0x1Cu), s_in_extra,
            I.mode, I.alpha, cam[0], cam[1], cam[2], rp[0], rp[1], rp[2], cv[0], cv[1], cv[2], rv[0], rv[1], rv[2],
            I.ts - s_t0_run);
    trace_flush();
}

/* ── render gate (XBOX_RENDER_GATE, d3d8_sync.h; off by default) ─────
 * Before a render starts (its content is frozen there: interp_begin reads
 * the clock), the game's thread waits until at most N frames are on their
 * way to the screen: frames submitted and not yet presented (the title's
 * own frame counts, producer - consumer, see sub_001688D0) plus frames in
 * the DXGI queue. Only the time of the render moves: every call the game
 * makes, its order and the ticks are the same. Above 60 fps only (at 60 the
 * game's own timer paces the frames, hooks not installed); bounded wait
 * (2 periods + 4 ms) so that a display that stops taking frames (hidden
 * window, display off) cannot hold the game. */
#define GPU_CTX_PTR     0x001776C0u
#define CTX_FRAME_CONS  0x2518u
#define CTX_FRAME_PROD  0x2B60u
int d3d8_sync_gate(void);
int d3d8_sync_immediate(void);
int d3d8_sync_queued(void);
long d3d8_sync_retired(void);
void d3d8_sync_gate_stats(double *stats_minus_count, unsigned long long *resync);
static struct {
    unsigned long long n, waited, timeouts, bypassed;
    double ms, max, t_log;
    int run;                        /* timeouts in a row */
    long stuck_at;                  /* retired count when the gate stood aside */
} GT;

/* 1 if it held the render: the display, not the slot, then times the frame. */
static int render_gate(void)
{
    int gate = d3d8_sync_gate(), pb, q;
    uint32_t ctx;
    double t0, t, lim;
    if (!gate || !g_fps_cap_on || s.cap == 60 || !s.tid || GetCurrentThreadId() != s.tid) return 0;
    /* The display takes no frame (2 timeouts in a row: hidden, off, mode
     * change): the gate stands aside until a frame leaves the queue again,
     * rather than costing its full wait on every render. */
    if (GT.run >= 2 && d3d8_sync_retired() != GT.stuck_at) GT.run = 0;
    ctx = MEM32(GPU_CTX_PTR);
    t0 = t = now_ms();
    lim = t0 + 2.0 * (s.period_ms > 0.0 ? s.period_ms : 4.0) + 4.0;
    if (GT.run >= 2) GT.bypassed++;
    else for (;;) {
        pb = ctx >= 0x1000u ? (int)(MEM32(ctx + CTX_FRAME_PROD) - MEM32(ctx + CTX_FRAME_CONS)) : 0;
        if (pb < 0 || pb > 2) pb = 0;      /* not a count (device not up yet) */
        q = d3d8_sync_queued();
        if (pb + q <= gate) break;
        if ((t = now_ms()) > lim) {
            GT.timeouts++;
            if (++GT.run >= 2) GT.stuck_at = d3d8_sync_retired();
            break;
        }
        YieldProcessor();
        if (lim - t > 0.3) Sleep(0);
    }
    if (t <= lim) GT.run = 0;
    GT.n++;
    if (t > t0) {
        double d = t - t0;
        GT.waited++;
        GT.ms += d;
        if (d > GT.max) GT.max = d;
    }
    if (t - GT.t_log >= 10000.0) {
        if (GT.t_log > 0.0) {
            double dq;
            unsigned long long rs;
            d3d8_sync_gate_stats(&dq, &rs);
            fprintf(stderr, "[GATE] %d: %llu renders, %llu waited (%.3f ms avg per render, max %.2f), %llu timeouts, "
                    "%llu let through (display not taking frames); queue count vs statistics %+.2f, corrected %llu\n",
                    gate, GT.n, GT.waited, GT.n ? GT.ms / (double)GT.n : 0.0, GT.max, GT.timeouts, GT.bypassed, dq, rs);
            fflush(stderr);
            GT.n = GT.waited = GT.timeouts = GT.bypassed = 0;
            GT.ms = GT.max = 0.0;
        }
        GT.t_log = t;
    }
    return t > t0;
}

static void sleep_until(double when);

/* ── the instant a render shows ───────────────────────────────────────
 * An image shows the race at the instant it reaches the screen.
 * - Frames that wait for a refresh (VSync; Adaptive while the game keeps its
 *   pace; VSync off and a slow Adaptive too when composed, without tearing or
 *   while unknown; the legacy chain, assumed to wait: d3d8_sync_immediate):
 *   every render belongs to one slot of the render grid, and its frame
 *   is shown on the refresh after it, one period after the previous one, even
 *   when the render starts a little after its slot: its image is the race at
 *   its slot. Extra renders are dispatched at their slot (grid_wait, the old
 *   loop: frame_t); the tick's render waits for its slot after the tick
 *   (render_slot_wait).
 * - Frames shown as soon as they are ready (VSync off, or Adaptive while the
 *   game is slower than its pace, with tearing allowed and the frames flipped
 *   on their own, not composed by the desktop: d3d8_sync_immediate): the image
 *   is the race at its render's start, its slot when it starts on time.
 * - Without a frame rate limit there is no slot. A render no slot started (a
 *   tick the loop found already due, without a frame wait; a test mode), or
 *   one the render gate held (the display times it), shows its start.
 * A variable refresh display (VRR) shows the frames as they come, but cannot
 * be told here: a render that starts after its slot shows its slot there. */
static double sample_time(double now)
{
    double f = s.frame_t;
    s.frame_t = 0.0;
    if (f > 0.0 && now - f < TICK_MS && f - now < s.period_ms && !d3d8_sync_immediate()) return f;
    return now;
}

/* The tick's render on its slot. The frame wait releases the tick as soon as
 * its event is seen and gives its render the slot that follows (grid_wait);
 * the tick runs, then its render waits here for that slot. Every render, the
 * tick's too, then starts one period after the previous one: before, the
 * tick ran at the slot and its render started the logic's time later (about
 * 1 ms at 240 fps), and so reached the display that much later than the
 * others. The wait is the frame wait's, for XBOX_PERF. */
static void render_slot_wait(void)
{
    double t, w;
    if (!s.slot_due || s_in_extra || GetCurrentThreadId() != s.tid) return;
    s.slot_due = 0;
    w = s.render_slot;
    t = now_ms();
    if (w > t && w - t <= s.period_ms + 1.0) {
        if (g_perf_on) {
            int prev = perf_zone_enter(PZ_FWAIT);
            double t0 = perf_now();
            sleep_until(w);
            perf_zone_leave(PZ_FWAIT, prev, perf_now() - t0);
        } else {
            sleep_until(w);
        }
    }
    s.frame_t = w;
}

static void hook_AB610(void)
{
    uint32_t state = g_ecx, app = MEM32(APP_GLOBAL), sv_eax, sv_ecx, sv_edx;
    double t0;
    int k;
    double t_in;
    render_slot_wait();
    if (render_gate()) s.frame_t = 0.0;
    t0 = now_ms();
    if (s_fix_hud && s.cap != 60 && !s_in_extra && app >= 0x1000u && MEM32(app + 4u) == state
        && (!s.tid || GetCurrentThreadId() == s.tid))
        hud_capture(state);
    interp_begin(state, sample_time(now_ms()));
    legs_begin();
    if (s_trace && GetCurrentThreadId() == s.tid) {
        /* position as the render sees it (interpolated or tick N) */
        uint32_t race = race_ptr(), r0 = race && MEM32(race + 0x88u) ? MEM32(race + 0xC4u) : 0;
        float c[3], r[3] = { 0, 0, 0 };
        memcpy(c, (const void *)XBOX_PTR(state + VIEW_OFF), sizeof c);
        if (r0 >= 0x1000u) memcpy(r, (const void *)XBOX_PTR(r0 + POS_OFF), sizeof r);
        t_in = now_ms();
        sub_000AB610();
        if (s_jank_every > 0 && race && MEM32(race + 0x1Cu) == 4u && ++s_jank_n % (unsigned)s_jank_every == 0u) {
            double until = now_ms() + s_jank_ms;
            fprintf(s_trace, "J %.3f %.1f\n", now_ms() - s_t0_run, s_jank_ms);
            while (now_ms() < until) YieldProcessor();
        }
        trace_render(race, t_in, now_ms(), c, r);
    } else {
        sub_000AB610();
    }
    legs_end();
    interp_end();
    if ((g_eax & 0xFFu) && (!s.tid || GetCurrentThreadId() == s.tid)) {
        maybe_stall();
        render_time(now_ms() - t0);
        s_measured = 1;
    }
    if (s_in_extra || s.dup <= 0 || !(g_eax & 0xFFu) || app < 0x1000u || MEM32(app + 4u) != state
        || (s.tid && GetCurrentThreadId() != s.tid))
        return;
    sv_eax = g_eax; sv_ecx = g_ecx; sv_edx = g_edx;
    s_in_extra = 1;
    for (k = 0; k < s.dup && extra_allowed(app); k++) { s_dup_k = k + 1; extra_render(app); }
    s_dup_k = 0;
    s_in_extra = 0;
    report(0);
    g_eax = sv_eax; g_ecx = sv_ecx; g_edx = sv_edx;
}

/* Menu render 0x7CBD0: on its slot as the race's, and measured (duration for
 * the pacing); nothing interpolated. */
void sub_0007CBD0(void);
static void hook_7CBD0(void)
{
    double t0;
    render_slot_wait();
    render_gate();
    s.frame_t = 0.0;
    t0 = now_ms();
    sub_0007CBD0();
    if ((g_eax & 0xFFu) && (!s.tid || GetCurrentThreadId() == s.tid)) {
        maybe_stall();
        render_time(now_ms() - t0);
        s_measured = 1;
    }
}

/* ── continuous render grid ─────────────────────────────────────────
 * The renders used to restart their grid at every tick (next = tick + period):
 * with a limit that is not a multiple of 60 the last slot before the next tick
 * was cut short. At 144, renders at T, T+6.94, T+13.89 and the tick's at
 * T+16.67: 6.94 / 6.94 / 2.78 ms, 180 fps instead of 144 when a render takes
 * less than 2.78 ms (165 fps in race, 180.0 in the menus). Now the slots run
 * every period whatever the ticks, one render per slot; a tick is released as
 * soon as its event is seen and its render takes the first slot after the
 * event (render_slot_wait): the logic is the same, its image up to one period
 * later (< 16.7 ms: no tick is lost), and every render, normal or extra,
 * starts one period after the previous one and shows the race at its slot
 * (sample_time). XBOX_FPS_CAP_GRID=0: the previous behaviour (comparison). */

/* Slots more than half a period behind are dropped; the phase is kept. */
static void grid_catch_up(double t)
{
    if (s.next_render <= 0.0 || t - s.next_render > 10.0 * TICK_MS) { s.next_render = t; return; }   /* start, pause, load */
    while (t - s.next_render > 0.5 * s.period_ms) s.next_render += s.period_ms;
}

/* Precise sleep until `when` (no event watched). */
static void sleep_until(double when)
{
    double t;
    while ((t = now_ms()) < when) {
        if (when - t > 1.5) Sleep((DWORD)(when - t - 1.0));
        else { YieldProcessor(); if (when - t > 0.3) Sleep(0); }
    }
}

/* Waits for the tick's event until `when`; 1 if it was signaled (*t_sig: when seen). */
static int event_until(uint32_t handle, double when, double *t_sig)
{
    double t = now_ms(), rem = when - t;
    if (rem > 1.5 && guest_wait(handle, (uint32_t)(rem - 1.0)) != WAIT_TIMEOUT_) { *t_sig = now_ms(); return 1; }
    while ((t = now_ms()) < when) {
        if (guest_wait(handle, 0) == WAIT_OBJ_0) { *t_sig = t; return 1; }
        YieldProcessor();
        if (when - t > 0.3) Sleep(0);
    }
    if (guest_wait(handle, 0) == WAIT_OBJ_0) { *t_sig = now_ms(); return 1; }   /* a due tick goes first */
    return 0;
}

/* The frame wait with the grid: extra renders on the free slots, then the
 * tick, released at once; its render's slot in s.render_slot. Sets
 * s.tick_fresh / s.last_tick_sig; s.next_render = the slot after the tick's
 * render. */
static void grid_wait(uint32_t app, uint32_t handle)
{
    double t = now_ms(), sig = 0.0, slot;
    int pending = 0, fresh = 0;
    grid_catch_up(t);
    /* Already signaled (during the previous render, time unknown): not fresh. */
    if (guest_wait(handle, 0) == WAIT_OBJ_0) pending = 1;
    while (!pending) {
        double expected_tick, tick_slot, k;
        t = now_ms();
        grid_catch_up(t);
        slot = s.next_render;
        /* The next tick: one timer period after the previous one (see the
         * old loop below); its slot is the first one at or after it. */
        expected_tick = s.last_tick_sig + TICK_MS;
        if (I.clock && I.on && I.t_est > 0.0 && t - I.t_est < 10.0 * TICK_MS) expected_tick = I.t_est + TICK_MS;
        k = ceil((expected_tick - slot) / s.period_ms);
        tick_slot = slot + (k < 1.0 ? 1.0 : k) * s.period_ms;
        /* An extra render that would end after the tick's slot would push the
         * tick (and the slots after it): this slot stays free. */
        if (!s.noskip && s.avg_render > 0.0 && slot + s.avg_render > tick_slot && t < expected_tick + TICK_MS) {
            P.skip_tick++;
            guest_wait(handle, 0xFFFFFFFFu);
            sig = now_ms(); pending = fresh = 1;
            break;
        }
        if (event_until(handle, slot, &sig)) { pending = fresh = 1; break; }
        t = now_ms();
        if (t > slot + 1.0) P.late++;
        s.frame_t = slot;                                  /* the extra render shows its slot */
        if (!extra_allowed(app) || !extra_render(app)) {   /* frame skip, or refused (as 0xAA28B): wait for the tick */
            s.frame_t = 0.0;
            guest_wait(handle, 0xFFFFFFFFu);
            sig = now_ms(); pending = fresh = 1;
            break;
        }
        s.frame_t = 0.0;
        s.next_render = slot + s.period_ms;
        report(0);
    }
    /* The tick at once; its render on the next slot (render_slot_wait).
     * Loads, videos, frame skip (no extra render there): its render at
     * once too, as the original. */
    t = now_ms();
    grid_catch_up(t);
    slot = s.next_render;
    if (extra_allowed(app)) {
        s.render_slot = slot;
        s.slot_due = 1;
        s.next_render = slot + s.period_ms;
    } else {
        s.next_render = (slot > t ? slot : t) + s.period_ms;
    }
    s.tick_fresh = fresh;
    s.last_tick_sig = fresh ? sig : now_ms();
}


/* ── the limit follows the display ─────────────────────────────────
 * A limit at the integer rate Windows lists (240 for a 239.98 Hz signal)
 * made frames slightly faster than the display: the DXGI queue filled up
 * and stayed full, each frame reaching the screen ~0.7 refresh later (+2.5 to +3 ms at 240 Hz). The limit is now the display's exact rate
 * (d3d8_monitor_rate) less a small margin, so the game never produces
 * faster than the screen and a queue filled by a hitch empties again
 * (one refresh shown twice every 1 / (margin x rate) s: ~8 s at 240 Hz
 * with 0.05 %, against a queue that never empties). 60 Hz displays (59.94
 * included) keep the game's own 60 fps pace. XBOX_FPS_CAP_MATCH_MARGIN=<%>
 * (0 to 5, default 0.05): the margin. */
#define MATCH_MARGIN_PCT 0.05

static double match_fps(double rate)
{
    double f;
    if (rate <= 61.0 || rate > 1000.0) return 60.0;
    f = rate * (1.0 - s.margin);
    return f < 60.0 ? 60.0 : f;
}

/* The limit for a display at `rate` Hz: "Match screen" = the display;
 * a limit asked is lowered to it only when it is above (VSync / Adaptive). */
static double follow_fps(double rate)
{
    double m = match_fps(rate);
    if (s.follow_monitor || s.req_fps <= 0.0 || s.req_fps > m) return m;
    return s.req_fps;
}

/* Game thread, at every frame wait: the window's display changed rate (window
 * created on another display, moved, mode changed): the limit follows. */
static void follow_display(void)
{
    LONG gen;
    double rate = d3d8_display_rate(&gen), f, p;
    if (gen == s.rate_gen) return;
    s.rate_gen = gen;
    if (rate <= 0.0) return;
    f = follow_fps(rate);
    p = 1000.0 / f;
    if (fabs(p - s.period_ms) < 1e-6) return;
    fprintf(stderr, "[FPSCAP] display %.3f Hz: cap %.3f fps (was %.3f)\n", rate, f, 1000.0 / s.period_ms);
    fflush(stderr);
    s.period_ms = p;
    d3d8_sync_set_pace((int)(f + 0.5));
}

/* ── hook 0xB2750 ─────────────────────────────────────────────── */

static void hook_B2750(void)
{
    uint32_t self = g_ecx, app = MEM32(APP_GLOBAL), handle, r;
    double t;

    P.hook_calls++;
    {
        static int shown;
        if ((s.log || s.check) && shown < 4) {
            shown++;
            fprintf(stderr, "[FPSCAP] frame wait: ecx=%08X edi=%u esi=%08X app=%08X [app+0x2C]=%08X tid=%lu\n",
                    self, g_edi, g_esi, app, app >= 0x1000u ? MEM32(app + 0x2Cu) : 0, GetCurrentThreadId());
        }
    }
    /* Only the main loop's wait at 0xAA296 (no tick done). */
    if (g_edi != 0u || app < 0x1000u || g_esi != app || MEM32(app + 0x2Cu) != self
        || (s.tid && GetCurrentThreadId() != s.tid)) {
        g_ecx = self;
        sub_000B2750();
        return;
    }
    if (!s.tid) s.tid = GetCurrentThreadId();
    s.slot_due = 0;       /* a slot the previous release gave and no render took */
    s.frame_t = 0.0;
    if (s.follow) follow_display();
    handle = MEM32(self + 4u);
    if (s.grid) {
        grid_wait(app, handle);
        goto released;
    }
    t = now_ms();
    /* The normal render just happened (tick -> render -> wait). */
    if (s.next_render < s.last_tick_ret) s.next_render = s.last_tick_ret + s.period_ms;

    s.tick_fresh = 0;
    for (;;) {
        double expected_tick = s.last_tick_ret + TICK_MS, wait_until, rem;
        t = now_ms();
        /* The next tick is produced by the game's 60 Hz timer one period
         * after the previous one, whenever it was consumed (a late consumption
         * would otherwise push the expected tick, and the extra renders, later
         * every time). */
        if (I.clock && I.on && I.t_est > 0.0 && t - I.t_est < 10.0 * TICK_MS) expected_tick = I.t_est + TICK_MS;
        /* Already signaled (produced during the previous render, time
         * unknown): tick now, not fresh. */
        if (guest_wait(handle, 0) == WAIT_OBJ_0) goto tick;
        wait_until = s.next_render;
        /* An extra render ending after the expected tick would delay the logic: wait for the tick. */
        if (!s.noskip && s.avg_render > 0.0 && wait_until + s.avg_render > expected_tick && t < expected_tick + TICK_MS) {
            wait_until = 1e300;
            P.skip_tick++;
        }
        rem = wait_until - t;
        if (rem > 1.5) {
            uint32_t ms = rem > 1e8 ? 0xFFFFFFFFu : (uint32_t)(rem - 1.0);
            if (ms != 0xFFFFFFFFu && wait_until == 1e300) ms = 0xFFFFFFFFu;
            r = guest_wait(handle, ms);
            if (r != WAIT_TIMEOUT_) { s.tick_fresh = r == WAIT_OBJ_0; break; }   /* signaled (or error: return, as the original does) */
            if (wait_until == 1e300) continue;
        }
        /* Fine end of the wait (< ~1.5 ms) by polling the event. */
        while ((t = now_ms()) < wait_until) {
            if (guest_wait(handle, 0) == WAIT_OBJ_0) { s.tick_fresh = 1; goto tick; }
            YieldProcessor();
            if (wait_until - t > 0.3) Sleep(0);
        }
        if (guest_wait(handle, 0) == WAIT_OBJ_0) { s.tick_fresh = 1; break; }   /* a due tick goes first */
        if (t > s.next_render + 1.0) P.late++;
        if (!extra_allowed(app)) {          /* wait for the tick (frame skip in progress) */
            r = guest_wait(handle, 0xFFFFFFFFu);
            s.tick_fresh = 1;
            break;
        }
        /* the extra render shows the instant it was due; without a limit no
         * render is due at any instant: it shows its start */
        s.frame_t = s.period_ms > 0.0 ? wait_until : 0.0;
        if (!extra_render(app)) {           /* refused: as 0xAA28B does, wait for the tick */
            s.frame_t = 0.0;
            r = guest_wait(handle, 0xFFFFFFFFu);
            s.tick_fresh = 1;
            break;
        }
        s.frame_t = 0.0;
        /* No catch-up: one render late at most. */
        s.next_render += s.period_ms;
        t = now_ms();
        if (s.next_render < t) s.next_render = t;
        report(0);
    }
tick:
    s.last_tick_ret = now_ms();
    s.last_tick_sig = s.last_tick_ret;
    s.next_render = s.last_tick_ret + s.period_ms;
    /* With a limit, the renders' slots start again here: the tick's render
     * shows this instant, not its start after the logic. Without one there
     * is no slot: every render shows its start. */
    if (s.period_ms > 0.0) {
        s.render_slot = s.last_tick_ret;
        s.slot_due = 1;
    }
    goto done;
released:                 /* grid: next_render and last_tick_sig set by grid_wait */
    s.last_tick_ret = now_ms();
done:
    H.valid = 0;          /* a tick follows: the HUD copy belongs to the previous one */
    P.ticks_ev++;
    s.from_wait = 1;
    report(0);
    g_eax = 0;            /* WAIT_OBJECT_0, as the original */
    g_ecx = self;
    g_esp += 4;           /* ret (dummy return address) */
}

/* ── race tick hook 0xAD4A0 (InGameState vt+0x14) ──────────────── */
void sub_000AD4A0(void);
static int s_dump_at;

static void dump_floats(const char *tag, uint32_t va, uint32_t len)
{
    uint32_t o;
    for (o = 0; o < len; o += 16) {
        const float *f = (const float *)XBOX_PTR(va + o);
        const uint32_t *u = (const uint32_t *)XBOX_PTR(va + o);
        fprintf(stderr, "[DUMP] %s+%X: %12.4f %12.4f %12.4f %12.4f | %08X %08X %08X %08X\n",
                tag, o, f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
    }
}

static void hook_AD4A0(void)
{
    uint32_t app = MEM32(APP_GLOBAL), st = app >= 0x1000u ? MEM32(app + 4u) : 0, race;
    double t = now_ms();
    double ref = I.on ? frame_clock_tick_ref() : 0.0;   /* before the tick runs: the timer's next one is not this */
    int pre = 0;
    if (I.written) interp_end();     /* to be safe: never a tick on an interpolated state */
    if (I.on && vs_layout(&I.pre, st)) { vs_read(&I.pre); pre = 1; }
    M.seq++;
    M.in_tick = 1;
    sub_000AD4A0();
    M.in_tick = 0;
    if (I.on) {
        I.ok = 0;
        if (pre && MEM32(app + 4u) == st && vs_layout(&I.cur, st) && vs_same_layout(&I.cur, &I.pre)) {
            vs_read(&I.cur);
            memcpy(&I.prev, &I.pre, sizeof I.prev);
            I.ok = 1;
        }
        /* Old clock: tick start = return of the frame wait if this tick
         * follows it by less than a tick, else now. A tick consumed late
         * (after a slow render, or the 2nd tick of a catch-up) then gets a
         * wrong time: alpha restarts late (the motion slows down, then jumps)
         * or uses the previous wait (it overshoots, then freezes at N). */
        double old = (t >= s.last_tick_ret && t - s.last_tick_ret < TICK_MS) ? s.last_tick_ret : t;
        /* New clock: the time the game's timer produced this tick. With the
         * frame clock (the default), the exact point of its grid
         * (frame_clock_tick_ref). Otherwise: when the wait saw the event
         * signaled while waiting -- late by as much as the render in progress
         * when the event fired during an extra render; else one period after
         * the previous tick's (the timer is regular), never later than now.
         * Re-anchored after a pause (> 10 ticks). */
        double est;
        if (ref > 0.0 && ref <= t && t - ref < 2.0 * TICK_MS) est = ref;
        /* last_tick_sig: when the wait saw the event (with or without the
         * grid, the tick is released then; without it, = last_tick_ret) */
        else if (s.from_wait && s.tick_fresh) est = s.last_tick_sig;
        else if (I.t_est > 0.0 && t - I.t_est < 10.0 * TICK_MS) { est = I.t_est + TICK_MS; if (est > t) est = t; }
        else { est = s.from_wait ? s.last_tick_ret : t; if (t - est > TICK_MS) est = t; }
        if (est > t) est = t;
        I.t_est = est;
        I.t_tick = I.clock ? est : old;
        if (s_trace) {
            uint32_t rc = race_ptr();
            fprintf(s_trace, "T %.3f %u %d %d %.3f %.3f %.3f\n", t - s_t0_run, rc ? MEM32(rc + 0x18u) : 0, s.from_wait,
                    s.tick_fresh, est - s_t0_run, old - s_t0_run, s.last_tick_sig - s_t0_run);
            trace_flush();
        }
    }
    s.from_wait = 0;
    legs_tick();
    race = race_ptr();
    if (s_dump_at && race && MEM32(race + 0x1Cu) == 4u) {
        uint32_t rt = MEM32(race + 0x18u), r0 = MEM32(race + 0xC4u), k;
        static uint32_t first;
        if (!first) first = rt;
        if (rt - first >= (uint32_t)s_dump_at && rt - first < (uint32_t)s_dump_at + 4u && r0 >= 0x1000u) {
            char tag[32];
            fprintf(stderr, "[DUMP] ===== tick %u r0=%08X st=%08X nv=%u\n", rt, r0, st, MEM32(st + 0x298u));
            dump_floats("st", st + 0xB0u, 0x60u);
            dump_floats("r0", r0 + 0x160u, 0x40u);
            dump_floats("r0", r0 + 0x440u, 0x60u);
            for (k = 0; k < 22; k++) {
                sprintf(tag, "r0os%u", k);
                dump_floats(tag, r0 + 0x48B0u + 0x40u * k, 0x40u);
            }
            fflush(stderr);
        }
    }
}

static void fps_cap_atexit(void)
{
    extern void *xbox_GetMemoryBase(void);      /* xbox_memory_layout.c */
    if (!g_fps_cap_on) return;
    /* The report reads the game's memory (race_ptr). On the error paths
     * that leave main() before the Xbox memory is mapped (default.xbe not
     * loaded, layout refused) or after it is released, nothing ran and
     * there is nothing to read: the process must end on its message, not
     * on an access violation. */
    if (!xbox_GetMemoryBase()) return;
    report(1);
    if (L.on && (s.log || s.check))
        fprintf(stderr, "[LEGS] renders=%llu resets=%llu skipped(memory!=tick)=%llu\n", L.n_render, L.n_reset, L.n_skip);
    if (M.on && (s.log || s.check))
        fprintf(stderr, "[MARKER] tick writes=%llu outside tick=%llu table full=%llu draws lerped=%llu cut=%llu stale=%llu kept=%llu\n",
                M.n_tick, M.n_outside, M.n_full, M.n_lerp, M.n_cut, M.n_stale, M.n_kept);
}

void fps_cap_init(void)
{
    extern int d3d8_monitor_hz(HWND hwnd);     /* d3d8_sync.h */
    extern void d3d8_test_display_hz_apply(void);   /* d3d8_sync.h: XBOX_TEST_DISPLAY_HZ, test only */
    extern double d3d8_monitor_rate(HWND hwnd, const char **src);   /* d3d8_sync.h */
    const char *e = getenv("XBOX_FPS_CAP"), *sy = getenv("XBOX_SYNC"), *src;
    LARGE_INTEGER f;
    int cap = 60, hz;
    double rate, cap_fps = 0.0;
    d3d8_test_display_hz_apply();               /* before the refresh rate is read: "monitor" follows it */
    hz = d3d8_monitor_hz(NULL);
    rate = d3d8_monitor_rate(NULL, &src);       /* exact (239.98), see match_fps */
    {
        const char *m = getenv("XBOX_FPS_CAP_MATCH_MARGIN");
        double v = m && *m ? atof(m) : MATCH_MARGIN_PCT;
        s.margin = (v >= 0.0 && v <= 5.0 ? v : MATCH_MARGIN_PCT) / 100.0;
    }
    if (e && !_stricmp(e, "monitor")) {
        /* Exact refresh rate of the primary monitor (the game window opens
         * there; followed afterwards, follow_display), less the margin;
         * 61 Hz or less (59.94 included), or unknown: 60 = hook not installed. */
        cap_fps = match_fps(rate);
        cap = rate > 61.0 && rate <= 1000.0 ? (int)(cap_fps + 0.5) : 60;
        if (cap == 60) cap_fps = 0.0;
        else s.follow = s.follow_monitor = 1;
        fprintf(stderr, "[FPSCAP] XBOX_FPS_CAP=monitor: display %d Hz (exact %.3f Hz, %s) -> cap %.3f (%.2f %% under)\n",
                hz, rate, src, cap_fps ? cap_fps : 60.0, cap_fps ? 100.0 * s.margin : 0.0);
    } else if (e && *e) {
        char *end;
        long v = strtol(e, &end, 10);
        if (*end || v < 0 || v > 1000 || (v > 0 && v < 60)) {
            fprintf(stderr, "[FPSCAP] XBOX_FPS_CAP=%s invalid (0 | 60..1000): 60\n", e);
            v = 60;
        }
        cap = (int)v;
    }
    /* With VSync / Adaptive, a cap above the monitor (or no limit) makes the
     * pump's Present wait on every extra frame, which delays the game (49 fps,
     * 18 % of frames held at 60 Hz): the cap is lowered to the refresh rate.
     * XBOX_FPS_CAP_SYNC_CLAMP=0 (test): no clamp. The display's exact rate
     * (less the margin) is the bound: a limit of 240 on a 239.98 Hz display
     * filled the queue as "Match screen" did; and the bound follows the
     * display afterwards (follow_display). */
    if (sy && (!_stricmp(sy, "vsync") || !_stricmp(sy, "adaptive")) && rate > 0.0 && !s.follow_monitor
        && !((e = getenv("XBOX_FPS_CAP_SYNC_CLAMP")) && e[0] == '0')) {
        double m = match_fps(rate);
        s.req_fps = cap;
        if (cap == 0 || cap > m) {
            int was = cap;
            cap = rate > 61.0 && rate <= 1000.0 ? (int)(m + 0.5) : 60;
            cap_fps = cap == 60 ? 0.0 : m;
            fprintf(stderr, "[FPSCAP] XBOX_SYNC=%s: cap %d lowered to the display (%d Hz, exact %.3f Hz, %s) -> %.3f\n",
                    sy, was, hz, rate, src, cap_fps ? cap_fps : 60.0);
        }
        s.follow = cap != 60;
    }
    /* Adaptive sync compares the frames' arrival to this pace when it is
     * below the display (60 included: the game's own pace, no hook). */
    d3d8_sync_set_pace(cap);
    e = getenv("XBOX_FPS_SMOOTH_LEGS"); L.on = e && e[0] == '1';
    if (cap == 60 && !L.on) return;    /* default: hook not installed */
    QueryPerformanceFrequency(&f);
    s.freq = (double)f.QuadPart;
    s.cap = cap;
    s.period_ms = cap_fps > 0.0 ? 1000.0 / cap_fps : cap ? 1000.0 / cap : 0.0;
    if (s.follow) {
        LONG g;
        (void)d3d8_display_rate(&g);
        s.rate_gen = g;     /* the game window's display: read when it is created (gen moves) */
    }
    e = getenv("XBOX_FPS_CAP_LOG");   s.log = e && e[0] == '1';
    e = getenv("XBOX_FPS_CAP_CHECK"); s.check = e ? atoi(e) : 0;   /* 1 = probe, 2 = control without render */
    e = getenv("XBOX_FPS_CAP_DUP"); s.dup = e ? atoi(e) : 0;     /* test, see hook_AB610 */
    e = getenv("XBOX_FPS_INTERP_DUMP"); s_dump_at = e ? atoi(e) : 0;
    e = getenv("XBOX_FPS_INTERP"); I.on = e && *e ? e[0] == '1' : 1;     /* default 1 when cap != 60 */
    if (cap == 60) I.on = 0;            /* 60: no interpolation, no extra frame */
    e = getenv("XBOX_FIX_MARKER_INTERP"); M.on = I.on && !(e && e[0] == '0');   /* default 1 */
    e = getenv("XBOX_FPS_INTERP_SYNTH"); I.synth = e && e[0] == '1';   /* DUP test: alpha = k / (DUP + 1) */
    e = getenv("XBOX_FPS_INTERP_LOG");
    if (e && *e) { I.log = atoi(e); e = strchr(e, '@'); I.log_from = e ? (uint32_t)atoi(e + 1) : 0; }
    e = getenv("XBOX_FPS_CAP_STALL");
    if (e && *e) { s_stall_at = atof(e); e = strchr(e, ':'); s_stall_ms = e ? atof(e + 1) : 0.0; }
    e = getenv("XBOX_FPS_INTERP_CLOCK"); I.clock = !(e && e[0] == '0');   /* 0 = old clock (comparison) */
    e = getenv("XBOX_FPS_INTERP_JANK");
    if (e && *e) { s_jank_every = atoi(e); e = strchr(e, ':'); s_jank_ms = e ? atof(e + 1) : 0.0; }
    e = getenv("XBOX_FPS_INTERP_TRACE");
    if (e && *e) {
        s_trace = fopen(e, "w");
        if (s_trace) setvbuf(s_trace, NULL, _IOFBF, 1 << 16);
    }
    e = getenv("XBOX_FPS_CAP_NOSKIP"); s.noskip = e && e[0] == '1';   /* test: render even if it delays the tick */
    e = getenv("XBOX_FPS_CAP_GRID"); s.grid = cap > 60 && !(e && e[0] == '0');   /* default 1; no limit: no grid */
    if (cap > 60) fprintf(stderr, "[FPSCAP] XBOX_FPS_CAP_GRID=%d (%s)\n", s.grid,
                          s.grid ? "continuous render grid, tick on its slot" : "grid restarted at every tick");
    e = getenv("XBOX_FIX_HUD_RATE"); s_fix_hud = !(e && e[0] == '0');  /* default 1 */
    fprintf(stderr, "[FPSCAP] XBOX_FIX_HUD_RATE=%d (overlay animations at tick rate, no extra frame while paused)\n", s_fix_hud);
    e = getenv("XBOX_D3D_DUMP_FROM"); s.dump_from = (e && getenv("XBOX_D3D_DUMP")) ? (unsigned)atoi(e) : 0;
    if (s.check) {
        int rg;
        for (rg = 0; rg < RG_N; rg++) s_rg_hist[rg] = (uint32_t *)calloc(s_rg_len[rg] / 4, sizeof(uint32_t));
    }
    if (s.check) s_pg = (uint64_t *)calloc(PG_MAX, sizeof(uint64_t));
    P.t0 = s_t0_run = now_ms();
    g_fps_cap_on = 1;
    atexit(fps_cap_atexit);
    fprintf(stderr, "[FPSCAP] cap %d%s, period %.4f ms%s (%s; logic 60 Hz)%s%s\n",
            cap, cap ? " fps" : " = no limit", s.period_ms, s.follow ? ", follows the display" : "",
            cap == 60 ? "no extra frame"
                      : I.on ? "camera + riders interpolation" : "duplicates without a tick",
            s.check ? "; state probe active" : "", L.on ? "; leg smoothing" : "");
}


void (*fps_cap_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x000B2750u && s.cap != 60) return hook_B2750;
    if (xbox_va == 0x000AB610u) return hook_AB610;
    if (xbox_va == 0x0007CBD0u && s.cap != 60) return hook_7CBD0;
    if (xbox_va == 0x000AD4A0u && (I.on || s_dump_at || L.on)) return hook_AD4A0;
    /* plain form: the regen also routes the direct calls (fix_manual_hook_calls.py);
     * both hooks pass straight through when the marker fix is off (60, M.on = 0) */
    if (xbox_va == 0x00043340u) return hook_fps_00043340;
    if (xbox_va == 0x00043690u) return hook_fps_00043690;
    return 0;
}
