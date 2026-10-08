/*
 * np_racebench -- replayed race for benchmarks.
 * See np_racebench.h for the switch, the files and the limits.
 */
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../recomp/recomp_types.h"
#include "np_cmdlog.h"
#include "np_racebench.h"

extern void sub_000AC9B0(void);     /* InGameState_LoadLevel */
extern void sub_0002E040(void);     /* Race_ResetPlayerRoster */
unsigned d3d8_PresentSeq(void);
void d3d8_RequestScreenshot(const wchar_t *path);
int d3d8_ScreenshotPending(void);
int xinput_hle_press(const char *name, int ms);

int g_np_rb_on = 0;
static int s_padmute = 0;   /* XBOX_RACE_PADMUTE=1 or replay on */

#define RB_UNSET     0xFFFFFFFFu
#define RB_NSTATES   16u
#define RB_MAXR      8u
#define RB_ROSTER_VA 0x001DE900u
#define RB_ROSTER_N  0x001DE8FCu
#define RB_APP       0x001E3C7Cu

/* Reference (read from the .npcl / .nprs) */
static struct {
    uint32_t track, seed98, seed9c, nriders, nroster, player_idx;
    uint8_t  roster[NPCL_MAX_RIDERS * NPCL_ROSTER_STRIDE];
    uint32_t r_start[RB_NSTATES];
    uint32_t fmax;                        /* highest race+0x18 recorded */
    uint32_t last4;                       /* last race+0x18 recorded in state 4 */
    uint8_t  *st_at;                      /* race state per race+0x18 */
    uint32_t *cmd[RB_MAXR];
    float    *sf[RB_MAXR];
    uint8_t  *valid[RB_MAXR];
    uint8_t  *state[RB_MAXR];             /* (fmax+1) x NPRS_SET */
    uint8_t  *have[RB_MAXR];
    uint8_t  *view;                       /* (fmax+1) x NPRS_SET */
    uint8_t  *have_view;
    int      have_rng;
    uint32_t rng_a[6], rng_b[6];
} R;

/* Settings */
static struct {
    uint32_t period;
    float    thr, jump;
    int      cam;
    uint32_t shot_lo[32], shot_hi[32], nshot;   /* XBOX_RACEBENCH_SHOT_TICKS */
    wchar_t  shot_pre[MAX_PATH];
    uint32_t pause_at;                          /* XBOX_RACEBENCH_PAUSE (0 = none) */
} P;

/* Run state */
static struct {
    uint32_t race, races, last_rf, cur_rf;
    uint32_t g_start[RB_NSTATES];
    int      done, rng_done, roster_done, in_tick;
    unsigned long long taken, fallback, n_corr, n_thr, n_jump, n_cam, ticks4;
    float    err_max;
    /* current tick line */
    uint32_t gf, st, present, corr_mask;
    long long t_us;
    float    err[RB_MAXR];
    float    cam_eye, cam_mat;
    FILE     *log;
    int      paused;
    LARGE_INTEGER qf;
} S;

static uint32_t rb_race_ptr(void)
{
    uint32_t app = MEM32(RB_APP), lvl;
    if (app < 0x1000u || (lvl = MEM32(app + 0x72Cu)) < 0x1000u) return 0;
    return MEM32(lvl + 0x1Cu);
}

/* InGameState (current game state object, [app+4]) */
static uint32_t rb_state_obj(void)
{
    uint32_t app = MEM32(RB_APP);
    return app >= 0x1000u ? MEM32(app + 4u) : 0;
}

/* ── recording side (np_cmdlog) ── */
void nprs_fill_rider(nprs_record *r, uint32_t rider)
{
    memcpy(r->data, (const void *)XBOX_PTR(rider + 0x170u), 0x50);
    memcpy(r->data + 0x50, (const void *)XBOX_PTR(rider + 0x454u), 0x08);
}

