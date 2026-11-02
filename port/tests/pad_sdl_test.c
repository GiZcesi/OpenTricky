/*
 * pad_sdl_test.c -- host test of the PlayStation controllers (port/src/pad_sdl.c)
 * and of how controls.c reads them, with no controller at hand: the pads are
 * SDL's virtual joysticks (Sony's ids for the PlayStation ones), XInput is a
 * stand-in whose indexes the test plugs and unplugs. No game, no window.
 *
 *   cmake -DSSX_BUILD_TESTS=ON ... ; cmake --build ... --target pad_sdl_test
 *   pad_sdl_test.exe off       XBOX_PS_PADS unset: XInput only, SDL never started
 *   pad_sdl_test.exe on        with XBOX_PS_PADS=1 in the environment
 *
 * Built with -DXInputGetState=pad_test_XInputGetState and -DXInputSetState=
 * pad_test_XInputSetState, so controls.c asks the stand-in below instead of
 * the real XInput.
 */
#include <windows.h>
#include <xinput.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "controls.h"
#include "pad_sdl.h"

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } } while (0)

/* controls.c's keyboard reading asks for the game window: none here. */
HWND d3d8_GetHostWindow(void) { return NULL; }

/* ── the stand-in XInput ── */
static volatile LONG s_xi_on[4];                /* plugged in */
static XINPUT_GAMEPAD s_xi_pad[4];              /* its state */
static volatile LONG s_xi_calls[4], s_xi_sets[4];
static volatile LONG s_xi_left[4], s_xi_right[4];

DWORD WINAPI pad_test_XInputGetState(DWORD port, XINPUT_STATE *st)
{
    if (port >= 4) return ERROR_DEVICE_NOT_CONNECTED;
    InterlockedIncrement(&s_xi_calls[port]);
    if (!s_xi_on[port]) return ERROR_DEVICE_NOT_CONNECTED;
    memset(st, 0, sizeof *st);
    st->Gamepad = s_xi_pad[port];
    return ERROR_SUCCESS;
}

DWORD WINAPI pad_test_XInputSetState(DWORD port, XINPUT_VIBRATION *v)
{
    if (port >= 4) return ERROR_DEVICE_NOT_CONNECTED;
    InterlockedIncrement(&s_xi_sets[port]);
    if (!s_xi_on[port]) return ERROR_DEVICE_NOT_CONNECTED;
    InterlockedExchange(&s_xi_left[port], v->wLeftMotorSpeed);
    InterlockedExchange(&s_xi_right[port], v->wRightMotorSpeed);
    return ERROR_SUCCESS;
}

static void xi_set(int port, int on, WORD buttons)
{
    s_xi_on[port] = on;
    memset(&s_xi_pad[port], 0, sizeof s_xi_pad[port]);
    s_xi_pad[port].wButtons = buttons;
}

static LONG xi_calls(void) { return s_xi_calls[0] + s_xi_calls[1] + s_xi_calls[2] + s_xi_calls[3]; }
static LONG xi_sets(void) { return s_xi_sets[0] + s_xi_sets[1] + s_xi_sets[2] + s_xi_sets[3]; }

static int thread_count(void)
{
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentProcessId();
    int n = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    te.dwSize = sizeof te;
    if (Thread32First(h, &te))
        do { if (te.th32OwnerProcessID == me) n++; te.dwSize = sizeof te; } while (Thread32Next(h, &te));
    CloseHandle(h);
    return n;
}

/* ── virtual pads ── */
typedef struct {
    SDL_JoystickID id; SDL_Joystick *js;
    volatile LONG rumbles, low, high, player;
} VPad;

static bool SDLCALL on_rumble(void *userdata, Uint16 low, Uint16 high)
{
    VPad *v = (VPad *)userdata;
    InterlockedExchange(&v->low, low);
    InterlockedExchange(&v->high, high);
    InterlockedIncrement(&v->rumbles);
    return true;
}

static void SDLCALL on_player(void *userdata, int player)
{
    InterlockedExchange(&((VPad *)userdata)->player, player);
}

