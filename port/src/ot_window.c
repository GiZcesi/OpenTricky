/* ot_window.c -- see ot_window.h. */
#include "ot_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(OT_HAVE_WINDOW)

#include <pthread.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif
#include <time.h>
#include <sys/stat.h>
#include <SDL3/SDL.h>

#define GUEST_STACK_BYTES ((size_t)64 * 1024 * 1024)   /* lifted code keeps big frames on the native stack */

/* ---- frame mailbox: written by the presenting thread, read by the window thread ---- */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- input: the event thread snapshots the keyboard and the pads; the game thread reads the snapshot through the
 * hooks the Win32 layer exposes (GetAsyncKeyState, XInputGetState, the focus test) ---------------------------- */
typedef struct { uint32_t packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } OtXPad;   /* = XINPUT_STATE */
extern int (*w32_key_hook)(int vk);
extern int (*w32_pad_hook)(unsigned port, OtXPad *st);
extern int (*w32_focus_hook)(void);

static volatile uint8_t s_key[256];       /* by Win32 virtual-key code */
static volatile int s_focus;
static OtXPad s_pad[4];                   /* guarded by s_pad_lock */
static volatile int s_pad_on[4];
static SDL_Gamepad *s_gp[4];
static pthread_mutex_t s_pad_lock = PTHREAD_MUTEX_INITIALIZER;

static const struct { SDL_Scancode sc; uint8_t vk; } k_vk[] = {
    { SDL_SCANCODE_SPACE, 0x20 }, { SDL_SCANCODE_RETURN, 0x0D }, { SDL_SCANCODE_ESCAPE, 0x1B }, { SDL_SCANCODE_TAB, 0x09 },
    { SDL_SCANCODE_BACKSPACE, 0x08 }, { SDL_SCANCODE_CAPSLOCK, 0x14 }, { SDL_SCANCODE_UP, 0x26 }, { SDL_SCANCODE_DOWN, 0x28 },
    { SDL_SCANCODE_LEFT, 0x25 }, { SDL_SCANCODE_RIGHT, 0x27 }, { SDL_SCANCODE_LSHIFT, 0xA0 }, { SDL_SCANCODE_RSHIFT, 0xA1 },
    { SDL_SCANCODE_LCTRL, 0xA2 }, { SDL_SCANCODE_RCTRL, 0xA3 }, { SDL_SCANCODE_LALT, 0xA4 }, { SDL_SCANCODE_RALT, 0xA5 },
    { SDL_SCANCODE_INSERT, 0x2D }, { SDL_SCANCODE_DELETE, 0x2E }, { SDL_SCANCODE_HOME, 0x24 }, { SDL_SCANCODE_END, 0x23 },
    { SDL_SCANCODE_PAGEUP, 0x21 }, { SDL_SCANCODE_PAGEDOWN, 0x22 },
    { SDL_SCANCODE_KP_0, 0x60 }, { SDL_SCANCODE_KP_1, 0x61 }, { SDL_SCANCODE_KP_2, 0x62 }, { SDL_SCANCODE_KP_3, 0x63 },
    { SDL_SCANCODE_KP_4, 0x64 }, { SDL_SCANCODE_KP_5, 0x65 }, { SDL_SCANCODE_KP_6, 0x66 }, { SDL_SCANCODE_KP_7, 0x67 },
    { SDL_SCANCODE_KP_8, 0x68 }, { SDL_SCANCODE_KP_9, 0x69 }, { SDL_SCANCODE_KP_MULTIPLY, 0x6A }, { SDL_SCANCODE_KP_PLUS, 0x6B },
    { SDL_SCANCODE_KP_MINUS, 0x6D }, { SDL_SCANCODE_KP_PERIOD, 0x6E }, { SDL_SCANCODE_KP_DIVIDE, 0x6F },
    { SDL_SCANCODE_SEMICOLON, 0xBA }, { SDL_SCANCODE_EQUALS, 0xBB }, { SDL_SCANCODE_COMMA, 0xBC }, { SDL_SCANCODE_MINUS, 0xBD },
    { SDL_SCANCODE_PERIOD, 0xBE }, { SDL_SCANCODE_SLASH, 0xBF }, { SDL_SCANCODE_GRAVE, 0xC0 }, { SDL_SCANCODE_LEFTBRACKET, 0xDB },
    { SDL_SCANCODE_BACKSLASH, 0xDC }, { SDL_SCANCODE_RIGHTBRACKET, 0xDD }, { SDL_SCANCODE_APOSTROPHE, 0xDE },
};

