/*
 * hud_anchor -- race HUD in its own proportions, at the size chosen.
 *
 * The title draws its HUD in a 640x480 orthographic view stretched to the
 * whole screen: x1.33 at 16:9 (as a console on a 16:9 TV -- the title never
 * scaled it properly), x1.79 at 21:9, x2.67 at 32:9. Here each element of
 * the race HUD keeps its proportions (scale = screen height / 480, as if the
 * title had been made for that screen), at the size chosen, and sits where
 * it belongs: at the left edge, the right edge or the centre of the player's
 * view (place, progress bar, speed and time at the left; score, Tricky gauge
 * at the right; countdown, CHECKPOINT, UBER TRICK, combos in the middle),
 * and likewise at the top, the bottom or the middle. Split screen: each
 * player's view. Menus and everything out of a race: the 16:9 frame
 * below.
 *
 * How: the race HUD (methods 0xC9BB0 / 0xC9D70 of the HUD object, with
 * HUD_DrawWorldSpaceMarkers 0xC44E0 and HUD_DrawRaceOverlay 0xC6730) only
 * appends records to the HUD view's list. Their hooks note the objects of
 * those records and the player's view; when RenderFrame draws the list, the
 * records are grouped into elements from their quads, and the pass_tags
 * hook on GfxContext_ApplyStateBlock writes two NOP markers before each
 * (hud_anchor_tag): the translator then scales the draws that follow
 * (nv2a_pgraph_d3d11.h, PGRAPH_HUD_TAG_MAGIC). On, it turns the pass markers
 * on (pass_tags_init).
 *
 *   HUD shape  Proportional (default)  as above, every screen shape
 *              Xbox                    like the Xbox: stretched, full size
 *   HUD size   100 / 90 / 85 (default) / 80 / 70 %, Proportional only
 *   .ini       [Display] HudShape=Proportional|Xbox, HudSize=85
 *   XBOX_HUD_SHAPE  proportional | xbox (also auto/on, stretched/off), wins ;
 *   XBOX_HUD_SIZE   100 | 90 | 85 | 80 | 70, wins ;
 *   XBOX_HUD_TRLOG=N  prints the elements of every Nth list.
 * At 4:3 and 100 %, or Xbox, nothing is tagged and no marker is written:
 * the push buffer and the image are those of the title (up to 16:9).
 *
 * The 16:9 frame. Wider than 16:9, only the race itself is wide:
 *   - out of a race (front end, menus with their 3D scenes, loading
 *     screens, videos, the riders' intro scenes before the start grid:
 *     race states 0 and 1), the frame is the title's own 16:9 -- its 16:9
 *     x-scale back (aspect_frame) -- shown in a centred 16:9 frame with
 *     black bars by the host ;
 *   - in a race (state 2 on: start grid, countdown, race, pause, end
 *     screens, replay), the 3D stays wide and every record of the HUD view that is not the race HUD (panels, pause
 *     and end-screen menus) is drawn in the centred 16:9 frame: its x is
 *     scaled by (16/9) / shape about the centre, as the title draws it at
 *     16:9 -- by (4/3) / shape with XBOX_FIX_RACE2D (below). Quads over the whole width touching the top or the bottom
 *     (fades, letterbox) stay stretched.
 * A frame tag after each FRAME_BEGIN says which; the translator applies
 * it to the image the frame's overlay goes into (the 3D of the frame
 * before), so a switch never shows a frame drawn for the other one.
 *   XBOX_WIDE_MENUS=0   off: everything stretched as before (tests).
 *   Menus at 4:3 (aspect.h, ASPECT_MENUS_*): the same, with a centred 4:3
 *   frame from 16:9 up and the title's 4:3 x-scale for the framed image.
 *
 * The race's own 2D (XBOX_FIX_RACE2D, default 1). The records of the HUD
 * view in a race that are not the race HUD -- the finish banner (FINISH,
 * "1st PLACE", time, medal: overlay method 0xD2D80), pause, "Are you
 * sure?", Top 5, results -- were drawn at the Xbox's 16:9 proportions
 * (x1.33 wider than the title's 640x480 art), unlike the race HUD. With
 * the proportional HUD, on any screen wider than 4:3 (16:9 included),
 * they keep the title's 4:3 proportions: x scaled by (4/3) / shape about
 * the centre, full size, inside the 16:9 frame when there is one. At 16:9
 * and 16:10 only these records change (frame tags without the frame:
 * the image, menus and 3D stay as before).
 *   XBOX_FIX_RACE2D=0   as before (16:9 proportions in the frame, nothing
 *                       at 16:9) ;
 *   XBOX_FIX_RACE2D=2   as 1, and the finish banner is part of the race
 *                       HUD (HUD size, anchored) -- to compare.
 * The 0xD2D80 hook prints its first draw (and every Nth with
 * XBOX_HUD_TRLOG=N) to tie the banner on screen to it.
 *   XBOX_HUD_BANNER_SHOTS=<prefix>  (tests) internal screenshots at the
 *                       banner's draws XBOX_HUD_BANNER_AT=N,... (default
 *                       20,60,120,240): the same moment on every run.
 *   XBOX_HUD_DIAG=1     prints the game state at each change (tests).
 *
 * Panels edge to edge (XBOX_FIX_PANEL_EDGES, default 1: wider than 4:3 with
 * the race's 2D above, so with the proportional HUD; and at 4:3, where
 * only the panels' ends move, their content as drawn). The title's panels
 * -- a dark backing between gold bars from x = 10 to 632 of its 640x480
 * screen: tutorial and lessons, the Uber Trick and "Single Event Race"
 * cards before the start, pause and options, the replay's help, Top 5 and
 * results -- keep their content at the 4:3 proportions, centred, and their
 * backing and the straight runs of their bars reach the screen's edges
 * (the edges of the centred 16:9 frame when the frame is shown whole, before
 * the start grid wider than 16:9). Before the start grid, a list of the
 * HUD view holding a panel gets its content at the 4:3 proportions too, as
 * in a race. How the panel is found: hud_anchor.c, "Panels edge to edge".
 *   XBOX_FIX_PANEL_EDGES=0  as before (the panels at the 4:3 columns in a
 *                       race, stretched before it) ;
 *   XBOX_PANEL_LOG=1    prints, per list, the panel ends found (tests) ;
 *   XBOX_PANEL_LOG=2    and every record of the lists (the vertices of the
 *                       wide ones), every 300th frame ; 3: and each tag call.
 */
