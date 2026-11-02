/*
 * xbox_perf.c -- per-zone profile of a frame. See xbox_perf.h.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include "xbox_perf.h"

int g_perf_on;
int g_perf_detail;
int g_perf_maptime;
unsigned long long g_perf_map_cyc, g_perf_map_pairs;
static unsigned s_ghist[17];            /* GPU draws by upload groups (16 = 16 or more), current window */
static unsigned long long s_tsc0, s_map_cyc0, s_map_pairs0;
static double s_qpc0, s_rdtsc_ovh;      /* cycles of one rdtsc pair, subtracted twice per Map/Unmap pair */

#define KORD   512
#define WIN    4096                     /* frames kept per 2 s window */
#define OUT    PZ_N                     /* "outside any zone" for the kernel time */

extern void pgraph_d3d11_perf_counts(unsigned *draws, unsigned *methods, unsigned *verts);
extern void nv2a_live_pb_hist_dump(void);
extern void nv2a_live_pb_hist_write(const char *path);

static double s_freq;
static FILE  *s_csv;
static char   s_methods_path[MAX_PATH];  /* XBOX_PERF_METHODS: histogram file, rewritten every window */
static CRITICAL_SECTION s_lock;
static DWORD  s_game_tid;
static __thread int t_zone = OUT;

/* game thread: current frame */
static double g_z[PZ_N + 1], g_k[PZ_N + 1], g_last_end;
static double g_kord[KORD];             /* ms per kernel ordinal, current window */
static unsigned g_kcnt[KORD];

/* pump: frame in progress */
static double p_z[PZ_N], p_last;
static unsigned p_c[PC_N], p_base[3];

/* windows (2 s) */
typedef struct { float frame, tick, render, render_k, fwait, fwait_k, other; unsigned char nticks; } GRec;
typedef struct { float itv, pb, present, dxgi, gpu, dup, dcb, dstate, ddraw, tex, psh, vsdec, pcpu, pgpu, z6[6]; unsigned draws, methods, verts, c[PC_N]; } PRec;
static GRec   s_g[WIN]; static int s_ng;
static PRec   s_p[WIN]; static int s_np;
static double s_win_t0;

double perf_now(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart * 1000.0 / s_freq;
}

int perf_zone_enter(int zone)
{
    int prev = t_zone;
    if (!s_game_tid && zone == PZ_RENDER) s_game_tid = GetCurrentThreadId();   /* race render thread */
    t_zone = zone;
    return prev;
}

void perf_zone_leave(int zone, int prev, double ms)
{
    t_zone = prev;
    if (GetCurrentThreadId() != s_game_tid) return;
    g_z[zone] += ms;
    /* The time of a nested zone (render inside the wait: the cap's extra
     * renders) is taken out of the enclosing zone. */
    if (prev != OUT) g_z[prev] -= ms;
}

void perf_kernel(unsigned ordinal, double ms)
{
    if (GetCurrentThreadId() != s_game_tid) return;
    g_k[t_zone] += ms;                  /* innermost zone: counted once */
    perf_count(PC_KCALL, 1);
    if (ordinal < KORD) { g_kord[ordinal] += ms; g_kcnt[ordinal]++; }
}

static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void pct(float *v, int n, float *p50, float *p95, float *mx)
{
    if (n <= 0) { *p50 = *p95 = *mx = 0; return; }
    qsort(v, (size_t)n, sizeof *v, cmpf);
    *p50 = v[n / 2]; *p95 = v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1]; *mx = v[n - 1];
}

