/*
 * fps_cap -- cadence d'images host au-delà de 60 (fork). Voir fps_cap.h.
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
static int s_in_extra;          /* rendu en plus en cours */
static int s_dup_k;             /* mode DUP : rang du rendu après le rendu normal */

#define APP_GLOBAL      0x001E3C7Cu
#define TICK_MS         (1000.0 / 60.0)
#define WAIT_OBJ_0      0u
#define WAIT_TIMEOUT_   0x102u

static struct {
    int      cap, log, check, noskip, dup;
    double   period_ms;             /* 0 = sans limite */
    DWORD    tid;
    double   freq;
    double   last_tick_ret;         /* retour du hook sur événement de frame (≈ début tick + rendu) */
    double   next_render;
    double   avg_render;
    unsigned dump_from;             /* XBOX_D3D_DUMP_FROM (journal des doublons, T4) */            /* moyenne glissante d'un rendu en plus (ms) */
} s;

static struct {
    unsigned long long rider_restored, rng_restored, hook_calls, ticks_ev, extra, extra_ok, refused, skip_tick, late;
    double   extra_ms, extra_max;
    unsigned present0;
    uint32_t race_tick0, race0;
    double   t0;
    /* instrument d'état */
    unsigned long long snaps, d_rng, d_race, d_rider, d_audio, d_state, d_app;
    unsigned long long page_scans, page_changed;
} P, S;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / s.freq;
}

/* Durée d'un rendu (normal ou en plus) dans la moyenne glissante qui décide
 * si un rendu en plus tient avant le tick. Avant,
 * seuls les rendus en plus la mettaient à jour ; une saccade (chargement,
 * 198 ms) la figeait au-dessus de la marge → plus aucun rendu en plus jusqu'à
 * la fin. Durées > 50 ms (chargements, saccades) ignorées. */
#define RENDER_OUTLIER_MS 50.0
#define NV_MAX      4u
#define NR_MAX      8u
#define VIEW_OFF    0xB0u
#define VIEW_LEN    0x58u           /* +0xB0..+0x107 */
#define VIEW_STRIDE 0x80u
#define POS_OFF     0x170u
#define POSE_OFF    0x48B0u
#define POSE_N      21u
#define TELEPORT    400.0f          /* unités par tick (course : ~30) */
#define CAM_CUT_COS 0.866f          /* rotation caméra > 60° en un tick = coupe */
#define BONE_CUT_COS 0.0f           /* os tourné de > 180°… : quaternions, cos(demi-angle) < 0 → 90° */

enum { SP_VIEW, SP_POS, SP_POSE };
typedef struct { uint32_t va, len, kind, off; } Span;
typedef struct {
    uint32_t st, race, n, nw;
    Span sp[NV_MAX + 2u * NR_MAX];
    float w[(NV_MAX * VIEW_LEN + NR_MAX * (0x10u + POSE_N * 0x40u)) / 4u];
} VS;

static struct {
    int on, log, synth;             /* XBOX_FPS_INTERP, XBOX_FPS_INTERP_LOG (nombre de rendus journalisés) */
    VS prev, cur, pre, out;
    int ok;                         /* prev et cur valides, même disposition */
    double t_tick;                  /* début du dernier tick */
    int written;                    /* état interpolé en mémoire (rendu en cours) */
    unsigned long long n_interp, n_mismatch, n_alpha1, n_cut_cam, n_cut_rider, n_badrot, n_logged;
    uint32_t log_from;
} I;

static unsigned long long s_outliers;
static int s_measured;           /* durée déjà comptée par hook_AB610 */
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

/* ── appels invités ─────────────────────────────────────────────── */

/* WaitForSingleObject(handle, ms) du jeu : renvoie 0 si signalé, 0x102 si délai. */
static uint32_t guest_wait(uint32_t handle, uint32_t ms)
{
    uint32_t sv_ecx = g_ecx, sv_edx = g_edx, esp0 = g_esp, r;
    g_esp -= 4; MEM32(g_esp) = ms;
    g_esp -= 4; MEM32(g_esp) = handle;
    g_esp -= 4; MEM32(g_esp) = 0;          /* retour fictif */
    sub_00151D07();
    r = g_eax;
    g_esp = esp0;                          /* ret 8 a tout dépilé ; par sûreté */
    g_ecx = sv_ecx; g_edx = sv_edx;
    return r;
}

