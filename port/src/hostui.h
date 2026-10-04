/*
 * hostui.h -- the game window's own keys (no menu bar: the settings are in
 * the launcher). See hostui.c.
 */
#ifndef SSX_HOSTUI_H
#define SSX_HOSTUI_H

#include "launcher.h"

/* Install the window's keys (F12, F11) before the title creates its device.
 * `cfg` and `persist_changes` are kept for the callers; the window no longer
 * changes settings (they are all in the launcher). */
void hostui_install(LauncherConfig *cfg, BOOL persist_changes);

#endif /* SSX_HOSTUI_H */
