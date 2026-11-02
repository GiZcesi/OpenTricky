/*
 * NV2A PGRAPH → D3D11 Translator
 *
 * Intercepts NV2A push buffer method calls and translates them into
 * D3D8→D3D11 rendering commands. This is the core of the GPU translation
 * layer for Xbox static recompilation.
 *
 * The push buffer contains NV2A Kelvin (NV097) methods:
 *   - Surface/viewport setup → D3D11 render target + viewport
 *   - Render state (blend, depth, cull) → D3D11 state objects
 *   - Begin/End draw + Inline vertex data → D3D11 DrawPrimitiveUP
 *   - Texture binding → D3D11 shader resource views
 *   - Clear commands → D3D11 ClearRenderTargetView
 *
 * Vertex formats observed in menus:
 *   5 dwords per vertex: float X, float Y, float U, float V, D3DCOLOR
 *   Drawn as TRIANGLE_STRIP (mode 6)
 *
 * This module is designed to be reusable across Xbox recompilation projects.
 * See: https://github.com/sp00nznet/xboxrecomp
 */

#ifndef NV2A_PGRAPH_D3D11_H
#define NV2A_PGRAPH_D3D11_H

#include <stdint.h>

/* Initialize the PGRAPH→D3D11 translator. Call after D3D11 device is created. */
void pgraph_d3d11_init(void);

/* Shut down and release resources. */
void pgraph_d3d11_shutdown(void);

/* Process an NV2A PGRAPH method call. Called from push buffer parser.
 * Returns 1 if handled, 0 if unhandled (caller should log/ignore). */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param);

/* Images presented by the translator so far (XBOX_PERF method profile). */
unsigned pgraph_d3d11_frame_count(void);

/* XBOX_FIX_PUMP_IDX, default 1: a run of n ARRAY_ELEMENT16 words, or
 * of incrementing SET_TRANSFORM_CONSTANT words from `method`, taken in one
 * call as pgraph_d3d11_method would take them word by word. index16 returns
 * 1 if it took all of them, 0 if the caller must go word by word; xfconst
 * returns how many words it took from the front of the run (0 = none). */
int      pgraph_d3d11_index16_run(int subchannel, const uint32_t *w, uint32_t n);
uint32_t pgraph_d3d11_xfconst_run(int subchannel, uint32_t method, const uint32_t *w, uint32_t n);
/* The same for every plain register array the title sends in runs --
 * transform constants and program, vertex stream offsets and formats,
 * SET_VERTEX_DATA4F_M -- returns how many words it took (0 = none). */
uint32_t pgraph_d3d11_run(int subchannel, uint32_t method, const uint32_t *w, uint32_t n);

/* Flush any pending draw commands (call at end of frame). */
void pgraph_d3d11_flush(void);

/* Set chyron scroll: pass frame counter to animate, 0 to disable.
 * Applies horizontal scroll offset to vertices in the chyron Y band. */
void pgraph_d3d11_set_chyron_scroll(uint32_t frame);

/* Hand the translator the base and size of the guest RAM window, so
 * DRAW_ARRAYS can follow SET_VERTEX_DATA_ARRAY_OFFSET into vertex buffers.
 * Called by the push buffer consumer, which already holds the mapping. */
void pgraph_d3d11_set_mem_base(void *base, uint32_t size);

/* Draw substitution (a title-side filter, e.g. terrain patches). Called on the
 * pump thread for every vertex-program indexed draw, with its primitive mode,
 * index list and the 16 vertex array offsets / formats. Returning 1 with `out`
 * filled draws out->indices instead, and every strided array whose offset
 * (masked to 28 bits) lies in [out->base, out->base + out->span) reads
 * out->host + (offset - out->base) -- same layout and stride -- for that draw
 * only. Up to PGRAPH_SUBST_OVERLAYS overlays (debug views) follow: triangle
 * lists over the same vertices in one flat colour, alpha blended, depth tested
 * as the draw, depth not written. A blend overlay (blend = 1) is drawn instead
 * with the title's own shading and other textures on some stages (tex_offset /
 * tex_format, 0 = the draw's own), its alpha taken per vertex from the float at
 * host + weights + 4 * index: the program gets one more instruction that
 * carries that value into oT0.z (unused by 2D lookups) and the pixel shader
 * writes it to alpha. Returning 0 leaves the draw untouched. NULL removes the
 * filter (the default: nothing changes). */
