/*
 * np_net -- online multiplayer, step 5: two instances over UDP.
 *
 * Two instances of the game (same PC on 127.0.0.1 for this step) run the
 * same race. Each one owns its Player; on the other side, that Player is
 * driven by an AI slot (OtherRider) fed with its commands and corrected by
 * its state snapshots. Each player stays full screen on their own machine.
 *
 * ── Switch (environment variables, read once at start)
 *
 *   XBOX_NET unset / "0"    OFF (default): installs no hook.
 *   XBOX_NET=host[:port]    host: listens on 127.0.0.1:port (default 45210).
 *   XBOX_NET=join:IP:port   guest: connects to the host. IP limited to
 *                           127.0.0.1 in this step (refused otherwise).
 *   XBOX_NET_SLOT=k         (host) roster index of the remote rider (an AI
 *                           slot of the host). Default: the one farthest
 *                           from the host's human on the grid (0 or n-1).
 *   XBOX_NET_HZ=20          state snapshots per second (60/HZ ticks).
 *   XBOX_NET_TIMEOUT=3      seconds without a packet in a race -> peer lost,
 *                           the AI takes the slot back until the race ends.
 *   XBOX_NET_WAIT=120       maximum wait in seconds (roster, barrier);
 *                           beyond it: solo for this race.
 *   XBOX_NET_EXTRAP=10      maximum age (ticks) of a snapshot applied late
 *                           (position extrapolated from the velocity).
 *   XBOX_NET_PACE=1         shared pacing: at the start of its tick k, each
 *                           instance waits (100 ms at most) until it has the
 *                           peer's tick k-PACE; 0 = off. Without it, a slowed
 *                           instance (the timer only catches up ~2 ticks)
 *                           stays behind for good (12 ticks on a loaded
 *                           laptop). Suspended after 10 waits in a row
 *                           (resumed on the peer's next tick); stopped when
 *                           the peer finishes its race (END message sent at
 *                           the start of its replay).
 *   XBOX_NET_SNAP_FALLS=1   a snapshot every tick while +0x454 is neither 1
 *                           (air) nor 2 (ground): falls, reset; 0 = off.
 *   XBOX_NET_FORCE_TRACK=1  (guest) the host's track [0x1DEC90] and mode
 *                           [0x1DEC94] forced at load; 0 = different track
 *                           -> solo (and the host is told: NOGO).
 *   XBOX_NET_TRACK=id       (host, test) track [0x1DEC90] forced at load
 *                           (scripted menus lose presses under load).
 *   XBOX_NET_LOG=<folder>   binary measurement log npnet_<role>_<pid>.bin
 *                           (see np_net_logrec); tool port/tools/np_net_cmp.py.
 *   Exclusive with XBOX_GHOST (the ghost is off when XBOX_NET is active).
 *
 * ── Course of a race
 *   1. Load (0xAC9B0 InGameState_LoadLevel, BEFORE the original): the host
 *      publishes its roster (0x1DE900, n x 0x98), [0x1DEC9C], the track
 *      [0x1DEC90], the index H of its human and the remote slot R. The
 *      guest waits for it, refuses a different track (solo), copies the
 *      roster, gives entry R to port 0 (its human) and turns entry H into an
 *      AI (the host's copy), copies [0x1DEC9C]. Mirrored rosters.
 *   2. 0x2E040 Race_ResetPlayerRoster (BEFORE the original): [0x1DEC98]
 *      published by the host, copied by the guest.
 *   3. Barrier: on the first command call of the first tick of state 3
 *      (countdown), both instances block their game thread until they see
 *      each other; the host releases with its RNG A / B states, copied by
 *      the guest. The game timer only catches up ~2 ticks after a block
 *      (bounded at -2 x target): the block freezes time.
 *      Shared race tick k = race+0x18 - (race+0x18 at the barrier).
 *   3b. GO barrier: on the first tick of state 4, the same meeting (AT4
 *      message repeated, 2 s at most): the instance that renders slowly
 *      runs its ticks in bursts; without it, GO showed up to ~9 frames
 *      later there.
 *   4. Race: on every tick, after its Player's command (0x5BEB0), each
 *      instance sends TICK(k, last 8 commands) and, every 60/HZ ticks or
 *      when +0x458 changes, the "rec" snapshot (88 bytes: +0x170..+0x1BF,
 *      +0x454, +0x458). On the other side, the copy slot (0x48C40) does not
 *      run the AI: command of tick k if received, else the last one
 *      received; then the newest snapshot not yet applied (tick j <= k,
 *      k-j <= EXTRAP; position + velocity x (k-j)/60); rider+0x15C = 1.0
 *      (as the ghost does).
 *   5. Peer lost (TIMEOUT without a packet, or BYE on close): the slot
 *      becomes a normal AI again (the original is called), no blocking.
 *
 * ── Transport: UDP, one network thread (receive + 5 Hz heartbeat) that
 *   NEVER writes game memory: it fills buffers guarded by a lock, read by
 *   the game thread in the hooks. The guest PULLS the sync data (REQ_SETUP /
 *   REQ_SEED / REQ_GO repeated, the host answers when it has the data):
 *   losses and arrival order have no effect.
 *
 * ── Known limits (step 5)
 *   The guest takes the character of the host's entry R. The remote rider
 *   stays an OtherRider (HUD / results show "CPU"). Pause is not synced.
 *   The AIs of each instance diverge (RNG A). Collisions with the copy of
 *   the remote rider are not filtered.
 */
