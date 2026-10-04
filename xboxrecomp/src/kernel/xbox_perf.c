/*
 * xbox_perf.c -- profil par zones d'une image. Voir xbox_perf.h.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbox_perf.h"

int g_perf_on;

#define KORD   512
#define WIN    4096                     /* images gardées par fenêtre de 2 s */
#define OUT    PZ_N                     /* « hors zone » pour le temps noyau */

extern void pgraph_d3d11_perf_counts(unsigned *draws, unsigned *methods, unsigned *verts);

static double s_freq;
static FILE  *s_csv;
static CRITICAL_SECTION s_lock;
static DWORD  s_game_tid;
static __thread int t_zone = OUT;

/* thread du jeu : image en cours */
static double g_z[PZ_N + 1], g_k[PZ_N + 1], g_last_end;
static double g_kord[KORD];             /* ms par ordinal noyau, fenêtre courante */
static unsigned g_kcnt[KORD];

/* pump : image en cours */
static double p_z[PZ_N], p_last;
static unsigned p_c[PC_N], p_base[3];

/* fenêtres (2 s) */
typedef struct { float frame, tick, render, render_k, fwait, fwait_k, other; unsigned char nticks; } GRec;
typedef struct { float itv, pb, present, dxgi, gpu, dup, dcb, dstate, ddraw, tex, psh, vsdec, pcpu, pgpu; unsigned draws, methods, verts, c[PC_N]; } PRec;
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
    if (!s_game_tid && zone == PZ_RENDER) s_game_tid = GetCurrentThreadId();   /* thread du rendu de course */
    t_zone = zone;
    return prev;
}

void perf_zone_leave(int zone, int prev, double ms)
{
    t_zone = prev;
    if (GetCurrentThreadId() != s_game_tid) return;
    g_z[zone] += ms;
    /* Le temps d'une zone imbriquée (rendu dans l'attente : rendus en plus
     * du cap) est retiré de la zone englobante. */
    if (prev != OUT) g_z[prev] -= ms;
}

