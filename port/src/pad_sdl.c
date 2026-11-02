/*
 * pad_sdl.c -- see pad_sdl.h.
 *
 * SDL lives on a thread of its own ("pad thread"): it starts SDL, polls the
 * pads every 2 ms and publishes their state; the game's threads only copy
 * that state under a lock and leave requests (rumble, player lights, quit),
 * never calling SDL. So SDL's start (device enumeration, its helper thread
 * and message windows) never stalls the game, and the COM apartment of the
 * game's threads (XAudio2, D3D: MTA) is never chosen by SDL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pad_sdl.h"

int padsdl_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("XBOX_PS_PADS");
        v = e && e[0] == '1';
#ifndef SSX_HAVE_SDL3
        if (v) fprintf(stderr, "[PAD] PlayStation controllers: not in this build (SSX_PS_PADS=OFF)\n");
        v = 0;
#endif
    }
    return v;
}

#ifdef SSX_HAVE_SDL3
#include <objbase.h>
#include <SDL3/SDL.h>

#define POLL_MS 2               /* the pad thread's period (timeBeginPeriod(1) in main.c) */
#define SCAN_MS 1000            /* pads plugged in or unplugged: looked for this often */
#define RUMBLE_MS 500           /* each rumble request lasts this long ... */
#define RUMBLE_RENEW_MS 200     /* ... and is renewed this often while it is not 0, 0 */
#define NAME_LEN 64

/* Shared with the game's threads, under s_lock. A request (rumble, player)
 * names the pad it is for: one for a pad that is no longer in its slot is
 * dropped, and a pad new in a slot starts with none. */
static SRWLOCK s_lock = SRWLOCK_INIT;
static DWORD s_id[PADSDL_SLOTS];                /* each slot's pad (SDL's instance id), 0: empty */
static XINPUT_GAMEPAD s_snap[PADSDL_SLOTS];     /* its state at the last poll */
static char s_name[PADSDL_SLOTS][NAME_LEN];
static WORD s_req[PADSDL_SLOTS][2];             /* rumble asked by the game */
static int s_req_new[PADSDL_SLOTS];
static int s_player[PADSDL_SLOTS];              /* player lights asked by the game */
static int s_player_new[PADSDL_SLOTS];
static volatile LONG s_started = 0;
static volatile LONG s_quit = 0, s_quit_done = 0;
static volatile DWORD s_thread_id;
static DWORD s_t0;                              /* the game's first call (GetTickCount) */

/* The pad thread's own. */
static SDL_Gamepad *s_pad[PADSDL_SLOTS];
static DWORD s_pad_id[PADSDL_SLOTS];
static WORD s_rumble[PADSDL_SLOTS][2];
static DWORD s_rumble_at[PADSDL_SLOTS];

static int is_playstation(SDL_GamepadType t)
{
    return t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
}

/* Before SDL_Init, above whatever the environment says: SDL reaches the
 * PlayStation pads through its HIDAPI drivers for them and nothing else. Its
 * XInput, DirectInput, raw input, Windows.Gaming.Input and GameInput back
 * ends stay off, so the Xbox controllers are left to the game's own XInput
 * reads: SDL never enumerates, opens or reads them. Nor does SDL take the
 * process's Ctrl+C / termination signals. */
static void set_hints(void)
{
    static const struct { const char *name, *value; } k[] = {
        { SDL_HINT_JOYSTICK_HIDAPI, "0" },          /* every HIDAPI driver off, ... */
        { SDL_HINT_JOYSTICK_HIDAPI_PS4, "1" },      /* ... but the DualShock 4's */
        { SDL_HINT_JOYSTICK_HIDAPI_PS5, "1" },      /* ... and the DualSense's */
        { SDL_HINT_XINPUT_ENABLED, "0" },
        { SDL_HINT_JOYSTICK_DIRECTINPUT, "0" },
        { SDL_HINT_JOYSTICK_RAWINPUT, "0" },
        { SDL_HINT_JOYSTICK_WGI, "0" },
        { SDL_HINT_JOYSTICK_GAMEINPUT, "0" },
        /* no SDL window: read the pads whatever window is in front, as XInput
         * does (the game checks its focus itself) */
        { SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1" },
        { SDL_HINT_NO_SIGNAL_HANDLERS, "1" },
    };
    size_t i;
    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        SDL_SetHintWithPriority(k[i].name, k[i].value, SDL_HINT_OVERRIDE);
}

/* SDL's HIDAPI loads hid.dll and cfgmgr32.dll by name, which looks in the
 * game's folder first: loaded here from the system folder, the names then
 * resolve to these copies. */
static void system_dlls(void)
{
    static const char *k[] = { "hid.dll", "cfgmgr32.dll" };
    size_t i;
    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!LoadLibraryExA(k[i], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32))
            fprintf(stderr, "[PAD] %s not loaded from the system folder (%lu)\n", k[i], GetLastError());
}

