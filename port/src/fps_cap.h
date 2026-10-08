/*
 * fps_cap -- host frame pacing above 60.
 *
 * The game runs its logic ticks at 60 Hz (software timer 0xB26B0) and, in its
 * main loop 0xAA1A0, "N ticks then 1 render": never a render without a tick.
 * When no tick is due, it waits for the frame event
 * (XBoxExecutionMan_WaitForFrameEvent 0xB2750, called at 0xAA296).
 *
 * This module replaces that wait: while no tick is due, it calls the current
 * state's render again (state->vt+0x18) at the pace of the host cap. A render
 * without a tick gets dt = 0 (0xAB610: [state+0x4C] = ticks since the last
 * render) and does not touch the logic state (measured).
 *
 * Interpolation (XBOX_FPS_INTERP=1 by default when the cap != 60): every
 * InGameState render (normal or extra) shows lerp(tick N-1, tick N, alpha),
 * alpha = time since tick N was produced by the game's 60 Hz timer / 16.67 ms
 * (not since it was consumed: a tick consumed late, after a slow render or in
 * a catch-up, would make the motion slow down, jump, then freeze). Display is
 * one tick late (+16.7 ms latency); no extrapolation.
 *   camera: view block InGameState+0xB0 (+i*0x80, views < [+0x298]):
 *           linear position, view matrix rotation by slerp (determinant -1
 *           handled), linear projection +0x100/+0x104;
 *   riders: position +0x170 and pose +0x48B0 (21 4x4 matrices in WORLD
 *           space): linear translation, slerp rotation.
 *   State copied at the tick (hook 0xAD4A0); before a render, memory must
 *   equal the copy of tick N, else nothing is written; after the render, the
 *   copy of tick N is written back (bit exact). Whole riders restored as they
 *   were after an extra render. Teleport (> 400 units in one tick) or camera
 *   cut (> 60 deg): state N (duplicate).
 *   Still at tick rate: HUD, particles, crowd, animated scenery, pose values
 *   derived by the render (+0x4DF0.., they follow the interpolated pose).
 *   XBOX_FPS_INTERP=0      duplicates (no interpolation)
 *   XBOX_FPS_INTERP_LOG=N[@t]  N race renders logged ([INTERP]) from race tick t
 *   XBOX_FPS_INTERP_SYNTH=1    (test, with XBOX_FPS_CAP_DUP) alpha = k / (DUP + 1)
 *   XBOX_FPS_INTERP_DUMP=t     (investigation) dumps camera / rider 0 over 4 ticks
 *   XBOX_FPS_CAP_CHECK=3       (test) extra render done again without interpolation:
 *                              words written by the render that depend on the interpolation
 *   XBOX_FPS_CAP_STALL=s:ms    (test) one simulated stall of ms in a normal render at s seconds
 *   XBOX_FPS_INTERP_CLOCK=0    (comparison) old alpha clock: time the tick was consumed
 *   XBOX_FPS_INTERP_JANK=n:ms  (test) a render slowed by ms every n race renders
 *   XBOX_FPS_INTERP_TRACE=file (test) per render: time, tick, alpha, camera / rider 0 positions
 *
 * Pacing: the average render duration is measured over normal renders
 * (InGameState 0xAB610, menus 0x7CBD0) and extra ones; durations > 50 ms
 * (loading) are ignored, so a single stall cannot freeze the average and
 * stop the extra frames.
 *
 *   XBOX_FPS_CAP=60 (default) | 120 | 144 | 240 | N (60..1000) | 0 (no limit)
 *       | monitor (refresh rate of the primary monitor, 60 if <= 61 Hz)
 *       60 or unset: hook not installed, strict original behavior.
 *   XBOX_FPS_CAP_LOG=1     statistics every ~2 s ([FPSCAP])
 *   XBOX_FPS_CAP_NOSKIP=1  (test) extra render even if the next tick is delayed by it
 *   XBOX_FPS_CAP_CHECK=2   control: same wait without a render (assigns differences to other threads)
 *   XBOX_FPS_CAP_CHECK=1   state probe: fingerprint before / after each extra
 *                          render (RNG, race, whole riders, AudioSystem),
 *                          sampled diff of memory pages
 *
 * Fidelity: RNG A/B states saved and restored around each extra render (the
 * render draws from RNG A during the intro fly-over); allow list of renders
 * (race 0xAB610, menus 0x7CBD0); none during the frame skip [state+0x6C].
 *
 * Rules:
 *   - a due tick always goes first (wait on the event with a timeout, poll
 *     at 0 right before each render);
 *   - no extra render that would end after the next expected tick (estimate:
 *     average render duration): ticks are not delayed;
 *   - never more than one render late (no catch-up on the render side);
 *   - only on the call at 0xAA296 (edi = 0, esi = Application,
 *     ecx = [esi+0x2C]); the boot wait 0xAA1D0 (edi = 3) keeps the
 *     original.
 */
#ifndef FPS_CAP_H
#define FPS_CAP_H

extern int g_fps_cap_on;

void fps_cap_init(void);
void (*fps_cap_lookup(unsigned int xbox_va))(void);

#endif
