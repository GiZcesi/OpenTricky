/*
 * np_net -- online multiplayer, step 5: two instances over UDP.
 * See np_net.h for the switch, the course of a race, the transport and the limits.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

#include "../recomp/recomp_types.h"
#include "np_cmdlog.h"
#include "np_net.h"

extern void sub_000AC9B0(void);     /* InGameState_LoadLevel */
extern void sub_0002E040(void);     /* Race_ResetPlayerRoster */

int g_np_net_on = 0;

#define NN_MAGIC      0x544E504Eu   /* "NPNT" */
#define NN_PORT       45210
#define NN_ROSTER_VA  0x001DE900u
#define NN_ROSTER_N   0x001DE8FCu
#define NN_MAXR       NPCL_MAX_RIDERS
#define NN_RING       4096u         /* received commands, per tick */
#define NN_SNAPS      64u           /* received snapshots */
#define NN_SNAP_SZ    88u           /* +0x170..+0x1BF (0x50) + +0x454 (8) */
#define NN_NCMD       8u            /* commands repeated per packet */

enum { T_HELLO = 1, T_WELCOME, T_REQ_SETUP, T_SETUP, T_REQ_SEED, T_SEED, T_REQ_GO, T_GO, T_TICK, T_HB, T_BYE, T_NOGO, T_END, T_AT4, T_AT4_ACK };

#pragma pack(push, 1)
typedef struct { uint32_t magic; uint8_t type, pad; uint16_t load; } nn_hdr;
typedef struct {
    nn_hdr   h;
    uint32_t track, mode, seed9c, nroster, human, slot;
    uint8_t  roster[NN_MAXR * NPCL_ROSTER_STRIDE];
} nn_setup;
typedef struct { nn_hdr h; uint32_t seq, seed98; } nn_seed;
typedef struct { nn_hdr h; uint32_t rng_a[6], rng_b[6]; } nn_go;
typedef struct {
    nn_hdr   h;
    int32_t  k;
    uint8_t  state, has_snap; uint16_t pad;
    uint32_t cmd[NN_NCMD];      /* cmd[i] = command of tick k - i */
    int32_t  snap_k;
    uint8_t  snap[NN_SNAP_SZ];
} nn_tick;
#pragma pack(pop)
#define NN_TICK_NOSNAP ((int)offsetof(nn_tick, snap_k))

/* ── Configuration ── */
static struct {
    int      host;
    char     ip[64];
    int      port;
    uint32_t slot_req;          /* XBOX_NET_SLOT (host), NN_UNSET otherwise */
    uint32_t period;            /* ticks between two snapshots */
    double   timeout_ms, wait_ms;
    int32_t  extrap;
    int32_t  pace;              /* max lead (ticks) over the peer, 0 = off */
    int      snap_falls;        /* a snapshot every tick while falling / reset */
    int      force_track;       /* guest: host's track / mode forced */
    uint32_t track_req;         /* host, test: XBOX_NET_TRACK */
    FILE    *log;
} s_cfg;
#define NN_UNSET 0xFFFFFFFFu

/* ── Network (lock s_lk for all that follows) ── */
static SOCKET            s_sock = INVALID_SOCKET;
static CRITICAL_SECTION  s_lk;
static struct {
    struct sockaddr_in peer;
    int      have_peer, connected, bye;
    double   last_rx_ms;
    /* host: published data, served on request */
    nn_setup setup;   int setup_load;     /* -1 = rien */
    nn_seed  seed;    int seed_seq;
    nn_go    go;      int go_load;
    int      guest_ready_load;            /* REQ_GO received for this load */
    int      guest_nogo_load;             /* the guest plays this load solo */
    int      peer_end_load;               /* the peer ended its session for this load */
    int      peer_at4_load, my_at4_load;  /* GO barrier (first tick of state 4) */
    /* guest: received data */
    nn_setup r_setup; int r_setup_load;
    nn_seed  r_seed;  int r_seed_seq;
    nn_go    r_go;    int r_go_load;
    /* race: the peer's commands and snapshots */
    int      tick_load;                   /* load of the buffers below */
    int32_t  ck[NN_RING]; uint32_t cv[NN_RING];
    int32_t  latest_k;
    int32_t  sk[NN_SNAPS]; uint8_t sv[NN_SNAPS][NN_SNAP_SZ];
    unsigned long long tx_pk, tx_b, rx_pk, rx_b, rx_bad;
} s_n;

/* ── Race (game thread only) ── */
static struct {
    uint32_t race;              /* objet course courant */
    int      load;              /* load number (1, 2, ...) */
    int      seed_seq;          /* calls of 0x2E040 */
    int      solo;              /* no peer for this load */
    uint32_t human, slot;       /* index of the local human / of the copy slot */
    int      barrier_done, lost;
    uint32_t rf0;               /* race+0x18 at the barrier */
    uint32_t mycmd[NN_RING];    /* my commands, for redundancy */
    int32_t  last_snap_k;       /* last applied snapshot */
    uint32_t last_ev;           /* +0x458 of my Player on the previous tick */
    uint32_t held_cmd;
    double   t_barrier;
    unsigned long long n_exact, n_held, n_snap, n_extrap, n_ai, n_sent_snap;
    uint32_t paced_rf;          /* last paced tick */
    uint32_t last_rf;           /* race+0x18 at the last call (replay = going back) */
    unsigned long long n_pace_wait, n_pace_giveup; double pace_ms;
    int      pace_off, pace_fail_run;  /* pacing stopped (END) / give-ups in a row */
    int32_t  pace_susp_k;       /* peer tick at suspension */
    int      go4_done;          /* GO barrier passed */
    unsigned long long n_pace_susp;
    double   err_sum; unsigned long long err_n; float err_max;
} s_r;