/* Nothing reads SDL's event queue: the state is polled. Every joystick and
 * gamepad event is turned off, the "update complete" ones too (SDL would
 * otherwise queue one at each change of a pad's state, up to its queue's
 * limit); anything left in the queue is dropped at each scan. */
static void no_events(void)
{
    SDL_SetJoystickEventsEnabled(false);
    SDL_SetGamepadEventsEnabled(false);
    SDL_SetEventEnabled(SDL_EVENT_JOYSTICK_UPDATE_COMPLETE, false);
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_UPDATE_COMPLETE, false);
    SDL_SetEventEnabled(SDL_EVENT_JOYSTICK_BATTERY_UPDATED, false);
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED, false);
}

static const char *type_name(SDL_GamepadType t)
{
    return is_playstation(t) ? "PlayStation" : "not used";
}

static const char *or_unknown(const char *s) { return s ? s : "?"; }

/* One line for bug reports: the controllers SDL sees, each time the list changes. */
static void log_list(void)
{
    static unsigned last_sig = 0;
    int count = 0, j;
    unsigned sig;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    sig = (unsigned)count * 2654435761u + 1u;
    for (j = 0; ids && j < count; j++) sig = sig * 31u + (unsigned)ids[j];
    if (sig != last_sig) {
        last_sig = sig;
        fprintf(stderr, "[PAD] controllers seen by SDL: %d", count);
        for (j = 0; ids && j < count; j++)
            fprintf(stderr, " [%s: %s]", or_unknown(SDL_GetGamepadNameForID(ids[j])),
                    type_name(SDL_GetGamepadTypeForID(ids[j])));
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    SDL_free(ids);
}

/* Pads unplugged are closed and their slot freed; PlayStation pads plugged
 * in are opened into the first free slot. No pad ever changes slot. */
static void scan(void)
{
    int i, count = 0, j;
    SDL_JoystickID *ids;
    for (i = 0; i < PADSDL_SLOTS; i++)
        if (s_pad[i] && !SDL_GamepadConnected(s_pad[i])) {
            fprintf(stderr, "[PAD] %s disconnected (slot %d)\n", or_unknown(SDL_GetGamepadName(s_pad[i])), i);
            SDL_CloseGamepad(s_pad[i]);
            s_pad[i] = NULL;
            s_pad_id[i] = 0;
        }
    log_list();
    ids = SDL_GetGamepads(&count);
    for (j = 0; ids && j < count; j++) {
        int open = 0, slot = -1;
        if (!is_playstation(SDL_GetGamepadTypeForID(ids[j]))) continue;
        for (i = 0; i < PADSDL_SLOTS; i++) {
            if (s_pad[i] && s_pad_id[i] == (DWORD)ids[j]) open = 1;
            if (!s_pad[i] && slot < 0) slot = i;
        }
        if (open || slot < 0) continue;
        s_pad[slot] = SDL_OpenGamepad(ids[j]);
        if (s_pad[slot]) {
            fprintf(stderr, "[PAD] %s connected (PlayStation, slot %d)\n", or_unknown(SDL_GetGamepadName(s_pad[slot])), slot);
            s_pad_id[slot] = (DWORD)ids[j];
            s_rumble[slot][0] = s_rumble[slot][1] = 0;
            s_rumble_at[slot] = 0;
        }
    }
    SDL_free(ids);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    fflush(stderr);
}

static SHORT axis(SDL_Gamepad *g, SDL_GamepadAxis a, BOOL flip)
{
    int v = SDL_GetGamepadAxis(g, a);
    /* SDL: down is positive; XInput: up is. ~v, as SDL turns XInput's Y
     * into its own (and the game's SDL2 back end the other way): the full
     * range maps onto the full range, and SDL's 0 reads -1. */
    if (flip) v = -v - 1;
    return (SHORT)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static BYTE trigger(SDL_Gamepad *g, SDL_GamepadAxis a)
{
    int v = SDL_GetGamepadAxis(g, a);           /* 0 .. 32767 */
    return (BYTE)(v <= 0 ? 0 : v >= 32767 ? 255 : v * 255 / 32767);
}

static void read_pad(SDL_Gamepad *g, XINPUT_GAMEPAD *out)
{
    static const struct { SDL_GamepadButton b; WORD bit; } map[] = {
        { SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A }, { SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B },
        { SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X }, { SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK }, { SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB },
        { SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
    };
    size_t i;
    memset(out, 0, sizeof *out);
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GetGamepadButton(g, map[i].b)) out->wButtons |= map[i].bit;
    out->bLeftTrigger  = trigger(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    out->bRightTrigger = trigger(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    out->sThumbLX = axis(g, SDL_GAMEPAD_AXIS_LEFTX, FALSE);
    out->sThumbLY = axis(g, SDL_GAMEPAD_AXIS_LEFTY, TRUE);
    out->sThumbRX = axis(g, SDL_GAMEPAD_AXIS_RIGHTX, FALSE);
    out->sThumbRY = axis(g, SDL_GAMEPAD_AXIS_RIGHTY, TRUE);
}


/* The game ends: motors stopped, pads closed (SDL waits there up to 30 ms
 * for a rumble report on its way), nothing published any more. */
static void quit_pads(void)
{
    int i, n = 0;
    for (i = 0; i < PADSDL_SLOTS; i++)
        if (s_pad[i]) {
            SDL_RumbleGamepad(s_pad[i], 0, 0, 0);
            SDL_CloseGamepad(s_pad[i]);
            s_pad[i] = NULL;
            s_pad_id[i] = 0;
            n++;
        }
    AcquireSRWLockExclusive(&s_lock);
    memset(s_id, 0, sizeof s_id);
    ReleaseSRWLockExclusive(&s_lock);
    fprintf(stderr, "[PAD] PlayStation controllers closed (%d), motors stopped\n", n);
    fflush(stderr);
}

static DWORD WINAPI pad_thread(LPVOID arg)
{
    DWORD scan_at = 0;
    (void)arg;
    s_thread_id = GetCurrentThreadId();
    /* The game's own choice (XAudio2, D3D), so SDL keeps it on this thread. */
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    system_dlls();
    set_hints();
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[PAD] PlayStation controllers unavailable: %s\n", SDL_GetError());
        fflush(stderr);
        InterlockedExchange(&s_quit_done, 1);
        return 0;
    }
    no_events();
    fprintf(stderr, "[PAD] PlayStation controllers on (experimental), SDL %d.%d.%d\n",
            SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
    fflush(stderr);
    for (;;) {
        DWORD now = GetTickCount();
        DWORD id[PADSDL_SLOTS];
        XINPUT_GAMEPAD snap[PADSDL_SLOTS];
        char name[PADSDL_SLOTS][NAME_LEN];
        WORD req[PADSDL_SLOTS][2];
        int req_new[PADSDL_SLOTS], player[PADSDL_SLOTS], player_new[PADSDL_SLOTS], i;

        if (s_quit) {
            quit_pads();
            InterlockedExchange(&s_quit_done, 1);
            return 0;
        }
        SDL_UpdateGamepads();
        if (!scan_at || now - scan_at >= SCAN_MS) {
            scan();
            scan_at = now ? now : 1;
        }
        memset(snap, 0, sizeof snap);
        memset(name, 0, sizeof name);
        for (i = 0; i < PADSDL_SLOTS; i++) {
            id[i] = s_pad[i] ? s_pad_id[i] : 0;
            if (!s_pad[i]) continue;
            read_pad(s_pad[i], &snap[i]);
            snprintf(name[i], NAME_LEN, "%s", or_unknown(SDL_GetGamepadName(s_pad[i])));
        }

        AcquireSRWLockExclusive(&s_lock);
        for (i = 0; i < PADSDL_SLOTS; i++) {
            if (s_id[i] != id[i]) {             /* a pad new in this slot (or none): no request yet */
                s_req[i][0] = s_req[i][1] = 0;
                s_req_new[i] = 0;
                s_player[i] = -1;
                s_player_new[i] = 0;
            }
            s_id[i] = id[i];
            memcpy(s_name[i], name[i], NAME_LEN);
        }
        memcpy(s_snap, snap, sizeof snap);
        memcpy(req, s_req, sizeof req);
        memcpy(req_new, s_req_new, sizeof req_new);
        memcpy(player, s_player, sizeof player);
        memcpy(player_new, s_player_new, sizeof player_new);
        memset(s_req_new, 0, sizeof s_req_new);
        memset(s_player_new, 0, sizeof s_player_new);
        ReleaseSRWLockExclusive(&s_lock);

        for (i = 0; i < PADSDL_SLOTS; i++) {
            if (!s_pad[i]) continue;
            /* XInput keeps a speed until told otherwise; SDL wants a duration:
             * a short one, renewed while the game's speed is not 0, 0. */
            if (req_new[i]) {
                s_rumble[i][0] = req[i][0];
                s_rumble[i][1] = req[i][1];
                SDL_RumbleGamepad(s_pad[i], req[i][0], req[i][1], (req[i][0] || req[i][1]) ? RUMBLE_MS : 0);
                s_rumble_at[i] = now;
            } else if ((s_rumble[i][0] || s_rumble[i][1]) && now - s_rumble_at[i] >= RUMBLE_RENEW_MS) {
                SDL_RumbleGamepad(s_pad[i], s_rumble[i][0], s_rumble[i][1], RUMBLE_MS);
                s_rumble_at[i] = now;
            }
            /* the player's number on the pad (DualSense lights, DualShock 4 light bar colour) */
            if (player_new[i]) SDL_SetGamepadPlayerIndex(s_pad[i], player[i]);
        }
        Sleep(POLL_MS);
    }
}

/* Starts the pad thread once; never waits for it (no pad until it is up). */
static void start(void)
{
    if (s_started) return;
    if (InterlockedCompareExchange(&s_started, 1, 0) == 0) {
        HANDLE t;
        s_t0 = GetTickCount();
        t = CreateThread(NULL, 0, pad_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        else {
            fprintf(stderr, "[PAD] PlayStation controllers: no thread (%lu)\n", GetLastError());
            InterlockedExchange(&s_quit_done, 1);
        }
    }
}

int padsdl_count(void)
{
    int n = 0, i;
    if (!padsdl_on()) return 0;
    start();
    AcquireSRWLockShared(&s_lock);
    for (i = 0; i < PADSDL_SLOTS; i++) if (s_id[i]) n++;
    ReleaseSRWLockShared(&s_lock);
    return n;
}

void padsdl_list(DWORD id[PADSDL_SLOTS])
{
    memset(id, 0, sizeof(DWORD) * PADSDL_SLOTS);
    if (!padsdl_on()) return;
    start();
    AcquireSRWLockShared(&s_lock);
    memcpy(id, s_id, sizeof s_id);
    ReleaseSRWLockShared(&s_lock);
}

BOOL padsdl_state(int slot, DWORD id, XINPUT_GAMEPAD *out)
{
    BOOL ok = FALSE;
    memset(out, 0, sizeof *out);
    if (slot < 0 || slot >= PADSDL_SLOTS || !id || !padsdl_on()) return FALSE;
    start();
    AcquireSRWLockShared(&s_lock);
    if (s_id[slot] == id) {
        *out = s_snap[slot];
        ok = TRUE;
    }
    ReleaseSRWLockShared(&s_lock);
    return ok;
}

void padsdl_rumble(int slot, DWORD id, WORD left, WORD right)
{
    if (slot < 0 || slot >= PADSDL_SLOTS || !id || !padsdl_on()) return;
    start();
    AcquireSRWLockExclusive(&s_lock);
    if (s_id[slot] == id && (s_req[slot][0] != left || s_req[slot][1] != right)) {   /* a change */
        s_req[slot][0] = left;
        s_req[slot][1] = right;
        s_req_new[slot] = 1;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void padsdl_set_player(int slot, DWORD id, int player)
{
    if (slot < 0 || slot >= PADSDL_SLOTS || !id || !padsdl_on()) return;
    start();
    AcquireSRWLockExclusive(&s_lock);
    if (s_id[slot] == id && s_player[slot] != player) {
        s_player[slot] = player;
        s_player_new[slot] = 1;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void padsdl_name(int slot, DWORD id, char *buf, size_t size)
{
    if (!size) return;
    buf[0] = 0;
    if (slot < 0 || slot >= PADSDL_SLOTS || !id || !padsdl_on()) return;
    AcquireSRWLockShared(&s_lock);
    if (s_id[slot] == id) snprintf(buf, size, "%s", s_name[slot]);
    ReleaseSRWLockShared(&s_lock);
}

void padsdl_quit(DWORD wait_ms)
{
    DWORD t0;
    if (!s_started) return;                         /* option off, or no pad ever asked for */
    if (GetCurrentThreadId() == s_thread_id) return; /* the pad thread itself crashed: leave SDL alone */
    InterlockedExchange(&s_quit, 1);
    t0 = GetTickCount();
    while (!s_quit_done && GetTickCount() - t0 < wait_ms) Sleep(1);
}

#else   /* built without SDL3 (-DSSX_PS_PADS=OFF) */

int  padsdl_count(void) { return 0; }
void padsdl_list(DWORD id[PADSDL_SLOTS]) { memset(id, 0, sizeof(DWORD) * PADSDL_SLOTS); }
BOOL padsdl_state(int slot, DWORD id, XINPUT_GAMEPAD *out) { (void)slot; (void)id; memset(out, 0, sizeof *out); return FALSE; }
void padsdl_rumble(int slot, DWORD id, WORD left, WORD right) { (void)slot; (void)id; (void)left; (void)right; }
void padsdl_set_player(int slot, DWORD id, int player) { (void)slot; (void)id; (void)player; }
void padsdl_name(int slot, DWORD id, char *buf, size_t size) { (void)slot; (void)id; if (size) buf[0] = 0; }
void padsdl_quit(DWORD wait_ms) { (void)wait_ms; }

#endif