static int vpad_attach(VPad *v, Uint16 vendor, Uint16 product, const char *name)
{
    SDL_VirtualJoystickDesc d;
    memset(v, 0, sizeof *v);
    v->player = -99;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = vendor;
    d.product_id = product;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;                  /* in SDL_GamepadAxis order */
    d.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;    /* SDL_GamepadButton order */
    d.name = name;
    d.userdata = v;
    d.Rumble = on_rumble;
    d.SetPlayerIndex = on_player;
    v->id = SDL_AttachVirtualJoystick(&d);
    v->js = v->id ? SDL_OpenJoystick(v->id) : NULL;
    if (!v->js) printf("  virtual pad %s: %s\n", name, SDL_GetError());
    /* triggers released (SDL's virtual axes start at 0, a trigger halfway) */
    if (v->js) {
        SDL_SetJoystickVirtualAxis(v->js, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
        SDL_SetJoystickVirtualAxis(v->js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    }
    return v->js != NULL;
}

static void vpad_detach(VPad *v)
{
    if (v->js) SDL_CloseJoystick(v->js);
    if (v->id) SDL_DetachVirtualJoystick(v->id);
    v->js = NULL;
    v->id = 0;
}

/* Waits (at most ms) for the pad thread to publish `n` PlayStation pads. */
static int wait_count(int n, int ms)
{
    DWORD t0 = GetTickCount();
    while (padsdl_count() != n) {
        if ((int)(GetTickCount() - t0) > ms) return 0;
        Sleep(5);
    }
    return 1;
}

/* Waits for port `port` to read as `want` through controls.c (the pad
 * thread polls every 2 ms; the game's reads see its last snapshot). */
static int wait_read(DWORD port, int (*want)(const ControlsPad *), ControlsPad *cp, int ms)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        BOOL ok = controls_read_pad(port, cp);
        if (ok && want(cp)) return 1;
        if ((int)(GetTickCount() - t0) > ms) return 0;
        Sleep(2);
    }
}

/* At rest an axis reads 0, or -1 on the Y axes (SDL's 0 turned up as ~0,
 * pad_sdl.c axis()). */
static int rest(int v) { return v == 0 || v == -1; }

static int is_idle(const ControlsPad *c)
{
    int i;
    for (i = 0; i < 8; i++) if (c->analog[i]) return 0;
    return !c->buttons && !c->lx && rest(c->ly) && !c->rx && rest(c->ry);
}

/* One input of the virtual pad and what the game must read for it with the
 * default bindings (controls.c): the console's analog button (A B X Y Black
 * White LT RT = 0..7), its digital bits, or a stick. */
typedef struct { const char *name; int button, axis, value; int analog, bits; int stick; SHORT sv; } Case;
static const Case *s_case;
static int s_queued_max;

static int matches_case(const ControlsPad *c)
{
    int i;
    for (i = 0; i < 8; i++)
        if (c->analog[i] != (i == s_case->analog ? 255 : 0)) return 0;
    if (c->buttons != s_case->bits) return 0;
    switch (s_case->stick) {
    case 1: return c->lx == s_case->sv && rest(c->ly) && !c->rx && rest(c->ry);
    case 2: return c->ly == s_case->sv && !c->lx && !c->rx && rest(c->ry);
    case 3: return c->rx == s_case->sv && !c->lx && rest(c->ly) && rest(c->ry);
    case 4: return c->ry == s_case->sv && !c->lx && rest(c->ly) && !c->rx;
    default: return !c->lx && rest(c->ly) && !c->rx && rest(c->ry);
    }
}

static void vpad_set(VPad *v, const Case *k, int on)
{
    if (k->button >= 0) {
        SDL_SetJoystickVirtualButton(v->js, k->button, on != 0);
    } else {
        int trig = k->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || k->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        SDL_SetJoystickVirtualAxis(v->js, k->axis, (Sint16)(on ? k->value : trig ? SDL_JOYSTICK_AXIS_MIN : 0));
    }
}

