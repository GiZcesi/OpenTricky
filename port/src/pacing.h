/*
 * pacing -- give the screens between the menus and the race, and the start countdown, time to be seen.
 *
 * The console loaded a track from the DVD in many seconds, behind the loading screen; from an SSD it takes about one, so the loading screen
 * flashes by, and the countdown at the gate (3, 2, 1) feels short next to it.
 *
 *   OT_MIN_LOAD_MS=3000   a level load (InGameState_LoadLevel, 0xAC9B0) takes at least this long, so its animated loading screen can be read (default 0: as fast as it loads, as the original).
 *   OT_COUNTDOWN_SLOW=1.5 the start countdown (race state 3) runs this much slower (default 1 = the original speed): the title's own clocks (interrupt time, performance
 *                         counter, KeTickCount) run slower by this factor while it lasts, so what it paces by them stretches; 1 = the original speed.
 */
#ifndef OT_PACING_H
#define OT_PACING_H
#include <stdint.h>

void (*pacing_lookup(uint32_t xbox_va))(void);

/* The race state (1 PreRace, 2 StartRace, 3 Countdown, 4 Race, 5 EndRace...), -1 outside a race. Safe to call from any thread. */
int ot_race_state(void);
/* The factor by which the 60 Hz tick is stretched right now (1 outside the countdown). */
double pacing_tick_scale(void);

#endif