/* state->vt+0x18 (rendu de l'état courant), comme la boucle en 0xAA283.
 * Renvoie al (1 = rendu fait). */
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
    g_esi = app;                           /* comme dans la boucle (esi = Application) */
    g_esp -= 4; MEM32(g_esp) = 0;          /* retour fictif */
    fn();
    al = (int)(g_eax & 0xFFu);
    if (g_esp != esp0) {
        static int warned;
        if (!warned++) fprintf(stderr, "[FPSCAP] pile déséquilibrée après le rendu : %d octets\n", (int)(g_esp - esp0));
        g_esp = esp0;
    }
    g_eax = sv[0]; g_ecx = sv[1]; g_edx = sv[2]; g_ebx = sv[3]; g_esi = sv[4]; g_edi = sv[5]; g_seh_ebp = sv[6];
    return al;
}

/* ── instrument d'état (XBOX_FPS_CAP_CHECK=1) ─────────────────── */

/* Régions copiées brutes avant / après chaque rendu en plus ; chaque mot
 * modifié est compté par (région, offset) pour savoir QUOI change. */
enum { RG_RNG, RG_RACE, RG_RIDER, RG_AUDIO, RG_STATE, RG_APP, RG_N };
static const char *const s_rg_name[RG_N] = { "rng", "course", "riders", "audio", "etat_jeu", "app" };
static const uint32_t s_rg_len[RG_N] = { 0x30, 0x400, 0x5A00, 0x400, 0x300, 0x800 };   /* etat_jeu : jusqu'aux vues +0xB0..+0x29F */
static uint32_t *s_rg_hist[RG_N];          /* compte par mot (riders : tous indices confondus) */

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
            if (r >= 0x1000u) o->base[RG_RIDER + i] = r;   /* sous-objet Rider entier (+0x58E0 utilisé) */
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

/* Compare la copie à la mémoire actuelle ; renvoie un masque des régions modifiées. */
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
                if (s_rg_hist[rg][w]++ < 6)   /* 6 premiers exemples par mot : valeur avant -> après */
                    fprintf(stderr, "[FPSCAP] diff %s+%X (base %08X) : %08X -> %08X\n",
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
        fprintf(stderr, "[FPSCAP]   %s : mots modifiés (offset:nb)", s_rg_name[rg]);
        for (w = 0; w < nw && shown < 24; w++)
            if (s_rg_hist[rg][w]) { fprintf(stderr, " +%X:%u", w * 4, s_rg_hist[rg][w]); shown++; }
        fprintf(stderr, "%s\n", shown ? "" : " aucun");
    }
}

/* Fonctions de rendu appelées par les rendus en plus (vt+0x18 de l'état courant). */
static struct { uint32_t va; unsigned long long n, ok; } s_rva[16];
static void rva_note(uint32_t va, int ok)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (s_rva[i].va == va || !s_rva[i].va) { s_rva[i].va = va; s_rva[i].n++; s_rva[i].ok += ok != 0; return; }
    }
}

