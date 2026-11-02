/*
 * pad_sdl.h -- PlayStation controllers (DualShock 4, DualSense) without any
 * third-party tool, through SDL3 (port/third_party/sdl3, linked statically).
 * Experimental: the launcher's CONTROLS tab sets XBOX_PS_PADS=1
 * ([Controls] PlayStationControllers, off by default).
 *
 * XInput stays the path of every Xbox controller (controls.c). SDL is
 * started with its HIDAPI drivers for the PlayStation pads alone: its XInput,
 * DirectInput, raw input, Windows.Gaming.Input and GameInput back ends are
 * turned off, so it never sees, opens or reads an Xbox controller, and
 * nothing is read twice.
 *
 * The pads sit in slots: a pad keeps its slot until it is unplugged, and a
 * pad plugged in takes the first free one. Each pad has an id (never 0) that
 * no later pad shares, so a caller can tell a pad from the next one in the
 * same slot. controls.c gives each pad its player.
 *
 * Each pad is read into an XINPUT_GAMEPAD, the buttons by position: Cross =
 * A, Circle = B, Square = X, Triangle = Y, L1 / R1 = LB / RB, L2 / R2 = the
 * triggers (analog), Share / Create = Back, Options = Start, L3 / R3 = the
 * stick presses. So the player's bindings (controls.c) apply unchanged. The
 * PS button, the touchpad and the motion sensors are not read.
 *
 * XBOX_PS_PADS unset or not "1": every function says "no pad" at once and
 * SDL is never started (no thread, no device opened). Built with
 * -DSSX_PS_PADS=OFF (no SDL3 in the executable): the same, whatever the
 * variable says.
 */
#ifndef SSX_PAD_SDL_H
#define SSX_PAD_SDL_H

#include <windows.h>
#include <xinput.h>

#define PADSDL_SLOTS 4

int  padsdl_on(void);                                   /* XBOX_PS_PADS=1 (read once) */
int  padsdl_count(void);                                /* PlayStation pads open now */
void padsdl_list(DWORD id[PADSDL_SLOTS]);               /* each slot's pad id, 0: empty */
BOOL padsdl_state(int slot, DWORD id, XINPUT_GAMEPAD *out);   /* FALSE: that pad is gone */
void padsdl_rumble(int slot, DWORD id, WORD left, WORD right); /* XInput motor speeds */
void padsdl_set_player(int slot, DWORD id, int player); /* its lights: player 0..3, -1 none */
void padsdl_name(int slot, DWORD id, char *buf, size_t size);  /* "" if gone */
/* Before the process ends: every motor stopped and the pads closed, waited
 * for at most wait_ms; then the pads are no longer read. Nothing if the
 * option is off or SDL was never started. */
void padsdl_quit(DWORD wait_ms);

#endif /* SSX_PAD_SDL_H */
