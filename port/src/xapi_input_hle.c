/*
 * xapi_input_hle.c -- the title's XAPI controller functions, on the host.
 *
 * SSX Tricky links XAPI statically, so XInputGetState and friends are guest
 * code in the XPP section that drives the Xbox's own USB host controller and
 * XID driver. There is no USB hardware behind this port, so those functions
 * can never see a controller. They are replaced here, at their entry points,
 * by host code on top of xboxrecomp's Windows XInput layer
 * (xboxrecomp/src/input/xinput_device.c).
 *
 * The lifted bodies are still in recomp_0009.c, renamed `<name>_lifted`, so
 * the generated call sites and the dispatch table bind to the functions
 * below. Addresses and stack cleanup confirmed from the XBE (Ghidra names,
 * `ret N` of each body):
 *
 *   0x0017FF2C  XInitDevices(DWORD, PXDEVICE_PREALLOC_TYPE)      ret 8
 *               (0x00180C54 is a jmp thunk to it and stays lifted)
 *   0x00180916  XInputOpen(PXPP_DEVICE_TYPE, port, slot, poll)   ret 0x10
 *   0x0018098B  XInputClose(HANDLE)                              ret 4
 *   0x00180997  XInputGetCapabilities(HANDLE, PXINPUT_CAPS)      ret 8
 *   0x00180B89  XInputGetState(HANDLE, PXINPUT_STATE)            ret 8
 *   0x00180BFA  XInputSetState(HANDLE, PXINPUT_FEEDBACK)         ret 8
 *   0x00180C59  XGetDevices(PXPP_DEVICE_TYPE)                    ret 4
 *   0x00180C7B  XGetDeviceChanges(PXPP_DEVICE_TYPE, ins, rem)    ret 0xC
 *
 * Calling convention of a replacement: the caller pushed the arguments and a
 * dummy return address, so argument i is MEM32(g_esp + 4 + 4*i); the body
 * sets g_eax and pops return address plus arguments, as `ret N` would.
 *
 * Port 0 also takes the keyboard while the game window has focus, so the
 * title is drivable without a pad. Both the keyboard and the controller go
 * through the player's bindings (controls.c); by default Enter =
 * Start, Space = A, Esc = B, C = X, V = Y, Tab = Back, Q/E = triggers,
 * R/F = Black/White, X = L3, arrows = D-pad and left stick.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "netplay/np_racebench.h"
#include <stdlib.h>

#include "recomp/recomp_types.h"
#include "controls.h"

extern __thread uint32_t g_eax;
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/* xboxrecomp/src/input -- declared here rather than including its header,
 * which pulls in the platform's Xbox-flavoured winnt definitions. */
typedef struct {
    WORD  wButtons;
    BYTE  bAnalogButtons[8];
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} hle_pad_t;
typedef struct { DWORD dwPacketNumber; hle_pad_t Gamepad; } hle_state_t;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } hle_vib_t;
DWORD xbox_InputGetState(DWORD dwPort, hle_state_t *pState);
DWORD xbox_InputSetState(DWORD dwPort, const hle_vib_t *pVibration);

#define XPP_TYPE_GAMEPAD   0x0017FB40u   /* g_DeviceType_Gamepad */
#define NPORTS             4
#define ERR_NOT_CONNECTED  0x48Fu        /* ERROR_DEVICE_NOT_CONNECTED */
#define ERR_IO_PENDING     0x3E5u

static uint32_t g_handle[NPORTS];        /* guest VA of each open device */
static int      g_pad_present[NPORTS];
static DWORD    g_pad_checked[NPORTS];   /* GetTickCount of the last probe */

static void hle_return(uint32_t eax, unsigned arg_bytes)
{
    g_eax = eax;
    g_esp += 4 + arg_bytes;
}

static void hle_trace(const char *what, uint32_t a, uint32_t b)
{
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 24) {
        fprintf(stderr, "  [XINPUT] %s 0x%08X 0x%08X\n", what, a, b);
        fflush(stderr);
    }
}

/* XInputGetState on an empty slot is slow, so a slot that was empty is
 * re-probed at most once a second. */
static int pad_connected(unsigned port)
{
    DWORD now = GetTickCount();
    if (g_pad_present[port] || now - g_pad_checked[port] >= 1000) {
        hle_state_t s;
        g_pad_checked[port] = now;
        g_pad_present[port] = (xbox_InputGetState(port, &s) == 0);
    }
    return g_pad_present[port];
}

