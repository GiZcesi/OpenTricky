/*
 * np_cmdlog -- journal des commandes et de l'état des riders (fork).
 * Voir np_cmdlog.h pour l'interrupteur, les hooks, les offsets et le format.
 */
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Même inclusion que recomp_manual.c : g_esp, MEM32, MEMF, sans les alias de
 * registres du code généré. */
#include "../recomp/recomp_types.h"
#include "np_cmdlog.h"
#include "np_ghost.h"

extern void sub_0005BEB0(void);     /* Player : commande depuis la manette */
extern void sub_00048C40(void);     /* OtherRider : commande depuis l'IA */
unsigned d3d8_PresentSeq(void);

int g_np_cmdlog_on = 0;

static CRITICAL_SECTION s_lock;

static struct {
    FILE    *f;                 /* fichier .npcl de la course en cours */
    FILE    *txt;               /* bilans texte, à côté */
    FILE    *st;                /* instantanés .npst (XBOX_NETLOG_STATE=1) */
    int      state_on;
    unsigned long long st_blocks;
    char     dir[MAX_PATH];
    char     stamp[32];
    unsigned nfile;
    uint32_t race;              /* objet course du fichier ouvert */
    uint32_t last_race_frame;
    uint32_t tick;
    uint32_t seen_mask;         /* riders déjà vus dans le tick courant */
    uint32_t last_state;        /* dernier race+0x1C journalisé (événements RNG) */
    unsigned long long rec_file, rec_total;
    unsigned long long calls[2];          /* par kind */
    unsigned long long no_race, idx_unknown;
    DWORD    tid[4];
    unsigned long long tid_calls[4];
} s_np;

static volatile LONG s_esp_bad;

static uint32_t np_race_ptr(void)
{
    uint32_t app = MEM32(0x001E3C7Cu), lvl;
    if (app < 0x1000u) return 0;
    lvl = MEM32(app + 0x72Cu);
    if (lvl < 0x1000u) return 0;
    return MEM32(lvl + 0x1Cu);
}

/* Le dossier « _local » le plus haut en remontant depuis le dossier de l'exe (même
 * règle que pass_tags), + \netlog. À défaut, le dossier de l'exe. */
static void np_default_dir(char *out, size_t cap)
{
    char dir[MAX_PATH], probe[MAX_PATH];
    char *s;
    DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) { snprintf(out, cap, "."); return; }
    s = strrchr(dir, '\\');
    if (s) *s = 0;
    snprintf(out, cap, "%s", dir);
    for (;;) {
        DWORD a;
        if (snprintf(probe, sizeof probe, "%s\\_local", dir) >= (int)sizeof probe) return;
        a = GetFileAttributesA(probe);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)
            && snprintf(out, cap, "%s\\netlog", probe) >= (int)cap)
            snprintf(out, cap, ".");
        s = strrchr(dir, '\\');
        if (!s) return;
        *s = 0;
    }
}

static void np_put32(uint32_t v)
{
    fwrite(&v, 4, 1, s_np.f);
}

static void np_summary(const char *why)
{
    if (!s_np.txt) return;
    fprintf(s_np.txt,
            "[%s] fichier=%u tick=%u enregistrements=%llu (total %llu) appels Player=%llu IA=%llu "
            "esp_bad=%ld hors_course=%llu index_inconnu=%llu threads=%lu:%llu,%lu:%llu,%lu:%llu,%lu:%llu "
            "present=%u\n",
            why, s_np.nfile, s_np.tick, s_np.rec_file, s_np.rec_total, s_np.calls[0], s_np.calls[1],
            (long)s_esp_bad, s_np.no_race, s_np.idx_unknown,
            (unsigned long)s_np.tid[0], s_np.tid_calls[0], (unsigned long)s_np.tid[1], s_np.tid_calls[1],
            (unsigned long)s_np.tid[2], s_np.tid_calls[2], (unsigned long)s_np.tid[3], s_np.tid_calls[3],
            d3d8_PresentSeq());
    fflush(s_np.txt);
}

static void np_close(void)
{
    if (!s_np.f) return;
    np_summary("fin");
    fclose(s_np.f);
    s_np.f = 0;
    if (s_np.st) {
        fprintf(stderr, "[NETLOG] instantanes : %llu blocs\n", s_np.st_blocks);
        fclose(s_np.st);
        s_np.st = 0;
    }
    s_np.race = 0;
}