#ifndef FORK_HUD_ANCHOR_H
#define FORK_HUD_ANCHOR_H

#include <stdint.h>

enum { HUD_SHAPE_PROPORTIONAL, HUD_SHAPE_XBOX, HUD_SHAPE_COUNT };
enum { HUD_SIZE_COUNT = 5 };                /* 100, 90, 85, 80, 70 % */
#define HUD_SIZE_DEFAULT 2                  /* 85 % */

/* The markers' format, as nv2a_pgraph_d3d11.h reads it (PGRAPH_HUD_TAG_*):
 * magic in the 16 high bits ; x tag: bit 15 = scale, bits 0-14 = anchor x ;
 * y tag: bits 0-14 = anchor y ; in 1/16 pixel of the 640x480 screen. */
#define HUD_TAG_MAGIC   0x4855u
#define HUD_TAG_MAGIC_Y 0x4856u
/* The 16:9 frame, PGRAPH_BOX_TAG_* there: low bits = what follows. */
#define HUD_TAG_MAGIC_BOX   0x4857u
#define HUD_BOX_OFF         0u      /* record drawn as the title drew it */
#define HUD_BOX_ON          1u      /* record framed in the centred 16:9 */
#define HUD_BOX_FRAME_BOXED 2u      /* frame tag: the whole frame shown at 16:9 */
#define HUD_BOX_FRAME_WIDE  3u      /* frame tag: wide 3D, menus framed */
/* Panels edge to edge, PGRAPH_PANEL_TAG_* there. First tag: bits 0-13 =
 * the panel's left edge (1/16 pixel of the 640x480 screen) ; second tag:
 * bits 0-13 = its right edge. */
#define HUD_TAG_MAGIC_PANEL     0x4858u
#define HUD_TAG_MAGIC_PANEL_R   0x4859u
#define HUD_PANEL_WIDE_ONLY     0x8000u     /* first tag: wide images only (a race) */
#define HUD_PANEL_SCALE_ONLY    0x4000u     /* first tag: 4:3 proportions, no edge */

extern int g_hud_anchor_on;
extern int g_box_on;                        /* wider than 16:9: menus framed */
extern int g_panel_on;                      /* the panels edge to edge (4:3 included) */
extern int g_race2d_on;                     /* wider than 4:3: the race's 2D at 4:3 proportions */

const char *hud_anchor_mode_name(int mode);         /* .ini values */
int  hud_anchor_mode_parse(const char *s, int fallback);
int  hud_anchor_size_value(int index);               /* percent */
int  hud_anchor_size_parse(const char *s, int fallback);   /* "85" -> index */
/* shape = presented width / height ; mode = HUD_SHAPE_*, size = index (the
 * launcher's), which XBOX_HUD_SHAPE / XBOX_HUD_SIZE override. Call before
 * pass_tags_init. */
void hud_anchor_set(double shape, int mode, int size);
/* Tags for the record whose object is obj in the list of view (the view
 * RenderFrame is drawing): x tag returned, y tag in *tag_y ; 0 if it is not
 * part of the race HUD or keeps the stretch. Called by pass_tags (H2), and
 * hud_anchor_frame at each H1. */
uint32_t hud_anchor_tag(uint32_t obj, uint32_t view, uint32_t *tag_y);
/* At each H1: the frame tag to write after FRAME_BEGIN (0 = none). */
uint32_t hud_anchor_frame(void);
void (*hud_anchor_lookup(unsigned int xbox_va))(void);

#endif