int nprs_fill_view(nprs_record *r)
{
    uint32_t st = rb_state_obj();
    if (st < 0x1000u) return 0;
    memcpy(r->data, (const void *)XBOX_PTR(st + NPRS_VIEW_OFF), NPRS_SET);
    return 1;
}

/* ── replay ── */
static void rb_summary(const char *why)
{
    fprintf(stderr, "[RACEBENCH] %s: race=%u ticks_in_race=%llu commands=%llu fallbacks=%llu corrections=%llu "
            "(on threshold %llu) jumps>%.0f=%llu gap_max=%.2f camera_copies=%llu\n",
            why, S.races, S.ticks4, S.taken, S.fallback, S.n_corr, S.n_thr, P.jump, S.n_jump, S.err_max, S.n_cam);
    fflush(stderr);
}

static void rb_end(const char *why)
{
    if (S.done) return;
    S.done = 1;
    if (S.log) { fflush(S.log); }
    fprintf(stderr, "[RACEBENCH] END (%s) at race+0x18=%u ref=%u\n", why, S.last_rf, S.gf);
    rb_summary("end");
}

static void rb_flush_tick(void)
{
    uint32_t i;
    if (!S.in_tick || !S.log) { S.in_tick = 0; return; }
    fprintf(S.log, "%u,%u,%u,%u,%lld", S.gf, S.cur_rf, S.st, S.present, S.t_us);
    for (i = 0; i < R.nriders; i++) fprintf(S.log, ",%.4f", S.err[i]);
    fprintf(S.log, ",%u,%.4f,%.6f\n", S.corr_mask, S.cam_eye, S.cam_mat);
    if (S.st == 4u && (++S.ticks4 % 60u) == 0) fflush(S.log);
    S.in_tick = 0;
}

static int rb_ref_tick(uint32_t st, uint32_t rf, uint32_t *gf)
{
    if (st >= RB_NSTATES || R.r_start[st] == RB_UNSET || S.g_start[st] == RB_UNSET || rf < S.g_start[st])
        return 0;
    *gf = R.r_start[st] + (rf - S.g_start[st]);
    return *gf <= R.fmax && R.st_at[*gf] == st;
}

/* Camera gap (and optional copy), first command call of the tick. */
static void rb_camera(uint32_t gf)
{
    uint32_t st = rb_state_obj(), i;
    const float *ref;
    float d2 = 0.0f, dm = 0.0f;
    S.cam_eye = -1.0f; S.cam_mat = -1.0f;
    if (st < 0x1000u || !R.have_view || !R.have_view[gf]) return;
    ref = (const float *)(R.view + (size_t)gf * NPRS_SET);
    for (i = 0; i < 3; i++) {
        float d = MEMF(st + NPRS_VIEW_OFF + 4u * i) - ref[i];
        d2 += d * d;
    }
    for (i = 0; i < 12; i++) {   /* view matrix +0xC0 = block offset 0x10; rotation (3x3) only */
        float d;
        if ((i & 3u) == 3u) continue;
        d = fabsf(MEMF(st + NPRS_VIEW_OFF + 0x10u + 4u * i) - ref[4 + i]);
        if (d > dm) dm = d;
    }
    S.cam_eye = sqrtf(d2);
    S.cam_mat = dm;
    if (P.cam && P.period && (gf % P.period) == 0 && S.st == 4u) {
        memcpy((void *)XBOX_PTR(st + NPRS_VIEW_OFF), ref, NPRS_SET);
        S.n_cam++;
    }
}

