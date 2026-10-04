/*
 * controls.c -- button mapping for the keyboard and the PC controller
 *.
 *
 * The title reads a console gamepad through its XAPI XInputGetState, which
 * xapi_input_hle.c answers on the host. This module turns a physical XInput
 * controller and the keyboard into that console state through the player's
 * bindings; the launcher's Controls page edits them (which
 * replaced the separate Controls window).
 *
 * Defaults: the controller maps like-for-like, with the bumpers on the two
 * console-only buttons (LB = White, RB = Black, as xemu and Cxbx-Reloaded
 * place them); the keyboard keeps the part-178 keys (Enter = Start, Space = A,
 * Esc = B, C = X, V = Y, Tab = Back, arrows = D-pad and left stick) and adds
 * Q/E for the triggers and R/F for Black/White.
 */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "controls.h"

HWND d3d8_GetHostWindow(void);   /* xboxrecomp d3d8_device.c */

/* ── Names ─────────────────────────────────────────────────────────── */

static const struct { const char *ini; const WCHAR *label; } k_ctl[CTL_COUNT] = {
    { "A", L"A" }, { "B", L"B" }, { "X", L"X" }, { "Y", L"Y" },
    { "Black", L"Black" }, { "White", L"White" },
    { "LeftTrigger", L"Left trigger" }, { "RightTrigger", L"Right trigger" },
    { "Start", L"Start" }, { "Back", L"Back" },
    { "LeftStickPress", L"Left stick press" }, { "RightStickPress", L"Right stick press" },
    { "DpadUp", L"D-pad up" }, { "DpadDown", L"D-pad down" },
    { "DpadLeft", L"D-pad left" }, { "DpadRight", L"D-pad right" },
    { "LeftStickUp", L"Left stick up" }, { "LeftStickDown", L"Left stick down" },
    { "LeftStickLeft", L"Left stick left" }, { "LeftStickRight", L"Left stick right" },
    { "RightStickUp", L"Right stick up" }, { "RightStickDown", L"Right stick down" },
    { "RightStickLeft", L"Right stick left" }, { "RightStickRight", L"Right stick right" },
};

static const struct { const char *ini; const WCHAR *label; } k_pad[PAD_COUNT] = {
    { "None", L"" }, { "A", L"A" }, { "B", L"B" }, { "X", L"X" }, { "Y", L"Y" },
    { "LB", L"Left bumper" }, { "RB", L"Right bumper" },
    { "LT", L"Left trigger" }, { "RT", L"Right trigger" },
    { "Start", L"Start" }, { "Back", L"Back" },
    { "LS", L"Left stick press" }, { "RS", L"Right stick press" },
    { "DpadUp", L"D-pad up" }, { "DpadDown", L"D-pad down" },
    { "DpadLeft", L"D-pad left" }, { "DpadRight", L"D-pad right" },
};

/* XINPUT_GAMEPAD button bit of each digital PAD_* (0 = a trigger). */
static const WORD k_pad_bit[PAD_COUNT] = {
    0, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0,
    XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
    XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
    XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
};