/* Window summary: called under the lock from the pump. */
static void report(int final)
{
    static float tmp[WIN];
    int i, nslow = 0;
    double sg[7] = {0}, sp[5] = {0}, slow[5] = {0}, cnt[3 + PC_N] = {0};
    float a50, a95, amx, b50, b95, bmx, c50, c95, cmx;
    double el = perf_now() - s_win_t0;
    if (s_ng == 0 && s_np == 0) return;
    for (i = 0; i < s_ng; i++) {
        sg[0] += s_g[i].frame; sg[1] += s_g[i].tick; sg[2] += s_g[i].render; sg[3] += s_g[i].render_k;
        sg[4] += s_g[i].fwait; sg[5] += s_g[i].fwait_k; sg[6] += s_g[i].nticks;
    }
    for (i = 0; i < s_np; i++) {
        int k;
        sp[0] += s_p[i].pb; sp[1] += s_p[i].present; sp[2] += s_p[i].dxgi; sp[3] += s_p[i].gpu; sp[4] += s_p[i].itv;
        cnt[0] += s_p[i].draws; cnt[1] += s_p[i].methods; cnt[2] += s_p[i].verts;
        for (k = 0; k < PC_N; k++) cnt[3 + k] += s_p[i].c[k];
        if (s_p[i].itv > 17.5f) {
            nslow++; slow[0] += s_p[i].itv; slow[1] += s_p[i].pb; slow[2] += s_p[i].draws; slow[3] += s_p[i].gpu; slow[4] += s_p[i].present;
        }
    }
    for (i = 0; i < s_ng; i++) tmp[i] = s_g[i].render;
    pct(tmp, s_ng, &a50, &a95, &amx);
    for (i = 0; i < s_np; i++) tmp[i] = s_p[i].itv;
    pct(tmp, s_np, &b50, &b95, &bmx);
    for (i = 0; i < s_np; i++) tmp[i] = s_p[i].pb;
    pct(tmp, s_np, &c50, &c95, &cmx);
#define AVG(x, n) ((n) ? (x) / (n) : 0.0)
    fprintf(stderr,
        "[PERF] %s%.1f s: game %d frames (%.1f/s), ticks/frame %.2f; ms/frame: tick %.2f render %.2f (kernel %.2f of it; p50 %.2f p95 %.2f max %.2f) "
        "wait %.2f (kernel %.2f)\n",
        final ? "SUMMARY " : "", el / 1000.0, s_ng, s_ng * 1000.0 / (el > 1 ? el : 1), AVG(sg[6], s_ng),
        AVG(sg[1], s_ng), AVG(sg[2], s_ng), AVG(sg[3], s_ng), a50, a95, amx, AVG(sg[4], s_ng), AVG(sg[5], s_ng));
    fprintf(stderr,
        "[PERF]   pump %d presents: interval p50 %.2f p95 %.2f max %.2f; ms/frame: pushbuffer %.2f (p95 %.2f max %.2f, host_present %.2f of it, DXGI Present %.2f of it) GPU %.2f; "
        "per frame: %.0f draws, %.0f methods, %.0f vertices, %.1f VS decodes, %.2f compiles, %.2f tex uploads, %.0f tex signatures, %.0f kernel calls (game)\n",
        s_np, b50, b95, bmx, AVG(sp[0], s_np), c95, cmx, AVG(sp[1], s_np), AVG(sp[2], s_np), AVG(sp[3], s_np),
        AVG(cnt[0], s_np), AVG(cnt[1], s_np), AVG(cnt[2], s_np), AVG(cnt[3 + PC_DECODE], s_np), AVG(cnt[3 + PC_COMPILE], s_np),
        AVG(cnt[3 + PC_TEXUP], s_np), AVG(cnt[3 + PC_TEXSIG], s_np), AVG(cnt[3 + PC_KCALL], s_np));
    {
        double d4[4] = {0};
        for (i = 0; i < s_np; i++) { d4[0] += s_p[i].dup; d4[1] += s_p[i].dcb; d4[2] += s_p[i].dstate; d4[3] += s_p[i].ddraw; }
        fprintf(stderr, "[PERF]   draws, ms/frame: vertex+index upload %.2f, constants %.2f, states+IA/VS/PS %.2f, DrawIndexed %.2f\n",
                AVG(d4[0], s_np), AVG(d4[1], s_np), AVG(d4[2], s_np), AVG(d4[3], s_np));
        {
            double q[5] = {0}, cv = 0, cd = 0, ds = 0, dk = 0;
            for (i = 0; i < s_np; i++) {
                q[0] += s_p[i].tex; q[1] += s_p[i].psh; q[2] += s_p[i].vsdec; q[3] += s_p[i].pcpu; q[4] += s_p[i].pgpu;
                cv += s_p[i].c[PC_CPUVTX]; cd += s_p[i].c[PC_CPUDRAW];
                ds += s_p[i].c[PC_D3DSET]; dk += s_p[i].c[PC_D3DSKIP];
            }
            fprintf(stderr, "[PERF]   translation, ms/frame: textures %.2f, PS key %.2f, VS decode %.2f, GPU draws %.2f, CPU draws %.2f (%.0f draws, %.0f vertices)\n",
                    AVG(q[0], s_np), AVG(q[1], s_np), AVG(q[2], s_np), AVG(q[4], s_np), AVG(q[3], s_np), AVG(cd, s_np), AVG(cv, s_np));
            fprintf(stderr, "[PERF]   D3D11 state calls per frame: %.0f issued, %.0f skipped (mirror)\n", AVG(ds, s_np), AVG(dk, s_np));
        }
        {   /* the rest of a GPU draw, split, and the pump's counters */
            double z[6] = {0}, c[PC_N] = {0};
            int k;
            for (i = 0; i < s_np; i++) {
                for (k = 0; k < 6; k++) z[k] += s_p[i].z6[k];
                for (k = PC_IDXW; k < PC_N; k++) c[k] += s_p[i].c[k];
            }
            fprintf(stderr, "[PERF]   draw detail, ms/frame: prepare %.2f, D3D8 states+textures %.2f, PS state %.2f, ZTIE/ZDECAL %.2f, hooks %.2f, before uploads %.2f\n",
                    AVG(z[0], s_np), AVG(z[1], s_np), AVG(z[2], s_np), AVG(z[3], s_np), AVG(z[4], s_np), AVG(z[5], s_np));
            fprintf(stderr, "[PERF]   pump per frame: %.0f index words; VS constants: %.0f same generation, %.0f equal, %.0f changed; "
                            "%.2f state objects created, %.0f sampler compares, %.0f texture binds; %.0f VS constants compared; "
                            "with XBOX_PERF_DETAIL: %.0f texture binds skipped; consecutive draws without state between: %.0f (%.0f plain), %.0f with the same bound state and constants; %.0f draws without a constant Map; "
                            "%.0f samplers from the memo, %.0f D3D8 state writes skipped\n",
                    AVG(c[PC_IDXW], s_np), AVG(c[PC_VSC_GEN], s_np), AVG(c[PC_VSC_EQ], s_np), AVG(c[PC_VSC_CHG], s_np),
                    AVG(c[PC_SOBJ], s_np), AVG(c[PC_SMPL], s_np), AVG(c[PC_SRV], s_np), AVG(c[PC_VSC_CMP], s_np),
                    AVG(c[PC_SRVSKIP], s_np), AVG(c[PC_MERGE], s_np), AVG(c[PC_MERGEP], s_np), AVG(c[PC_MERGES], s_np), AVG(c[PC_CBSKIP], s_np),
                    AVG(c[PC_SMEMO], s_np), AVG(c[PC_RSKIP], s_np));
        }
        {
            double gd = 0, gm = 0, gg = 0, hn = 0;
            int h;
            for (i = 0; i < s_np; i++) { gd += s_p[i].c[PC_GDRAW]; gm += s_p[i].c[PC_GMAP]; gg += s_p[i].c[PC_GGRP]; }
            for (h = 0; h < 17; h++) hn += s_ghist[h];
            fprintf(stderr, "[PERF]   GPU draws per frame %.0f: Maps %.0f (%.2f per draw), upload groups %.2f per draw; groups/draw %%:",
                    AVG(gd, s_np), AVG(gm, s_np), AVG(gm, gd), AVG(gg, gd));
            for (h = 0; h < 17; h++)
                if (s_ghist[h]) fprintf(stderr, " %d:%.1f", h, 100.0 * s_ghist[h] / hn);
            fprintf(stderr, "\n");
            if (g_perf_maptime) {
                double ms = perf_now() - s_qpc0;
                double cyc_per_us = ms > 0 ? (double)(__rdtsc() - s_tsc0) / (ms * 1000.0) : 0;
                unsigned long long np = g_perf_map_pairs - s_map_pairs0, nc = g_perf_map_cyc - s_map_cyc0;
                double net = (double)nc - 2.0 * s_rdtsc_ovh * (double)np;
                fprintf(stderr, "[PERF]   Map+Unmap: %.3f us per pair (%llu pairs; rdtsc overhead %.0f cycles removed, %.0f cycles/us)\n",
                        np && cyc_per_us > 0 ? net / (double)np / cyc_per_us : 0.0, np, s_rdtsc_ovh, cyc_per_us);
                s_map_cyc0 = g_perf_map_cyc; s_map_pairs0 = g_perf_map_pairs;
            }
            memset(s_ghist, 0, sizeof s_ghist);
        }
    }
    if (nslow)
        fprintf(stderr, "[PERF]   slow frames (> 17.5 ms): %d; mean interval %.2f pushbuffer %.2f draws %.0f GPU %.2f host_present %.2f\n",
                nslow, slow[0] / nslow, slow[1] / nslow, slow[2] / nslow, slow[3] / nslow, slow[4] / nslow);
    {   /* the game thread's most expensive kernel calls */
        int top[4] = {-1, -1, -1, -1}, k, j;
        for (k = 0; k < KORD; k++) {
            for (j = 0; j < 4; j++)
                if (top[j] < 0 || g_kord[k] > g_kord[top[j]]) {
                    memmove(&top[j + 1], &top[j], (size_t)(3 - j) * sizeof top[0]);
                    top[j] = k;
                    break;
                }
        }
        fprintf(stderr, "[PERF]   kernel (game, ms/frame):");
        for (j = 0; j < 4; j++)
            if (top[j] >= 0 && g_kord[top[j]] > 0)
                fprintf(stderr, " ord %d %.2f (%u calls)", top[j], AVG(g_kord[top[j]], s_ng), g_kcnt[top[j]]);
        fprintf(stderr, "\n");
    }
    memset(g_kord, 0, sizeof g_kord);
    memset(g_kcnt, 0, sizeof g_kcnt);
    if (final) nv2a_live_pb_hist_dump();   /* every method word of the run, by method */
    else if (s_methods_path[0]) nv2a_live_pb_hist_write(s_methods_path);   /* (pump thread) */
    s_ng = s_np = 0;
    s_win_t0 = perf_now();
    if (s_csv) fflush(s_csv);
}

