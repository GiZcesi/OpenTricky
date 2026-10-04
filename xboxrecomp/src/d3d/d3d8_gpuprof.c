/*
 * d3d8_gpuprof.c -- temps GPU par passe d'une image.
 * Voir d3d8_gpuprof.h.
 */
#include "d3d8_internal.h"
#include "d3d8_gpuprof.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_gpuprof_on;

#define NF    8                 /* images en vol avant relecture */
#define MAXQ  8192              /* horodatages par image (2 par travail encadré) */
#define WIN   4096              /* images gardées par fenêtre pour les centiles */

static const char *const k_cat_name[GP_N] = {
    "g0", "g1", "g2", "g3", "g4", "g5", "g6", "persp>=7", "hud", "other",
    "clear", "post_split", "post_present", "prevframe", "resolve", "blit", "soft_shadow"
};

typedef struct {
    ID3D11Query *dj;
    ID3D11Query *ts[MAXQ];
    unsigned char cat[MAXQ / 2];
    int n, ncreated, open, pending;
    unsigned flushes, overflow, note;
} Slot;

static Slot *s_slot;
static int   s_cur;
static int   s_in;                      /* une paire est ouverte */
static int   s_mode;                    /* 1 = par suite de draws de même catégorie, 2 = par draw */
static int   s_run, s_run_cat;          /* la paire ouverte est une suite de draws */
static int   s_phase, s_group;
static unsigned s_flushes, s_note;     /* image en cours de construction */
static FILE *s_csv;
static LARGE_INTEGER s_qf, s_t0;

static struct {
    unsigned frames, lost, notready, overflow, flushes;
    double cat_ms[GP_N], busy[WIN], span[WIN];
    unsigned long long cat_n[GP_N];
} W;
static struct {
    unsigned long long frames;
    double cat_ms[GP_N], busy, span;
} TOT;

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double pct(double *v, unsigned n, double p)
{
    unsigned i;
    if (!n) return 0.0;
    qsort(v, n, sizeof *v, cmpd);
    i = (unsigned)(p * (n - 1) + 0.5);
    return v[i];
}

static void window_print(double secs)
{
    unsigned j;
    char per[512];
    int len = 0;
    double busy_mean = 0.0, span_mean = 0.0, b50, b95, s50;
    if (!W.frames) {
        fprintf(stderr, "[GPU] %.1f s : 0 image relue (perdues %u, pas prêtes %u)\n", secs, W.lost, W.notready);
        return;
    }
    for (j = 0; j < W.frames && j < WIN; j++) { busy_mean += W.busy[j]; span_mean += W.span[j]; }
    busy_mean /= (W.frames < WIN ? W.frames : WIN);
    span_mean /= (W.frames < WIN ? W.frames : WIN);
    b50 = pct(W.busy, W.frames < WIN ? W.frames : WIN, 0.50);
    b95 = pct(W.busy, W.frames < WIN ? W.frames : WIN, 0.95);
    s50 = pct(W.span, W.frames < WIN ? W.frames : WIN, 0.50);
    per[0] = 0;
    for (j = 0; j < GP_N; j++)
        if (W.cat_n[j] && len < (int)sizeof per - 48)
            len += snprintf(per + len, sizeof per - (size_t)len, " %s %.3f (%.0f)",
                            k_cat_name[j], W.cat_ms[j] / W.frames, (double)W.cat_n[j] / W.frames);
    fprintf(stderr, "[GPU] %.1f s : %u img ; ms GPU/img : occupé moy %.3f p50 %.3f p95 %.3f ; "
            "span p50 %.3f moy %.3f ; vidages %.1f/img ; perdues %u, pas prêtes %u, débord %u\n",
            secs, W.frames, busy_mean, b50, b95, s50, span_mean,
            (double)W.flushes / W.frames, W.lost, W.notready, W.overflow);
    fprintf(stderr, "[GPU]   par catégorie, ms/img (encadrés/img) :%s\n", per);
    memset(&W, 0, sizeof W);
}