static void np_open(uint32_t race)
{
    char path[MAX_PATH];
    uint32_t n, np, nai, r, i;
    if (snprintf(path, sizeof path, "%s\\cmdlog_%s_%02u.npcl", s_np.dir, s_np.stamp,
                 s_np.nfile + 1) >= (int)sizeof path) return;
    s_np.f = fopen(path, "wb");
    if (!s_np.f) {
        fprintf(stderr, "[NETLOG] impossible d'ouvrir %s\n", path);
        return;
    }
    setvbuf(s_np.f, NULL, _IOFBF, 256 * 1024);
    if (s_np.state_on) {
        char sp[MAX_PATH];
        uint32_t hdr[4] = { NPST_MAGIC, NPST_VERSION, NPST_BLOCK, 0 };
        snprintf(sp, sizeof sp, "%.*s.npst", (int)(strlen(path) - 5), path);
        s_np.st = fopen(sp, "wb");
        s_np.st_blocks = 0;
        if (s_np.st) {
            setvbuf(s_np.st, NULL, _IOFBF, 1024 * 1024);
            fwrite(hdr, sizeof hdr, 1, s_np.st);
        } else {
            fprintf(stderr, "[NETLOG] impossible d'ouvrir %s\n", sp);
        }
    }
    s_np.nfile++;
    s_np.race = race;
    s_np.tick = 0;
    s_np.seen_mask = 0;
    s_np.rec_file = 0;
    s_np.last_race_frame = MEM32(race + 0x18u);
    s_np.last_state = 0xFFFFFFFFu;

    n = MEM32(race + 0x88u);   if (n > NPCL_MAX_RIDERS) n = NPCL_MAX_RIDERS;
    np = MEM32(race + 0x7Cu);
    nai = MEM32(race + 0x80u);
    r = MEM32(0x001DE8FCu);    if (r > NPCL_MAX_RIDERS) r = NPCL_MAX_RIDERS;

    np_put32(NPCL_MAGIC);
    np_put32(NPCL_VERSION);
    np_put32(4u * (15u + n) + r * NPCL_ROSTER_STRIDE);  /* taille de l'en-tête */
    np_put32((uint32_t)sizeof(npcl_record));
    np_put32(MEM32(0x001DEC90u));                        /* piste */
    np_put32(MEM32(0x001DEC94u));                        /* mode de jeu */
    np_put32(MEM32(0x001DEC98u));
    np_put32(MEM32(0x001DEC9Cu));
    np_put32(race);
    np_put32(n);
    np_put32(np);
    np_put32(nai);
    for (i = 0; i < n; i++) np_put32(MEM32(race + 0xC4u + 4u * i));
    np_put32(r);
    for (i = 0; i < r * NPCL_ROSTER_STRIDE; i++) {
        uint8_t b = MEM8(0x001DE900u + i);
        fwrite(&b, 1, 1, s_np.f);
    }
    /* 2 dwords de réserve (à zéro) : 12 fixes + nombre de roster + 2 = 15. */
    np_put32(0);
    np_put32(0);
    fflush(s_np.f);
    fprintf(stderr, "[NETLOG] course %u -> %s (riders=%u players=%u ia=%u roster=%u)\n",
            s_np.nfile, path, n, np, nai, r);
    np_summary("debut");
}

static void np_note_thread(void)
{
    DWORD me = GetCurrentThreadId();
    int i;
    for (i = 0; i < 4; i++) {
        if (s_np.tid[i] == me) { s_np.tid_calls[i]++; return; }
        if (!s_np.tid[i]) { s_np.tid[i] = me; s_np.tid_calls[i] = 1; return; }
    }
}

/* Ouvre / change de fichier si besoin. Renvoie 0 sans fichier. Verrou tenu. */
static int np_ensure(uint32_t race, uint32_t rf)
{
    if (s_np.f && (race != s_np.race || rf < s_np.last_race_frame)) np_close();
    if (!s_np.f) np_open(race);
    if (!s_np.f) return 0;
    s_np.last_race_frame = rf;
    return 1;
}

/* Événements RNG (v3) : à chaque changement de race+0x1C, au PREMIER appel de
 * commande du tick et AVANT l'original, les deux états RNG (6 dwords chacun)
 * sont écrits dans deux enregistrements : kind NPCL_KIND_RNG_A / _B,
 * idx 0xFE, cmd = nouvel état de course, pos[0..2] + vel[0..2] = les 6 dwords
 * bruts. C'est le point exact où np_ghost les réécrit (état 3). */