static int hook_key(int vk) { return vk > 0 && vk < 256 && s_key[vk]; }
static int hook_focus(void) { return s_focus; }
static int hook_pad(unsigned port, OtXPad *st)
{
    int ok;
    if (port >= 4) return 0;
    pthread_mutex_lock(&s_pad_lock);
    ok = s_pad_on[port];
    if (ok) *st = s_pad[port];
    pthread_mutex_unlock(&s_pad_lock);
    return ok;
}

static void pad_open_all(void)
{
    int n = 0, i, slot;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n; i++) {
        int have = 0;
        for (slot = 0; slot < 4; slot++) if (s_gp[slot] && SDL_GetGamepadID(s_gp[slot]) == ids[i]) have = 1;
        if (have) continue;
        for (slot = 0; slot < 4; slot++)
            if (!s_gp[slot]) {
                s_gp[slot] = SDL_OpenGamepad(ids[i]);
                if (s_gp[slot]) fprintf(stderr, "[INPUT] pad %d: %s\n", slot, SDL_GetGamepadName(s_gp[slot]));
                break;
            }
    }
    SDL_free(ids);
}

/* ---- display shape: what the title is told and how big the window starts ---------------------------------------------
 * The title is told its display is widescreen, and how wide, before it starts (XBOX_ASPECT / XBOX_RENDER, read by
 * main.c); the wider view cannot change while it runs. OT_SHAPE chooses it: auto (the desktop's own shape, 4:3 below
 * 1.5, 16:9 up to 1.95), 4:3, 16:9, 21:9, 32:9, or a number (width / height). OT_RENDER_H is the scene target's height
 * in pixels (default 1080 for the wide shapes, 960 at 4:3). Environment XBOX_ASPECT / XBOX_RENDER / XBOX_WIDESCREEN set
 * by the user win. Returns the shape the window should have (0 = 4:3). */
static double choose_display(void)
{
    const char *sh = getenv("OT_SHAPE");
    double A = 0.0;
    if (getenv("XBOX_ASPECT") || getenv("XBOX_RENDER") || getenv("XBOX_WIDESCREEN")) {
        unsigned rw = 0, rh = 0; const char *r = getenv("XBOX_RENDER");
        if (r && sscanf(r, "%ux%u", &rw, &rh) == 2 && rw && rh) return (double)rw / (double)rh;
        return getenv("XBOX_WIDESCREEN") ? 16.0 / 9.0 : 0.0;
    }
    if (!sh || !sh[0] || !strcmp(sh, "auto")) {
        SDL_DisplayID d = SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *m = d ? SDL_GetDesktopDisplayMode(d) : NULL;
        double a = (m && m->w > 0 && m->h > 0) ? (double)m->w / (double)m->h : 16.0 / 9.0;
        A = a < 1.5 ? 0.0 : a < 1.95 ? 16.0 / 9.0 : a;
    } else if (!strcmp(sh, "4:3")) A = 0.0;
    else if (!strcmp(sh, "16:9")) A = 16.0 / 9.0;
    else if (!strcmp(sh, "21:9")) A = 64.0 / 27.0;          /* 2560x1080 / 3440x1440 are 2.37 / 2.39 */
    else if (!strcmp(sh, "32:9")) A = 32.0 / 9.0;
    else { A = atof(sh); if (A < 1.4) A = 0.0; if (A > 4.0) A = 4.0; }      /* bounded: a stray value must not ask for a huge render target */
    if (A > 1.0) {
        char buf[64]; const char *rh = getenv("OT_RENDER_H"); unsigned h = (rh && atoi(rh) >= 480) ? (unsigned)atoi(rh) : 1080u;
        if (h > 4320u) h = 4320u;
        unsigned w = (unsigned)(h * A + 0.5) & ~1u;
        setenv("XBOX_ASPECT", "auto", 1);
        snprintf(buf, sizeof buf, "%ux%u", w, h & ~1u);
        setenv("XBOX_RENDER", buf, 1);
    } else if (getenv("OT_RENDER_H") && atoi(getenv("OT_RENDER_H")) >= 480) {
        char buf[64]; unsigned h = (unsigned)atoi(getenv("OT_RENDER_H")); if (h > 4320u) h = 4320u; h &= ~1u;
        snprintf(buf, sizeof buf, "%ux%u", (unsigned)(h * 4 / 3) & ~1u, h);
        setenv("XBOX_RENDER", buf, 1);
    }
    return A;
}

