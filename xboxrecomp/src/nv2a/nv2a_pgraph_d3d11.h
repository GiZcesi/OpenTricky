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

/* Flush any pending draw commands (call at end of frame). */
void pgraph_d3d11_flush(void);

/* Set chyron scroll: pass frame counter to animate, 0 to disable.
 * Applies horizontal scroll offset to vertices in the chyron Y band. */
void pgraph_d3d11_set_chyron_scroll(uint32_t frame);

/* Hand the translator the base and size of the guest RAM window, so
 * DRAW_ARRAYS can follow SET_VERTEX_DATA_ARRAY_OFFSET into vertex buffers.
 * Called by the push buffer consumer, which already holds the mapping. */
void pgraph_d3d11_set_mem_base(void *base, uint32_t size);

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
 * in a 16:9 frame (the host presents it at 16:9), 3 = the frame is wide.
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

#endif /* NV2A_PGRAPH_D3D11_H */
void pgraph_d3d11_present_guest_fb(uint32_t va, uint32_t pitch, uint32_t w, uint32_t h);