static double nn_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static uint32_t nn_race_ptr(void)
{
    uint32_t app = MEM32(0x001E3C7Cu), lvl;
    if (app < 0x1000u || (lvl = MEM32(app + 0x72Cu)) < 0x1000u) return 0;
    return MEM32(lvl + 0x1Cu);
}

static void nn_send(const void *p, int len)
{
    struct sockaddr_in to;
    int ok;
    EnterCriticalSection(&s_lk);
    ok = s_n.have_peer;
    to = s_n.peer;
    LeaveCriticalSection(&s_lk);
    if (!ok || s_sock == INVALID_SOCKET) return;
    if (sendto(s_sock, (const char *)p, len, 0, (struct sockaddr *)&to, sizeof to) == len) {
        EnterCriticalSection(&s_lk);
        s_n.tx_pk++; s_n.tx_b += (unsigned)len;
        LeaveCriticalSection(&s_lk);
    }
}

static void nn_hdr_set(nn_hdr *h, int type, int load)
{
    h->magic = NN_MAGIC; h->type = (uint8_t)type; h->pad = 0; h->load = (uint16_t)load;
}

static void nn_send_small(int type, int load)
{
    nn_hdr h;
    nn_hdr_set(&h, type, load);
    nn_send(&h, sizeof h);
}

/* Receives a packet (network thread), lock held. The host's answers are
 * copied into *reply (sent after the lock). */
static int nn_rx_locked(const uint8_t *b, int len, const struct sockaddr_in *from, uint8_t *reply)
{
    const nn_hdr *h = (const nn_hdr *)b;
    int rlen = 0;
    if (len < (int)sizeof *h || h->magic != NN_MAGIC) { s_n.rx_bad++; return 0; }
    if (s_cfg.host) {
        if (!s_n.have_peer || h->type == T_HELLO) {
            if (h->type != T_HELLO) return 0;
            if (!s_n.have_peer) fprintf(stderr, "[NET] guest connected from %s:%u\n",
                                         inet_ntoa(from->sin_addr), ntohs(from->sin_port));
            s_n.peer = *from; s_n.have_peer = 1; s_n.connected = 1; s_n.bye = 0;
        } else if (from->sin_port != s_n.peer.sin_port || from->sin_addr.s_addr != s_n.peer.sin_addr.s_addr) {
            return 0;
        }
    } else {
        if (from->sin_port != s_n.peer.sin_port || from->sin_addr.s_addr != s_n.peer.sin_addr.s_addr) return 0;
    }
    s_n.rx_pk++; s_n.rx_b += (unsigned)len;
    s_n.last_rx_ms = nn_now_ms();
    switch (h->type) {
    case T_HELLO:
        nn_hdr_set((nn_hdr *)reply, T_WELCOME, 0); rlen = sizeof(nn_hdr);
        break;
    case T_WELCOME:
        if (!s_n.connected) fprintf(stderr, "[NET] connected to the host\n");
        s_n.connected = 1; s_n.bye = 0;
        break;
    case T_REQ_SETUP:
        if (s_n.setup_load == h->load) { memcpy(reply, &s_n.setup, sizeof s_n.setup); rlen = sizeof s_n.setup; }
        break;
    case T_REQ_SEED:
        if (len >= (int)sizeof(nn_seed) && s_n.seed_seq == (int)((const nn_seed *)b)->seq) {
            memcpy(reply, &s_n.seed, sizeof s_n.seed); rlen = sizeof s_n.seed;
        }
        break;
    case T_REQ_GO:
        s_n.guest_ready_load = h->load;
        if (s_n.go_load == h->load) { memcpy(reply, &s_n.go, sizeof s_n.go); rlen = sizeof s_n.go; }
        break;
    case T_SETUP:
        if (len == (int)sizeof(nn_setup)) { s_n.r_setup = *(const nn_setup *)b; s_n.r_setup_load = h->load; }
        break;
    case T_SEED:
        if (len == (int)sizeof(nn_seed)) { s_n.r_seed = *(const nn_seed *)b; s_n.r_seed_seq = (int)s_n.r_seed.seq; }
        break;
    case T_GO:
        if (len == (int)sizeof(nn_go)) { s_n.r_go = *(const nn_go *)b; s_n.r_go_load = h->load; }
        break;
    case T_TICK: {
        const nn_tick *t = (const nn_tick *)b;
        uint32_t i;
        if (len < NN_TICK_NOSNAP) break;
        if (h->load != s_n.tick_load) {
            /* new race on the peer's side: buffers reset */
            for (i = 0; i < NN_RING; i++) s_n.ck[i] = -1;
            for (i = 0; i < NN_SNAPS; i++) s_n.sk[i] = -1;
            s_n.latest_k = -1;
            s_n.tick_load = h->load;
        }
        for (i = 0; i < NN_NCMD; i++) {
            int32_t k = t->k - (int32_t)i;
            if (k < 0) break;
            s_n.ck[k & (NN_RING - 1)] = k;
            s_n.cv[k & (NN_RING - 1)] = t->cmd[i];
        }
        if (t->k > s_n.latest_k) s_n.latest_k = t->k;
        if (t->has_snap && len == (int)sizeof(nn_tick) && t->snap_k >= 0) {
            uint32_t at = (uint32_t)t->snap_k % NN_SNAPS;
            s_n.sk[at] = t->snap_k;
            memcpy(s_n.sv[at], t->snap, NN_SNAP_SZ);
        }
        break;
    }
    case T_AT4:
        s_n.peer_at4_load = h->load;
        if (s_n.my_at4_load == h->load) { nn_hdr_set((nn_hdr *)reply, T_AT4_ACK, h->load); rlen = sizeof(nn_hdr); }
        break;
    case T_AT4_ACK:
        s_n.peer_at4_load = h->load;
        break;
    case T_END:
        s_n.peer_end_load = h->load;
        break;
    case T_NOGO:
        s_n.guest_nogo_load = h->load;
        break;
    case T_BYE:
        fprintf(stderr, "[NET] the peer left (BYE)\n");
        s_n.bye = 1;
        break;
    default: break;
    }
    return rlen;
}