/* ---- frame rate and presentation ---------------------------------------------------------------------------------------
 * The game's own pacing above 60 (fps_cap.c: extra renders between the 60 Hz ticks, with interpolation) is chosen by
 * XBOX_FPS_CAP (60 | 120 | 144 | 240 | N | 0 = no limit | monitor); OT_FPS is the short name. It only makes extra frames if
 * a present does not wait for the display, so a cap above 60 presents without waiting unless vsync is asked for.
 * XBOX_SYNC / OT_VSYNC:  vsync (one frame per refresh at most, the cap is lowered to the display's rate), adaptive
 * (mailbox presentation where the system has it: the newest frame is shown at each refresh, no tearing, no waiting),
 * off or unset with a cap above 60 (present at once: tearing possible in full screen); unset at 60: vsync. */
static int s_display_hz = 60;
static int hook_monitor_hz(void) { return s_display_hz; }
extern int (*d3d8_monitor_hz_hook)(void);

static SDL_GPUPresentMode choose_present_mode(SDL_GPUDevice *gd, SDL_Window *win)
{
    const char *fps = getenv("OT_FPS"), *vs = getenv("OT_VSYNC");
    SDL_DisplayID d = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *m = d ? SDL_GetDesktopDisplayMode(d) : NULL;
    int above60;
    const char *sync;
    if (fps && fps[0] && !getenv("XBOX_FPS_CAP")) setenv("XBOX_FPS_CAP", fps, 1);
    {   /* OT_BUTTONS=modern|playstation|ps2 (auto = modern) -> XBOX_BUTTON_ICONS; OT_HD_TEXTURES=<pack folder> -> XBOX_HD_TEXTURES,
         * OT_HD_MENUS=1 -> XBOX_HD_TEXTURES_MENUS: the Windows layer's own names */
        const char *bt = getenv("OT_BUTTONS"), *hd = getenv("OT_HD_TEXTURES"), *hm = getenv("OT_HD_MENUS");
        if (bt && bt[0] && !getenv("XBOX_BUTTON_ICONS")) setenv("XBOX_BUTTON_ICONS", bt, 1);
        if (hd && hd[0] && !getenv("XBOX_HD_TEXTURES")) setenv("XBOX_HD_TEXTURES", hd, 1);
        if (hm && hm[0] == '1' && !getenv("XBOX_HD_TEXTURES_MENUS")) setenv("XBOX_HD_TEXTURES_MENUS", "1", 1);
    }
    {   /* OT_SOFT_SHADOWS=on -> XBOX_SOFT_SHADOWS=1 */
        const char *ss = getenv("OT_SOFT_SHADOWS");
        if (ss && (!strcmp(ss, "on") || !strcmp(ss, "1")) && !getenv("XBOX_SOFT_SHADOWS")) setenv("XBOX_SOFT_SHADOWS", "1", 1);
    }
    {   /* OT_SMAA=low|medium|high|ultra (anything else: off) -> the Windows layer's XBOX_SMAA / XBOX_SMAA_PRESET */
        const char *sm = getenv("OT_SMAA");
        if (sm && sm[0] && !getenv("XBOX_SMAA") &&
            (!strcmp(sm, "low") || !strcmp(sm, "medium") || !strcmp(sm, "high") || !strcmp(sm, "ultra"))) {
            setenv("XBOX_SMAA", "1", 1);
            setenv("XBOX_SMAA_PRESET", sm, 1);
        }
    }
    if (vs && vs[0] && !getenv("XBOX_SYNC")) setenv("XBOX_SYNC", vs, 1);
    if (m && m->refresh_rate > 1.0f) s_display_hz = (int)(m->refresh_rate + 0.5f);
    d3d8_monitor_hz_hook = hook_monitor_hz;
    {
        const char *c = getenv("XBOX_FPS_CAP");
        above60 = c && c[0] && (!strcmp(c, "0") || (!strcmp(c, "monitor") ? s_display_hz > 61 : atoi(c) > 60));
    }
    sync = getenv("XBOX_SYNC");
    fprintf(stderr, "[WINDOW] display %d Hz, frame limit %s, sync %s\n", s_display_hz,
            getenv("XBOX_FPS_CAP") ? getenv("XBOX_FPS_CAP") : "60", sync && sync[0] ? sync : "default");
    if (sync && !strcmp(sync, "vsync")) return SDL_GPU_PRESENTMODE_VSYNC;
    if (sync && !strcmp(sync, "adaptive"))
        return SDL_WindowSupportsGPUPresentMode(gd, win, SDL_GPU_PRESENTMODE_MAILBOX) ? SDL_GPU_PRESENTMODE_MAILBOX
                                                                                     : SDL_GPU_PRESENTMODE_IMMEDIATE;
    if ((sync && !strcmp(sync, "off")) || above60) return SDL_GPU_PRESENTMODE_IMMEDIATE;
    return SDL_GPU_PRESENTMODE_VSYNC;
}

