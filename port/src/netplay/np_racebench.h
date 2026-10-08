/*
 * np_racebench -- replayed race for benchmarks.
 *
 * Replays a recorded reference race: the commands of ALL the riders are
 * imposed on every tick and the riders are snapped back onto the recorded
 * trajectory (state corrections, 20 Hz by default). Two runs of the same
 * reference then show the same race tick by tick (riders, hence race camera,
 * HUD and view), so FPS can be compared per tick between runs and no rider
 * gets stuck if the reference did not. BENCH ONLY: the game state is written
 * by the tool. Off by default: without XBOX_RACEBENCH none of its hooks is
 * installed and nothing below runs.
 *
 * ── Recording a reference (np_cmdlog, no replay)
 *   XBOX_NETLOG=1 XBOX_NETLOG_STATE=rec
 *     next to each .npcl, a .nprs file: header (magic 'NPRS', version, record
 *     size, set size), then 96-byte records (struct nprs_record): on every
 *     rider command call (after the original, same point as the .npcl
 *     record) the "rec" set of that rider: +0x170..+0x1BF (position,
 *     velocity, progress, quaternion, heading) and +0x454..+0x45B (physics
 *     mode, RiderEvent state); on the Player's call, one extra record
 *     (idx NPRS_IDX_VIEW) with the race camera view block 0 of InGameState
 *     (+0xB0..+0x107: eye, view matrix, projection words). ~40 KB/s.
 *
 * ── Replay (switch read once at start)
 *   XBOX_RACEBENCH unset      OFF (default).
 *   XBOX_RACEBENCH=<f.npcl>   replays the FIRST race of the session from
 *                             f.npcl (commands, roster, seeds, RNG) and f.nprs
 *                             (state corrections, camera reference).
 *   XBOX_RACEBENCH_LOG=<f>    per-tick CSV (default: <f.npcl>.<pid>.rbt.csv):
 *                             reference tick, race+0x18, race state,
 *                             d3d8_PresentSeq, QPC time (us), position gap of
 *                             each rider BEFORE this tick's correction, flag of
 *                             correction, camera gaps (eye distance, largest
 *                             view-matrix element difference).
 *   XBOX_RACEBENCH_HZ=20      periodic corrections per second (reference
 *                             ticks that are multiples of 60/HZ); 0 = none.
 *   XBOX_RACEBENCH_THR=20     also correct a rider whose gap exceeds this
 *                             (units); 0 = off.
 *   XBOX_RACEBENCH_JUMP=39.4  "visible jump" threshold of the summary (1 m).
 *   XBOX_RACE_PADMUTE=1       port-0 pad reads as released during race states
 *                             3 and 4 (countdown, race). Always on in replay;
 *                             set it when recording a reference too: the game
 *                             reads the pad outside the command hook (start
 *                             push 0x2D65B), and timed presses land on other
 *                             ticks on every run.
 *   XBOX_RACEBENCH_SHOTS=<prefix> + XBOX_RACEBENCH_SHOT_TICKS=T,A-B,...
 *                             internal screenshot (scene target, PNG,
 *                             <prefix>tNNNNN.png) requested at the start of
 *                             each listed reference tick of the race (state 4);
 *                             saved at the next Present (the game waits
 *                             for the previous one first). Up to 32 items.
 *   XBOX_RACEBENCH_PAUSE=T    at reference tick T (state 4): the replay ends
 *                             (pad live again) and START is pressed: the game
 *                             stays paused on that tick (pump_ident).
 *   XBOX_RACEBENCH_CAM=1      also copy the recorded camera view block (same
 *                             periodicity). Default 0: camera measured only.
 *
 * ── What it does (hooks shared with np_cmdlog / np_ghost)
 *   0x000AC9B0  LoadLevel, BEFORE the original: roster 0x1DE900 copied raw
 *               from the .npcl (same riders, same human index) and
 *               [0x1DEC9C]; first race only.
 *   0x0002E040  Race_ResetPlayerRoster, BEFORE the original: [0x1DEC98].
 *   0x0005BEB0 / 0x00048C40  command hooks of np_cmdlog: the ORIGINAL is
 *               called (the AI keeps its CPU cost and its internal state),
 *               then the command word and rider+0x15C are overwritten with the
 *               recorded ones of that rider index, then the correction.
 *   RNG A/B forced at the start of state 3 (as np_ghost).
 *   Tick alignment per race state, as np_ghost (the intro fly-over length
 *   varies): ref tick = r_start[state] + (race+0x18 - g_start[state]).
 *   End: no recorded tick left, state 5, or a new race object ->
 *   "[RACEBENCH] END ..." line (the tool stops the run on it).
 *
 * ── Stuck watchdog, usable without the replay
 *   XBOX_STUCK_BACK=<s>       when the Player moves slower than 2 m/s for s
 *                             seconds in the race, BACK (respawn) is pressed.
 *                             XBOX_STUCK_SPEED=<m/s> changes the 2 m/s.
 *                             Installs the command hooks; off by default.
 *
 * ── Limits
 *   Crowd, particles, the RNG A draws of other threads and the number of
 *   renders per tick stay free: images are not identical at the pixel level
 *   between runs. The menus up to the race are not replayed (timed presses).
 *   Exclusive with XBOX_GHOST and XBOX_NET (racebench wins).
 */
#ifndef NP_RACEBENCH_H
#define NP_RACEBENCH_H

#include <stdint.h>

#define NPRS_MAGIC     0x5352504Eu   /* "NPRS" */
#define NPRS_VERSION   1u
#define NPRS_SET       88u           /* +0x170 (0x50) + +0x454 (0x08) */
#define NPRS_IDX_VIEW  0xFDu
#define NPRS_VIEW_OFF  0xB0u         /* InGameState view block 0 */

#pragma pack(push, 1)
typedef struct {
    uint32_t race_frame;    /* race+0x18 */
    uint8_t  race_state;    /* race+0x1C */
    uint8_t  idx;           /* rider index in race+0xC4[], or NPRS_IDX_VIEW */
    uint16_t pad;
    uint8_t  data[NPRS_SET];
} nprs_record;
#pragma pack(pop)

typedef char nprs_record_size_check[sizeof(nprs_record) == 96 ? 1 : -1];

extern int g_np_rb_on;

void np_rb_init(void);
void (*np_rb_lookup(uint32_t xbox_va))(void);
/* Called by the command hooks before the original (first call of a tick). */
void np_rb_before(void);
/* Called by the command hooks after the original. */
void np_rb_after(uint32_t rider, uint32_t pcmd, int is_player);
/* Stuck watchdog (XBOX_STUCK_BACK), called by the Player command hook. */
extern int g_np_stuck_on;
void np_stuck_init(void);
void np_stuck_player(uint32_t rider);
/* 1 while the port-0 pad must read as released (XInputGetState). */
int  np_rb_pad_muted(void);
/* Recording side (np_cmdlog): fills one record of rider `rider`. */
void nprs_fill_rider(nprs_record *r, uint32_t rider);
int  nprs_fill_view(nprs_record *r);

#endif /* NP_RACEBENCH_H */
