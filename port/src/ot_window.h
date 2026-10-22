/*
 * ot_window.h -- the game window on hosts without the Windows front end (macOS).
 *
 * Cocoa wants the window and its event loop on the process's main thread, so the game
 * (the translated code and everything it calls) runs on a second thread and the main
 * thread belongs to this module. Frames reach it from the thread that presents.
 *
 *   OT_WINDOW=0 (or XBOX_HEADLESS=1)   no window: the game runs on the main thread as before
 *   OT_WINDOW_SHOT=<file.ppm>          write what the window shows (after OT_WINDOW_SHOT_FRAMES
 *                                      frames, default 90) and carry on; a way to check the
 *                                      picture without looking at the screen
 *   SDL_VIDEODRIVER=dummy              no display needed (with OT_WINDOW_SHOT, for tests)
 */
#ifndef OT_WINDOW_H
#define OT_WINDOW_H

#include <stdint.h>

/* 1 when this build has a window and the environment does not turn it off. */
int ot_window_wanted(void);

/* Main thread: open the window, start `guest(arg)` on its own thread with a big stack, and run the
 * event loop until the guest returns or the window is closed. Returns the guest's result (0 when the
 * window was closed). */
int ot_window_run(int (*guest)(void *), void *arg);

/* Any thread: show this frame (RGBA, 4 bytes a pixel, rows packed). Copied; the caller keeps it. */
void ot_window_post_frame(const uint8_t *rgba, unsigned w, unsigned h);

#endif