void perf_kernel(unsigned ordinal, double ms)
{
    if (GetCurrentThreadId() != s_game_tid) return;
    g_k[t_zone] += ms;                  /* zone la plus interne : compté une fois */
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

/* Résumé de la fenêtre : appelé sous verrou depuis le pump. */
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
        "[PERF] %s%.1f s : jeu %d img (%.1f/s), ticks/img %.2f ; ms/img : tick %.2f rendu %.2f (dont noyau %.2f ; p50 %.2f p95 %.2f max %.2f) "
        "attente %.2f (noyau %.2f)\n",
        final ? "BILAN " : "", el / 1000.0, s_ng, s_ng * 1000.0 / (el > 1 ? el : 1), AVG(sg[6], s_ng),
        AVG(sg[1], s_ng), AVG(sg[2], s_ng), AVG(sg[3], s_ng), a50, a95, amx, AVG(sg[4], s_ng), AVG(sg[5], s_ng));
    fprintf(stderr,
        "[PERF]   pump %d presents : intervalle p50 %.2f p95 %.2f max %.2f ; ms/img : pushbuffer %.2f (p95 %.2f max %.2f, dont host_present %.2f, dont Present DXGI %.2f) GPU %.2f ; "
        "par img : %.0f draws, %.0f méthodes, %.0f sommets, %.1f décodages VS, %.2f compilations, %.2f envois tex, %.0f signatures tex, %.0f appels noyau (jeu)\n",
        s_np, b50, b95, bmx, AVG(sp[0], s_np), c95, cmx, AVG(sp[1], s_np), AVG(sp[2], s_np), AVG(sp[3], s_np),
        AVG(cnt[0], s_np), AVG(cnt[1], s_np), AVG(cnt[2], s_np), AVG(cnt[3 + PC_DECODE], s_np), AVG(cnt[3 + PC_COMPILE], s_np),
        AVG(cnt[3 + PC_TEXUP], s_np), AVG(cnt[3 + PC_TEXSIG], s_np), AVG(cnt[3 + PC_KCALL], s_np));
    {
        double d4[4] = {0};
        for (i = 0; i < s_np; i++) { d4[0] += s_p[i].dup; d4[1] += s_p[i].dcb; d4[2] += s_p[i].dstate; d4[3] += s_p[i].ddraw; }
        fprintf(stderr, "[PERF]   draws, ms/img : envoi sommets+indices %.2f, constantes %.2f, états+IA/VS/PS %.2f, DrawIndexed %.2f\n",
                AVG(d4[0], s_np), AVG(d4[1], s_np), AVG(d4[2], s_np), AVG(d4[3], s_np));
        {
            double q[5] = {0}, cv = 0, cd = 0, ds = 0, dk = 0;
            for (i = 0; i < s_np; i++) {
                q[0] += s_p[i].tex; q[1] += s_p[i].psh; q[2] += s_p[i].vsdec; q[3] += s_p[i].pcpu; q[4] += s_p[i].pgpu;
                cv += s_p[i].c[PC_CPUVTX]; cd += s_p[i].c[PC_CPUDRAW];
                ds += s_p[i].c[PC_D3DSET]; dk += s_p[i].c[PC_D3DSKIP];
            }
            fprintf(stderr, "[PERF]   traduction, ms/img : textures %.2f, clé PS %.2f, décodage VS %.2f, draws GPU %.2f, draws CPU %.2f (%.0f draws, %.0f sommets)\n",
                    AVG(q[0], s_np), AVG(q[1], s_np), AVG(q[2], s_np), AVG(q[4], s_np), AVG(q[3], s_np), AVG(cd, s_np), AVG(cv, s_np));
            fprintf(stderr, "[PERF]   appels d'état D3D11 par img : %.0f émis, %.0f sautés (miroir)\n", AVG(ds, s_np), AVG(dk, s_np));
        }
    }
    if (nslow)
        fprintf(stderr, "[PERF]   images lentes (> 17,5 ms) : %d ; moyenne intervalle %.2f pushbuffer %.2f draws %.0f GPU %.2f host_present %.2f\n",
                nslow, slow[0] / nslow, slow[1] / nslow, slow[2] / nslow, slow[3] / nslow, slow[4] / nslow);
    {   /* appels noyau du thread du jeu les plus coûteux */
        int top[4] = {-1, -1, -1, -1}, k, j;
        for (k = 0; k < KORD; k++) {
            for (j = 0; j < 4; j++)
                if (top[j] < 0 || g_kord[k] > g_kord[top[j]]) {
                    memmove(&top[j + 1], &top[j], (size_t)(3 - j) * sizeof top[0]);
                    top[j] = k;
                    break;
                }
        }
        fprintf(stderr, "[PERF]   noyau (jeu, ms/img) :");
        for (j = 0; j < 4; j++)
            if (top[j] >= 0 && g_kord[top[j]] > 0)
                fprintf(stderr, " ord %d %.2f (%u appels)", top[j], AVG(g_kord[top[j]], s_ng), g_kcnt[top[j]]);
        fprintf(stderr, "\n");
    }
    memset(g_kord, 0, sizeof g_kord);
    memset(g_kcnt, 0, sizeof g_kcnt);
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
    if (s_csv)                          /* ligne complémentaire, même image */
        fprintf(s_csv, "Q,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u\n", now, r.tex, r.psh, r.vsdec, r.pcpu, r.pgpu,
                r.c[PC_CPUVTX], r.c[PC_CPUDRAW], r.c[PC_D3DSET], r.c[PC_D3DSKIP]);
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
                           "# P,t_ms,interval_ms,pushbuffer_ms,host_present_ms,dxgi_present_ms,gpu_ms_prev,draws,methods,verts,vs_decodes,compiles,tex_uploads,tex_sigs,kernel_calls_game,draw_upload_ms,draw_consts_ms,draw_state_ms,draw_call_ms\n");
    }
    s_win_t0 = perf_now();
    g_perf_on = 1;
    atexit(perf_atexit);
    fprintf(stderr, "[PERF] profil par zones actif%s%s\n", s_csv ? " ; CSV " : "", s_csv ? e : "");
}
