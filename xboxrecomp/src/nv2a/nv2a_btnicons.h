/* Button icons: the game's Xbox (Duke) button glyphs redrawn for a modern
 * Xbox pad or a PlayStation pad.
 *
 * The game draws its button prompts from four texture atlases (menus, race
 * HUD, trick HUD, loading screens). When one of them is uploaded, its texels
 * (the player's own files) are scaled up 4x, the rectangle of each button
 * glyph is cleared and an icon drawing is fitted inside it; the rest of the
 * atlas is kept. Drawings: port/assets/btnicons/juliocacko/ (CC0), built into
 * the executable as resources; the "ps2" style has its own pictures
 * (port/assets/btnicons/ps2/). No picture of the Xbox game is stored anywhere.
 *
 * The launcher always sets XBOX_BUTTON_ICONS ([Fork] ButtonIcons). Unset or
 * "0" (test runs started without the launcher's settings): nothing here
 * runs and the texture path is the original one, as the benches expect.
 *
 *   XBOX_BUTTON_ICONS=auto         the pad in use: modern until pad detection
 *                                  exists (so is any value not listed here)
 *                     modern       Xbox Series pad (LB RB LT RT, View, Menu)
 *                     playstation  PlayStation pad (Cross ... Triangle, L1 ... R2,
 *                                  SELECT, START)
 *                     ps2          the PS2 button style, its pictures
 *                                  (port/assets/btnicons/ps2/), at
 *                                  the PS2 sizes and sampled nearest
 *                                  neighbour (auto in a build made with
 *                                  -DSSX_PS2_BUTTONS=OFF)
 *                     debug        each glyph rectangle filled with a bright
 *                                  colour and its number (table in the .c),
 *                                  to tell on screen which glyph is which
 *   XBOX_BUTTON_ICONS_LOG=1        one line per atlas composed, with its cost
 *   XBOX_FIX_BUMPERS_WIDE=0        bumpers at the Duke glyphs' size (comparisons;
 *                                  default: wide, btnicons_wide_bumpers)
 */
#ifndef NV2A_BTNICONS_H
#define NV2A_BTNICONS_H

#include <stdint.h>
#include "../d3d/d3d8_xbox.h"

/* 1 when icons replace the game's glyphs (reads the variable once). */
int btnicons_on(void);

/* The modern bumpers (LB RB, L1 R1) are wider than tall, the Duke's White and
 * Black glyphs the other way round. In the modern and PlayStation styles the
 * race / trick HUD atlas draws them squeezed into a BTNICONS_BUMPER_RW x _RH
 * texel region of their glyphs (the slot's full width, one texel in from its
 * top), and ctlscheme.c gives shapes 103 and 104 the size that shows a
 * _RW x _RH_SLOT region at BTNICONS_BUMPER_W x _H, then narrows their UVs to
 * the _RH rows (same place on screen, same size): about as many texels per
 * unit across as down, so the drawing is sampled from its sharp mip levels. */
int btnicons_wide_bumpers(void);
/* The bumpers' dy in the trick text's glyph layout (ctlscheme.c) when they
 * are wide: the one that puts their letters on the triggers' line, per style. */
int btnicons_bumper_text_dy(void);
#define BTNICONS_BUMPER_RW 14
#define BTNICONS_BUMPER_RH 8
#define BTNICONS_BUMPER_RH_SLOT 16
#define BTNICONS_BUMPER_W  24.0f
#define BTNICONS_BUMPER_H  15.0f

/* After a texture-cache miss on a w x h texture: when its level-0 guest bytes
 * are one of the button atlases, the composed atlas (a new reference for the
 * caller), else NULL. `guest` is the texture just decoded from those bytes
 * (lockable, linear A8R8G8B8 or DXT); `color` is the NV2A colour format. */
IDirect3DTexture8 *btnicons_compose(const uint8_t *level0, uint32_t n0, uint32_t w, uint32_t h,
                                    uint32_t color, IDirect3DTexture8 *guest);

/* 1 in the "ps2" style (the PS2 button style): ctlscheme.c then draws
 * the button shapes at the PS2 sizes. */
int btnicons_ps2(void);

/* 1 when `t` is a composed button atlas to be sampled nearest neighbour (the
 * "ps2" style: its pixels are shown as they are). */
int btnicons_point(const IDirect3DTexture8 *t);

/* The "ps2" style's shapes of the trick and race HUDs, i = 0, 1, ...: the
 * shape number, its UV rectangle in texels (u0, v0, u1, v1, as measured in
 * game), its size in the game and the size that shows the PS2 glyph at the
 * PS2 size (the game's rule: texels + 1). 0 past the last one. */
int btnicons_ps2_shape(int i, unsigned *shape, float uv[4], float game_wh[2], float ps2_wh[2]);

/* The trick tutorial of the "ps2" and "playstation" styles, i = 0, 1, ...:
 * the lesson pad (shape 96) and its gold markers' shapes, as the shape record
 * {w, h, v0, u0, u1, v1} (UV normalised) the game writes and the one that
 * shows the style's picture placed in the composed trick HUD atlas. 0 past
 * the last one, or when the style or its lesson pictures are not there (the
 * Duke stays). */
int btnicons_lesson_shape(int i, unsigned *shape, float game[6], float pic[6]);

/* The same styles: the corner of the marker of row `row` of the title's
 * table 0x1AA6C8 (LT, RT, A, B, X, Y, White, Black, left, right, down, up)
 * on the style's pad. 0 when the style has no lesson pad. */
int btnicons_lesson_mark(int row, float *dx, float *dy);

/* Configure Controller's pad and lines in the style's picture (the front
 * end's atlas, shape 61): 1 "ps2", 2 "playstation", 3 "modern", 0 none (the
 * Duke stays). ps2legend.c then lays the labels out to match. */
int btnicons_cfg_style(void);

#endif
