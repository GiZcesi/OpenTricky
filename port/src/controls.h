/*
 * controls.h -- button mapping for the keyboard and the PC controller.
 *
 * Every Xbox control can be bound to one keyboard key and (for buttons,
 * triggers and the D-pad) one button of an XInput controller. The sticks are
 * passed straight through on a controller; on the keyboard each stick
 * direction is a key. Bindings are stored in settings.ini ([Keyboard] and
 * [Controller], settings.h) and take effect immediately.
 */
#ifndef SSX_CONTROLS_H
#define SSX_CONTROLS_H

#include <windows.h>
#include <stdio.h>

enum {
    CTL_A, CTL_B, CTL_X, CTL_Y, CTL_BLACK, CTL_WHITE, CTL_LT, CTL_RT,
    CTL_START, CTL_BACK, CTL_L3, CTL_R3,
    CTL_DUP, CTL_DDOWN, CTL_DLEFT, CTL_DRIGHT,
    CTL_PAD_COUNT,                          /* controls a controller button can drive */
    CTL_LS_UP = CTL_PAD_COUNT, CTL_LS_DOWN, CTL_LS_LEFT, CTL_LS_RIGHT,
    CTL_RS_UP, CTL_RS_DOWN, CTL_RS_LEFT, CTL_RS_RIGHT,
    CTL_COUNT
};

/* Controller inputs a binding can name. */
enum {
    PAD_NONE, PAD_A, PAD_B, PAD_X, PAD_Y, PAD_LB, PAD_RB, PAD_LT, PAD_RT,
    PAD_START, PAD_BACK, PAD_LS, PAD_RS, PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT,
    PAD_COUNT
};

typedef struct ControlMap {
    BYTE key[CTL_COUNT];        /* virtual-key code, 0 = none */
    BYTE pad[CTL_PAD_COUNT];    /* PAD_*, PAD_NONE = none */
} ControlMap;

/* An Xbox gamepad state, as XInputGetState returns it on the console. */
typedef struct ControlsPad {
    WORD  buttons;              /* D-pad 0x1..0x8, Start 0x10, Back 0x20, L3 0x40, R3 0x80 */
    BYTE  analog[8];            /* A B X Y Black White LT RT, 0..255 */
    SHORT lx, ly, rx, ry;
} ControlsPad;

struct Settings;
void controls_defaults(ControlMap *m);
/* The bindings of settings.ini ([Keyboard] and [Controller], settings.h),
 * one per control in the order above; a key name not known is None. */
void controls_from_settings(ControlMap *m, const struct Settings *s);
void controls_to_settings(const ControlMap *m, struct Settings *s);

/* The live mapping the game reads (set once at start, and by the dialog). */
void controls_set_current(const ControlMap *m);
void controls_get_current(ControlMap *m);

/* Physical controller `port` through the mapping. FALSE if none is there. */
BOOL controls_read_pad(DWORD port, ControlsPad *out);
/* OR the keyboard's held bindings into `io` (port 0). */
void controls_read_keyboard(ControlsPad *io);

/* Game input off while one of our own windows (menu, dialog) is in use. */
void controls_set_suspended(BOOL on);
BOOL controls_suspended(void);

/* For the launcher's Controls page (it replaced the Controls
 * window). Readable names: a control is named after the PS2 button in its
 * place (the game uses the PS2 layout, ctlscheme.h). */
const WCHAR *controls_label(int ctl);                  /* "Cross", "L2", "Select", ... */
const WCHAR *controls_pad_label(int pad);              /* "Left bumper", ...; "" for PAD_NONE */
void controls_key_label(BYTE vk, WCHAR *out, int n);   /* "Space", "Q", ...; "" for none */
/* The key a WM_KEYDOWN / WM_SYSKEYDOWN names (left / right Shift and Ctrl
 * told apart), or 0 for the keys the game window keeps: Alt, F10, F11, F12,
 * the Windows keys. */
BYTE controls_key_from_msg(WPARAM wp, LPARAM lp);
/* Capturing a controller button: take a snapshot, then poll; inputs already
 * held at the snapshot are not reported. */
typedef struct ControlsPadSnap { WORD buttons[4]; BYTE trig[4][2]; } ControlsPadSnap;
void controls_pad_snap(ControlsPadSnap *s);
int  controls_pad_newly_pressed(ControlsPadSnap *s);   /* PAD_*, or PAD_NONE */

#endif /* SSX_CONTROLS_H */