/* XBOX_INPUT_HOST=0: ignore the physical pads and the keyboard (scripted
 * XBOX_INPUT_AUTOPRESS still works). XInput reads a pad whatever window has
 * focus, so someone playing while a test runs changes what the test measures
 * -- a skipped video moved every captured frame and made a crash appear in
 * some gate runs and not others. The audit tools set this. */
static int host_input_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("XBOX_INPUT_HOST");
        v = !(e && e[0] == '0');
    }
    return v;
}

/* Test only: XBOX_INPUT_PAD2=1 plugs a virtual pad into port 1
 * that gets the same scripted presses as port 0, so a run can reach the
 * two-player split screen. Off by default. */
static int pad2_test(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("XBOX_INPUT_PAD2"); v = e && e[0] == '1'; }
    return v;
}

static uint32_t gamepad_mask(void)
{
    uint32_t m = pad2_test() ? 3u : 1u;   /* port 0: pad or keyboard */
    unsigned p;
    for (p = 1; host_input_enabled() && p < NPORTS; p++)
        if (pad_connected(p)) m |= 1u << p;
    return m;
}

static int port_of(uint32_t handle)
{
    int p;
    if (!handle) return -1;
    for (p = 0; p < NPORTS; p++)
        if (g_handle[p] == handle) return p;
    return -1;
}

/* XBOX_INPUT_AUTOPRESS=A@20,START@35,DOWN@40 -- hold a button on port 0 for
 * 300 ms at each time (seconds after the first poll). Lets a test run walk
 * through screens without anyone at the controller. Names: A B X Y BLACK
 * WHITE LT RT START BACK UP DOWN LEFT RIGHT L3 R3, and LSLEFT LSRIGHT LSUP
 * LSDOWN (the left stick pushed all the way; UP..RIGHT are the D-pad);
 * several joined by '+' are held together ("LT+WHITE@30").
 *
 * NAME@T+DxN repeats: N presses, D seconds apart, from T -- "A@18+1.5x40"
 * taps A through every default menu choice to a race in about a minute
 * instead of the two that spaced single presses took. */
static const struct { const char *name; WORD bit; int analog; } names[] = {
    { "A", 0, 0 }, { "B", 0, 1 }, { "X", 0, 2 }, { "Y", 0, 3 },
    { "BLACK", 0, 4 }, { "WHITE", 0, 5 }, { "LT", 0, 6 }, { "RT", 0, 7 },
    { "START", 0x10, -1 }, { "BACK", 0x20, -1 }, { "UP", 0x01, -1 },
    { "DOWN", 0x02, -1 }, { "LEFT", 0x04, -1 }, { "RIGHT", 0x08, -1 },
    { "L3", 0x40, -1 }, { "R3", 0x80, -1 },
    /* analog >= STICK: the left stick, analog - STICK = left, right, up, down */
    { "LSLEFT", 0, 100 }, { "LSRIGHT", 0, 101 }, { "LSUP", 0, 102 }, { "LSDOWN", 0, 103 },
};
#define STICK 100
#define NNAMES (sizeof(names) / sizeof(names[0]))

/* Live presses from the diag port (`press A 150`): the tick each button is
 * held until. Written by the diag thread, read by the game's poll. */
static volatile DWORD g_live_until[NNAMES];

int xinput_hle_press(const char *name, int ms)
{
    unsigned k;
    for (k = 0; k < NNAMES; k++)
        if (!_stricmp(name, names[k].name)) {
            g_live_until[k] = GetTickCount() + (DWORD)(ms > 0 ? ms : 150);
            return 1;
        }
    return 0;
}

/* `list` ("LT+WHITE") names `one`. */
#define AUTOPRESS_MAX 2048            /* held combinations repeat every 250 ms in long scripts */

static int name_has(const char *list, const char *one)
{
    size_t n = strlen(one);
    while (*list) {
        const char *e = strchr(list, '+');
        size_t len = e ? (size_t)(e - list) : strlen(list);
        if (len == n && !_strnicmp(list, one, n)) return 1;
        if (!e) break;
        list = e + 1;
    }
    return 0;
}

static void autopress_hold(hle_state_t *s, WORD bit, int analog)
{
    static const SHORT x[4] = { -32767, 32767, 0, 0 }, y[4] = { 0, 0, 32767, -32767 };
    if (analog >= STICK) {
        if (x[analog - STICK]) s->Gamepad.sThumbLX = x[analog - STICK];
        if (y[analog - STICK]) s->Gamepad.sThumbLY = y[analog - STICK];
    } else if (analog >= 0) s->Gamepad.bAnalogButtons[analog] = 0xFF;
    else s->Gamepad.wButtons |= bit;
}