void perf_game_frame_end(int nticks)
{
    double now = perf_now();
    GRec r;
    if (GetCurrentThreadId() != s_game_tid) return;
    r.frame = g_last_end ? (float)(now - g_last_end) : 0.0f;
    r.tick = (float)g_z[PZ_TICK]; r.render = (float)g_z[PZ_RENDER]; r.render_k = (float)g_k[PZ_RENDER];
    r.fwait = (float)g_z[PZ_FWAIT]; r.fwait_k = (float)g_k[PZ_FWAIT];
    r.other = r.frame - r.tick - r.render - r.fwait;
    r.nticks = (unsigned char)(nticks > 255 ? 255 : nticks);
    g_last_end = now;
    EnterCriticalSection(&s_lock);
    if (s_ng < WIN) s_g[s_ng++] = r;
    if (s_csv)
        fprintf(s_csv, "G,%.3f,%.3f,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", now, r.frame, r.nticks, r.tick,
                (double)g_k[PZ_TICK], r.render, r.render_k, r.fwait, r.fwait_k, r.other);
    LeaveCriticalSection(&s_lock);
    memset(g_z, 0, sizeof g_z);
    memset(g_k, 0, sizeof g_k);
}

void perf_add(int zone, double ms) { p_z[zone] += ms; }
void perf_count(int c, unsigned n) { p_c[c] += n; }

