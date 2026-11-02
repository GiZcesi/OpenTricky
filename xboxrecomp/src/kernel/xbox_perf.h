/*
 * xbox_perf.h -- per-zone profile of a frame.
 *
 *   XBOX_PERF=1            [PERF] summary every 2 s on stderr + report at exit
 *   XBOX_PERF_CSV=<path>   also, one line per frame (game thread: G, pump: P)
 *   XBOX_PERF_DETAIL=1     also the sub-zones of a GPU draw, the counts of
 *                          draws a merge could join, of the texture binds
 *                          skipped and of the draws whose constants needed no
 *                          Map (line R), of the sampler memo's hits and
 *                          of the D3D8 state writes skipped (line R) --
 *                          they cost time on every draw, method word and
 *                          state write, and the skip counts on the skipping
 *                          side only: XBOX_PERF alone costs the same with and
 *                          without the pump optimizations
 *   XBOX_PERF_METHODS=<path>  the method histogram of the run, rewritten there
 *                          every 2 s (also on stderr at exit)
 *
 * Inert by default: without XBOX_PERF, g_perf_on is 0 and each measurement
 * point comes down to a test of that variable.
 *
 * Game thread (hooks port/src/perf_hooks.c): tick (vt+0x14), render
 * (vt+0x18), wait on the frame event (0xB2750), and the time spent in kernel
 * calls during each of these zones (kernel_bridge.c dispatcher). Pump:
 * pushbuffer translation (nv2a_live_pb_tick), host_present (post + blit +
 * DXGI Present), DXGI Present alone, and a GPU time (timestamp queries
 * between two Presents). Counters per frame: draws, methods, vertices,
 * decoded programs, compilations, texture uploads, texture signatures.
 *
 * GPU program draws (line M of the CSV, and the [PERF] "uploads" line):
 * draws, buffer Map calls they made, upload groups (vertex slots), and a
 * histogram of groups per draw.
 *   XBOX_PERF_MAPTIME=1    also time each Map and Unmap of those draws
 *                          (rdtsc, minus its own overhead): microseconds
 *                          per Map/Unmap pair. Costs ~2 rdtsc per call: off
 *                          for FPS measurements.
 */
#ifndef XBOX_PERF_H
#define XBOX_PERF_H

#ifdef __cplusplus
extern "C" {
#endif

extern int g_perf_on;
/* XBOX_PERF_DETAIL=1 (with XBOX_PERF=1): also the PZ_PREP..PZ_DPRE
 * sub-zones of a GPU draw -- about ten more clock reads per draw, so off
 * unless asked (the other zones keep their cost). */
extern int g_perf_detail;

enum {
    /* game thread */
    PZ_TICK, PZ_RENDER, PZ_FWAIT,
    /* pump */
    PZ_PB, PZ_PRESENT, PZ_DXGI,
    /* pump, in d3d8_nv2a_draw_program_gpu: vertex+index uploads, constants, states+IA/VS/PS, DrawIndexed */
    PZ_DUP, PZ_DCB, PZ_DSTATE, PZ_DDRAW,
    /* pump, translation: textures (signature + upload), PS key + lookup, VS decode,
     * whole draw_program for the CPU (particles) and GPU draws */
    PZ_TEX, PZ_PSH, PZ_VSDEC, PZ_PROGCPU, PZ_PROGGPU,
    /* pump, inside draw_program (the "rest" of a GPU draw, line R of the CSV):
     * gpu_prepare (index list, streams), the D3D8 state and texture writes
     * (textures included, PZ_TEX is part of it), the pixel-shader state and
     * constants up to its key, the ZTIE / ZDECAL / soft-cut block, the fork's
     * per-draw hooks (HUD, lights, temporal, culling stats), and in
     * d3d8_nv2a_draw_program_gpu what comes before the uploads (VS lookup,
     * stream grouping) */
    PZ_PREP, PZ_STATEW, PZ_PSB, PZ_ZT, PZ_HOOK, PZ_DPRE,
    PZ_N
};

enum {
    PC_DECODE, PC_COMPILE, PC_TEXUP, PC_TEXSIG, PC_KCALL,
    PC_CPUVTX, PC_CPUDRAW,              /* vertices / draws transformed on the CPU */
    PC_D3DSET, PC_D3DSKIP,              /* D3D11 state calls issued / skipped by the mirror */
    PC_GDRAW, PC_GMAP, PC_GGRP,         /* GPU program draws, their buffer Maps, their upload groups */
    /* line R of the CSV: index words (ARRAY_ELEMENT16), the VS constants of a
     * GPU draw (generation unchanged since the last draw / bytes equal /
     * changed), state objects created (blend, depth-stencil, rasteriser,
     * sampler), sampler lookups, texture binds (PSSetShaderResources), and
     * consecutive program draws with no method but indices / streams / begin-
     * end between them (all / both plain: GPU triangles, no split, no mark) */
    PC_IDXW, PC_VSC_GEN, PC_VSC_EQ, PC_VSC_CHG, PC_SOBJ, PC_SMPL, PC_SRV, PC_MERGE, PC_MERGEP,
    PC_CBSKIP,                          /* GPU draws whose constants needed no Map (XBOX_FIX_PUMP_CGEN; XBOX_PERF_DETAIL only) */
    PC_SRVSKIP,                         /* texture binds skipped by the mirror (XBOX_FIX_PUMP_SRV; XBOX_PERF_DETAIL only) */
    PC_MERGES,                          /* GPU draws binding the same state and constants as the previous one */
    PC_VSC_CMP,                         /* VS constants compared with the GPU path's copy (16 bytes each) */
    PC_SMEMO,                           /* samplers bound from the per-stage memo (XBOX_FIX_PUMP_SMEMO; XBOX_PERF_DETAIL only) */
    PC_RSKIP,                           /* D3D8 render / stage states not set again (XBOX_FIX_PUMP_RSKIP; XBOX_PERF_DETAIL only) */
    PC_N
};

void   perf_init(void);
double perf_now(void);                  /* ms (QPC) */

/* Game thread: nestable zones (the current zone gets the kernel time). */
int    perf_zone_enter(int zone);       /* returns the previous zone */
void   perf_zone_leave(int zone, int prev, double ms);
void   perf_kernel(unsigned ordinal, double ms);   /* from the kernel dispatcher */
void   perf_game_frame_end(int nticks);

/* Pump */
void   perf_add(int zone, double ms);
void   perf_count(int c, unsigned n);
void   perf_present_done(double gpu_ms_prev);   /* end of host_present */

/* GPU program draw: `maps` buffer Maps, `groups` vertex upload groups. */
void   perf_gpu_draw(unsigned maps, unsigned groups);

/* XBOX_PERF_MAPTIME: g_perf_maptime is 1 when on; callers add the rdtsc
 * cycles spent inside Map / Unmap and the number of pairs. */
extern int g_perf_maptime;
extern unsigned long long g_perf_map_cyc, g_perf_map_pairs;

/* XBOX_PERF_CSV, line V: the vertex/index ring for one presented frame
 * (wraps, DISCARDs, DISCARDs for lack of a completed query, draws larger
 * than the ring, 1 if the lap shares the memory of the previous one). */
void   perf_ring_frame(unsigned wraps, unsigned discards, unsigned fallbacks, unsigned refused, int shared);

#ifdef __cplusplus
}
#endif
#endif