static void screenshot_request(void)
{
    extern void d3d8_gpu_request_screenshot(const char *path);
    char dir[1024], path[1200]; time_t t = time(NULL); struct tm tmv;
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(dir, sizeof dir, "%s/Pictures/OpenTricky", home);
    mkdir(dir, 0755);
    localtime_r(&t, &tmv);
    snprintf(path, sizeof path, "%s/SSX-Tricky-%04d%02d%02d-%02d%02d%02d.png", dir, tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    d3d8_gpu_request_screenshot(path);
}

static int16_t axis_y(int16_t v) { return v == -32768 ? 32767 : (int16_t)-v; }   /* SDL down is +, XInput up is + */

static void input_snapshot(SDL_Window *win)
{
    int n = 0, i, slot;
    const bool *ks = SDL_GetKeyboardState(&n);
    static uint8_t vk_by_sc[SDL_SCANCODE_COUNT]; static int init;
    if (!init) {
        init = 1;
        for (i = 0; i < (int)(sizeof k_vk / sizeof k_vk[0]); i++) vk_by_sc[k_vk[i].sc] = k_vk[i].vk;
        for (i = 0; i < 26; i++) vk_by_sc[SDL_SCANCODE_A + i] = (uint8_t)('A' + i);
        for (i = 0; i < 9; i++) vk_by_sc[SDL_SCANCODE_1 + i] = (uint8_t)('1' + i);
        vk_by_sc[SDL_SCANCODE_0] = '0';
        for (i = 0; i < 12; i++) vk_by_sc[SDL_SCANCODE_F1 + i] = (uint8_t)(0x70 + i);
    }
    if (ks) {
        uint8_t now[256]; memset(now, 0, sizeof now);
        for (i = 0; i < n && i < SDL_SCANCODE_COUNT; i++) if (ks[i] && vk_by_sc[i]) now[vk_by_sc[i]] = 1;
        now[0x10] = now[0xA0] | now[0xA1]; now[0x11] = now[0xA2] | now[0xA3]; now[0x12] = now[0xA4] | now[0xA5];
        for (i = 0; i < 256; i++) s_key[i] = now[i];
    }
    s_focus = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    pthread_mutex_lock(&s_pad_lock);
    for (slot = 0; slot < 4; slot++) {
        SDL_Gamepad *g = s_gp[slot];
        OtXPad p; uint16_t b = 0;
        if (!g) { s_pad_on[slot] = 0; continue; }
        memset(&p, 0, sizeof p);
#define BTN(sdl, bit) if (SDL_GetGamepadButton(g, sdl)) b |= (bit)
        BTN(SDL_GAMEPAD_BUTTON_DPAD_UP, 0x1); BTN(SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x2); BTN(SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x4);
        BTN(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x8); BTN(SDL_GAMEPAD_BUTTON_START, 0x10); BTN(SDL_GAMEPAD_BUTTON_BACK, 0x20);
        BTN(SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x40); BTN(SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x80);
        BTN(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x100); BTN(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x200);
        BTN(SDL_GAMEPAD_BUTTON_SOUTH, 0x1000); BTN(SDL_GAMEPAD_BUTTON_EAST, 0x2000);
        BTN(SDL_GAMEPAD_BUTTON_WEST, 0x4000); BTN(SDL_GAMEPAD_BUTTON_NORTH, 0x8000);
#undef BTN
        p.buttons = b;
        p.lt = (uint8_t)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
        p.rt = (uint8_t)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
        p.lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX); p.ly = axis_y(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
        p.rx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX); p.ry = axis_y(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
        if (memcmp(&p.buttons, &s_pad[slot].buttons, sizeof p - 4)) p.packet = s_pad[slot].packet + 1; else p.packet = s_pad[slot].packet;
        s_pad[slot] = p; s_pad_on[slot] = 1;
    }
    pthread_mutex_unlock(&s_pad_lock);
}
static uint8_t *s_frame;          /* latest RGBA frame */
static unsigned s_w, s_h;
static unsigned long s_frame_seq; /* bumped on every post */

void ot_window_post_frame(const uint8_t *rgba, unsigned w, unsigned h)
{
    size_t n = (size_t)w * h * 4;
    if (!rgba || !w || !h) return;
    pthread_mutex_lock(&s_lock);
    if (w != s_w || h != s_h || !s_frame) {
        free(s_frame);
        s_frame = (uint8_t *)malloc(n);
        s_w = w; s_h = h;
    }
    if (s_frame) { memcpy(s_frame, rgba, n); s_frame_seq++; }
    pthread_mutex_unlock(&s_lock);
}

/* ---- the guest's thread ---- */
typedef struct { int (*fn)(void *); void *arg; int result; volatile int done; } guest_t;

static void *guest_thread(void *p)
{
    guest_t *g = (guest_t *)p;
    g->result = g->fn(g->arg);
    __atomic_store_n(&g->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int ot_window_wanted(void)
{
    const char *e = getenv("OT_WINDOW"), *h = getenv("XBOX_HEADLESS");
    if (e && e[0] == '0') return 0;
    if (h && h[0] == '1') return 0;
    return 1;
}

static void save_ppm(SDL_Renderer *r, const char *path)
{
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
    FILE *f;
    int x, y;
    if (!s) { fprintf(stderr, "[WINDOW] readback failed: %s\n", SDL_GetError()); return; }
    {   SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(s); s = c; }
    if (!s || !(f = fopen(path, "wb"))) { if (s) SDL_DestroySurface(s); return; }
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (y = 0; y < s->h; y++) {
        const uint8_t *row = (const uint8_t *)s->pixels + (size_t)y * s->pitch;
        for (x = 0; x < s->w; x++) fwrite(row + x * 4, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "[WINDOW] saved what the window shows (%dx%d) to %s\n", s->w, s->h, path);
    SDL_DestroySurface(s);
}

#if !defined(_WIN32)
/* Window size on the keyboard. Cmd+1..4 = 540 / 720 / 900 / 1080 high, F9 smaller, F10 larger (each leaves full screen first); the width
 * keeps the shape the window was opened with. OT_WINDOW_H sets the height it opens at. */
static double s_win_aspect = 4.0 / 3.0;
static void window_height(SDL_Window *win, int h)
{
    SDL_Rect usable; int w;
    if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) { SDL_SetWindowFullscreen(win, false); SDL_SyncWindow(win); }
    if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &usable)) {      /* never larger than the screen's usable area */
        if (h > usable.h - 60) h = usable.h - 60;
        if ((int)(h * s_win_aspect) > usable.w) h = (int)(usable.w / s_win_aspect);
    }
    if (h < 360) h = 360;
    w = (int)(h * s_win_aspect + 0.5);
    SDL_SetWindowSize(win, w, h);
    SDL_SyncWindow(win);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    fprintf(stderr, "[WINDOW] size %dx%d\n", w, h);
}
static void window_step(SDL_Window *win, double f)
{
    int w = 0, h = 0;
    if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) { SDL_SetWindowFullscreen(win, false); SDL_SyncWindow(win); }
    SDL_GetWindowSize(win, &w, &h);
    window_height(win, (int)(h * f + 0.5));
}
/* The settings window (OT_TOOL, set by the app's launcher): Cmd+, or F2. One at a time. */
extern void ot_menu_install(void);
static pid_t s_settings_pid;
static void open_settings(SDL_Window *win)
{
    const char *tool = getenv("OT_TOOL");
    char *argv[] = { (char *)tool, (char *)"gui", (char *)"--live", NULL };
    if (!tool || !tool[0]) { fprintf(stderr, "[WINDOW] settings window: only in the SSX Tricky app (OT_TOOL is not set)\n"); return; }
    if (s_settings_pid > 0 && waitpid(s_settings_pid, NULL, WNOHANG) == 0) return;      /* still open */
    s_settings_pid = 0;
    if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) { SDL_SetWindowFullscreen(win, false); SDL_SyncWindow(win); }
    if (posix_spawn(&s_settings_pid, tool, NULL, NULL, argv, environ) != 0) { s_settings_pid = 0; fprintf(stderr, "[WINDOW] could not start %s\n", tool); }
}
#endif

