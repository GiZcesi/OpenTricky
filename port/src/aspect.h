/*
 * aspect -- display shapes wider than 16:9 (fork).
 *
 * The title already renders "Hor+" in its widescreen mode: told by the
 * dashboard setting that the TV is 16:9, Renderer_SetScreenMode (0xF9EA0,
 * screen mode 2) sets the renderer's x-scale [this+0x53C] to 0.75, and the
 * perspective projection divides its aspect by it -- vertical field of view
 * unchanged, more on the sides (SceneView_RenderPass 0xFFB50 ; the camera
 * setup 0xFE770 reads it too). Orthographic views (HUD, menus) do not.
 *
 * For a shape A wider than 16:9 the x-scale becomes (4/3) / A: the hook on
 * 0xF9EA0 runs the title's own function, then rewrites [this+0x53C] in
 * mode 2. The host shows the frame at A (d3d8_SetHostAspect).
 *
 *   aspect_init(A)   A = width / height, from the launcher or XBOX_ASPECT.
 *                    4:3, 16:9 or 0: hook not installed, nothing changes.
 *   XBOX_ASPECT      direct mode (tests): "21:9", "32:9", "auto" (the
 *                    resolution's own shape, XBOX_RENDER) or a number;
 *                    implies the title's widescreen mode.
 *
 * Field of view on wide screens. The title keeps its vertical field
 * of view (82.7 degrees in a race) and Hor+ adds the sides, so at 21:9 and
 * 32:9 the horizontal one reaches 129 and 145 degrees and things near the
 * edges stretch (x2.33 and x3.29, against x1.84 at 16:9). A hook on
 * Matrix_BuildPerspectiveProjection (0x17770D: out, fov, aspect, near, far ;
 * every perspective view goes through it, so the culling and the lens flare
 * follow) narrows the vertical fov when the aspect is wider than 16:9:
 *   NoStretch  the horizontal fov stays the 16:9 one (default): the wider
 *              screen shows the same width of the world, a little less
 *              height, and the edges stretch no more than at 16:9 ;
 *   Balanced   the horizontal fov halfway between that and Hor+ ;
 *   Full       Hor+, as for the aspect above.
 *   XBOX_WIDE_FOV  nostretch | balanced | full ; wins over the launcher.
 * The title sizes its particles in pixels from their distance, not from
 * the projection, so the narrowed view would shrink them against the scene
 * (x0.75 at 21:9, x0.5 at 32:9, NoStretch): the hook hands the zoom to the
 * translator (d3d8_SetPointZoom), which scales the program's point size.
 *
 * Follow camera. NoStretch's zoom (z = A / (16/9)) also made the
 * rider z times bigger. The follow camera now backs off: the title's camera
 * runs as is, then the eye left for the view moves back along the view axis
 * by (z - 1) times the depth of the camera's pivot (on the rider's body), and
 * goes through the title's own collision check (ground, then pivot -> eye
 * against walls and tunnels) ; the title's eye is put back before its next
 * update (hooks on 0x75DE0, 0x759F0, 0x78DE0). The rider keeps its 16:9
 * size and place on screen. Balanced and Full: camera as the title.
 *   XBOX_WIDE_CAMERA=0      keeps the close camera in NoStretch.
 *   XBOX_WIDE_CAMERA_LOG=1  (tests) pivot depth, intended and actual move, 1 tick in 15.
 */
#ifndef FORK_ASPECT_H
#define FORK_ASPECT_H

extern int g_aspect_hook_on;
extern int g_aspect_menus43;      /* 1: menus at 4:3, see ASPECT_MENUS_* */
extern int g_aspect_camera;                 /* follow camera backs off (XBOX_WIDE_CAMERA) */

/* 1 if A needs the title's widescreen mode (anything wider than 4:3). */
int  aspect_init(double aspect);
/* Parses XBOX_ASPECT; render_w x render_h gives "auto" its shape. 0 = unset. */
double aspect_from_env(unsigned render_w, unsigned render_h);
void (*aspect_lookup(unsigned int xbox_va))(void);
/* 1: the frame about to be rendered is shown in a centred 16:9 frame (the
 * title's own 16:9 x-scale), 0: wide (see hud_anchor.h). */
void aspect_frame(int boxed);

enum { ASPECT_FOV_NOSTRETCH, ASPECT_FOV_BALANCED, ASPECT_FOV_FULL, ASPECT_FOV_COUNT };
extern int g_aspect_fov;                    /* ASPECT_FOV_*, NoStretch by default */
const char *aspect_fov_name(int mode);      /* "NoStretch", "Balanced", "Full" (.ini values) */
int  aspect_fov_parse(const char *s, int fallback);
/* XBOX_WIDE_FOV when set, else `chosen`; call before aspect_init. */
void aspect_set_fov(int chosen);

/* Menus out of a race on screens wider than 4:3:
 *   16:9  the title's own widescreen menus (art x1.33), in a centred 16:9
 *         frame beyond 16:9 ;
 *   4:3   the menus whole at 4:3, as drawn (3D and 2D at the title's 4:3
 *         x-scale), in a centred 4:3 frame from 16:9 up.
 * [Fork] Menus=16:9|4:3 in the .ini; XBOX_WIDE_MENUS=169|43 wins over it
 * (=0: no frame at all, everything stretched, tests). The race is the same
 * in both. ASPECT_MENUS_DEFAULT is the choice of a new player and of the
 * test runs with no variable: the one line to change to switch the default. */
enum { ASPECT_MENUS_169, ASPECT_MENUS_43, ASPECT_MENUS_COUNT };
#define ASPECT_MENUS_DEFAULT ASPECT_MENUS_43     /* 16:9 for the Xbox menus */
const char *aspect_menus_name(int mode);    /* "16:9", "4:3" (.ini values) */
int  aspect_menus_parse(const char *s, int fallback);
/* XBOX_WIDE_MENUS when it names one, else `chosen`; call before aspect_init. */
void aspect_set_menus(int chosen);

#endif