static void autopress_into(hle_state_t *s)
{
    static int parsed = 0, n = 0;
    static struct { DWORD at_ms; WORD bit; int analog; } ev[AUTOPRESS_MAX];
    static DWORD t0 = 0;
    DWORD now = GetTickCount();
    int i;
    unsigned k;
    for (k = 0; k < NNAMES; k++)
        if ((LONG)(g_live_until[k] - now) > 0)
            autopress_hold(s, names[k].bit, names[k].analog);
    if (!parsed) {
        const char *e = getenv("XBOX_INPUT_AUTOPRESS");
        parsed = 1;
        t0 = now;
        while (e && *e && n < AUTOPRESS_MAX) {
            char name[48]; double sec;
            k = 0;
            while (*e && *e != '@' && k < sizeof(name) - 1) name[k++] = *e++;
            name[k] = 0;
            if (*e != '@') break;
            sec = strtod(e + 1, (char **)&e);
            {
                double step = 0.0;
                long reps = 1, r;
                if (*e == '+') {
                    step = strtod(e + 1, (char **)&e);
                    if (*e == 'x' || *e == 'X') reps = strtol(e + 1, (char **)&e, 10);
                    if (reps < 1) reps = 1;
                }
                for (r = 0; r < reps && n < AUTOPRESS_MAX; r++)
                    for (k = 0; k < NNAMES; k++)
                        if (name_has(name, names[k].name) && n < AUTOPRESS_MAX) {
                            ev[n].at_ms = (DWORD)((sec + step * (double)r) * 1000.0);
                            ev[n].bit = names[k].bit;
                            ev[n].analog = names[k].analog;
                            n++;
                        }
            }
            if (*e == ',') e++;
        }
        if (n) { fprintf(stderr, "  [XINPUT] autopress: %d event(s)\n", n); fflush(stderr); }
    }
    for (i = 0; i < n; i++) {
        DWORD t = now - t0;
        if (t >= ev[i].at_ms && t < ev[i].at_ms + 300)
            autopress_hold(s, ev[i].bit, ev[i].analog);
    }
}

/* ---- the replaced entry points ---- */

void sub_0017FF2C(void)   /* XInitDevices -- no USB stack to bring up */
{
    hle_trace("XInitDevices", MEM32(g_esp + 4), MEM32(g_esp + 8));
    hle_return(0, 8);
}

void sub_00180C59(void)   /* XGetDevices */
{
    uint32_t type = MEM32(g_esp + 4);
    uint32_t mask = (type == XPP_TYPE_GAMEPAD) ? gamepad_mask() : 0u;
    if (type) {                            /* what the real one leaves behind */
        MEM32(type + 0) = mask;
        MEM32(type + 4) = 0;
        MEM32(type + 8) = mask;
    }
    hle_return(mask, 4);
}

void sub_00180C7B(void)   /* XGetDeviceChanges */
{
    uint32_t type = MEM32(g_esp + 4), pins = MEM32(g_esp + 8), prem = MEM32(g_esp + 12);
    uint32_t cur = (type == XPP_TYPE_GAMEPAD) ? gamepad_mask() : 0u;
    uint32_t prev = type ? MEM32(type + 8) : 0u;
    uint32_t ins = cur & ~prev, rem = prev & ~cur;
    if (pins) MEM32(pins) = ins;
    if (prem) MEM32(prem) = rem;
    if (type) { MEM32(type + 0) = cur; MEM32(type + 4) = 0; MEM32(type + 8) = cur; }
    hle_return((ins | rem) ? 1u : 0u, 12);
}

void sub_00180916(void)   /* XInputOpen */
{
    uint32_t type = MEM32(g_esp + 4), port = MEM32(g_esp + 8);
    uint32_t h = 0;
    if (type == XPP_TYPE_GAMEPAD && port < NPORTS && (gamepad_mask() & (1u << port))) {
        if (!g_handle[port]) g_handle[port] = xbox_HeapAlloc(0x100, 16);
        h = g_handle[port];
    }
    hle_trace("XInputOpen port/handle", port, h);
    hle_return(h, 16);
}

void sub_0018098B(void)   /* XInputClose -- the handle block is kept for reuse */
{
    hle_return(0, 4);
}

