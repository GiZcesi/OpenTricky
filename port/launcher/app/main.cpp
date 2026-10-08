/*
 * main.cpp -- OpenTricky.exe, the launcher.
 *
 * RmlUi on SDL3 + OpenGL 3. Three documents written in RML/RCSS (ui/): the
 * home page, the settings page (built from the settings registry,
 * settings.h) and the dialogs shown over them (key capture, crash, about...).
 * The look of a track is read from the player's disc image (port/launcher).
 * Keyboard, mouse and controller (SDL gamepads) all drive the same focus.
 *
 * PLAY writes settings.ini and starts "SSX Tricky.exe" beside the launcher;
 * the launcher hides while the game runs and comes back when it ends, with
 * the crash screen if the game left a report (CrashReports\).
 * "Skip launcher next time" starts the game at once; holding Shift (or
 * Select on a controller) while the launcher starts shows it anyway.
 *
 * Command line (all optional; the test options are for the project's tools):
 *   --iso <path>        disc image (else the one in settings.ini)
 *   --track <0-9>       the theme's track (tests; else random at each start)
 *   --page home|settings
 *   --tab <0-5>         the settings tab
 *   --size WxH          window size in pixels (default 1280x800 x display scale)
 *   --no-skip           ignore "Skip launcher next time"
 *   --crash-test <dir>  show the crash screen for this report folder at start
 *   --report-dir <dir>  bug report zips there instead of the Desktop (with a trailing slash)
 *   --script <steps>    test steps, comma separated, one every few frames:
 *                       k:<key> (up down left right enter esc tab q e backspace
 *                       or a letter / digit), p:<button> (a b x y lb rb up down
 *                       left right start back; pushed as SDL controller events),
 *                       click:<id> (an element of the page or dialog),
 *                       mclick:<x>:<y> (the mouse at window pixels, logged;
 *                       mhover:<x>:<y> the same without the click),
 *                       shot:<file.png> (the window's picture), wait:<frames>,
 *                       disc:<path> (as if chosen; empty: none),
 *                       waitgame (until the game has ended), quit
 *   --log <file>        messages and timings to a file
 *   --quit-after <ms>   close after this long (idle measurements)
 */
#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"
#include "ot_render.h"
#include "ot_util.h"
#include "ot_update.h"
#include "ot_install.h"

extern "C" {
#include "ot_disc.h"
#include "ot_image.h"
#include "ot_theme.h"
#include "settings.h"
#include "version.h"
}

/* RmlUi's GL3 renderer loads OpenGL through glad; the read-back for test
 * captures uses the entry points below from the same loader. */
#include "RmlUi_Include_GL3.h"

namespace {

const char k_whatsnew[] =
#include "ot_whatsnew.inc"
    ;

const char *const k_url_releases = "https://github.com/GiZcesi/OpenTricky/releases";
const char *const k_url_github = "https://github.com/GiZcesi/OpenTricky";
const char *const k_url_kofi = "https://ko-fi.com/giz_music";
/* The list of releases (pre-releases included: the channel picks among them). */
const char *const k_url_api_releases = "https://api.github.com/repos/GiZcesi/OpenTricky/releases?per_page=10";
const double k_update_wait_ms = 3000;      /* an answer later than this is not shown (the cache keeps it) */
const char *const k_game_exe = "SSX Tricky.exe";

/* ── Timing and log ──────────────────────────────────────────────── */

double now_ms()
{
    return (double)SDL_GetTicksNS() / 1e6;
}

/* Milliseconds since the process was created (Windows), or -1. */
double ms_since_process_start()
{
#ifdef _WIN32
    FILETIME c, e, k, u, n;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return -1;
    GetSystemTimePreciseAsFileTime(&n);
    ULARGE_INTEGER a, b;
    a.LowPart = c.dwLowDateTime; a.HighPart = c.dwHighDateTime;
    b.LowPart = n.dwLowDateTime; b.HighPart = n.dwHighDateTime;
    return (double)(b.QuadPart - a.QuadPart) / 1e4;
#else
    return -1;
#endif
}

/* The last lines, for a bug report (the launcher has no console). */
std::deque<std::string> g_log_ring;

void otlog(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
    fflush(stderr);
    g_log_ring.push_back(buf);
    if (g_log_ring.size() > 400) g_log_ring.pop_front();
}

std::string hex_rgb(uint32_t argb)
{
    char b[16];
    snprintf(b, sizeof b, "#%06x", argb & 0xFFFFFFu);
    return b;
}

std::string rgba(uint32_t argb, float alpha)
{
    char b[48];
    /* RCSS: the alpha of rgba() is 0-255. */
    snprintf(b, sizeof b, "rgba(%u,%u,%u,%d)", (argb >> 16) & 255, (argb >> 8) & 255, argb & 255, (int)(alpha * 255 + 0.5f));
    return b;
}

void image_to_pixels(const OtImage &im, OtPixels &p)
{
    p.w = im.w;
    p.h = im.h;
    p.rgba.resize((size_t)im.w * im.h * 4);
    for (size_t i = 0; i < (size_t)im.w * im.h; i++) {
        uint32_t c = im.px[i];
        p.rgba[i * 4 + 0] = (c >> 16) & 255;
        p.rgba[i * 4 + 1] = (c >> 8) & 255;
        p.rgba[i * 4 + 2] = c & 255;
        p.rgba[i * 4 + 3] = c >> 24;
    }
}

bool ends_with_ci(const std::string &s, const char *suf)
{
    size_t n = strlen(suf);
    return s.size() >= n && SDL_strcasecmp(s.c_str() + s.size() - n, suf) == 0;
}

/* ── Models ──────────────────────────────────────────────────────── */

/* Launcher actions shown as rows of the settings page (not settings). */
enum {
    A_OPEN_LOGS = 1000, A_BUG_REPORT, A_ABOUT, A_RESTORE,
};

struct Row {
    int id = -1;
    bool is_group = false;
    Rml::String label, value, button;
    bool pc = false, changed = false, pending = false;
    bool arrows = false, toggle = false, on = false, plain = false;
};

/* One control of the remapping table: a PS2 button, its key and its
 * controller input (the sticks have no controller binding). */
struct CtlRow {
    int k = 0;
    Rml::String label, key, pad, icon;
    bool has_pad = false, kchg = false, pchg = false;
};

struct NoteSec {
    Rml::String name;
    std::vector<Rml::String> items;
};

struct App {
    SDL_Window *window = nullptr;
    SDL_GLContext gl = nullptr;
    Rml::Context *ctx = nullptr;
    OtRenderInterface *render = nullptr;
    OtFileInterface files;
    std::string base;               /* the exe's folder, with a trailing slash */
    std::string gl_renderer;

    /* disc and theme */
    std::string iso;
    OtDiscCheck disc{};
    OtDiscAssets assets{};
    bool have_assets = false;
    OtImage hd_card[OT_TRACK_COUNT]{};   /* ui/cards/<id>.png, read only with a verified disc */
    int track = OT_TRACK_SNOW;
    OtTheme theme{};
    bool bullet = false;

    /* settings */
    Settings *set = nullptr;
    std::string ini_read, ini_write;
    bool ini_in_docs = false;
    Rml::DataModelHandle model;
    std::vector<Rml::String> tabs, presets;
    std::vector<Row> rows;
    std::vector<CtlRow> ctls;
    std::vector<int> carpet;          /* the background's cards, by track */
    int tab = 0, preset = 0, focus_id = -1, bstyle = 0;
    /* Controller hints drawn as 0 Xbox, 1 PlayStation modern, 2 PS2 original:
     * the Button style, or with Auto the type of the controller in use. */
    int hint_style = 0;
    bool pad_is_ps = false;           /* the last controller pressed is a PlayStation one */
    Rml::String help_title, help_text, help_default, help_env, help_extra, saved_note;
    bool help_pc = false, help_pending = false, pad_active = false, have_disc = false;
    Rml::String disc_state, disc_title, disc_note, track_name, version_text;
    /* the home page's disc panel (no good disc image) */
    Rml::String disc_kicker, disc_head, disc_text, disc_path, disc_drop1, disc_drop2;

    /* What's new */
    Rml::String notes_version, notes_title;
    std::vector<NoteSec> notes;
    OtNotes installed_notes;          /* the block built into the exe */

    /* Is there a newer version (ot_update.h)? */
    Rml::String upd_state;            /* "" = the version alone, "latest", "update" */
    bool upd_view = false;            /* the panel shows the newer version's notes */
    bool upd_have = false;            /* a list of releases was read in time */
    bool upd_started = false;         /* one check per start at most */
    std::vector<OtRelease> upd_releases;
    std::string upd_skip, upd_url, upd_tag, upd_cache;
    std::string upd_test_json, upd_test_url;
    /* the "update" dialog (install / restore) */
    Rml::String inst_kick, inst_head, inst_text, inst_phase, inst_s1, inst_s2, inst_s3, inst_l1, inst_l3, inst_foot;
    Rml::String inst_bar = "0%";
    std::string inst_test_fail_at;
    std::string started_after;        /* "--updated <v>" / "--restored <v>": a notice once shown */
    bool started_restored = false;
    double upd_t0 = 0;

    /* dialogs */
    Rml::String modal;                /* "", capture, conflict, crash, about, report_ask, notice */
    Rml::String cap_kind, cap_for, cap_now, cap_hint;
    int cap_ctl = -1, cap_col = 0;
    double cap_until = 0;
    Rml::String conf_kind, conf_line, conf_swap, conf_both;
    int conf_ctl = -1, conf_col = 0, conf_other = -1;
    std::string conf_value;
    Rml::String crash_head, crash_when, crash_version;
    std::string crash_dir;
    Rml::String notice_head, notice_text;
    std::string notice_file;
    bool focus_modal = false;

    /* pages */
    Rml::ElementDocument *home = nullptr, *settings = nullptr, *dialog = nullptr;
    std::string page = "home";
    bool focus_first_row = false;
    float stage_w = 1280;

    /* game process */
    SDL_Process *game = nullptr;
    SDL_Time launch_time = 0;
    bool skip_mode = false;           /* started by "Skip launcher": never shown unless a crash */
    bool shown = false;

    /* test script */
    std::vector<std::string> script;
    size_t script_pos = 0;
    int script_wait = 0;
    std::string shot_pending;
    bool quit = false;
};

App g;

void rebuild_rows();
void update_help(int id);
void show_modal(const char *name);
void close_modal();
void load_disc();
void pick_theme();
void load_documents();

/* ── Values ──────────────────────────────────────────────────────── */

const char *const k_resolutions[] = { "auto", "1280x720", "1280x800", "1600x900", "1920x1080",
                                      "1920x1200", "2560x1080", "2560x1440", "3440x1440", "3840x2160" };

std::string monitor_label(int i)
{
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    std::string s = std::to_string(i + 1);
    if (ids && i >= 0 && i < n) {
        const char *name = SDL_GetDisplayName(ids[i]);
        if (name && *name) s += std::string(" \xC2\xB7 ") + name;
        if (ids[i] == SDL_GetPrimaryDisplay()) s += " (main)";
    } else if (ids) {
        s += " \xC2\xB7 not connected";
    }
    SDL_free(ids);
    return s;
}

std::string value_label(int id)
{
    const SettingDef *d = settings_def(id);
    const char *v = settings_get(g.set, id);
    switch (d->type) {
    case SETTING_CHOICE: {
        int i = settings_choice_index(g.set, id);
        return i >= 0 ? d->choices[i].label : v;
    }
    case SETTING_RESOLUTION: {
        int w, h;
        if (!settings_get_resolution(g.set, id, &w, &h)) return "Auto";
        return std::to_string(w) + " \xC3\x97 " + std::to_string(h);
    }
    case SETTING_PATH:
        if (!*v) return id == S_SAVE_FOLDER ? "Automatic \xC2\xB7 Choose\xE2\x80\xA6" : "Not set \xC2\xB7 Choose\xE2\x80\xA6";
        return ot_file_name(v).empty() ? v : ot_file_name(v) + " \xE2\x80\xBA";
    case SETTING_INT:
        if (id == S_MONITOR) return monitor_label(atoi(v));
        return v;
    default: return v;
    }
}

void update_settings_changed();
void update_action(const std::string &a);
void install_start(bool restore);
bool install_running();

void settings_changed(int id)
{
    if (id == S_CHECK_UPDATES || id == S_UPDATE_CHANNEL || id < 0) update_settings_changed();
    g.preset = settings_current_preset(g.set);
    g.saved_note = "";
    rebuild_rows();
    if (id >= 0) update_help(id);
}

/* Step a setting's value left (-1) or right (+1). */
void step_value(int id, int dir)
{
    const SettingDef *d = settings_def(id);
    if (!d) return;
    char buf[32];
    switch (d->type) {
    case SETTING_BOOL:
        settings_set(g.set, id, settings_get_bool(g.set, id) ? "0" : "1");
        break;
    case SETTING_CHOICE: {
        int i = settings_choice_index(g.set, id);
        int n = d->nchoices;
        i = ((i < 0 ? 0 : i) + dir + n) % n;
        settings_set(g.set, id, d->choices[i].token);
        break;
    }
    case SETTING_INT: {
        int v = settings_get_int(g.set, id) + dir;
        if (v < d->min) v = d->min;
        if (v > d->max) v = d->max;
        snprintf(buf, sizeof buf, "%d", v);
        settings_set(g.set, id, buf);
        break;
    }
    case SETTING_RESOLUTION: {
        int n = (int)(sizeof k_resolutions / sizeof k_resolutions[0]), i = 0;
        for (int k = 0; k < n; k++)
            if (SDL_strcasecmp(settings_get(g.set, id), k_resolutions[k]) == 0) i = k;
        i = (i + dir + n) % n;
        settings_set(g.set, id, k_resolutions[i]);
        break;
    }
    default: return;
    }
    settings_changed(id);
}

bool save_settings_file()
{
    if (g.ini_write.empty() || !settings_save(g.set, g.ini_write.c_str())) {
        otlog("settings: could not write %s", g.ini_write.c_str());
        return false;
    }
    otlog("settings written");
    return true;
}

void set_disc(const std::string &path)
{
    settings_set(g.set, S_DISC_IMAGE, path.c_str());
    g.iso = path;
    load_disc();
    /* No verified disc any more: drop every picture made from the previous
     * one (cards, emblems) from the texture cache too, not only from view. */
    if (!g.have_assets) Rml::ReleaseTextures();
    if (g.have_assets) g.track = (int)(std::random_device{}() % OT_TRACK_COUNT);
    pick_theme();
    save_settings_file();             /* the launcher remembers the disc at once */
    rebuild_rows();
    g.model.DirtyAllVariables();
    load_documents();
}

/* The file / folder dialogs answer on SDL's dialog thread (SDL_dialog.h:
 * "the callback may be called from a different thread"): the choice waits
 * here and the main loop applies it (dialog_poll), never the dialog thread. */
std::mutex g_dialog_mutex;
int g_dialog_id = -1;
std::string g_dialog_path;

void dialog_poll()
{
    int id;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(g_dialog_mutex);
        if (g_dialog_id < 0) return;
        id = g_dialog_id;
        path.swap(g_dialog_path);
        g_dialog_id = -1;
    }
    if (id == S_DISC_IMAGE) {
        set_disc(path);
        return;
    }
    settings_set(g.set, id, path.c_str());
    settings_changed(id);
}