void np_rb_before(void)
{
    uint32_t race = rb_race_ptr(), st, rf, i, gf;
    LARGE_INTEGER now;
    if (!race || S.done) return;
    rf = MEM32(race + 0x18u);
    if (race != S.race) {
        if (S.races >= 1) { rb_flush_tick(); rb_end("new race object"); return; }
        S.race = race;
        S.races++;
        for (i = 0; i < RB_NSTATES; i++) S.g_start[i] = RB_UNSET;
    } else if (rf < S.last_rf) {
        rb_flush_tick(); rb_end("race+0x18 went back"); return;
    }
    S.last_rf = rf;
    st = MEM32(race + 0x1Cu);
    if (st < RB_NSTATES && S.g_start[st] == RB_UNSET) {
        S.g_start[st] = rf;
        fprintf(stderr, "[RACEBENCH] state %u: race+0x18=%u (reference: %u)\n", st, rf,
                R.r_start[st] == RB_UNSET ? 0xFFFFFFFFu : R.r_start[st]);
        fflush(stderr);
    }
    if (st == 5u) { rb_flush_tick(); rb_end("EndRace"); return; }
    if (st == 3u && !S.rng_done && R.have_rng) {
        for (i = 0; i < 6; i++) {
            MEM32(NPCL_RNG_A_VA + 4u * i) = R.rng_a[i];
            MEM32(NPCL_RNG_B_VA + 4u * i) = R.rng_b[i];
        }
        S.rng_done = 1;
        fprintf(stderr, "[RACEBENCH] RNG A/B forced at the start of state 3 (race+0x18=%u)\n", rf);
    }
    if (S.in_tick && rf == S.cur_rf) return;
    /* new tick */
    rb_flush_tick();
    if (!rb_ref_tick(st, rf, &gf)) {
        if (st == 4u && S.g_start[4] != RB_UNSET && R.r_start[4] != RB_UNSET
            && R.r_start[4] + (rf - S.g_start[4]) > R.last4) {
            S.gf = R.r_start[4] + (rf - S.g_start[4]);
            rb_end("reference exhausted");
        }
        return;
    }
    QueryPerformanceCounter(&now);
    S.in_tick = 1;
    S.cur_rf = rf;
    S.gf = gf;
    if (st == 4u) {
        for (i = 0; i < P.nshot; i++)
            if (gf >= P.shot_lo[i] && gf <= P.shot_hi[i]) {
                wchar_t path[MAX_PATH];
                DWORD w0 = GetTickCount();
                /* One shot per tick: the game waits (bench only) until the
                 * previous request has been taken by a Present, so a burst
                 * of consecutive ticks gives consecutive frames. */
                while (d3d8_ScreenshotPending() && GetTickCount() - w0 < 3000u) Sleep(1);
                _snwprintf(path, MAX_PATH, L"%lst%05u.png", P.shot_pre, gf);
                path[MAX_PATH - 1] = 0;
                d3d8_RequestScreenshot(path);
                fprintf(stderr, "[RACEBENCH] shot ref=%u race+0x18=%u present=%u\n", gf, rf, d3d8_PresentSeq());
                break;
            }
        if (P.pause_at && gf == P.pause_at && !S.paused) {
            S.paused = 1;
            S.done = 1;               /* pad live again: the START below gets through */
            xinput_hle_press("START", 300);
            fprintf(stderr, "[RACEBENCH] PAUSE at ref=%u race+0x18=%u\n", gf, rf);
            fflush(stderr);
        }
    }
    S.st = st;
    S.present = d3d8_PresentSeq();
    S.t_us = (long long)((double)now.QuadPart * 1e6 / (double)S.qf.QuadPart);
    S.corr_mask = 0;
    for (i = 0; i < RB_MAXR; i++) S.err[i] = -1.0f;
    rb_camera(gf);
}

