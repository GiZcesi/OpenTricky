/*
 * d3d8_sync.h -- presentation sync.
 *
 *   XBOX_SYNC=legacy   (default without the launcher) original chain: DXGI_SWAP_EFFECT_DISCARD,
 *                      Present(0) (XBOX_VSYNC=1: Present(1)), composed by the desktop
 *                      (Windows 11 may flip it: "Optimizations for windowed games")
 *   XBOX_SYNC=off      flip model (FLIP_DISCARD, 3 buffers), Present(0) with
 *                      ALLOW_TEARING if the display allows it: no wait on the
 *                      display, tearing possible in full screen (and in a window on a hardware plane)
 *   XBOX_SYNC=vsync    flip model, Present(1): one frame per refresh at most
 *   XBOX_SYNC=adaptive flip model, Present(1) when the frame is on time,
 *                      Present(0) + tearing when it is late while the game
 *                      stays slower than the refresh rate (or than the frame
 *                      rate limit, d3d8_sync_set_pace) over ~0.5 s, rather
 *                      than waiting for the next refresh; a load (no frame
 *                      for 60 ms) is not taken for a slow game, nor a video
 *                      (its frames come at the file's rate: VSync)
 *   XBOX_SYNC_LATENCY=N  (flip model) frames queued at most, 1..3, default 2;
 *                      wait on the latency object before each Present
 *   XBOX_RENDER_GATE=N (flip model, off by default) the wait moves to the
 *                      game's thread, before each render: at most N (1, 2)
 *                      frames on their way to the screen (d3d8_sync_gate)
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
/* The window changed (size, full screen): read the display's refresh rate
 * again, and how the frames reach the display (d3d8_sync_immediate). */
void d3d8_sync_window_changed(void);
/* Latency wait + Present + measurements. *dxgi_ms = duration of the Present alone. */
HRESULT d3d8_sync_present(IDXGISwapChain *sc, double *dxgi_ms);
/* O-B: fn is called by d3d8_sync_present after its waits, just
 * before the DXGI Present, on the presenting thread (NULL: none). */
void d3d8_sync_set_before_present(void (*fn)(void));
/* The next d3d8_sync_present carries a frame of the title's video
 * player (d3d8_device.c), which comes at the file's rate (29.97 fps), once per
 * frame -- not one of the game's frames at its frame rate limit. That present
 * only; presenting thread, right before it. */
void d3d8_sync_video_frame(int on);
/* 1 when the last d3d8_sync_present left its frame out (60 Hz displays: two
 * of the game's frames on one refresh), 0 when it presented it. */
int  d3d8_sync_left_out(void);
/* Any thread: 1 while frames are shown as soon as they are presented (VSync
 * off, or the late frames of Adaptive while the game is slower than its pace;
 * tearing allowed; flipped on their own, not composed by the desktop), 0
 * while each waits for a refresh of the display, or while that is unknown. A
 * variable refresh display (VRR), which shows frames as they come, is not
 * known here. */
int  d3d8_sync_immediate(void);
/* [PACING] summary (window close: TerminateProcess, no atexit). */
void d3d8_sync_report(void);
/* Before the chain is released. */
void d3d8_sync_release(void);

/* Frame rate limit of the game (fps, 0 = none): with Adaptive, frames that
 * keep up with a limit below the display are on time. Any thread. */
void d3d8_sync_set_pace(int fps);

/* The title's frame timer is on an exact grid (frame_clock.c). Set
 * once at start-up when that clock is on; the present grid then needs a
 * smaller margin (its frames arrive on an exact grid). */
void d3d8_sync_set_frame_clock(void);

/* Refresh rate (Hz, integer) of the display holding hwnd, or of the primary
 * display if hwnd is NULL; 0 if unknown. */
int  d3d8_monitor_hz(HWND hwnd);

/* Exact refresh rate (Hz) of the display holding hwnd (primary if NULL): the
 * signal's vertical sync frequency (239.98, 59.94...) from QueryDisplayConfig,
 * else the integer rate; 0 if unknown. *src (may be NULL): where it came from.
 * XBOX_TEST_DISPLAY_RATE=<Hz or N/D> (test only): that rate instead, the
 * display untouched (the frame rate limit follows it). */
double d3d8_monitor_rate(HWND hwnd, const char **src);
/* The exact rate of the game window's display as last read (0 before the
 * window exists); *gen (may be NULL) changes whenever it does. Any thread. */
double d3d8_display_rate(LONG *gen);
/* Window thread: display mode changed (moved = 0), or the window moved
 * (moved = 1: read again only if it is now on another display). */
void d3d8_sync_display_changed(int moved);

/* Render gate (XBOX_RENDER_GATE=1|2, flip model only, off by default): the
 * game's thread waits before each render (fps_cap.c) instead of the pump
 * before Present. gate: frames allowed on their way to the screen when a
 * render starts (0 = off); queued: frames presented, not yet on screen.
 * Any thread. */
int  d3d8_sync_gate(void);
int  d3d8_sync_queued(void);
long d3d8_sync_retired(void);
void d3d8_sync_gate_stats(double *stats_minus_count, unsigned long long *resync);

/* TEST ONLY: XBOX_TEST_DISPLAY_HZ=N switches the primary display to N Hz
 * (same size and depth) for the life of the process, to measure the pacing at
 * a refresh rate the desktop is not set to. Ignored when absent; never set by
 * the launcher or settings.ini. apply: before the refresh rate is read (start
 * of fps_cap_init); restore: at exit and from the crash filter (Windows also
 * restores a CDS_FULLSCREEN mode when the process ends). Both idempotent. */
void d3d8_test_display_hz_apply(void);
void d3d8_test_display_hz_restore(void);

#ifdef __cplusplus
}
#endif

#endif
