/*
 * frame_clock -- the title's 60 Hz frame timer on an exact grid.
 * See frame_clock.c.
 */
#ifndef FRAME_CLOCK_H
#define FRAME_CLOCK_H

void   frame_clock_init(void);
void (*frame_clock_lookup(unsigned int xbox_va))(void);
int    frame_clock_on(void);
/* Time (QPC ms) the tick of the last frame timer callback was produced, its
 * point of the exact grid; 0 when unknown. Any thread. */
double frame_clock_tick_ref(void);

#endif
