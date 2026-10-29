/*
 * extras_off.c -- the game's side of the extras when they are not built
 * (port/extras.cmake, off by default): the speed camera and the
 * speed blur's view (speedfx.h), the Stick dead zone (smooth_stick.h) and the
 * lighting map of the ambient haze (lightmap_map.h). The D3D side is
 * xboxrecomp/src/d3d/d3d8_extras_off.c.
 *
 * Each call the rest of the game makes into them does nothing and answers
 * what the real one answers with the extra off, so the game is the one of
 * the extras turned off, bit for bit. With the extras the real sources are
 * built instead and this file is empty.
 */
#include <stdint.h>
#include "speedfx.h"
#include "smooth_stick.h"
#include "lightmap_map.h"

/* ── speed camera, speed blur's view: never on ── */
void speedfx_init(void) {}
void speedfx_note_replay(void) {}
int  speedfx_camera(float *k) { (void)k; return 0; }
/* Only asked with a race camera angle of the tuning panel (aspect.c): none here. */
int  speedfx_in_race(void) { return 0; }

/* ── Stick dead zone: the original stick ──
 * fork_regen.sh renames the translated reader of the Player's command
 * 0x000A87E0 to sub_000A87E0_lifted (it is replaced by smooth_stick.c when
 * the extras are built); here it is called as it is. */
void sub_000A87E0_lifted(void);
void sub_000A87E0(void) { sub_000A87E0_lifted(); }
void smooth_stick_init(void) {}

/* ── lighting map of the ambient haze: never armed (drawdist.c hooks nothing for it) ── */
int  g_lmap_armed;
void lightmap_map_init(void) {}
void lightmap_map_note(uint32_t obj, uint32_t vb, uint32_t pidx) { (void)obj; (void)vb; (void)pidx; }
void lightmap_map_renderer(uint32_t r) { (void)r; }