#define PGRAPH_SUBST_OVERLAYS 10
typedef struct {
    const uint32_t *indices;    /* triangle list */
    uint32_t count;
    float rgba[4];
    int blend;
    uint32_t weights;           /* byte offset of the per-vertex alphas in host */
    uint32_t tex_offset[4], tex_format[4];
    uint32_t address[4];        /* nonzero: stage's TEXTURE_ADDRESS word for this overlay */
} PgraphDrawOverlay;
typedef struct {
    const uint32_t *indices;
    uint32_t count;
    const uint8_t *host;        /* replacement vertices, `size` bytes */
    uint32_t size;
    uint32_t base, span;        /* the guest span the replaced arrays start in */
    uint32_t n_overlay;
    PgraphDrawOverlay overlay[PGRAPH_SUBST_OVERLAYS];
    uint32_t address[4];        /* nonzero: stage's TEXTURE_ADDRESS word (U 3:0, V 11:8) for the main pass (not the overlays) */
} PgraphDrawSubst;
typedef int (*pgraph_draw_subst_fn)(uint32_t mode, const uint32_t *indices, uint32_t count,
                                    const uint32_t offset[16], const uint32_t format[16],
                                    PgraphDrawSubst *out);
void pgraph_d3d11_set_draw_subst(pgraph_draw_subst_fn fn);
/* Fork: an observer of every program draw (pump thread), called before
 * the substitution filter with the same arguments; it changes nothing.
 * NULL (the default): not called. */
typedef void (*pgraph_draw_observe_fn)(uint32_t mode, const uint32_t *indices, uint32_t count,
                                       const uint32_t offset[16], const uint32_t format[16]);
void pgraph_d3d11_set_draw_observe(pgraph_draw_observe_fn fn);
/* The texture bound to stage 0..3 (offset and format registers), for a
 * substitution filter on the pump thread. */
void pgraph_d3d11_tex_binding(int stage, uint32_t *offset, uint32_t *format);
/* The stage's TEXTURE_ADDRESS word (pump thread). */
uint32_t pgraph_d3d11_tex_address(int stage);
/* The stage's FORMAT, FILTER, ADDRESS and CONTROL0 words as the title set them (pump thread, diagnostics). */
void pgraph_d3d11_tex_state(int stage, uint32_t *format, uint32_t *filter, uint32_t *address, uint32_t *control0, int *enabled);

/* Statistics */
typedef struct {
    uint32_t frames;
    uint32_t draw_calls;
    uint32_t vertices_submitted;
    uint32_t methods_handled;
    uint32_t methods_ignored;
    uint32_t clears;
} PgraphD3D11Stats;

/* Present the frame.
 *
 * The title never emits NV097_FLIP_STALL -- it flips the way the hardware
 * actually does, by moving the CRTC scanout base. So the swap is signalled by a
 * write to NV_PCRTC_START, and pcrtc_write() calls this when the base changes.
 * Without it nothing the translator draws is ever shown: the back buffer
 * accumulates draws forever and the window keeps whatever was last composited.
 */
void pgraph_d3d11_present(uint32_t crtc_start);

/* Non-zero once the title has flipped its render surface, i.e. the frame just
 * built is complete and should be presented. Cleared by the caller. */
int  pgraph_d3d11_take_frame_complete(void);
int  pgraph_d3d11_flipping(void);