void np_rb_after(uint32_t rider, uint32_t pcmd, int is_player)
{
    uint32_t race, n, idx, st, rf, gf, i;
    (void)is_player;
    if (S.done || !pcmd) return;
    race = rb_race_ptr();
    if (!race || race != S.race) return;
    n = MEM32(race + 0x88u);
    if (n > R.nriders) n = R.nriders;
    for (idx = 0; idx < n; idx++)
        if (MEM32(race + 0xC4u + 4u * idx) == rider) break;
    if (idx >= n) return;
    st = MEM32(race + 0x1Cu);
    rf = MEM32(race + 0x18u);
    if (!rb_ref_tick(st, rf, &gf) || !R.valid[idx][gf]) { S.fallback++; return; }
    MEM32(pcmd) = R.cmd[idx][gf];
    MEMF(rider + 0x15Cu) = R.sf[idx][gf];
    S.taken++;
    if (st != 4u || !R.have[idx] || !R.have[idx][gf]) return;
    {
        const uint8_t *d = R.state[idx] + (size_t)gf * NPRS_SET;
        const float *p = (const float *)d;
        float d2 = 0.0f, err;
        int periodic, over;
        for (i = 0; i < 3; i++) {
            float dv = MEMF(rider + 0x170u + 4u * i) - p[i];
            d2 += dv * dv;
        }
        err = sqrtf(d2);
        if (S.in_tick && S.cur_rf == rf && idx < RB_MAXR) S.err[idx] = err;
        periodic = P.period && (gf % P.period) == 0;
        over = P.thr > 0.0f && err > P.thr;
        if (!periodic && !over) return;
        memcpy((void *)XBOX_PTR(rider + 0x170u), d, 0x50);
        memcpy((void *)XBOX_PTR(rider + 0x454u), d + 0x50, 0x08);
        S.n_corr++;
        if (over) S.n_thr++;
        if (err > P.jump) S.n_jump++;
        if (err > S.err_max) S.err_max = err;
        if (S.in_tick && S.cur_rf == rf) S.corr_mask |= 1u << idx;
        if (S.n_corr % 6000u == 0) rb_summary("summary");
    }
}

/* ── 0xAC9B0: InGameState_LoadLevel (thiscall, 0 arg, ret) ── */
static void hook_AC9B0(void)
{
    uint32_t ecx = g_ecx, i;
    if (S.races == 0 && !S.roster_done) {
        uint32_t cur = MEM32(RB_ROSTER_N);
        for (i = 0; i < R.nroster * NPCL_ROSTER_STRIDE; i++) MEM8(RB_ROSTER_VA + i) = R.roster[i];
        MEM32(RB_ROSTER_N) = R.nroster;
        MEM32(0x001DEC9Cu) = R.seed9c;
        fprintf(stderr, "[RACEBENCH] LoadLevel: roster forced (%u entries, %u before), [0x1DEC9C]=0x%08X, "
                "live track=%u reference=%u\n", R.nroster, cur, R.seed9c, MEM32(0x001DEC90u), R.track);
        S.roster_done = 1;
    }
    g_ecx = ecx;
    sub_000AC9B0();
}

/* ── 0x2E040: Race_ResetPlayerRoster (thiscall, ret 8) ── */
static void hook_2E040(void)
{
    uint32_t ecx = g_ecx;
    if (!S.done && S.races <= 1) {
        uint32_t before = MEM32(0x001DEC98u);
        MEM32(0x001DEC98u) = R.seed98;
        fprintf(stderr, "[RACEBENCH] Race_ResetPlayerRoster: [0x1DEC98] 0x%08X -> 0x%08X\n", before, R.seed98);
    }
    g_ecx = ecx;
    sub_0002E040();
}

void (*np_rb_lookup(uint32_t xbox_va))(void)
{
    if (xbox_va == 0x000AC9B0u) return hook_AC9B0;
    if (xbox_va == 0x0002E040u) return hook_2E040;
    return 0;
}