#ifndef NP_NET_H
#define NP_NET_H

#include <stdint.h>

extern int g_np_net_on;

void np_net_init(void);
void (*np_net_lookup(uint32_t xbox_va))(void);
/* Called by the command hooks (np_cmdlog.c), before the original. */
void np_net_before(void);
/* 1 if the call is the remote slot's and the command was written. */
int  np_net_take(uint32_t rider, uint32_t pcmd);
/* Player hook, after the original: publishes the command and the snapshot. */
void np_net_player_after(uint32_t rider, uint32_t pcmd);

/* Measurement log (XBOX_NET_LOG): header 'NPNL', version, role (0 host,
 * 1 guest), record size; then one record per command call of the local
 * Player (kind 0, position after the original) and of the copy slot
 * (kind 1, position BEFORE correction = what is displayed). */
#define NPNL_MAGIC 0x4C4E504Eu   /* "NPNL" */
#pragma pack(push, 1)
typedef struct {
    uint32_t load;          /* load number (race) */
    int32_t  k;             /* shared tick (-1 before the barrier) */
    uint8_t  kind;          /* 0 = local Player, 1 = copy of the remote one */
    uint8_t  state;         /* race+0x1C */
    uint8_t  flags;         /* NPNL_F_* */
    uint8_t  slot;          /* index race+0xC4[] */
    float    pos[3];        /* +0x170 */
    double   qpc_ms;        /* absolute QPC clock (same PC: comparable) */
    int32_t  remote_k;      /* last tick received from the peer */
    uint16_t snap_age;      /* k - j of the applied snapshot (copy), 0xFFFF = none */
    uint8_t  ev;            /* +0x458 (RiderEvent state; copy: before correction) */
    uint8_t  mode;          /* +0x454 (physics mode) */
} np_net_logrec;
#pragma pack(pop)
#define NPNL_F_CMD_EXACT  0x01u   /* command of tick k received */
#define NPNL_F_CMD_HELD   0x02u   /* previous command repeated */
#define NPNL_F_SNAP       0x04u   /* snapshot applied */
#define NPNL_F_AI         0x08u   /* slot handed back to the AI (peer lost / solo) */
#define NPNL_F_BARRIER    0x10u   /* first tick after the barrier */

#endif /* NP_NET_H */
