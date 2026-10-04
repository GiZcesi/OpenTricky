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
 */
#ifndef FORK_ASPECT_H
#define FORK_ASPECT_H

extern int g_aspect_hook_on;

/* 1 if A needs the title's widescreen mode (anything wider than 4:3). */
int  aspect_init(double aspect);
/* Parses XBOX_ASPECT; render_w x render_h gives "auto" its shape. 0 = unset. */
double aspect_from_env(unsigned render_w, unsigned render_h);
void (*aspect_lookup(unsigned int xbox_va))(void);

enum { ASPECT_FOV_NOSTRETCH, ASPECT_FOV_BALANCED, ASPECT_FOV_FULL, ASPECT_FOV_COUNT };
extern int g_aspect_fov;                    /* ASPECT_FOV_*, NoStretch by default */
const char *aspect_fov_name(int mode);      /* "NoStretch", "Balanced", "Full" (.ini values) */
int  aspect_fov_parse(const char *s, int fallback);
/* XBOX_WIDE_FOV when set, else `chosen`; call before aspect_init. */
void aspect_set_fov(int chosen);

#endif