/* Diff de pages mémoire invitées (4 Ko) autour d'un rendu en plus, échantillonné. */
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
    fprintf(stderr, "[FPSCAP] pages modifiées par les rendus en plus (%llu balayages ; page:nb) :", S.page_scans);
    for (i = 0; i <= PG_MAX; i++) {
        int hit = i < PG_MAX && s_pg_hits[i];
        if (hit && !run) { start = i; run = 1; }
        if (!hit && run) {
            if (shown++ < 60) fprintf(stderr, " %06X-%06X:%u", start << 12, (i << 12) - 1, s_pg_hits[start]);
            run = 0;
        }
    }
    fprintf(stderr, " (%u plages)\n", shown);
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
        "[FPSCAP] t=%.1f cap=%d presents/s=%.1f evenements/s=%.1f ticks_course/s=%.1f "
        "en_plus/s=%.1f (faits %.1f refuses %.1f) sautes_pour_tick/s=%.1f en_retard/s=%.1f "
        "rendu_en_plus_ms=%.2f (max %.2f, moyenne glissante %.2f) rng_restaure=%llu",
        t / 1000.0, s.cap, (d3d8_PresentSeq() - P.present0) / el, P.ticks_ev / el, race_tps,
        P.extra / el, P.extra_ok / el, P.refused / el, P.skip_tick / el, P.late / el,
        P.extra_ok ? P.extra_ms / P.extra_ok : 0.0, P.extra_max, s.avg_render, P.rng_restored);
    if (s.check)
        fprintf(stderr, " | etat[n=%llu rng=%llu course=%llu riders=%llu audio=%llu etat_jeu=%llu app=%llu] pages[balayages=%llu modifiees=%llu]",
            P.snaps, P.d_rng, P.d_race, P.d_rider, P.d_audio, P.d_state, P.d_app, P.page_scans, P.page_changed);
    if (I.on)
        fprintf(stderr, " | interp[n=%llu memoire!=tick=%llu alpha>=1=%llu coupe_cam=%llu coupe_rider=%llu os_non_interp=%llu riders_restaures=%llu] saccades_ignorees=%llu",
                I.n_interp, I.n_mismatch, I.n_alpha1, I.n_cut_cam, I.n_cut_rider, I.n_badrot, P.rider_restored, s_outliers);
    fprintf(stderr, " course_tick=%u etat=%u\n", rt, race ? MEM32(race + 0x1Cu) : 0);
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
        fprintf(stderr, "[FPSCAP] cumul : RNG tires puis restaures dans %llu rendus en plus\n", S.rng_restored);
        fprintf(stderr, "[FPSCAP] cumul : rendus en plus %llu (faits %llu) ; differences rng=%llu course=%llu riders=%llu audio=%llu etat_jeu=%llu app=%llu\n",
            S.extra, S.extra_ok, S.d_rng, S.d_race, S.d_rider, S.d_audio, S.d_state, S.d_app);
        hist_report();
        {
            int i;
            fprintf(stderr, "[FPSCAP]   fonctions de rendu (va:appels/faits)");
            for (i = 0; i < 16 && s_rva[i].va; i++) fprintf(stderr, " %06X:%llu/%llu", s_rva[i].va, s_rva[i].n, s_rva[i].ok);
            fputc('\n', stderr);
        }
        if (s_pg) pages_report();
    }
}

/* Fidélité : le rendu d'InGameState (0xAB610) décompte un saut d'images
 * [état+0x6C] à chaque appel ; un rendu en plus le consommerait plus vite
 * qu'à l'origine. Pas de rendu en plus tant qu'il est > 0. */
static int extra_allowed(uint32_t app)
{
    uint32_t st = MEM32(app + 4u), va;
    if (st < 0x1000u) return 0;
    va = MEM32(MEM32(st) + 0x18u);
    /* Liste blanche: course (InGameState 0xAB610) et menus (0x7CBD0),
     * vérifiés sans effet logique. Les autres états (démarrage, vidéos,
     * chargements) font avancer la machine d'états dans leur rendu ou
     * bloquent : rendu en plus interdit, comme à l'origine. */
    if (va == 0x000AB610u) return (int32_t)MEM32(st + 0x6Cu) <= 0;
    return va == 0x0007CBD0u;
}

/* ── interpolation ─────────────────────────────────────────
 * Un rendu montre lerp(tick N-1, tick N, alpha), alpha = temps depuis le
 * tick N / 16,67 ms : le rendu normal (juste après le tick) comme les rendus
 * en plus. Affichage décalé d'un tick (latence +16,7 ms), aucune extrapolation.
 * États :
 *   InGameState +0xB0 + i×0x80 (vue i < [+0x298]) : position caméra (4 f),
 *     matrice de vue +0xC0 (lignes 0-2 = rotation, ligne 3 = -c·R),
 *     projection +0x100 / +0x104 ;
 *   rider +0x170 (position, 4 f) et pose +0x48B0 : 21 matrices 4×4 en
 *     coordonnées MONDE (rotation orthonormée + translation).
 * Tout est écrit au tick seulement ; le rendu ne les écrit pas (vérifié).
 * Avant chaque rendu : la mémoire doit être égale à la copie du tick N (sinon
 * pas d'interpolation) ; après : la copie du tick N est réécrite (au bit près). */

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