void sub_00180997(void)   /* XInputGetCapabilities */
{
    uint32_t h = MEM32(g_esp + 4), caps = MEM32(g_esp + 8);
    int port = port_of(h);
    uint32_t rc = ERR_NOT_CONNECTED;
    if (port >= 0 && caps) {
        int i;
        /* XINPUT_CAPABILITIES: BYTE SubType; WORD Reserved;
         * XINPUT_GAMEPAD In (18 bytes); XINPUT_RUMBLE Out (4 bytes) = 25 */
        for (i = 0; i < 25; i++) MEM8(caps + i) = 0;
        MEM8(caps + 0) = 1;                          /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
        MEM16(caps + 3) = 0x00FF;                    /* every digital button */
        for (i = 0; i < 8; i++) MEM8(caps + 5 + i) = 0xFF;
        for (i = 0; i < 4; i++) MEM16(caps + 13 + 2 * i) = 0xFFFF;
        MEM16(caps + 21) = 0xFFFF;                   /* both rumble motors */
        MEM16(caps + 23) = 0xFFFF;
        rc = 0;
    }
    hle_return(rc, 8);
}

void sub_00180B89(void)   /* XInputGetState */
{
    uint32_t h = MEM32(g_esp + 4), ps = MEM32(g_esp + 8);
    int port = port_of(h);
    hle_state_t s;
    uint32_t rc = ERR_NOT_CONNECTED;
    memset(&s, 0, sizeof(s));
    if (port >= 0) {
        ControlsPad cp;
        memset(&cp, 0, sizeof cp);
        if (host_input_enabled() && controls_read_pad((DWORD)port, &cp)) rc = 0;
        else g_pad_present[port] = 0;
        if (port == 0) {
            if (host_input_enabled()) controls_read_keyboard(&cp);
            rc = 0;
        }
        s.Gamepad.wButtons = cp.buttons;
        memcpy(s.Gamepad.bAnalogButtons, cp.analog, 8);
        s.Gamepad.sThumbLX = cp.lx;
        s.Gamepad.sThumbLY = cp.ly;
        s.Gamepad.sThumbRX = cp.rx;
        s.Gamepad.sThumbRY = cp.ry;
        if (port == 0) autopress_into(&s);
        if (port == 0 && np_rb_pad_muted()) memset(&s.Gamepad, 0, sizeof s.Gamepad);
        if (port == 1 && pad2_test()) {
            rc = 0;
            autopress_into(&s);
        }
    }
    if (rc == 0 && ps) {
        int i;
        static DWORD packet = 0;
        static WORD last_buttons = 0xFFFF;
        MEM32(ps + 0) = ++packet;
        MEM16(ps + 4) = s.Gamepad.wButtons;
        for (i = 0; i < 8; i++) MEM8(ps + 6 + i) = s.Gamepad.bAnalogButtons[i];
        MEM16(ps + 14) = (uint16_t)s.Gamepad.sThumbLX;
        MEM16(ps + 16) = (uint16_t)s.Gamepad.sThumbLY;
        MEM16(ps + 18) = (uint16_t)s.Gamepad.sThumbRX;
        MEM16(ps + 20) = (uint16_t)s.Gamepad.sThumbRY;
        {   /* one line per change of what is held, so input is visible */
            WORD held = s.Gamepad.wButtons;
            for (i = 0; i < 8; i++) if (s.Gamepad.bAnalogButtons[i] > 30) held |= (WORD)(0x100u << i);
            if (held != last_buttons && port == 0) {
                last_buttons = held;
                /* Its own budget: sharing hle_trace's 24-line cap with the
                 * init chatter hid every press after the fifth, which read as
                 * the game no longer polling input. */
                static LONG held_lines = 0;
                if (held && InterlockedIncrement(&held_lines) <= 400) {
                    fprintf(stderr, "  [XINPUT] input port0 held 0x%08X\n", (unsigned)held);
                    fflush(stderr);
                }
            }
        }
    }
    hle_return(rc, 8);
}

void sub_00180BFA(void)   /* XInputSetState -- rumble, completed at once */
{
    uint32_t h = MEM32(g_esp + 4), fb = MEM32(g_esp + 8);
    int port = port_of(h);
    if (port >= 0 && fb) {
        hle_vib_t v;
        v.wLeftMotorSpeed  = MEM16(fb + 0x42);
        v.wRightMotorSpeed = MEM16(fb + 0x44);
        xbox_InputSetState((DWORD)port, &v);
        MEM32(fb + 0) = 0;       /* Header.dwStatus: done (the title polls it) */
    }
    hle_return(port >= 0 ? ERR_IO_PENDING : ERR_NOT_CONNECTED, 8);
}