static DWORD WINAPI nn_thread(LPVOID unused)
{
    static uint8_t buf[4096], reply[4096];
    double next_hb = 0;
    (void)unused;
    for (;;) {
        fd_set rs;
        struct timeval tv = { 0, 50000 };
        double now;
        FD_ZERO(&rs); FD_SET(s_sock, &rs);
        if (select(0, &rs, 0, 0, &tv) > 0) {
            struct sockaddr_in from;
            int fl = sizeof from, n, rlen;
            n = recvfrom(s_sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                EnterCriticalSection(&s_lk);
                rlen = nn_rx_locked(buf, n, &from, reply);
                LeaveCriticalSection(&s_lk);
                if (rlen) nn_send(reply, rlen);
            }
        }
        now = nn_now_ms();
        if (now >= next_hb) {
            int connected;
            next_hb = now + 200.0;
            EnterCriticalSection(&s_lk);
            connected = s_n.connected;
            LeaveCriticalSection(&s_lk);
            if (!s_cfg.host && !connected) nn_send_small(T_HELLO, 0);
            else if (connected) nn_send_small(T_HB, 0);
        }
    }
    return 0;
}

static void nn_bye(void)
{
    if (g_np_net_on) { nn_send_small(T_BYE, s_r.load); nn_send_small(T_BYE, s_r.load); }
}

/* Bounded busy wait (game thread): returns 1 if cond() becomes true.
 * resend() is called again every 100 ms (guest requests). */
static int nn_peer_lost(void);
static int nn_wait(int (*cond)(void), void (*resend)(void), const char *what)
{
    double t0 = nn_now_ms(), next = 0, now;
    for (;;) {
        int ever;
        now = nn_now_ms();
        if (cond()) {
            fprintf(stderr, "[NET] %s: %.0f ms of waiting\n", what, now - t0);
            return 1;
        }
        EnterCriticalSection(&s_lk); ever = s_n.connected; LeaveCriticalSection(&s_lk);
        if (ever && nn_peer_lost()) {
            fprintf(stderr, "[NET] %s: peer lost (no packet for %.1f s or BYE) -> solo for this race\n",
                    what, s_cfg.timeout_ms / 1000.0);
            return 0;
        }
        if (now - t0 > s_cfg.wait_ms) {
            fprintf(stderr, "[NET] %s: nothing after %.0f s -> solo for this race\n", what, s_cfg.wait_ms / 1000.0);
            return 0;
        }
        if (resend && now >= next) { resend(); next = now + 100.0; }
        Sleep(1);
    }
}

/* ── Chargement : roster et [0x1DEC9C] (0xAC9B0, avant l'original) ── */
static int c_setup(void)
{
    int r;
    EnterCriticalSection(&s_lk); r = s_n.r_setup_load == s_r.load; LeaveCriticalSection(&s_lk);
    return r;
}
static void q_setup(void) { nn_send_small(T_REQ_SETUP, s_r.load); }

static void nn_load_host(void)
{
    uint32_t n = MEM32(NN_ROSTER_N), i, h = NN_UNSET, nh = 0;
    nn_setup *s = &s_n.setup;
    if (n > NN_MAXR) n = NN_MAXR;
    if (s_cfg.track_req != NN_UNSET) {
        /* XBOX_NET_TRACK (test): track forced at load, same write as the
         * track forced on the guest (before the original). */
        fprintf(stderr, "[NET] load %d (host): XBOX_NET_TRACK: track %u -> %u\n", s_r.load,
                MEM32(0x001DEC90u), s_cfg.track_req);
        MEM32(0x001DEC90u) = s_cfg.track_req;
    }
    for (i = 0; i < n; i++)
        if ((int8_t)MEM8(NN_ROSTER_VA + i * NPCL_ROSTER_STRIDE + 0x7Du) >= 0) { if (h == NN_UNSET) h = i; nh++; }
    if (h == NN_UNSET || nh != 1 || n < 2) {
        fprintf(stderr, "[NET] load %d: roster of %u entries, %u human(s): solo\n", s_r.load, n, nh);
        s_r.solo = 1;
        return;
    }
    s_r.human = h;
    s_r.slot = s_cfg.slot_req != NN_UNSET ? s_cfg.slot_req : (h < n / 2u ? n - 1u : 0u);
    if (s_r.slot >= n || s_r.slot == h) {
        fprintf(stderr, "[NET] remote slot %u invalid (roster %u, human %u): solo\n", s_r.slot, n, h);
        s_r.solo = 1;
        return;
    }
    EnterCriticalSection(&s_lk);
    nn_hdr_set(&s->h, T_SETUP, s_r.load);
    s->track = MEM32(0x001DEC90u); s->mode = MEM32(0x001DEC94u); s->seed9c = MEM32(0x001DEC9Cu);
    s->nroster = n; s->human = h; s->slot = s_r.slot;
    memset(s->roster, 0, sizeof s->roster);
    for (i = 0; i < n * NPCL_ROSTER_STRIDE; i++) s->roster[i] = MEM8(NN_ROSTER_VA + i);
    s_n.setup_load = s_r.load;
    LeaveCriticalSection(&s_lk);
    fprintf(stderr, "[NET] load %d (host): track=%u roster=%u human=%u remote slot=%u [0x1DEC9C]=0x%08X published\n",
            s_r.load, s->track, n, h, s_r.slot, s->seed9c);
}