void choose_path(int id)
{
    static int s_id;
    s_id = id;
    auto done = [](void *, const char *const *list, int) {
        if (!list || !list[0]) return;
        {
            std::lock_guard<std::mutex> lock(g_dialog_mutex);
            g_dialog_id = s_id;
            g_dialog_path = list[0];
        }
        SDL_Event ev{};                       /* wakes the main loop (SDL_PushEvent is thread-safe) */
        ev.type = SDL_EVENT_USER;
        SDL_PushEvent(&ev);
    };
    if (id == S_DISC_IMAGE) {
        static const SDL_DialogFileFilter f[] = { { "Xbox disc image (.iso, .xiso)", "iso;xiso" } };
        SDL_ShowOpenFileDialog(done, nullptr, g.window, f, 1, nullptr, false);
    } else if (settings_def(id)->type == SETTING_PATH) {
        SDL_ShowOpenFolderDialog(done, nullptr, g.window, nullptr, false);
    }
}

/* ── Controls ────────────────────────────────────────────────────── */

const char *const k_ctl_icon[SETTINGS_KEY_CONTROLS] = {
    "cross", "circle", "square", "triangle", "r1", "l1", "l2", "r2", "start", "select", "stick", "stick",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "stick", "stick", "stick", "stick",
    "stick", "stick", "stick", "stick",
};

/* D-pad and left stick of the same direction share a key by default. */
bool ctl_twins(int a, int b)
{
    if (a > b) std::swap(a, b);
    return a >= 12 && a <= 15 && b == a + 4;
}

int ctl_id(int k, int col)
{
    return col == 0 ? S_KEY_FIRST + k : S_PAD_FIRST + k;
}

std::string pad_label(const char *token)
{
    if (SDL_strncasecmp(token, "Dpad", 4) == 0) {
        const SettingDef *d = settings_def(S_PAD_FIRST);
        for (int i = 0; i < d->nchoices; i++)
            if (SDL_strcasecmp(d->choices[i].token, token) == 0) return d->choices[i].label;
    }
    return token;
}

void update_hint_style()
{
    int s = g.bstyle == 1 ? 0 : g.bstyle == 2 ? 1 : g.bstyle == 3 ? 2 : g.pad_is_ps ? 1 : 0;
    if (s == g.hint_style) return;
    g.hint_style = s;
    if (g.model) g.model.DirtyVariable("hint_style");
}

void rebuild_ctls()
{
    g.ctls.clear();
    for (int k = 0; k < SETTINGS_KEY_CONTROLS; k++) {
        CtlRow c;
        c.k = k;
        c.label = settings_def(S_KEY_FIRST + k)->label;
        c.key = settings_get(g.set, S_KEY_FIRST + k);
        c.kchg = !settings_is_default(g.set, S_KEY_FIRST + k);
        c.has_pad = k < SETTINGS_PAD_CONTROLS;
        if (c.has_pad) {
            c.pad = pad_label(settings_get(g.set, S_PAD_FIRST + k));
            c.pchg = !settings_is_default(g.set, S_PAD_FIRST + k);
        }
        c.icon = std::string("icons/ps2/ps2_") + k_ctl_icon[k] + ".png";
        g.ctls.push_back(c);
    }
    g.bstyle = std::max(0, settings_choice_index(g.set, S_BUTTON_STYLE));
    if (g.model) {
        g.model.DirtyVariable("ctls");
        g.model.DirtyVariable("bstyle");
    }
    update_hint_style();
}

/* Settings key names from SDL keys (settings_key_names, letters, digits, F1-F24). */
std::string key_name(SDL_Keycode k, SDL_Scancode sc)
{
    if (k >= SDLK_A && k <= SDLK_Z) return std::string(1, (char)('A' + (k - SDLK_A)));
    if (k >= SDLK_0 && k <= SDLK_9) return std::string(1, (char)('0' + (k - SDLK_0)));
    if (k >= SDLK_F1 && k <= SDLK_F12) return "F" + std::to_string(1 + (k - SDLK_F1));
    if (k >= SDLK_F13 && k <= SDLK_F24) return "F" + std::to_string(13 + (k - SDLK_F13));
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return "Num" + std::to_string(1 + (sc - SDL_SCANCODE_KP_1));
    switch (sc) {
    case SDL_SCANCODE_KP_0: return "Num0";
    case SDL_SCANCODE_KP_MULTIPLY: return "Num*";
    case SDL_SCANCODE_KP_PLUS: return "Num+";
    case SDL_SCANCODE_KP_MINUS: return "Num-";
    case SDL_SCANCODE_KP_PERIOD: return "Num.";
    case SDL_SCANCODE_KP_DIVIDE: return "Num/";
    case SDL_SCANCODE_KP_ENTER: return "Enter";
    default: break;
    }
    switch (k) {
    case SDLK_SPACE: return "Space";
    case SDLK_RETURN: return "Enter";
    case SDLK_TAB: return "Tab";
    case SDLK_CAPSLOCK: return "CapsLock";
    case SDLK_UP: return "Up";
    case SDLK_DOWN: return "Down";
    case SDLK_LEFT: return "Left";
    case SDLK_RIGHT: return "Right";
    case SDLK_LSHIFT: return "LeftShift";
    case SDLK_RSHIFT: return "RightShift";
    case SDLK_LCTRL: return "LeftCtrl";
    case SDLK_RCTRL: return "RightCtrl";
    case SDLK_INSERT: return "Insert";
    case SDLK_DELETE: return "Delete";
    case SDLK_HOME: return "Home";
    case SDLK_END: return "End";
    case SDLK_PAGEUP: return "PageUp";
    case SDLK_PAGEDOWN: return "PageDown";
    case SDLK_SEMICOLON: return ";";
    case SDLK_EQUALS: return "=";
    case SDLK_COMMA: return ",";
    case SDLK_MINUS: return "-";
    case SDLK_PERIOD: return ".";
    case SDLK_SLASH: return "/";
    case SDLK_GRAVE: return "`";
    case SDLK_LEFTBRACKET: return "[";
    case SDLK_BACKSLASH: return "\\";
    case SDLK_RIGHTBRACKET: return "]";
    case SDLK_APOSTROPHE: return "'";
    default: return "";
    }
}

const char *pad_token(int button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "A";
    case SDL_GAMEPAD_BUTTON_EAST: return "B";
    case SDL_GAMEPAD_BUTTON_WEST: return "X";
    case SDL_GAMEPAD_BUTTON_NORTH: return "Y";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_BACK: return "Back";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LS";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RS";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "DpadUp";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "DpadDown";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "DpadLeft";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "DpadRight";
    default: return nullptr;
    }
}

void ctl_changed()
{
    rebuild_ctls();
    settings_changed(g.focus_id);
}

void capture_start(int k, int col)
{
    if (k < 0 || k >= SETTINGS_KEY_CONTROLS || (col == 1 && k >= SETTINGS_PAD_CONTROLS)) return;
    g.cap_ctl = k;
    g.cap_col = col;
    g.cap_kind = col == 0 ? "CONTROLS \xC2\xB7 KEYBOARD" : "CONTROLS \xC2\xB7 CONTROLLER";
    g.cap_for = settings_def(S_KEY_FIRST + k)->label;
    g.cap_now = col == 0 ? settings_get(g.set, ctl_id(k, 0)) : pad_label(settings_get(g.set, ctl_id(k, 1)));
    g.cap_hint = col == 0 ? "Press the new key" : "Press the new button or trigger";
    g.cap_until = col == 1 ? now_ms() + 8000 : 0;   /* every button is a binding: a timeout cancels */
    show_modal("capture");
}

/* The new binding, unless another control uses it (then the conflict dialog). */
void capture_assign(const std::string &value)
{
    int k = g.cap_ctl, col = g.cap_col;
    close_modal();
    if (k < 0) return;
    int id = ctl_id(k, col);
    if (SDL_strcasecmp(value.c_str(), "None") != 0) {
        int n = col == 0 ? SETTINGS_KEY_CONTROLS : SETTINGS_PAD_CONTROLS;
        for (int j = 0; j < n; j++) {
            if (j == k || (col == 0 && ctl_twins(j, k))) continue;
            if (SDL_strcasecmp(settings_get(g.set, ctl_id(j, col)), value.c_str()) != 0) continue;
            std::string shown = col == 0 ? value : pad_label(value.c_str());
            std::string old = col == 0 ? settings_get(g.set, id) : pad_label(settings_get(g.set, id));
            const char *me = settings_def(S_KEY_FIRST + k)->label, *other = settings_def(S_KEY_FIRST + j)->label;
            g.conf_ctl = k;
            g.conf_col = col;
            g.conf_other = j;
            g.conf_value = value;
            g.conf_kind = col == 0 ? "CONTROLS \xC2\xB7 KEYBOARD" : "CONTROLS \xC2\xB7 CONTROLLER";
            g.conf_line = shown + " is already set for " + other + ".";
            g.conf_swap = std::string("Swap them? ") + me + " gets " + shown + " and " + other + " gets " + old + ".";
            g.conf_both = "Use " + shown + " for both";
            show_modal("conflict");
            return;
        }
    }
    settings_set(g.set, id, value.c_str());
    ctl_changed();
}

void conflict_answer(int how)   /* 0 swap, 1 both, 2 cancel */
{
    close_modal();
    if (how == 2 || g.conf_ctl < 0) return;
    int me = ctl_id(g.conf_ctl, g.conf_col), other = ctl_id(g.conf_other, g.conf_col);
    std::string old = settings_get(g.set, me);
    settings_set(g.set, me, g.conf_value.c_str());
    if (how == 0) settings_set(g.set, other, old.c_str());
    ctl_changed();
}

/* ── Rows and help ───────────────────────────────────────────────── */

void add_action_row(int id, const char *label, const char *value, const char *button)
{
    Row r;
    r.id = id;
    r.label = label;
    r.value = value ? value : "";
    r.button = button;
    g.rows.push_back(r);
}

void rebuild_rows()
{
    g.rows.clear();
    const char *group = nullptr;
    for (int id = 0; id < SETTINGS_COUNT; id++) {
        const SettingDef *d = settings_def(id);
        if (d->tab != g.tab || (d->flags & SETTING_HIDDEN)) continue;
        /* Nothing that does nothing yet is shown (they stay in the registry): the launcher's music and sounds (not played yet) and the settings
         * the game does not read yet (SETTING_PENDING: Monitor, pause / mute in the background). */
        if (id == S_LAUNCHER_MUSIC || id == S_LAUNCHER_SOUNDS ||
            id == S_THEME ||          /* random at each start, no choice */
            (d->flags & SETTING_PENDING)) continue;
        /* CONTROLS: the bindings are the remapping table, not rows. */
        if (id >= S_KEY_FIRST && id < S_PAD_FIRST + SETTINGS_PAD_CONTROLS) continue;
        if (d->group && (!group || strcmp(group, d->group) != 0)) {
            Row h;
            h.is_group = true;
            h.label = d->group;
            g.rows.push_back(h);
            group = d->group;
        }
        Row r;
        r.id = id;
        r.label = d->label;
        r.pc = (d->flags & SETTING_PC) != 0;
        r.pending = (d->flags & SETTING_PENDING) != 0;
        r.changed = !settings_is_default(g.set, id);
        if (d->type == SETTING_BOOL) {
            r.toggle = true;
            r.on = settings_get_bool(g.set, id) != 0;
        } else if (d->type == SETTING_PATH || d->type == SETTING_KEY) {
            r.plain = true;
            r.value = value_label(id);
        } else {
            r.arrows = true;
            r.value = value_label(id);
        }
        g.rows.push_back(r);
        if (id == S_UPDATE_CHANNEL) {
            std::string v;
            if (ot_install_has_backup(g.base, v)) add_action_row(A_RESTORE, "Previous version", ("v" + v).c_str(), "Restore");
        }
        if (id == S_LOG_FILE) {
            add_action_row(A_OPEN_LOGS, "Logs", nullptr, "Open logs");
            add_action_row(A_BUG_REPORT, "Bug report", nullptr, "Prepare a bug report\xE2\x80\xA6");
            Row h;
            h.is_group = true;
            h.label = "ABOUT";
            g.rows.push_back(h);
            add_action_row(A_ABOUT, "About OpenTricky", ("v" + std::string(OT_VERSION)).c_str(), "Credits & licences");
        }
    }
    if (g.model) {
        g.model.DirtyVariable("rows");
        g.model.DirtyVariable("preset");
        g.model.DirtyVariable("saved_note");
    }
}

