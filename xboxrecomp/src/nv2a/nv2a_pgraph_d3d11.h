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

#endif /* NV2A_PGRAPH_D3D11_H */
void pgraph_d3d11_present_guest_fb(uint32_t va, uint32_t pitch, uint32_t w, uint32_t h);