static void nn_load_guest(void)
{
    nn_setup s;
    uint32_t i, track = MEM32(0x001DEC90u);
    if (!nn_wait(c_setup, q_setup, "host roster")) { s_r.solo = 1; return; }
    EnterCriticalSection(&s_lk); s = s_n.r_setup; LeaveCriticalSection(&s_lk);
    if (s.track != track && s_cfg.force_track && s.nroster <= NN_MAXR) {
        /* The host forces the track and the mode: written before the
         * original, which loads the track of [0x1DEC90]. */
        fprintf(stderr, "[NET] load %d: local track %u mode %u -> host track %u mode %u\n",
                s_r.load, track, MEM32(0x001DEC94u), s.track, s.mode);
        MEM32(0x001DEC90u) = s.track;
        MEM32(0x001DEC94u) = s.mode;
        track = s.track;
    }
    if (s.track != track || s.nroster > NN_MAXR || s.human >= s.nroster || s.slot >= s.nroster) {
        fprintf(stderr, "[NET] load %d: local track %u, host %u (roster %u, human %u, slot %u): solo\n",
                s_r.load, track, s.track, s.nroster, s.human, s.slot);
        s_r.solo = 1;
        nn_send_small(T_NOGO, s_r.load);
        nn_send_small(T_NOGO, s_r.load);
        return;
    }
    for (i = 0; i < s.nroster * NPCL_ROSTER_STRIDE; i++) MEM8(NN_ROSTER_VA + i) = s.roster[i];
    MEM8(NN_ROSTER_VA + s.human * NPCL_ROSTER_STRIDE + 0x7Du) = 0xFFu;   /* the host = driven AI */
    MEM8(NN_ROSTER_VA + s.slot * NPCL_ROSTER_STRIDE + 0x7Du) = 0x00u;    /* moi = port 0 */
    MEM32(NN_ROSTER_N) = s.nroster;
    MEM32(0x001DEC9Cu) = s.seed9c;
    s_r.human = s.slot;          /* mon index */
    s_r.slot = s.human;          /* the host's copy */
    fprintf(stderr, "[NET] load %d (guest): host roster copied (%u entries), me=%u (port 0), "
            "host=%u (driven AI), [0x1DEC9C]=0x%08X, track=%u\n", s_r.load, s.nroster, s_r.human, s_r.slot,
            s.seed9c, track);
}

static void hook_AC9B0(void)
{
    uint32_t ecx = g_ecx;
    int connected;
    s_r.load++;
    s_r.solo = 0;
    s_r.seed_seq = 0;
    s_r.race = 0;
    EnterCriticalSection(&s_lk); connected = s_n.connected; LeaveCriticalSection(&s_lk);
    if (s_cfg.host) nn_load_host();
    else nn_load_guest();
    (void)connected;
    g_ecx = ecx;
    sub_000AC9B0();
}

/* ── 0x2E040 : [0x1DEC98] (avant l'original) ── */
static int c_seed(void)
{
    int r;
    EnterCriticalSection(&s_lk); r = s_n.r_seed_seq == s_r.seed_seq; LeaveCriticalSection(&s_lk);
    return r;
}
static void q_seed(void)
{
    nn_seed q;
    memset(&q, 0, sizeof q);
    nn_hdr_set(&q.h, T_REQ_SEED, s_r.load);
    q.seq = (uint32_t)s_r.seed_seq;
    nn_send(&q, sizeof q);
}

static void hook_2E040(void)
{
    uint32_t ecx = g_ecx;
    s_r.seed_seq = (s_r.load << 8) + ((s_r.seed_seq + 1) & 0xFF);
    if (!s_r.solo) {
        if (s_cfg.host) {
            EnterCriticalSection(&s_lk);
            nn_hdr_set(&s_n.seed.h, T_SEED, s_r.load);
            s_n.seed.seq = (uint32_t)s_r.seed_seq;
            s_n.seed.seed98 = MEM32(0x001DEC98u);
            s_n.seed_seq = s_r.seed_seq;
            LeaveCriticalSection(&s_lk);
            fprintf(stderr, "[NET] Race_ResetPlayerRoster %d: [0x1DEC98]=0x%08X published\n", s_r.seed_seq & 0xFF,
                    MEM32(0x001DEC98u));
        } else if (nn_wait(c_seed, q_seed, "host [0x1DEC98]")) {
            uint32_t before = MEM32(0x001DEC98u), v;
            EnterCriticalSection(&s_lk); v = s_n.r_seed.seed98; LeaveCriticalSection(&s_lk);
            MEM32(0x001DEC98u) = v;
            fprintf(stderr, "[NET] Race_ResetPlayerRoster %d: [0x1DEC98] 0x%08X -> 0x%08X\n", s_r.seed_seq & 0xFF,
                    before, v);
        }
    }
    g_ecx = ecx;
    sub_0002E040();
}

void (*np_net_lookup(uint32_t xbox_va))(void)
{
    if (xbox_va == 0x000AC9B0u) return hook_AC9B0;
    if (xbox_va == 0x0002E040u) return hook_2E040;
    return 0;
}

