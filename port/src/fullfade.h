/*
 * fullfade -- full-screen fades reach the edges of the screen (fork).
 *
 * The title pulls its 2D layer in from the edges of a TV: when it sets an
 * orthographic 640x480 view with the "safe area" flag (0xFE940, 7th argument),
 * it widens the projection by the margins at 0x1C0840 (-30, -17, +58, +44), so
 * 0..640 x 0..480 lands on 28..615 x 16..456 of the screen. Text and gauges
 * stay off the overscan, but a quad meant to cover the screen -- the white
 * flash of the respawn (BACK) -- is pulled in too: on a CRT the band it
 * leaves was hidden by overscan; on a PC screen (and an emulator) it shows as
 * a white rectangle with the game visible around it, at every resolution and
 * every aspect ratio.
 *
 * The flash is a world-quads node (vtable 0x1A2AC8) drawn by its method
 * 0x1009D0 from the 2D vertex buffer. When that method draws, in an
 * orthographic view, a single untextured-looking quad (all four texture
 * coordinates equal) whose corners are exactly 0/640 x 0/480, the corners
 * are moved to the title's own widened rectangle (-30..668 x -17..507, read
 * from 0x1C0840), which the projection maps to the edges of the screen, plus
 * 4 pixels so that the half-pixel viewport offset leaves no uncovered column
 * at high resolutions: the quad covers the whole screen, in its own colour
 * and fade. Nothing else is touched.
 *
 *   XBOX_FULLSCREEN_FADES  1 (default) as above; 0 = like the Xbox (the hook
 *                          is not handed out).
 */
#ifndef FORK_FULLFADE_H
#define FORK_FULLFADE_H

extern int g_fullfade_on;

void fullfade_init(void);                       /* reads XBOX_FULLSCREEN_FADES */
void (*fullfade_lookup(unsigned int xbox_va))(void);

#endif
