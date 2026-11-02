/*
 * hostui.h -- the game window's own keys (no menu bar: the settings are in
 * the launcher). See hostui.c.
 */
#ifndef SSX_HOSTUI_H
#define SSX_HOSTUI_H

#include "launcher.h"

/* Install the window's keys (F12, F11) before the title creates its device.
 * The window changes no setting; `persist_changes` = a start from the
 * player's settings.ini (the .txt of F12 then shows it), `cfg` is unused. */
void hostui_install(LauncherConfig *cfg, BOOL persist_changes);
/* A test start (persist_changes FALSE) that took its options from a
 * settings.ini: XBOX_FORK_INI as main.c found it, in the ANSI code page as
 * main.c opens it. The .txt of F12 then shows that file; without this call,
 * it says that no settings.ini was used. */
void hostui_test_ini(const char *ini);

#endif /* SSX_HOSTUI_H */