static void np_log_rng(uint32_t race, uint32_t rf, uint32_t state)
{
    static const uint32_t base[2] = { NPCL_RNG_A_VA, NPCL_RNG_B_VA };
    npcl_record rec;
    int k, i;
    for (k = 0; k < 2; k++) {
        uint32_t w[6];
        memset(&rec, 0, sizeof rec);
        rec.tick = s_np.tick;
        rec.race_frame = rf;
        rec.present = d3d8_PresentSeq();
        rec.idx = 0xFE;
        rec.kind = (uint8_t)(NPCL_KIND_RNG_A + k);
        rec.race_state = (uint8_t)state;
        rec.cmd = state;
        for (i = 0; i < 6; i++) w[i] = MEM32(base[k] + 4u * i);
        memcpy(rec.pos, &w[0], 12);
        memcpy(rec.vel, &w[3], 12);
        fwrite(&rec, sizeof rec, 1, s_np.f);
        s_np.rec_file++;
        s_np.rec_total++;
    }
    (void)race;
}

static void np_before_locked(void)
{
    uint32_t race = np_race_ptr(), rf, st;
    if (!race) return;
    rf = MEM32(race + 0x18u);
    if (!np_ensure(race, rf)) return;
    st = MEM32(race + 0x1Cu);
    if (st != s_np.last_state) {
        np_log_rng(race, rf, st);
        s_np.last_state = st;
    }
}

/* Appelé après l'original, verrou tenu. */
static void np_note(uint32_t rider, uint32_t pcmd, uint8_t kind)
{
    npcl_record rec;
    uint32_t race = np_race_ptr(), n, i, m, key, rf;

    s_np.calls[kind]++;
    np_note_thread();
    if (!race || !rider) { s_np.no_race++; return; }

    rf = MEM32(race + 0x18u);
    if (!np_ensure(race, rf)) return;

    memset(&rec, 0, sizeof rec);
    n = MEM32(race + 0x88u);
    if (n > NPCL_MAX_RIDERS) n = NPCL_MAX_RIDERS;
    rec.idx = 0xFF;
    for (i = 0; i < n; i++)
        if (MEM32(race + 0xC4u + 4u * i) == rider) { rec.idx = (uint8_t)i; break; }
    if (rec.idx == 0xFF) { rec.flags |= NPCL_F_IDX_UNKNOWN; s_np.idx_unknown++; }
    else {
        if (s_np.seen_mask & (1u << rec.idx)) {
            s_np.tick++;
            s_np.seen_mask = 0;
            if (s_np.tick % 60u == 0) fflush(s_np.f);
            if (s_np.tick % 600u == 0) np_summary("bilan");
        }
        s_np.seen_mask |= 1u << rec.idx;
    }

    rec.tick       = s_np.tick;
    rec.race_frame = rf;
    rec.present    = d3d8_PresentSeq();
    rec.kind       = kind;
    rec.race_state = (uint8_t)MEM32(race + 0x1Cu);
    rec.cmd        = pcmd ? MEM32(pcmd) : 0;
    for (i = 0; i < 3; i++) {
        rec.pos[i] = MEMF(rider + 0x170u + 4u * i);
        rec.vel[i] = MEMF(rider + 0x180u + 4u * i);
    }
    rec.ev_state     = MEM32(rider + 0x458u);
    rec.speed_factor = MEMF(rider + 0x15Cu);
    m = MEM32(rider + 0x58E0u);
    if (m >= 0x1000u && MEM32(m + 0x30u) >= 0x1000u) {
        /* exactement comme 0x31640 : [m + [[m+0x30]+4] + 0x488] */
        key = m + MEM32(MEM32(m + 0x30u) + 4u) + 0x488u;
        rec.phys_mode = MEM32(key);
        if (key == rider + 0x458u) rec.flags |= NPCL_F_MODE_IS_458;
    } else {
        rec.phys_mode = 0xFFFFFFFFu;
    }
    fwrite(&rec, sizeof rec, 1, s_np.f);
    if (s_np.st && kind == NPCL_KIND_PLAYER && rec.idx != 0xFF) {
        npst_record sr;
        sr.race_frame = rf;
        sr.race_state = MEM32(race + 0x1Cu);
        fwrite(&sr, sizeof sr, 1, s_np.st);
        fwrite((const void *)XBOX_PTR(rider), NPST_BLOCK, 1, s_np.st);
        s_np.st_blocks++;
    }
    s_np.rec_file++;
    s_np.rec_total++;
}

static void np_check_esp(uint32_t esp0, uint32_t popped)
{
    if (g_esp != esp0 + 4u + popped) InterlockedIncrement(&s_esp_bad);
}

/* Avant l'original : ghost (réécriture RNG à l'état 3) puis journal (états
 * RNG) -- le journal voit donc les valeurs forcées. */