void set_help(const char *title, const std::string &text, const std::string &def, const std::string &extra)
{
    g.help_title = title;
    g.help_text = text;
    g.help_default = def;
    g.help_extra = extra;
    g.help_pc = false;
    g.help_pending = false;
    g.help_env = "";
}

void dirty_help()
{
    if (!g.model) return;
    for (const char *v : { "help_title", "help_text", "help_default", "help_pc", "help_env", "help_extra", "help_pending" })
        g.model.DirtyVariable(v);
}

void update_help(int id)
{
    g.focus_id = id;
    if (id >= 2000 && id < 2000 + 2 * SETTINGS_KEY_CONTROLS) {     /* a cell of the remapping table */
        int k = (id - 2000) / 2, col = (id - 2000) % 2;
        const SettingDef *d = settings_def(ctl_id(k, col));
        std::string now = col == 0 ? settings_get(g.set, ctl_id(k, 0)) : pad_label(settings_get(g.set, ctl_id(k, 1)));
        std::string def = col == 0 ? d->def : pad_label(d->def);
        set_help(settings_def(S_KEY_FIRST + k)->label,
                 std::string(col == 0 ? "Keyboard: " : "Controller: ") + now +
                     ". Select it (Enter or A), then press the new " + (col == 0 ? "key" : "button") +
                     ". Esc cancels; Backspace clears.",
                 def, k >= 12 && k <= 19 && col == 0 ? "The D-pad and the left stick may share a key." : "");
        dirty_help();
        return;
    }
    switch (id) {
    case A_OPEN_LOGS:
        set_help("Logs", "Opens the folder of SSX Tricky.log, the game's log file (when Log file is on), and the launcher's settings.ini.", "", "");
        dirty_help();
        return;
    case A_BUG_REPORT:
        set_help("Bug report", "Makes a zip on your Desktop with the logs, your settings and the last crash report, never your disc image or your saves. Nothing is sent automatically: attach it to a GitHub issue if you want.", "", "");
        dirty_help();
        return;
    case A_RESTORE:
        set_help("Restore previous version", "Puts back the version that was installed before the last update (kept in the old folder). Your settings and saves stay.", "", "");
        dirty_help();
        return;
    case A_ABOUT:
        set_help("About OpenTricky", "Credits, version and the licences of the libraries and fonts the launcher uses.", "", "");
        dirty_help();
        return;
    default: break;
    }
    if (id < 0 || id >= SETTINGS_COUNT) return;
    const SettingDef *d = settings_def(id);
    g.help_title = d->label;
    g.help_text = d->help ? d->help : "";
    std::string def;
    if (d->type == SETTING_CHOICE) {
        for (int i = 0; i < d->nchoices; i++)
            if (SDL_strcasecmp(d->choices[i].token, d->def) == 0) def = d->choices[i].label;
    } else if (d->type == SETTING_BOOL) {
        def = strcmp(d->def, "1") == 0 ? "On" : "Off";
    } else if (d->type == SETTING_RESOLUTION) {
        def = SDL_strcasecmp(d->def, "auto") == 0 ? "Auto" : d->def;
    } else if (id == S_MONITOR) {
        def = "1 (the main screen)";
    } else {
        def = *d->def ? d->def : (id == S_SAVE_FOLDER ? "automatic" : "empty");
    }
    g.help_default = def;
    g.help_pc = (d->flags & SETTING_PC) != 0;
    g.help_pending = (d->flags & SETTING_PENDING) != 0;
    g.help_env = d->env ? d->env : "";
    g.help_extra = "";
    if (id == S_HD_TEXTURES || id == S_HD_PACK_FOLDER) g.help_extra = "HD textures by Bl4ckH4nd (not included).";
    if (id == S_CHECK_UPDATES || id == S_UPDATE_CHANNEL)
        g.help_extra = "The launcher contacts GitHub (api.github.com) at most once a day for the list of OpenTricky releases. "
                       "Nothing about you or your game is sent, and nothing is installed without your click.";
    if (id == S_SKIP_LAUNCHER) g.help_extra = "To see the launcher again, hold Shift (or Select on a controller) while it starts.";
    if (id == S_DISC_IMAGE || id == S_HD_PACK_FOLDER || id == S_SAVE_FOLDER) {
        const char *v = settings_get(g.set, id);
        if (*v) g.help_extra = (g.help_extra.empty() ? "" : g.help_extra + " ") + "Now: " + v;
    }
    dirty_help();
}

void set_tab(int t)
{
    t = (t + SETTINGS_TAB_COUNT) % SETTINGS_TAB_COUNT;
    g.tab = t;
    rebuild_rows();
    g.model.DirtyVariable("tab");
    g.focus_first_row = true;
}

/* ── Theme, disc, documents ──────────────────────────────────────── */

void apply_theme_tokens()
{
    auto &t = g.theme;
    g.files.tokens["$acc$"] = hex_rgb(t.acc);
    g.files.tokens["$acc_dark$"] = hex_rgb(t.acc_dark);
    g.files.tokens["$acc_light$"] = hex_rgb(t.acc_light);
    g.files.tokens["$acc_soft$"] = rgba(t.acc, 0.16f);
    g.files.tokens["$acc_line$"] = rgba(t.acc, 0.55f);
    g.files.tokens["$glow$"] = rgba(t.acc, 0.40f);
    g.files.tokens["$play_text$"] = hex_rgb(t.play_text);
    g.files.tokens["$track$"] = std::to_string(g.track);
    g.files.tokens["$display$"] = g.bullet ? "Bullet SmallCaps" : "Exo 2";
    g.files.tokens["$display_style$"] = g.bullet ? "normal" : "italic";
    g.files.tokens["$display_weight$"] = g.bullet ? "normal" : "900";
}

void pick_theme()
{
    if (g.have_assets)
        ot_theme_for_track(&g.assets, g.track, true, &g.theme);
    else
        ot_theme_default(&g.theme);
    g.track_name = g.have_assets ? ot_track_name(g.track) : "";
    apply_theme_tokens();
}

bool make_texture(const std::string &name, OtPixels &out)
{
    /* Names carry the track ("emblem/7") so a new track never reuses a picture. */
    std::string kind = name.substr(0, name.find('/'));
    int track = name.find('/') != std::string::npos ? atoi(name.c_str() + name.find('/') + 1) : g.track;
    OtImage im{};
    bool ok = false;
    if (kind == "icon") return ot_draw_icon(name.substr(5), 48, out);
    if (kind == "bg") {
        OtTheme t;
        if (g.have_assets) ot_theme_for_track(&g.assets, track, true, &t);
        else ot_theme_default(&t);
        ok = ot_theme_background(&t, 640, 400, &im);
    } else if (kind == "otbadge") {
        ok = ot_badge_ot(96, &im);
    } else if (g.have_assets && track >= 0 && track < OT_TRACK_COUNT) {
        /* The shipped HD card when it was read (verified disc only, see
         * load_hd_cards), else the disc's own card, smoothed and scaled up. */
        const OtImage *hd = g.hd_card[track].px ? &g.hd_card[track] : nullptr;
        if (kind == "emblem") ok = hd ? ot_card_emblem(hd, &im) : ot_image_copy(&g.assets.emblem[track], &im);
        else if (kind == "badge") ok = hd ? ot_badge_make(hd, 0, OT_CARD_HD_W, &im) : ot_badge_from_disc(&g.assets, track, 1040, &im);
        else if (kind == "carpet") ok = hd ? ot_badge_make(hd, 0, 400, &im) : ot_badge_from_disc(&g.assets, track, 400, &im);
    }
    if (!ok) return false;
    image_to_pixels(im, out);
    ot_image_free(&im);
    return true;
}

void free_hd_cards()
{
    for (OtImage &im : g.hd_card) ot_image_free(&im);
}

/* The ten HD track cards shipped in ui/cards/: read only once the player's
 * disc image is verified and loaded; without it nothing of them is in memory
 * or on screen. A missing or odd file leaves that track on the disc's card. */
void load_hd_cards()
{
    free_hd_cards();
    if (!g.have_assets || g.disc.status != OT_DISC_OK) return;
    double t0 = now_ms();
    /* one thread per file: decoding ten PNGs one after the other would cost
     * ~100 ms before the first frame */
    auto read_card = [](int t) {
        std::string path = g.base + "ui/cards/" + ot_track_id(t) + ".png";
        SDL_Surface *s = SDL_LoadPNG(path.c_str());
        if (!s) return;
        SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);   /* = OtImage's 0xAARRGGBB */
        SDL_DestroySurface(s);
        if (!c) return;
        if (c->w == OT_CARD_HD_W && c->h == OT_CARD_HD_H && ot_image_alloc(&g.hd_card[t], c->w, c->h))
            for (int y = 0; y < c->h; y++)
                memcpy(&g.hd_card[t].px[(size_t)y * c->w], (unsigned char *)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
        SDL_DestroySurface(c);
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < OT_TRACK_COUNT; t++) pool.emplace_back(read_card, t);
    for (std::thread &th : pool) th.join();
    int n = 0;
    for (const OtImage &im : g.hd_card) n += im.px != nullptr;
    otlog("hd cards: %d of %d (%.1f ms)", n, OT_TRACK_COUNT, now_ms() - t0);
}

void load_disc()
{
    free_hd_cards();
    if (g.have_assets) {
        ot_disc_free(&g.assets);
        g.have_assets = false;
    }
    g.disc = g.iso.empty() ? OtDiscCheck{ OT_DISC_NO_PATH } : ot_disc_check(g.iso.c_str());
    if (g.disc.status == OT_DISC_OK) {
        double t0 = now_ms();
        g.have_assets = ot_disc_load(g.iso.c_str(), &g.assets);
        otlog("timing disc_load_ms=%.1f ok=%d", now_ms() - t0, (int)g.have_assets);
        load_hd_cards();
    }
    g.have_disc = g.have_assets;
    g.disc_path = g.iso;
    switch (g.disc.status) {
    case OT_DISC_OK:
        g.disc_state = "ok"; g.disc_title = "SSX Tricky (USA)"; g.disc_note = "disc verified";
        break;
    case OT_DISC_NO_PATH:
        g.disc_state = "warn"; g.disc_title = "No disc image yet"; g.disc_note = "Choose\xE2\x80\xA6";
        g.disc_kicker = "WELCOME";
        g.disc_head = "POINT TO YOUR DISC IMAGE";
        g.disc_text = "OpenTricky plays the Xbox version of SSX Tricky from your own disc image. The game itself is not included.";
        g.disc_drop1 = "Drop your SSX Tricky (USA) Xbox disc image here";
        g.disc_drop2 = "or click to browse \xC2\xB7 checked right away";
        break;
    case OT_DISC_NOT_FOUND:
        g.disc_state = "bad"; g.disc_title = "Disc image not found"; g.disc_note = "Locate\xE2\x80\xA6";
        g.disc_kicker = "DISC IMAGE NOT FOUND";
        g.disc_head = "WE CAN'T FIND YOUR DISC IMAGE";
        g.disc_text = "It was moved, renamed, or the drive is not connected.";
        g.disc_drop1 = "Locate it again";
        g.disc_drop2 = "or drop the disc image here";
        break;
    default:
        g.disc_state = "bad"; g.disc_title = "Not SSX Tricky (USA) for Xbox"; g.disc_note = "Choose another\xE2\x80\xA6";
        g.disc_kicker = "WRONG DISC IMAGE";
        g.disc_head = "THIS ISN'T SSX TRICKY (USA) FOR XBOX";
        if (!g.disc.xbox)
            g.disc_text = "This file is not an Xbox disc image (a PlayStation 2 or GameCube image, or another kind of file). OpenTricky needs the Xbox version, USA release.";
        else if (g.disc.title[0])
            g.disc_text = std::string("This Xbox disc image is \xE2\x80\x9C") + g.disc.title + "\xE2\x80\x9D. OpenTricky needs SSX Tricky, USA release.";
        else
            g.disc_text = "This Xbox disc image is another game or another region. OpenTricky needs SSX Tricky, USA release.";
        g.disc_drop1 = "Choose another file";
        g.disc_drop2 = "or drop the right disc image here";
        break;
    }
    otlog("disc: state=%s xbox=%d title_id=%08X", g.disc_state.c_str(), (int)g.disc.xbox, g.disc.title_id);
}

void show_page(const std::string &p);

