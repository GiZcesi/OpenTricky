/*
 * xbox_perf.h -- per-zone profile of a frame.
 *
 *   XBOX_PERF=1            [PERF] summary every 2 s on stderr + report at exit
 *   XBOX_PERF_CSV=<path>   also, one line per frame (game thread: G, pump: P)
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
    PZ_N
};

enum {
    PC_DECODE, PC_COMPILE, PC_TEXUP, PC_TEXSIG, PC_KCALL,
    PC_CPUVTX, PC_CPUDRAW,              /* vertices / draws transformed on the CPU */
    PC_D3DSET, PC_D3DSKIP,              /* D3D11 state calls issued / skipped by the mirror */
    PC_GDRAW, PC_GMAP, PC_GGRP,         /* GPU program draws, their buffer Maps, their upload groups */
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

#ifdef __cplusplus
}
#endif
#endif