void perf_gpu_draw(unsigned maps, unsigned groups)
{
    p_c[PC_GDRAW]++;
    p_c[PC_GMAP] += maps;
    p_c[PC_GGRP] += groups;
    s_ghist[groups > 16 ? 16 : groups]++;
}

void perf_present_done(double gpu_prev)
{
    double now = perf_now();
    unsigned d, m, v;
    PRec r;
    int k;
    pgraph_d3d11_perf_counts(&d, &m, &v);
    r.itv = p_last ? (float)(now - p_last) : 0.0f;
    r.tex = (float)p_z[PZ_TEX]; r.psh = (float)p_z[PZ_PSH]; r.vsdec = (float)p_z[PZ_VSDEC];
    r.pcpu = (float)p_z[PZ_PROGCPU]; r.pgpu = (float)p_z[PZ_PROGGPU];
    r.dup = (float)p_z[PZ_DUP]; r.dcb = (float)p_z[PZ_DCB]; r.dstate = (float)p_z[PZ_DSTATE]; r.ddraw = (float)p_z[PZ_DDRAW];
    for (k = 0; k < 6; k++) r.z6[k] = (float)p_z[PZ_PREP + k];
    r.pb = (float)p_z[PZ_PB]; r.present = (float)p_z[PZ_PRESENT]; r.dxgi = (float)p_z[PZ_DXGI]; r.gpu = (float)gpu_prev;
    r.draws = d - p_base[0]; r.methods = m - p_base[1]; r.verts = v - p_base[2];
    for (k = 0; k < PC_N; k++) r.c[k] = p_c[k];
    p_base[0] = d; p_base[1] = m; p_base[2] = v;
    p_last = now;
    memset(p_z, 0, sizeof p_z);
    memset(p_c, 0, sizeof p_c);
    EnterCriticalSection(&s_lock);
    if (s_np < WIN) s_p[s_np++] = r;
    if (s_csv)
        fprintf(s_csv, "P,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f\n", now, r.itv, r.pb, r.present, r.dxgi, r.gpu,
                r.draws, r.methods, r.verts, r.c[PC_DECODE], r.c[PC_COMPILE], r.c[PC_TEXUP], r.c[PC_TEXSIG], r.c[PC_KCALL],
                r.dup, r.dcb, r.dstate, r.ddraw);
    if (s_csv)                          /* additional line, same frame */
        fprintf(s_csv, "Q,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u\n", now, r.tex, r.psh, r.vsdec, r.pcpu, r.pgpu,
                r.c[PC_CPUVTX], r.c[PC_CPUDRAW], r.c[PC_D3DSET], r.c[PC_D3DSKIP]);
    if (s_csv)                          /* GPU program draws, same frame */
        fprintf(s_csv, "M,%.3f,%u,%u,%u\n", now, r.c[PC_GDRAW], r.c[PC_GMAP], r.c[PC_GGRP]);
    if (s_csv)                          /* rest of the GPU draws, pump counters, same frame */
        fprintf(s_csv, "R,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", now,
                r.z6[0], r.z6[1], r.z6[2], r.z6[3], r.z6[4], r.z6[5],
                r.c[PC_IDXW], r.c[PC_VSC_GEN], r.c[PC_VSC_EQ], r.c[PC_VSC_CHG], r.c[PC_SOBJ], r.c[PC_SMPL],
                r.c[PC_SRV], r.c[PC_MERGE], r.c[PC_MERGEP], r.c[PC_CBSKIP], r.c[PC_SRVSKIP], r.c[PC_MERGES], r.c[PC_VSC_CMP],
                r.c[PC_SMEMO], r.c[PC_RSKIP]);
    if (now - s_win_t0 >= 2000.0) report(0);
    LeaveCriticalSection(&s_lock);
}

