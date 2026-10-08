/*
 * d3d8_sync.h -- presentation sync.
 *
 *   XBOX_SYNC=legacy   (default without the launcher) original chain: DXGI_SWAP_EFFECT_DISCARD,
 *                      Present(0) (XBOX_VSYNC=1: Present(1)), composed by the desktop
 *   XBOX_SYNC=off      flip model (FLIP_DISCARD, 3 buffers), Present(0) with
 *                      ALLOW_TEARING if the display allows it: no wait on the
 *                      display, tearing possible in full screen
 *   XBOX_SYNC=vsync    flip model, Present(1): one frame per refresh at most
 *   XBOX_SYNC=adaptive flip model, Present(1) when the frame is on time,
 *                      Present(0) + tearing when it is late (average pace
 *                      below the refresh rate) rather than waiting for the
 *                      next refresh
 *   XBOX_SYNC_LATENCY=N  (flip model) frames queued at most, 1..3, default 2;
 *                      wait on the latency object before each Present
 *   XBOX_PACING_LOG=1  [PACING] smoothness summary every 10 s and at exit
 *   XBOX_PACING_CSV=<path>  one line per Present (time, interval, waits,
 *                      DXGI frame statistics)
 *
 * Flip model refused by the system (creation fails): back to legacy, said
 * in the log. Everything happens on the presenting thread (pump).
 */
#ifndef D3D8_SYNC_H
#define D3D8_SYNC_H

#include <windows.h>
#include <dxgi.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { D3D8_SYNC_LEGACY, D3D8_SYNC_OFF, D3D8_SYNC_VSYNC, D3D8_SYNC_ADAPTIVE };

/* Chosen mode (reads the environment on the first call). */
int  d3d8_sync_mode(void);
/* Fills the chain description (effect, buffers, flags) for the mode. */
void d3d8_sync_swap_desc(DXGI_SWAP_CHAIN_DESC *scd);
/* Flip model creation failed: switch to legacy (the description is redone). */
void d3d8_sync_fallback(DXGI_SWAP_CHAIN_DESC *scd, HRESULT why);
/* Chain created: latency object, mode reminder in the log. */
void d3d8_sync_created(IDXGISwapChain *sc, HWND hwnd);
/* Flags to pass again to ResizeBuffers (the same as at creation). */
UINT d3d8_sync_swap_flags(void);
/* The window changed (size, full screen): read the display's refresh rate again. */
void d3d8_sync_window_changed(void);
/* Latency wait + Present + measurements. *dxgi_ms = duration of the Present alone. */
HRESULT d3d8_sync_present(IDXGISwapChain *sc, double *dxgi_ms);
/* [PACING] summary (window close: TerminateProcess, no atexit). */
void d3d8_sync_report(void);
/* Before the chain is released. */
void d3d8_sync_release(void);

/* Refresh rate (Hz, integer) of the display holding hwnd, or of the primary
 * display if hwnd is NULL; 0 if unknown. */
int  d3d8_monitor_hz(HWND hwnd);

#ifdef __cplusplus
}
#endif

#endif
