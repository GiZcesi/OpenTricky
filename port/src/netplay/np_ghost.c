/*
 * np_ghost -- ghost local rejoué depuis les commandes enregistrées (fork).
 * Voir np_ghost.h pour l'interrupteur, les hooks, l'alignement et les limites.
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
#include "np_ghost.h"

extern void sub_000AC9B0(void);     /* InGameState_LoadLevel */
extern void sub_0002E040(void);     /* Race_ResetPlayerRoster */

int g_np_ghost_on = 0;

static void nc_stats(const char *why);   /* plus bas */

#define NG_UNSET      0xFFFFFFFFu
#define NG_NSTATES    16u
#define NG_ROSTER_VA  0x001DE900u
#define NG_ROSTER_N   0x001DE8FCu

/* Données lues dans le .npcl */
static struct {
    uint32_t version, track, mode, seed98, seed9c, nriders, nroster;
    uint8_t  roster[NPCL_MAX_RIDERS * NPCL_ROSTER_STRIDE];
    uint32_t player_idx;                /* index du Player enregistré */
    uint32_t slot, human;               /* slot ghost, index de l'humain */
    uint32_t r_start[NG_NSTATES];       /* 1er race+0x18 de chaque état */
    uint32_t fmax;                      /* race+0x18 max enregistré */
    uint32_t *cmd;                      /* commande par race+0x18 */
    uint8_t  *valid;
    uint8_t  *st_at;                    /* état de course enregistré par race+0x18 */
    uint32_t *hcmd;                     /* XBOX_GHOST_HUMAN_REPLAY : commande de l'IA */
    float    *hsf;                      /*   enregistrée à l'index humain, et son +0x15C */
    uint8_t  *hvalid;
    int      human_replay;
    int      have_rng;
    uint32_t rng_a[6], rng_b[6];        /* états RNG au début de l'état 3 */
    int      force;                     /* forcer roster / graines / RNG */
} s_g;

/* État pendant la course */
static struct {
    uint32_t race, races, last_rf;
    uint32_t g_start[NG_NSTATES];
    int      done, rng_done, roster_done, warned_empty;
    unsigned long long taken, fallback, seed_writes;
} s_rt;

static uint32_t ng_race_ptr(void)
{
    uint32_t app = MEM32(0x001E3C7Cu), lvl;
    if (app < 0x1000u || (lvl = MEM32(app + 0x72Cu)) < 0x1000u) return 0;
    return MEM32(lvl + 0x1Cu);
}

static void ng_stats(const char *why)
{
    fprintf(stderr, "[GHOST] %s : course=%u commandes_rejouees=%llu replis_IA=%llu termine=%d rng_force=%d\n",
            why, s_rt.races, s_rt.taken, s_rt.fallback, s_rt.done, s_rt.rng_done);
    fflush(stderr);
    nc_stats(why);
}

void np_ghost_before(void)
{
    uint32_t race = ng_race_ptr(), st, rf, i;
    if (!race) return;
    rf = MEM32(race + 0x18u);
    if (race != s_rt.race) {
        s_rt.race = race;
        s_rt.races++;
        for (i = 0; i < NG_NSTATES; i++) s_rt.g_start[i] = NG_UNSET;
        s_rt.rng_done = 0;
        if (s_rt.races > 1 && !s_rt.done) { s_rt.done = 1; ng_stats("nouvel objet course, ghost termine"); }
    } else if (!s_rt.done && rf < s_rt.last_rf) {
        s_rt.done = 1;
        ng_stats("race+0x18 recule (relecture), ghost termine");
    }
    s_rt.last_rf = rf;
    if (s_rt.done) return;

    st = MEM32(race + 0x1Cu);
    if (st < NG_NSTATES && s_rt.g_start[st] == NG_UNSET) {
        s_rt.g_start[st] = rf;
        fprintf(stderr, "[GHOST] etat %u : race+0x18=%u (enregistre : %u)\n", st, rf,
                s_g.r_start[st] == NG_UNSET ? 0xFFFFFFFFu : s_g.r_start[st]);
        ng_stats("bilan");
    }
    if (st == 5u) { s_rt.done = 1; ng_stats("EndRace, ghost termine"); return; }
    if (st == 3u && !s_rt.rng_done && s_g.have_rng && s_g.force) {
        for (i = 0; i < 6; i++) {
            MEM32(NPCL_RNG_A_VA + 4u * i) = s_g.rng_a[i];
            MEM32(NPCL_RNG_B_VA + 4u * i) = s_g.rng_b[i];
        }
        s_rt.rng_done = 1;
        fprintf(stderr, "[GHOST] etats RNG A/B forces au debut de l'etat 3 (race+0x18=%u)\n", rf);
    }
}