/* XBOX_TEST_QUAD diagnostic: draw a known-good quad through this device. */
void pgraph_d3d11_test_quad(void);

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out);

/* NV097_NO_OPERATION count and parameter histogram (fork). */
void pgraph_d3d11_report_nops(void);
void pgraph_d3d11_poll_reports(void);       /* pump thread, every tick */
void pgraph_d3d11_report_occlusion(void);   /* periodic stats */

/* Render pass phases from the pass_tags markers (see "Pass
 * phases" in nv2a_pgraph_d3d11.c). Without XBOX_PASS_TAGS=emit the phase
 * stays PGRAPH_PHASE_UNTAGGED and the callback is never called. The callback
 * runs on the pump thread, inside the method dispatch, when a marker changes
 * the phase: the hook for a 3D-only post-process is the change to
 * PGRAPH_PHASE_END (FRAME_END = end of the 3D of the frame). */
enum {
    PGRAPH_PHASE_UNTAGGED = 0,
    PGRAPH_PHASE_BEGIN,             /* FRAME_BEGIN, before any group */
    PGRAPH_PHASE_3D,                /* groups 0..6 */
    PGRAPH_PHASE_AFTER3D_PERSP,     /* groups >= 7, perspective view */
    PGRAPH_PHASE_HUD,               /* groups >= 7, orthographic view */
    PGRAPH_PHASE_END,               /* after FRAME_END */
    PGRAPH_PHASE_COUNT
};
typedef void (*pgraph_pass_phase_fn)(int old_phase, int new_phase, uint32_t tag);
void pgraph_d3d11_set_pass_phase_callback(pgraph_pass_phase_fn fn);
int  pgraph_d3d11_pass_phase(void);

/* Image end (fork, hook of an optional module): called on the pump thread at each
 * presented image, after the title's last draw of that image and before the
 * present, so what it draws on the default render target lies over the HUD
 * and the menus. NULL (the default) = nothing called. */
typedef void (*pgraph_image_end_fn)(void);
void pgraph_d3d11_set_image_end_callback(pgraph_image_end_fn fn);

/* Race HUD in its own proportions (the title side is
 * port/src/hud_anchor.c, which groups the HUD into elements and picks an
 * anchor for each). With the pass markers on, the title writes two HUD tags
 * before the records of each element; the HUD-phase draws that follow are
 * scaled by kx horizontally and ky vertically around the anchor, up to the
 * next tag or pass marker. kx = (4/3) / screen shape x HUD size, ky = HUD
 * size; both 1 or 0 (default) = off, nothing changes.
 * Tags: PGRAPH_HUD_TAG_MAGIC in the 16 high bits, bit 15 = scale, bits 0-14
 * = anchor x in 1/16 pixel of the title's 640x480 screen; then
 * PGRAPH_HUD_TAG_MAGIC_Y, bits 0-14 = anchor y, same unit. */
#define PGRAPH_HUD_TAG_MAGIC   0x4855u
#define PGRAPH_HUD_TAG_MAGIC_Y 0x4856u
void pgraph_d3d11_set_hud_scale(float kx, float ky);

/* Menus in a centred 16:9 frame beyond 16:9 (fork; the title side is
 * port/src/hud_anchor.c). PGRAPH_BOX_TAG_MAGIC in the 16 high bits, low
 * bits: 0 = the records that follow are drawn as is, 1 = framed (x scaled
 * by k about the centre, wide images only), 2 = the frame is shown whole
 * in a 16:9 frame (the host presents it at 16:9), 3 = the frame is wide,
 * 4 = the frame is shown whole in a centred 16:9 frame whatever the menus,
 * its 2D as in a wide image on a 16:9 screen (pgraph_d3d11_set_box169).
 * k = (16/9) / screen shape; 0 (default) = off, nothing changes. */
#define PGRAPH_BOX_TAG_MAGIC   0x4857u