/* The stage is 1280 x 800 dp at 16:10; wider windows (16:9, 21:9) widen it up
 * to 1600 dp: the left column keeps its place and the right panels follow
 * the right edge, so nothing floats in a sea of background. */
void apply_layout()
{
    if (!g.ctx) return;
    float dp = g.ctx->GetDensityIndependentPixelRatio();
    float vw = g.ctx->GetDimensions().x / (dp > 0 ? dp : 1);
    float w = std::max(1280.0f, std::min(1600.0f, vw));
    g.stage_w = w;
    float left = (vw - w) / 2;
    char wbuf[32], mbuf[32], fbuf[32];
    snprintf(wbuf, sizeof wbuf, "%.1fdp", w);
    snprintf(mbuf, sizeof mbuf, "%.1fdp", -w / 2);
    snprintf(fbuf, sizeof fbuf, "%.1fdp", left - 76);
    for (Rml::ElementDocument *d : { g.home, g.settings, g.dialog }) {
        if (!d) continue;
        if (Rml::Element *s = d->GetElementById("stage")) {
            s->SetProperty("width", wbuf);
            s->SetProperty("margin-left", mbuf);
        }
        if (Rml::Element *f = d->GetElementById("fin")) {
            f->SetProperty("width", wbuf);
            f->SetProperty("left", fbuf);
        }
    }
}

void load_documents()
{
    std::string keep = g.page;
    if (g.home) g.home->Close();
    if (g.settings) g.settings->Close();
    if (g.dialog) g.dialog->Close();
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    g.home = g.ctx->LoadDocument(g.base + "ui/home.rml");
    g.settings = g.ctx->LoadDocument(g.base + "ui/settings.rml");
    g.dialog = g.ctx->LoadDocument(g.base + "ui/dialog.rml");
    if (!g.home || !g.settings || !g.dialog) otlog("error: cannot load the interface documents");
    /* The help panel follows the focused (or hovered) setting. */
    struct Follow : Rml::EventListener {
        void ProcessEvent(Rml::Event &ev) override
        {
            for (Rml::Element *e = ev.GetTargetElement(); e; e = e->GetParentNode()) {
                int id = e->GetAttribute<int>("rid", -1);
                if (id >= 0) {
                    update_help(id);
                    return;
                }
            }
        }
    };
    static Follow follow;
    if (g.settings) {
        g.settings->AddEventListener(Rml::EventId::Focus, &follow, true);
        g.settings->AddEventListener(Rml::EventId::Mouseover, &follow, true);
    }
    apply_layout();
    show_page(keep);
    if (!g.modal.empty()) show_modal(g.modal.c_str());
}

Rml::ElementDocument *page_doc()
{
    return g.page == "settings" ? g.settings : g.home;
}

void show_page(const std::string &p)
{
    g.page = p;
    Rml::ElementDocument *on = p == "settings" ? g.settings : g.home;
    Rml::ElementDocument *off = p == "settings" ? g.home : g.settings;
    if (off) off->Hide();
    if (!on) return;
    on->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
    if (p == "settings") {
        g.focus_first_row = true;
    } else if (Rml::Element *e = on->GetElementById(g.have_disc ? "play" : "drop")) {
        e->Focus(true);
    }
}

void show_modal(const char *name)
{
    g.modal = name;
    g.model.DirtyAllVariables();       /* the dialog's texts were set just before */
    if (!g.dialog) return;
    g.dialog->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
    g.focus_modal = true;            /* the first button, once the section exists */
}

void close_modal()
{
    g.modal = "";
    g.model.DirtyVariable("modal");
    if (g.dialog) g.dialog->Hide();
    if (Rml::ElementDocument *d = page_doc()) {
        d->Focus();
        if (g.page == "settings") {
            /* back on the remapping cell or row that opened the dialog */
            Rml::ElementList list;
            d->QuerySelectorAll(list, "[rid]");
            for (Rml::Element *e : list)
                if (e->GetAttribute<int>("rid", -1) == g.focus_id) { e->Focus(true); break; }
        } else if (Rml::Element *e = d->GetElementById(g.have_disc ? "play" : "drop")) {
            e->Focus(true);
        }
    }
}

/* ── Game ────────────────────────────────────────────────────────── */

void set_crash(const OtCrashReport &r, int code)
{
    g.crash_dir = r.found ? r.dir : "";
    bool freeze = r.kind == "freeze";
    g.crash_head = freeze ? "THE GAME FROZE" : "THE GAME CLOSED UNEXPECTEDLY";
    SDL_Time t = r.found ? r.time : 0;
    if (!t) SDL_GetCurrentTime(&t);
    char code_txt[48] = "";
    if (code) snprintf(code_txt, sizeof code_txt, " \xC2\xB7 error 0x%08X", (unsigned)code);
    g.crash_when = "When: " + ot_when(t) + (freeze ? " \xC2\xB7 freeze" : " \xC2\xB7 crash") + code_txt;
    g.crash_version = "Version: v" + std::string(OT_VERSION) + (g.gl_renderer.empty() ? "" : " \xC2\xB7 " + g.gl_renderer) +
                      (r.found ? "" : " \xC2\xB7 no crash report was written");
    for (const char *v : { "crash_head", "crash_when", "crash_version" }) g.model.DirtyVariable(v);
}

bool play()
{
    if (!g.have_disc || g.game) return false;
    std::string exe = g.base + k_game_exe;
    if (!SDL_GetPathInfo(exe.c_str(), nullptr)) {
        otlog("play: %s not found beside the launcher", k_game_exe);
        g.notice_head = "GAME NOT FOUND";
        g.notice_text = std::string("\xE2\x80\x9C") + k_game_exe + "\xE2\x80\x9D should be beside OpenTricky.exe. Unzip the whole release into one folder.";
        g.notice_file = "";
        g.model.DirtyVariable("notice_head");
        g.model.DirtyVariable("notice_text");
        show_modal("notice");
        return false;
    }
    save_settings_file();                       /* the game reads settings.ini at start */
    SDL_Environment *env = SDL_CreateEnvironment(true);
    SDL_SetEnvironmentVariable(env, "OPENTRICKY_LAUNCHER", "1", true);   /* the crash screen is the launcher's */
    const char *args[] = { exe.c_str(), "--play", nullptr };
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetStringProperty(p, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, g.base.c_str());
    SDL_GetCurrentTime(&g.launch_time);
    g.game = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    SDL_DestroyEnvironment(env);
    if (!g.game) {
        otlog("play: cannot start the game: %s", SDL_GetError());
        return false;
    }
    otlog("game started (pid %lld)", (long long)SDL_GetNumberProperty(SDL_GetProcessProperties(g.game), SDL_PROP_PROCESS_PID_NUMBER, 0));
    SDL_HideWindow(g.window);                   /* hidden while the game runs */
    return true;
}

void game_ended(int code)
{
    otlog("game ended (exit code %d, 0x%08X)", code, (unsigned)code);
    /* A report folder written since the launch, or an exception code. */
    OtCrashReport r = ot_find_crash_report(g.base, g.launch_time - 2 * SDL_NS_PER_SECOND);
    bool exception = (unsigned)code >= 0xC0000000u;
    if (r.found || exception) {
        otlog("crash screen: %s", r.found ? r.name.c_str() : "(no report folder)");
        set_crash(r, exception ? code : 0);
        if (!g.shown) {
            g.shown = true;
            g.skip_mode = false;
        }
        SDL_ShowWindow(g.window);
        SDL_RaiseWindow(g.window);
        show_modal("crash");
        return;
    }
    if (g.skip_mode) {                          /* never shown: the launcher ends with the game */
        g.quit = true;
        return;
    }
    SDL_ShowWindow(g.window);
    SDL_RaiseWindow(g.window);
}

/* ── Bug report ──────────────────────────────────────────────────── */

std::string g_report_dir;              /* --report-dir (tests): instead of the Desktop */

std::string desktop_dir()
{
    if (!g_report_dir.empty()) return g_report_dir;
    const char *d = SDL_GetUserFolder(SDL_FOLDER_DESKTOP);
    if (d && *d) return d;
    return g.base;
}

void open_folder(const std::string &dir)
{
    std::string url = "file:///" + dir;
    for (char &c : url) if (c == '\\') c = '/';
    if (!SDL_OpenURL(url.c_str())) otlog("cannot open %s: %s", dir.c_str(), SDL_GetError());
}

std::string ini_dir()
{
    std::string p = !g.ini_write.empty() ? g.ini_write : g.ini_read;
    return p.empty() ? g.base : ot_dir_of(p);
}

