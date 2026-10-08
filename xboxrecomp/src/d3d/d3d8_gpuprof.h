/*
 * d3d8_gpuprof.h -- GPU time per pass of a frame.
 *
 *   XBOX_GPUPROF=1             [GPU] summary every 2 s on stderr + report at exit
 *   XBOX_GPUPROF_CSV=<path>    also, one line per frame
 *
 * Each draw, clear and post step is bracketed by two timestamp queries,
 * issued back to back with no CPU work in between: their gap is the GPU
 * execution time of that work alone, even when the GPU waits for the CPU
 * between two draws (which the "GPU" measure of XBOX_PERF, Present to
 * Present, does count). Gaps are summed per category; "span" = from the
 * first to the last timestamp of the frame (busy time + gaps). Read back
 * without waiting, NF frames later.
 *
 * Inert by default: without XBOX_GPUPROF, g_gpuprof_on is 0 and each
 * measurement point comes down to a test of that variable. Pump thread only.
 * Notable CPU cost when active (~2 queries per draw): do not measure FPS
 * with it.
 */
#ifndef D3D8_GPUPROF_H
#define D3D8_GPUPROF_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GP_G0, GP_G1, GP_G2, GP_G3, GP_G4, GP_G5, GP_G6,    /* 3D draws per group */
    GP_PERSP7,          /* draws groupes >= 7 en perspective (brume, lens flare) */
    GP_HUD,             /* draws groupes >= 7 ortho (HUD / menus) */
    GP_OTHER,           /* draws without a phase (no markers, videos, 2nd thread) */
    GP_CLEAR,           /* game clears */
    GP_POST_SPLIT,      /* FRAME_END: MSAA resolve + post chain + write back */
    GP_POST_PRESENT,    /* post chain at present (without split) */
    GP_PREVFRAME,       /* copy of the previous frame (chrome "Master") */
    GP_RESOLVE,         /* MSAA resolve at present */
    GP_BLIT,            /* copy / blit (+ DAC gamma) to the swap chain */
    GP_SOFTSHADOW,      /* ombres douces: masque stencil, flou, application */
    GP_N
};

extern int g_gpuprof_on;

void gpuprof_init(void);
/* Translator: current phase (PGRAPH_PHASE_*) and group of the last marker. */
void gpuprof_set_phase(int phase, int group);
int  gpuprof_draw_cat(void);            /* category of a game draw right now */
void gpuprof_begin(int cat);
void gpuprof_end(void);
void gpuprof_draw_begin(void);          /* game draws (mode 1: per run, 2: per draw) */
void gpuprof_draw_end(void);
void gpuprof_count_flush(void);         /* GetData that may have flushed the command buffer */
void gpuprof_note(unsigned bits);       /* "note" column of the frame (1 = SMAA stencil, 2 = direct write) */
void gpuprof_frame(void);               /* right before the DXGI Present */

#ifdef __cplusplus
}
#endif
#endif