static void perf_atexit(void)
{
    EnterCriticalSection(&s_lock);
    report(1);
    if (s_csv) fclose(s_csv);
    s_csv = NULL;
    LeaveCriticalSection(&s_lock);
}

void perf_init(void)
{
    const char *e = getenv("XBOX_PERF");
    LARGE_INTEGER f;
    if (!e || e[0] != '1') return;
    QueryPerformanceFrequency(&f);
    s_freq = (double)f.QuadPart;
    InitializeCriticalSection(&s_lock);
    e = getenv("XBOX_PERF_CSV");
    if (e && *e) {
        s_csv = fopen(e, "w");
        if (s_csv)
            fprintf(s_csv, "# G,t_ms,frame_ms,nticks,tick_ms,tick_kernel_ms,render_ms,render_kernel_ms,wait_ms,wait_kernel_ms,other_ms\n"
                           "# P,t_ms,interval_ms,pushbuffer_ms,host_present_ms,dxgi_present_ms,gpu_ms_prev,draws,methods,verts,vs_decodes,compiles,tex_uploads,tex_sigs,kernel_calls_game,draw_upload_ms,draw_consts_ms,draw_state_ms,draw_call_ms\n"
                           "# M,t_ms,gpu_draws,gpu_draw_maps,gpu_draw_groups\n"
                           "# R,t_ms,prepare_ms,d3d8_states_textures_ms,ps_state_ms,ztie_zdecal_ms,hooks_ms,before_uploads_ms,"
                           "index_words,vsc_same_gen,vsc_equal,vsc_changed,state_objects_created,sampler_compares,texture_binds,"
                           "consecutive_draws,consecutive_plain,constant_map_skipped,texture_binds_skipped,same_bound_state,vsc_compared,"
                           "sampler_memo_hits,d3d8_state_writes_skipped\n");
    }
    e = getenv("XBOX_PERF_DETAIL");
    g_perf_detail = e && e[0] == '1';
    e = getenv("XBOX_PERF_METHODS");
    if (e && *e) snprintf(s_methods_path, sizeof s_methods_path, "%s", e);
    e = getenv("XBOX_PERF_MAPTIME");
    if (e && e[0] == '1') {
        unsigned long long best = ~0ull, a, b;
        int k;
        for (k = 0; k < 1000; k++) { a = __rdtsc(); b = __rdtsc(); if (b - a < best) best = b - a; }
        s_rdtsc_ovh = (double)best;
        g_perf_maptime = 1;
    }
    s_tsc0 = __rdtsc();
    s_qpc0 = perf_now();
    s_win_t0 = perf_now();
    g_perf_on = 1;
    atexit(perf_atexit);
    fprintf(stderr, "[PERF] per-zone profile active%s%s\n", s_csv ? "; CSV " : "", s_csv ? e : "");
}

/* The vertex/index ring of d3d8_nv2a.c, line V of the CSV: one per presented
 * frame, at the head of host_present (its header comes before the first one).
 * Counts only, no clock read. Under the lock, as the other lines: the CSV is
 * closed at exit. */
void perf_ring_frame(unsigned wraps, unsigned discards, unsigned fallbacks, unsigned refused, int shared)
{
    static int head;
    EnterCriticalSection(&s_lock);
    if (s_csv) {
        if (!head) {
            head = 1;
            fprintf(s_csv, "# V,t_ms,ring_wraps,ring_discards,ring_discards_no_completed_query,ring_draws_too_large,ring_lap_shared\n");
        }
        fprintf(s_csv, "V,%.3f,%u,%u,%u,%u,%d\n", perf_now(), wraps, discards, fallbacks, refused, shared);
    }
    LeaveCriticalSection(&s_lock);
}