void prepare_bug_report()
{
    std::vector<OtZipEntry> z;
    /* the folders of the disc image, the settings and the game, by name only */
    std::vector<std::pair<std::string, std::string>> fold = {
        { ot_dir_of(g.iso), "<disc folder>" }, { ini_dir(), "<settings folder>" }, { g.base, "<game folder>" } };
    std::sort(fold.begin(), fold.end(), [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    SDL_DateTime d{};
    SDL_TimeToDateTime(now, &d, true);
    char stamp[64];
    snprintf(stamp, sizeof stamp, "%04d-%02d-%02d_%02d-%02d", d.year, d.month, d.day, d.hour, d.minute);

    /* The settings, with the paths reduced to file names. */
    Settings *s = (Settings *)calloc(1, sizeof(Settings));
    memcpy(s, g.set, sizeof(Settings));
    for (int id = 0; id < SETTINGS_COUNT; id++)
        if (settings_def(id)->type == SETTING_PATH && *settings_get(s, id))
            settings_set(s, id, ot_file_name(settings_get(s, id)).c_str());
    std::string ini(settings_format(s, nullptr, 0) + 1, '\0');
    settings_format(s, &ini[0], ini.size());
    ini.resize(strlen(ini.c_str()));
    free(s);
    z.push_back({ "settings.ini", ot_scrub(ini, fold) });

    std::string info = "OpenTricky bug report, " + std::string(stamp) + "\n";
    info += "Launcher: v" + std::string(OT_VERSION) + "\n";
    info += "OpenGL: " + g.gl_renderer + "\n";
    info += "Platform: " + std::string(SDL_GetPlatform()) + ", " + std::to_string(SDL_GetNumLogicalCPUCores()) +
            " logical CPUs, " + std::to_string(SDL_GetSystemRAM()) + " MB RAM\n";
    char tid[64];
    snprintf(tid, sizeof tid, "%08X", g.disc.title_id);
    info += "Disc image: " + std::string(g.disc_state) + " (title id " + tid + ")\n";
    info += "Settings file: " + std::string(g.ini_in_docs ? "Documents\\My Games\\SSX Tricky" : "beside the launcher") + "\n\n";
    info += "Launcher messages (last lines):\n";
    for (const std::string &l : g_log_ring) info += l + "\n";
    z.push_back({ "launcher.txt", ot_scrub(info, fold) });

    std::string log;
    if (ot_read_tail(ini_dir() + "SSX Tricky.log", log, 4u << 20)) z.push_back({ "SSX Tricky.log", ot_scrub(log, fold) });

    /* The newest crash report folder (already free of folders and user name). */
    OtCrashReport r = g.crash_dir.empty() ? ot_find_crash_report(g.base, 0) : OtCrashReport{};
    std::string dir = !g.crash_dir.empty() ? g.crash_dir : r.found ? r.dir : "";
    if (!dir.empty()) {
        std::string name = ot_file_name(dir.substr(0, dir.size() - 1));
        for (const char *f : { "report.txt", "log.txt", "settings.txt", "crash.dmp", "hang.dmp" }) {
            std::string data;
            if (ot_read_file(dir + f, data, 32u << 20)) z.push_back({ "crash-report/" + name + "/" + f, data });
        }
    }
    z.push_back({ "README.txt",
        "This zip was made by OpenTricky's launcher for a bug report.\n"
        "It holds the launcher's and the game's logs, the settings (paths reduced to\n"
        "file names) and the last crash report, if any. It never holds the disc\n"
        "image or the saves. Nothing was sent: attach it to an issue on\n"
        "https://github.com/GiZcesi/OpenTricky/issues if you want.\n" });

    std::string file = desktop_dir() + "OpenTricky-bug-report-" + stamp + ".zip";
    bool ok = ot_zip_write(file, z);
    otlog("bug report: %s (%s, %d files)", ot_file_name(file).c_str(), ok ? "written" : "NOT written", (int)z.size());
    g.notice_head = ok ? "BUG REPORT READY" : "COULD NOT WRITE THE BUG REPORT";
    g.notice_text = ok ? "Saved on your Desktop: " + ot_file_name(file) +
                             ". Nothing was sent: attach it to a GitHub issue if you want."
                       : "The zip could not be written on the Desktop (" + std::string(SDL_GetError()) + ").";
    g.notice_file = ok ? file : "";
    g.model.DirtyVariable("notice_head");
    g.model.DirtyVariable("notice_text");
    g.model.DirtyVariable("notice_file");
    show_modal("notice");
}

/* ── Actions ─────────────────────────────────────────────────────── */

void save_settings()
{
    if (!save_settings_file()) {
        g.saved_note = "Could not save";
    } else {
        g.saved_note = g.ini_in_docs ? "Saved in Documents\\My Games\\SSX Tricky" : "Saved";
        for (Row &r : g.rows)
            if (!r.is_group && r.id < SETTINGS_COUNT) r.changed = !settings_is_default(g.set, r.id);
        g.model.DirtyVariable("rows");
    }
    g.model.DirtyVariable("saved_note");
}

void row_action(int id)
{
    switch (id) {
    case A_OPEN_LOGS: open_folder(ini_dir()); break;
    case A_BUG_REPORT: show_modal("report_ask"); break;
    case A_ABOUT: show_modal("about"); break;
    case A_RESTORE: install_start(true); break;
    default: break;
    }
}

void dialog_action(const std::string &a)
{
    if (a == "cap_cancel") { close_modal(); return; }
    if (a == "cap_clear") { capture_assign("None"); return; }
    if (a == "swap") { conflict_answer(0); return; }
    if (a == "both") { conflict_answer(1); return; }
    if (a == "conf_cancel") { conflict_answer(2); return; }
    if (a == "report") { close_modal(); prepare_bug_report(); return; }
    if (a == "inst_retry") { bool r = g.inst_kick == "RESTORE"; close_modal(); install_start(r); return; }
    if (a == "inst_page") { if (!g.upd_url.empty()) SDL_OpenURL(g.upd_url.c_str()); close_modal(); return; }
    if (a == "logs") { open_folder(ini_dir()); return; }
    if (a == "licences") {
        std::string f = g.base + "THIRD-PARTY-LICENSES.txt";
        std::string url = "file:///" + f;
        for (char &c : url) if (c == '\\') c = '/';
        if (!SDL_OpenURL(url.c_str())) otlog("cannot open the licences: %s", SDL_GetError());
        return;
    }
    if (a == "github") { SDL_OpenURL(k_url_github); return; }
    if (a == "show_file") { if (!g.notice_file.empty()) open_folder(ot_dir_of(g.notice_file)); close_modal(); return; }
    close_modal();
}

/* The dialog's "Esc / B". */
void dialog_back()
{
    if (g.modal == "update" && install_running()) return;     /* no way out half-way */
    if (g.modal == "capture") close_modal();
    else if (g.modal == "conflict") conflict_answer(2);
    else close_modal();
}

Rml::Element *focused()
{
    return g.ctx->GetFocusElement();
}

int focused_row()
{
    for (Rml::Element *e = focused(); e; e = e->GetParentNode()) {
        int id = e->GetAttribute<int>("rid", -1);
        if (id >= 0) return id;
    }
    return -1;
}

/* One navigation input, from the keyboard, a controller or a test step. */
enum Nav { N_UP, N_DOWN, N_LEFT, N_RIGHT, N_OK, N_BACK, N_PREV, N_NEXT };

void nav(Nav n)
{
    static const Rml::Input::KeyIdentifier keys[] = { Rml::Input::KI_UP, Rml::Input::KI_DOWN,
        Rml::Input::KI_LEFT, Rml::Input::KI_RIGHT, Rml::Input::KI_RETURN };
    if (!g.modal.empty()) {
        if (n == N_BACK) { dialog_back(); return; }
        if (n == N_PREV || n == N_NEXT) return;
    } else if (g.page == "settings") {
        if (n == N_BACK) { show_page("home"); return; }
        if (n == N_PREV || n == N_NEXT) { set_tab(g.tab + (n == N_NEXT ? 1 : -1)); return; }
        int id = focused_row();
        if (id >= 0 && id < SETTINGS_COUNT && (n == N_LEFT || n == N_RIGHT)) { step_value(id, n == N_RIGHT ? 1 : -1); return; }
    } else {
        if (n == N_PREV || n == N_NEXT) return;   /* the theme is random, no track to choose */
        if (n == N_BACK) return;
    }
    if (n <= N_OK) {
        g.ctx->ProcessKeyDown(keys[n], 0);
        g.ctx->ProcessKeyUp(keys[n], 0);
    }
}

void set_pad_active(bool on)
{
    if (g.pad_active == on) return;
    g.pad_active = on;
    g.model.DirtyVariable("pad_active");
}

bool pad_button(int b)
{
    set_pad_active(true);
    if (g.modal == "capture" && g.cap_col == 1) {
        if (const char *t = pad_token(b)) capture_assign(t);
        return true;
    }
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: nav(N_UP); return true;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: nav(N_DOWN); return true;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: nav(N_LEFT); return true;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: nav(N_RIGHT); return true;
    case SDL_GAMEPAD_BUTTON_SOUTH: nav(N_OK); return true;
    case SDL_GAMEPAD_BUTTON_EAST: nav(N_BACK); return true;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: nav(N_PREV); return true;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: nav(N_NEXT); return true;
    case SDL_GAMEPAD_BUTTON_START: if (g.page == "home" && g.modal.empty()) play(); return true;
    case SDL_GAMEPAD_BUTTON_NORTH: if (g.page == "settings" && g.modal.empty()) save_settings(); return true;
    default: return false;
    }
}

/* Keys the launcher handles itself; others go to RmlUi. */
bool key_down(SDL_Keycode k, SDL_Scancode sc)
{
    set_pad_active(false);
    if (g.modal == "capture") {
        if (k == SDLK_ESCAPE) close_modal();
        else if (k == SDLK_BACKSPACE) capture_assign("None");
        else if (g.cap_col == 0) {
            std::string n = key_name(k, sc);
            if (!n.empty()) capture_assign(n);
        }
        return true;
    }
    switch (k) {
    case SDLK_UP: nav(N_UP); return true;
    case SDLK_DOWN: nav(N_DOWN); return true;
    case SDLK_LEFT: nav(N_LEFT); return true;
    case SDLK_RIGHT: nav(N_RIGHT); return true;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: nav(N_OK); return true;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: nav(N_BACK); return true;
    case SDLK_Q: case SDLK_PAGEUP: if (!g.modal.empty()) return true; nav(N_PREV); return true;
    case SDLK_E: case SDLK_PAGEDOWN: if (!g.modal.empty()) return true; nav(N_NEXT); return true;
    default: return false;
    }
}

/* Left stick: directions with a repeat (400 ms, then every 120 ms); the
 * triggers count as buttons while a controller binding is captured. */
struct StickRepeat {
    int dir = -1;
    double next = 0;
    bool trig[2] = { false, false };
} g_stick;

void stick_update(SDL_Gamepad *pad)
{
    if (!pad) return;
    for (int t = 0; t < 2; t++) {
        bool on = SDL_GetGamepadAxis(pad, t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
        if (on && !g_stick.trig[t] && g.modal == "capture" && g.cap_col == 1) capture_assign(t ? "RT" : "LT");
        g_stick.trig[t] = on;
    }
    float x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
    float y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    int dir = -1;
    if (std::fabs(x) > 0.6f || std::fabs(y) > 0.6f) {
        if (std::fabs(x) > std::fabs(y)) dir = x > 0 ? SDL_GAMEPAD_BUTTON_DPAD_RIGHT : SDL_GAMEPAD_BUTTON_DPAD_LEFT;
        else dir = y > 0 ? SDL_GAMEPAD_BUTTON_DPAD_DOWN : SDL_GAMEPAD_BUTTON_DPAD_UP;
    }
    if (g.modal == "capture") { g_stick.dir = dir; return; }
    double t = now_ms();
    if (dir != g_stick.dir) {
        g_stick.dir = dir;
        if (dir >= 0) { pad_button(dir); g_stick.next = t + 400; }
    } else if (dir >= 0 && t >= g_stick.next) {
        pad_button(dir);
        g_stick.next = t + 120;
    }
}

/* Shift on the keyboard, or Select (Back) on a controller, held right now. */
bool launcher_key_held()
{
#ifdef _WIN32
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) return true;
#else
    if (SDL_GetModState() & SDL_KMOD_SHIFT) return true;
#endif
    bool held = false;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n && !held; i++) {
        SDL_Gamepad *p = SDL_OpenGamepad(ids[i]);
        if (!p) continue;
        /* the first state comes with the device: give it a moment */
        for (int k = 0; k < 10 && !held; k++) {
            SDL_UpdateGamepads();
            held = SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_BACK);
            if (!held) SDL_Delay(10);
        }
        SDL_CloseGamepad(p);
    }
    SDL_free(ids);
    return held;
}

/* ── Test script ─────────────────────────────────────────────────── */