static volatile int s_mouse_moved;

/* Controller vibration for the title's XInputSetState (called from the game thread): the pad in that slot rumbles for a second unless the title
 * refreshes or stops it sooner. OT_RUMBLE=0 turns it off. Returns 1 if a pad took it. */
int ot_window_rumble(int port, unsigned short low, unsigned short high)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("OT_RUMBLE"); on = !(e && e[0] == '0'); }
    static unsigned calls, told;
    int ok;
    calls++;
    if (port < 0 || port >= 4 || !s_gp[port]) {
        if (told++ < 3) fprintf(stderr, "[RUMBLE] the title asked for vibration on port %d (low %u high %u): no controller in that slot\n", port, low, high);
        return 0;
    }
    if (!on) return 1;
    ok = SDL_RumbleGamepad(s_gp[port], low, high, low || high ? 1000 : 0) ? 1 : 0;
    if (told++ < 6 || !ok)
        if (told < 40) { told++; fprintf(stderr, "[RUMBLE] call %u: port %d low %u high %u on %s -> %s%s%s\n", calls, port, low, high, SDL_GetGamepadName(s_gp[port]), ok ? "ok" : "FAILED (", ok ? "" : SDL_GetError(), ok ? "" : ")"); }
    return ok;
}
int ot_window_run(int (*guest)(void *), void *arg)
{
    guest_t g = { guest, arg, 0, 0 };
    pthread_t th;
    pthread_attr_t attr;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    unsigned tex_w = 0, tex_h = 0;
    unsigned long shown_seq = 0, frames = 0;
    const char *shot = getenv("OT_WINDOW_SHOT");
    const char *sf = getenv("OT_WINDOW_SHOT_FRAMES");
    unsigned long shot_after = (sf && atoi(sf) > 0) ? (unsigned long)atoi(sf) : 90;
    int shot_done = 0, quit = 0;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[WINDOW] SDL_Init failed (%s): running without a window\n", SDL_GetError());
        return guest(arg);
    }