/* ── loading ── */
static int rb_load_npcl(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint32_t h[12], hsize, rsize, i, n;
    npcl_record rec;
    int rng_seen[2] = { 0, 0 };
    if (!f) { fprintf(stderr, "[RACEBENCH] cannot open %s\n", path); return 0; }
    if (fread(h, 4, 12, f) != 12 || h[0] != NPCL_MAGIC || h[3] != sizeof(npcl_record) || h[1] < 3u) {
        fprintf(stderr, "[RACEBENCH] %s: not a .npcl v3\n", path); fclose(f); return 0;
    }
    hsize = h[2]; rsize = h[3];
    R.track = h[4]; R.seed98 = h[6]; R.seed9c = h[7];
    n = h[9];
    if (n > RB_MAXR) { fprintf(stderr, "[RACEBENCH] %u riders: too many\n", n); fclose(f); return 0; }
    R.nriders = n;
    fseek(f, 48 + 4 * (long)h[9], SEEK_SET);
    if (fread(&R.nroster, 4, 1, f) != 1 || R.nroster > NPCL_MAX_RIDERS
        || fread(R.roster, NPCL_ROSTER_STRIDE, R.nroster, f) != R.nroster) { fclose(f); return 0; }
    for (i = 0; i < RB_NSTATES; i++) R.r_start[i] = RB_UNSET;
    R.player_idx = RB_UNSET;
    fseek(f, (long)hsize, SEEK_SET);
    while (fread(&rec, rsize, 1, f) == 1) {
        if (rec.idx >= n) continue;
        if (rec.race_state < RB_NSTATES && R.r_start[rec.race_state] == RB_UNSET)
            R.r_start[rec.race_state] = rec.race_frame;
        if (rec.race_frame > R.fmax) R.fmax = rec.race_frame;
        if (rec.race_state == 4u && rec.race_frame > R.last4) R.last4 = rec.race_frame;
        if (rec.kind == NPCL_KIND_PLAYER && R.player_idx == RB_UNSET) R.player_idx = rec.idx;
    }
    if (R.fmax == 0 || R.fmax > 2000000u || R.r_start[4] == RB_UNSET) {
        fprintf(stderr, "[RACEBENCH] %s: no race (state 4) recorded\n", path); fclose(f); return 0;
    }
    R.st_at = calloc(R.fmax + 1u, 1);
    if (!R.st_at) { fclose(f); return 0; }
    for (i = 0; i < n; i++) {
        R.cmd[i] = calloc(R.fmax + 1u, 4);
        R.sf[i] = calloc(R.fmax + 1u, 4);
        R.valid[i] = calloc(R.fmax + 1u, 1);
        if (!R.cmd[i] || !R.sf[i] || !R.valid[i]) { fclose(f); return 0; }
    }
    fseek(f, (long)hsize, SEEK_SET);
    while (fread(&rec, rsize, 1, f) == 1) {
        if ((rec.kind == NPCL_KIND_RNG_A || rec.kind == NPCL_KIND_RNG_B) && rec.cmd == 3u) {
            int k = rec.kind - NPCL_KIND_RNG_A;
            uint32_t *dst = k ? R.rng_b : R.rng_a;
            if (!rng_seen[k]) { memcpy(&dst[0], rec.pos, 12); memcpy(&dst[3], rec.vel, 12); rng_seen[k] = 1; }
            continue;
        }
        if (rec.idx >= n || (rec.kind != NPCL_KIND_PLAYER && rec.kind != NPCL_KIND_AI)) continue;
        R.cmd[rec.idx][rec.race_frame] = rec.cmd;
        R.sf[rec.idx][rec.race_frame] = rec.speed_factor;
        R.valid[rec.idx][rec.race_frame] = 1;
        R.st_at[rec.race_frame] = rec.race_state;
    }
    R.have_rng = rng_seen[0] && rng_seen[1];
    fclose(f);
    fprintf(stderr, "[RACEBENCH] %s: track=%u, %u riders, Player = index %u, race+0x18 up to %u "
            "(race state 4 from %u to %u), state 3 RNG: %s\n", path, R.track, n, R.player_idx, R.fmax,
            R.r_start[4], R.last4, R.have_rng ? "yes" : "no");
    return 1;
}