/* Panels edge to edge (fork; the title side is port/src/hud_anchor.c,
 * "Panels edge to edge"). Two tags before the records of a panel's end
 * pieces: PGRAPH_PANEL_TAG_MAGIC, bits 0-13 = left edge, then
 * PGRAPH_PANEL_TAG_MAGIC_R, bits 0-13 = right edge, in 1/16 pixel of the
 * title's 640x480 screen. The HUD-phase draws that follow, up to the next
 * tag or pass marker, have their x at the 4:3 proportions about the centre
 * (k2d, or 0.75 in an image shown at 16:9; as drawn at 4:3, with neither),
 * except the vertices left of the
 * left edge (to x = 0) and right of the right edge (to the screen's
 * width); those draws take the CPU path, where a moved vertex gets the
 * attributes of its triangle's plane at its new place. First tag bit 15: wide images only (else drawn as is, like a
 * framed record) ; bit 14: 4:3 proportions only, no edge. */
#define PGRAPH_PANEL_TAG_MAGIC   0x4858u
#define PGRAPH_PANEL_TAG_MAGIC_R 0x4859u
void pgraph_d3d11_set_box(float k);

/* The HD HUD's gate of an image (fork; the title side is port/src/hud_anchor.c
 * hud_anchor_hd_gate): PGRAPH_HDGATE_TAG_MAGIC in the 16 high bits, bits 0-1
 * = the HDTEX_GATE_* bits (nv2a_hdtex.h), written at the start of each image;
 * they hold for the draws that follow, up to the next such marker. */
#define PGRAPH_HDGATE_TAG_MAGIC  0x485Au
/* The race's own 2D outside the race HUD (finish banner, pause, end
 * screens) at the title's 4:3 proportions: the framed records of a wide
 * image are scaled by k2d = (4/3) / screen shape about the centre instead
 * of k. Works at any shape wider than 4:3, with or without the 16:9 frame
 * (at 16:9 or 16:10 the title side writes the frame tags for it only, and
 * the presentation does not change). 0 (default) = off, k as before. */
void pgraph_d3d11_set_box_race2d(float k2d);
/* The shape of the frame a framed (not wide) image is shown in, for the
 * panels' content (PGRAPH_PANEL_TAG_MAGIC): kboxed = (4/3) / frame shape,
 * 0.75 for the 16:9 frame (default), 1 for the 4:3 frame (the menus at
 * 4:3: the image is already at the title's proportions). */
void pgraph_d3d11_set_box_frame(float kboxed);
/* Frame tag 4 (the title's staged shots in a race, wider than 16:9):
 * k169 = (16/9) / screen shape. Such an image is drawn as a wide one, its
 * 2D x-scales (race 2D, panels, proportional HUD) divided by k169, and the
 * host presents it squeezed by k169 into a centred 16:9 frame: as a wide
 * image on a 16:9 screen. 0 (default) = no such image. */
void pgraph_d3d11_set_box169(float k169);
/* How the terrain reads its lightmap (t1, an atlas of 8 x 8-texel tiles, one
 * texel per patch vertex): 0 the title's bilinear read, 1 bounded Catmull-
 * Rom (passes through the texels: the lighting is unchanged, only how finely
 * it is interpolated), 2 cubic B-spline (hidden: it does not pass through the
 * texels, so it moves the light on contrasted tiles), 5 the same B-spline
 * from exact texel loads (diagnostic). All kept inside the patch's tile.
 * Rendering only. Starts from XBOX_LIGHTMAP_FILTER=off|catmull|bspline|
 * bspline-ref (default off);
 * returns the mode now in use. */
int  pgraph_d3d11_set_lightmap_filter(int mode);
int  pgraph_d3d11_lightmap_filter(void);

#endif /* NV2A_PGRAPH_D3D11_H */
void pgraph_d3d11_present_guest_fb(uint32_t va, uint32_t pitch, uint32_t w, uint32_t h);