#if defined(OT_GPU_RENDERER)
    {
        double shape = choose_display();
        int ww = 1200, wh = 900;
        if (shape > 1.0) { wh = 900; ww = (int)(900.0 * shape + 0.5); if (ww > 2400) { ww = 2400; wh = (int)(2400.0 / shape + 0.5); } }
        {   const char *oh = getenv("OT_WINDOW_H"); int h = oh ? atoi(oh) : 0;      /* the height the window opens at: 360 .. 2160 */
            if (h >= 360 && h <= 2160) { ww = (int)((double)ww * h / wh + 0.5); wh = h; }
        }
#if !defined(_WIN32)
        s_win_aspect = (double)ww / (double)wh;
#endif
        win = SDL_CreateWindow("SSX Tricky (OpenTricky, macOS)", ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
#if !defined(_WIN32)
        if (win) ot_menu_install();
#endif
        if (win && (getenv("XBOX_FULLSCREEN") ? getenv("XBOX_FULLSCREEN")[0] == '1' : getenv("OT_FULLSCREEN") && getenv("OT_FULLSCREEN")[0] == '1'))
            SDL_SetWindowFullscreen(win, true);
    }
    {
        /* The GPU renderer (xboxrecomp/src/d3d/d3d8_gpu.c) owns the window's swapchain: it draws and presents
         * from the thread that presents. This thread only runs the event loop. */
        extern int d3d8_gpu_attach(void *device, void *window);
        SDL_GPUDevice *gd = win ? SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, getenv("OT_GPU_DEBUG") != NULL, NULL) : NULL;
        if (!gd || !SDL_ClaimWindowForGPUDevice(gd, win)) {
            fprintf(stderr, "[WINDOW] no GPU device (%s): running without a window\n", SDL_GetError());
            if (win) SDL_DestroyWindow(win);
            SDL_Quit();
            return guest(arg);
        }
        {   /* OT_HDR=on: HDR output (extended linear, 1.0 = SDR white) where the display allows it; the colour pass of
             * d3d8_gpu_out.c then gives the picture a wider gamut and lets colourful highlights use the display's headroom */
            extern void d3d8_gpu_set_hdr(int on);
            const char *hd = getenv("OT_HDR");
            SDL_GPUSwapchainComposition comp = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
            if (hd && (!strcmp(hd, "on") || !strcmp(hd, "1"))) {
                if (SDL_WindowSupportsGPUSwapchainComposition(gd, win, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR)) comp = SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;
                else fprintf(stderr, "[WINDOW] HDR output is not available on this display: SDR\n");
            }
            SDL_SetGPUSwapchainParameters(gd, win, comp, choose_present_mode(gd, win));
            if (comp == SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR) {
                d3d8_gpu_set_hdr(1);
                fprintf(stderr, "[WINDOW] HDR output (extended linear), window headroom %.2f\n",
                        SDL_GetFloatProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f));
            }
        }
        d3d8_gpu_attach(gd, win);
        fprintf(stderr, "[WINDOW] GPU renderer on %s\n", SDL_GetGPUDeviceDriver(gd));
        ren = NULL;
    }
    {
#else
    win = SDL_CreateWindow("SSX Tricky (OpenTricky, macOS: no 3D yet)", 1280, 960,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    {
#endif
#if !defined(OT_GPU_RENDERER)
    if (!ren) {
        fprintf(stderr, "[WINDOW] could not open a window (%s): running without one\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return guest(arg);
    }
    fprintf(stderr, "[WINDOW] %s renderer, video driver %s\n", SDL_GetRendererName(ren), SDL_GetCurrentVideoDriver());
    SDL_SetRenderLogicalPresentation(ren, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);   /* the title's 4:3 */
#endif
    }

    w32_key_hook = hook_key; w32_pad_hook = hook_pad; w32_focus_hook = hook_focus;
    pad_open_all();
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GUEST_STACK_BYTES);
    if (pthread_create(&th, &attr, guest_thread, &g) != 0) {
        fprintf(stderr, "[WINDOW] could not start the game thread\n");
        return 1;
    }
    pthread_attr_destroy(&attr);

    while (!quit && !__atomic_load_n(&g.done, __ATOMIC_ACQUIRE)) {
        SDL_Event ev;
        int have;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_MOUSE_MOTION || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_WHEEL) s_mouse_moved = 1;
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = 1;
            else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
                     (ev.key.key == SDLK_F11 || (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))))
                { bool on = !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN); SDL_SetWindowFullscreen(win, on); fprintf(stderr, "[WINDOW] fullscreen %s\n", on ? "on" : "off"); }
            else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_F12) screenshot_request();
