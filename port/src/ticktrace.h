/*
 * ticktrace -- per race tick trace: RNG and state fingerprints.
 *
 * Purpose: an end-to-end check for the frame cap. Two runs cannot be compared
 * frame by frame (not deterministic); the sequence of TICKS is compared
 * instead: on each race tick, the logic state must be the same. The first
 * tick where the fingerprint differs, and the RNG draw that explains it, tell
 * whether a difference comes from uncaptured state or from the game's own
 * variance.
 *
 *   XBOX_TICKTRACE=1        one line per race tick (InGameState vt+0x14 = 0xAD4A0)
 *   XBOX_TICKTRACE_RNG=1    + one line per RNG A/B draw (hook 0x12A610) with
 *                           phase (tick / off tick / other thread) and the
 *                           first 3 calling guest functions
 *   XBOX_TICKTRACE_DIR=dir  folder (default: the highest "_local" above the
 *                           exe, + \ticktrace)
 *   XBOX_TICKTRACE_EXTRA=va:len,...   extra absolute regions in the fingerprint
 *   XBOX_TICKTRACE_WATCH=base+off:len,...   "who writes what": for each word,
 *                           number of ticks where it changes DURING the tick
 *                           and of renders (0xAB610) where it changes DURING
 *                           the render; base = st (InGameState [app+4]), cam
 *                           ([st+0x2C]), race, app, r0..r7 (riders
 *                           [race+0xC4+4i]) or absolute VA; "# watch" summary
 *                           every 1,800 ticks (8 regions, 64 KB each at most)
 *
 * T line (after each tick):
 *   T seq race_tick state nA nB dA dB rngA rngB race cam nriders r0..r5 [xEXTRA]
 *     nA/nB = draw counters [0x1FAD84] / [0x1FAD9C] ([obj+0x14], +1 per draw,
 *     0x12A62C); dA/dB = draws during this tick; FNV-1a 32 fingerprints:
 *     rngA/rngB (6 words), race (0x400 bytes), cam = view 0 of InGameState
 *     (view matrix +0xC0 and projection +0x100/+0x104), then nriders and, per
 *     rider, the kinematics (position +0x170, velocity +0x180, +0x458, +0x15C,
 *     gauge +0x1C).
 *     The draws made in tick N are the R lines of seq N-1.
 * R line (XBOX_TICKTRACE_RNG=1):
 *   R seq A|B n phase caller1 caller2 caller3
 *     phase: t = inside the race tick, h = loop thread outside the tick
 *     (render, other states), o = other thread. Callers: guest functions
 *     (start address) found from the host stack; a guest function inlined by
 *     the compiler may be missing.
 *
 *   XBOX_TICKTRACE_READS=base+off:len   (investigation, slow) one region: one
 *                           render in XBOX_TICKTRACE_READS_EVERY (30), its host
 *                           pages become PAGE_NOACCESS; each access is recorded
 *                           (word read / written, reading / writing guest
 *                           function); "# reads" summary.
 *                           XBOX_TICKTRACE_READS_PHASE=tick: traces the ticks
 *                           instead of the renders (who writes the state during
 *                           the tick)
 *
 * Inert by default: without XBOX_TICKTRACE, the tick hook is not installed;
 * the RNG hook (direct calls routed by pass 13) calls the original right
 * away. Comparing two traces: port/tools/ticktrace_cmp.py.
 */
#ifndef TICKTRACE_H
#define TICKTRACE_H

extern int g_ticktrace_on;

void ticktrace_init(void);
void (*ticktrace_lookup(unsigned int xbox_va))(void);

#endif