static void at_exit(void)
{
    unsigned j;
    if (!TOT.frames) return;
    fprintf(stderr, "[GPU] bilan : %llu img ; ms GPU/img : occupé %.3f span %.3f ;",
            TOT.frames, TOT.busy / TOT.frames, TOT.span / TOT.frames);
    for (j = 0; j < GP_N; j++)
        if (TOT.cat_ms[j] > 0.0) fprintf(stderr, " %s %.3f", k_cat_name[j], TOT.cat_ms[j] / TOT.frames);
    fprintf(stderr, "\n");
    if (s_csv) fflush(s_csv);
}

void gpuprof_init(void)
{
    const char *e = getenv("XBOX_GPUPROF");
    const char *c = getenv("XBOX_GPUPROF_CSV");
    if (!e || (e[0] != '1' && e[0] != '2')) return;
    s_mode = e[0] - '0';
    s_slot = (Slot *)calloc(NF, sizeof *s_slot);
    if (!s_slot) return;
    QueryPerformanceFrequency(&s_qf);
    QueryPerformanceCounter(&s_t0);
    if (c && c[0]) {
        s_csv = fopen(c, "w");
        if (s_csv) {
            int j;
            fprintf(s_csv, "# t_ms,busy_ms,span_ms,brackets,flushes,note");
            for (j = 0; j < GP_N; j++) fprintf(s_csv, ",%s", k_cat_name[j]);
            fprintf(s_csv, "\n");
        }
    }
    g_gpuprof_on = 1;
    atexit(at_exit);
    fprintf(stderr, "[GPU] profil GPU par passe actif%s%s\n", s_csv ? " ; CSV " : "", s_csv ? c : "");
}

void gpuprof_set_phase(int phase, int group)
{
    s_phase = phase;
    if (group >= 0) s_group = group;
}

int gpuprof_draw_cat(void)
{
    switch (s_phase) {      /* PGRAPH_PHASE_* (nv2a_pgraph_d3d11.h) */
    case 2: return GP_G0 + (s_group < 0 ? 0 : s_group > 6 ? 6 : s_group);
    case 3: return GP_PERSP7;
    case 4: return GP_HUD;
    default: return GP_OTHER;
    }
}

/* 1 = relue, 0 = pas encore prête. */
static int slot_collect(Slot *sl)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d;
    static UINT64 t[MAXQ];
    double cat[GP_N], busy = 0.0, span = 0.0, f;
    int k;
    if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)sl->dj, &d, sizeof d,
                                    D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return 0;
    for (k = 0; k < sl->n; k++)
        if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)sl->ts[k], &t[k], sizeof t[k],
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return 0;
    sl->pending = 0;
    if (d.Disjoint || !d.Frequency || sl->n < 2) return 1;
    f = 1000.0 / (double)d.Frequency;
    memset(cat, 0, sizeof cat);
    for (k = 0; k + 1 < sl->n; k += 2) {
        double ms = t[k + 1] >= t[k] ? (double)(t[k + 1] - t[k]) * f : 0.0;
        cat[sl->cat[k / 2]] += ms;
        busy += ms;
        W.cat_n[sl->cat[k / 2]]++;
    }
    if (t[sl->n - 1] >= t[0]) span = (double)(t[sl->n - 1] - t[0]) * f;
    for (k = 0; k < GP_N; k++) { W.cat_ms[k] += cat[k]; TOT.cat_ms[k] += cat[k]; }
    if (W.frames < WIN) { W.busy[W.frames] = busy; W.span[W.frames] = span; }
    W.frames++;
    W.flushes += sl->flushes;
    W.overflow += sl->overflow;
    TOT.frames++; TOT.busy += busy; TOT.span += span;
    if (s_csv) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        fprintf(s_csv, "%.3f,%.4f,%.4f,%d,%u,%u", (double)(now.QuadPart - s_t0.QuadPart) * 1000.0 / (double)s_qf.QuadPart,
                busy, span, sl->n / 2, sl->flushes, sl->note);
        for (k = 0; k < GP_N; k++) fprintf(s_csv, ",%.4f", cat[k]);
        fprintf(s_csv, "\n");
    }
    return 1;
}