#if !defined(_WIN32)
            else if (ev.type == SDL_EVENT_USER) {      /* the menu bar (ot_menu_macos.m) */
                switch (ev.user.code) {
                case 1: open_settings(win); break;
                case 2: { bool on = !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN); SDL_SetWindowFullscreen(win, on); } break;
                case 3: screenshot_request(); break;
                case 4: window_step(win, 0.8); break;
                case 5: window_step(win, 1.25); break;
                case 10: window_height(win, 540); break;
                case 11: window_height(win, 720); break;
                case 12: window_height(win, 900); break;
                case 13: window_height(win, 1080); break;
                }
            }
            else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_F9) window_step(win, 0.8);
            else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_F10) window_step(win, 1.25);
            else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_F2) open_settings(win);
            else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && (ev.key.mod & SDL_KMOD_GUI)) {
                if (ev.key.key == SDLK_COMMA) open_settings(win);
                else if (ev.key.key == SDLK_1) window_height(win, 540);
                else if (ev.key.key == SDLK_2) window_height(win, 720);
                else if (ev.key.key == SDLK_3) window_height(win, 900);
                else if (ev.key.key == SDLK_4) window_height(win, 1080);
            }
#endif
#if !defined(_WIN32)
            else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                /* the settings window from the pad: the Guide / PlayStation button, or Back (Share) and Start (Options) together */
                SDL_Gamepad *gp = SDL_GetGamepadFromID(ev.gbutton.which);
                int b = ev.gbutton.button;
                if (b == SDL_GAMEPAD_BUTTON_GUIDE ||
                    (gp && ((b == SDL_GAMEPAD_BUTTON_START && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK)) ||
                            (b == SDL_GAMEPAD_BUTTON_BACK && SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START)))))
                    open_settings(win);
            }