/* Quaternion d'une matrice de rotation 3×3 (lignes m[0..2], stride 4). */
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

/* Rotation 3×3 (lignes de m) presque orthonormée ; renvoie +1 (directe),
 * -1 (indirecte : la matrice de vue du jeu a un déterminant -1) ou 0, et les
 * normes des lignes. */
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

/* Rotation interpolée (slerp), normes des lignes interpolées. dot = cos(demi-angle). */
static int rot_lerp(const float *a, const float *b, float t, float *o, float min_dot)
{
    float na[3], nb[3], ua[12], ub[12], qa[4], qb[4], q[4], d, l;
    int i, j, sa = rot_ok(a, na), sb = rot_ok(b, nb);
    if (!sa || sa != sb) return 0;
    if (sa < 0) { na[2] = -na[2]; nb[2] = -nb[2]; }   /* ligne 2 retournée : rotation directe, remise à la fin */
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

/* Vue : bloc +0xB0 (22 f) : [0..3] position, [4..19] matrice de vue, [20], [21] projection. */
static int view_lerp(const float *a, const float *b, float t, float *o)
{
    float ca[3], cb[3], c[3], *m = o + 4;
    const float *ma = a + 4, *mb = b + 4;
    int i, j, r;
    /* position caméra déduite de la matrice : c = -t·Rᵀ */
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

/* Rider : position + 21 os. Téléportation (os racine) → état N. */
static int rider_lerp(const float *pa, const float *pb, const float *ka, const float *kb, float t, float *po, float *ko)
{
    uint32_t b;
    int i;
    if (!(dist3(pa, pb) < TELEPORT) || !(dist3(ka + 12, kb + 12) < TELEPORT)) return -1;
    for (i = 0; i < 3; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
    for (b = 0; b < POSE_N; b++) {
        const float *x = ka + 16 * b, *y = kb + 16 * b;
        float *o = ko + 16 * b;
        if (rot_lerp(x, y, t, o, BONE_CUT_COS) <= 0) { I.n_badrot++; continue; }   /* reste à N */
        for (i = 0; i < 3; i++) o[12 + i] = x[12 + i] + (y[12 + i] - x[12 + i]) * t;
    }
    return 1;
}

/* Journal des coupes (XBOX_FPS_CAP_LOG=1, 40 premières) : état de course et saut de position. */
static void cut_note(const char *what, uint32_t va, const float *a, const float *b)
{
    static int n;
    uint32_t race = race_ptr();
    if (!s.log || n++ >= 40) return;
    fprintf(stderr, "[INTERP] coupe %s %08X : etat=%u tick=%u saut=%.1f unites\n", what, va,
            race ? MEM32(race + 0x1Cu) : 0, race ? MEM32(race + 0x18u) : 0, dist3(a, b));
}

/* Écrit l'état interpolé avant un rendu d'InGameState ; 1 si écrit. */
static int interp_begin(uint32_t st)
{
    double a;
    float t;
    uint32_t k;
    VS *o = &I.out;
    I.written = 0;
    if (!I.on || !I.ok || I.cur.st != st || GetCurrentThreadId() != s.tid) return 0;
    if (!vs_equals_mem(&I.cur)) { I.n_mismatch++; return 0; }
    a = (now_ms() - I.t_tick) / TICK_MS;
    if (I.synth && s.dup > 0) a = (double)s_dup_k / (s.dup + 1);   /* test DUP : temps synthétique */
    if (a >= 1.0) { I.n_alpha1++; return 0; }
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
            /* vue 0, puis rider 0 : premier bloc position et sa pose (os 0 = racine) */
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
    vs_write(&I.cur);       /* état du tick N, au bit près */
    I.written = 0;
}

/* Test XBOX_FPS_CAP_CHECK=3 : le même rendu en plus refait SANS
 * interpolation depuis le même état ; les mots des riders (et le RNG) qui
 * diffèrent entre les deux rendus sont des valeurs écrites par le rendu qui
 * DÉPENDENT de l'état interpolé. Sert à juger le rendu normal (qui garde ses
 * écritures). Deux Presents par rendu en plus : mesure seulement. */
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
        fprintf(stderr, "[FPSCAP] dependance a l'interpolation : %llu essais, %llu avec mots riders differents, %llu avec RNG different ; mots (offset:nb)",
                s_dep_runs, s_dep_diff, s_dep_rng);
        for (w = 0; w < 0x5A00u / 4u && shown < 64; w++) if (s_dep_hist[w]) { fprintf(stderr, " +%X:%u", w * 4, s_dep_hist[w]); shown++; }
        fputc('\n', stderr);
    }
}

/* ── un rendu en plus ─────────────────────────────────────────── */

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
        if (s.check == 2) {             /* témoin : même durée, sans rendu */
            double until = now_ms() + (s.avg_render > 0.0 ? s.avg_render : 8.0);
            while (now_ms() < until) Sleep(0);
            ok = 1;
        } else {
            /* Fidélité : le rendu d'InGameState tire le RNG A pendant le
             * survol d'intro (état de course 1) ; un rendu en
             * plus décalerait la suite des tirages de la logique. États RNG
             * A et B (6 + 6 mots, 0x1FAD70) remis tels qu'avant le rendu. */
            uint32_t rng[12], i;
            static uint8_t rsv[NR_MAX][0x5A00];
            uint32_t rva[NR_MAX], nr = 0, race = race_ptr();
            for (i = 0; i < 12; i++) rng[i] = MEM32(0x001FAD70u + 4u * i);
            /* Interpolation : le rendu calcule quelques dérivés de pose dans le
             * rider (+0x4DF0..) à partir de l'état interpolé ; un rendu
             * en plus n'existe pas à l'origine → riders entiers remis tels
             * qu'avant (0x5A00 o chacun). */
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
        /* T4 : quels Presents sont des doublons, dans la fenêtre XBOX_D3D_DUMP_FROM. */
        if (s.dump_from && seq0 + 1u >= s.dump_from && seq0 < s.dump_from + 64u)
            fprintf(stderr, "[FPSCAP] rendu en plus : present %u -> %u (ok=%d)\n", seq0, d3d8_PresentSeq(), ok);
    }
    d = now_ms() - t0;
    P.extra++;
    if (ok) {
        if (!s_measured) render_time(d);   /* menus : rendu non mesuré par hook_AB610 */
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

/* ── test : XBOX_FPS_CAP_DUP=N ────────────────────────────────────
 * Après chaque rendu réussi d'InGameState (0xAB610), N rendus en plus
 * sans tick, collés. Même opération que le cap (rendu sans tick), mais
 * forcée : sert au critère « 0 différence sur ≥ 10 000 rendus en course »
 * sur une machine où le rendu de course dépasse le tick (l'attente
 * 0xAA296 n'est alors jamais atteinte). Équivalent de XBOX_C019_EXTRA. */
void sub_000AB610(void);

/* Test (XBOX_FPS_CAP_STALL=s:ms) : une seule saccade simulée de ms dans un
 * rendu normal, s secondes après le démarrage ; prouve que la cadence repart. */
static double s_stall_at, s_stall_ms, s_t0_run;
static void maybe_stall(void)
{
    if (s_stall_ms > 0.0 && !s_in_extra && now_ms() - s_t0_run >= s_stall_at * 1000.0) {
        fprintf(stderr, "[FPSCAP] saccade simulee : %.0f ms\n", s_stall_ms);
        Sleep((DWORD)s_stall_ms);
        s_stall_ms = 0.0;
    }
}

static void hook_AB610(void)
{
    uint32_t state = g_ecx, app = MEM32(APP_GLOBAL), sv_eax, sv_ecx, sv_edx;
    double t0 = now_ms();
    int k;
    interp_begin(state);
    sub_000AB610();
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

/* Rendu des menus 0x7CBD0 : seulement mesuré (durée pour la cadence). */
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
            fprintf(stderr, "[FPSCAP] attente de frame : ecx=%08X edi=%u esi=%08X app=%08X [app+0x2C]=%08X tid=%lu\n",
                    self, g_edi, g_esi, app, app >= 0x1000u ? MEM32(app + 0x2Cu) : 0, GetCurrentThreadId());
        }
    }
    /* Seulement l'attente de la boucle principale en 0xAA296 (aucun tick fait). */
    if (g_edi != 0u || app < 0x1000u || g_esi != app || MEM32(app + 0x2Cu) != self
        || (s.tid && GetCurrentThreadId() != s.tid)) {
        g_ecx = self;
        sub_000B2750();
        return;
    }
    if (!s.tid) s.tid = GetCurrentThreadId();
    handle = MEM32(self + 4u);
    t = now_ms();
    /* Le rendu normal vient de se faire (tick → rendu → attente). */
    if (s.next_render < s.last_tick_ret) s.next_render = s.last_tick_ret + s.period_ms;

    for (;;) {
        double expected_tick = s.last_tick_ret + TICK_MS, wait_until, rem;
        t = now_ms();
        wait_until = s.next_render;
        /* Un rendu en plus qui finirait après le tick attendu retarderait la logique : on attend le tick. */
        if (!s.noskip && s.avg_render > 0.0 && wait_until + s.avg_render > expected_tick && t < expected_tick + TICK_MS) {
            wait_until = 1e300;
            P.skip_tick++;
        }
        rem = wait_until - t;
        if (rem > 1.5) {
            uint32_t ms = rem > 1e8 ? 0xFFFFFFFFu : (uint32_t)(rem - 1.0);
            if (ms != 0xFFFFFFFFu && wait_until == 1e300) ms = 0xFFFFFFFFu;
            r = guest_wait(handle, ms);
            if (r != WAIT_TIMEOUT_) break;      /* signalé (ou erreur : comme l'original, on rend la main) */
            if (wait_until == 1e300) continue;
        }
        /* Fin d'attente fine (< ~1,5 ms) en sondant l'événement. */
        while ((t = now_ms()) < wait_until) {
            if (guest_wait(handle, 0) == WAIT_OBJ_0) goto tick;
            YieldProcessor();
            if (wait_until - t > 0.3) Sleep(0);
        }
        if (guest_wait(handle, 0) == WAIT_OBJ_0) break;   /* un tick dû passe avant */
        if (t > s.next_render + 1.0) P.late++;
        if (!extra_allowed(app)) {          /* attendre le tick (compteur de saut en cours) */
            r = guest_wait(handle, 0xFFFFFFFFu);
            break;
        }
        if (!extra_render(app)) {           /* refusé : comme 0xAA28B, on attend le tick */
            r = guest_wait(handle, 0xFFFFFFFFu);
            break;
        }
        /* Pas de rattrapage : au plus un rendu en retard. */
        s.next_render += s.period_ms;
        t = now_ms();
        if (s.next_render < t) s.next_render = t;
        report(0);
    }
tick:
    P.ticks_ev++;
    s.last_tick_ret = now_ms();
    s.next_render = s.last_tick_ret + s.period_ms;
    report(0);
    g_eax = 0;            /* WAIT_OBJECT_0, comme l'original */
    g_ecx = self;
    g_esp += 4;           /* ret (retour fictif) */
}

