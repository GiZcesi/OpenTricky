/*
 * np_ghost -- local ghost replayed from recorded commands.
 *
 * Runs, in an AI slot, a rider driven by the Player commands of an earlier
 * race, read from an np_cmdlog log (.npcl). No network: this is step 2b of
 * multiplayer.
 *
 * ── Switch (environment variables, read once at start)
 *
 *   XBOX_GHOST unset       OFF (default): installs none of the hooks (the
 *                          command hooks are installed only if
 *                          XBOX_NETLOG=1).
 *   XBOX_GHOST=<f.npcl>    ghost active for the FIRST race of the session.
 *   XBOX_GHOST_SLOT=k      index (race+0xC4[], = roster index) of the driven
 *                          AI slot. Default: the index of the recorded
 *                          Player (same place on the grid).
 *   XBOX_GHOST_HUMAN=k     roster index given to the human player. Default:
 *                          the one farthest from the ghost on the grid
 *                          (0 or n-1).
 *   XBOX_GHOST_FORCE=none  forces no equality condition (ablation).
 *   XBOX_GHOST_HUMAN_REPLAY=1  the human Player also replays the command and
 *                          the +0x15C of the AI recorded at its index: no
 *                          rider differs from the recording any more (test
 *                          that the whole race reproduces).
 *   Combine with XBOX_NETLOG=1 to log the ghost's race and compare it
 *   (port/tools/np_ghost_cmp.py).
 *
 * ── Format read: .npcl v2 or v3 (port/src/netplay/np_cmdlog.h)
 *   Header: track, mode, seeds, roster. Player records (kind 0): command per
 *   race+0x18 and race state. v3: RNG states at the start of each race state
 *   (kind 2/3) -- without them (v2), the RNG state is not forced.
 *
 * ── Hooks (all reached only through tables -> lookup_manual sees them)
 *   0x00048C40  AI command (thiscall, 1 arg, ret 4), via np_cmdlog.c: for
 *               the ghost slot, the AI generator is NOT called; the hook
 *               writes the recorded command and pops like `ret 4`. The other
 *               slots call the original. rider+0x15C forced to 1.0 (the
 *               constant value of a Player).
 *   0x0005BEB0  Player command: only the "before" point (RNG).
 *   0x000AC9B0  InGameState_LoadLevel (thiscall, 0 arg, ret; table 0x19A500)
 *               : BEFORE the original, roster 0x1DE900 (n x 0x98) copied from
 *               the .npcl, the ghost's entry turned AI (+0x7D = -1), the
 *               human's entry given port 0 (+0x7D = 0); [0x1DEC9C] (seed read
 *               at 0xACA97) copied.
 *   0x0002E040  Race_ResetPlayerRoster (thiscall, ret 8; table 0x19A580):
 *               BEFORE the original, [0x1DEC98] (seed of the "race" RNG, read
 *               at 0x2E17A) copied.
 *   RNG: on the first command call of the first tick of state 3 (countdown),
 *   before the original, both RNG states (0x1FAD70, 0x1FAD88) get those of
 *   the .npcl at the same point (v3 only).
 *
 * ── Tick alignment
 *   The intro fly-over (state 1) does not always last the same: the original
 *   tick is r_start[state] + (race+0x18 - g_start[state]), where r_start and
 *   g_start are the first race+0x18 of each state in the recorded race and in
 *   the current race.
 *
 * ── End and fallback
 *   No recorded command for the tick (end of the recording, state missing)
 *   -> fallback to the AI generator for that tick. After state 5 (EndRace),
 *   a replay (race+0x18 going back) or a new race: ghost done, the slot
 *   becomes a normal AI again.
 *
 * ── State corrections -- all OFF without XBOX_GHOST_STATE
 *   Original race recorded with XBOX_NETLOG=1 XBOX_NETLOG_STATE=1: a .npst
 *   next to the .npcl (Rider sub-object of the Player, 0x5A00 bytes per tick).
 *   XBOX_GHOST_STATE=<f.npst>  turns on the ghost corrections: when the
 *                          command is requested (np_ghost_take), the chosen
 *                          ranges of the original tick's snapshot are copied
 *                          into the ghost.
 *   XBOX_GHOST_CORR_HZ=20  frequency (original ticks that are multiples of
 *                          60/HZ; 0 = on threshold only). Default 20.
 *   XBOX_GHOST_CORR_THR=20 also correct as soon as the position gap exceeds
 *                          this threshold (units; contact). 0 = off.
 *   XBOX_GHOST_CORR_SET    ranges: "rec" (default) = +0x170..+0x1BF
 *                          (position, velocity, progress, quaternion,
 *                          heading) + +0x454 (physics mode) + +0x458
 *                          (RiderEvent state); "body", "pos", "kin",
 *                          "kinrot", "kinrot458", or "off:len,..." (hex).
 *                          DO NOT write +0x1C0..+0x3FF: the ghost gets stuck
 *                          falling.
 *   XBOX_GHOST_CORR_STATE=4  race state in which to correct (4 = Race; 0 = all).
 *   XBOX_GHOST_CORR_SAMEEV=1 correct only if the ghost's +0x458 = original
 *                          (default 0: worse when on).
 *   XBOX_GHOST_CORR_JUMP=39.4 threshold of a "visible jump" in the summary (1 m).
 *   Summary [GHOST] corrections: applied, on threshold, jumps per +0x458 state.
 *
 * ── Known limits
 *   The Players' start push is read straight from the pad (0x2D65B): the
 *   ghost does not get it. The skipped AI no longer updates its difficulty
 *   (0x481A0) nor its internal state; on fallback it starts again from that
 *   frozen state. The human player takes the character of the roster entry
 *   it is given.
 */
#ifndef NP_GHOST_H
#define NP_GHOST_H

#include <stdint.h>

extern int g_np_ghost_on;

void np_ghost_init(void);
void (*np_ghost_lookup(uint32_t xbox_va))(void);
/* Called by the command hooks (np_cmdlog.c), before the original. */
void np_ghost_before(void);
/* 1 if the call is the ghost slot's and the command was written. */
int  np_ghost_take(uint32_t rider, uint32_t pcmd);
/* XBOX_GHOST_HUMAN_REPLAY: called by the Player hook after the original. */
void np_ghost_player_after(uint32_t rider, uint32_t pcmd);

#endif /* NP_GHOST_H */