/* ── Corrections d'état (instantanés .npst du Player enregistré) ──
 * Au point de la demande de commande (même point que l'enregistrement), les
 * plages choisies du sous-objet Rider enregistré au tick d'origine gf sont
 * recopiées dans le ghost : tous les `period` ticks d'origine (gf % period),
 * et dès que l'écart de position dépasse `thr` (contact). Voir np_ghost.h. */
#define NC_MAXR 16
static struct {
    int      on, same_ev;
    uint32_t period;            /* ticks d'origine entre deux corrections (0 = jamais) */
    float    thr, jump;         /* seuils : correction sur écart / saut « visible » */
    uint32_t nr, off[NC_MAXR], len[NC_MAXR], setsz;
    uint8_t  *data;             /* (fmax+1) x setsz */
    uint8_t  *have;
    uint32_t *ev;               /* +0x458 enregistré */
    float    *pos;              /* +0x170 enregistré (3 floats) */
    unsigned long long n_corr, n_thr, n_jump, n_skip_ev, n_nostate;
    float    err_max;
    uint32_t race_state;         /* état de course où corriger (4 = Race) */
    unsigned jump_ev[32];        /* sauts par état +0x458 du ghost (0..31) */
    double   err_sum; unsigned long long err_n;   /* écart avant correction */
} s_c;

static int nc_add(uint32_t off, uint32_t len)
{
    if (s_c.nr >= NC_MAXR || !len || off + len > NPST_BLOCK) return 0;
    s_c.off[s_c.nr] = off; s_c.len[s_c.nr] = len; s_c.nr++;
    s_c.setsz += len;
    return 1;
}

/* "pos" | "kin" | "kinrot" | "kinrot458" | liste "off:len,..." (hexa) */
static void nc_parse(const char *e)
{
    if (!e || !*e || !strcmp(e, "rec")) { nc_add(0x170, 0x50); nc_add(0x454, 0x08); return; }   /* retenu */
    if (!strcmp(e, "kinrot")) { nc_add(0x170, 0x24); nc_add(0x448, 0x10); nc_add(0x478, 0x20); return; }
    if (!strcmp(e, "pos")) { nc_add(0x170, 0x0C); return; }
    if (!strcmp(e, "body")) { nc_add(0x170, 0x50); return; }   /* pos, vel, avancement, quaternion, direction */
    if (!strcmp(e, "kin")) { nc_add(0x170, 0x24); return; }
    if (!strcmp(e, "kinrot458")) { nc_add(0x170, 0x24); nc_add(0x448, 0x14); nc_add(0x478, 0x20); return; }
    while (*e) {
        char *end;
        uint32_t o = (uint32_t)strtoul(e, &end, 16), l;
        if (*end != ':') break;
        l = (uint32_t)strtoul(end + 1, &end, 16);
        nc_add(o, l);
        if (*end != ',') break;
        e = end + 1;
    }
}