static Slot *slot_open(void)
{
    Slot *sl = &s_slot[s_cur];
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    if (sl->open) return sl;
    if (!dev || !ctx) return NULL;
    if (sl->pending && !slot_collect(sl)) { W.lost++; sl->pending = 0; }
    if (!sl->dj) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &sl->dj))) { g_gpuprof_on = 0; return NULL; }
    }
    sl->n = 0; sl->flushes = 0; sl->overflow = 0;
    sl->open = 1;
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)sl->dj);
    return sl;
}

static int ts_emit(Slot *sl)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    if (sl->n >= sl->ncreated) {
        D3D11_QUERY_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &sl->ts[sl->n]))) return 0;
        sl->ncreated++;
    }
    ID3D11DeviceContext_End(d3d8_GetD3D11Context(), (ID3D11Asynchronous *)sl->ts[sl->n]);
    sl->n++;
    return 1;
}

void gpuprof_begin(int cat)
{
    Slot *sl;
    if (!g_gpuprof_on) return;
    if (s_in) gpuprof_end();             /* ferme une suite de draws ouverte */
    if (!(sl = slot_open())) return;
    if (sl->n + 2 > MAXQ) { sl->overflow++; return; }
    sl->cat[sl->n / 2] = (unsigned char)cat;
    if (ts_emit(sl)) s_in = 1;
}

void gpuprof_end(void)
{
    if (!g_gpuprof_on || !s_in) return;
    s_in = 0;
    s_run = 0;
    if (!ts_emit(&s_slot[s_cur])) s_slot[s_cur].n--;   /* paire incomplète : retirée */
}

void gpuprof_count_flush(void)
{
    if (g_gpuprof_on) s_flushes++;
}

void gpuprof_frame(void)
{
    static LARGE_INTEGER last;
    LARGE_INTEGER now;
    Slot *sl;
    int i;
    if (!g_gpuprof_on) return;
    sl = &s_slot[s_cur];
    if (s_in) gpuprof_end();
    if (sl->open) {
        ID3D11DeviceContext_End(d3d8_GetD3D11Context(), (ID3D11Asynchronous *)sl->dj);
        sl->flushes = s_flushes; sl->note = s_note;
        sl->open = 0;
        sl->pending = 1;
        s_cur = (s_cur + 1) % NF;
    }
    s_flushes = 0; s_note = 0;
    /* Relire dans l'ordre les images prêtes, la plus ancienne d'abord. */
    for (i = 0; i < NF; i++) {
        Slot *o = &s_slot[(s_cur + i) % NF];
        if (o->pending && !slot_collect(o)) break;
    }
    QueryPerformanceCounter(&now);
    if (!last.QuadPart) last = now;
    if ((double)(now.QuadPart - last.QuadPart) >= 2.0 * (double)s_qf.QuadPart) {
        window_print((double)(now.QuadPart - s_t0.QuadPart) / (double)s_qf.QuadPart);
        last = now;
    }
}

void gpuprof_note(unsigned bits)
{
    if (g_gpuprof_on) s_note |= bits;
}

/* Draws du jeu. Mode 1 : une seule paire par suite de draws de même
 * catégorie (fermée par un autre travail encadré, un changement de
 * catégorie ou la fin de l'image) -- deux horodatages par draw sérialisent le
 * GPU et gonflent les totaux (2,3 -> 8,3 ms en 640 sur un iGPU). Une
 * suite peut contenir des trous où le GPU attend le CPU : majorant par suite.
 * Mode 2 : une paire par draw (répartition fine, totaux gonflés). */
void gpuprof_draw_begin(void)
{
    int c;
    if (!g_gpuprof_on) return;
    c = gpuprof_draw_cat();
    if (s_mode == 1 && s_in && s_run && s_run_cat == c) return;
    gpuprof_begin(c);
    if (s_in) { s_run = 1; s_run_cat = c; }
}

void gpuprof_draw_end(void)
{
    if (g_gpuprof_on && s_mode == 2) gpuprof_end();
}