#endif
            else if (ev.type == SDL_EVENT_GAMEPAD_ADDED) pad_open_all();
            else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                int k;
                for (k = 0; k < 4; k++)
                    if (s_gp[k] && !SDL_GamepadConnected(s_gp[k])) { SDL_CloseGamepad(s_gp[k]); s_gp[k] = NULL; fprintf(stderr, "[INPUT] pad %d removed\n", k); }
            }
        }
        {   /* the mouse pointer hides after OT_CURSOR_HIDE_MS (default 2000, 0 = never) without movement, and returns when the mouse moves */
            static int hide_ms = -1, hidden; static Uint64 last_move;
            Uint64 now = SDL_GetTicks();
            if (hide_ms < 0) { const char *e = getenv("OT_CURSOR_HIDE_MS"); hide_ms = (e && e[0]) ? atoi(e) : 2000; if (hide_ms < 0) hide_ms = 0; last_move = now; }
            if (hide_ms > 0) {
                if (s_mouse_moved) { s_mouse_moved = 0; last_move = now; if (hidden) { SDL_ShowCursor(); hidden = 0; } }
                else if (!hidden && now - last_move >= (Uint64)hide_ms && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) { SDL_HideCursor(); hidden = 1; }
                else if (hidden && !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) { SDL_ShowCursor(); hidden = 0; last_move = now; }
            }
        }
        input_snapshot(win);
        {   /* tests: OT_TEST_SCREENSHOT_AT=<seconds> presses F12, OT_TEST_FULLSCREEN_AT=<seconds> presses F11 */
            static Uint64 t0; static int shot_done, fs_done; Uint64 now = SDL_GetTicks();
            const char *a = getenv("OT_TEST_SCREENSHOT_AT"), *f = getenv("OT_TEST_FULLSCREEN_AT");
            if (!t0) t0 = now;
            if (a && !shot_done && now - t0 > (Uint64)(atof(a) * 1000.0)) { shot_done = 1; screenshot_request(); }
            if (f && !fs_done && now - t0 > (Uint64)(atof(f) * 1000.0)) {
                bool on = !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN);
                fs_done = 1; SDL_SetWindowFullscreen(win, on); fprintf(stderr, "[WINDOW] fullscreen %s\n", on ? "on" : "off");
            }
        }

#if defined(OT_GPU_RENDERER)
        SDL_Delay(4);        /* the GPU renderer presents from the game's thread */
        continue;
#endif
        pthread_mutex_lock(&s_lock);
        have = s_frame && s_frame_seq != shown_seq;
        if (have) {
            if (!tex || tex_w != s_w || tex_h != s_h) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)s_w, (int)s_h);
                if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
                tex_w = s_w; tex_h = s_h;
            }
            if (tex) SDL_UpdateTexture(tex, NULL, s_frame, (int)s_w * 4);
            shown_seq = s_frame_seq;
        }
        pthread_mutex_unlock(&s_lock);

        if (have && tex) {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren, tex, NULL, NULL);
            frames++;
            if (shot && !shot_done && frames >= shot_after) { save_ppm(ren, shot); shot_done = 1; }
            SDL_RenderPresent(ren);
        } else {
            SDL_Delay(4);
        }
    }

    if (quit) {
        fprintf(stderr, "[WINDOW] closed: exiting\n");
        fflush(NULL);
        _exit(0);       /* the game threads are killed with the process; no atexit handlers or destructors run under them */
    }
    pthread_join(th, NULL);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return g.result;
}

#else /* no window in this build */

int ot_window_wanted(void) { return 0; }
int ot_window_run(int (*guest)(void *), void *arg) { return guest(arg); }
void ot_window_post_frame(const uint8_t *rgba, unsigned w, unsigned h) { (void)rgba; (void)w; (void)h; }

#endif