static void nc_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint32_t hdr[4], n = s_g.fmax + 1, i;
    npst_record r;
    uint8_t *blk;
    unsigned long long cnt = 0;
    if (!f) { fprintf(stderr, "[GHOST] corrections : %s introuvable : off\n", path); return; }
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != NPST_MAGIC || hdr[2] != NPST_BLOCK) {
        fprintf(stderr, "[GHOST] corrections : %s n'est pas un .npst v1 : off\n", path); fclose(f); return;
    }
    s_c.data = calloc((size_t)n, s_c.setsz);
    s_c.have = calloc(n, 1);
    s_c.ev = calloc(n, 4);
    s_c.pos = calloc((size_t)n * 3, 4);
    blk = malloc(NPST_BLOCK);
    if (!s_c.data || !s_c.have || !s_c.ev || !s_c.pos || !blk) { fclose(f); free(blk); return; }
    while (fread(&r, sizeof r, 1, f) == 1 && fread(blk, NPST_BLOCK, 1, f) == 1) {
        uint32_t at = 0;
        if (r.race_frame >= n) continue;
        for (i = 0; i < s_c.nr; i++) {
            memcpy(s_c.data + (size_t)r.race_frame * s_c.setsz + at, blk + s_c.off[i], s_c.len[i]);
            at += s_c.len[i];
        }
        memcpy(&s_c.ev[r.race_frame], blk + 0x458, 4);
        memcpy(&s_c.pos[(size_t)r.race_frame * 3], blk + 0x170, 12);
        s_c.have[r.race_frame] = 1;
        cnt++;
    }
    fclose(f); free(blk);
    s_c.on = cnt > 0;
    fprintf(stderr, "[GHOST] corrections : %llu instantanes (%u plages, %u o par correction), periode %u ticks, "
            "seuil %.0f, saut visible > %.0f, meme etat +0x458 %s\n", cnt, s_c.nr, s_c.setsz, s_c.period,
            s_c.thr, s_c.jump, s_c.same_ev ? "exige" : "non exige");
}

