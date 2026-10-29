/*
 * speedfx -- the game side of an effect that this source tree does not hold:
 * the game still calls these, and each one (extras_off.c) answers "off".
 */
#ifndef SSX_SPEEDFX_H
#define SSX_SPEEDFX_H

void speedfx_init(void);
void speedfx_note_replay(void);
int  speedfx_camera(float *k);
int  speedfx_in_race(void);

#endif