/* ── Barrier (start of state 3) ── */
static int c_ready(void)
{
    int r;
    EnterCriticalSection(&s_lk); r = s_n.guest_ready_load == s_r.load || s_n.guest_nogo_load == s_r.load; LeaveCriticalSection(&s_lk);
    return r;
}
static int c_go(void)
{
    int r;
    EnterCriticalSection(&s_lk); r = s_n.r_go_load == s_r.load; LeaveCriticalSection(&s_lk);
    return r;
}
static void q_go(void) { nn_send_small(T_REQ_GO, s_r.load); }

static void nn_stats(const char *why)
{
    unsigned long long tp, tb, rp, rb;
    double dt = nn_now_ms() - s_r.t_barrier;
    EnterCriticalSection(&s_lk);
    tp = s_n.tx_pk; tb = s_n.tx_b; rp = s_n.rx_pk; rb = s_n.rx_b;
    LeaveCriticalSection(&s_lk);
    fprintf(stderr, "[NET] %s: load %d, copy: exact=%llu repeated=%llu snapshots=%llu (extrapolated %llu) "
            "AI=%llu; sent: %llu packets %llu B, received: %llu packets %llu B; send rate %.0f B/s since the "
            "barrier; gap before correction avg=%.1f max=%.1f; pacing: %llu waits (%.0f ms), %llu give-ups, %llu suspensions\n", why, s_r.load, s_r.n_exact, s_r.n_held,
            s_r.n_snap, s_r.n_extrap, s_r.n_ai, tp, tb, rp, rb, dt > 0 ? (double)tb * 1000.0 / dt : 0.0,
            s_r.err_n ? s_r.err_sum / (double)s_r.err_n : 0.0, s_r.err_max, s_r.n_pace_wait, s_r.pace_ms,
            s_r.n_pace_giveup, s_r.n_pace_susp);
    fflush(stderr);
}

static void nn_new_race(uint32_t race)
{
    s_r.race = race;
    s_r.barrier_done = 0;
    s_r.lost = 0;
    s_r.last_snap_k = -1;
    s_r.last_ev = NN_UNSET;
    s_r.held_cmd = 0;
    s_r.paced_rf = 0xFFFFFFFFu;
    s_r.last_rf = 0;
    s_r.n_pace_wait = s_r.n_pace_giveup = 0; s_r.pace_ms = 0;
    s_r.pace_off = s_r.pace_fail_run = 0; s_r.n_pace_susp = 0;
    s_r.go4_done = 0;
    s_r.n_exact = s_r.n_held = s_r.n_snap = s_r.n_extrap = s_r.n_ai = s_r.n_sent_snap = 0;
    s_r.err_sum = 0; s_r.err_n = 0; s_r.err_max = 0;
    EnterCriticalSection(&s_lk);
    s_n.tx_pk = s_n.tx_b = s_n.rx_pk = s_n.rx_b = 0;
    LeaveCriticalSection(&s_lk);
}