static void nc_stats(const char *why)
{
    if (!s_c.on) return;
    fprintf(stderr, "[GHOST] corrections %s : appliquees=%llu (sur seuil %llu), sauts>%.0f=%llu, "
            "ignorees etat+0x458 different=%llu, sans instantane=%llu, ecart avant correction moy=%.1f max=%.1f\n",
            why, s_c.n_corr, s_c.n_thr, s_c.jump, s_c.n_jump, s_c.n_skip_ev, s_c.n_nostate,
            s_c.err_n ? s_c.err_sum / (double)s_c.err_n : 0.0, s_c.err_max);
    {
        unsigned i;
        fprintf(stderr, "[GHOST] sauts par etat +0x458 du ghost :");
        for (i = 0; i < 32; i++) if (s_c.jump_ev[i]) fprintf(stderr, " %u:%u", i, s_c.jump_ev[i]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

static void nc_apply(uint32_t rider, uint32_t gf, uint32_t st)
{
    float d2 = 0.0f, err;
    uint32_t i, at = 0;
    int periodic, over;
    if (!s_c.on || (s_c.race_state && st != s_c.race_state)) return;
    if (gf > s_g.fmax || !s_c.have[gf]) { s_c.n_nostate++; return; }
    for (i = 0; i < 3; i++) {
        float dv = MEMF(rider + 0x170u + 4u * i) - s_c.pos[gf * 3 + i];
        d2 += dv * dv;
    }
    err = sqrtf(d2);
    periodic = s_c.period && (gf % s_c.period) == 0;
    over = s_c.thr > 0.0f && err > s_c.thr;
    if (!periodic && !over) return;
    if (s_c.same_ev && MEM32(rider + 0x458u) != s_c.ev[gf]) { s_c.n_skip_ev++; return; }
    for (i = 0; i < s_c.nr; i++) {
        memcpy((void *)XBOX_PTR(rider + s_c.off[i]), s_c.data + (size_t)gf * s_c.setsz + at, s_c.len[i]);
        at += s_c.len[i];
    }
    s_c.n_corr++;
    if (over) s_c.n_thr++;
    if (err > s_c.jump) { uint32_t ev = MEM32(rider + 0x458u); s_c.n_jump++; s_c.jump_ev[ev < 32u ? ev : 31u]++; }
    if (err > s_c.err_max) s_c.err_max = err;
    s_c.err_sum += err; s_c.err_n++;
    if (s_c.n_corr % 600u == 0) nc_stats("bilan");
}

static void nc_init(void)
{
    const char *e = getenv("XBOX_GHOST_STATE"), *v;
    if (!e || !*e) return;
    v = getenv("XBOX_GHOST_CORR_HZ");
    s_c.period = (v && *v) ? (atoi(v) > 0 ? 60u / (uint32_t)atoi(v) : 0u) : 3u;
    v = getenv("XBOX_GHOST_CORR_THR");    s_c.thr = (v && *v) ? (float)atof(v) : 20.0f;
    v = getenv("XBOX_GHOST_CORR_JUMP");   s_c.jump = (v && *v) ? (float)atof(v) : 39.4f;
    v = getenv("XBOX_GHOST_CORR_STATE");  s_c.race_state = (v && *v) ? (uint32_t)atoi(v) : 4u;
    v = getenv("XBOX_GHOST_CORR_SAMEEV"); s_c.same_ev = v && v[0] == '1';   /* défaut 0 : mieux mesuré */
    nc_parse(getenv("XBOX_GHOST_CORR_SET"));
    if (!s_c.nr) { fprintf(stderr, "[GHOST] corrections : XBOX_GHOST_CORR_SET vide : off\n"); return; }
    nc_load(e);
}

int np_ghost_take(uint32_t rider, uint32_t pcmd)
{
    uint32_t race, st, rf, gf;
    if (s_rt.done || !pcmd) return 0;
    race = ng_race_ptr();
    if (!race || s_g.slot >= MEM32(race + 0x88u)) return 0;
    if (rider != MEM32(race + 0xC4u + 4u * s_g.slot)) return 0;
    st = MEM32(race + 0x1Cu);
    rf = MEM32(race + 0x18u);
    if (st >= NG_NSTATES || s_g.r_start[st] == NG_UNSET || s_rt.g_start[st] == NG_UNSET
        || rf < s_rt.g_start[st]) { s_rt.fallback++; return 0; }
    gf = s_g.r_start[st] + (rf - s_rt.g_start[st]);
    if (gf > s_g.fmax || !s_g.valid[gf] || s_g.st_at[gf] != st) {
        s_rt.fallback++;
        if (!s_rt.warned_empty) {
            s_rt.warned_empty = 1;
            fprintf(stderr, "[GHOST] plus de commande enregistree (etat %u, tick d'origine %u) : repli IA\n", st, gf);
            ng_stats("repli");
        }
        return 0;
    }
    MEM32(pcmd) = s_g.cmd[gf];
    MEMF(rider + 0x15Cu) = 1.0f;
    nc_apply(rider, gf, st);
    s_rt.taken++;
    return 1;
}

/* XBOX_GHOST_HUMAN_REPLAY=1 : appelé par le hook Player APRÈS l'original ;
 * remplace la commande du Player humain par celle de l'IA enregistrée au même
 * index, et son facteur de vitesse. Sert à reproduire la course entière
 * (aucun rider ne diffère de l'enregistrement). */
void np_ghost_player_after(uint32_t rider, uint32_t pcmd)
{
    uint32_t race, st, rf, gf;
    if (!s_g.human_replay || s_rt.done || !pcmd) return;
    race = ng_race_ptr();
    if (!race || rider != MEM32(race + 0xC4u + 4u * s_g.human)) return;
    st = MEM32(race + 0x1Cu);
    rf = MEM32(race + 0x18u);
    if (st >= NG_NSTATES || s_g.r_start[st] == NG_UNSET || s_rt.g_start[st] == NG_UNSET
        || rf < s_rt.g_start[st]) return;
    gf = s_g.r_start[st] + (rf - s_rt.g_start[st]);
    if (gf > s_g.fmax || !s_g.hvalid[gf] || s_g.st_at[gf] != st) return;
    MEM32(pcmd) = s_g.hcmd[gf];
    MEMF(rider + 0x15Cu) = s_g.hsf[gf];
}

/* ── 0xAC9B0 : InGameState_LoadLevel (thiscall, 0 arg, ret) ── */
static void hook_AC9B0(void)
{
    uint32_t ecx = g_ecx, esp0 = g_esp, i;
    if (s_g.force && s_rt.races == 0) {
        uint32_t n = s_g.nroster;
        uint32_t cur = MEM32(NG_ROSTER_N);
        for (i = 0; i < n * NPCL_ROSTER_STRIDE; i++) MEM8(NG_ROSTER_VA + i) = s_g.roster[i];
        for (i = 0; i < n; i++) MEM8(NG_ROSTER_VA + i * NPCL_ROSTER_STRIDE + 0x7Du) = 0xFFu;
        MEM8(NG_ROSTER_VA + s_g.human * NPCL_ROSTER_STRIDE + 0x7Du) = 0x00u;
        MEM32(NG_ROSTER_N) = n;
        MEM32(0x001DEC9Cu) = s_g.seed9c;
        if (!s_rt.roster_done)
            fprintf(stderr, "[GHOST] LoadLevel : roster force (%u entrees, %u avant), ghost=%u (IA), humain=%u (port 0), "
                    "[0x1DEC9C]=0x%08X, piste live=%u enregistree=%u\n", n, cur, s_g.slot, s_g.human,
                    s_g.seed9c, MEM32(0x001DEC90u), s_g.track);
        s_rt.roster_done = 1;
    }
    g_ecx = ecx;
    sub_000AC9B0();
    (void)esp0;
}

/* ── 0x2E040 : Race_ResetPlayerRoster (thiscall, ret 8) ── */
static void hook_2E040(void)
{
    uint32_t ecx = g_ecx;
    if (s_g.force && !s_rt.done && s_rt.races <= 1) {
        uint32_t before = MEM32(0x001DEC98u);
        MEM32(0x001DEC98u) = s_g.seed98;
        s_rt.seed_writes++;
        fprintf(stderr, "[GHOST] Race_ResetPlayerRoster : [0x1DEC98] 0x%08X -> 0x%08X\n", before, s_g.seed98);
    }
    g_ecx = ecx;
    sub_0002E040();
}

void (*np_ghost_lookup(uint32_t xbox_va))(void)
{
    if (xbox_va == 0x000AC9B0u) return hook_AC9B0;
    if (xbox_va == 0x0002E040u) return hook_2E040;
    return 0;
}

/* ── Lecture du .npcl ── */
static int ng_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint32_t h[12], hsize, rsize, i, n;
    long fsz;
    npcl_record rec;
    int rng_seen[2] = { 0, 0 };

    if (!f) { fprintf(stderr, "[GHOST] impossible d'ouvrir %s\n", path); return 0; }
    if (fread(h, 4, 12, f) != 12 || h[0] != NPCL_MAGIC || h[3] != sizeof(npcl_record)) {
        fprintf(stderr, "[GHOST] %s : pas un .npcl lisible\n", path); fclose(f); return 0;
    }
    s_g.version = h[1]; hsize = h[2]; rsize = h[3];
    s_g.track = h[4]; s_g.mode = h[5]; s_g.seed98 = h[6]; s_g.seed9c = h[7];
    n = h[9]; if (n > NPCL_MAX_RIDERS) n = NPCL_MAX_RIDERS;
    s_g.nriders = n;
    fseek(f, 48 + 4 * (long)h[9], SEEK_SET);
    if (fread(&s_g.nroster, 4, 1, f) != 1 || s_g.nroster > NPCL_MAX_RIDERS) { fclose(f); return 0; }
    if (fread(s_g.roster, NPCL_ROSTER_STRIDE, s_g.nroster, f) != s_g.nroster) { fclose(f); return 0; }

    fseek(f, 0, SEEK_END); fsz = ftell(f);
    for (i = 0; i < NG_NSTATES; i++) s_g.r_start[i] = NG_UNSET;
    s_g.player_idx = NG_UNSET;
    s_g.fmax = 0;
    /* 1re passe : bornes */
    fseek(f, (long)hsize, SEEK_SET);
    while (fread(&rec, rsize, 1, f) == 1) {
        if (rec.race_state < NG_NSTATES && s_g.r_start[rec.race_state] == NG_UNSET)
            s_g.r_start[rec.race_state] = rec.race_frame;
        if (rec.kind == NPCL_KIND_PLAYER) {
            if (s_g.player_idx == NG_UNSET) s_g.player_idx = rec.idx;
            if (rec.race_frame > s_g.fmax) s_g.fmax = rec.race_frame;
        }
    }
    if (s_g.player_idx == NG_UNSET || s_g.fmax > 2000000u) {
        fprintf(stderr, "[GHOST] %s : aucun enregistrement Player\n", path); fclose(f); return 0;
    }
    s_g.cmd = (uint32_t *)calloc(s_g.fmax + 1u, 4);
    s_g.valid = (uint8_t *)calloc(s_g.fmax + 1u, 1);
    s_g.st_at = (uint8_t *)calloc(s_g.fmax + 1u, 1);
    s_g.hcmd = (uint32_t *)calloc(s_g.fmax + 1u, 4);
    s_g.hsf = (float *)calloc(s_g.fmax + 1u, 4);
    s_g.hvalid = (uint8_t *)calloc(s_g.fmax + 1u, 1);
    if (!s_g.cmd || !s_g.valid || !s_g.st_at || !s_g.hcmd || !s_g.hsf || !s_g.hvalid) { fclose(f); return 0; }
    /* slot et humain (nécessaires à la 2e passe) */
    {
        const char *v = getenv("XBOX_GHOST_SLOT");
        s_g.slot = (v && *v) ? (uint32_t)strtoul(v, 0, 10) : s_g.player_idx;
        s_g.human = s_g.slot < s_g.nroster / 2u ? s_g.nroster - 1u : 0u;
        v = getenv("XBOX_GHOST_HUMAN");
        if (v && *v) s_g.human = (uint32_t)strtoul(v, 0, 10);
        v = getenv("XBOX_GHOST_HUMAN_REPLAY");
        s_g.human_replay = v && v[0] == '1';
    }
    /* 2e passe : commandes et états RNG du début de l'état 3 */
    fseek(f, (long)hsize, SEEK_SET);
    while (fread(&rec, rsize, 1, f) == 1) {
        if (rec.kind == NPCL_KIND_PLAYER && rec.idx == s_g.player_idx) {
            s_g.cmd[rec.race_frame] = rec.cmd;
            s_g.valid[rec.race_frame] = 1;
            s_g.st_at[rec.race_frame] = rec.race_state;
        } else if (rec.kind == NPCL_KIND_AI && rec.idx == s_g.human && rec.race_frame <= s_g.fmax) {
            s_g.hcmd[rec.race_frame] = rec.cmd;
            s_g.hsf[rec.race_frame] = rec.speed_factor;
            s_g.hvalid[rec.race_frame] = 1;
        } else if ((rec.kind == NPCL_KIND_RNG_A || rec.kind == NPCL_KIND_RNG_B) && rec.cmd == 3u) {
            int k = rec.kind - NPCL_KIND_RNG_A;
            uint32_t *dst = k ? s_g.rng_b : s_g.rng_a;
            if (!rng_seen[k]) {
                memcpy(&dst[0], rec.pos, 12);
                memcpy(&dst[3], rec.vel, 12);
                rng_seen[k] = 1;
            }
        }
    }
    s_g.have_rng = rng_seen[0] && rng_seen[1];
    fclose(f);
    fprintf(stderr, "[GHOST] %s : v%u, %ld octets, piste=%u mode=%u, %u riders, Player enregistre = index %u, "
            "commandes jusqu'a race+0x18=%u, RNG etat 3 : %s\n", path, s_g.version, fsz, s_g.track, s_g.mode,
            s_g.nriders, s_g.player_idx, s_g.fmax, s_g.have_rng ? "oui" : "non (v2 : RNG non force)");
    return 1;
}

void np_ghost_init(void)
{
    const char *e = getenv("XBOX_GHOST"), *v;
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "off")) return;
    if (!ng_load(e)) { fprintf(stderr, "[GHOST] off\n"); return; }
    v = getenv("XBOX_GHOST_FORCE");
    s_g.force = !(v && !strcmp(v, "none"));
    if (s_g.slot >= s_g.nroster || s_g.human >= s_g.nroster || s_g.slot == s_g.human) {
        fprintf(stderr, "[GHOST] slot=%u humain=%u invalides (roster de %u) : off\n", s_g.slot, s_g.human, s_g.nroster);
        return;
    }
    fprintf(stderr, "[GHOST] actif : slot=%u humain=%u conditions d'egalite %s\n", s_g.slot, s_g.human,
            s_g.force ? "forcees (roster, graines, RNG)" : "NON forcees (XBOX_GHOST_FORCE=none)");
    if (s_g.human_replay)
        fprintf(stderr, "[GHOST] humain : rejoue la commande et le +0x15C de l'IA enregistree a l'index %u\n",
                s_g.human);
    nc_init();
    g_np_ghost_on = 1;
}