static const Case k_cases[] = {
    /* the buttons by position: Cross = A, Circle = B, Square = X, Triangle = Y */
    { "Cross", SDL_GAMEPAD_BUTTON_SOUTH, -1, 0, 0, 0, 0, 0 },
    { "Circle", SDL_GAMEPAD_BUTTON_EAST, -1, 0, 1, 0, 0, 0 },
    { "Square", SDL_GAMEPAD_BUTTON_WEST, -1, 0, 2, 0, 0, 0 },
    { "Triangle", SDL_GAMEPAD_BUTTON_NORTH, -1, 0, 3, 0, 0, 0 },
    /* R1 = RB = the console's Black, L1 = LB = White */
    { "R1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -1, 0, 4, 0, 0, 0 },
    { "L1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, -1, 0, 5, 0, 0, 0 },
    { "L2", -1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX, 6, 0, 0, 0 },
    { "R2", -1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX, 7, 0, 0, 0 },
    /* Options = Start, Share / Create = Back (Select), stick presses, D-pad */
    { "Options", SDL_GAMEPAD_BUTTON_START, -1, 0, -1, 0x10, 0, 0 },
    { "Create", SDL_GAMEPAD_BUTTON_BACK, -1, 0, -1, 0x20, 0, 0 },
    { "L3", SDL_GAMEPAD_BUTTON_LEFT_STICK, -1, 0, -1, 0x40, 0, 0 },
    { "R3", SDL_GAMEPAD_BUTTON_RIGHT_STICK, -1, 0, -1, 0x80, 0, 0 },
    { "D-pad up", SDL_GAMEPAD_BUTTON_DPAD_UP, -1, 0, -1, 0x01, 0, 0 },
    { "D-pad down", SDL_GAMEPAD_BUTTON_DPAD_DOWN, -1, 0, -1, 0x02, 0, 0 },
    { "D-pad left", SDL_GAMEPAD_BUTTON_DPAD_LEFT, -1, 0, -1, 0x04, 0, 0 },
    { "D-pad right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT, -1, 0, -1, 0x08, 0, 0 },
    /* the sticks: SDL's Y axes point down, XInput's up */
    { "left stick left", -1, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN, -1, 0, 1, -32768 },
    { "left stick right", -1, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX, -1, 0, 1, 32767 },
    { "left stick up", -1, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN, -1, 0, 2, 32767 },
    { "left stick down", -1, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX, -1, 0, 2, -32768 },
    { "right stick left", -1, SDL_GAMEPAD_AXIS_RIGHTX, SDL_JOYSTICK_AXIS_MIN, -1, 0, 3, -32768 },
    { "right stick up", -1, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MIN, -1, 0, 4, 32767 },
    { "right stick down", -1, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MAX, -1, 0, 4, -32768 },
};

/* ── off: the XInput path as before ── */
static void test_off(void)
{
    static const WORD bits[4] = { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_DPAD_UP };
    int threads0 = thread_count(), pattern, port, n = 0;
    LONG calls0, sets0;
    ControlsPad cp;
    XINPUT_GAMEPAD g;
    DWORD ids[PADSDL_SLOTS];

    printf("-- off (XBOX_PS_PADS unset)\n");
    CHECK(!padsdl_on() && !controls_ps_on());
    CHECK(padsdl_count() == 0);
    padsdl_list(ids);
    CHECK(!ids[0] && !ids[1] && !ids[2] && !ids[3]);
    CHECK(!padsdl_state(0, 1, &g));
    padsdl_rumble(0, 1, 1000, 1000);
    padsdl_set_player(0, 1, 0);
    padsdl_quit(100);
    for (pattern = 0; pattern < 16; pattern++) {       /* every set of XInput indexes plugged in */
        for (port = 0; port < 4; port++) xi_set(port, (pattern >> port) & 1, bits[port]);
        for (port = 0; port < 4; port++) {
            BOOL on = (pattern >> port) & 1, ok;
            calls0 = xi_calls();
            ok = controls_read_pad((DWORD)port, &cp);
            /* exactly the XInput read of the port's own index, and its value */
            CHECK(xi_calls() - calls0 == 1 && s_xi_calls[port] > 0);
            CHECK(ok == on);
            if (on) {
                CHECK(port == 0 ? cp.analog[0] == 255 : port == 1 ? cp.analog[1] == 255 :
                      port == 2 ? cp.buttons == 0x10 : cp.buttons == 0x01);
            } else {
                CHECK(is_idle(&cp));
            }
            calls0 = xi_calls();
            sets0 = xi_sets();
            CHECK(!controls_port_present((DWORD)port));
            controls_port_rumble((DWORD)port, 30000, 30000);
            CHECK(xi_calls() == calls0 && xi_sets() == sets0);   /* the PlayStation functions ask nothing */
            n++;
        }
    }
    CHECK(!controls_read_pad(4, &cp));
    /* SDL never started, no thread made (Windows' own workers may end meanwhile) */
    CHECK(SDL_WasInit(0) == 0);
    CHECK(thread_count() <= threads0);
    printf("   %d reads, threads %d -> %d, SDL started: %s\n", n, threads0, thread_count(), SDL_WasInit(0) ? "yes" : "no");
}

/* ── on: who plays on each port ──
 * During these steps each controller holds a button of its own, so a read
 * of the port tells which one plays there. */
enum { WHO_NONE, WHO_DS, WHO_DS4, WHO_XI0, WHO_XI1, WHO_XI2, WHO_OTHER };
static const char *k_who[] = { "nobody", "DualSense", "DualShock 4", "XInput 0", "XInput 1", "XInput 2", "?" };
static const WORD k_xi_sig[3] = { XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_START };

static int who(DWORD port)
{
    ControlsPad c;
    if (!controls_read_pad(port, &c)) return WHO_NONE;
    if (c.analog[0] == 255) return WHO_DS;          /* Cross */
    if (c.analog[1] == 255) return WHO_DS4;         /* Circle */
    if (c.analog[3] == 255) return WHO_XI0;         /* Y */
    if (c.analog[2] == 255) return WHO_XI1;         /* X */
    if (c.buttons & 0x10) return WHO_XI2;           /* Start */
    return WHO_OTHER;
}

/* Waits for the four ports to read as `w` (an XInput index plugged in is seen
 * within a second, a PlayStation pad within a scan). */
static int wait_ports(const int w[4], int ms)
{
    DWORD t0 = GetTickCount();
    int got[4], p, ok;
    for (;;) {
        ok = 1;
        for (p = 0; p < 4; p++) { got[p] = who((DWORD)p); if (got[p] != w[p]) ok = 0; }
        if (ok) return 1;
        if ((int)(GetTickCount() - t0) > ms) break;
        Sleep(20);
    }
    printf("   ports read: %s, %s, %s, %s (wanted %s, %s, %s, %s)\n", k_who[got[0]], k_who[got[1]], k_who[got[2]],
           k_who[got[3]], k_who[w[0]], k_who[w[1]], k_who[w[2]], k_who[w[3]]);
    return 0;
}

static int wait_player(VPad *v, int player, int ms)
{
    DWORD t0 = GetTickCount();
    while (v->player != player) {
        if ((int)(GetTickCount() - t0) > ms) { printf("   lights: player index %ld, wanted %d\n", v->player, player); return 0; }
        Sleep(5);
    }
    return 1;
}

/* Rumble asked on `port` reaches `pad` (or the XInput index `xi`) and no other. */
static void check_rumble(DWORD port, VPad *pad, int xi, VPad *others[], int nothers, WORD l, WORD r)
{
    LONG r0 = pad ? pad->rumbles : 0, s0[4], o0[4];
    DWORD t0;
    int i;
    for (i = 0; i < 4; i++) s0[i] = s_xi_sets[i];
    for (i = 0; i < nothers; i++) o0[i] = others[i]->rumbles;
    controls_port_rumble(port, l, r);
    t0 = GetTickCount();
    if (pad) {
        while (pad->rumbles == r0 && GetTickCount() - t0 < 500) Sleep(2);
        CHECK(pad->rumbles > r0 && pad->low == l && pad->high == r);
    } else {
        CHECK(s_xi_sets[xi] == s0[xi] + 1 && s_xi_left[xi] == l && s_xi_right[xi] == r);
    }
    Sleep(30);
    for (i = 0; i < 4; i++) if (pad || i != xi) CHECK(s_xi_sets[i] == s0[i]);
    for (i = 0; i < nothers; i++) CHECK(others[i]->rumbles == o0[i]);
}

/* ── on: virtual pads ── */
static void test_on(void)
{
    VPad ds, ds4, xb, gen;
    ControlsPad cp;
    DWORD t0;
    size_t i;
    int port, ok;

    printf("-- on (XBOX_PS_PADS=1)\n");
    for (port = 0; port < 4; port++) xi_set(port, 0, 0);
    CHECK(padsdl_on() && controls_ps_on());
    CHECK(!controls_read_pad(0, &cp));                 /* starts the pad thread; no pad yet */
    t0 = GetTickCount();
    while (!SDL_WasInit(SDL_INIT_GAMEPAD) && GetTickCount() - t0 < 10000) Sleep(5);
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) != 0);
    Sleep(50);                                          /* the pad thread's own settings after SDL_Init */
    printf("   SDL up in %lu ms, threads %d\n", (unsigned long)(GetTickCount() - t0), thread_count());
    {   /* SDL's back ends: the PlayStation HIDAPI drivers only */
        static const struct { const char *hint, *want; } k[] = {
            { SDL_HINT_JOYSTICK_HIDAPI, "0" }, { SDL_HINT_JOYSTICK_HIDAPI_PS4, "1" },
            { SDL_HINT_JOYSTICK_HIDAPI_PS5, "1" }, { SDL_HINT_XINPUT_ENABLED, "0" },
            { SDL_HINT_JOYSTICK_DIRECTINPUT, "0" }, { SDL_HINT_JOYSTICK_RAWINPUT, "0" },
            { SDL_HINT_JOYSTICK_WGI, "0" }, { SDL_HINT_JOYSTICK_GAMEINPUT, "0" },
            { SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1" }, { SDL_HINT_NO_SIGNAL_HANDLERS, "1" },
        };
        HMODULE hid = GetModuleHandleA("hid.dll");
        char path[MAX_PATH] = "";
        for (i = 0; i < sizeof k / sizeof k[0]; i++) {
            const char *v = SDL_GetHint(k[i].hint);
            CHECK(v && !strcmp(v, k[i].want));
        }
        /* no event is kept: nothing reads SDL's queue */
        CHECK(!SDL_EventEnabled(SDL_EVENT_JOYSTICK_UPDATE_COMPLETE));
        CHECK(!SDL_EventEnabled(SDL_EVENT_GAMEPAD_UPDATE_COMPLETE));
        CHECK(!SDL_EventEnabled(SDL_EVENT_JOYSTICK_AXIS_MOTION) && !SDL_EventEnabled(SDL_EVENT_GAMEPAD_BUTTON_DOWN));
        CHECK(!SDL_EventEnabled(SDL_EVENT_JOYSTICK_ADDED) && !SDL_EventEnabled(SDL_EVENT_GAMEPAD_ADDED));
        /* hid.dll: the system's copy */
        if (hid) GetModuleFileNameA(hid, path, sizeof path);
        printf("   hid.dll: %s\n", path[0] ? path : "not loaded");
        _strlwr(path);
        CHECK(hid && strstr(path, "\\system32\\hid.dll"));
    }

    /* an Xbox 360 and an unknown pad seen by SDL: never opened */
    CHECK(vpad_attach(&xb, 0x045E, 0x028E, "Xbox 360 Controller (test)"));
    CHECK(vpad_attach(&gen, 0x1234, 0x5678, "Generic Gamepad (test)"));
    CHECK(SDL_GetGamepadTypeForID(xb.id) == SDL_GAMEPAD_TYPE_XBOX360);
    Sleep(1300);                                        /* past a scan */
    CHECK(padsdl_count() == 0);
    CHECK(!controls_read_pad(0, &cp));

    /* a DualSense, alone: player 1, its lights say so */
    CHECK(vpad_attach(&ds, 0x054C, 0x0CE6, "DualSense Wireless Controller (test)"));
    CHECK(SDL_GetGamepadTypeForID(ds.id) == SDL_GAMEPAD_TYPE_PS5);
    t0 = GetTickCount();
    ok = wait_count(1, 2500);
    CHECK(ok);
    printf("   DualSense opened in %lu ms\n", (unsigned long)(GetTickCount() - t0));
    CHECK(wait_read(0, is_idle, &cp, 500));             /* at rest: nothing held, triggers released */
    CHECK(controls_port_present(0) && !controls_port_present(1));
    CHECK(!controls_read_pad(1, &cp));
    CHECK(wait_player(&ds, 0, 500));
    for (i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        int queued;
        s_case = &k_cases[i];
        vpad_set(&ds, s_case, 1);
        ok = wait_read(0, matches_case, &cp, 500);
        if (!ok) printf("   %s: read buttons 0x%02X analog %u %u %u %u %u %u %u %u sticks %d %d %d %d\n", s_case->name,
                        cp.buttons, cp.analog[0], cp.analog[1], cp.analog[2], cp.analog[3], cp.analog[4], cp.analog[5],
                        cp.analog[6], cp.analog[7], cp.lx, cp.ly, cp.rx, cp.ry);
        CHECK(ok);
        vpad_set(&ds, s_case, 0);
        CHECK(wait_read(0, is_idle, &cp, 500));
        /* each change of the pad's state leaves nothing in SDL's event queue */
        queued = SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST);
        if (queued > s_queued_max) s_queued_max = queued;
    }
    CHECK(s_queued_max == 0);
    printf("   %d inputs read through controls.c, most events queued: %d\n", (int)(sizeof k_cases / sizeof k_cases[0]),
           s_queued_max);
    {   /* a trigger halfway: analog */
        ControlsPad c2;
        SDL_SetJoystickVirtualAxis(ds.js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
        t0 = GetTickCount();
        do { controls_read_pad(0, &c2); Sleep(2); } while (c2.analog[7] == 0 && GetTickCount() - t0 < 500);
        CHECK(c2.analog[7] >= 120 && c2.analog[7] <= 135);
        printf("   R2 halfway reads %u\n", c2.analog[7]);
        SDL_SetJoystickVirtualAxis(ds.js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
        CHECK(wait_read(0, is_idle, &cp, 500));
    }

    /* From here each controller holds its own button: who(port) tells who plays there. */
    SDL_SetJoystickVirtualButton(ds.js, SDL_GAMEPAD_BUTTON_SOUTH, true);
    {
        int w[4];
        VPad *o_ds[1] = { &ds }, *both[2];

        /* (c) an Xbox controller turned on while the DualSense plays on port 0: player 2, the DualSense stays player 1 */
        xi_set(0, 1, k_xi_sig[0]);
        w[0] = WHO_DS; w[1] = WHO_XI0; w[2] = WHO_NONE; w[3] = WHO_NONE;
        CHECK(wait_ports(w, 2500));
        printf("   Xbox controller turned on beside a DualSense on port 0: player 2\n");
        check_rumble(1, NULL, 0, o_ds, 1, 0x7000, 0x3000);      /* port 1's rumble: XInput index 0 */
        check_rumble(0, &ds, -1, NULL, 0, 0x5000, 0x2000);      /* port 0's: the DualSense, not XInput 0 */
        check_rumble(0, &ds, -1, NULL, 0, 0, 0);

        /* another Xbox controller (XInput index 1, whose port 1 is taken): the lowest free port */
        xi_set(1, 1, k_xi_sig[1]);
        w[2] = WHO_XI1;
        CHECK(wait_ports(w, 2500));

        /* (a) player 2's Xbox controller turned off: nobody else moves */
        xi_set(0, 0, 0);
        w[1] = WHO_NONE;
        CHECK(wait_ports(w, 2500));
        printf("   player 2's Xbox controller off: DualSense player 1, XInput 1 player 3\n");

        /* a DualShock 4 plugged in: the lowest free port, 1; its lights say player 2 */
        CHECK(vpad_attach(&ds4, 0x054C, 0x09CC, "Wireless Controller (test)"));
        CHECK(SDL_GetGamepadTypeForID(ds4.id) == SDL_GAMEPAD_TYPE_PS4);
        SDL_SetJoystickVirtualButton(ds4.js, SDL_GAMEPAD_BUTTON_EAST, true);
        w[1] = WHO_DS4;
        CHECK(wait_ports(w, 3000));
        CHECK(wait_player(&ds4, 1, 500));
        check_rumble(1, &ds4, -1, o_ds, 1, 0x6000, 0x6000);
        check_rumble(1, &ds4, -1, o_ds, 1, 0, 0);

        /* (b) the DualSense unplugged: the DualShock 4 stays player 2 */
        vpad_detach(&ds);
        CHECK(wait_count(1, 2500));
        w[0] = WHO_NONE;
        CHECK(wait_ports(w, 2500));
        printf("   first PlayStation pad unplugged: the second one stays player 2\n");

        /* XInput index 0 back: its own port 0 is free again */
        xi_set(0, 1, k_xi_sig[0]);
        w[0] = WHO_XI0;
        CHECK(wait_ports(w, 2500));

        /* the DualSense back (a new pad for SDL): the lowest free port, 3 */
        CHECK(vpad_attach(&ds, 0x054C, 0x0CE6, "DualSense Wireless Controller (test)"));
        SDL_SetJoystickVirtualButton(ds.js, SDL_GAMEPAD_BUTTON_SOUTH, true);
        w[3] = WHO_DS;
        CHECK(wait_ports(w, 3000));
        CHECK(wait_player(&ds, 3, 500));
        both[0] = &ds; both[1] = &ds4;
        check_rumble(0, NULL, 0, both, 2, 0x1111, 0x2222);      /* XInput 0 on its own port */
        check_rumble(0, NULL, 0, both, 2, 0, 0);

        /* a fifth controller (XInput index 2) waits; it takes its port once one is free */
        xi_set(2, 1, k_xi_sig[2]);
        Sleep(1200);
        CHECK(wait_ports(w, 500));
        xi_set(1, 0, 0);
        w[2] = WHO_XI2;
        CHECK(wait_ports(w, 3000));
        printf("   a fifth controller waited, then took the port freed\n");
    }

    /* after all these plugs and unplugs, a scan later: nothing left in SDL's queue */
    Sleep(1100);
    CHECK(!SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST));

    /* the game ends: the motors of a rumbling pad are stopped, the pads closed */
    {
        LONG r0;
        controls_port_rumble(1, 0x8000, 0x4000);               /* the DualShock 4 */
        t0 = GetTickCount();
        while (ds4.low != 0x8000 && GetTickCount() - t0 < 500) Sleep(2);
        CHECK(ds4.low == 0x8000);
        Sleep(600);                                             /* past SDL's 500 ms: renewed, never stopped */
        CHECK(ds4.low == 0x8000 && ds4.high == 0x4000);
        r0 = ds4.rumbles;
        t0 = GetTickCount();
        padsdl_quit(500);
        printf("   quit in %lu ms\n", (unsigned long)(GetTickCount() - t0));
        CHECK(GetTickCount() - t0 < 500);
        CHECK(ds4.rumbles > r0 && ds4.low == 0 && ds4.high == 0);
        CHECK(padsdl_count() == 0);
        CHECK(who(1) == WHO_NONE && who(3) == WHO_NONE);         /* no PlayStation pad read any more */
        CHECK(who(0) == WHO_XI0 && who(2) == WHO_XI2);           /* the Xbox controllers: read as before */
        r0 = ds4.rumbles;
        controls_port_rumble(1, 0x8000, 0x8000);
        Sleep(50);
        CHECK(ds4.rumbles == r0);
    }
    vpad_detach(&ds4);
    vpad_detach(&ds);
    vpad_detach(&xb);
    vpad_detach(&gen);
    {   /* nobody empties SDL's queue once the pad thread has quit (the game is ending): what the four
         * unplugs above left there, for the record */
        SDL_Event ev[16];
        int n = SDL_PeepEvents(ev, 16, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST), k;
        printf("   events queued after the quit: %d", n);
        for (k = 0; k < n; k++) printf(" 0x%x", (unsigned)ev[k].type);
        printf("\n");
    }
    printf("   threads at the end: %d\n", thread_count());
}

int main(int argc, char **argv)
{
    int on = argc > 1 && !strcmp(argv[1], "on");
    const char *e = getenv("XBOX_PS_PADS");
    if (on != (e && e[0] == '1')) {
        printf("run it as \"pad_sdl_test off\" with XBOX_PS_PADS unset, or \"pad_sdl_test on\" with XBOX_PS_PADS=1\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (on) test_on();
    else test_off();
    printf("%s: %d checks, %d failed\n", s_fail ? "FAIL" : "PASS", s_checks, s_fail);
    fflush(stdout);
    ExitProcess(s_fail ? 1u : 0u);
    return 0;
}