void save_shot(const std::string &file)
{
    int w, h;
    SDL_GetWindowSizeInPixels(g.window, &w, &h);
    std::vector<unsigned char> px((size_t)w * h * 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    SDL_Surface *s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < h; y++) {
        unsigned char *row = (unsigned char *)s->pixels + (size_t)y * s->pitch;
        memcpy(row, &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        for (int x = 0; x < w; x++) row[x * 4 + 3] = 255;
    }
    if (!SDL_SavePNG(s, file.c_str())) otlog("shot: cannot write %s: %s", file.c_str(), SDL_GetError());
    else otlog("shot %s %dx%d", file.c_str(), w, h);
    SDL_DestroySurface(s);
}

void script_step()
{
    if (g.script_pos < g.script.size() && g.script[g.script_pos] == "waitgame") {
        if (g.game) return;
        g.script_pos++;
        g.script_wait = 30;
        return;
    }
    if (g.script_wait > 0) { g.script_wait--; return; }
    if (g.script_pos >= g.script.size()) return;
    std::string s = g.script[g.script_pos++];
    g.script_wait = 6;
    std::string a = s.substr(0, s.find(':')), b = s.find(':') != std::string::npos ? s.substr(s.find(':') + 1) : "";
    otlog("step %s focus=%s", s.c_str(), focused() ? focused()->GetAddress(false, false).c_str() : "-");
    if (a == "k") {
        static const struct { const char *n; SDL_Keycode k; } m[] = {
            { "up", SDLK_UP }, { "down", SDLK_DOWN }, { "left", SDLK_LEFT }, { "right", SDLK_RIGHT },
            { "enter", SDLK_RETURN }, { "esc", SDLK_ESCAPE }, { "tab", SDLK_TAB }, { "q", SDLK_Q }, { "e", SDLK_E },
            { "backspace", SDLK_BACKSPACE } };
        SDL_Keycode key = SDLK_UNKNOWN;
        for (auto &x : m)
            if (b == x.n) key = x.k;
        if (key == SDLK_UNKNOWN && b.size() == 1) key = (SDL_Keycode)tolower((unsigned char)b[0]);
        if (key != SDLK_UNKNOWN && !key_down(key, SDL_SCANCODE_UNKNOWN)) {
            g.ctx->ProcessKeyDown(RmlSDL::ConvertKey(key), 0);
            g.ctx->ProcessKeyUp(RmlSDL::ConvertKey(key), 0);
        }
    } else if (a == "p") {
        static const struct { const char *n; int b; } m[] = {
            { "a", SDL_GAMEPAD_BUTTON_SOUTH }, { "b", SDL_GAMEPAD_BUTTON_EAST }, { "x", SDL_GAMEPAD_BUTTON_WEST },
            { "y", SDL_GAMEPAD_BUTTON_NORTH }, { "lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
            { "rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER }, { "up", SDL_GAMEPAD_BUTTON_DPAD_UP },
            { "down", SDL_GAMEPAD_BUTTON_DPAD_DOWN }, { "left", SDL_GAMEPAD_BUTTON_DPAD_LEFT },
            { "right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT }, { "start", SDL_GAMEPAD_BUTTON_START },
            { "back", SDL_GAMEPAD_BUTTON_BACK } };
        /* Through SDL's event queue, as a controller's button would come. */
        for (auto &x : m)
            if (b == x.n) {
                SDL_Event e{};
                e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                e.gbutton.button = (Uint8)x.b;
                e.gbutton.down = true;
                SDL_PushEvent(&e);
            }
    } else if (a == "click") {
        Rml::Element *e = nullptr;
        if (!g.modal.empty() && g.dialog) e = g.dialog->GetElementById(b);
        if (!e && page_doc()) e = page_doc()->GetElementById(b);
        if (e) e->Click();
        else otlog("click: no element #%s", b.c_str());
    } else if (a == "probe") {
        Rml::ElementDocument *d = page_doc();
        Rml::Element *e = d ? d->GetElementById(b) : nullptr;
        int w, h;
        SDL_GetWindowSizeInPixels(g.window, &w, &h);
        otlog("probe ctx=%dx%d dp=%.3f px=%dx%d stage=%.0fdp", g.ctx->GetDimensions().x, g.ctx->GetDimensions().y,
              g.ctx->GetDensityIndependentPixelRatio(), w, h, g.stage_w);
        if (e) {
            Rml::Vector2f o = e->GetAbsoluteOffset(Rml::BoxArea::Border), z = e->GetBox().GetSize(Rml::BoxArea::Border);
            otlog("probe #%s at %.1f,%.1f size %.1fx%.1f", b.c_str(), o.x, o.y, z.x, z.y);
        }
    } else if (a == "mclick" || a == "mhover") {
        /* The mouse at x,y (window pixels) as a player's would be; the element
         * under it and the dialog opened (if any) are logged. */
        int x = atoi(b.c_str()), y = b.find(':') != std::string::npos ? atoi(b.c_str() + b.find(':') + 1) : 0;
        set_pad_active(false);
        g.ctx->ProcessMouseMove(x, y, 0);
        Rml::Element *h = g.ctx->GetHoverElement();
        if (a == "mclick") {
            g.ctx->ProcessMouseButtonDown(0, 0);
            g.ctx->ProcessMouseButtonUp(0, 0);
        }
        otlog("%s %d,%d -> %s -> modal=%s", a.c_str(), x, y, h ? h->GetAddress(false, false).c_str() : "-",
              g.modal.empty() ? "-" : g.modal.c_str());
    } else if (a == "shot") {
        g.shot_pending = b;
    } else if (a == "disc") {
        set_disc(b);                  /* as if this disc image was chosen or dropped */
    } else if (a == "wait") {
        g.script_wait = atoi(b.c_str());
    } else if (a == "quit") {
        g.quit = true;
    }
}

/* ── Data model ──────────────────────────────────────────────────── */

bool build_model()
{
    Rml::DataModelConstructor c = g.ctx->CreateDataModel("ot");
    if (!c) return false;
    c.RegisterArray<std::vector<Rml::String>>();
    c.RegisterArray<std::vector<int>>();
    if (auto h = c.RegisterStruct<Row>()) {
        h.RegisterMember("id", &Row::id);
        h.RegisterMember("is_group", &Row::is_group);
        h.RegisterMember("label", &Row::label);
        h.RegisterMember("value", &Row::value);
        h.RegisterMember("button", &Row::button);
        h.RegisterMember("pc", &Row::pc);
        h.RegisterMember("changed", &Row::changed);
        h.RegisterMember("pending", &Row::pending);
        h.RegisterMember("arrows", &Row::arrows);
        h.RegisterMember("toggle", &Row::toggle);
        h.RegisterMember("on", &Row::on);
        h.RegisterMember("plain", &Row::plain);
    }
    c.RegisterArray<std::vector<Row>>();
    if (auto h = c.RegisterStruct<CtlRow>()) {
        h.RegisterMember("k", &CtlRow::k);
        h.RegisterMember("label", &CtlRow::label);
        h.RegisterMember("key", &CtlRow::key);
        h.RegisterMember("pad", &CtlRow::pad);
        h.RegisterMember("icon", &CtlRow::icon);
        h.RegisterMember("has_pad", &CtlRow::has_pad);
        h.RegisterMember("kchg", &CtlRow::kchg);
        h.RegisterMember("pchg", &CtlRow::pchg);
    }
    c.RegisterArray<std::vector<CtlRow>>();
    if (auto h = c.RegisterStruct<NoteSec>()) {
        h.RegisterMember("name", &NoteSec::name);
        h.RegisterMember("items", &NoteSec::items);
    }
    c.RegisterArray<std::vector<NoteSec>>();
    c.Bind("carpet", &g.carpet);
    c.Bind("rows", &g.rows);
    c.Bind("ctls", &g.ctls);
    c.Bind("bstyle", &g.bstyle);
    c.Bind("hint_style", &g.hint_style);
    c.Bind("tabs", &g.tabs);
    c.Bind("presets", &g.presets);
    c.Bind("tab", &g.tab);
    c.Bind("preset", &g.preset);
    c.Bind("help_title", &g.help_title);
    c.Bind("help_text", &g.help_text);
    c.Bind("help_default", &g.help_default);
    c.Bind("help_env", &g.help_env);
    c.Bind("help_extra", &g.help_extra);
    c.Bind("help_pc", &g.help_pc);
    c.Bind("help_pending", &g.help_pending);
    c.Bind("pad_active", &g.pad_active);
    c.Bind("have_disc", &g.have_disc);
    c.Bind("disc_state", &g.disc_state);
    c.Bind("disc_title", &g.disc_title);
    c.Bind("disc_note", &g.disc_note);
    c.Bind("disc_kicker", &g.disc_kicker);
    c.Bind("disc_head", &g.disc_head);
    c.Bind("disc_text", &g.disc_text);
    c.Bind("disc_path", &g.disc_path);
    c.Bind("disc_drop1", &g.disc_drop1);
    c.Bind("disc_drop2", &g.disc_drop2);
    c.Bind("track", &g.track);
    c.Bind("track_name", &g.track_name);
    c.Bind("version_text", &g.version_text);
    c.BindFunc("version", [](Rml::Variant &v) { v = Rml::String(OT_VERSION); });
    c.Bind("saved_note", &g.saved_note);
    c.Bind("notes_version", &g.notes_version);
    c.Bind("notes_title", &g.notes_title);
    c.Bind("notes", &g.notes);
    c.Bind("upd_state", &g.upd_state);
    c.Bind("upd_view", &g.upd_view);
    for (auto &b : std::initializer_list<std::pair<const char *, Rml::String *>>{
             { "inst_kick", &g.inst_kick }, { "inst_head", &g.inst_head }, { "inst_text", &g.inst_text }, { "inst_phase", &g.inst_phase },
             { "inst_s1", &g.inst_s1 }, { "inst_s2", &g.inst_s2 }, { "inst_s3", &g.inst_s3 }, { "inst_l1", &g.inst_l1 },
             { "inst_l3", &g.inst_l3 }, { "inst_bar", &g.inst_bar }, { "inst_foot", &g.inst_foot } })
        c.Bind(b.first, b.second);
    c.Bind("modal", &g.modal);
    c.Bind("cap_kind", &g.cap_kind);
    c.Bind("cap_for", &g.cap_for);
    c.Bind("cap_now", &g.cap_now);
    c.Bind("cap_hint", &g.cap_hint);
    c.Bind("conf_kind", &g.conf_kind);
    c.Bind("conf_line", &g.conf_line);
    c.Bind("conf_swap", &g.conf_swap);
    c.Bind("conf_both", &g.conf_both);
    c.Bind("crash_head", &g.crash_head);
    c.Bind("crash_when", &g.crash_when);
    c.Bind("crash_version", &g.crash_version);
    c.Bind("notice_head", &g.notice_head);
    c.Bind("notice_text", &g.notice_text);
    c.BindFunc("notice_file", [](Rml::Variant &v) { v = !g.notice_file.empty(); });

    c.BindEventCallback("set_tab", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (!a.empty()) set_tab(a[0].Get<int>());
    });
    c.BindEventCallback("preset", [](Rml::DataModelHandle h, Rml::Event &, const Rml::VariantList &a) {
        if (a.empty()) return;
        int p = a[0].Get<int>();
        if (p >= 0 && p < SETTINGS_PRESET_COUNT) settings_apply_preset(g.set, p);
        rebuild_ctls();
        settings_changed(-1);
        h.DirtyVariable("preset");
    });
    c.BindEventCallback("row_click", [](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &a) {
        if (a.empty()) return;
        int id = a[0].Get<int>();
        if (id < 0) return;
        if (id >= 1000) { row_action(id); return; }
        const SettingDef *d = settings_def(id);
        if (d->type == SETTING_PATH) { choose_path(id); return; }
        if (d->type == SETTING_KEY) return;
        /* A click on the left arrow steps back; anywhere else steps forward. */
        Rml::Element *t = ev.GetTargetElement();
        step_value(id, t && t->IsClassSet("left") ? -1 : 1);
        if (id == S_BUTTON_STYLE) rebuild_ctls();
    });
    c.BindEventCallback("ctl_click", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (a.size() < 2) return;
        g.focus_id = 2000 + a[0].Get<int>() * 2 + a[1].Get<int>();
        capture_start(a[0].Get<int>(), a[1].Get<int>());
    });
    c.BindEventCallback("style_pick", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (a.empty()) return;
        const SettingDef *d = settings_def(S_BUTTON_STYLE);
        int i = a[0].Get<int>();
        if (i >= 0 && i < d->nchoices) settings_set(g.set, S_BUTTON_STYLE, d->choices[i].token);
        rebuild_ctls();
        settings_changed(S_BUTTON_STYLE);
    });
    c.BindEventCallback("go", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (a.empty()) return;
        Rml::String p = a[0].Get<Rml::String>();
        if (p == "updates") {             /* the update icon: ADVANCED > Updates */
            g.tab = SETTINGS_TAB_ADVANCED;
            rebuild_rows();
            g.model.DirtyVariable("tab");
            p = "settings";
        }
        show_page(p);
    });
    c.BindEventCallback("open", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (a.empty()) return;
        Rml::String w = a[0].Get<Rml::String>();
        const char *url = w == "releases" ? k_url_releases : w == "github" ? k_url_github : w == "kofi" ? k_url_kofi : nullptr;
        if (url && !SDL_OpenURL(url)) otlog("cannot open %s: %s", url, SDL_GetError());
    });
    c.BindEventCallback("dlg", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (!a.empty()) dialog_action(a[0].Get<Rml::String>());
    });
    c.BindEventCallback("upd", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        if (!a.empty()) update_action(a[0].Get<Rml::String>());
    });
    c.BindEventCallback("bug", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { show_modal("report_ask"); });
    c.BindEventCallback("play", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { play(); });
    c.BindEventCallback("save", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { save_settings(); });
    c.BindEventCallback("reset_tab", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
        for (int id = 0; id < SETTINGS_COUNT; id++)
            if (settings_def(id)->tab == g.tab && id != S_DISC_IMAGE) settings_set(g.set, id, settings_def(id)->def);
        rebuild_ctls();
        settings_changed(-1);
    });
    c.BindEventCallback("choose_disc", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
        choose_path(S_DISC_IMAGE);
    });
    g.model = c.GetModelHandle();
    return true;
}

/* The panel shows these notes. */
void show_notes(const OtNotes &n, const std::string &version)
{
    g.notes_version = version;
    g.notes_title = n.title;
    for (char &ch : g.notes_title) ch = (char)toupper((unsigned char)ch);
    g.notes.clear();
    for (const OtNoteSection &s : n.sections) {
        NoteSec ns;
        ns.name = s.name;
        for (const std::string &i : s.items) ns.items.push_back(i);
        g.notes.push_back(ns);
    }
    if (g.model)
        for (const char *v : { "notes_version", "notes_title", "notes", "upd_view" }) g.model.DirtyVariable(v);
}

void load_notes()
{
    g.installed_notes = ot_notes_parse(k_whatsnew);
    if (!g.installed_notes.ok) {
        otlog("what's new: no launcher block built in");
        return;
    }
    show_notes(g.installed_notes, g.installed_notes.version.empty() ? OT_VERSION : g.installed_notes.version);
}

/* ── Is there a newer version? (ot_update.h) ─────────────────────────
 * One check per start at most, in a thread of its own: the home page shows
 * at once with the version alone and the built-in notes, and the pill
 * changes if the list of releases comes within k_update_wait_ms. */

struct UpdateJob {
    std::mutex m;
    bool done = false;
    OtUpdateResult r;
};
std::shared_ptr<UpdateJob> g_update_job;
bool g_update_late_logged = false;

bool updates_off() { return SDL_strcasecmp(settings_get(g.set, S_CHECK_UPDATES), "never") == 0; }

std::string tag_version(const std::string &tag)
{
    return !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
}

void update_back_to_installed()
{
    g.upd_view = false;
    if (g.installed_notes.ok)
        show_notes(g.installed_notes, g.installed_notes.version.empty() ? OT_VERSION : g.installed_notes.version);
    else if (g.model)
        g.model.DirtyVariable("upd_view");
}

/* The pill from what was read: "✓ vX · Latest", "Update to vY", or "vX"
 * (never checked, no answer, Never, a skipped version, or this build newer
 * than any release). */
void update_apply()
{
    g.upd_state = "";
    g.upd_url.clear();
    g.upd_tag.clear();
    if (!updates_off() && g.upd_have) {
        bool unstable = SDL_strcasecmp(settings_get(g.set, S_UPDATE_CHANNEL), "unstable") == 0;
        int i = ot_update_pick(g.upd_releases, unstable);
        if (i >= 0) {
            const OtRelease &r = g.upd_releases[i];
            int c = ot_version_cmp(r.tag, OT_VERSION);
            if (c > 0 && r.tag != g.upd_skip) {
                g.upd_state = "update";
                g.upd_url = r.url.rfind("https://", 0) == 0 ? r.url : k_url_releases;
                g.upd_tag = r.tag;
            } else if (c == 0) {
                g.upd_state = "latest";
            }
            otlog("updates: %s channel follows %s%s -> %s", unstable ? "unstable" : "stable", r.tag.c_str(),
                  r.prerelease ? " (pre-release)" : "",
                  g.upd_state.empty() ? (c > 0 ? "skipped" : "this build is newer") : g.upd_state.c_str());
        } else {
            otlog("updates: no release with a version in the list");
        }
    }
    if (g.upd_state == "update") g.version_text = "Update to v" + tag_version(g.upd_tag);
    else if (g.upd_state == "latest") g.version_text = std::string("v") + OT_VERSION + " \xC2\xB7 Latest";
    else g.version_text = std::string("v") + OT_VERSION;
    if (g.upd_state != "update" && g.upd_view) update_back_to_installed();
    if (g.model)
        for (const char *v : { "version_text", "upd_state" }) g.model.DirtyVariable(v);
}

void update_start()
{
    if (g.upd_started) return;
    if (updates_off()) {
        otlog("updates: Check for updates = Never, no request");
        return;
    }
    OtUpdateRequest req;
    req.json_file = g.upd_test_json;
    req.url = g.upd_test_url.empty() ? k_url_api_releases : g.upd_test_url;
    req.cache_file = g.upd_test_json.empty() && g.upd_test_url.empty() ? g.upd_cache : "";
    req.allow_loopback_http = !g.upd_test_url.empty();
    req.user_agent = std::string("OpenTricky-launcher/") + OT_VERSION;
    req.timeout_ms = (int)k_update_wait_ms;
    if (req.json_file.empty() && req.cache_file.empty() && g.upd_test_url.empty()) {
        otlog("updates: no folder for the cache file, no request");     /* nothing would keep it to once a day */
        return;
    }
    g.upd_started = true;
    g.upd_t0 = now_ms();
    auto job = std::make_shared<UpdateJob>();
    g_update_job = job;
    std::thread([job, req]() {
        OtUpdateResult r = ot_update_check(req);
        std::lock_guard<std::mutex> lk(job->m);
        job->r = std::move(r);
        job->done = true;
    }).detach();
    otlog("updates: check started (%s)", !req.json_file.empty() ? "test file" : req.cache_file.empty() ? "test address" : "cache or GitHub");
}