void np_net_before(void)
{
    uint32_t race = nn_race_ptr(), st, rf, i;
    if (!race) return;
    if (race != s_r.race) nn_new_race(race);
    if (s_r.solo) return;
    if (s_r.barrier_done) {
        /* Shared pacing: at the start of tick k, wait (100 ms at most) until
         * the peer's tick k - pace is here. Both instances do it: neither gets
         * more than `pace` ticks ahead, no deadlock. */
        int32_t k;
        rf = MEM32(race + 0x18u);
        if (rf < s_r.last_rf) {
            /* race+0x18 goes back: replay or restarted race. The replay
             * re-simulates from the recorded commands: nothing more is
             * injected or sent for this race. */
            nn_stats("race+0x18 went back (replay), session over for this race");
            s_r.solo = 1;
            nn_send_small(T_END, s_r.load);   /* the peer stops waiting for me */
            nn_send_small(T_END, s_r.load);
            return;
        }
        s_r.last_rf = rf;
        if (!s_r.go4_done && MEM32(race + 0x1Cu) == 4u && !s_r.lost) {
            /* GO barrier: on the first tick of state 4, each side says it is
             * there (repeated every 20 ms) and waits for the other (2 s at
             * most). Without it, the instance that renders slowly runs its
             * ticks in bursts and GO shows up to ~9 frames later. */
            double t0 = nn_now_ms(), next = 0, now;
            int ok = 0;
            s_r.go4_done = 1;
            EnterCriticalSection(&s_lk); s_n.my_at4_load = s_r.load; LeaveCriticalSection(&s_lk);
            for (;;) {
                now = nn_now_ms();
                if (now >= next) { nn_send_small(T_AT4, s_r.load); next = now + 20.0; }
                EnterCriticalSection(&s_lk);
                ok = s_n.peer_at4_load == s_r.load || s_n.peer_end_load == s_r.load;
                LeaveCriticalSection(&s_lk);
                if (ok || now - t0 > 2000.0) break;
                Sleep(0);
            }
            fprintf(stderr, "[NET] GO barrier (tick %d): %s after %.1f ms, QPC=%.3f ms\n", (int)(rf - s_r.rf0),
                    ok ? "peer present" : "timed out", nn_now_ms() - t0, nn_now_ms());
        }
        if (!s_cfg.pace || s_r.lost || s_r.pace_off || rf == s_r.paced_rf) return;
        s_r.paced_rf = rf;
        k = (int32_t)(rf - s_r.rf0);
        if (k > s_cfg.pace) {
            double t0 = nn_now_ms(), t;
            int ok = 0, waited = 0, ended;
            int32_t latest;
            EnterCriticalSection(&s_lk);
            ended = s_n.peer_end_load == s_r.load;
            latest = s_n.tick_load == s_r.load ? s_n.latest_k : -1;
            LeaveCriticalSection(&s_lk);
            if (ended) {
                /* the peer finished its race (replay): no more ticks to wait for */
                s_r.pace_off = 1;
                fprintf(stderr, "[NET] pacing stopped at tick %d: the peer finished its race (END)\n", k);
                return;
            }
            if (s_r.pace_fail_run >= 10) {
                /* suspended: resume as soon as a new peer tick arrives */
                if (latest == s_r.pace_susp_k) return;
                s_r.pace_fail_run = 0;
            }
            for (;;) {
                EnterCriticalSection(&s_lk);
                latest = s_n.tick_load == s_r.load ? s_n.latest_k : -1;
                ok = latest >= k - s_cfg.pace;
                LeaveCriticalSection(&s_lk);
                t = nn_now_ms() - t0;
                if (ok || t > 100.0) break;
                waited = 1;
                Sleep(0);
            }
            if (waited) { s_r.n_pace_wait++; s_r.pace_ms += t; }
            if (!ok) {
                s_r.n_pace_giveup++;
                /* 10 give-ups in a row (1 s): the peer sends no more ticks
                 * (results, pause, long stall) -> pacing suspended until its
                 * next tick (otherwise 100 ms lost per tick). */
                if (++s_r.pace_fail_run == 10) {
                    s_r.pace_susp_k = latest;
                    s_r.n_pace_susp++;
                    fprintf(stderr, "[NET] pacing suspended at tick %d (peer at tick %d): 10 waits of 100 ms without a tick\n",
                            k, latest);
                }
            } else {
                s_r.pace_fail_run = 0;
            }
        }
        return;
    }
    st = MEM32(race + 0x1Cu);
    if (st != 3u) return;
    rf = MEM32(race + 0x18u);
    if (s_cfg.host) {
        int nogo;
        if (!nn_wait(c_ready, 0, "barrier (guest ready)")) { s_r.solo = 1; return; }
        EnterCriticalSection(&s_lk); nogo = s_n.guest_nogo_load == s_r.load; LeaveCriticalSection(&s_lk);
        if (nogo) {
            fprintf(stderr, "[NET] load %d: the guest plays solo (NOGO) -> solo for this race\n", s_r.load);
            s_r.solo = 1;
            return;
        }
        EnterCriticalSection(&s_lk);
        nn_hdr_set(&s_n.go.h, T_GO, s_r.load);
        for (i = 0; i < 6; i++) {
            s_n.go.rng_a[i] = MEM32(NPCL_RNG_A_VA + 4u * i);
            s_n.go.rng_b[i] = MEM32(NPCL_RNG_B_VA + 4u * i);
        }
        s_n.go_load = s_r.load;
        LeaveCriticalSection(&s_lk);
        nn_send(&s_n.go, sizeof s_n.go);
    } else {
        nn_go g;
        if (!nn_wait(c_go, q_go, "barrier (host GO)")) { s_r.solo = 1; return; }
        EnterCriticalSection(&s_lk); g = s_n.r_go; LeaveCriticalSection(&s_lk);
        for (i = 0; i < 6; i++) {
            MEM32(NPCL_RNG_A_VA + 4u * i) = g.rng_a[i];
            MEM32(NPCL_RNG_B_VA + 4u * i) = g.rng_b[i];
        }
    }
    s_r.rf0 = rf;
    s_r.barrier_done = 1;
    s_r.t_barrier = nn_now_ms();
    EnterCriticalSection(&s_lk);
    s_n.last_rx_ms = s_r.t_barrier;
    LeaveCriticalSection(&s_lk);
    fprintf(stderr, "[NET] barrier passed: load %d, race+0x18=%u, QPC=%.3f ms%s\n", s_r.load, rf,
            s_r.t_barrier, s_cfg.host ? "" : " (host RNG A/B copied)");
}

static void nn_log(int kind, uint32_t rider, uint32_t race, int32_t k, uint8_t flags, uint32_t snap_age)
{
    np_net_logrec r;
    uint32_t i;
    if (!s_cfg.log) return;
    r.load = (uint32_t)s_r.load;
    r.k = k;
    r.kind = (uint8_t)kind;
    r.state = (uint8_t)MEM32(race + 0x1Cu);
    r.flags = flags;
    r.slot = (uint8_t)(kind ? s_r.slot : s_r.human);
    for (i = 0; i < 3; i++) r.pos[i] = MEMF(rider + 0x170u + 4u * i);
    r.qpc_ms = nn_now_ms();
    EnterCriticalSection(&s_lk); r.remote_k = s_n.tick_load == s_r.load ? s_n.latest_k : -1; LeaveCriticalSection(&s_lk);
    r.snap_age = (uint16_t)(snap_age > 0xFFFFu ? 0xFFFFu : snap_age);
    r.ev = (uint8_t)MEM32(rider + 0x458u);
    r.mode = (uint8_t)MEM32(rider + 0x454u);
    fwrite(&r, sizeof r, 1, s_cfg.log);
    if (k >= 0 && k % 300 == 0) fflush(s_cfg.log);
}

/* Pair perdu ? (verrou non tenu) */
static int nn_peer_lost(void)
{
    double last; int bye;
    EnterCriticalSection(&s_lk); last = s_n.last_rx_ms; bye = s_n.bye; LeaveCriticalSection(&s_lk);
    return bye || nn_now_ms() - last > s_cfg.timeout_ms;
}