/* Key names: stable for the .ini, readable in the window. */
static const struct { BYTE vk; const char *name; } k_keys[] = {
    { VK_SPACE, "Space" }, { VK_RETURN, "Enter" }, { VK_ESCAPE, "Escape" },
    { VK_TAB, "Tab" }, { VK_BACK, "Backspace" }, { VK_CAPITAL, "CapsLock" },
    { VK_UP, "Up" }, { VK_DOWN, "Down" }, { VK_LEFT, "Left" }, { VK_RIGHT, "Right" },
    { VK_LSHIFT, "LeftShift" }, { VK_RSHIFT, "RightShift" }, { VK_SHIFT, "Shift" },
    { VK_LCONTROL, "LeftCtrl" }, { VK_RCONTROL, "RightCtrl" }, { VK_CONTROL, "Ctrl" },
    { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" }, { VK_HOME, "Home" },
    { VK_END, "End" }, { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" },
    { VK_NUMPAD0, "Num0" }, { VK_NUMPAD1, "Num1" }, { VK_NUMPAD2, "Num2" },
    { VK_NUMPAD3, "Num3" }, { VK_NUMPAD4, "Num4" }, { VK_NUMPAD5, "Num5" },
    { VK_NUMPAD6, "Num6" }, { VK_NUMPAD7, "Num7" }, { VK_NUMPAD8, "Num8" },
    { VK_NUMPAD9, "Num9" }, { VK_MULTIPLY, "Num*" }, { VK_ADD, "Num+" },
    { VK_SUBTRACT, "Num-" }, { VK_DECIMAL, "Num." }, { VK_DIVIDE, "Num/" },
    { VK_OEM_1, ";" }, { VK_OEM_PLUS, "=" }, { VK_OEM_COMMA, "," },
    { VK_OEM_MINUS, "-" }, { VK_OEM_PERIOD, "." }, { VK_OEM_2, "/" },
    { VK_OEM_3, "`" }, { VK_OEM_4, "[" }, { VK_OEM_5, "\\" }, { VK_OEM_6, "]" },
    { VK_OEM_7, "'" },
};

static void key_name(BYTE vk, char *out, size_t n)
{
    size_t i;
    if (!vk) { out[0] = '\0'; return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(out, n, "%c", vk); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(out, n, "F%d", vk - VK_F1 + 1); return; }
    for (i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (k_keys[i].vk == vk) { snprintf(out, n, "%s", k_keys[i].name); return; }
    snprintf(out, n, "Key0x%02X", vk);
}

static BYTE key_parse(const char *s)
{
    size_t i;
    unsigned v;
    if (!s[0] || !_stricmp(s, "None")) return 0;
    if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9')))
        return (BYTE)toupper((unsigned char)s[0]);
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int f = atoi(s + 1);
        if (f >= 1 && f <= 24) return (BYTE)(VK_F1 + f - 1);
    }
    for (i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (!_stricmp(k_keys[i].name, s)) return k_keys[i].vk;
    if (sscanf(s, "Key0x%x", &v) == 1 && v > 0 && v < 256) return (BYTE)v;
    return 0;
}

/* ── Settings ──────────────────────────────────────────────────────── */

void controls_defaults(ControlMap *m)
{
    static const BYTE keys[CTL_COUNT] = {
        [CTL_A] = VK_SPACE, [CTL_B] = VK_ESCAPE, [CTL_X] = 'C', [CTL_Y] = 'V',
        [CTL_BLACK] = 'R', [CTL_WHITE] = 'F', [CTL_LT] = 'Q', [CTL_RT] = 'E',
        [CTL_START] = VK_RETURN, [CTL_BACK] = VK_TAB,
        [CTL_DUP] = VK_UP, [CTL_DDOWN] = VK_DOWN, [CTL_DLEFT] = VK_LEFT, [CTL_DRIGHT] = VK_RIGHT,
        [CTL_LS_UP] = VK_UP, [CTL_LS_DOWN] = VK_DOWN, [CTL_LS_LEFT] = VK_LEFT, [CTL_LS_RIGHT] = VK_RIGHT,
    };
    static const BYTE pads[CTL_PAD_COUNT] = {
        [CTL_A] = PAD_A, [CTL_B] = PAD_B, [CTL_X] = PAD_X, [CTL_Y] = PAD_Y,
        [CTL_BLACK] = PAD_RB, [CTL_WHITE] = PAD_LB, [CTL_LT] = PAD_LT, [CTL_RT] = PAD_RT,
        [CTL_START] = PAD_START, [CTL_BACK] = PAD_BACK, [CTL_L3] = PAD_LS, [CTL_R3] = PAD_RS,
        [CTL_DUP] = PAD_DUP, [CTL_DDOWN] = PAD_DDOWN, [CTL_DLEFT] = PAD_DLEFT, [CTL_DRIGHT] = PAD_DRIGHT,
    };
    memcpy(m->key, keys, sizeof m->key);
    memcpy(m->pad, pads, sizeof m->pad);
}

void controls_load(ControlMap *m, const char *ini)
{
    int i, j;
    char buf[64];
    controls_defaults(m);
    if (!ini || GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;
    for (i = 0; i < CTL_COUNT; i++) {
        if (GetPrivateProfileStringA("Keyboard", k_ctl[i].ini, "\x01", buf, sizeof buf, ini) &&
            buf[0] != '\x01')
            m->key[i] = key_parse(buf);
    }
    for (i = 0; i < CTL_PAD_COUNT; i++) {
        if (GetPrivateProfileStringA("Controller", k_ctl[i].ini, "\x01", buf, sizeof buf, ini) &&
            buf[0] != '\x01') {
            m->pad[i] = PAD_NONE;
            for (j = 0; j < PAD_COUNT; j++)
                if (!_stricmp(buf, k_pad[j].ini)) m->pad[i] = (BYTE)j;
        }
    }
}

void controls_write(const ControlMap *m, FILE *f)
{
    int i;
    char name[32];
    fprintf(f, "\n[Keyboard]\n; One key per Xbox control: a letter or digit, F1-F24, Space, Enter,\n"
               "; Escape, Tab, Up/Down/Left/Right, LeftShift, Num0-Num9 ... or None.\n");
    for (i = 0; i < CTL_COUNT; i++) {
        key_name(m->key[i], name, sizeof name);
        fprintf(f, "%s=%s\n", k_ctl[i].ini, name[0] ? name : "None");
    }
    fprintf(f, "\n[Controller]\n; The PC controller input for each Xbox control: A B X Y LB RB LT RT\n"
               "; Start Back LS RS DpadUp DpadDown DpadLeft DpadRight, or None.\n");
    for (i = 0; i < CTL_PAD_COUNT; i++)
        fprintf(f, "%s=%s\n", k_ctl[i].ini, k_pad[m->pad[i] < PAD_COUNT ? m->pad[i] : 0].ini);
}

/* ── The live mapping ──────────────────────────────────────────────── */

static SRWLOCK s_lock = SRWLOCK_INIT;
static ControlMap s_map;
static volatile LONG s_map_set = 0;
static volatile LONG s_suspended = 0;

void controls_set_current(const ControlMap *m)
{
    AcquireSRWLockExclusive(&s_lock);
    s_map = *m;
    s_map_set = 1;
    ReleaseSRWLockExclusive(&s_lock);
}

void controls_get_current(ControlMap *m)
{
    AcquireSRWLockShared(&s_lock);
    if (s_map_set) *m = s_map;
    ReleaseSRWLockShared(&s_lock);
    if (!s_map_set) controls_defaults(m);
}

void controls_set_suspended(BOOL on) { InterlockedExchange(&s_suspended, on ? 1 : 0); }
BOOL controls_suspended(void)        { return s_suspended != 0; }

/* Console button bit or analog index of each control. */
static void apply(ControlsPad *p, int ctl, BYTE value)
{
    static const WORD digital[CTL_PAD_COUNT] = {
        [CTL_START] = 0x10, [CTL_BACK] = 0x20, [CTL_L3] = 0x40, [CTL_R3] = 0x80,
        [CTL_DUP] = 0x01, [CTL_DDOWN] = 0x02, [CTL_DLEFT] = 0x04, [CTL_DRIGHT] = 0x08,
    };
    if (!value) return;
    if (ctl <= CTL_RT) {
        if (value > p->analog[ctl]) p->analog[ctl] = value;
    } else if (ctl < CTL_PAD_COUNT) {
        p->buttons |= digital[ctl];
    } else {
        switch (ctl) {
        case CTL_LS_UP:    p->ly =  32767; break;
        case CTL_LS_DOWN:  p->ly = -32767; break;
        case CTL_LS_LEFT:  p->lx = -32767; break;
        case CTL_LS_RIGHT: p->lx =  32767; break;
        case CTL_RS_UP:    p->ry =  32767; break;
        case CTL_RS_DOWN:  p->ry = -32767; break;
        case CTL_RS_LEFT:  p->rx = -32767; break;
        case CTL_RS_RIGHT: p->rx =  32767; break;
        }
    }
}

/* 0..255 for a controller input: triggers are analog, buttons 0 or 255. */
static BYTE pad_value(const XINPUT_GAMEPAD *g, int src)
{
    if (src == PAD_LT) return g->bLeftTrigger;
    if (src == PAD_RT) return g->bRightTrigger;
    if (src > PAD_NONE && src < PAD_COUNT && k_pad_bit[src])
        return (g->wButtons & k_pad_bit[src]) ? 255 : 0;
    return 0;
}

BOOL controls_read_pad(DWORD port, ControlsPad *out)
{
    XINPUT_STATE st;
    ControlMap m;
    int i;

    memset(out, 0, sizeof *out);
    memset(&st, 0, sizeof st);
    if (port >= 4 || XInputGetState(port, &st) != ERROR_SUCCESS) return FALSE;
    if (s_suspended) return TRUE;               /* connected, but idle for now */
    controls_get_current(&m);
    for (i = 0; i < CTL_PAD_COUNT; i++) {
        BYTE v = pad_value(&st.Gamepad, m.pad[i]);
        /* A trigger driving a digital control needs a firm press. */
        if (i > CTL_RT && v && v < 30) v = 0;
        apply(out, i, v);
    }
    out->lx = st.Gamepad.sThumbLX;
    out->ly = st.Gamepad.sThumbLY;
    out->rx = st.Gamepad.sThumbRX;
    out->ry = st.Gamepad.sThumbRY;
    return TRUE;
}

/* Test only: XBOX_INPUT_AUTOKEY=K@20,Space@30 -- a keyboard key
 * held for 300 ms at each time (seconds after the first keyboard read), as
 * if it had been pressed, so a test can show a binding at work without
 * typing into the desktop. It goes through the player's bindings like a
 * real key, and needs no window in front. Key names as in the .ini. */
static BOOL autokey_held(BYTE vk)
{
    static int parsed = 0, n = 0;
    static struct { BYTE vk; DWORD at_ms; } ev[64];
    static DWORD t0;
    DWORD now = GetTickCount();
    int i;
    if (!parsed) {
        const char *e = getenv("XBOX_INPUT_AUTOKEY");
        parsed = 1;
        t0 = now;
        while (e && *e && n < 64) {
            char name[24];
            size_t k = 0;
            while (*e && *e != '@' && k < sizeof name - 1) name[k++] = *e++;
            name[k] = 0;
            if (*e != '@') break;
            ev[n].at_ms = (DWORD)(strtod(e + 1, (char **)&e) * 1000.0);
            ev[n].vk = key_parse(name);
            if (ev[n].vk) n++;
            if (*e == ',') e++;
        }
        if (n) { fprintf(stderr, "  [CONTROLS] autokey: %d key press(es)\n", n); fflush(stderr); }
    }
    for (i = 0; i < n; i++)
        if (ev[i].vk == vk && now - t0 >= ev[i].at_ms && now - t0 < ev[i].at_ms + 300) return TRUE;
    return FALSE;
}

void controls_read_keyboard(ControlsPad *io)
{
    ControlMap m;
    int i;
    HWND game = d3d8_GetHostWindow();
    BOOL front;
    if (s_suspended || !game) return;
    /* Real keys only while the game window itself is in front: not in one
     * of our windows, not in another program. */
    front = GetForegroundWindow() == game;
    controls_get_current(&m);
    for (i = 0; i < CTL_COUNT; i++)
        if (m.key[i] && ((front && (GetAsyncKeyState(m.key[i]) & 0x8000)) || autokey_held(m.key[i])))
            apply(io, i, 255);
}

/* ── For the launcher's Controls page ──────────────────── */

const WCHAR *controls_label(int ctl)
{
    return ctl >= 0 && ctl < CTL_COUNT ? k_ctl[ctl].label : L"";
}

const WCHAR *controls_pad_label(int pad)
{
    return pad > PAD_NONE && pad < PAD_COUNT ? k_pad[pad].label : L"";
}

void controls_key_label(BYTE vk, WCHAR *out, int n)
{
    char a[32];
    key_name(vk, a, sizeof a);
    if (!MultiByteToWideChar(CP_ACP, 0, a, -1, out, n)) out[0] = 0;
}

/* The key a WM_KEYDOWN / WM_SYSKEYDOWN names, left and right Shift / Ctrl
 * told apart; 0 for the keys the window keeps (Alt, F10, F11, F12, Windows). */
BYTE controls_key_from_msg(WPARAM wp, LPARAM lp)
{
    UINT vk = (UINT)wp;
    if (vk == VK_SHIFT || vk == VK_CONTROL) {
        UINT sc = (UINT)((lp >> 16) & 0xFF) | ((lp & (1 << 24)) ? 0xE000u : 0u);
        UINT lr = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
        if (lr) vk = lr;
        if (vk == VK_CONTROL) vk = (lp & (1 << 24)) ? VK_RCONTROL : VK_LCONTROL;
    }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_F10 || vk == VK_F11 ||
        vk == VK_F12 || vk == VK_LWIN || vk == VK_RWIN || vk == 0 || vk > 255)
        return 0;
    return (BYTE)vk;
}

void controls_pad_snap(ControlsPadSnap *s)
{
    int i;
    for (i = 0; i < 4; i++) {
        XINPUT_STATE st;
        memset(&st, 0, sizeof st);
        XInputGetState((DWORD)i, &st);
        s->buttons[i] = st.Gamepad.wButtons;
        s->trig[i][0] = st.Gamepad.bLeftTrigger;
        s->trig[i][1] = st.Gamepad.bRightTrigger;
    }
}

/* A controller input pressed since the snapshot (PAD_*), or PAD_NONE. Held
 * inputs are not seen again: the snapshot follows the controllers. */
int controls_pad_newly_pressed(ControlsPadSnap *s)
{
    int i, p, got = PAD_NONE;
    for (i = 0; i < 4 && got == PAD_NONE; i++) {
        XINPUT_STATE st;
        memset(&st, 0, sizeof st);
        if (XInputGetState((DWORD)i, &st) != ERROR_SUCCESS) continue;
        for (p = PAD_A; p < PAD_COUNT && got == PAD_NONE; p++) {
            BOOL now, before;
            if (p == PAD_LT) { now = st.Gamepad.bLeftTrigger > 100; before = s->trig[i][0] > 100; }
            else if (p == PAD_RT) { now = st.Gamepad.bRightTrigger > 100; before = s->trig[i][1] > 100; }
            else { now = (st.Gamepad.wButtons & k_pad_bit[p]) != 0; before = (s->buttons[i] & k_pad_bit[p]) != 0; }
            if (now && !before) got = p;
        }
        s->buttons[i] = st.Gamepad.wButtons;
        s->trig[i][0] = st.Gamepad.bLeftTrigger;
        s->trig[i][1] = st.Gamepad.bRightTrigger;
    }
    return got;
}
