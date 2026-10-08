/*
 * ctlscheme -- the control scheme: the PS2 layout on every pad.
 *
 * The title maps buttons to actions through two text files read at run time,
 * data/config/btnmap0.dat and btnmap1.dat (parser 0xA87F0, opened by 0xA8C50
 * each time a Player's input is set up, 0x5B650). Only the order of the lines
 * matters: names are skipped; each line gives a "pressed" mask and an
 * "observed" mask, and an action is active when
 * (buttons & observed) == pressed (0xA8490). (NONE) = 0xFFFFFFFF never matches.
 *
 * The Xbox files wire 7 of the 15 grab slots of every rider (LT, RT, Y and
 * their combinations) and leave the other 8 at (NONE), although the grabs,
 * their animations and their points are all on the disc (the AI uses them).
 * The PS2 game had four shoulder buttons and used all 15 combinations; the
 * title still builds the PS2 table of 15 masks (0x5B68E-0x5B6FE): the four
 * single buttons, the six pairs, the four triples, all four.
 *
 * The host serves a rewritten copy of each file (the disc and the extracted
 * files are never modified) that does what the two PS2 control files do,
 * every action on its PS2 button, moved by position onto a modern pad:
 * Cross = A, Circle = B, Square = X, Triangle = Y, L1 = LB (White),
 * R1 = RB (Black), L2 = LT, R2 = RT, Select = Back, L3 = left stick press.
 * The PS2 lines are written by hand in ctlscheme.c (k_rules, k_slot_ps2).
 * What changes against the Xbox files:
 *   - the 15 grab slots: the PS2 combination of L1 / R1 / L2 / R2 for each
 *     (k_slot_ps2 below), observed over the four shoulder buttons only;
 *   - Boost and Tweak on Square alone (Circle | Square on Xbox);
 *   - btnmap0: AnticAbort on L3 (none on Xbox); LateSpinMode on no button
 *     (Black on Xbox, which is R1 here); Spin and Flip on the D-pad alone and
 *     GateRock on the left stick alone (D-pad, else left stick, on Xbox);
 *   - btnmap1: CameraReverse on Triangle (Black on Xbox);
 *   - RandomGrab (never active) observes the shoulder buttons, like the grabs.
 * CameraToggle stays on no button in both, as on the PS2 and the Xbox.
 * Every other line is passed through byte for byte; a line that does not
 * read as the Xbox file does is kept as it is (logged).
 *
 * The PS2 layout is the only one a player gets; the launcher no longer
 * offers a choice and ignores an old ControlScheme line in the .ini.
 *
 * Lessons (Trick Tutorial, game mode 6) use the PS2 layout too. Their demos
 * replay controller states recorded with the Xbox layout: LessonMan injects
 * one recorded state per frame into the rider's input (0x565B0), so while it
 * does, the two button words of that state (previous, current) are
 * translated to the PS2 layout -- the grab combination of Y / LT / RT becomes
 * the same grab slot's PS2 combination, Circle (tweak / boost) becomes
 * Square, Black (late spin mode) and White (unused) are dropped -- and put
 * back right after, so the recording itself is never changed. The analog
 * values are replayed as recorded: the 360 demos spin and flip on the D-pad,
 * which the PS2 layout keeps (the left stick alone moves in a few dozen of
 * their 377,300 frames), and none presses Black or L3.
 * The prompts drawn over the demo (0x56760) record action and
 * grab numbers, not buttons, and turn them into buttons through the tables
 * the served file filled: they follow the PS2 layout without any change.
 *
 * The command word a Player sends already carries the resolved grab number,
 * so replays, ghosts and network commands do not depend on the scheme.
 *
 *   XBOX_CONTROL_SCHEME   hidden, for debugging only: xbox = the original
 *                         Xbox files (nothing hooked). Unset or any other
 *                         value: the PS2 layout. The launcher never sets it.
 *   XBOX_PS2_SHOULDERS    which shoulder button each bit of the title's PS2
 *                         table is: "a" (default: 0x200 L1, 0x400 R1,
 *                         0x1000 L2, 0x2000 R2) or "b" (L1 and L2 swapped).
 *   XBOX_FIX_LESSON_PS2   1 (default): lessons in the PS2 layout, as above;
 *                         0: lessons get the original Xbox files and demos.
 *   XBOX_FIX_TRICKTEXT_PS2 1 (default): the buttons of a trick are written in
 *                         the PS2 order (shoulders L1 R1 L2 R2; glyph table
 *                         0x1AA820, ctlscheme.c); 0: in the Xbox order. The
 *                         tweak reads Square alone either way (Tweak = Square).
 */
#ifndef SSX_CTLSCHEME_H
#define SSX_CTLSCHEME_H

#include <stdint.h>

void ctlscheme_init(void);

/* 1 when the PS2 layout is served (everything but XBOX_CONTROL_SCHEME=xbox). */
int ctlscheme_ps2(void);

/* recomp_lookup_manual: the lesson demo injection hook (0x565B0). */
void (*ctlscheme_lookup(unsigned int xbox_va))(void);

/* The rewrite itself, for tools and tests: a malloc'd copy of `data`, the
 * text of btnmap<map>.dat, in the PS2 layout, or NULL when the text does not
 * look like a button map. */
char *ctlscheme_rewrite_text(const char *data, uint32_t size, int map, uint32_t *out_size);

/* One button word of a lesson demo, recorded with the Xbox layout, in the
 * PS2 layout (see above); for tools and tests too. */
uint32_t ctlscheme_lesson_buttons(uint32_t buttons);

#endif /* SSX_CTLSCHEME_H */