static void rb_load_nprs(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint32_t hdr[4], i;
    nprs_record r;
    unsigned long long cnt = 0, nv = 0;
    if (!f) { fprintf(stderr, "[RACEBENCH] %s not found: commands only, no correction\n", path); return; }
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != NPRS_MAGIC || hdr[2] != sizeof(nprs_record)
        || hdr[3] != NPRS_SET) {
        fprintf(stderr, "[RACEBENCH] %s is not a .nprs v1: no correction\n", path); fclose(f); return;
    }
    for (i = 0; i < R.nriders; i++) {
        R.state[i] = calloc((size_t)R.fmax + 1u, NPRS_SET);
        R.have[i] = calloc(R.fmax + 1u, 1);
        if (!R.state[i] || !R.have[i]) { fclose(f); return; }
    }
    R.view = calloc((size_t)R.fmax + 1u, NPRS_SET);
    R.have_view = calloc(R.fmax + 1u, 1);
    if (!R.view || !R.have_view) { fclose(f); return; }
    while (fread(&r, sizeof r, 1, f) == 1) {
        if (r.race_frame > R.fmax) continue;
        if (r.idx == NPRS_IDX_VIEW) {
            memcpy(R.view + (size_t)r.race_frame * NPRS_SET, r.data, NPRS_SET);
            R.have_view[r.race_frame] = 1;
            nv++;
        } else if (r.idx < R.nriders) {
            memcpy(R.state[r.idx] + (size_t)r.race_frame * NPRS_SET, r.data, NPRS_SET);
            R.have[r.idx][r.race_frame] = 1;
            cnt++;
        }
    }
    fclose(f);
    fprintf(stderr, "[RACEBENCH] %s: %llu rider states, %llu camera blocks\n", path, cnt, nv);
}

/* ── Stuck watchdog (XBOX_STUCK_BACK=<seconds>), independent of the replay:
 * when the Player moves slower than 2 m/s (79 units/s) for that long in the
 * race (state 4), BACK is pressed (respawn on the track). ── */
int g_np_stuck_on = 0;
static struct { uint32_t secs_ticks, slow, last_rf, presses; float last[3], min_ups; int have; } K;

void np_stuck_player(uint32_t rider)
{
    uint32_t race = rb_race_ptr(), rf, i;
    float d2 = 0.0f, p[3];
    if (!race || MEM32(race + 0x1Cu) != 4u) { K.have = 0; K.slow = 0; return; }
    rf = MEM32(race + 0x18u);
    for (i = 0; i < 3; i++) p[i] = MEMF(rider + 0x170u + 4u * i);
    if (K.have && rf == K.last_rf + 1u) {
        for (i = 0; i < 3; i++) d2 += (p[i] - K.last[i]) * (p[i] - K.last[i]);
        if (sqrtf(d2) * 60.0f < K.min_ups) K.slow++; else K.slow = 0;
        if (K.slow >= K.secs_ticks) {
            K.slow = 0;
            K.presses++;
            xinput_hle_press("BACK", 300);
            fprintf(stderr, "[STUCK] Player slow for %.1f s at race+0x18=%u: BACK (%u)\n",
                    K.secs_ticks / 60.0f, rf, K.presses);
            fflush(stderr);
        }
    }
    memcpy(K.last, p, sizeof p);
    K.last_rf = rf;
    K.have = 1;
}

void np_stuck_init(void)
{
    const char *v = getenv("XBOX_STUCK_BACK");
    double s = (v && *v) ? atof(v) : 0.0;
    if (s <= 0.0) return;
    K.secs_ticks = (uint32_t)(s * 60.0);
    v = getenv("XBOX_STUCK_SPEED");               /* m/s, default 2 (test hook) */
    K.min_ups = (float)(((v && *v) ? atof(v) : 2.0) * 39.4);
    g_np_stuck_on = 1;
    fprintf(stderr, "[STUCK] watchdog: BACK after %.1f s below %.1f m/s in the race\n", s, K.min_ups / 39.4);
}