/* From the main loop: takes the answer when it is there. */
void update_poll()
{
    if (!g_update_job) return;
    double waited = now_ms() - g.upd_t0;
    OtUpdateResult r;
    {
        std::lock_guard<std::mutex> lk(g_update_job->m);
        if (!g_update_job->done) {
            if (waited > k_update_wait_ms && !g_update_late_logged) {
                otlog("updates: no answer within %.0f ms: the version alone", k_update_wait_ms);
                g_update_late_logged = true;
            }
            return;
        }
        r = std::move(g_update_job->r);
    }
    g_update_job.reset();
    otlog("updates: %s, %s, after %.0f ms (%d releases)", r.ok ? "read" : "nothing", r.source.c_str(), waited, (int)r.releases.size());
    if (!r.ok) return;
    if (waited > k_update_wait_ms + 250) {      /* the main loop looks every 100 ms at most */
        otlog("updates: too late to be shown");
        return;
    }
    g.upd_have = true;
    g.upd_releases = std::move(r.releases);
    if (!r.skip.empty()) g.upd_skip = r.skip;
    update_apply();
}

void update_settings_changed()
{
    update_apply();
    if (!updates_off()) update_start();
}

/* ── Installing a newer version, or going back (ot_install.h) ─────────
 * Only after a click; never while the game runs (the launcher is hidden
 * then). The dialog "update" shows the steps; on success the new launcher
 * is started and this one closes. */

std::shared_ptr<OtInstallState> g_install;
bool g_install_restore = false;
std::string g_install_version;

const OtRelease *update_release()
{
    for (const OtRelease &r : g.upd_releases)
        if (r.tag == g.upd_tag) return &r;
    return nullptr;
}

void install_texts()
{
    for (const char *v : { "inst_kick", "inst_head", "inst_text", "inst_phase", "inst_s1", "inst_s2", "inst_s3",
                           "inst_l1", "inst_l3", "inst_bar", "inst_foot" })
        g.model.DirtyVariable(v);
}

void install_fail(const std::string &phase, const std::string &head, const std::string &text)
{
    otlog("updates: dialog %s: %s", head.c_str(), text.c_str());
    g.inst_phase = phase;
    g.inst_head = head;
    g.inst_text = text;
    install_texts();
    if (g.modal != "update") show_modal("update");
}

void install_start(bool restore)
{
    if (g_install || g.game) return;
    OtInstallRequest req;
    req.base = g.base;
    req.version_now = OT_VERSION;
    req.user_agent = std::string("OpenTricky-launcher/") + OT_VERSION;
    req.allow_loopback_http = !g.upd_test_url.empty();
    req.fail_at = g.inst_test_fail_at;
    std::string backup_version;
    g_install_restore = restore;
    g.inst_kick = restore ? "RESTORE" : (SDL_strcasecmp(settings_get(g.set, S_UPDATE_CHANNEL), "unstable") == 0 ? "UPDATE \xC2\xB7 UNSTABLE" : "UPDATE \xC2\xB7 STABLE");
    g.inst_s1 = g.inst_s2 = g.inst_s3 = "";
    g.inst_bar = "0%";
    g.inst_l1 = "Download";
    g.inst_l3 = "Install";
    g.inst_foot = "";
    if (restore) {
        if (!ot_install_has_backup(g.base, backup_version)) return;
        g_install_version = backup_version;
        g.inst_head = "RESTORING v" + backup_version;
    } else {
        const OtRelease *r = update_release();
        if (!r) return;
        g_install_version = tag_version(r->tag);
        g.inst_head = "UPDATING TO v" + g_install_version;
        const OtAsset *zip = nullptr, *sums = nullptr;
        for (const OtAsset &a : r->assets) {
            std::string n = a.name;
            for (char &c : n) c = (char)tolower((unsigned char)c);
            if (n == "sha256sums.txt") sums = &a;
            else if (n.size() > 10 && n.compare(n.size() - 10, 10, "-win64.zip") == 0) zip = &a;
            else if (!zip && n.size() > 4 && n.compare(n.size() - 4, 4, ".zip") == 0) zip = &a;
        }
        if (!zip) {             /* nothing to install: the release's page */
            otlog("updates: %s has no zip, its page opens", r->tag.c_str());
            if (!g.upd_url.empty()) SDL_OpenURL(g.upd_url.c_str());
            return;
        }
        req.zip_name = zip->name;
        req.zip_url = zip->url;
        req.zip_size = zip->size;
        req.sums_url = sums ? sums->url : "";
        req.version_new = g_install_version;
    }
    g.inst_text = "The game stays closed while it updates. Your settings, saves, disc image and HD pack are never touched.";
    if (!ot_install_writable(g.base)) {
        otlog("updates: the program's folder cannot be written");
        install_fail("nowrite", "CAN'T UPDATE HERE",
                     "This folder can't be written (Program Files?), so nothing was changed. Download v" + g_install_version +
                     " from its page and unzip it over this folder: your settings and saves stay.");
        return;
    }
    if (ot_install_game_running(g.base)) {
        install_fail("fail", "THE GAME IS RUNNING", "Close SSX Tricky first. Nothing was changed.");
        return;
    }
    g.inst_phase = "run";
    install_texts();
    show_modal("update");
    auto st = std::make_shared<OtInstallState>();
    g_install = st;
    otlog("updates: %s %s", restore ? "restore of" : "install of", g_install_version.c_str());
    std::thread([st, req, restore]() {
        if (restore) ot_install_restore(req, *st);
        else ot_install_update(req, *st);
    }).detach();
}

/* The new launcher, then this one closes. */
void install_relaunch()
{
    std::vector<std::string> args = { g.base + "OpenTricky.exe", g_install_restore ? "--restored" : "--updated", g_install_version };
    if (!g.upd_test_url.empty()) {      /* tests: the new launcher shows itself, takes its picture and closes */
        args.insert(args.end(), { "--updates-url", g.upd_test_url, "--log", g.base + "relaunch.log",
                                  "--script", "wait:150,shot:" + g.base + "relaunch.png" });
        if (!g.iso.empty()) args.insert(args.end(), { "--iso", g.iso });
    }
    std::vector<const char *> argv;
    for (const std::string &a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_Process *p = SDL_CreateProcess(argv.data(), false);
    if (!p) {
        otlog("updates: cannot start the new launcher: %s", SDL_GetError());
        g.inst_phase = "done_manual";
        g.inst_text = "Done. Start OpenTricky again to use the new version.";
        install_texts();
        return;
    }
    SDL_DestroyProcess(p);
    otlog("updates: new launcher started, this one closes");
    g.quit = true;
}

void install_poll()
{
    if (!g_install) return;
    OtInstallState &st = *g_install;
    int step = st.step.load();
    long long got = st.got.load(), total = st.total.load();
    if (!g_install_restore) {
        g.inst_s1 = step > 1 ? "ok" : step == 1 ? "run" : "";
        g.inst_s2 = step > 2 ? "ok" : step == 2 ? "run" : "";
        char b[96];
        if (step <= 1 && total > 0) snprintf(b, sizeof b, "Downloading \xC2\xB7 %.1f / %.1f MB", got / 1048576.0, total / 1048576.0);
        else snprintf(b, sizeof b, "Downloaded \xC2\xB7 %.1f MB", (total > 0 ? total : got) / 1048576.0);
        g.inst_l1 = b;
    }
    g.inst_s3 = step > 3 ? "ok" : step == 3 ? "run" : "";
    g.inst_l3 = step >= 3 ? (g_install_restore ? "Putting back the previous version\xE2\x80\xA6" : "Installing\xE2\x80\xA6") : "Install";
    int pct = 0;
    if (g_install_restore) pct = step >= 4 ? 100 : 50;
    else if (step <= 1) pct = total > 0 ? (int)(got * 80 / total) : 0;
    else pct = step == 2 ? 85 : step == 3 ? 92 : 100;
    g.inst_bar = std::to_string(pct) + "%";
    install_texts();
    if (!st.done.load()) return;
    bool ok;
    std::string fail, detail;
    {
        std::lock_guard<std::mutex> lk(st.m);
        ok = st.ok;
        fail = st.fail;
        detail = st.detail;
    }
    g_install.reset();
    otlog("updates: %s %s%s%s", g_install_restore ? "restore" : "install", ok ? "done" : ("failed (" + fail + ")").c_str(),
          detail.empty() ? "" : ": ", detail.c_str());
    if (ok) {
        g.inst_phase = "done";
        g.inst_l3 = g_install_restore ? "Previous version back" : "Installed";
        g.inst_bar = "100%";
        install_texts();
        install_relaunch();
        return;
    }
    if (fail == "verify") install_fail("fail", "UPDATE STOPPED", "The update couldn't be verified. Nothing was changed.");
    else if (fail == "download") install_fail("fail", "UPDATE STOPPED", "The update couldn't be downloaded. Nothing was changed.");
    else if (fail == "busy") install_fail("fail", "THE GAME IS RUNNING", "Close SSX Tricky first. Nothing was changed.");
    else if (g_install_restore) install_fail("fail", "RESTORE STOPPED", "The previous version couldn't be put back. Nothing was changed.");
    else install_fail("fail", "UPDATE STOPPED", "The update couldn't be installed. Nothing was changed.");
}

bool install_running() { return g_install != nullptr; }

/* The pill, and the three buttons under the newer version's notes. */
void update_action(const std::string &a)
{
    if (a == "pill") {
        if (g.upd_state != "update") return;
        int i = -1;
        for (int k = 0; k < (int)g.upd_releases.size(); k++)
            if (g.upd_releases[k].tag == g.upd_tag) i = k;
        if (i < 0) return;
        OtNotes n = ot_update_notes(g.upd_releases[i]);
        g.upd_view = true;
        show_notes(n, tag_version(g.upd_tag));
        if (g.page != "home") show_page("home");
        otlog("updates: notes of %s shown (%s)", g.upd_tag.c_str(),
              ot_notes_parse(g.upd_releases[i].body).ok ? "launcher block" : "highlights");
    } else if (a == "now") {
        otlog("updates: Update now (%s)", g.upd_tag.c_str());
        install_start(false);
    } else if (a == "later") {
        update_back_to_installed();     /* the pill stays; the next start shows it again */
    } else if (a == "skip") {
        g.upd_skip = g.upd_tag;
        if (!g.upd_test_json.empty() || !g.upd_test_url.empty()) otlog("updates: %s skipped (test: not kept)", g.upd_tag.c_str());
        else if (!ot_update_write_skip(g.upd_cache, g.upd_skip)) otlog("updates: could not keep the skipped version");
        else otlog("updates: %s skipped", g.upd_skip.c_str());
        update_back_to_installed();
        update_apply();
    }
}

/* Resize the RmlUi context to the window. */
void fit_window(int pw, int ph)
{
    g.render->SetViewport(pw, ph);
    g.ctx->SetDimensions({ pw, ph });
    g.ctx->SetDensityIndependentPixelRatio(std::min(pw / 1280.0f, ph / 800.0f));
    apply_layout();
}

} // namespace

