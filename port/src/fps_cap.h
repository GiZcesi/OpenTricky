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
 *   Still at tick rate: HUD (see below), the particles that are not the rider
 *   effects below (among the riders': the Uber glow, the boost ribbons), crowd,
 *   animated scenery, pose values derived by the render (+0x4DF0.., they follow
 *   the interpolated pose).
 *   XBOX_FPS_INTERP=0      duplicates (no interpolation)
 *   XBOX_FPS_INTERP_LOG=N[@t]  N race renders logged ([INTERP]) from race tick t
 *   XBOX_FPS_INTERP_SYNTH=1    (test, with XBOX_FPS_CAP_DUP) alpha = k / (DUP + 1)
 *   XBOX_FPS_INTERP_DUMP=t     (investigation) dumps camera / rider 0 over 4 ticks
 *   XBOX_FPS_CAP_CHECK=3       (test) extra render done again without interpolation:
 *                              words written by the render that depend on the interpolation
 *   XBOX_FPS_CAP_STALL=s:ms    (test) one simulated stall of ms in a normal render at s seconds
 *   XBOX_FPS_INTERP_CLOCK=0    (comparison) old alpha clock: time the tick was consumed
 *   XBOX_FPS_INTERP_JANK=n:ms  (test) a render slowed by ms every n race renders
 *   XBOX_FPS_SMOOTH_LEGS=1     legs and board (bones 14-20) shown as the average of the
 *                              last 4 ticks relative to the root while riding on the ground
 *                              (display only, off by default; also at 60 fps: then only the
 *                              render and tick hooks are installed). Launcher: ADVANCED >
 *                              EXPERIMENTAL "Smoother legs in turns" ([Advanced] SmoothLegs)
 *   XBOX_FPS_INTERP_TRACE=file (test) per render: time, tick, alpha, camera / rider 0 positions
 *   Board trail (always on above 60): the slices of tick N (the live slice at the
 *   board's contact, and the newest one when laid at N) drawn with the board as
 *   shown (interpolated, leg smoothed), on the snow; see fps_cap.c. Developer tools:
 *   XBOX_TRAIL_TIP=0 (comparison; diagnostics `set trailtip 0|1`),
 *   XBOX_TRAIL_TRACE=file (per draw of the player's trail: the gaps it closes),
 *   XBOX_FPS_HOLD_ALPHA=a (0..0.99: the held frame shows lerp(N-1, N, a)).
 *   Rider effects (always on above 60): what tick N laid from the board (sprays,
 *   puffs, bursts, the powder ribbon's newest slice and head, the surface spray's emitter)
 *   drawn with the board as shown, what lives on shown at the render's instant (age
 *   and points between ticks N-1 and N). Riders: a step of the hips that
 *   accelerates by more than 100 units per tick in one tick (the game moved the
 *   rider at once) shown at tick N for that tick, as the camera's cuts; a pose
 *   whose feet or board turn around the hips by more than 35 deg in one tick shown
 *   as the skeleton of tick N at the interpolated hips (no chord through the
 *   body). Developer tools:
 *   XBOX_FX_SHOWN=0 / XBOX_FPS_RIDER_CUT=0 (comparison; diagnostics `set fxshown`,
 *   `set ridercut`), XBOX_FX_TRACE=file (what the player's effects were moved by),
 *   XBOX_FPS_CUT_TRACE=file (per race tick and rider: step, acceleration, turn, cut).
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
 * HUD (XBOX_FIX_HUD_RATE=1, default): the overlay panels' draws step their
 * animations on every call (selected item pulse, banner fade, marker sway);
 * an extra race render starts from the HUD state copied before the normal
 * render and the state after it is written back, so the HUD advances once
 * per tick as at 60; no extra render while the gameplay tick is frozen
 * (pause). XBOX_FIX_HUD_RATE=0: previous behavior. With
 * XBOX_FPS_CAP_CHECK=1 the probe also lists the HUD regions changed by the
 * extra renders.
 *
 * Rules:
 *   - a due tick always goes first (wait on the event with a timeout, poll
 *     at 0 right before each render);
 *   - an extra render that would end after the next expected tick (estimate:
 *     average render duration) delays the moment the tick runs, not what it
 *     does (the timer reads the pad at its own point); it is done only
 *     within a bound: with the render grid, while the tick's render keeps
 *     its slot (at most half a period late); without a limit, while,
 *     lasting the average render duration plus one standard deviation, it
 *     would end at most half of that after the tick (see fps_cap.c); with
 *     XBOX_FPS_CAP_GRID=0 (comparison), never;
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