/* Port-0 pad muted during the countdown and the race (states 3 and 4):
 * the game reads the pad outside the command hook (start push 0x2D65B,
 * possibly more), and timed presses land on different ticks every run. */
int np_rb_pad_muted(void)
{
    uint32_t race, st;
    if (!s_padmute || S.done) return 0;
    race = rb_race_ptr();
    if (!race) return 0;
    st = MEM32(race + 0x1Cu);
    return st == 3u || st == 4u;
}

void np_rb_init(void)
{
    const char *e = getenv("XBOX_RACEBENCH"), *v;
    char p[MAX_PATH];
    size_t L;
    v = getenv("XBOX_RACE_PADMUTE");
    if (v && v[0] == '1') {
        s_padmute = 1;
        fprintf(stderr, "[RACEBENCH] port-0 pad muted in race states 3 and 4\n");
    }
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "off")) return;
    if (!rb_load_npcl(e)) { fprintf(stderr, "[RACEBENCH] off\n"); return; }
    L = strlen(e);
    if (L > 5 && L < sizeof p) {
        snprintf(p, sizeof p, "%.*s.nprs", (int)(L - 5), e);
        rb_load_nprs(p);
    }
    v = getenv("XBOX_RACEBENCH_HZ");
    P.period = (v && *v) ? (atoi(v) > 0 ? 60u / (uint32_t)atoi(v) : 0u) : 3u;
    v = getenv("XBOX_RACEBENCH_THR");  P.thr = (v && *v) ? (float)atof(v) : 20.0f;
    v = getenv("XBOX_RACEBENCH_JUMP"); P.jump = (v && *v) ? (float)atof(v) : 39.4f;
    v = getenv("XBOX_RACEBENCH_CAM");  P.cam = v && v[0] == '1';
    v = getenv("XBOX_RACEBENCH_PAUSE"); P.pause_at = (v && *v) ? (uint32_t)strtoul(v, 0, 10) : 0u;
    v = getenv("XBOX_RACEBENCH_SHOT_TICKS");
    while (v && *v && P.nshot < 32u) {
        char *end;
        uint32_t lo = (uint32_t)strtoul(v, &end, 10), hi = lo;
        if (end == v) break;
        if (*end == '-') hi = (uint32_t)strtoul(end + 1, &end, 10);
        P.shot_lo[P.nshot] = lo; P.shot_hi[P.nshot] = hi; P.nshot++;
        v = (*end == ',') ? end + 1 : end;
        if (*v == 0 || end == v) break;
    }
    {
        const char *sp = getenv("XBOX_RACEBENCH_SHOTS");
        if (sp && *sp) MultiByteToWideChar(CP_ACP, 0, sp, -1, P.shot_pre, MAX_PATH);
        else P.nshot = 0;
    }
    v = getenv("XBOX_RACEBENCH_LOG");
    if (v && *v) snprintf(p, sizeof p, "%s", v);
    else snprintf(p, sizeof p, "%s.%lu.rbt.csv", e, (unsigned long)GetCurrentProcessId());
    S.log = fopen(p, "w");
    if (S.log) {
        uint32_t i;
        setvbuf(S.log, NULL, _IOFBF, 256 * 1024);
        fprintf(S.log, "ref,rf,st,present,t_us");
        for (i = 0; i < R.nriders; i++) fprintf(S.log, ",e%u", i);
        fprintf(S.log, ",corr,cam_eye,cam_mat\n");
    }
    QueryPerformanceFrequency(&S.qf);
    s_padmute = 1;
    fprintf(stderr, "[RACEBENCH] active: corrections every %u ticks, threshold %.0f, camera %s, log %s\n",
            P.period, P.thr, P.cam ? "copied" : "measured only", S.log ? p : "(none)");
    fflush(stderr);
    g_np_rb_on = 1;
}