int np_net_take(uint32_t rider, uint32_t pcmd)
{
    uint32_t race, rf, cmd = 0, i;
    int32_t k, best = -1;
    uint8_t flags = 0, snap[NN_SNAP_SZ];
    int have_cmd = 0, stale;
    if (!pcmd || s_r.solo || !s_r.barrier_done) return 0;
    race = nn_race_ptr();
    if (!race || race != s_r.race || s_r.slot >= MEM32(race + 0x88u)) return 0;
    if (rider != MEM32(race + 0xC4u + 4u * s_r.slot)) return 0;
    rf = MEM32(race + 0x18u);
    k = (int32_t)(rf - s_r.rf0);
    if (!s_r.lost && nn_peer_lost()) {
        s_r.lost = 1;
        fprintf(stderr, "[NET] peer lost at tick %d (no packet for %.1f s or BYE): the AI takes slot %u back\n",
                k, s_cfg.timeout_ms / 1000.0, s_r.slot);
        nn_stats("peer lost");
    }
    if (s_r.lost) {
        s_r.n_ai++;
        nn_log(1, rider, race, k, NPNL_F_AI, 0);
        return 0;
    }
    EnterCriticalSection(&s_lk);
    stale = s_n.tick_load != s_r.load || s_n.latest_k < k - 60;
    if (s_n.tick_load == s_r.load) {
        uint32_t at = (uint32_t)k & (NN_RING - 1);
        if (k >= 0 && s_n.ck[at] == k) { cmd = s_n.cv[at]; have_cmd = 1; }
        for (i = 0; i < NN_SNAPS; i++)
            if (s_n.sk[i] > s_r.last_snap_k && s_n.sk[i] <= k && s_n.sk[i] > best) best = s_n.sk[i];
        if (best >= 0) memcpy(snap, s_n.sv[(uint32_t)best % NN_SNAPS], NN_SNAP_SZ);
    }
    LeaveCriticalSection(&s_lk);
    if (stale && k > 60) {
        /* the peer has sent no ticks for 1 s (left the race, pause): AI for this tick */
        s_r.n_ai++;
        nn_log(1, rider, race, k, NPNL_F_AI, 0);
        return 0;
    }
    if (have_cmd) { s_r.held_cmd = cmd; flags |= NPNL_F_CMD_EXACT; s_r.n_exact++; }
    else { cmd = s_r.held_cmd; flags |= NPNL_F_CMD_HELD; s_r.n_held++; }
    if (best >= 0 && k - best > s_cfg.extrap) best = -1;    /* too old: ignored */
    nn_log(1, rider, race, k, (uint8_t)(flags | (best >= 0 ? NPNL_F_SNAP : 0)), best >= 0 ? (uint32_t)(k - best) : 0xFFFFFFFFu);
    MEM32(pcmd) = cmd;
    MEMF(rider + 0x15Cu) = 1.0f;
    if (best >= 0) {
        float p[3], v[3], d2 = 0, err;
        float dt = (float)(k - best) / 60.0f;
        memcpy(p, snap + 0x00, 12);
        memcpy(v, snap + 0x10, 12);
        for (i = 0; i < 3; i++) {
            float dv;
            p[i] += v[i] * dt;
            dv = MEMF(rider + 0x170u + 4u * i) - p[i];
            d2 += dv * dv;
        }
        err = sqrtf(d2);
        memcpy((void *)XBOX_PTR(rider + 0x170u), snap, 0x50);
        memcpy((void *)XBOX_PTR(rider + 0x454u), snap + 0x50, 8);
        for (i = 0; i < 3; i++) MEMF(rider + 0x170u + 4u * i) = p[i];
        s_r.last_snap_k = best;
        s_r.n_snap++;
        if (k != best) s_r.n_extrap++;
        s_r.err_sum += err; s_r.err_n++;
        if (err > s_r.err_max) s_r.err_max = err;
    }
    if (s_r.n_snap && s_r.n_snap % 600u == 0 && best >= 0) nn_stats("summary");
    return 1;
}

void np_net_player_after(uint32_t rider, uint32_t pcmd)
{
    uint32_t race, rf, ev, i;
    int32_t k;
    nn_tick t;
    int snap;
    if (!pcmd || s_r.solo || !s_r.barrier_done) return;
    race = nn_race_ptr();
    if (!race || race != s_r.race || s_r.human >= MEM32(race + 0x88u)) return;
    if (rider != MEM32(race + 0xC4u + 4u * s_r.human)) return;
    rf = MEM32(race + 0x18u);
    k = (int32_t)(rf - s_r.rf0);
    if (k < 0) return;
    s_r.mycmd[(uint32_t)k & (NN_RING - 1)] = MEM32(pcmd);
    nn_log(0, rider, race, k, k == 0 ? NPNL_F_BARRIER : 0, 0);
    if (s_r.lost) return;
    ev = MEM32(rider + 0x458u);
    /* Snapshot: every `period` ticks, on each change of +0x458, and on EVERY
     * tick off air / ground (+0x454 != 1, 2: fall 5 / 6, reset 4): fall
     * physics diverges within a few ticks. */
    {
        uint32_t mode = MEM32(rider + 0x454u);
        snap = (k % (int32_t)s_cfg.period) == 0 || ev != s_r.last_ev || (s_cfg.snap_falls && mode != 1u && mode != 2u);
    }
    s_r.last_ev = ev;
    memset(&t, 0, sizeof t);
    nn_hdr_set(&t.h, T_TICK, s_r.load);
    t.k = k;
    t.state = (uint8_t)MEM32(race + 0x1Cu);
    for (i = 0; i < NN_NCMD; i++)
        t.cmd[i] = (int32_t)i <= k ? s_r.mycmd[(uint32_t)(k - (int32_t)i) & (NN_RING - 1)] : 0;
    if (snap) {
        t.has_snap = 1;
        t.snap_k = k;
        memcpy(t.snap, (const void *)XBOX_PTR(rider + 0x170u), 0x50);
        memcpy(t.snap + 0x50, (const void *)XBOX_PTR(rider + 0x454u), 8);
        s_r.n_sent_snap++;
        nn_send(&t, sizeof t);
    } else {
        nn_send(&t, NN_TICK_NOSNAP);
    }
    if (k > 0 && k % 1200 == 0) nn_stats("summary");
}