int main(int argc, char **argv)
{
    double t_main = ms_since_process_start();
    std::string page = "home", size_arg, log_file, crash_test;
    int track_arg = -1, tab_arg = -1;
    bool no_skip = false;
    double quit_after = -1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--iso") g.iso = next();
        else if (a == "--track") track_arg = atoi(next().c_str());
        else if (a == "--page") page = next();
        else if (a == "--tab") tab_arg = atoi(next().c_str());
        else if (a == "--size") size_arg = next();
        else if (a == "--log") log_file = next();
        else if (a == "--quit-after") quit_after = atof(next().c_str());
        else if (a == "--no-skip") no_skip = true;
        else if (a == "--crash-test") crash_test = next();
        else if (a == "--report-dir") g_report_dir = next();
        /* Tests of the update check: a local JSON file instead of GitHub, or
         * another https address (no cache read or written with either). */
        else if (a == "--updates-json") g.upd_test_json = next();
        else if (a == "--updates-url") g.upd_test_url = next();
        else if (a == "--updates-fail-at") g.inst_test_fail_at = next();   /* tests: an install that fails there */
        else if (a == "--updated" || a == "--restored") { g.started_restored = a == "--restored"; g.started_after = next(); }
        else if (a == "--script") {
            std::string s = next();
            size_t p = 0;
            while (p <= s.size()) {
                size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                if (q > p) g.script.push_back(s.substr(p, q - p));
                p = q + 1;
            }
        }
    }
    if (!log_file.empty()) freopen(log_file.c_str(), "w", stderr);
    otlog("OpenTricky launcher %s", OT_VERSION);
    if (!g.started_after.empty()) otlog("started after %s %s", g.started_restored ? "the restore of" : "the update to", g.started_after.c_str());
    otlog("timing process_to_main_ms=%.1f", t_main);

    double t0 = now_ms();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        otlog("error: SDL_Init: %s", SDL_GetError());
        return 1;
    }
    double t_sdl = now_ms();
    const char *bp = SDL_GetBasePath();
    g.base = bp ? bp : "";
    {
        /* What an update left: OpenTricky.exe.old (the launcher that installed
         * this one may still be closing) and update-tmp\. */
        std::string b = g.base;
        std::thread([b]() { ot_install_cleanup(b); }).detach();
    }

    /* Settings: the registry and settings.ini. */
    g.set = (Settings *)calloc(1, sizeof(Settings));
    settings_defaults(g.set);
    char path[1024];
    if (settings_locate(nullptr, nullptr, 0, path, sizeof path) != SETTINGS_AT_NONE) {
        g.ini_read = path;
        int st = settings_load(g.set, path, nullptr);
        static const char *const names[] = { "read", "no file yet: defaults", "an older file (no [Meta] Version): ignored, defaults",
                                             "written by a newer version", "could not be read: defaults" };
        otlog("settings: %s", st >= 0 && st <= 4 ? names[st] : "?");
    }
    int at = settings_locate(nullptr, nullptr, 1, path, sizeof path);
    if (at != SETTINGS_AT_NONE) g.ini_write = path;
    g.ini_in_docs = at == SETTINGS_AT_DOCUMENTS;
    if (g.iso.empty()) g.iso = settings_get(g.set, S_DISC_IMAGE);
    else settings_set(g.set, S_DISC_IMAGE, g.iso.c_str());
    g.preset = settings_current_preset(g.set);
    for (int t = 0; t < SETTINGS_TAB_COUNT; t++) g.tabs.push_back(settings_tab_name(t));
    for (int p = 0; p < SETTINGS_PRESET_CUSTOM + 1; p++) g.presets.push_back(settings_preset_name(p));
    g.version_text = std::string("v") + OT_VERSION;   /* until the list of releases is read (update_apply) */
    if (tab_arg >= 0 && tab_arg < SETTINGS_TAB_COUNT) g.tab = tab_arg;
    load_notes();
    {
        std::string ini = !g.ini_write.empty() ? g.ini_write : g.ini_read;
        if (!ini.empty()) g.upd_cache = ot_dir_of(ini) + "update-cache.txt";
    }

    /* Skip launcher: straight to the game, unless Shift / Select is held. */
    if (!no_skip && crash_test.empty() && g.script.empty() && settings_get_bool(g.set, S_SKIP_LAUNCHER)) {
        if (launcher_key_held()) {
            otlog("skip launcher: Shift / Select held, the launcher shows");
        } else {
            OtDiscCheck d = g.iso.empty() ? OtDiscCheck{ OT_DISC_NO_PATH } : ot_disc_check(g.iso.c_str());
            if (d.status == OT_DISC_OK) g.skip_mode = true;
            else otlog("skip launcher: no good disc image, the launcher shows");
        }
    }

    /* Window and OpenGL 3.3 core. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    int ww = (int)(1280 * scale), wh = (int)(800 * scale);
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) {
        float k = std::min(1.0f, std::min(usable.w * 0.92f / ww, usable.h * 0.92f / wh));
        ww = (int)(ww * k);
        wh = (int)(wh * k);
    }
    if (!size_arg.empty()) sscanf(size_arg.c_str(), "%dx%d", &ww, &wh);
    /* The window icon (title bar, task bar) is the exe's own icon resource
     * (OpenTricky.rc, the OT badge): SDL loads the first ICON group of the
     * exe for its window class on Windows, at every size the .ico holds.
     * SDL_SetWindowIcon would replace it with a single-size copy. */
    g.window = SDL_CreateWindow("OpenTricky", ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!g.window) {
        otlog("error: window: %s", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(g.window, 640, 400);
    g.gl = SDL_GL_CreateContext(g.window);
    SDL_GL_MakeCurrent(g.window, g.gl);
    SDL_GL_SetSwapInterval(1);
    Rml::String gl_message;
    if (!RmlGL3::Initialize(&gl_message)) {
        otlog("error: OpenGL 3.3: %s", gl_message.c_str());
        return 1;
    }
    otlog("%s", gl_message.c_str());
    if (const GLubyte *r = glGetString(GL_RENDERER)) g.gl_renderer = (const char *)r;
    double t_gl = now_ms();

    /* RmlUi. */
    OtSystemInterface system(g.window);
    OtRenderInterface render;
    g.render = &render;
    render.maker = make_texture;
    Rml::SetSystemInterface(&system);
    Rml::SetRenderInterface(&render);
    Rml::SetFileInterface(&g.files);
    Rml::Initialise();
    const char *fonts[] = { "ui/fonts/Inter-Variable.ttf", "ui/fonts/BebasNeue-Regular.ttf", "ui/fonts/Exo2-Italic-Variable.ttf" };
    for (const char *f : fonts)
        if (!Rml::LoadFontFace(g.base + f, f == fonts[0])) otlog("font missing: %s", f);
    /* Bullet (House Industries): the logo, PLAY and the titles; Exo 2 Black
     * Italic (free) if the file is missing. */
    g.bullet = Rml::LoadFontFace(g.base + "ui/fonts/bullet-smallcaps.otf", false, Rml::Style::FontWeight::Normal);
    otlog("display font: %s", g.bullet ? "Bullet" : "Exo 2 Black Italic (fallback)");
    double t_rml = now_ms();

    load_disc();
    if (g.have_assets) {
        int t = track_arg;
        /* A track picked at random at each start, with no choice anywhere;
         * --track is for the project's tests only. */
        if (t < 0) t = (int)(std::random_device{}() % OT_TRACK_COUNT);
        g.track = t % OT_TRACK_COUNT;
    }
    pick_theme();
    for (int i = 0; i < 90; i++) g.carpet.push_back((i * 7 + (i / 9) * 3) % OT_TRACK_COUNT);
    double t_disc = now_ms();

    int pw, ph;
    SDL_GetWindowSizeInPixels(g.window, &pw, &ph);
    render.SetViewport(pw, ph);
    g.ctx = Rml::CreateContext("main", Rml::Vector2i(pw, ph));
    g.ctx->SetDensityIndependentPixelRatio(std::min(pw / 1280.0f, ph / 800.0f));
    build_model();
    rebuild_rows();
    rebuild_ctls();
    g.page = page;
    load_documents();
    double t_docs = now_ms();

    if (!crash_test.empty()) {
        OtCrashReport r;
        r.found = SDL_GetPathInfo(crash_test.c_str(), nullptr);
        r.dir = crash_test + (ends_with_ci(crash_test, "\\") || ends_with_ci(crash_test, "/") ? "" : "\\");
        r.name = ot_file_name(crash_test);
        r.kind = r.name.find("freeze") != std::string::npos ? "freeze" : "crash";
        SDL_PathInfo pi;
        if (SDL_GetPathInfo(crash_test.c_str(), &pi)) r.time = pi.create_time;
        set_crash(r, 0);
        show_modal("crash");
    }
    if (g.skip_mode) {
        otlog("skip launcher: the game starts at once");
        if (!play()) g.skip_mode = false;
    }

    SDL_Gamepad *pad = nullptr;
    bool first = true;
    int frames = 0;
    while (!g.quit) {
        SDL_Event ev;
        bool hidden = g.game || g.skip_mode;
        bool busy = !hidden && (!g.script.empty() || first || g.focus_first_row || g.focus_modal || g.cap_until > 0);
        double delay = g.ctx->GetNextUpdateDelay();
        bool has = busy ? SDL_PollEvent(&ev)
                        : SDL_WaitEventTimeout(&ev, hidden ? 100 : (Sint32)std::min(delay * 1000.0, 100.0));
        while (has) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                if (install_running()) otlog("close asked during an install: it finishes first");
                else g.quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!key_down(ev.key.key, ev.key.scancode)) RmlSDL::InputEventHandler(g.ctx, g.window, ev);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                fit_window(ev.window.data1, ev.window.data2);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad) pad = SDL_OpenGamepad(ev.gdevice.which);
                otlog("controller: %s", pad ? SDL_GetGamepadName(pad) : "?");
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad && SDL_GetGamepadID(pad) == ev.gdevice.which) { SDL_CloseGamepad(pad); pad = nullptr; }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                SDL_GamepadType t = SDL_GetGamepadTypeForID(ev.gbutton.which);
                g.pad_is_ps = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
                update_hint_style();
                if (!g.game) pad_button(ev.gbutton.button);
                break;
            }
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                break;   /* the layout already follows the window's size (1280 x 800 dp stage) */
            case SDL_EVENT_MOUSE_MOTION:
                set_pad_active(false);
                RmlSDL::InputEventHandler(g.ctx, g.window, ev);
                break;
            case SDL_EVENT_DROP_FILE:
                if (ev.drop.data && (ends_with_ci(ev.drop.data, ".iso") || ends_with_ci(ev.drop.data, ".xiso"))) {
                    otlog("disc image dropped");
                    set_disc(ev.drop.data);
                }
                break;
            default: RmlSDL::InputEventHandler(g.ctx, g.window, ev); break;
            }
            has = SDL_PollEvent(&ev);
        }
        if (g.game) {
            int code = 0;
            if (SDL_WaitProcess(g.game, false, &code)) {
                SDL_DestroyProcess(g.game);
                g.game = nullptr;
                game_ended(code);
            } else {
                continue;                       /* nothing to draw while the game runs */
            }
        }
        if (g.skip_mode && !g.game) { if (g.quit) break; continue; }
        if (!g.game && g.cap_until > 0 && g.modal == "capture" && now_ms() > g.cap_until) close_modal();
        if (g.modal != "capture") g.cap_until = 0;
        stick_update(pad);
        dialog_poll();
        update_poll();
        install_poll();
        script_step();

        g.ctx->Update();
        if (g.focus_first_row && g.page == "settings" && g.settings && g.modal.empty()) {
            Rml::ElementList list;
            g.settings->QuerySelectorAll(list, ".row, .cell");
            if (!list.empty()) {
                list[0]->Focus(true);
                list[0]->ScrollIntoView(Rml::ScrollAlignment::Nearest);
                update_help(list[0]->GetAttribute<int>("rid", -1));
            }
            g.focus_first_row = false;
            g.ctx->Update();
        }
        if (g.focus_modal && g.dialog) {
            Rml::ElementList list;
            g.dialog->QuerySelectorAll(list, ".sec .mbtn");
            for (Rml::Element *e : list)
                if (e->IsVisible(true)) { e->Focus(true); break; }
            g.focus_modal = false;
            g.ctx->Update();
        }
        render.Clear();
        render.BeginFrame();
        g.ctx->Render();
        render.EndFrame();
        if (!g.shot_pending.empty()) {
            save_shot(g.shot_pending);
            g.shot_pending.clear();
        }
        SDL_GL_SwapWindow(g.window);
        frames++;
        if (first) {
            if (!g.skip_mode && !g.game) {
                SDL_ShowWindow(g.window);
                g.shown = true;
            }
            first = false;
            if (!g.skip_mode) update_start();      /* after the first frame: never in its way */
            if (!g.started_after.empty() && g.modal.empty()) {
                std::string v;
                g.notice_head = (g.started_restored ? "BACK TO v" : "UPDATED TO v") + g.started_after;
                g.notice_text = g.started_restored ? "The previous version is back. Your settings and saves are as they were."
                              : ot_install_has_backup(g.base, v) ? "Your settings and saves are as they were. The previous version (v" + v +
                                                                     ") is kept: Settings \xE2\x80\xBA Advanced \xE2\x80\xBA Restore."
                                                                 : "Your settings and saves are as they were.";
                g.notice_file.clear();
                show_modal("notice");
            }
            otlog("timing sdl_init_ms=%.1f gl_ms=%.1f rmlui_fonts_ms=%.1f disc_theme_ms=%.1f docs_ms=%.1f "
                 "first_frame_ms=%.1f (since main) process_to_first_frame_ms=%.1f",
                 t_sdl - t0, t_gl - t_sdl, t_rml - t_gl, t_disc - t_rml, t_docs - t_disc, now_ms() - t0,
                 ms_since_process_start());
        }
        if (!g.script.empty() && g.script_pos >= g.script.size() && g.script_wait == 0 && g.shot_pending.empty() && !g.game)
            g.quit = true;
        if (quit_after >= 0 && now_ms() - t0 >= quit_after) g.quit = true;
    }
    otlog("frames %d run_ms=%.0f", frames, now_ms() - t0);
#ifdef _WIN32
    {
        FILETIME c, e, k, u;
        PROCESS_MEMORY_COUNTERS pm;
        pm.cb = sizeof pm;
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) &&
            GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm)) {
            ULARGE_INTEGER kk, uu;
            kk.LowPart = k.dwLowDateTime; kk.HighPart = k.dwHighDateTime;
            uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
            otlog("stats cpu_ms=%.0f (user %.0f) peak_working_set_mb=%.1f", (kk.QuadPart + uu.QuadPart) / 1e4,
                  uu.QuadPart / 1e4, pm.PeakWorkingSetSize / 1048576.0);
        }
    }
#endif

    if (g.game) {                               /* closed while the game runs: leave it running */
        SDL_DestroyProcess(g.game);
        g.game = nullptr;
    }
    if (pad) SDL_CloseGamepad(pad);
    g.ctx = nullptr;
    Rml::Shutdown();
    RmlGL3::Shutdown();
    free_hd_cards();
    if (g.have_assets) ot_disc_free(&g.assets);
    SDL_GL_DestroyContext(g.gl);
    SDL_DestroyWindow(g.window);
    SDL_Quit();
    free(g.set);
    return 0;
}