/* ── hook du tick de course 0xAD4A0 (InGameState vt+0x14) ──────── */
void sub_000AD4A0(void);
static int s_dump_at;

static void dump_floats(const char *tag, uint32_t va, uint32_t len)
{
    uint32_t o;
    for (o = 0; o < len; o += 16) {
        const float *f = (const float *)XBOX_PTR(va + o);
        const uint32_t *u = (const uint32_t *)XBOX_PTR(va + o);
        fprintf(stderr, "[DUMP] %s+%X : %12.4f %12.4f %12.4f %12.4f | %08X %08X %08X %08X\n",
                tag, o, f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
    }
}

static void hook_AD4A0(void)
{
    uint32_t app = MEM32(APP_GLOBAL), st = app >= 0x1000u ? MEM32(app + 4u) : 0, race;
    double t = now_ms();
    int pre = 0;
    if (I.written) interp_end();     /* par sûreté : jamais de tick sur un état interpolé */
    if (I.on && vs_layout(&I.pre, st)) { vs_read(&I.pre); pre = 1; }
    sub_000AD4A0();
    if (I.on) {
        I.ok = 0;
        if (pre && MEM32(app + 4u) == st && vs_layout(&I.cur, st) && vs_same_layout(&I.cur, &I.pre)) {
            vs_read(&I.cur);
            memcpy(&I.prev, &I.pre, sizeof I.prev);
            I.ok = 1;
        }
        /* début du tick : retour de l'attente de frame si ce tick la suit, sinon maintenant */
        I.t_tick = (t >= s.last_tick_ret && t - s.last_tick_ret < TICK_MS) ? s.last_tick_ret : t;
    }
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
    const char *e = getenv("XBOX_FPS_CAP");
    LARGE_INTEGER f;
    int cap = 60;
    if (e && *e) {
        char *end;
        long v = strtol(e, &end, 10);
        if (*end || v < 0 || v > 1000 || (v > 0 && v < 60)) {
            fprintf(stderr, "[FPSCAP] XBOX_FPS_CAP=%s invalide (0 | 60..1000) : 60\n", e);
            v = 60;
        }
        cap = (int)v;
    }
    if (cap == 60) return;    /* défaut : hook non installé */
    QueryPerformanceFrequency(&f);
    s.freq = (double)f.QuadPart;
    s.cap = cap;
    s.period_ms = cap ? 1000.0 / cap : 0.0;
    e = getenv("XBOX_FPS_CAP_LOG");   s.log = e && e[0] == '1';
    e = getenv("XBOX_FPS_CAP_CHECK"); s.check = e ? atoi(e) : 0;   /* 1 = instrument, 2 = témoin sans rendu */
    e = getenv("XBOX_FPS_CAP_DUP"); s.dup = e ? atoi(e) : 0;     /* test, voir hook_AB610 */
    e = getenv("XBOX_FPS_INTERP_DUMP"); s_dump_at = e ? atoi(e) : 0;
    e = getenv("XBOX_FPS_INTERP"); I.on = e && *e ? e[0] == '1' : 1;     /* défaut 1 quand cap != 60 */
    e = getenv("XBOX_FPS_INTERP_SYNTH"); I.synth = e && e[0] == '1';   /* test DUP : alpha = k / (DUP + 1) */
    e = getenv("XBOX_FPS_INTERP_LOG");
    if (e && *e) { I.log = atoi(e); e = strchr(e, '@'); I.log_from = e ? (uint32_t)atoi(e + 1) : 0; }
    e = getenv("XBOX_FPS_CAP_STALL");
    if (e && *e) { s_stall_at = atof(e); e = strchr(e, ':'); s_stall_ms = e ? atof(e + 1) : 0.0; }
    e = getenv("XBOX_FPS_CAP_NOSKIP"); s.noskip = e && e[0] == '1';   /* test : rendre même si ça retarde le tick */
    e = getenv("XBOX_D3D_DUMP_FROM"); s.dump_from = (e && getenv("XBOX_D3D_DUMP")) ? (unsigned)atoi(e) : 0;
    if (s.check) {
        int rg;
        for (rg = 0; rg < RG_N; rg++) s_rg_hist[rg] = (uint32_t *)calloc(s_rg_len[rg] / 4, sizeof(uint32_t));
    }
    if (s.check) s_pg = (uint64_t *)calloc(PG_MAX, sizeof(uint64_t));
    P.t0 = s_t0_run = now_ms();
    g_fps_cap_on = 1;
    atexit(fps_cap_atexit);
    fprintf(stderr, "[FPSCAP] plafond %d%s (%s ; logique 60 Hz)%s\n",
            cap, cap ? " images/s" : " = sans limite", I.on ? "interpolation camera + riders" : "doublons sans tick",
            s.check ? " ; instrument d'etat actif" : "");
}

void (*fps_cap_lookup(unsigned int xbox_va))(void)
{
    if (xbox_va == 0x000B2750u) return hook_B2750;
    if (xbox_va == 0x000AB610u) return hook_AB610;
    if (xbox_va == 0x0007CBD0u) return hook_7CBD0;
    if (xbox_va == 0x000AD4A0u && (I.on || s_dump_at)) return hook_AD4A0;
    return 0;
}