void np_net_init(void)
{
    const char *e = getenv("XBOX_NET"), *v;
    WSADATA wd;
    struct sockaddr_in a;
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "off")) return;
    s_cfg.port = NN_PORT;
    snprintf(s_cfg.ip, sizeof s_cfg.ip, "127.0.0.1");
    if (!strncmp(e, "host", 4)) {
        s_cfg.host = 1;
        if (e[4] == ':') s_cfg.port = atoi(e + 5);
    } else if (!strncmp(e, "join:", 5)) {
        const char *c = strrchr(e + 5, ':');
        if (!c) { fprintf(stderr, "[NET] XBOX_NET=%s: join:IP:port expected: off\n", e); return; }
        snprintf(s_cfg.ip, sizeof s_cfg.ip, "%.*s", (int)(c - (e + 5)), e + 5);
        s_cfg.port = atoi(c + 1);
    } else {
        fprintf(stderr, "[NET] XBOX_NET=%s unknown (host[:port] | join:IP:port): off\n", e);
        return;
    }
    if (strcmp(s_cfg.ip, "127.0.0.1")) {
        fprintf(stderr, "[NET] step 5: 127.0.0.1 only (got %s): off\n", s_cfg.ip);
        return;
    }
    if (s_cfg.port <= 0 || s_cfg.port > 65535) { fprintf(stderr, "[NET] port %d invalid: off\n", s_cfg.port); return; }
    v = getenv("XBOX_NET_SLOT");    s_cfg.slot_req = (v && *v) ? (uint32_t)atoi(v) : NN_UNSET;
    v = getenv("XBOX_NET_HZ");      s_cfg.period = (v && atoi(v) > 0 && atoi(v) <= 60) ? 60u / (uint32_t)atoi(v) : 3u;
    v = getenv("XBOX_NET_TIMEOUT"); s_cfg.timeout_ms = 1000.0 * ((v && atof(v) > 0) ? atof(v) : 3.0);
    v = getenv("XBOX_NET_WAIT");    s_cfg.wait_ms = 1000.0 * ((v && atof(v) > 0) ? atof(v) : 120.0);
    v = getenv("XBOX_NET_EXTRAP");  s_cfg.extrap = (v && *v) ? atoi(v) : 10;
    v = getenv("XBOX_NET_PACE");    s_cfg.pace = (v && *v) ? atoi(v) : 1;
    v = getenv("XBOX_NET_TRACK");   s_cfg.track_req = (v && *v) ? (uint32_t)atoi(v) : NN_UNSET;
    v = getenv("XBOX_NET_FORCE_TRACK"); s_cfg.force_track = !(v && v[0] == '0');
    v = getenv("XBOX_NET_SNAP_FALLS"); s_cfg.snap_falls = !(v && v[0] == '0');

    InitializeCriticalSection(&s_lk);
    s_n.setup_load = s_n.seed_seq = s_n.go_load = s_n.guest_ready_load = s_n.guest_nogo_load = s_n.peer_end_load = -1;
    s_n.peer_at4_load = s_n.my_at4_load = -1;
    s_n.r_setup_load = s_n.r_seed_seq = s_n.r_go_load = s_n.tick_load = -1;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) { fprintf(stderr, "[NET] WSAStartup failed: off\n"); return; }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) { fprintf(stderr, "[NET] socket failed: off\n"); return; }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = htons((u_short)(s_cfg.host ? s_cfg.port : 0));
    if (bind(s_sock, (struct sockaddr *)&a, sizeof a)) {
        fprintf(stderr, "[NET] bind 127.0.0.1:%d failed (%d): off\n", s_cfg.host ? s_cfg.port : 0, WSAGetLastError());
        closesocket(s_sock); s_sock = INVALID_SOCKET;
        return;
    }
    if (!s_cfg.host) {
        s_n.peer.sin_family = AF_INET;
        s_n.peer.sin_addr.s_addr = inet_addr(s_cfg.ip);
        s_n.peer.sin_port = htons((u_short)s_cfg.port);
        s_n.have_peer = 1;
    }
    v = getenv("XBOX_NET_LOG");
    if (v && *v) {
        char p[MAX_PATH];
        uint32_t hdr[4] = { NPNL_MAGIC, 1u, (uint32_t)!s_cfg.host, (uint32_t)sizeof(np_net_logrec) };
        CreateDirectoryA(v, NULL);
        snprintf(p, sizeof p, "%s\\npnet_%s_%lu.bin", v, s_cfg.host ? "host" : "guest", (unsigned long)GetCurrentProcessId());
        s_cfg.log = fopen(p, "wb");
        if (s_cfg.log) { setvbuf(s_cfg.log, NULL, _IOFBF, 256 * 1024); fwrite(hdr, sizeof hdr, 1, s_cfg.log); }
        fprintf(stderr, "[NET] measurement log -> %s%s\n", p, s_cfg.log ? "" : " (open failed)");
    }
    CreateThread(NULL, 0, nn_thread, NULL, 0, NULL);
    atexit(nn_bye);
    fprintf(stderr, "[NET] active: %s 127.0.0.1:%d, snapshots every %u ticks, peer-lost delay %.1f s, max wait %.0f s\n",
            s_cfg.host ? "host, listening" : "guest ->", s_cfg.port, s_cfg.period, s_cfg.timeout_ms / 1000.0,
            s_cfg.wait_ms / 1000.0);
    g_np_net_on = 1;
}