static void np_before(void)
{
    uint32_t sv_eax = g_eax, sv_ecx = g_ecx, sv_edx = g_edx;
    if (g_np_ghost_on) np_ghost_before();
    if (g_np_cmdlog_on) {
        EnterCriticalSection(&s_lock);
        np_before_locked();
        LeaveCriticalSection(&s_lock);
    }
    g_eax = sv_eax; g_ecx = sv_ecx; g_edx = sv_edx;
}

static void np_after(uint32_t rider, uint32_t pcmd, uint8_t kind)
{
    uint32_t sv_eax = g_eax, sv_ecx = g_ecx, sv_edx = g_edx;
    if (!g_np_cmdlog_on) return;
    EnterCriticalSection(&s_lock);
    np_note(rider, pcmd, kind);
    LeaveCriticalSection(&s_lock);
    g_eax = sv_eax; g_ecx = sv_ecx; g_edx = sv_edx;
}

/* ── 0x5BEB0 : commande du Player (thiscall, 1 arg, ret 4) ── */
static void hook_5BEB0(void)
{
    uint32_t rider = g_ecx, esp0 = g_esp, pcmd = MEM32(g_esp + 4u);
    np_before();
    g_ecx = rider;
    sub_0005BEB0();
    np_check_esp(esp0, 4);
    if (g_np_ghost_on) {
        uint32_t sv_eax = g_eax, sv_ecx = g_ecx, sv_edx = g_edx;
        np_ghost_player_after(rider, pcmd);
        g_eax = sv_eax; g_ecx = sv_ecx; g_edx = sv_edx;
    }
    np_after(rider, pcmd, NPCL_KIND_PLAYER);
}

/* ── 0x48C40 : commande d'un OtherRider (thiscall, 1 arg, ret 4) ── */
static void hook_48C40(void)
{
    uint32_t rider = g_ecx, esp0 = g_esp, pcmd = MEM32(g_esp + 4u);
    np_before();
    if (g_np_ghost_on && np_ghost_take(rider, pcmd)) {
        /* Le ghost remplace le générateur IA : on fait ce que ferait
         * `ret 4` (retour fictif + argument dépilés), sans appeler l'original. */
        g_esp = esp0 + 8u;
    } else {
        g_ecx = rider;
        sub_00048C40();
    }
    np_check_esp(esp0, 4);
    np_after(rider, pcmd, NPCL_KIND_AI);
}

/* Hooks de commande : installés si le journal OU le ghost est actif.
 * Le ghost a ses propres hooks (chargement, roster) via np_ghost_lookup. */
void (*np_cmdlog_lookup(uint32_t xbox_va))(void)
{
    if (xbox_va == 0x0005BEB0u) return hook_5BEB0;
    if (xbox_va == 0x00048C40u) return hook_48C40;
    if (g_np_ghost_on) return np_ghost_lookup(xbox_va);
    return 0;
}

void np_cmdlog_init(void)
{
    const char *e = getenv("XBOX_NETLOG");
    char txt[MAX_PATH];
    SYSTEMTIME t;

    InitializeCriticalSection(&s_lock);
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "off")) return;
    if (strcmp(e, "1")) {
        fprintf(stderr, "[NETLOG] XBOX_NETLOG=%s inconnu (0 | 1) : off\n", e);
        return;
    }
    e = getenv("XBOX_NETLOG_DIR");
    if (e && *e) snprintf(s_np.dir, sizeof s_np.dir, "%s", e);
    else np_default_dir(s_np.dir, sizeof s_np.dir);
    CreateDirectoryA(s_np.dir, NULL);
    GetLocalTime(&t);
    snprintf(s_np.stamp, sizeof s_np.stamp, "%04u%02u%02u_%02u%02u%02u",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (snprintf(txt, sizeof txt, "%s\\cmdlog_%s.txt", s_np.dir, s_np.stamp) < (int)sizeof txt)
        s_np.txt = fopen(txt, "w");
    if (!s_np.txt) {
        fprintf(stderr, "[NETLOG] impossible d'ouvrir le bilan dans %s : off\n", s_np.dir);
        return;
    }
    fprintf(s_np.txt, "# np_cmdlog : un .npcl par course ; bilans ci-dessous\n");
    fflush(s_np.txt);
    e = getenv("XBOX_NETLOG_STATE");
    s_np.state_on = e && e[0] == '1';
    fprintf(stderr, "[NETLOG] journal des commandes -> %s%s\n", s_np.dir,
            s_np.state_on ? " (+ instantanes .npst)" : "");
    g_np_cmdlog_on = 1;
}
