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

void sub_000B2750(void);    /* XBoxExecutionMan_WaitForFrameEvent (thiscall, ret) */
void sub_00151D07(void);    /* WaitForSingleObject (stdcall, ret 8) */
unsigned d3d8_PresentSeq(void);

int g_fps_cap_on;
static int s_in_extra;          /* an extra render is in progress */
static int s_dup_k;             /* DUP mode: index of the render after the normal one */

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
    unsigned dump_from;             /* XBOX_D3D_DUMP_FROM (duplicate log, T4) */            /* running average of an extra render (ms) */
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
    int clock;                      /* XBOX_FPS_INTERP_CLOCK: 1 = timer-based tick time, 0 = consumption time */
    int mode;                       /* last interp_begin: 0 interpolated, 1 alpha >= 1, 2 memory != tick, 3 not applicable */
    float alpha;                    /* last alpha (before the >= 1 test) */
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

/* ── statistiques ─────────────────────────────────────────────── */

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
     * original. */
    if (va == 0x000AB610u) return (int32_t)MEM32(st + 0x6Cu) <= 0;
    return va == 0x0007CBD0u;
}

/* ── interpolation ────────────────────────────────────────────────
 * A render shows lerp(tick N-1, tick N, alpha), alpha = time since tick N /
 * 16.67 ms: the normal render (right after the tick) as well as the extra
 * ones. Display is one tick late (+16.7 ms latency), no extrapolation.
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

/* Writes the interpolated state before an InGameState render; 1 if written. */
static int interp_begin(uint32_t st)
{
    double a;
    float t;
    uint32_t k;
    VS *o = &I.out;
    I.written = 0;
    I.mode = 3; I.alpha = 0.0f;
    if (!I.on || !I.ok || I.cur.st != st || GetCurrentThreadId() != s.tid) return 0;
    if (!vs_equals_mem(&I.cur)) { I.n_mismatch++; I.mode = 2; return 0; }
    a = (now_ms() - I.t_tick) / TICK_MS;
    if (I.synth && s.dup > 0) a = (double)s_dup_k / (s.dup + 1);   /* DUP test: synthetic time */
    I.alpha = (float)a;
    if (a >= 1.0) { I.n_alpha1++; I.mode = 1; return 0; }
    I.mode = 0;
    if (a < 0.0) a = 0.0;
    t = (float)a;
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

static void interp_end(void)
{
    if (!I.written) return;
    vs_write(&I.cur);       /* state of tick N, bit exact */
    I.written = 0;
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
            {
                int sv_in = s_in_extra;
                s_in_extra = 1;
                ok = guest_render(app);
                if (s.check == 3 && nr) dep_experiment(app, rva, rsv, nr, rng);
                s_in_extra = sv_in;
            }
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
 * camera(3) rider0(3) camera_step(3) rider0_step(3) -- step = tick N - tick N-1. */
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
    fprintf(s_trace, "R %.3f %.3f %u %u %u %d %d %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
            t0 - s_t0_run, t1 - s_t0_run, d3d8_PresentSeq(), MEM32(race + 0x18u), MEM32(race + 0x1Cu), s_in_extra,
            I.mode, I.alpha, cam[0], cam[1], cam[2], rp[0], rp[1], rp[2], cv[0], cv[1], cv[2], rv[0], rv[1], rv[2]);
    trace_flush();
}

static void hook_AB610(void)
{
    uint32_t state = g_ecx, app = MEM32(APP_GLOBAL), sv_eax, sv_ecx, sv_edx;
    double t0 = now_ms();
    int k;
    double t_in;
    interp_begin(state);
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

/* Menu render 0x7CBD0: only measured (duration for the pacing). */
void sub_0007CBD0(void);
static void hook_7CBD0(void)
{
    double t0 = now_ms();
    sub_0007CBD0();
    if ((g_eax & 0xFFu) && (!s.tid || GetCurrentThreadId() == s.tid)) {
        maybe_stall();
        render_time(now_ms() - t0);
        s_measured = 1;
    }
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
    handle = MEM32(self + 4u);
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
        if (!extra_render(app)) {           /* refused: as 0xAA28B does, wait for the tick */
            r = guest_wait(handle, 0xFFFFFFFFu);
            s.tick_fresh = 1;
            break;
        }
        /* No catch-up: one render late at most. */
        s.next_render += s.period_ms;
        t = now_ms();
        if (s.next_render < t) s.next_render = t;
        report(0);
    }
tick:
    P.ticks_ev++;
    s.last_tick_ret = now_ms();
    s.from_wait = 1;
    s.next_render = s.last_tick_ret + s.period_ms;
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
    int pre = 0;
    if (I.written) interp_end();     /* to be safe: never a tick on an interpolated state */
    if (I.on && vs_layout(&I.pre, st)) { vs_read(&I.pre); pre = 1; }
    sub_000AD4A0();
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
        /* New clock: the time the game's timer produced this tick. Exact when
         * the wait saw the event signaled while waiting; otherwise one period
         * after the previous tick's (the timer is regular), never later than
         * now. Re-anchored after a pause (> 10 ticks). */
        double est;
        if (s.from_wait && s.tick_fresh) est = s.last_tick_ret;
        else if (I.t_est > 0.0 && t - I.t_est < 10.0 * TICK_MS) { est = I.t_est + TICK_MS; if (est > t) est = t; }
        else { est = s.from_wait ? s.last_tick_ret : t; if (t - est > TICK_MS) est = t; }
        if (est > t) est = t;
        I.t_est = est;
        I.t_tick = I.clock ? est : old;
        if (s_trace) {
            uint32_t rc = race_ptr();
            fprintf(s_trace, "T %.3f %u %d %d %.3f %.3f\n", t - s_t0_run, rc ? MEM32(rc + 0x18u) : 0, s.from_wait, s.tick_fresh,
                    est - s_t0_run, old - s_t0_run);
            trace_flush();
        }
    }
    s.from_wait = 0;
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

static void fps_cap_atexit(void) { if (g_fps_cap_on) report(1); }

void fps_cap_init(void)
{
    extern int d3d8_monitor_hz(HWND hwnd);     /* d3d8_sync.h */
    const char *e = getenv("XBOX_FPS_CAP"), *sy = getenv("XBOX_SYNC");
    LARGE_INTEGER f;
    int cap = 60, hz = d3d8_monitor_hz(NULL);
    if (e && !_stricmp(e, "monitor")) {
        /* Refresh rate of the primary monitor (the game window opens there);
         * 60 Hz or less, or unknown: 60 = hook not installed. */
        cap = hz > 61 && hz <= 1000 ? hz : 60;
        fprintf(stderr, "[FPSCAP] XBOX_FPS_CAP=monitor: display %d Hz -> cap %d\n", hz, cap);
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
     * XBOX_FPS_CAP_SYNC_CLAMP=0 (test): no clamp. */
    if (sy && (!_stricmp(sy, "vsync") || !_stricmp(sy, "adaptive")) && hz > 0
        && (cap == 0 || cap > hz) && !((e = getenv("XBOX_FPS_CAP_SYNC_CLAMP")) && e[0] == '0')) {
        int was = cap;
        cap = hz > 61 && hz <= 1000 ? hz : 60;
        fprintf(stderr, "[FPSCAP] XBOX_SYNC=%s: cap %d lowered to the display (%d Hz) -> %d\n", sy, was, hz, cap);
    }
    if (cap == 60) return;    /* default: hook not installed */
    QueryPerformanceFrequency(&f);
    s.freq = (double)f.QuadPart;
    s.cap = cap;
    s.period_ms = cap ? 1000.0 / cap : 0.0;
    e = getenv("XBOX_FPS_CAP_LOG");   s.log = e && e[0] == '1';
    e = getenv("XBOX_FPS_CAP_CHECK"); s.check = e ? atoi(e) : 0;   /* 1 = probe, 2 = control without render */
    e = getenv("XBOX_FPS_CAP_DUP"); s.dup = e ? atoi(e) : 0;     /* test, see hook_AB610 */
    e = getenv("XBOX_FPS_INTERP_DUMP"); s_dump_at = e ? atoi(e) : 0;
    e = getenv("XBOX_FPS_INTERP"); I.on = e && *e ? e[0] == '1' : 1;     /* default 1 when cap != 60 */
    e = getenv("XBOX_FPS_INTERP_SYNTH"); I.synth = e && e[0] == '1';   /* test DUP : alpha = k / (DUP + 1) */
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
    e = getenv("XBOX_D3D_DUMP_FROM"); s.dump_from = (e && getenv("XBOX_D3D_DUMP")) ? (unsigned)atoi(e) : 0;
    if (s.check) {
        int rg;
        for (rg = 0; rg < RG_N; rg++) s_rg_hist[rg] = (uint32_t *)calloc(s_rg_len[rg] / 4, sizeof(uint32_t));
    }
    if (s.check) s_pg = (uint64_t *)calloc(PG_MAX, sizeof(uint64_t));
    P.t0 = s_t0_run = now_ms();
    g_fps_cap_on = 1;
    atexit(fps_cap_atexit);
    fprintf(stderr, "[FPSCAP] cap %d%s (%s; logic 60 Hz)%s\n",
            cap, cap ? " fps" : " = no limit", I.on ? "camera + riders interpolation" : "duplicates without a tick",
            s.check ? "; state probe active" : "");
}

void (*fps_cap_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x000B2750u) return hook_B2750;
    if (xbox_va == 0x000AB610u) return hook_AB610;
    if (xbox_va == 0x0007CBD0u) return hook_7CBD0;
    if (xbox_va == 0x000AD4A0u && (I.on || s_dump_at)) return hook_AD4A0;
    return 0;
}
