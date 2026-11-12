/*
 * main.cpp -- OpenTricky.exe, the launcher.
 *
 * RmlUi on SDL3 + OpenGL 3. Three documents written in RML/RCSS (ui/): the
 * home page, the settings page (built from the settings registry,
 * settings.h) and the dialogs shown over them (key capture, crash, about...).
 * The look of a track is read from the player's disc image (port/launcher).
 * Keyboard, mouse and controller (SDL gamepads) all drive the same focus.
 *
 * PLAY writes settings.ini and starts "bin\SSX Tricky.exe" (beside the
 * launcher in an older or a developer's install: ot_program_dir), in the
 * launcher's folder, the install root where settings.ini and saves are;
 * the launcher hides while the game runs and comes back when it ends, with
 * the crash screen if the game left a report (CrashReports\).
 * "Skip launcher next time" starts the game at once; holding Shift (or
 * Back / Select on a controller) while the launcher starts shows it anyway.
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
 *   --find-root <dir>   Find my disc image also looks there, as beside the launcher (tests)
 *   --script <steps>    test steps, comma separated, one every few frames:
 *                       k:<key> (up down left right enter esc tab q e backspace
 *                       space f10 f11 f12 or a letter / digit), p:<button> (a b x y lb rb up down
 *                       left right start back; pushed as SDL controller events),
 *                       click:<id> (an element of the page or dialog),
 *                       mclick:<x>:<y> (the mouse at window pixels, logged;
 *                       mhover:<x>:<y> the same without the click, mdown:<x>:<y>
 *                       the button held down: the pressed look),
 *                       shot:<file.png> (the window's picture), boxes:<file> (with the
 *                       next shot, the layout: save_boxes), wait:<frames>,
 *                       size:<w>x<h> (the window resized, as by a drag),
 *                       disc:<path> (as if chosen; empty: none), drop:<path> (as if
 *                       dropped on the window), focus (the window's focus back
 *                       from another program), find (the list
 *                       of Find my disc image, without the file dialog),
 *                       waitgame (until the game has ended), quit
 *   --log <file>        messages and timings to a file
 *   --quit-after <ms>   close after this long (idle measurements)
 *
 * One launcher per install folder: a second one brings the first to the
 * front and ends at once with exit code 3 (instance_lock).
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
#include <functional>
#include <iterator>
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
/* The default font engine's handle (its glyphs: the capitals' height, layout_tokens). */
#include "../../third_party/rmlui/Source/Core/FontEngineDefault/FontFaceHandleDefault.h"
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

/* A page a build adds to the launcher besides Home and Settings (none in this tree): every hook is optional,
 * nullptr = nothing. The page's own file fills the table before main() runs. */
struct OtPages {
    bool (*owns)(const std::string &page);                 /* the page is its own */
    Rml::ElementDocument *(*doc)();                        /* its document, nullptr when not loaded */
    void (*load_documents)();
    bool (*show_page)(const std::string &page);            /* true: shown by it (the others hidden) */
    bool (*nav)(int n);                                    /* a move on its page; true: taken */
    void (*primary)();                                     /* START on its page */
    bool (*typing)();                                      /* a text field of its page has the keys */
    void (*play_env)(SDL_Environment *env);                /* the game's environment when it starts it */
    void (*game_ended)(int code);
    void (*poll)();                                        /* each frame */
    void (*tick_hidden)();                                 /* each loop while the game runs */
    bool (*arg)(const std::string &a, const std::function<std::string()> &next);   /* a command-line option */
    int  (*texture)(const std::string &name, OtPixels &out);   /* -1: not its picture, else 0 / 1 */
    bool (*script_hold)();                                 /* test scripts: the next step waits */
    bool (*script_step)(const std::string &a, const std::string &b);   /* a test step of its own */
    void (*bind)(Rml::DataModelConstructor &c);            /* its variables in the data model */
    void (*log_line)(const char *line);                    /* each log line */
    void (*shutdown)();
};
OtPages g_pages;

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
    if (g_pages.log_line) g_pages.log_line(buf);
}


std::string hex_rgb(uint32_t argb)
{
    char b[16];
    snprintf(b, sizeof b, "#%06x", argb & 0xFFFFFFu);
    return b;
}

/* A colour pressed: its channels at 0.82 (the launcher's pressed fill). */
std::string pressed_rgb(uint32_t argb)
{
    auto k = [&](int sh) { return (unsigned)std::lround(((argb >> sh) & 255) * 0.82f); };
    char b[16];
    snprintf(b, sizeof b, "#%02x%02x%02x", k(16), k(8), k(0));
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
    A_OPEN_LOGS = 1000, A_BUG_REPORT, A_ABOUT, A_RESTORE, A_CHECK_UPDATES,
};

struct Row {
    int id = -1;
    bool is_group = false;
    Rml::String label, value, button;
    bool changed = false, pending = false;
    bool idle = false;                /* its button has nothing to do for now (dimmed, inert) */
    bool arrows = false, toggle = false, on = false, plain = false;
    Rml::String note;                 /* a short line under the row (empty: none) */
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

/* A disc image found on this computer (Find my disc image). */
struct DiscFound {
    Rml::String name, dir, path;
    bool cur = false;                 /* the one chosen now */
};

struct App {
    SDL_Window *window = nullptr;
    SDL_GLContext gl = nullptr;
    Rml::Context *ctx = nullptr;
    OtRenderInterface *render = nullptr;
    OtFileInterface files;
    std::string base;               /* the exe's folder, with a trailing slash: the install root */
    std::string prog;               /* the game and ui\: base + "bin\", or base (ot_program_dir) */
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
    bool pad_pressed = false;         /* a controller button was pressed in the launcher */
    bool pad_game_ps = false;         /* ... last on a PlayStation controller the game reads (pad_is_ps_game) */
    Rml::String help_title, help_text, help_extra, saved_note;
    Rml::String prev_src;             /* the help panel's picture of the value ("ot:prev/..."; empty: none) */
    Rml::String prev_old;             /* the picture it replaces, under it while it fades in (empty: none) */
    Rml::String prev_label;           /* the value the picture shows, written on it */
    Rml::String ini_note;             /* above the rows: settings.ini was not read as written (empty: it was) */
    bool deck_note = false;           /* ini_note says "Steam Deck detected": until the settings page is left or saved */
    bool help_pending = false, pad_active = false;
    bool have_disc = false;           /* the disc image is verified: PLAY */
    bool have_art = false;            /* and its pictures were read: the track cards, the emblem */
    Rml::String disc_state, disc_title, disc_note, track_name, version_text;
    Rml::String ver_num, ver_note;    /* the version label: "v0.1.4" and its state word (Latest, Available, ...) */
    /* the home page's disc panel (no good disc image) */
    Rml::String disc_kicker, disc_head, disc_text, disc_path, disc_drop1, disc_drop2;
    std::vector<DiscFound> disc_found;   /* the list of the "discs" dialog */
    bool disc_auto = false;           /* chosen at start: the only one beside the launcher */
    std::string find_root;            /* --find-root (tests): searched as if beside the launcher */

    /* What's new */
    Rml::String notes_version, notes_title;
    std::vector<NoteSec> notes;
    OtNotes installed_notes;          /* the block built into the exe */

    /* Is there a newer version (ot_update.h)? */
    Rml::String upd_state;            /* "" = the version alone, "latest", "update" */
    bool upd_view = false;            /* the panel shows the newer version's notes */
    bool upd_have = false;            /* a list of releases was read in time */
    bool upd_started = false;         /* one automatic check per start at most */
    bool upd_manual = false;          /* the player clicked Check now: its answer shows, even with Never */
    bool upd_error = false;           /* the player's last Check now got no answer */
    double upd_fail_until = 0;        /* now_ms(): until then the pill says "Couldn't check" (a Check now with no answer) */
    double upd_last_click = -1e9;     /* now_ms() of the last Check now */
    std::vector<OtRelease> upd_releases;
    std::string upd_skip, upd_url, upd_tag, upd_cache;
    std::string theme_order;       /* theme-order.txt, beside settings.ini (theme_next_track) */
    std::string upd_test_json, upd_test_url;
    /* the "update" dialog (install / restore) */
    Rml::String inst_kick, inst_head, inst_text, inst_phase, inst_s1, inst_s2, inst_s3, inst_l1, inst_l3, inst_foot;
    Rml::String inst_bar = "0%";
    bool inst_cancel = false;         /* the dialog shows Cancel: an update not at its step 3 yet */
    bool inst_busy = false;           /* an update or a restore runs: the version label shows its step and bar */
    Rml::String inst_word;            /* that step, in a word */
    std::string inst_test_fail_at, inst_test_kill_at;
    std::string started_after;        /* "--updated <v>" / "--restored <v>": a notice once shown */
    bool started_restored = false;
    bool recovered = false;
    bool save_warned = false;         /* PLAY said once that settings.ini could not be written */           /* an interrupted install was found at start: the notice is ready */
    double upd_t0 = 0;

    /* dialogs */
    Rml::String modal;                /* "", capture, conflict, crash, about, report_ask, restore_ask, notice */
    Rml::String restore_version;      /* restore_ask: the version Restore puts back */
    Rml::String cap_kind, cap_for, cap_now, cap_hint;
    int cap_ctl = -1, cap_col = 0;
    double cap_until = 0;
    Rml::String conf_kind, conf_line, conf_swap;
    int conf_ctl = -1, conf_col = 0, conf_other = -1;
    std::string conf_value;
    Rml::String crash_head, crash_text, crash_when, crash_version;
    std::string crash_dir;
    Rml::String notice_kick, notice_head, notice_text;
    std::string notice_file;
    bool focus_modal = false;

    /* pages */
    Rml::ElementDocument *home = nullptr, *settings = nullptr, *dialog = nullptr;
    std::string page = "home";
    bool focus_first_row = false;
    bool kbd_nav = false;          /* the last input was the keyboard (pad_active: a controller) */
    bool mouse_click = false;      /* the click being handled comes from the mouse (not Enter / A / a test's click:) */
    float stage_w = 1280;
    bool rescaled = false;         /* the window's scale changed: texts are sized again (fit_window) */
    bool focus_in_view = false;    /* laid out at a new scale: the focus shown is brought back into view */
    int note_in_view = -1;         /* a setting just changed: its row and the note under it, brought into view */

    /* game process */
    SDL_Process *game = nullptr;
    SDL_Time launch_time = 0;
    bool skip_mode = false;           /* started by "Skip launcher": never shown unless a crash */
    bool shown = false;

    /* test script */
    std::vector<std::string> script;
    size_t script_pos = 0;
    int script_wait = 0;
    int games_started = 0;                       /* test step waitrace */
    std::string shot_pending;
    std::string boxes_pending;   /* test: the layout written with the next shot (save_boxes) */
    bool quit = false;
};

App g;

void rebuild_rows();
void update_help(int id);
void update_preview(int id);
Rml::ElementDocument *page_doc();
void show_modal(const char *name);
void close_modal();
void load_disc();
void pick_theme();
void load_documents();

/* ── Values ──────────────────────────────────────────────────────── */

/* The Resolution list: "auto", then the sizes of the chosen Screen shape --
 * the game's own lists per shape (port/src/launcher.c, res_list). Auto: the
 * screen's own size and its shape at the common heights below it. A saved
 * size of another shape stays shown and chosen until the player steps away
 * from it: a change of shape never rewrites the resolution. */
struct ResSize { int w, h; };
const ResSize k_res43[] = { { 640, 480 }, { 960, 720 }, { 1280, 960 }, { 1440, 1080 },
                            { 1920, 1440 }, { 2560, 1920 }, { 2880, 2160 } };
const ResSize k_res169[] = { { 854, 480 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 },
                             { 2560, 1440 }, { 3840, 2160 } };
const ResSize k_res219[] = { { 2560, 1080 }, { 3440, 1440 }, { 3840, 1600 }, { 5120, 2160 } };
const ResSize k_res329[] = { { 3840, 1080 }, { 5120, 1440 }, { 7680, 2160 } };

/* The size in pixels of the screen the game opens on (Monitor). */
void screen_pixels(int &w, int &h)
{
    w = 1920;
    h = 1080;
    int n = 0, m = settings_get_int(g.set, S_MONITOR);
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    SDL_DisplayID id = (ids && m >= 0 && m < n) ? ids[m] : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *dm = id ? SDL_GetDesktopDisplayMode(id) : nullptr;
    if (dm) {
        float k = dm->pixel_density > 0.0f ? dm->pixel_density : 1.0f;
        int pw = (int)(dm->w * k + 0.5f), ph = (int)(dm->h * k + 0.5f);
        if (pw >= 640 && ph >= 480) { w = pw; h = ph; }
    }
    SDL_free(ids);
    /* Test only, as in the game: XBOX_MONITOR_SIZE=3440x1440 stands in for the screen. */
    const char *e = SDL_getenv("XBOX_MONITOR_SIZE");
    int tw, th;
    if (e && SDL_sscanf(e, "%dx%d", &tw, &th) == 2 && tw >= 640 && th >= 480) { w = tw; h = th; }
}

/* That screen's refresh rate in Hz, 0 when it is not known. */
float screen_hz()
{
    int n = 0, m = settings_get_int(g.set, S_MONITOR);
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    SDL_DisplayID id = (ids && m >= 0 && m < n) ? ids[m] : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *dm = id ? SDL_GetDesktopDisplayMode(id) : nullptr;
    float hz = dm ? dm->refresh_rate : 0.0f;
    SDL_free(ids);
    return hz;
}

/* "1920 x 1080" (with a multiplication sign): the screen's size, as the notes name it. */
std::string screen_size_text()
{
    int w, h;
    screen_pixels(w, h);
    return std::to_string(w) + " \xC3\x97 " + std::to_string(h);
}

bool fullscreen_mode()
{
    return SDL_strcasecmp(settings_get(g.set, S_DISPLAY_MODE), "fullscreen") == 0;
}

/* The saved Resolution goes with the Screen shape: "auto", or a size within
 * 0.1 of the shape's ratio -- the game's own rule (aspect.c, aspect_from_env).
 * Otherwise the game shows the shape's picture with black bars (4:3, 16:9) or
 * the resolution's shape under another name (21:9, 32:9): never offered.
 * With the Auto shape the game follows the resolution's own shape: any size
 * in a window (the window has that shape), but in fullscreen a size of
 * another shape than the screen's leaves black bars. */
bool resolution_matches_shape()
{
    static const double k_ratio[] = { 4.0 / 3.0, 16.0 / 9.0, 21.0 / 9.0, 32.0 / 9.0 };
    int w, h, i = settings_choice_index(g.set, S_ASPECT);
    if (!settings_get_resolution(g.set, S_RESOLUTION, &w, &h) || w <= 0 || h <= 0) return true;
    if (i < 0 || i > 3) {
        if (!fullscreen_mode()) return true;
        int mw, mh;
        screen_pixels(mw, mh);
        return std::fabs((double)w / h - (double)mw / mh) < 0.1;
    }
    return std::fabs((double)w / h - k_ratio[i]) < 0.1;
}

/* The shape of the game's picture, as the game works it out
 * (launcher_display_aspect, auto_resolution): 4:3 and 16:9 as named;
 * otherwise the resolution's own shape, and with Resolution Auto the
 * shape's sizes (21:9, 32:9) or the screen's own (Auto). */
double picture_aspect()
{
    int w, h, i = settings_choice_index(g.set, S_ASPECT);
    if (i == 0) return 4.0 / 3.0;
    if (i == 1) return 16.0 / 9.0;
    if (settings_get_resolution(g.set, S_RESOLUTION, &w, &h) && w > 0 && h > 0) return (double)w / h;
    if (i == 2) return 21.0 / 9.0;
    if (i == 3) return 32.0 / 9.0;
    screen_pixels(w, h);
    return (double)w / h;
}

SDL_EnumerationResult SDLCALL find_dds(void *found, const char *, const char *name)
{
    size_t n = SDL_strlen(name);
    if (n > 4 && SDL_strcasecmp(name + n - 4, ".dds") == 0) {
        *(bool *)found = true;
        return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

SDL_EnumerationResult SDLCALL list_folders(void *out, const char *dirname, const char *name)
{
    auto *v = (std::vector<std::string> *)out;
    std::string p = std::string(dirname) + name;
    SDL_PathInfo pi;
    if (SDL_GetPathInfo(p.c_str(), &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) v->push_back(p);
    return v->size() < 32 ? SDL_ENUM_CONTINUE : SDL_ENUM_SUCCESS;
}

/* The last answer of hd_pack_found, for its folder. What the folder holds can
 * change while the launcher is open (the pack unzipped into it after it was
 * chosen, in another program): the answer is forgotten whenever the window
 * gets the focus back (hd_pack_forget), and looked for again. */
struct {
    std::string folder;
    bool found = false, known = false;
} g_hd_pack;

void hd_pack_forget() { g_hd_pack.known = false; }

/* Whether the HD pack folder holds a pack: .dds files in it, or in a
 * "replacements" folder in it or up to two folders below it -- where the
 * pack's archive puts them and the game looks (nv2a_hdtex.c, find_pack_dir;
 * the game then also checks the names). A relative path is relative to the
 * game's folder. Kept for the same folder until hd_pack_forget. */
bool hd_pack_found(const std::string &folder)
{
    if (g_hd_pack.known && folder == g_hd_pack.folder) return g_hd_pack.found;
    std::string d = folder;
    bool absolute = d.size() > 1 && (d[1] == ':' || d[0] == '/' || d[0] == '\\');
    if (!absolute) d = g.base + d;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    bool found = false;
    SDL_EnumerateDirectory(d.c_str(), find_dds, &found);
    std::vector<std::string> level = { d };
    for (int depth = 0; depth < 3 && !found; depth++) {
        for (const std::string &f : level)
            if (!found) SDL_EnumerateDirectory((f + "/replacements").c_str(), find_dds, &found);
        std::vector<std::string> next;
        if (depth < 2)
            for (const std::string &f : level) SDL_EnumerateDirectory(f.c_str(), list_folders, &next);
        level.swap(next);
    }
    g_hd_pack.folder = folder;
    g_hd_pack.found = found;
    g_hd_pack.known = true;
    return found;
}

/* The short line under a row when, with the other settings or this PC, its
 * value does not do what it says (or, for VSync Off, what it costs). Shown
 * only: the value stays as it is. */
std::string row_note(int id)
{
    switch (id) {
    case S_RESOLUTION:
        /* a file saved that way (by hand, an older launcher): shown, not rewritten */
        if (resolution_matches_shape()) return "";
        if (settings_choice_index(g.set, S_ASPECT) > 3)
            return "Doesn't match your screen (" + screen_size_text() + "): pick a size or Auto.";
        return "Doesn't match the screen shape: pick a size or Auto.";
    case S_ASPECT: {
        int i = settings_choice_index(g.set, S_ASPECT), mw, mh;
        if (i < 0 || i > 3 || !fullscreen_mode()) return "";
        screen_pixels(mw, mh);
        if (std::fabs(picture_aspect() - (double)mw / mh) < 0.1) return "";
        return "Your screen is " + screen_size_text() + ": black bars around the picture.";
    }
    case S_SHADOW_SOFTNESS:
        return settings_get_bool(g.set, S_SOFT_SHADOWS) ? "" : "No effect with Soft shadows off.";
    case S_MENUS: {
        double a = picture_aspect();
        if (a <= 4.0 / 3.0 + 0.005) return "No effect at 4:3.";
        if (settings_choice_index(g.set, S_MENUS) == 1 && a < 16.0 / 9.0 - 0.005)
            return "Squeezed on this screen.";
        return "";
    }
    case S_FOV:
        return picture_aspect() <= 16.0 / 9.0 + 0.005 ? "No effect up to 16:9." : "";
    case S_FPS_LIMIT: {
        /* with VSync the game lowers a higher limit to the screen's rate (fps_cap.c) */
        int sync = settings_choice_index(g.set, S_VSYNC), lim = settings_choice_index(g.set, S_FPS_LIMIT);
        float hz = screen_hz();
        if ((sync != 0 && sync != 1) || lim <= 0 || hz < 1.0f) return "";
        const char *v = settings_get(g.set, S_FPS_LIMIT);
        int fps = SDL_strcasecmp(v, "unlimited") == 0 ? 100000 : atoi(v);
        int whole = (int)(hz + 0.5f);
        if (fps <= whole) return "";
        return "Your screen runs at " + std::to_string(whole) + " Hz: with VSync the game shows at most " +
               std::to_string(whole) + ".";
    }
    case S_VSYNC:
        /* Off: each frame goes to the screen as soon as it is drawn, not on a refresh,
         * so frames reach it unevenly and the picture tears. A warning only: the value
         * stays the player's. On is every preset's value. */
        if (SDL_strcasecmp(settings_get(g.set, S_VSYNC), "off") != 0) return "";
        return "Off: uneven motion and tearing. On is recommended: it adds very little input delay.";
    case S_PS_CONTROLLERS:
        return "DualShock 4 and DualSense only.";
    case S_SHOW_FPS:
        if (settings_get_bool(g.set, S_SHOW_FPS) && fullscreen_mode())
            return "Not seen in fullscreen: the title bar shows only in a window.";
        return "";
    case S_HD_HUD:
        /* the files the game ships (nv2a_hdtex.c, "The shipped HD HUD"); the game
         * also checks each one and keeps the original HUD for a damaged one */
        if (!settings_get_bool(g.set, S_HD_HUD)) return "";
        for (const char *f : { "map1.dds", "map4.dds", "title.dds", "menu.dds" })
            if (!SDL_GetPathInfo((g.prog + "hd-hud/" + f).c_str(), nullptr))
                return "The HD HUD files are missing: the original display is shown.";
        return "";
    case S_HD_TEXTURES: {
        if (settings_choice_index(g.set, S_HD_TEXTURES) <= 0) return "";
        const char *dir = settings_get(g.set, S_HD_PACK_FOLDER);
        if (!*dir) return "Choose an HD pack folder.";
        if (!hd_pack_found(dir)) return "No HD pack in this folder.";
        return "";
    }
    case S_UPDATE_CHANNEL:
        return SDL_strcasecmp(settings_get(g.set, S_CHECK_UPDATES), "never") == 0 ? "Applies to Check now only." : "";
    default: return "";
    }
}

/* The line above the rows when settings.ini was not read as written
 * (settings_load's answer); gone once the player saves. */
std::string ini_note_text(int status, int bad)
{
    switch (status) {
    case SETTINGS_LOAD_NOT_V1:
        return "Your settings file couldn't be read. Defaults are used until you save.";
    case SETTINGS_LOAD_NEWER:
        return "Settings from a newer OpenTricky: unknown ones are kept.";
    case SETTINGS_LOAD_ERROR:
        return "Your settings file couldn't be read. Defaults are used.";
    case SETTINGS_LOAD_OK:
        if (bad <= 0) return "";
        return "Some settings couldn't be read and use their default.";
    default: return "";
    }
}

/* No settings.ini yet: on a Steam Deck (under Proton) the launcher starts
 * from the Steam Deck preset instead of the defaults (settings.h), writes it
 * at once and says so above the settings rows, once. Elsewhere, nothing. */
void first_start_preset()
{
    SettingsPlatform p;
    char why[200];
    settings_platform_probe(&p, nullptr);
    if (settings_first_start_preset(&p, why, sizeof why) != SETTINGS_PRESET_STEAM_DECK) {
        if (p.wine) otlog("settings: first start under Wine, not a Steam Deck (%s): defaults", why);
        return;
    }
    settings_apply_preset(g.set, SETTINGS_PRESET_STEAM_DECK);
    otlog("settings: first start on a Steam Deck (%s): Steam Deck preset", why);
    g.ini_note = "Steam Deck preset applied.";
    g.deck_note = true;
}

std::vector<std::string> resolution_list()
{
    std::vector<ResSize> sizes;
    switch (settings_choice_index(g.set, S_ASPECT)) {
    case 0: sizes.assign(std::begin(k_res43), std::end(k_res43)); break;
    case 1: sizes.assign(std::begin(k_res169), std::end(k_res169)); break;
    case 2: sizes.assign(std::begin(k_res219), std::end(k_res219)); break;
    case 3: sizes.assign(std::begin(k_res329), std::end(k_res329)); break;
    default: {
        static const int heights[] = { 720, 1080, 1200, 1440, 2160 };
        int mw, mh;
        screen_pixels(mw, mh);
        for (int hh : heights)
            if (hh < mh) sizes.push_back({ ((int)((double)hh * mw / mh + 0.5) + 1) & ~1, hh });
        sizes.push_back({ mw, mh });
        break;
    }
    }
    int cw, ch;
    if (settings_get_resolution(g.set, S_RESOLUTION, &cw, &ch)) {
        bool listed = false;
        for (const ResSize &r : sizes) listed = listed || (r.w == cw && r.h == ch);
        if (!listed) {          /* the saved size, in its place by area */
            size_t at = 0;
            while (at < sizes.size() && (long long)sizes[at].w * sizes[at].h <= (long long)cw * ch) at++;
            sizes.insert(sizes.begin() + (long)at, { cw, ch });
        }
    }
    std::vector<std::string> out = { "auto" };
    for (const ResSize &r : sizes) out.push_back(std::to_string(r.w) + "x" + std::to_string(r.h));
    return out;
}

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
        return (ot_file_name(v).empty() ? std::string(v) : ot_file_name(v)) + " \xC2\xB7 Change\xE2\x80\xA6";
    case SETTING_INT:
        if (id == S_MONITOR) return monitor_label(atoi(v));
        return v;
    default: return v;
    }
}

void update_settings_changed();
void update_action(const std::string &a);
void update_start(bool manual = false);
std::string update_row_value();
std::string update_row_button(bool *idle);
bool update_just_checked();
void update_row_refresh();
void install_start(bool restore);
bool install_running();
bool install_cancel();

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
        std::vector<std::string> list = resolution_list();
        int n = (int)list.size(), i = 0, cw, ch;
        if (settings_get_resolution(g.set, id, &cw, &ch)) {
            std::string cur = std::to_string(cw) + "x" + std::to_string(ch);
            for (int k = 1; k < n; k++)
                if (list[k] == cur) i = k;
        }
        i = (i + dir + n) % n;
        settings_set(g.set, id, list[i].c_str());
        break;
    }
    default: return;
    }
    /* A new shape never keeps a size of another shape: the player's change,
     * so it is written at Save like any setting. */
    if (id == S_ASPECT && !resolution_matches_shape()) settings_set(g.set, S_RESOLUTION, "auto");
    settings_changed(id);
    g.note_in_view = id;   /* a note the new value brings under the row is seen, not left below the list's edge */
}

bool save_settings_file()
{
    if (g.ini_write.empty() || !settings_save(g.set, g.ini_write.c_str())) {
        otlog("settings: could not write %s", g.ini_write.c_str());
        return false;
    }
    otlog("settings written");
    if (!g.ini_note.empty() && !g.deck_note) {   /* the file is now as this version writes it */
        g.ini_note = "";
        if (g.model) g.model.DirtyVariable("ini_note");
    }
    return true;
}

/* The "Steam Deck detected" note is seen once: gone when the player leaves
 * the settings page or saves there. */
void deck_note_seen()
{
    if (!g.deck_note) return;
    g.deck_note = false;
    g.ini_note = "";
    if (g.model) g.model.DirtyVariable("ini_note");
}

/* The theme's track. Plain chance came back to the same colours (three
 * tracks are orange), so the ten tracks are dealt like a shuffled playlist:
 * all ten before any comes back, never the same one twice in a row, nor two
 * close colours one after the other (the oranges: Alaska, Mesablanca,
 * Untracked; the blues: Garibaldi, Merqury City). theme-order.txt, beside
 * settings.ini, holds the last one shown, then what is left of the deal
 * ("3;7,1,..."); an unreadable file is a new deal. A folder that can't be
 * written: a plain draw at each start. */
int theme_group(int t)
{
    if (t == OT_TRACK_ALAS || t == OT_TRACK_MESA || t == OT_TRACK_UNTR) return 1;
    if (t == OT_TRACK_GARI || t == OT_TRACK_MERQ) return 2;
    return 0;
}

bool theme_close(int a, int b) { return a == b || (theme_group(a) && theme_group(a) == theme_group(b)); }

int theme_next_track()
{
    std::mt19937 rng(std::random_device{}());
    std::vector<int> nums;
    std::string order;          /* through SDL's file calls: the path is UTF-8, any folder name works */
    if (!g.theme_order.empty() && ot_read_file(g.theme_order, order, 4096)) {
        int v = -1;
        for (size_t i = 0; i < order.size() && nums.size() < 2 * OT_TRACK_COUNT; i++) {
            char c = order[i];
            if (c >= '0' && c <= '9') v = std::min((v < 0 ? 0 : v * 10) + (c - '0'), 1000);
            else if (v >= 0) { nums.push_back(v); v = -1; }
        }
        if (v >= 0 && nums.size() < 2 * OT_TRACK_COUNT) nums.push_back(v);
    }
    int last = nums.empty() || nums[0] >= OT_TRACK_COUNT ? -1 : nums[0];
    std::vector<int> bag(nums.size() > 1 ? nums.begin() + 1 : nums.end(), nums.end());
    bool seen[OT_TRACK_COUNT] = {};
    for (int t : bag) {
        if (t >= OT_TRACK_COUNT || seen[t] || t == last) { bag.clear(); break; }
        seen[t] = true;
    }
    if (bag.empty()) {
        for (int t = 0; t < OT_TRACK_COUNT; t++) bag.push_back(t);
        for (int tries = 0; tries < 500; tries++) {   /* about one deal in four fits: never 500 misses */
            std::shuffle(bag.begin(), bag.end(), rng);
            bool ok = last < 0 || !theme_close(bag[0], last);
            for (size_t i = 1; ok && i < bag.size(); i++) ok = !theme_close(bag[i - 1], bag[i]);
            if (ok) break;
        }
    }
    int t = bag.front();
    bag.erase(bag.begin());
    std::string line = std::to_string(t) + ";";
    for (size_t i = 0; i < bag.size(); i++) line += (i ? "," : "") + std::to_string(bag[i]);
    line += '\n';
    bool kept = !g.theme_order.empty() && SDL_SaveFile(g.theme_order.c_str(), line.data(), line.size());
    if (!kept) t = (int)(rng() % OT_TRACK_COUNT);   /* nothing remembered: a plain draw */
    otlog("theme: track %d (%s)", t, kept ? "theme-order.txt" : "a plain draw");
    return t;
}

/* The disc image chosen: it loads the documents again (load_documents), so
 * never from inside one of their events (a choice made there waits for the
 * main loop: dialog_chosen). */
void set_disc(const std::string &path)
{
    g.disc_auto = false;
    settings_set(g.set, S_DISC_IMAGE, path.c_str());
    g.iso = path;
    load_disc();
    /* No verified disc any more: drop every picture made from the previous
     * one (cards, emblems) from the texture cache too, not only from view. */
    if (!g.have_assets) Rml::ReleaseTextures();
    if (g.have_assets) g.track = theme_next_track();
    update_preview(g.focus_id);       /* a setting's picture only with a verified disc */
    pick_theme();
    save_settings_file();             /* the launcher remembers the disc at once */
    rebuild_rows();
    g.model.DirtyAllVariables();
    load_documents();
}

/* A path chosen in a dialog waits here and the main loop applies it
 * (dialog_poll), before the next input:
 * - the file / folder dialogs answer on SDL's dialog thread (SDL_dialog.h:
 *   "the callback may be called from a different thread"), never the main
 *   loop's own;
 * - a disc image of Find my disc image is chosen in a click on its line, an
 *   event of the dialog's document: set_disc loads the documents again
 *   (load_documents), and RmlUi would free the line's click handler while it
 *   runs. */
std::mutex g_dialog_mutex;
int g_dialog_id = -1;
std::string g_dialog_path;

void dialog_chosen(int id, const std::string &path)
{
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    g_dialog_id = id;
    g_dialog_path = path;
}

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
        dialog_chosen(s_id, list[0]);
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

/* ── Find my disc image ──────────────────────────────────────────── */

OtDiscCheck disc_check(const std::string &iso, bool *other_build, bool *bad_path);

/* The .iso / .xiso files under `dir` (with its trailing separator), `depth`
 * folder levels deep (0: that folder only), dot folders skipped; `seen`
 * caps the entries looked at. */
void find_iso_files(const std::string &dir, int depth, int &seen, std::vector<std::string> &out)
{
    struct Ctx {
        int depth, *seen;
        std::vector<std::string> *out, sub;
    } c{ depth, &seen, &out, {} };
    SDL_EnumerateDirectory(dir.c_str(), [](void *u, const char *d, const char *name) -> SDL_EnumerationResult {
        Ctx *c = (Ctx *)u;
        if (++*c->seen > 20000 || c->out->size() >= 64) return SDL_ENUM_SUCCESS;
        if (name[0] == '.' || name[0] == '$') return SDL_ENUM_CONTINUE;
        std::string p = std::string(d) + name;
        SDL_PathInfo pi;
        if (!SDL_GetPathInfo(p.c_str(), &pi)) return SDL_ENUM_CONTINUE;
        if (pi.type == SDL_PATHTYPE_DIRECTORY && c->depth > 0) c->sub.push_back(p + "\\");
        else if (pi.type == SDL_PATHTYPE_FILE && (ends_with_ci(p, ".iso") || ends_with_ci(p, ".xiso"))) c->out->push_back(p);
        return SDL_ENUM_CONTINUE;
    }, &c);
    for (const std::string &sd : c.sub) find_iso_files(sd, depth - 1, seen, out);
}

/* Under Wine (Proton, a Steam Deck), the Linux side through Z:\: each home
 * folder, and the SD cards and USB drives under /run/media. */
void wine_roots(std::vector<std::pair<std::string, int>> &roots)
{
    SettingsPlatform p;
    settings_platform_probe(&p, nullptr);
    if (!p.wine) return;
    for (const char *top : { "Z:\\home\\", "Z:\\run\\media\\" }) {
        struct Ctx {
            int depth;
            std::vector<std::pair<std::string, int>> *roots;
        } c{ top[3] == 'h' ? 2 : 3, &roots };   /* /home/deck/...; /run/media/deck/<card>/... or /run/media/<card>/... */
        SDL_EnumerateDirectory(top, [](void *u, const char *d, const char *name) -> SDL_EnumerationResult {
            Ctx *c = (Ctx *)u;
            if (name[0] != '.') c->roots->push_back({ std::string(d) + name + "\\", c->depth });
            return SDL_ENUM_CONTINUE;
        }, &c);
    }
}

/* The SSX Tricky (USA) disc images on this computer that the game can open,
 * checked as a chosen one is (disc_check): beside the launcher (and one
 * folder down), in Downloads, Desktop and Documents (two folders down), and
 * under Wine in the Linux home folders and on the SD card. `beside_only`:
 * beside the launcher only (the choice at start). */
std::vector<DiscFound> find_discs(bool beside_only)
{
    double t0 = now_ms();
    std::vector<std::pair<std::string, int>> roots = { { g.base, 1 } };
    if (!g.find_root.empty()) roots.push_back({ g.find_root, 1 });    /* tests */
    if (!beside_only) {
        for (SDL_Folder f : { SDL_FOLDER_DOWNLOADS, SDL_FOLDER_DESKTOP, SDL_FOLDER_DOCUMENTS })
            if (const char *p = SDL_GetUserFolder(f)) roots.push_back({ p, 2 });
        wine_roots(roots);
    }
    std::vector<std::string> files, keys;
    int seen = 0, wrong = 0;
    for (const auto &r : roots) find_iso_files(r.first, r.second, seen, files);
    std::vector<DiscFound> out;
    auto path_key = [](std::string k) {          /* one spelling of a path: lower case, backslashes */
        for (char &ch : k) ch = ch == '/' ? '\\' : (char)tolower((unsigned char)ch);
        return k;
    };
    const std::string cur = path_key(g.iso);
    for (const std::string &f : files) {
        std::string key = path_key(f);
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;   /* a folder reached twice */
        keys.push_back(key);
        if (out.size() >= 8) break;                /* what the dialog shows */
        if (disc_check(f, nullptr, nullptr).status != OT_DISC_OK) { wrong++; continue; }
        DiscFound d;
        d.path = f;
        d.name = ot_file_name(f);
        d.dir = ot_dir_of(f);
        d.cur = key == cur;
        out.push_back(d);
    }
    otlog("find discs: %d folder(s), %d entries, %d .iso, %d SSX Tricky (USA), %d other (%.1f ms)", (int)roots.size(), seen,
          (int)keys.size(), (int)out.size(), wrong, now_ms() - t0);
    return out;
}

/* The disc pill and the disc panel: the disc images found, as buttons (a
 * controller is enough), or the file dialog at once when none is found.
 * Browse... in the list opens the file dialog. */
void choose_disc()
{
    g.disc_found = find_discs(false);
    if (g.disc_found.empty()) {
        choose_path(S_DISC_IMAGE);
        return;
    }
    g.model.DirtyVariable("disc_found");
    show_modal("discs");
}

/* No disc image chosen yet, and one only beside the launcher (a player who
 * put it next to OpenTricky): that one, remembered as a chosen one. */
void auto_pick_disc()
{
    std::vector<DiscFound> d = find_discs(true);
    if (d.size() != 1) return;
    g.iso = d[0].path;
    settings_set(g.set, S_DISC_IMAGE, g.iso.c_str());
    g.disc_auto = true;
    otlog("disc: none chosen yet, the only one beside the launcher is taken: %s", d[0].name.c_str());
}

/* A file dropped on the window: a disc image is taken as if chosen. Another
 * file (often the archive the disc image came in) is said, over the page or
 * the Find my disc image list (which invites the drop), never over another
 * dialog; the disc image chosen before stays. */
void file_dropped(const std::string &path)
{
    if (ends_with_ci(path, ".iso") || ends_with_ci(path, ".xiso")) {
        otlog("disc image dropped");
        set_disc(path);
        return;
    }
    otlog("dropped: %s is not a disc image (.iso, .xiso)", ot_file_name(path).c_str());
    if (!g.modal.empty() && g.modal != "discs") return;
    g.notice_kick = "DISC IMAGE";
    g.notice_head = "THIS ISN'T A DISC IMAGE";
    g.notice_text = "OpenTricky needs the disc image file itself (.iso or .xiso). If it is inside an archive "
                    "(.zip, .7z, .rar), extract it first, then drop the file you get.";
    g.notice_file = "";
    show_modal("notice");
}

/* The window has the focus again, back from another program (where the HD
 * pack may have been unzipped meanwhile): the rows' notes that read the disk
 * are worked out again. */
void focus_back()
{
    hd_pack_forget();
    if (g.page == "settings") rebuild_rows();
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

/* A controller binding being captured: every button of the controller is a
 * binding, so none of them can cancel; after this long the capture cancels
 * itself (Esc and the dialog's buttons cancel at once). */
const int k_pad_capture_s = 8;

void capture_start(int k, int col)
{
    if (k < 0 || k >= SETTINGS_KEY_CONTROLS || (col == 1 && k >= SETTINGS_PAD_CONTROLS)) return;
    g.cap_ctl = k;
    g.cap_col = col;
    g.cap_kind = col == 0 ? "CONTROLS \xC2\xB7 KEYBOARD" : "CONTROLS \xC2\xB7 CONTROLLER";
    g.cap_for = settings_def(S_KEY_FIRST + k)->label;
    g.cap_now = col == 0 ? settings_get(g.set, ctl_id(k, 0)) : pad_label(settings_get(g.set, ctl_id(k, 1)));
    /* A keyboard key: the title says it, a refusal goes here. A controller
     * button: how to cancel, since no button of the controller can. */
    g.cap_hint = col == 1 ? "Any controller button you press is taken. To cancel, press Esc or wait " + std::to_string(k_pad_capture_s) + " s."
                          : "";
    g.cap_until = col == 1 ? now_ms() + k_pad_capture_s * 1000.0 : 0;
    show_modal("capture");
}

/* Cross and Start (CTL_A, CTL_START in controls.h) always keep a key on the
 * keyboard: without them a player with no controller cannot confirm in the
 * menus or pause. Why `value` cannot be taken for control k, or "". */
const int k_ctl_cross = 0, k_ctl_start = 8;

std::string capture_refusal(int k, int col, const std::string &value)
{
    if (col != 0) return "";
    /* the game window's own keys: they would do both things at once */
    if (value == "F10" || value == "F11" || value == "F12")
        return "F10 to F12 are used by the game. Press another key.";
    auto essential = [](int c) { return c == k_ctl_cross || c == k_ctl_start; };
    if (SDL_strcasecmp(value.c_str(), "None") == 0)
        return essential(k) ? std::string(settings_def(S_KEY_FIRST + k)->label) + " needs a key: press one, or Esc to keep it."
                            : "";
    if (SDL_strcasecmp(settings_get(g.set, ctl_id(k, 0)), "None") != 0) return "";
    /* this control has no key yet: a swap would leave the other one without */
    for (int j = 0; j < SETTINGS_KEY_CONTROLS; j++) {
        if (j == k || !essential(j) || SDL_strcasecmp(settings_get(g.set, ctl_id(j, 0)), value.c_str()) != 0) continue;
        const char *other = settings_def(S_KEY_FIRST + j)->label;
        return value + " is " + other + "'s key, and " + other + " needs one: press another key.";
    }
    return "";
}

/* The new binding, unless another control uses it: then the conflict dialog,
 * Swap or Cancel. No key is shared by two controls (they would be pressed
 * together), except a D-pad direction and the left stick's, by default. */
void capture_assign(const std::string &value)
{
    int k = g.cap_ctl, col = g.cap_col;
    std::string refused = k < 0 ? "" : capture_refusal(k, col, value);
    if (!refused.empty()) {                     /* the dialog stays, saying why */
        g.cap_hint = refused;
        g.model.DirtyVariable("cap_hint");
        return;
    }
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
            show_modal("conflict");
            return;
        }
    }
    settings_set(g.set, id, value.c_str());
    ctl_changed();
}

void conflict_answer(bool swap)   /* Swap, or Cancel */
{
    close_modal();
    if (!swap || g.conf_ctl < 0) return;
    int me = ctl_id(g.conf_ctl, g.conf_col), other = ctl_id(g.conf_other, g.conf_col);
    std::string old = settings_get(g.set, me);
    settings_set(g.set, me, g.conf_value.c_str());
    settings_set(g.set, other, old.c_str());
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
        r.note = row_note(id);
        g.rows.push_back(r);
        if (id == S_UPDATE_CHANNEL) {
            { bool idle; add_action_row(A_CHECK_UPDATES, "Updates", update_row_value().c_str(), update_row_button(&idle).c_str()); g.rows.back().idle = idle; }
            std::string v;
            if (ot_install_has_backup(g.base, v)) add_action_row(A_RESTORE, "Previous version", ("v" + v).c_str(), "Restore");
        }
        if (id == S_LOG_FILE) {
            add_action_row(A_OPEN_LOGS, "Logs", nullptr, "Open");
            add_action_row(A_BUG_REPORT, "Bug report", nullptr, "Prepare\xE2\x80\xA6");
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

void set_help(const char *title, const std::string &text, const std::string &extra)
{
    g.help_title = title;
    g.help_text = text;
    g.help_extra = extra;
    g.help_pending = false;
}

void dirty_help()
{
    if (!g.model) return;
    for (const char *v : { "help_title", "help_text", "help_extra", "help_pending", "prev_src",
                           "prev_old", "prev_label" })
        g.model.DirtyVariable(v);
}

/* ── Setting previews ──
 * A picture at the top of the help panel that shows the chosen value of a
 * graphics setting: ui/previews/<Key>-<value>.png (PNG, 688 x 388). Same
 * rule as the HD cards: only with a verified disc image, read when shown;
 * without it there is no picture and no empty place. A value without its
 * own picture shows the nearest value's, and none at all shows nothing. */
const int k_preview_ids[] = { S_RENDER_SCALE, S_TEX_FILTER, S_SMAA, S_SOFT_SHADOWS, S_SHADOW_SOFTNESS, S_DRAW_DISTANCE,
                              S_LIGHTMAP_SMOOTH, S_SNOW_LOOK, S_WEATHER, S_HD_HUD };

bool previews_allowed() { return g.have_assets && g.disc.status == OT_DISC_OK; }

std::string preview_path(const std::string &name)
{
    return g.prog + "ui/previews/" + name + ".png";
}

/* "<Key>-<value>" for setting id at its current value, or "" (no picture);
 * label: the shown value's name, as the row writes it. */
std::string preview_name(int id, std::string *label = nullptr)
{
    if (!previews_allowed() || std::find(std::begin(k_preview_ids), std::end(k_preview_ids), id) == std::end(k_preview_ids))
        return "";
    const SettingDef *d = settings_def(id);
    std::vector<std::string> tokens, labels;
    int cur = -1;
    if (d->type == SETTING_BOOL) {
        tokens = { "0", "1" };
        labels = { "Off", "On" };
        cur = settings_get_bool(g.set, id) ? 1 : 0;
    } else if (d->type == SETTING_CHOICE) {
        for (int i = 0; i < d->nchoices; i++) {
            tokens.push_back(d->choices[i].token);
            labels.push_back(d->choices[i].label);
        }
        cur = settings_choice_index(g.set, id);
    }
    if (cur < 0) return "";
    int n = (int)tokens.size();
    for (int dist = 0; dist < n; dist++)
        for (int side : { 1, -1 }) {
            int j = cur + side * dist;
            if (j < 0 || j >= n || (dist == 0 && side < 0)) continue;
            std::string name = std::string(d->key) + "-" + tokens[j];
            if (SDL_GetPathInfo(preview_path(name).c_str(), nullptr)) {
                if (label) *label = labels[j];
                return name;
            }
        }
    return "";
}

/* A preview's pixels, for make_texture ("prev/<Key>-<value>"). */
bool load_preview(const std::string &name, int w, int h, OtPixels &out)
{
    if (!previews_allowed() || name.empty() ||
        name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.") != std::string::npos ||
        name.find("..") != std::string::npos)
        return false;
    SDL_Surface *s = SDL_LoadPNG(preview_path(name).c_str());
    if (!s) return false;
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);   /* = OtImage's 0xAARRGGBB */
    SDL_DestroySurface(s);
    if (!c) return false;
    OtImage im{}, at{};
    bool ok = ot_image_alloc(&im, c->w, c->h);
    for (int y = 0; ok && y < c->h; y++)
        memcpy(&im.px[(size_t)y * c->w], (unsigned char *)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
    SDL_DestroySurface(c);
    if (!ok) return false;
    /* at the size it is drawn (a Lanczos filter), else as it is */
    if (w > 0 && h > 0 && (w != im.w || h != im.h) && ot_image_resize(&im, w, h, &at)) {
        ot_image_free(&im);
        im = at;
    }
    image_to_pixels(im, out);
    ot_image_free(&im);
    return true;
}

/* A new picture fades in over the one it replaces (a short cross-fade, as
 * soon as the value changes); one appearing on its own is shown at once. */
void update_preview(int id)
{
    std::string label, name = preview_name(id, &label);
    Rml::String src = name.empty() ? "" : "ot:prev/" + name;
    if (src == g.prev_src) return;
    g.prev_old = (!src.empty() && !g.prev_src.empty()) ? g.prev_src : "";
    g.prev_src = src;
    g.prev_label = label;
    Rml::ElementDocument *d = page_doc();
    Rml::Element *e = d ? d->GetElementById("prevnew") : nullptr;
    if (e && !g.prev_old.empty()) {
        const Rml::Property from(0.0f, Rml::Unit::NUMBER);
        e->Animate("opacity", Rml::Property(1.0f, Rml::Unit::NUMBER), 0.18f,
                   Rml::Tween{Rml::Tween::Quadratic, Rml::Tween::Out}, 1, false, 0.0f, &from);
    }
}

void update_help(int id)
{
    g.focus_id = id;
    update_preview(id);
    if (id >= 2000 && id < 2000 + 2 * SETTINGS_KEY_CONTROLS) {     /* a cell of the remapping table */
        int k = (id - 2000) / 2, col = (id - 2000) % 2;
        set_help(settings_def(S_KEY_FIRST + k)->label,
                 std::string("Select it, then press the new ") + (col == 0 ? "key" : "button") + ". Esc cancels, Backspace clears.",
                 k >= 12 && k <= 19 && col == 0 ? "The D-pad and the left stick may share a key." : "");
        dirty_help();
        return;
    }
    switch (id) {
    case A_OPEN_LOGS:
        set_help("Logs", "Opens the folder with the game's log and your settings file.", "");
        dirty_help();
        return;
    case A_BUG_REPORT:
        set_help("Bug report", "Saves a zip on your Desktop with the logs, your settings and the last crash report. Nothing is sent. Attach it to a GitHub issue.", "");
        dirty_help();
        return;
    case A_CHECK_UPDATES:
        set_help("Updates", "Looks for a new version now (Check now), even when automatic checks are off.", "");
        dirty_help();
        return;
    case A_RESTORE:
        set_help("Restore previous version", "Puts back the version you had before the last update. Your settings and saves stay.", "");
        dirty_help();
        return;
    case A_ABOUT:
        set_help("About OpenTricky", "The version and credits, and the licences of OpenTricky and of the third-party code in the launcher and the game.", "");
        dirty_help();
        return;
    default: break;
    }
    if (id < 0 || id >= SETTINGS_COUNT) { dirty_help(); return; }
    const SettingDef *d = settings_def(id);
    g.help_title = d->label;
    g.help_text = d->help ? d->help : "";
    g.help_pending = (d->flags & SETTING_PENDING) != 0;
    g.help_extra = "";
    if (id == S_HD_TEXTURES || id == S_HD_PACK_FOLDER) g.help_extra = "HD textures by Bl4ckH4nd (not included).";
    if (id == S_CHECK_UPDATES || id == S_UPDATE_CHANNEL)
        g.help_extra = "Nothing about you is sent.";
    if (id == S_SAVE_PROFILE)
        g.help_extra = "Your own save is never changed.";
    if (id == S_SKIP_LAUNCHER) g.help_extra = "To see the launcher again, hold Shift (or the Back / Select button on a controller) while it starts.";
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
    g.files.tokens["$acc_press$"] = pressed_rgb(t.acc);
    g.files.tokens["$acc_dark_press$"] = pressed_rgb(t.acc_dark);
    g.files.tokens["$acc_light_press$"] = pressed_rgb(t.acc_light);
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

/* The size in pixels an "ot:" picture is drawn at: the box of a picture element
 * that shows it, in the pages loaded. False when none shows it, or before its
 * first layout (RmlUi asks for a picture's size while laying it out: it is then
 * made at its own size, and again at the drawn size by fit_sized_pictures). */
bool drawn_size(const std::string &src, int &w, int &h)
{
    Rml::ElementDocument *docs[] = { g.home, g.settings, g.dialog, g_pages.doc ? g_pages.doc() : nullptr };
    for (Rml::ElementDocument *d : docs) {
        if (!d) continue;
        Rml::ElementList list;
        d->GetElementsByTagName(list, "img");
        for (Rml::Element *e : list) {
            if (e->GetAttribute<Rml::String>("src", "") != src) continue;
            Rml::Vector2f z = e->GetBox().GetSize(Rml::BoxArea::Content);
            w = (int)std::lround(z.x);
            h = (int)std::lround(z.y);
            if (w > 0 && h > 0) return true;
        }
    }
    return false;
}

/* The pictures made at the size they are drawn (make_texture): made again at a new scale. */
bool sized_picture(const std::string &name)
{
    return !name.compare(0, 5, "icon/") || !name.compare(0, 7, "emblem/") || !name.compare(0, 6, "badge/") ||
           name == "otbadge" || !name.compare(0, 5, "prev/");
}

/* After each layout: a picture made at its drawn size whose box is not that
 * size (its first layout, a new scale) is let go, and made again at the next
 * draw, at the box's size (within a pixel: the card's height follows its width). */
void fit_sized_pictures()
{
    Rml::ElementDocument *docs[] = { g.home, g.settings, g.dialog, g_pages.doc ? g_pages.doc() : nullptr };
    for (Rml::ElementDocument *d : docs) {
        if (!d || !d->IsVisible()) continue;
        Rml::ElementList list;
        d->GetElementsByTagName(list, "img");
        for (Rml::Element *e : list) {
            Rml::String src = e->GetAttribute<Rml::String>("src", "");
            if (src.compare(0, 3, "ot:") != 0 || !sized_picture(src.substr(3))) continue;
            Rml::Vector2f z = e->GetBox().GetSize(Rml::BoxArea::Content), t;
            float ratio = 0;
            if (z.x < 1 || z.y < 1 || !e->GetIntrinsicDimensions(t, ratio) || t.x < 1) continue;
            if (std::fabs(t.x - std::round(z.x)) > 1 || std::fabs(t.y - std::round(z.y)) > 1) Rml::ReleaseTexture(src);
        }
    }
}

bool make_texture(const std::string &name, OtPixels &out)
{
    /* Names carry the track ("emblem/7") so a new track never reuses a picture. */
    std::string kind = name.substr(0, name.find('/'));
    int track = name.find('/') != std::string::npos ? atoi(name.c_str() + name.find('/') + 1) : g.track;
    OtImage im{};
    bool ok = false;
    if (g_pages.texture) {                      /* a picture of an extra page */
        int r = g_pages.texture(name, out);
        if (r >= 0) return r > 0;
    }
    /* The emblem, the What's new card, the badge without a disc, the line icons and
     * a setting's picture are made at the size they are drawn, in pixels: never
     * scaled down by the graphics card (stair-stepped round edges, soft strokes). */
    int dw = 0, dh = 0;
    bool sized = sized_picture(name) && drawn_size("ot:" + name, dw, dh);
    if (kind == "icon") return ot_draw_icon(name.substr(5), sized ? dw : 48, out);
    if (kind == "chamfer") return ot_draw_chamfer(out);
    if (kind == "prev") return load_preview(name.substr(5), sized ? dw : 0, sized ? dh : 0, out);
    if (kind == "bg") {
        OtTheme t;
        if (g.have_assets) ot_theme_for_track(&g.assets, track, true, &t);
        else ot_theme_default(&t);
        ok = ot_theme_background(&t, 640, 400, &im);
    } else if (kind == "otbadge") {
        ok = ot_badge_ot(sized ? dw : 96, &im);
    } else if (g.have_assets && track >= 0 && track < OT_TRACK_COUNT) {
        /* The shipped HD card when it was read (verified disc only, see
         * load_hd_cards), else the disc's own card, smoothed and scaled up. */
        const OtImage *hd = g.hd_card[track].px ? &g.hd_card[track] : nullptr;
        if (kind == "emblem") {
            ok = hd ? ot_card_emblem(hd, &im) : ot_image_copy(&g.assets.emblem[track], &im);
            OtImage at{};
            if (ok && sized && (dw != im.w || dh != im.h) && ot_image_resize(&im, dw, dh, &at)) {
                ot_image_free(&im);
                im = at;
            }
        } else if (kind == "badge") {
            int w = sized ? dw : OT_CARD_HD_W;
            ok = hd ? ot_badge_make(hd, 0, w, &im) : ot_badge_from_disc(&g.assets, track, w, &im);
        } else if (kind == "carpet") {
            /* the background's cards: turned, blurred and half seen, at one size (mipmaps) */
            ok = hd ? ot_badge_make(hd, 0, 400, &im) : ot_badge_from_disc(&g.assets, track, 400, &im);
        }
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
        std::string path = g.prog + "ui/cards/" + ot_track_id(t) + ".png";
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

/* The launcher's disc check, and the game's own (launcher_check_iso): the
 * XBE's entry point must also be the one SSX Tricky.exe was recompiled from,
 * so a disc image the game would refuse is never shown as verified, and
 * Skip launcher never starts the game with it. `other_build`: the title and
 * region are right, the executable on the disc is not that one. `bad_path`:
 * the right disc, at a path the game cannot open (game_can_open). */
bool game_can_open(const std::string &u8);
OtDiscCheck disc_check(const std::string &iso, bool *other_build = nullptr, bool *bad_path = nullptr)
{
    OtDiscCheck d = iso.empty() ? OtDiscCheck{ OT_DISC_NO_PATH } : ot_disc_check(iso.c_str());
    bool other = d.status == OT_DISC_OK &&
                 ot_disc_check_entry(iso.c_str(), OT_SSX_TRICKY_USA_ENTRY).status != OT_DISC_OK;
    if (other) d.status = OT_DISC_WRONG;
    if (other_build) *other_build = other;
    bool path = d.status == OT_DISC_OK && !game_can_open(iso);
    if (path) d.status = OT_DISC_WRONG;
    if (bad_path) *bad_path = path;
    return d;
}

/* The game opens the disc image through the ANSI API (port/src/launcher.c,
 * u8_to_path / to_path): a path outside the system's code page needs its
 * short 8.3 form (often missing on other drives), and the whole path must fit
 * in MAX_PATH. The same test here, so the launcher never calls verified a
 * disc image the game will refuse. A UTF-8 system code page is not judged. */
bool game_can_open(const std::string &u8)
{
#ifdef _WIN32
    if (u8.empty() || GetACP() == CP_UTF8) return true;
    wchar_t w[MAX_PATH], sh[MAX_PATH];
    char out[MAX_PATH];
    BOOL lossy = FALSE;
    if (!MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, w, MAX_PATH)) return false;
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, MAX_PATH, nullptr, &lossy) && !lossy) return true;
    lossy = FALSE;
    return GetShortPathNameW(w, sh, MAX_PATH) &&
           WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, sh, -1, out, MAX_PATH, nullptr, &lossy) && !lossy;
#else
    (void)u8;
    return true;
#endif
}

void load_disc()
{
    bool other_build = false, bad_path = false;
    free_hd_cards();
    if (g.have_assets) {
        ot_disc_free(&g.assets);
        g.have_assets = false;
    }
    g.disc = disc_check(g.iso, &other_build, &bad_path);
    if (g.disc.status == OT_DISC_OK) {
        double t0 = now_ms();
        g.have_assets = ot_disc_load(g.iso.c_str(), &g.assets);
        otlog("timing disc_load_ms=%.1f ok=%d", now_ms() - t0, (int)g.have_assets);
        load_hd_cards();
    }
    /* PLAY follows the disc check: the game doesn't need the launcher's
     * pictures, which only this page's looks depend on. */
    g.have_disc = g.disc.status == OT_DISC_OK;
    g.have_art = g.have_assets;
    if (g.have_disc && !g.have_art)
        otlog("disc image verified, but its pictures could not be read: PLAY works, no track pictures here");
    g.disc_path = g.iso;
    switch (g.disc.status) {
    case OT_DISC_OK:
        g.disc_state = "ok"; g.disc_title = "SSX Tricky (USA)";
        g.disc_note = "disc verified";
        break;
    case OT_DISC_NO_PATH:
        g.disc_state = "warn"; g.disc_title = "No disc image yet"; g.disc_note = "Choose\xE2\x80\xA6";
        g.disc_kicker = "DISC IMAGE";
        g.disc_head = "POINT TO YOUR DISC IMAGE";
        g.disc_text = "OpenTricky plays the Xbox version of SSX Tricky from your own disc image: you need your own copy of the game.";
        g.disc_drop1 = "Drop your SSX Tricky (USA) Xbox disc image here";
        g.disc_drop2 = "or click to find or browse";
        break;
    case OT_DISC_NOT_FOUND:
        g.disc_state = "bad"; g.disc_title = "Not found"; g.disc_note = "Locate\xE2\x80\xA6";
        g.disc_kicker = "DISC IMAGE";
        g.disc_head = "WE CAN'T FIND YOUR DISC IMAGE";
        g.disc_text = "It was moved, renamed, or the drive is not connected.";
        g.disc_drop1 = "Locate it again";
        g.disc_path.clear();                    /* the old path tells the player nothing new: not shown */
        g.disc_drop2 = "or drop the disc image here";
        break;
    case OT_DISC_INCOMPLETE:
        g.disc_state = "bad"; g.disc_title = "Incomplete"; g.disc_note = "Choose another\xE2\x80\xA6";
        g.disc_kicker = "DISC IMAGE";
        g.disc_head = "THIS DISC IMAGE IS INCOMPLETE";
        g.disc_text = "The file is shorter than the disc, probably an interrupted copy. Copy or make the disc image again.";
        g.disc_drop1 = "Choose another file";
        g.disc_drop2 = "or drop the complete disc image here";
        break;
    default:
        g.disc_state = "bad"; g.disc_title = "Wrong disc image"; g.disc_note = "Choose another\xE2\x80\xA6";
        g.disc_kicker = "DISC IMAGE";
        g.disc_head = "THIS ISN'T SSX TRICKY (USA) FOR XBOX";
        if (other_build)
            g.disc_text = "This SSX Tricky (USA) disc image has been modified. OpenTricky needs an unmodified copy.";
        else if (!g.disc.xbox)
            g.disc_text = "This file isn't an Xbox disc image. OpenTricky needs the Xbox version of SSX Tricky (USA).";
        else if (g.disc.title[0])
            g.disc_text = std::string("This Xbox disc image is \xE2\x80\x9C") + g.disc.title + "\xE2\x80\x9D. OpenTricky needs SSX Tricky, USA release.";
        else
            g.disc_text = "This Xbox disc image is another game or another region. OpenTricky needs SSX Tricky, USA release.";
        g.disc_drop1 = "Choose another file";
        g.disc_drop2 = "or drop the right disc image here";
        if (bad_path) {
            g.disc_title = "Folder not readable";
            g.disc_kicker = "DISC IMAGE";
            g.disc_head = "THE GAME CAN'T OPEN THIS FOLDER";
            g.disc_text = "This is the right disc image, but its folder's name has letters the game can't read on this computer, "
                          "or the path is too long. Move it to a folder whose name uses only plain letters, then choose it again.";
            g.disc_drop1 = "Choose the moved file";
            g.disc_drop2 = "or drop it here";
        }
        break;
    }
    otlog("disc: state=%s xbox=%d title_id=%08X%s", g.disc_state.c_str(), (int)g.disc.xbox, g.disc.title_id,
          bad_path ? " (a path the game cannot open)" : "");
}

void show_page(const std::string &p);

/* ── Type and controls in whole pixels ─────────────────────────────────────
 * RmlUi draws a text at its baseline rounded to a pixel and a box between its
 * edges rounded each on its own, takes a font size and a letter-spacing in
 * whole pixels by cutting off the fraction, and rounds a 1 dp outline to whole
 * pixels while a line-height in dp keeps its fraction. In dp, a label's
 * capitals sat up to 1.5 px off the middle of its frame, a 14 dp row and a
 * 13.5 dp paragraph were both 18 px at 1920 x 1080, and the small capitals'
 * spacing went from 1 px to 2 px by the window's size.
 *
 * So at each layout (apply_layout) the sizes are set in whole pixels, as tokens
 * of tokens.rcss: every font size and spacing rounded to the nearest pixel (no
 * text under 10 px); every control's height, the line inside it and the margin
 * that centres it in its parent, from the real metrics of its font, so that its
 * text's baseline falls on a whole pixel and its capitals in its middle (the
 * capitals of Inter are centred in its line box: ascent - descent = their
 * height, to half a pixel after hinting). A line that mixes the small capitals
 * (9 dp) with a larger text (a dialog's key / value line) has one baseline: the
 * small capitals are centred in it, the text beside them on their baseline; a
 * small button in it is centred on them. The two status labels at the top and
 * the lines of Find my disc image centre each of their parts instead. */
struct OtFm { int a, d, c, o; };   /* ascent, descent, capitals' height, overhang (whole pixels, as drawn) */

static Rml::FontFaceHandle font_handle(const char *family, int weight, int size)
{
    Rml::FontEngineInterface *fe = Rml::GetFontEngineInterface();
    /* The family as the style sheets hold it: in lower case. */
    return fe ? fe->GetFontFaceHandle(Rml::StringUtilities::ToLower(family), Rml::Style::FontStyle::Normal,
                                      (Rml::Style::FontWeight)weight, size) : 0;
}

static OtFm font_metrics(const char *family, int weight, int size)
{
    Rml::FontEngineInterface *fe = Rml::GetFontEngineInterface();
    Rml::FontFaceHandle h = font_handle(family, weight, size);
    if (!h) {
        otlog("layout: no font %s %d %dpx", family, weight, size);
        return { (int)std::lround(size * 0.97f), (int)std::lround(size * 0.25f), (int)std::lround(size * 0.73f), 0 };
    }
    const Rml::FontMetrics &m = fe->GetFontMetrics(h);
    /* The capitals' height as drawn: the top of the H above the baseline, hinted; its overhang: how far
     * its ink goes past its advance on the right less its left bearing (a slanted face's ink leans right
     * of its advance). The default font engine's handle, whose glyphs of ASCII are made when it is. */
    const Rml::FontGlyphMap &glyphs = reinterpret_cast<Rml::FontFaceHandleDefault *>(h)->GetGlyphs();
    auto it = glyphs.find(Rml::Character('H'));
    if (it == glyphs.end()) return { (int)std::lround(m.ascent), (int)std::lround(m.descent), (int)std::lround(m.ascent - m.descent), 0 };
    const Rml::FontGlyph &H = it->second;
    const int rsb = H.advance - H.bearing.x - H.bitmap_dimensions.x;
    return { (int)std::lround(m.ascent), (int)std::lround(m.descent), H.bearing.y, H.bearing.x - rsb };
}

/* The ink of a text (ASCII) as drawn in a face: how many pixel rows it reaches above its baseline and
 * below it, counting the rows of its glyphs that are half covered or more (a faint row of a hinted
 * curve is not seen as its edge). False when the face or its glyphs' pixels are not there. */
static bool ink_rows(const char *family, int weight, int size, const char *text, int *above, int *below)
{
    Rml::FontFaceHandle h = font_handle(family, weight, size);
    if (!h) return false;
    const Rml::FontGlyphMap &glyphs = reinterpret_cast<Rml::FontFaceHandleDefault *>(h)->GetGlyphs();
    int up = 0, down = 0;
    bool any = false;
    for (const char *t = text; *t; t++) {
        auto it = glyphs.find(Rml::Character((unsigned char)*t));
        if (it == glyphs.end()) continue;
        const Rml::FontGlyph &gl = it->second;
        if (gl.bitmap_dimensions.x <= 0 || gl.bitmap_dimensions.y <= 0) continue;   /* a space: no ink */
        if (!gl.bitmap_data || gl.color_format != Rml::ColorFormat::A8) return false;
        int first = -1, last = -1;
        for (int y = 0; y < gl.bitmap_dimensions.y; y++) {
            const Rml::byte *row = gl.bitmap_data + (size_t)y * gl.bitmap_dimensions.x;
            for (int x = 0; x < gl.bitmap_dimensions.x; x++)
                if (row[x] >= 128) {
                    if (first < 0) first = y;
                    last = y;
                    break;
                }
        }
        if (first < 0) continue;
        up = std::max(up, gl.bearing.y - first);
        down = std::max(down, last + 1 - gl.bearing.y);
        any = true;
    }
    *above = up;
    *below = down;
    return any;
}

/* The line box inside `in` pixels that centres the capitals: the inside, moved by
 * the capitals' offset in the font's line box (ascent - descent - their height: 0
 * or a pixel for Inter, a few for the display face); one pixel more when the
 * half-leading would be half a pixel (the baseline on a whole pixel). `base`: the
 * baseline from the top of the inside. */
static int line_in(int in, OtFm f, int *base = nullptr)
{
    int l = in + f.c - (f.a - f.d);
    l += (l - f.a - f.d) & 1;
    if (base) *base = (l - f.a - f.d) / 2 + f.a;
    return l;
}

/* The line box inside `in` pixels that centres the ink of `text` in the face of `f` (ink_rows: its
 * capitals or figures, its ascenders, its descenders as drawn), half a pixel low when it cannot be
 * exact, as line_in; the capitals' height when the glyphs cannot be read. */
static int line_ink(int in, const char *family, int weight, int size, OtFm f, const char *text)
{
    int above = f.c, below = 0;
    if (!ink_rows(family, weight, size, text, &above, &below)) { above = f.c; below = 0; }
    const int base = (in - above - below + 1) / 2 + above;
    return 2 * base - f.a + f.d;
}

static void layout_tokens(float dp, std::vector<std::pair<std::string, std::string>> &tk)
{
    auto px = [dp](float v) { return (int)std::lround(v * dp); };
    auto put = [&tk](const char *name, int v) { tk.emplace_back(name, std::to_string(v) + "px"); };
    /* A size of `v` dp with the parity of `in`: a box centred in another, on whole pixels. */
    auto fit = [&px](float v, int in) { int h = px(v); return h + ((in - h) & 1); };

    /* Type: sizes and spacings. */
    static const struct { const char *name; float dp; } fs[] = {
        { "--fs-xs", 9 }, { "--fs-s", 11 }, { "--fs-m", 12 }, { "--fs-body", 13.5f }, { "--fs-l", 14 }, { "--fs-h", 19 },
        { "--fs-d1", 26 }, { "--fs-d2", 40 }, { "--fs-d3", 30 }, { "--fs-d4", 44 }, { "--fs-d5", 82 }, { "--fs-btn-d", 20 } };
    int size[12];
    for (int i = 0; i < 12; i++) {
        size[i] = std::max(10, px(fs[i].dp));
        put(fs[i].name, size[i]);
    }
    const int s_xs = size[0], s_s = size[1], s_m = size[2], s_body = size[3], s_l = size[4], s_d4 = size[9], s_btn = size[11];
    static const struct { const char *name, *back; float dp; } ls[] = {
        { "--ls-xs", "--ls-xs-back", 1.4f }, { "--ls-grp", "--ls-grp-back", 1.8f }, { "--ls-s", "--ls-s-back", 1.6f },
        { "--ls-m", "--ls-m-back", 2 }, { "--ls-pri", "--ls-pri-back", 1.5f }, { "--ls-play", "--ls-play-back", 3 } };
    for (auto &l : ls) {   /* and its negative: the space after the last letter given back (centred labels) */
        put(l.name, px(l.dp));
        put(l.back, -px(l.dp));
    }

    const char *disp = g.bullet ? "Bullet SmallCaps" : "Exo 2";
    const int dw = g.bullet ? 400 : 900;
    const OtFm xs7 = font_metrics("Inter", 700, s_xs), xs8 = font_metrics("Inter", 800, s_xs), s7 = font_metrics("Inter", 700, s_s),
               m4 = font_metrics("Inter", 400, s_m), m6 = font_metrics("Inter", 600, s_m), b4 = font_metrics("Inter", 400, s_body),
               b6 = font_metrics("Inter", 600, s_body), b7 = font_metrics("Inter", 700, s_body), l4 = font_metrics("Inter", 400, s_l),
               btn = font_metrics(disp, dw, s_btn), d4 = font_metrics(disp, dw, s_d4);
    const int b = std::max(1, px(1));   /* a 1 dp outline as RmlUi draws it */
    int o;

    /* The status labels (32 dp) and their small button (22 dp), the button centred. The two labels of the
     * pages (the disc image, the version) put each of their parts in the middle of their inside, on its
     * own ink, whatever their state: the disc's name on its capitals, the version on its figures (as
     * drawn), the small-capitals word on its capitals, the dot, the button and its label in it. Two texts
     * of a line may then sit on two baselines: on a single baseline, the name's capitals or the version's
     * figures sat up to 2 px high, or the small capitals up to 2 px low. A label without its own line (no
     * id) keeps one baseline, the button's (--lh-pill). */
    const int pill = px(32), pill_in = pill - 2 * b, pb = fit(22, pill_in), m_pb = (pill_in - pb) / 2;
    put("--h-pill", pill); put("--h-pb", pb); put("--m-pb", m_pb);
    put("--lh-pb", line_in(pb - 2 * b, xs7, &o));
    put("--lh-pill", 2 * (m_pb + b + o) - m6.a + m6.d);
    put("--lh-disc", line_in(pill_in, m6));
    put("--lh-ver", line_ink(pill_in, "Inter", 600, s_m, m6, "0123456789"));
    put("--lh-sw", line_in(pill_in, xs7));
    const int dot = std::max(2, px(8));
    put("--h-dot", dot); put("--m-dot", (pill_in - dot + 1) / 2);
    put("--h-prog", std::max(1, px(2))); put("--m-prog", px(4));   /* the bar of an update running */
    char rdot[32];
    snprintf(rdot, sizeof rdot, "%.1fpx", dot / 2.0f);
    tk.emplace_back("--r-dot", rdot);
    put("--pr-pill", px(12)); put("--mr-pb", m_pb - px(12));   /* the button's right gap = its top and bottom ones */
    /* The side paddings of the centred labels in whole pixels (their text centred to the pixel); for the
     * labels without an inner span (small buttons, chips, links) the right one less the small capitals'
     * spacing (given back). */
    for (int n : { 5, 6, 7, 9, 12, 15, 16, 18 }) {
        char nm[16];
        snprintf(nm, sizeof nm, "--p%d", n);
        put(nm, px((float)n));
        snprintf(nm, sizeof nm, "--p%d-r", n);
        put(nm, px((float)n) - px(1.4f));
    }

    /* OFFLINE / ONLINE (34 dp) and SOON (15 dp) in it. */
    const int mode = px(34), mode_in = mode - 2 * b, soon = fit(15, mode_in);
    put("--h-mode", mode); put("--lh-mode", line_in(mode_in, s7));
    put("--h-soon", soon); put("--lh-soon", line_in(soon - 2 * b, xs7)); put("--m-soon", (mode_in - soon) / 2);
    /* The presets (30 dp) and the tabs (32 dp). */
    const int seg = px(30);
    put("--h-seg", seg); put("--lh-seg", line_in(seg - 2 * b, xs7)); put("--lh-seg-out", line_in(seg, xs7));
    const int tab = px(32);
    put("--h-tab", tab); put("--lh-tab", line_in(tab - 2 * b, s7));

    /* A settings line (40 dp, its rule under it) and what sits in it: a button, SOON, the switch,
     * the mark of a changed value, a remapping cell, a PS2 button. */
    const int row = px(40);
    put("--h-row", row); put("--lh-row", line_in(row, l4));
    const int ab = fit(22, row), soonb = fit(15, row), tog = fit(18, row), mod = fit(4, row), cell = fit(28, row);
    put("--h-ab", ab); put("--lh-ab", line_in(ab - 2 * b, xs7)); put("--m-ab", (row - ab) / 2);
    put("--h-soonb", soonb); put("--lh-soonb", line_in(soonb - 2 * b, xs7)); put("--m-soonb", (row - soonb) / 2);
    put("--h-tog", tog); put("--m-tog", (row - tog) / 2);
    put("--h-mod", mod); put("--m-mod", (row - mod) / 2);
    put("--h-cell", cell); put("--lh-cell", line_in(cell - 2 * b, b7)); put("--m-cell", (row - cell) / 2);
    /* The scroll bar of a list (4 dp) and its gutter (16 dp, --p16): drawn as wide, and as far from the
     * lines, in each list, wherever its edges fall; the gutter given back where a list reaches into its
     * panel's padding (Find my disc image). */
    const int sb = std::max(1, px(4));
    put("--w-sb", sb); put("--w-sb-back", -sb);
    put("--p16-back", -px(16));

    /* The help: a picture's tag (20 dp). */
    const int tag = px(20);
    put("--h-tag", tag); put("--lh-tag", line_in(tag - 2 * b, xs7));

    /* A dialog's key / value line (30 dp): the key's small capitals centred, the value on their baseline. */
    const int kv = px(30);
    put("--h-kv", kv); put("--lh-kv-k", line_in(kv, xs7, &o)); put("--lh-kv", 2 * o - b4.a + b4.d);
    /* Find my disc image: a name line (18 dp in whole pixels, --h-dl, with or without IN USE) and IN USE
     * (15 dp) on it, each in the middle of the line, as in the top labels: the name on its capitals (its
     * own line from the top, its margin box the line's height: --mb-dn), IN USE's box (centred by its
     * margin), its label in it. The folder's line under it keeps the height the name's line had
     * (--lh-dname). */
    const int dl = px(18), chip = fit(15, dl), m_chip = (dl - chip) / 2, lh_dn = line_in(dl, b7);
    put("--h-chip", chip); put("--m-chip", m_chip);
    put("--lh-chip", line_in(chip - 2 * b, xs7, &o));
    put("--lh-dname", 2 * (m_chip + b + o) - b6.a + b6.d);
    put("--h-dl", dl); put("--lh-dn", lh_dn); put("--mb-dn", dl - lh_dn);
    /* An install's steps (22 dp, the update dialog) and their number in a square (20 dp): the square in
     * the middle of the line, its figure in the middle of the square. */
    const int step = px(22), sic = fit(20, step);
    const OtFm s8 = font_metrics("Inter", 800, s_s);
    put("--h-step", step); put("--lh-step", line_in(step, b4));
    put("--h-sic", sic); put("--lh-sic", line_in(sic - 2 * b, s8)); put("--m-sic", (step - sic) / 2);
    /* A dialog's buttons (34 dp) and the update buttons of What's new (36 dp, no outline). */
    const int mb = px(34);
    put("--h-mbtn", mb); put("--lh-mbtn", line_in(mb - 2 * b, s7)); put("--lh-mbtn-pri", line_in(mb - 2 * b, btn));
    const int ub = px(36);
    put("--h-ubtn", ub); put("--lh-ubtn", line_in(ub, s7));

    /* The footer (54 dp): its links (22 dp, a rule of 12 dp between), the controller hints (their keys
     * 18 dp), Reset tab and Save (36 dp), the Saved note. */
    const int foot = px(54), fl = fit(22, foot), sep = fit(12, fl), fb = fit(36, foot), kb = fit(18, fl);
    put("--h-foot", foot); put("--h-fl", fl); put("--m-fl", (foot - fl) / 2); put("--lh-fl", line_in(fl, xs7));
    put("--h-sep", sep); put("--m-sep", (fl - sep) / 2);
    put("--lh-hint", line_in(fl, m4)); put("--h-kb", kb); put("--m-kb", (fl - kb) / 2); put("--lh-kb", line_in(kb, xs8));
    put("--h-fbtn", fb); put("--m-fbtn", (foot - fb) / 2); put("--lh-fbtn", line_in(fb - 2 * b, s7));
    put("--lh-save", line_in(fb - 2 * b, btn)); put("--lh-note", line_in(foot, m4));

    /* PLAY (82 dp), with its outline inside when it is off. */
    const int play = px(82);
    put("--h-play", play); put("--lh-play", line_in(play, d4)); put("--lh-play-off", line_in(play - 2 * b, d4));

    /* The display face leans: its ink sits right of its advance by half its overhang (the H's: the same
     * for every capital of a slanted face). The main buttons' labels move back by that much (half
     * pixels: RmlUi rounds the result). PLAY's own letter-spacing already makes up for it. */
    char dx[32];
    snprintf(dx, sizeof dx, "%.1fpx", -btn.o / 2.0f);
    tk.emplace_back("--dx-pri", dx);
}

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
    /* Type and controls in whole pixels (layout_tokens); no text under 10 real pixels, whatever the
     * scale (at 960 x 600, 1 dp = 0.75 px, so 9 dp would be 6.75 px). The window's minimum stays as it is. */
    std::vector<std::pair<std::string, std::string>> tk;
    layout_tokens(dp > 0 ? dp : 1, tk);
    {
        static std::string last;
        std::string all;
        for (auto &t : tk) all += t.first + "=" + t.second + " ";
        /* New values: every element computes its properties again (refit_texts; a font-size or a
         * line-height written with a variable too: the local fix of RmlUi, third_party/VERSIONS.txt). */
        if (all != last) {
            otlog("layout dp=%.3f %s", dp, all.c_str());
            g.rescaled = true;
        }
        last = all;
    }
    /* The focus ring in whole pixels (2 dp rounded, at least 2 px): a ring of 2.5 px
     * (1600 x 1000) or 2.7 px (1920 x 1080) is drawn with a half-lit row of pixels,
     * a soft edge. PLAY's ring is written with its shadows, the ring first so the
     * dark offset shadow never covers it. */
    const int ring = std::max(2, (int)std::lround(2.0f * dp));
    char ringbuf[48], ringin[56], iconbuf[32], edgebuf[48];
    snprintf(ringbuf, sizeof ringbuf, "#ffffff 0 0 0 %dpx", ring);
    snprintf(ringin, sizeof ringin, "#ffffff 0 0 0 %dpx inset", ring);    /* the lines of the scrolled list */
    snprintf(edgebuf, sizeof edgebuf, "#ffffff 0 0 0 %dpx", std::max(1, (int)std::lround(dp)));   /* PLAY hovered: 1 dp */
    /* The line icons (22 dp) on whole pixels: they are drawn at that size (make_texture). */
    snprintf(iconbuf, sizeof iconbuf, "%dpx", (int)std::lround(22.0f * dp));
    for (Rml::ElementDocument *d : { g.home, g.settings, g.dialog }) {
        if (!d) continue;
        d->SetProperty("--focus-ring", ringbuf);
        d->SetProperty("--focus-ring-in", ringin);
        d->SetProperty("--play-ring", ringbuf);
        d->SetProperty("--hover-edge", edgebuf);
        d->SetProperty("--icon", iconbuf);
        for (auto &t : tk) d->SetProperty(t.first, t.second);
        if (Rml::Element *s = d->GetElementById("stage")) {
            s->SetProperty("width", wbuf);
            s->SetProperty("margin-left", mbuf);
        }
        if (Rml::Element *f = d->GetElementById("fin")) {
            f->SetProperty("width", wbuf);
            f->SetProperty("left", fbuf);
        }
    }
    if (Rml::ElementDocument *d = g_pages.doc ? g_pages.doc() : nullptr) {   /* an extra page: the same stage */
        d->SetProperty("--icon", iconbuf);
        for (auto &t : tk) d->SetProperty(t.first, t.second);
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
    g.home = g.ctx->LoadDocument(g.prog + "ui/home.rml");
    g.settings = g.ctx->LoadDocument(g.prog + "ui/settings.rml");
    g.dialog = g.ctx->LoadDocument(g.prog + "ui/dialog.rml");
    if (!g.home || !g.settings || !g.dialog) otlog("error: cannot load the interface documents");
    if (g_pages.load_documents) g_pages.load_documents();
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
    if (g_pages.owns && g_pages.owns(g.page) && g_pages.doc && g_pages.doc()) return g_pages.doc();
    return g.page == "settings" ? g.settings : g.home;
}

/* The focus is shown (focus-visible) only to a player who moves it with the
 * keyboard or a controller: a page or dialog opened with the mouse, or at
 * start, has its focus on PLAY or the first button without the highlight. */
bool focus_shown() { return g.pad_active || g.kbd_nav; }

void show_page(const std::string &p)
{
    if (g.model) g.model.DirtyVariable("page");   /* set below; read at the next update */
    if (g.page == "settings" && p != "settings") deck_note_seen();
    g.page = p;
    if (g_pages.show_page && g_pages.show_page(p)) return;
    Rml::ElementDocument *on = p == "settings" ? g.settings : g.home;
    Rml::ElementDocument *off = p == "settings" ? g.home : g.settings;
    if (off) off->Hide();
    if (!on) return;
    on->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
    if (p == "settings") {
        g.focus_first_row = true;
    } else if (Rml::Element *e = on->GetElementById(g.have_disc ? "play" : "drop")) {
        e->Focus(focus_shown());
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
                if (e->GetAttribute<int>("rid", -1) == g.focus_id) { e->Focus(focus_shown()); break; }
        } else if (Rml::Element *e = d->GetElementById(g.have_disc ? "play" : "drop")) {
            e->Focus(focus_shown());
        }
    }
}

/* ── Game ────────────────────────────────────────────────────────── */

void set_crash(const OtCrashReport &r, int code)
{
    g.crash_dir = r.found ? r.dir : "";
    bool freeze = r.kind == "freeze";
    if (code == OT_EXIT_GPU_START) {
        g.crash_head = "THE GAME COULDN'T START ITS GRAPHICS";
        g.crash_text = "SSX Tricky needs a graphics card with Direct3D 11 (feature level 11_0) and an up-to-date driver. "
                       "Update your graphics driver, then press PLAY again. If it still happens, a bug report helps: "
                       "a zip with the logs and your settings, never your disc image or your saves.";
    } else if (code == OT_EXIT_GPU_LOST) {
        g.crash_head = "THE GRAPHICS DRIVER STOPPED RESPONDING";
        g.crash_text = "Your graphics driver stopped responding or was reset while you played, so the game had to close. "
                       "Update your graphics driver if this happens again. A bug report helps: "
                       "a zip with the logs and your settings, never your disc image or your saves.";
    } else {
        g.crash_head = freeze ? "THE GAME FROZE" : "THE GAME CLOSED UNEXPECTEDLY";
        g.crash_text = "Sorry about that. A bug report helps fix it: it is a zip with the logs, your settings and a crash file. "
                       "It never contains your disc image or your saves.";
    }
    SDL_Time t = r.found ? r.time : 0;
    if (!t) SDL_GetCurrentTime(&t);
    g.crash_when = "When: " + ot_when(t);
    g.crash_version = "Version: v" + std::string(OT_VERSION);      /* the code and the graphics card: in the bug report */
    for (const char *v : { "crash_head", "crash_text", "crash_when", "crash_version" }) g.model.DirtyVariable(v);
}

/* A PlayStation controller the game reads with its PlayStation controllers
 * setting (port/src/pad_sdl.c): a DualShock 4 or a DualSense, not a
 * DualShock 3 (the launcher's own button pictures count it too). */
static bool pad_is_ps_game(SDL_GamepadType t)
{
    return t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
}

/* The game will most likely be played with a PlayStation controller: the
 * last controller pressed here is one the game reads, or, when none was
 * pressed, the controllers plugged in are all such ones. */
bool pad_in_use_is_ps()
{
    if (g.pad_pressed) return g.pad_game_ps;
    int n = 0;
    bool ps = false, other = false;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        if (pad_is_ps_game(SDL_GetGamepadTypeForID(ids[i]))) ps = true;
        else other = true;
    }
    SDL_free(ids);
    return ps && !other;
}

bool play()
{
    if (!g.have_disc || g.game) return false;
    std::string exe = g.prog + k_game_exe;
    if (!SDL_GetPathInfo(exe.c_str(), nullptr)) {
        otlog("play: %s not found in bin\\ nor beside the launcher", k_game_exe);
        g.notice_kick = "PLAY";
        g.notice_head = "GAME NOT FOUND";
        g.notice_text = std::string("\xE2\x80\x9C") + k_game_exe + "\xE2\x80\x9D should be in the bin folder beside OpenTricky.exe. Unzip the whole release into one folder.";
        g.notice_file = "";
        g.model.DirtyVariable("notice_kick");
        g.model.DirtyVariable("notice_head");
        g.model.DirtyVariable("notice_text");
        show_modal("notice");
        return false;
    }
#ifdef _WIN32
    {
        /* A player's game of this folder already running (started by a
         * double-click, or left by an earlier launcher): never a second one. */
        char name[128];
        HANDLE m = settings_instance_name("Game", name, sizeof name) ? OpenMutexA(SYNCHRONIZE, FALSE, name) : nullptr;
        if (m) {
            CloseHandle(m);
            otlog("play: SSX Tricky of this folder is already running");
            g.notice_kick = "PLAY";
            g.notice_head = "SSX TRICKY IS ALREADY RUNNING";
            g.notice_text = "Switch to its window (Alt+Tab), or close it before pressing PLAY again.";
            g.notice_file = "";
            for (const char *v : { "notice_kick", "notice_head", "notice_text" }) g.model.DirtyVariable(v);
            show_modal("notice");
            return false;
        }
    }
#endif
    /* The game reads settings.ini at start: when it cannot be written, the
     * player is told once and PLAY again starts with the settings saved before. */
    if (!save_settings_file() && !g.skip_mode && !g.save_warned) {
        g.save_warned = true;
        otlog("play: settings.ini could not be written: the player is told first");
        g.notice_kick = "PLAY";
        g.notice_head = "YOUR SETTINGS COULDN'T BE SAVED";
        g.notice_text = "Your settings file can't be written (read-only or blocked by an antivirus). "
                        "Press PLAY again to start with your previous settings.";
        g.notice_file = "";
        for (const char *v : { "notice_kick", "notice_head", "notice_text" }) g.model.DirtyVariable(v);
        show_modal("notice");
        return false;
    }
    SDL_Environment *env = SDL_CreateEnvironment(true);
    SDL_SetEnvironmentVariable(env, "OPENTRICKY_LAUNCHER", "1", true);   /* the crash screen is the launcher's */
    /* Button style Auto, PlayStation controllers on (experimental): the
     * game's buttons drawn as the controller in use. The game picks its
     * style once, at start; a variable already set (tests) is kept. */
    if (settings_choice_index(g.set, S_BUTTON_STYLE) == 0 && settings_get_bool(g.set, S_PS_CONTROLLERS) &&
        pad_in_use_is_ps()) {
        SDL_SetEnvironmentVariable(env, "XBOX_BUTTON_ICONS", "playstation", false);
        otlog("play: button style Auto, PlayStation controller in use");
    }
    if (g_pages.play_env) g_pages.play_env(env);
    const char *args[] = { exe.c_str(), "--play", nullptr };
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetStringProperty(p, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, g.base.c_str());   /* the install root */
    SDL_GetCurrentTime(&g.launch_time);
    g.game = SDL_CreateProcessWithProperties(p);
    if (g.game) g.games_started++;
    SDL_DestroyProperties(p);
    SDL_DestroyEnvironment(env);
    if (!g.game) {
        /* The file is there (checked above) but Windows would not start it:
         * an antivirus that blocks it, a damaged copy. Said, never a PLAY
         * that does nothing. */
        otlog("play: cannot start the game: %s", SDL_GetError());
        g.notice_kick = "PLAY";
        g.notice_head = "THE GAME COULDN'T START";
        g.notice_text = std::string("Windows couldn't start \xE2\x80\x9C") + k_game_exe +
                        "\xE2\x80\x9D in the bin folder. An antivirus may have blocked it: allow it, or unzip the release again.";
        g.notice_file = "";
        show_modal("notice");
        return false;
    }
    otlog("game started (pid %lld)", (long long)SDL_GetNumberProperty(SDL_GetProcessProperties(g.game), SDL_PROP_PROCESS_PID_NUMBER, 0));
    SDL_HideWindow(g.window);                   /* hidden while the game runs */
    return true;
}

/* XBOX_NO_ACTIVATE=1 (test tools only): the launcher never takes the focus
 * from the program in front, like the game it starts (which inherits the
 * variable). Its window is shown without activation and put behind the
 * others; it is never raised. Unset or 0: shown and raised as always. */
bool no_activate()
{
    static int on = -1;
    if (on < 0) { const char *e = SDL_getenv("XBOX_NO_ACTIVATE"); on = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return on == 1;
}

void show_launcher_window(bool raise)
{
    if (no_activate()) {
#ifdef _WIN32
        /* Shown behind the others in one step (shown first, it would cover
         * the window in front for a moment); SDL then only records it. */
        HWND w = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        const UINT f = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        /* Never the active window, not even when the one in front is
         * minimised or closed (WS_EX_APPWINDOW keeps the task bar button). */
        if (w) SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_APPWINDOW);
        if (w) SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, f | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        SDL_ShowWindow(g.window);
        if (w) SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, f);
#else
        SDL_ShowWindow(g.window);
#endif
        return;
    }
    SDL_ShowWindow(g.window);
    if (raise) SDL_RaiseWindow(g.window);
}

#ifdef _WIN32
/* XBOX_NO_ACTIVATE, each frame: when the window in front closes, Windows
 * activates the next window even with WS_EX_NOACTIVATE. The foreground is
 * then given back to the last window seen in front if it is still there,
 * else to the next visible window of another program in the z-order, else
 * to the desktop, and the launcher goes to the bottom (as the game does). */
bool na_candidate(HWND w)
{
    DWORD pid = 0;
    RECT r;
    if (!w || !IsWindow(w) || !IsWindowVisible(w) || IsIconic(w)) return false;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return false;
    if (GetWindowLongPtrW(w, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
    if (!GetWindowRect(w, &r) || r.right <= r.left || r.bottom <= r.top) return false;
    typedef HRESULT (WINAPI *DwmAttrFn)(HWND, DWORD, PVOID, DWORD);
    static DwmAttrFn dwm_attr = [] {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        return m ? (DwmAttrFn)(void *)GetProcAddress(m, "DwmGetWindowAttribute") : (DwmAttrFn) nullptr;
    }();
    DWORD cloaked = 0;
    return !(dwm_attr && SUCCEEDED(dwm_attr(w, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof cloaked)) && cloaked);
}

void keep_out_of_front()
{
    static HWND last_front;
    static int logged;
    HWND self = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    HWND f = GetForegroundWindow();
    if (!self) return;
    if (f != self) {
        if (f && na_candidate(f)) last_front = f;
        return;
    }
    HWND to = nullptr;
    const char *how = "the desktop";
    if (na_candidate(last_front)) { to = last_front; how = "the last window in front"; }
    else {
        for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT))
            if (na_candidate(w)) { to = w; how = "the next window in the z-order"; break; }
    }
    if (!to) to = GetShellWindow();
    bool ok = to && SetForegroundWindow(to);
    SetWindowPos(self, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (logged++ < 20) otlog("window: XBOX_NO_ACTIVATE: activated by the system, handed back to %s%s", how, ok ? "" : " (refused)");
}
#endif

void game_ended(int code)
{
    otlog("game ended (exit code %d, 0x%08X)", code, (unsigned)code);
    if (g_pages.game_ended) g_pages.game_ended(code);
    if (code == OT_EXIT_DISC_REFUSED) {         /* the game could not use the disc image: say so, never loop */
        otlog("the game refused the disc image: the launcher shows");
        if (!g.shown) {
            g.shown = true;
            g.skip_mode = false;
        }
        g.notice_kick = "DISC IMAGE";
        g.notice_head = "THE GAME COULDN'T USE THIS DISC IMAGE";
        g.notice_text = "SSX Tricky couldn't open it, or it isn't the exact SSX Tricky (USA) disc OpenTricky was made for. "
                        "Choose your disc image again. With Log file on (Advanced), the game's log says why.";
        g.notice_file = "";
        g.model.DirtyVariable("notice_kick");
        g.model.DirtyVariable("notice_head");
        g.model.DirtyVariable("notice_text");
        show_launcher_window(true);
        show_modal("notice");
        return;
    }
    /* A report folder written since the launch, or an exception code. */
    OtCrashReport r = ot_find_crash_report(g.base, g.launch_time - 2 * SDL_NS_PER_SECOND);
    bool exception = (unsigned)code >= 0xC0000000u;
    bool graphics = code == OT_EXIT_GPU_START || code == OT_EXIT_GPU_LOST;     /* said even if no report could be written */
    if (r.found || exception || graphics) {
        otlog("crash screen: %s", r.found ? r.name.c_str() : "(no report folder)");
        set_crash(r, exception || graphics ? code : 0);
        if (!g.shown) {
            g.shown = true;
            g.skip_mode = false;
        }
        show_launcher_window(true);
        show_modal("crash");
        return;
    }
    if (g.skip_mode) {                          /* never shown: the launcher ends with the game */
        g.quit = true;
        return;
    }
    show_launcher_window(true);
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
    otlog("bug report: %s (%s, %d files)%s%s", ot_file_name(file).c_str(), ok ? "written" : "NOT written", (int)z.size(),
          ok ? "" : ": ", ok ? "" : SDL_GetError());
    g.notice_kick = "TROUBLESHOOTING";
    g.notice_head = ok ? "BUG REPORT READY" : "COULDN'T SAVE THE BUG REPORT";
    g.notice_text = ok ? "Saved on your Desktop: " + ot_file_name(file) +
                             ". Nothing is sent. Attach it to a GitHub issue."
                       : "The Desktop can't be written to.";
    g.notice_file = ok ? file : "";
    g.model.DirtyVariable("notice_kick");
    g.model.DirtyVariable("notice_head");
    g.model.DirtyVariable("notice_text");
    g.model.DirtyVariable("notice_file");
    show_modal("notice");
}

/* ── Actions ─────────────────────────────────────────────────────── */

void save_settings()
{
    if (!save_settings_file()) {
        g.saved_note = "Couldn't save";
    } else {
        deck_note_seen();
        g.saved_note = "Saved";
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
    case A_CHECK_UPDATES:
        if (g.upd_state == "update") update_action("pill");                  /* Update: the new version's notes */
        else if (g.upd_state != "checking" && !update_just_checked()) update_start(true);
        break;
    case A_ABOUT: show_modal("about"); break;
    case A_RESTORE: {
        std::string v;
        if (!ot_install_has_backup(g.base, v)) break;
        g.restore_version = v;             /* asked first: Restore / Cancel */
        show_modal("restore_ask");
        break;
    }
    default: break;
    }
}

void dialog_action(const std::string &a)
{
    if (a == "cap_cancel") { close_modal(); return; }
    if (a == "cap_clear") { capture_assign("None"); return; }
    if (a == "swap") { conflict_answer(true); return; }
    if (a == "conf_cancel") { conflict_answer(false); return; }
    if (a == "report") { close_modal(); prepare_bug_report(); return; }
    if (a == "restore") { close_modal(); install_start(true); return; }
    if (a == "inst_retry") { bool r = g.inst_kick == "RESTORE"; close_modal(); install_start(r); return; }
    if (a == "inst_cancel") { install_cancel(); return; }
    if (a == "inst_page") { if (!g.upd_url.empty()) SDL_OpenURL(g.upd_url.c_str()); close_modal(); return; }
    if (a == "logs") { open_folder(ini_dir()); return; }
    if (a == "licences") {
        std::string f = g.prog + "THIRD-PARTY-LICENSES.txt";
        std::string url = "file:///" + f;
        for (char &c : url) if (c == '\\') c = '/';
        if (!SDL_OpenURL(url.c_str())) otlog("cannot open the licences: %s", SDL_GetError());
        return;
    }
    if (a == "github") { SDL_OpenURL(k_url_github); return; }
    if (a == "disc_browse") { close_modal(); choose_path(S_DISC_IMAGE); return; }
    if (a == "show_file") { if (!g.notice_file.empty()) open_folder(ot_dir_of(g.notice_file)); close_modal(); return; }
    close_modal();
}

/* The dialog's "Esc / B". */
void dialog_back()
{
    if (g.modal == "update" && install_running()) {           /* no way out half-way: Cancel while it can */
        install_cancel();
        return;
    }
    if (g.modal == "capture") close_modal();
    else if (g.modal == "conflict") conflict_answer(false);
    else close_modal();
}

Rml::Element *focused()
{
    return g.ctx->GetFocusElement();
}

/* e or one of its parents up to the row (the element with a rid) has this class. */
bool has_class_up(Rml::Element *e, const char *cls)
{
    for (; e; e = e->GetParentNode()) {
        if (e->IsClassSet(cls)) return true;
        if (e->HasAttribute("rid")) return false;
    }
    return false;
}

int focused_row()
{
    for (Rml::Element *e = focused(); e; e = e->GetParentNode()) {
        int id = e->GetAttribute<int>("rid", -1);
        if (id >= 0) return id;
    }
    return -1;
}

/* Entirely in view in each list that scrolls it, as RmlUi's ScrollIntoView reads the lists (the
 * settings, Find my disc image: they scroll up and down); a line taller than its list counts as in
 * view, since it cannot be more. */
bool in_view(Rml::Element *e)
{
    const float top = e->GetAbsoluteOffset(Rml::BoxArea::Border).y, h = e->GetBox().GetSize(Rml::BoxArea::Border).y;
    for (Rml::Element *p = e->GetParentNode(); p; p = p->GetParentNode()) {
        const Rml::Style::Overflow o = p->GetComputedValues().overflow_y();
        if (o == Rml::Style::Overflow::Visible || o == Rml::Style::Overflow::Hidden || p->GetScrollHeight() <= p->GetClientHeight())
            continue;
        const float vt = p->GetAbsoluteOffset(Rml::BoxArea::Padding).y, vh = p->GetClientHeight();
        if (h <= vh && (top < vt - 0.5f || top + h > vt + vh + 0.5f)) return false;
    }
    return true;
}

/* A press on a focus out of its list's view (the wheel or the scroll bar moved the list) only brings
 * it back, shown: never an action on a line the player cannot see. True: the press stops here. */
bool focus_back_in_view(Rml::Element *f)
{
    if (!f || f == f->GetOwnerDocument() || in_view(f)) return false;
    f->SetPseudoClass("focus-visible", true);
    f->ScrollIntoView(Rml::ScrollAlignment::Nearest);
    return true;
}

/* A key or a controller's button stops what is left of the wheel's smooth scroll: RmlUi runs one smooth
 * scroll at a time and moves its list a little each frame until it ends, whatever else scrolls the list (a
 * focus brought into view would leave it again). A smooth scroll of nothing replaces it. */
void stop_wheel()
{
    Rml::Element *e = g.ctx ? g.ctx->GetFocusElement() : nullptr;    /* any element of a document will do */
    if (e && e->GetContext()) e->ScrollTo(Rml::Vector2f(e->GetScrollLeft(), e->GetScrollTop()), Rml::ScrollBehavior::Smooth);
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
        if (id >= 0 && id < SETTINGS_COUNT && (n == N_LEFT || n == N_RIGHT)) {
            if (!focus_back_in_view(focused())) step_value(id, n == N_RIGHT ? 1 : -1);
            return;
        }
    } else if (g_pages.owns && g_pages.owns(g.page)) {
        if (g_pages.nav && g_pages.nav(n)) return;
    } else {
        if (n == N_PREV || n == N_NEXT) return;   /* the theme is random, no track to choose */
        if (n == N_BACK) return;
    }
    if (n < N_OK) {
        Rml::Element *f = g.ctx->GetFocusElement();
        if (f && f != f->GetOwnerDocument() && !f->IsPseudoClassSet("focus-visible")) {
            /* first press: the focus shows where it is, in view (the wheel or the scroll bar may have
             * scrolled it out of its list) */
            f->SetPseudoClass("focus-visible", true);
            f->ScrollIntoView(Rml::ScrollAlignment::Nearest);
            return;
        }
    }
    if (n <= N_OK && !focus_back_in_view(g.ctx->GetFocusElement())) {
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
    stop_wheel();
    set_pad_active(true);
    if (g.modal == "capture" && g.cap_col == 1) {
        if (const char *t = pad_token(b)) capture_assign(t);
        return true;
    }
    if (b == SDL_GAMEPAD_BUTTON_START && g_pages.owns && g_pages.owns(g.page)) {
        if (g.modal.empty() && g_pages.primary) g_pages.primary();
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
    stop_wheel();
    set_pad_active(false);
    g.kbd_nav = true;
    if (g.modal == "capture") {
        if (k == SDLK_ESCAPE) close_modal();
        else if (k == SDLK_BACKSPACE) capture_assign("None");
        else if (g.cap_col == 0) {
            std::string n = key_name(k, sc);
            if (!n.empty()) capture_assign(n);
        }
        return true;
    }
    /* A text field of an extra page: its keys go to it (but Esc, Enter and
     * the up / down arrows, which move between the fields). */
    if (g_pages.typing && g_pages.typing() && k != SDLK_ESCAPE && k != SDLK_UP && k != SDLK_DOWN && k != SDLK_RETURN &&
        k != SDLK_KP_ENTER) return false;
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

/* Test: every visible element of the page (or of the dialog shown) with its boxes, fill, outline and
 * font, and every text with its baseline, in window pixels: one JSON object a line. The project's
 * tools measure the pictures against it (a text's ink against its frame). */
void save_boxes(const std::string &file)
{
    std::string out;
    auto put = [&out](const char *fmt, ...) {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        out += buf;
    };
    auto esc = [](const Rml::String &s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            if ((unsigned char)c < 0x20) { o += ' '; continue; }
            o += c;
        }
        return o;
    };
    int n = 0;
    std::function<void(Rml::Element *, int, int)> walk = [&](Rml::Element *e, int depth, int parent) {
        if (!e->IsVisible()) return;
        const int me = n++;
        const Rml::ComputedValues &cv = e->GetComputedValues();
        if (Rml::ElementText *t = rmlui_dynamic_cast<Rml::ElementText *>(e)) {
            const Rml::FontMetrics &m = Rml::GetFontEngineInterface()->GetFontMetrics(t->GetFontFaceHandle());
            Rml::Vector2f o = t->GetAbsoluteOffset();
            for (const auto &ln : t->GetLines()) {
                if (ln.text.find_first_not_of(" \t\n") == std::string::npos) continue;
                put("{\"k\":\"t\",\"n\":%d,\"p\":%d,\"x\":%.3f,\"y\":%.3f,\"w\":%d,\"size\":%d,\"asc\":%.3f,\"desc\":%.3f,"
                           "\"xh\":%.3f,\"style\":%d,\"weight\":%d,\"tt\":%d,\"family\":\"%s\",\"text\":\"%s\"}\n",
                        me, parent, o.x + ln.position.x, o.y + ln.position.y, ln.width, m.size, m.ascent, m.descent, m.x_height,
                        (int)cv.font_style(), (int)cv.font_weight(), (int)cv.text_transform(), esc(cv.font_family()).c_str(),
                        esc(ln.text).c_str());
            }
            return;
        }
        const Rml::Box &b = e->GetBox();
        Rml::Vector2f bo = e->GetAbsoluteOffset(Rml::BoxArea::Border), bs = b.GetSize(Rml::BoxArea::Border);
        Rml::Vector2f po = e->GetAbsoluteOffset(Rml::BoxArea::Padding), ps = b.GetSize(Rml::BoxArea::Padding);
        Rml::Vector2f co = e->GetAbsoluteOffset(Rml::BoxArea::Content), cs = b.GetSize(Rml::BoxArea::Content);
        Rml::Colourb bg = cv.background_color(), bc = cv.border_top_color();
        const bool deco = e->GetLocalProperty(Rml::PropertyId::Decorator) != nullptr || cv.has_decorator();
        put("{\"k\":\"e\",\"n\":%d,\"p\":%d,\"d\":%d,\"tag\":\"%s\",\"id\":\"%s\",\"cls\":\"%s\",\"bx\":[%.3f,%.3f,%.3f,%.3f],"
                   "\"px\":[%.3f,%.3f,%.3f,%.3f],\"cx\":[%.3f,%.3f,%.3f,%.3f],\"bw\":[%.3f,%.3f,%.3f,%.3f],\"bg\":[%d,%d,%d,%d],"
                   "\"bc\":[%d,%d,%d,%d],\"deco\":%d,\"skew\":%d,\"fs\":%.3f,\"lh\":%.3f,\"va\":%d}\n",
                me, parent, depth, esc(e->GetTagName()).c_str(), esc(e->GetId()).c_str(), esc(e->GetClassNames()).c_str(),
                bo.x, bo.y, bs.x, bs.y, po.x, po.y, ps.x, ps.y, co.x, co.y, cs.x, cs.y,
                b.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Top), b.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Right),
                b.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Bottom), b.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Left),
                bg.red, bg.green, bg.blue, bg.alpha, bc.red, bc.green, bc.blue, bc.alpha, deco ? 1 : 0,
                cv.has_local_transform() ? 1 : 0, cv.font_size(), cv.line_height().value, (int)cv.vertical_align().type);
        for (int i = 0; i < e->GetNumChildren(); i++) walk(e->GetChild(i), depth + 1, me);
    };
    Rml::ElementDocument *d = !g.modal.empty() && g.dialog && g.dialog->IsVisible() ? g.dialog : page_doc();
    if (d) walk(d, 0, -1);
    SDL_IOStream *io = SDL_IOFromFile(file.c_str(), "wb");
    if (!io || SDL_WriteIO(io, out.data(), out.size()) != out.size()) otlog("boxes: cannot write %s", file.c_str());
    else otlog("boxes %s %d", file.c_str(), n);
    if (io) SDL_CloseIO(io);
}

void script_step()
{
    if (g_pages.script_hold && g_pages.script_hold()) return;   /* a wait of an extra page's test step */
    if (g.script_pos < g.script.size() && g.script[g.script_pos] == "waitrace") {
        /* test: until a game has been started (a room's race starts when the room says so) and has closed */
        if (g.game || g.games_started == 0) return;
        g.script_pos++;
        g.script_wait = 30;
        return;
    }
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
            { "backspace", SDLK_BACKSPACE }, { "space", SDLK_SPACE },
            { "f10", SDLK_F10 }, { "f11", SDLK_F11 }, { "f12", SDLK_F12 } };
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
    } else if (a == "mclick" || a == "mhover" || a == "mdown") {
        /* The mouse at x,y (window pixels) as a player's would be; the element
         * under it and the dialog opened (if any) are logged. */
        int x = atoi(b.c_str()), y = b.find(':') != std::string::npos ? atoi(b.c_str() + b.find(':') + 1) : 0;
        set_pad_active(false);
        g.ctx->ProcessMouseMove(x, y, 0);
        Rml::Element *h = g.ctx->GetHoverElement();
        if (a == "mclick") {
            g.mouse_click = true;
            g.ctx->ProcessMouseButtonDown(0, 0);
            g.ctx->ProcessMouseButtonUp(0, 0);
            g.mouse_click = false;
        } else if (a == "mdown") {
            g.ctx->ProcessMouseButtonDown(0, 0);   /* held: never released (the shot, then quit) */
        }
        otlog("%s %d,%d -> %s -> modal=%s", a.c_str(), x, y, h ? h->GetAddress(false, false).c_str() : "-",
              g.modal.empty() ? "-" : g.modal.c_str());
    } else if (a == "wheel") {
        /* The mouse wheel, n notches down (up below 0), where the mouse is, as a player's would be: a
         * smooth scroll that the next steps may meet before it ends. */
        g.ctx->ProcessMouseWheel(Rml::Vector2f(0.0f, (float)atoi(b.c_str())), 0);
        otlog("wheel %s", b.c_str());
    } else if (a == "shot") {
        g.shot_pending = b;
    } else if (a == "boxes") {
        g.boxes_pending = b;
    } else if (a == "find") {
        g.disc_found = find_discs(false);   /* the "discs" dialog, even empty (no file dialog in a test) */
        g.model.DirtyVariable("disc_found");
        show_modal("discs");
    } else if (a == "disc") {
        set_disc(b);                  /* as if this disc image was chosen */
    } else if (a == "drop") {
        file_dropped(b);              /* as if this file was dropped on the window */
    } else if (a == "focus") {
        focus_back();                 /* as if the window got the focus back from another program */
    } else if (a == "size") {
        int w = 0, h = 0;             /* the window resized, as by a drag (in window units) */
        if (sscanf(b.c_str(), "%dx%d", &w, &h) == 2) SDL_SetWindowSize(g.window, w, h);
    } else if (a == "text") {
        g.ctx->ProcessTextInput(b);   /* typed into the focused field */
    } else if (g_pages.script_step && g_pages.script_step(a, b)) {
        /* a test step of an extra page */
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
        h.RegisterMember("changed", &Row::changed);
        h.RegisterMember("pending", &Row::pending);
        h.RegisterMember("idle", &Row::idle);
        h.RegisterMember("arrows", &Row::arrows);
        h.RegisterMember("toggle", &Row::toggle);
        h.RegisterMember("on", &Row::on);
        h.RegisterMember("plain", &Row::plain);
        h.RegisterMember("note", &Row::note);
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
    if (auto h = c.RegisterStruct<DiscFound>()) {
        h.RegisterMember("name", &DiscFound::name);
        h.RegisterMember("dir", &DiscFound::dir);
        h.RegisterMember("cur", &DiscFound::cur);
    }
    c.RegisterArray<std::vector<DiscFound>>();
    c.Bind("disc_found", &g.disc_found);
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
    c.Bind("help_extra", &g.help_extra);
    c.Bind("prev_src", &g.prev_src);
    c.Bind("prev_old", &g.prev_old);
    c.Bind("prev_label", &g.prev_label);
    c.Bind("help_pending", &g.help_pending);
    c.Bind("pad_active", &g.pad_active);
    c.Bind("page", &g.page);              /* the sidebar's icon and the hints follow it (tpl_*.rml) */
    c.Bind("have_disc", &g.have_disc);
    c.Bind("have_art", &g.have_art);
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
    c.Bind("ver_num", &g.ver_num);
    c.Bind("ver_note", &g.ver_note);
    c.Bind("inst_busy", &g.inst_busy);
    c.Bind("inst_word", &g.inst_word);
    c.BindFunc("version", [](Rml::Variant &v) { v = Rml::String(OT_VERSION); });
    c.Bind("saved_note", &g.saved_note);
    c.Bind("ini_note", &g.ini_note);
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
    c.Bind("inst_cancel", &g.inst_cancel);
    c.Bind("modal", &g.modal);
    c.Bind("cap_kind", &g.cap_kind);
    c.Bind("cap_for", &g.cap_for);
    c.Bind("cap_now", &g.cap_now);
    c.Bind("cap_hint", &g.cap_hint);
    c.Bind("conf_kind", &g.conf_kind);
    c.Bind("conf_line", &g.conf_line);
    c.Bind("conf_swap", &g.conf_swap);
    c.Bind("crash_head", &g.crash_head);
    c.Bind("crash_text", &g.crash_text);
    c.Bind("crash_when", &g.crash_when);
    c.Bind("crash_version", &g.crash_version);
    c.Bind("notice_kick", &g.notice_kick);
    c.Bind("notice_head", &g.notice_head);
    c.Bind("restore_version", &g.restore_version);
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
        /* A mouse click on an action row opens it only on its button (on a path row: its value). */
        if (g.mouse_click && (id >= 1000 || settings_def(id)->type == SETTING_PATH) &&
            !has_class_up(ev.GetTargetElement(), id >= 1000 ? "ab" : "plainv")) return;
        if (id >= 1000) { row_action(id); return; }
        const SettingDef *d = settings_def(id);
        if (d->type == SETTING_PATH) { choose_path(id); return; }
        if (d->type == SETTING_KEY) return;
        /* Enter / A (and a test's click:) step forward; a mouse click selects the row and only
         * its arrows or its switch change the value. */
        Rml::Element *t = ev.GetTargetElement();
        int dir = 1;
        if (g.mouse_click) {
            if (has_class_up(t, "left")) dir = -1;
            else if (!has_class_up(t, "right") && !has_class_up(t, "tog")) return;
        } else if (t && t->IsClassSet("left")) {
            dir = -1;
        }
        step_value(id, dir);
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
        if (!url) return;
        /* OT_OPENURL_FAIL: test only, as if there was no browser (Game Mode has none). */
        if (getenv("OT_OPENURL_FAIL") || !SDL_OpenURL(url)) {
            otlog("cannot open %s: %s", url, getenv("OT_OPENURL_FAIL") ? "test" : SDL_GetError());
            g.notice_kick = "LINK";
            g.notice_head = "COULDN'T OPEN THE BROWSER";
            g.notice_text = std::string("Open this address on another device or in a browser: ") + url;
            g.notice_file = "";
            for (const char *v : { "notice_kick", "notice_head", "notice_text", "notice_file" }) g.model.DirtyVariable(v);
            show_modal("notice");
        }
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
        /* the disc image and the save folder stay: a reset must not make the saves look gone */
        for (int id = 0; id < SETTINGS_COUNT; id++)
            if (settings_def(id)->tab == g.tab && id != S_DISC_IMAGE && id != S_SAVE_FOLDER)
                settings_set(g.set, id, settings_def(id)->def);
        rebuild_ctls();
        settings_changed(-1);
    });
    c.BindEventCallback("choose_disc", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) { choose_disc(); });
    c.BindEventCallback("disc_pick", [](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &a) {
        int i = a.empty() ? -1 : a[0].Get<int>();
        if (i < 0 || i >= (int)g.disc_found.size()) return;
        std::string p = g.disc_found[(size_t)i].path;
        close_modal();
        otlog("find discs: chosen %s", ot_file_name(p).c_str());
        dialog_chosen(S_DISC_IMAGE, p);   /* set_disc, by the main loop: it loads this dialog again */
    });
    if (g_pages.bind) g_pages.bind(c);
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

/* The installed version's "What's new": whatsnew.txt beside OpenTricky.exe
 * (the release zip has it, so its text can change without a new launcher),
 * else the block built in. A file for another version (left by an older
 * install, or copied in) is not used. */
void load_notes()
{
    const std::string file = g.base + "whatsnew.txt";
    std::string text;
    OtNotes n;
    SDL_PathInfo pi;
    if (ot_read_file(file, text, 64u << 10)) {
        n = ot_notes_parse(text);
        if (!n.ok) {
            otlog("what's new: whatsnew.txt has no launcher block: the one built in");
        } else if (!n.version.empty() && n.version != OT_VERSION_NUMBER && n.version != OT_VERSION) {
            otlog("what's new: whatsnew.txt is for v%s, not this version: the one built in", n.version.c_str());
            n = OtNotes();
        } else {
            otlog("what's new: whatsnew.txt");
        }
    } else if (SDL_GetPathInfo(file.c_str(), &pi)) {
        otlog("what's new: whatsnew.txt could not be read (or is too large): the one built in");
    }
    g.installed_notes = n.ok ? n : ot_notes_parse(k_whatsnew);
    if (!g.installed_notes.ok) {
        otlog("what's new: no launcher block built in");
        return;
    }
    show_notes(g.installed_notes, g.installed_notes.version.empty() ? OT_VERSION : g.installed_notes.version);
}

/* ── Is there a newer version? (ot_update.h) ─────────────────────────
 * One automatic check per start at most, in a thread of its own: the home
 * page shows at once with the version alone and the built-in notes, and the
 * pill changes if the list of releases comes within k_update_wait_ms; no
 * answer says nothing. Check now (a click on the pill when there is no
 * update to show, or Settings > Advanced > Updates) asks at once, even with
 * Check for updates = Never: the pill says "Checking...", then the answer,
 * or that it couldn't check. */

struct UpdateJob {
    std::mutex m;
    bool done = false;
    OtUpdateResult r;
};
std::shared_ptr<UpdateJob> g_update_job;
bool g_update_job_manual = false;     /* the running check is the player's (or the player clicked during it) */
bool g_update_late_logged = false;
const double k_update_manual_wait_ms = 10000;
const double k_update_click_gap_ms = 60000;     /* clicks reach GitHub once a minute at most (the cache answers between) */

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

/* The pill from what was read: "Update to vY", "Checking…" while a check asked by the player
 * runs, otherwise "vX" (up to date, never checked, no answer, Never, a skipped version, or this
 * build newer than any release). */
void update_apply()
{
    g.upd_state = "";
    g.upd_url.clear();
    g.upd_tag.clear();
    if ((!updates_off() || g.upd_manual) && g.upd_have) {
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
    /* A newer version stays shown while the player checks again. */
    if (g.upd_state != "update") {
        if (g_update_job && g_update_job_manual) g.upd_state = "checking";
        else if (g.upd_error) g.upd_state = "error";
    }
    if (g.upd_state == "update") g.version_text = "Update to v" + tag_version(g.upd_tag);
    /* quiet when nothing is to be done: up to date or not checked, the version only (the answer is in
     * Settings > Advanced > Updates); the pill speaks up for an update only */
    else if (g.upd_state == "checking") g.version_text = "Checking\xE2\x80\xA6";
    else if (g.upd_state == "error" && g.upd_fail_until > now_ms()) g.version_text = "Couldn't check";
    else g.version_text = std::string("v") + OT_VERSION;
    /* The label: the version, and a word for its state (none while nothing is known). */
    g.ver_num = g.upd_state == "update" ? "v" + tag_version(g.upd_tag) : std::string("v") + OT_VERSION;
    if (g.upd_state == "latest") g.ver_note = "Latest";
    else if (g.upd_state == "update") g.ver_note = "Available";
    else if (g.upd_state == "checking") g.ver_note = "Checking\xE2\x80\xA6";
    else if (g.upd_state == "error" && g.upd_fail_until > now_ms()) g.ver_note = "Couldn't check";
    else g.ver_note = "";
    if (g.upd_state != "update" && g.upd_view) update_back_to_installed();
    if (g.model)
        for (const char *v : { "version_text", "upd_state", "ver_num", "ver_note" }) g.model.DirtyVariable(v);
    update_row_refresh();
}

/* Settings > Advanced > Updates: the last answer. */
/* One message per row, a few words each: the value is the version (or the new one available), the button
 * says what happens: Check now, Checking..., Up to date (dimmed for a minute), Update, Try again.
 * A failed check is also said by the pill on the home page. */
std::string update_row_value()
{
    if (g.upd_state == "update") return "v" + tag_version(g.upd_tag) + " available";
    return std::string("v") + OT_VERSION;
}

std::string update_row_button(bool *idle)
{
    *idle = false;
    if (g.upd_state == "update") return "Update";
    if (g.upd_state == "checking") { *idle = true; return "Checking\xE2\x80\xA6"; }
    if (update_just_checked()) { *idle = true; return "Up to date"; }
    if (g.upd_error) return "Try again";
    return "Check now";
}

/* A second Check now within k_update_click_gap_ms only rereads the cache, so nothing would seem to happen.
 * After a manual check that found this version up to date, the button says so, dimmed and inert, for that
 * minute; then Check now works again. */
bool update_just_checked()
{
    return g.upd_state == "latest" && g.upd_manual && !g_update_job && now_ms() - g.upd_last_click < k_update_click_gap_ms;
}

void update_row_refresh()
{
    for (Row &r : g.rows)
        if (r.id == A_CHECK_UPDATES) {
            bool idle;
            std::string b = update_row_button(&idle);
            if (r.value == update_row_value() && r.button == b && r.idle == idle) continue;
            r.value = update_row_value();
            r.button = b;
            r.idle = idle;
            if (g.model) g.model.DirtyVariable("rows");
        }
}

void update_start(bool manual)
{
    if (g_update_job) {                 /* one at a time: a click during the check at start takes it over */
        if (manual && !g_update_job_manual) {
            g_update_job_manual = true;
            g.upd_manual = true;
            g.upd_error = false;
            otlog("updates: Check now during the check at start: its answer is shown");
            update_apply();
        }
        return;
    }
    if (!manual && g.upd_started) return;
    if (!manual && updates_off()) {
        otlog("updates: Check for updates = Never, no request");
        return;
    }
    OtUpdateRequest req;
    req.json_file = g.upd_test_json;
    req.url = g.upd_test_url.empty() ? k_url_api_releases : g.upd_test_url;
    req.cache_file = g.upd_test_json.empty() && g.upd_test_url.empty() ? g.upd_cache : "";
    req.allow_loopback_http = !g.upd_test_url.empty();
    req.user_agent = std::string("OpenTricky-launcher/") + OT_VERSION;
    req.timeout_ms = (int)(manual ? k_update_manual_wait_ms : k_update_wait_ms);
    if (req.json_file.empty() && req.cache_file.empty() && g.upd_test_url.empty()) {
        otlog("updates: no folder for the cache file, no request");     /* nothing would keep it to every 6 hours */
        return;
    }
    if (manual) {
        req.force = now_ms() - g.upd_last_click >= k_update_click_gap_ms;
        g.upd_last_click = now_ms();
        g.upd_manual = true;
        g.upd_error = false;
        g.upd_fail_until = 0;
        if (!g.upd_skip.empty()) {      /* the player asks: a skipped version shows again */
            otlog("updates: Check now forgets the skipped %s", g.upd_skip.c_str());
            g.upd_skip.clear();
            if (req.cache_file.empty()) otlog("updates: (test: the cache file is not changed)");
            else if (!ot_update_write_skip(req.cache_file, "")) otlog("updates: could not clear the skipped version");
        }
    } else {
        g.upd_started = true;
    }
    g_update_job_manual = manual;
    g_update_late_logged = false;
    g.upd_t0 = now_ms();
    auto job = std::make_shared<UpdateJob>();
    g_update_job = job;
    std::thread([job, req]() {
        OtUpdateResult r = ot_update_check(req);
        std::lock_guard<std::mutex> lk(job->m);
        job->r = std::move(r);
        job->done = true;
    }).detach();
    otlog("updates: %s started (%s)", manual ? (req.force ? "Check now" : "Check now (within a minute: the cache first)") : "check",
          !req.json_file.empty() ? "test file" : req.cache_file.empty() ? "test address" : "cache or GitHub");
    if (manual) update_apply();         /* "Checking..." */
}

/* From the main loop: takes the answer when it is there. */
void update_poll()
{
    static bool just = false;                 /* the minute after a Check now ends: the row comes back */
    if (just != update_just_checked()) { just = !just; update_row_refresh(); }
    if (!g_update_job) return;
    double waited = now_ms() - g.upd_t0;
    OtUpdateResult r;
    {
        std::lock_guard<std::mutex> lk(g_update_job->m);
        if (!g_update_job->done) {
            if (!g_update_job_manual && waited > k_update_wait_ms && !g_update_late_logged) {
                otlog("updates: no answer within %.0f ms: the version alone", k_update_wait_ms);
                g_update_late_logged = true;
            }
            return;
        }
        r = std::move(g_update_job->r);
    }
    g_update_job.reset();
    bool manual = g_update_job_manual;
    g_update_job_manual = false;
    otlog("updates: %s, %s, after %.0f ms (%d releases)%s", r.ok ? "read" : "nothing", r.source.c_str(), waited,
          (int)r.releases.size(), manual ? " (Check now)" : "");
    if (!r.ok) {
        g.upd_error = manual;           /* said only to a player who asked */
        if (manual) {
            g.upd_fail_until = now_ms() + 3000;      /* the pill says it for a moment, then is quiet again */
            update_apply();
        }
        return;
    }
    if (!manual && waited > k_update_wait_ms + 250) {      /* the main loop looks every 100 ms at most */
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
bool g_quit_after_install = false;
std::string g_install_version;

const OtRelease *update_release()
{
    for (const OtRelease &r : g.upd_releases)
        if (r.tag == g.upd_tag) return &r;
    return nullptr;
}

void install_texts()
{
    g.model.DirtyVariable("inst_cancel");
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
    if (!g.upd_test_url.empty()) req.kill_at = g.inst_test_kill_at;   /* tests only: with a local fake GitHub */
    std::string backup_version;
    g_install_restore = restore;
    g.inst_kick = restore ? "RESTORE" : (SDL_strcasecmp(settings_get(g.set, S_UPDATE_CHANNEL), "unstable") == 0 ? "UPDATE \xC2\xB7 BETA" : "UPDATE");
    g.inst_s1 = g.inst_s2 = g.inst_s3 = "";
    g.inst_bar = "0%";
    g.inst_l1 = "Download";
    g.inst_l3 = "Install";
    g.inst_foot = "";
    g.inst_cancel = false;
    if (restore) {
        if (!ot_install_has_backup(g.base, backup_version)) return;
        g_install_version = backup_version;
        g.inst_head = "RESTORING v" + backup_version;
    } else {
        const OtRelease *r = update_release();
        if (!r) return;
        g_install_version = tag_version(r->tag);
        g.inst_head = "UPDATING TO v" + g_install_version;
        const OtAsset *zip = nullptr, *sums = nullptr, *sig = nullptr;
        for (const OtAsset &a : r->assets) {
            std::string n = a.name;
            for (char &c : n) c = (char)tolower((unsigned char)c);
            if (n == "sha256sums.txt") sums = &a;
            else if (n == "sha256sums.txt.sig") sig = &a;
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
        req.sig_url = sig ? sig->url : "";
        req.version_new = g_install_version;
    }
    g.inst_text = "The game stays closed while it updates. Your settings, saves, disc image and HD pack are never touched.";
    if (!ot_install_writable(g.base)) {
        otlog("updates: the program's folder cannot be written");
        install_fail("nowrite", "CAN'T UPDATE HERE",
                     "OpenTricky can't write to its folder. Nothing was changed. Download v" + g_install_version +
                     " and copy what is in the folder inside its zip over this folder.");
        return;
    }
    if (ot_install_game_running(g.base)) {
        install_fail("fail", "THE GAME IS RUNNING", "Close SSX Tricky first. Nothing was changed.");
        return;
    }
    g.inst_phase = "run";
    g.inst_cancel = !restore;
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
        g.inst_l1 = st.cancel ? "Cancelling\xE2\x80\xA6" : b;
    }
    g.inst_cancel = !g_install_restore && step < 3 && !st.cancel && !st.done;
    g.inst_s3 = step > 3 ? "ok" : step == 3 ? "run" : "";
    g.inst_l3 = step >= 3 ? (g_install_restore ? "Restoring\xE2\x80\xA6" : "Installing\xE2\x80\xA6") : "Install";
    int pct = 0;
    if (g_install_restore) pct = step >= 4 ? 100 : 50;
    else if (step <= 1) pct = total > 0 ? (int)(got * 80 / total) : 0;
    else pct = step == 2 ? 85 : step == 3 ? 92 : 100;
    g.inst_bar = std::to_string(pct) + "%";
    g.inst_busy = !st.done.load();
    g.inst_word = g_install_restore ? "Restoring" : step <= 1 ? "Downloading" : step == 2 ? "Verifying" : "Installing";
    for (const char *v : { "inst_busy", "inst_word" }) g.model.DirtyVariable(v);
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
    g.inst_cancel = false;
    if (g_quit_after_install) g.quit = true;      /* the window was closed during the download */
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
    if (fail == "cancel") {             /* nothing changed: back where the player was */
        close_modal();
        return;
    }
    if (fail == "signature")
        install_fail("fail", "UPDATE REFUSED", "This update isn't signed by the OpenTricky project, so it wasn't installed. Nothing was changed. Get it from the project's releases page if you trust it.");
    else if (fail == "verify") install_fail("fail", "UPDATE STOPPED", "The update couldn't be verified. Nothing was changed.");
    else if (fail == "download") install_fail("fail", "UPDATE STOPPED", "The update couldn't be downloaded. Nothing was changed.");
    else if (fail == "busy") install_fail("fail", "THE GAME IS RUNNING", "Close SSX Tricky first. Nothing was changed.");
    else if (fail == "partial")
        install_fail("fail", g_install_restore ? "RESTORE STOPPED" : "UPDATE STOPPED",
                     "Some files are in use. Close SSX Tricky, then restart OpenTricky: the version you were using comes back as it was.");
    else if (g_install_restore) install_fail("fail", "RESTORE STOPPED", "The previous version couldn't be put back. Nothing was changed.");
    else install_fail("fail", "UPDATE STOPPED", "The update couldn't be installed. Nothing was changed.");
}

bool install_running() { return g_install != nullptr; }

/* The player's Cancel (the button, Esc / B, closing the window): an update
 * stops before its step 3, nothing changed. false: too late, or a Restore. */
bool install_cancel()
{
    if (!g_install || g_install_restore || g_install->step.load() >= 3) return false;
    if (!g_install->cancel.exchange(true)) otlog("updates: Cancel asked (step %d)", g_install->step.load());
    return true;
}

/* The pill, and the three buttons under the newer version's notes. */
void update_action(const std::string &a)
{
    if (a == "pill") {
        if (g.upd_state != "update") {          /* Check now (nothing while a check of the player's runs) */
            if (!(g_update_job && g_update_job_manual)) update_start(true);
            return;
        }
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

/* ── One launcher per install ─────────────────────────────────────────
 * A second launcher of the same folder (a double-click while the first is
 * open) brings the first one's window to the front and quits: two never
 * write settings.ini, nor update-tmp\ during an update, at once. The
 * launcher an update or a Restore starts waits for the one that installed
 * it to close. */

#ifdef _WIN32
HANDLE g_instance = nullptr;

BOOL CALLBACK raise_other_launcher(HWND w, LPARAM exe)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t title[32];
    if (pid == GetCurrentProcessId() || !IsWindowVisible(w) || GetWindowTextW(w, title, 32) <= 0 || wcscmp(title, L"OpenTricky") != 0)
        return TRUE;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    bool same = QueryFullProcessImageNameW(p, 0, path, &n) && _wcsicmp(path, (const wchar_t *)exe) == 0;
    CloseHandle(p);
    if (!same) return TRUE;
    if (no_activate()) return FALSE;    /* found, but left where it is */
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    SetForegroundWindow(w);
    return FALSE;
}
#endif

/* true: this is the install's only launcher (or no lock could be made). */
bool instance_lock(bool wait)
{
#ifdef _WIN32
    char name[128];
    if (!settings_instance_name("Launcher", name, sizeof name)) return true;
    g_instance = CreateMutexA(nullptr, FALSE, name);
    if (!g_instance) return true;
    DWORD r = WaitForSingleObject(g_instance, wait ? 30000 : 0);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) return true;   /* abandoned: the other one ended without letting go */
    CloseHandle(g_instance);
    g_instance = nullptr;
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) EnumWindows(raise_other_launcher, (LPARAM)exe);
    return false;
#else
    (void)wait;
    return true;
#endif
}

void instance_unlock()
{
#ifdef _WIN32
    if (!g_instance) return;
    ReleaseMutex(g_instance);
    CloseHandle(g_instance);
    g_instance = nullptr;
#endif
}

/* Under Wine, gamescope (Steam's Game Mode) or Steam's Big Picture: a full
 * screen launcher. The variables come from the Linux side. */
bool game_mode_screen(std::string &why)
{
    const char *o = getenv("OT_LAUNCHER_FULLSCREEN");
    if (o && *o) { why = std::string("OT_LAUNCHER_FULLSCREEN=") + o; return o[0] == '1'; }
    SettingsPlatform p;
    settings_platform_probe(&p, nullptr);
    if (!p.wine) { why = "not under Wine"; return false; }
    const char *gs = getenv("GAMESCOPE_WAYLAND_DISPLAY"), *de = getenv("XDG_CURRENT_DESKTOP"), *ui = getenv("SteamGamepadUI");
    if (gs && *gs) { why = "gamescope"; return true; }
    if (de && SDL_strcasestr(de, "gamescope")) { why = "XDG_CURRENT_DESKTOP=gamescope"; return true; }
    if (ui && !strcmp(ui, "1")) { why = "SteamGamepadUI=1"; return true; }
    why = "under Wine, a desktop";
    return false;
}

/* Resize the RmlUi context to the window. */
void fit_window(int pw, int ph)
{
    float dp = std::min(pw / 1280.0f, ph / 800.0f);
    g.rescaled = g.rescaled || dp != g.ctx->GetDensityIndependentPixelRatio();
    g.render->SetViewport(pw, ph);
    g.ctx->SetDimensions({ pw, ph });
    g.ctx->SetDensityIndependentPixelRatio(dp);
    apply_layout();
}

/* After a new scale, the texts are sized again. RmlUi 6.3 computes again,
 * on a new dp ratio, only the properties written in dp: those written with
 * a variable (font-size: var(--fs-l) in launcher.rcss, the variable holding
 * the dp) keep their old value, so after a live resize the boxes followed the
 * window but the texts kept their old size (a title wrapped on two lines, the
 * footer's links run together). Releasing the font sizes dirties the font size
 * of every element, which computes all of its properties again, variables
 * included, with no page loaded again: the dialog open, a key being captured,
 * the unsaved settings, the tab and the focus stay as they are. Once per frame
 * at most (a drag sends many sizes). A scrolled list keeps its offset in
 * pixels while its lines take their new size: once laid out (main loop), the
 * focus shown is brought back into view. */
void refit_texts()
{
    if (!g.rescaled) return;
    g.rescaled = false;
    Rml::ReleaseFontResources();
    g.focus_in_view = true;
}

/* What stops the launcher before it has a page. It has no console and its log
 * is off by default: Windows' own box, which needs nothing of SDL (SDL's fails
 * silently under a video driver that has none). */
void fatal_box(const std::string &text)
{
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    if (n > 0 && MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &w[0], n))
        MessageBoxW(nullptr, w.c_str(), L"OpenTricky", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenTricky", text.c_str(), nullptr);
#endif
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
        else if (a == "--find-root") { g.find_root = next(); if (!g.find_root.empty() && !ends_with_ci(g.find_root, "\\")) g.find_root += "\\"; }
        /* Tests of the update check: a local JSON file instead of GitHub, or
         * another https address (no cache read or written with either). */
        else if (a == "--updates-json") g.upd_test_json = next();
        else if (a == "--updates-url") g.upd_test_url = next();
        else if (a == "--updates-fail-at") g.inst_test_fail_at = next();   /* tests: an install that fails there */
        else if (a == "--updates-kill-at") g.inst_test_kill_at = next();   /* tests: an install cut there (OtInstallRequest) */
        else if (a == "--updated" || a == "--restored") { g.started_restored = a == "--restored"; g.started_after = next(); }
        else if (g_pages.arg && g_pages.arg(a, next)) {}   /* an option of an extra page */
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
    if (!instance_lock(!g.started_after.empty())) {
        otlog("another launcher of this folder is open: it is brought to the front, this one closes");
        return 3;
    }

    /* What stops the launcher before it has a page is said to the player,
     * except in the tools' runs, which read the log and the exit code. */
    const auto fatal = [&](const std::string &text, const char *detail) {
        std::string m = text;
        if (detail && *detail) m += std::string("\n\nDetails: ") + detail;
        if (!no_activate() && g.script.empty() && quit_after < 0) fatal_box(m);
        return 1;
    };
    const std::string game_itself = std::string("Update your graphics driver, then try again. You can also start the game itself: \xE2\x80\x9C") +
                                    k_game_exe + "\xE2\x80\x9D in the bin folder.";
    const std::string no_window = "OpenTricky couldn't open its window.\n\n" + game_itself;
    const std::string no_gl = "OpenTricky draws its window with OpenGL 3.3, which this computer doesn't offer. "
                              "This can happen without a graphics driver, in a remote desktop session or in a virtual machine.\n\n" + game_itself;
    /* OpenGL 3.3 is there, but the interface's shaders were refused: not a missing OpenGL. */
    const std::string no_renderer = "OpenTricky couldn't start its renderer.\n\n" + game_itself;

    double t0 = now_ms();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        otlog("error: SDL_Init: %s", SDL_GetError());
        return fatal(no_window, SDL_GetError());
    }
    double t_sdl = now_ms();
    const char *bp = SDL_GetBasePath();
    g.base = bp ? bp : "";
    {
        /* An update or a Restore cut half-way: put back (or finished) before
         * anything reads the program's files; the player is told at the first frame. */
        OtRecovery r = ot_install_recover(g.base);
        if (r.found) {
            otlog("updates: an interrupted %s was found: %s%s%s", r.kind.empty() ? "install" : r.kind.c_str(),
                  !r.ok ? (r.gave_up ? "not put right, given up" : "not put right yet") : r.finished ? "finished" : ("v" + r.version + " is back").c_str(),
                  r.detail.empty() ? "" : ": ", r.detail.c_str());
            g.notice_kick = "UPDATES";
            if (r.ok && r.finished) {
                g.notice_head = r.kind == "restore" ? "RESTORE FINISHED" : "UPDATE FINISHED";
                g.notice_text = r.kind == "restore" ? "The last restore is now complete. Your settings and saves are unchanged."
                                                    : "The last update is now complete. Your settings and saves stay (a preset takes its new values).";
            } else if (r.ok) {
                g.notice_head = r.kind == "restore" ? "RESTORE INTERRUPTED" : "UPDATE INTERRUPTED";
                g.notice_text = std::string("The last ") + (r.kind == "restore" ? "restore" : "update") + " was interrupted, so it was undone: v" +
                                r.version + " is back as it was. Your settings and saves were never touched.";
            } else {
                g.notice_head = r.kind == "restore" ? "RESTORE INTERRUPTED" : "UPDATE INTERRUPTED";
                g.notice_text = std::string("The last ") + (r.kind == "restore" ? "restore" : "update") +
                                (r.gave_up
                                     ? " was interrupted and couldn't be undone. Download OpenTricky from its releases page and copy what is in the folder inside the zip over this folder: your settings and saves stay."
                                     : " was interrupted and couldn't be undone yet (a file in use?). Close SSX Tricky, then start OpenTricky again.");
            }
            g.notice_file.clear();
            g.recovered = true;
        }
    }
    {
        /* The game in bin\ (0.1.2 and later): the copies of the program that
         * 0.1.1 had beside the launcher, left when a new version was unzipped
         * over it, go now -- only what is exactly 0.1.1's (ot_install.h). What
         * is in use or protected stays until the next start. */
        std::vector<std::string> removed, kept, failed;
        ot_install_tidy_root(g.base, removed, kept, failed);
        for (const std::string &n : removed) otlog("tidy: removed the old %s beside the launcher (the game is in bin\\)", n.c_str());
        for (const std::string &n : kept) {                     /* "<name>: <why>" */
            const size_t c = n.find(": ");
            otlog("tidy: kept %s beside the launcher: %s", n.substr(0, c).c_str(), c == std::string::npos ? "" : n.c_str() + c + 2);
        }
        for (const std::string &n : failed) otlog("tidy: %s beside the launcher could not be removed (in use, or protected?): next start", n.c_str());
        g.prog = ot_program_dir(g.base);
        otlog("program folder: %s", g.prog == g.base ? "beside the launcher" : "bin\\");
    }
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
        int bad = 0, st = settings_load(g.set, path, &bad);
        static const char *const names[] = { "read", "no file yet: defaults", "an older file (no [Meta] Version): ignored, defaults",
                                             "written by a newer version", "could not be read: defaults" };
        otlog("settings: %s", st >= 0 && st <= 4 ? names[st] : "?");
        if (bad) otlog("settings: %d line(s) not understood, not used: %s", bad, g.set->ignored);
        g.ini_note = ini_note_text(st, bad);
        if (st == SETTINGS_LOAD_MISSING) first_start_preset();
    }
    int at = settings_locate(nullptr, nullptr, 1, path, sizeof path);
    if (at != SETTINGS_AT_NONE) g.ini_write = path;
    g.ini_in_docs = at == SETTINGS_AT_DOCUMENTS;
    if (g.iso.empty()) g.iso = settings_get(g.set, S_DISC_IMAGE);
    else settings_set(g.set, S_DISC_IMAGE, g.iso.c_str());
    if (g.deck_note) save_settings_file();   /* the Steam Deck preset, written at once: one detection only */
    g.preset = settings_current_preset(g.set);
    for (int t = 0; t < SETTINGS_TAB_COUNT; t++) g.tabs.push_back(settings_tab_name(t));
    for (int p = 0; p < SETTINGS_PRESET_CUSTOM + 1; p++) g.presets.push_back(settings_preset_name(p));
    g.version_text = std::string("v") + OT_VERSION;   /* until the list of releases is read (update_apply) */
    g.ver_num = g.version_text;
    if (tab_arg >= 0 && tab_arg < SETTINGS_TAB_COUNT) g.tab = tab_arg;
    load_notes();
    {
        std::string ini = !g.ini_write.empty() ? g.ini_write : g.ini_read;
        if (!ini.empty()) g.upd_cache = ot_dir_of(ini) + "update-cache.txt";
        if (!ini.empty()) g.theme_order = ot_dir_of(ini) + "theme-order.txt";
    }

    /* Skip launcher: straight to the game, unless Shift / Select is held. */
    if (!no_skip && crash_test.empty() && g.script.empty() && !g.recovered && settings_get_bool(g.set, S_SKIP_LAUNCHER)) {
        if (launcher_key_held()) {
            otlog("skip launcher: Shift / Select held, the launcher shows");
        } else {
            OtDiscCheck d = disc_check(g.iso);
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
    if (no_activate()) {                        /* test tools: never the focus (show_launcher_window) */
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
        otlog("window: XBOX_NO_ACTIVATE: shown without the focus, behind the other windows");
    }
    g.window = SDL_CreateWindow("OpenTricky", ww, wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!g.window) {
        otlog("error: window: %s", SDL_GetError());
        return fatal(no_window, SDL_GetError());
    }
    /* The window resizes only into shapes the page is drawn for, 16:10
     * (1280 x 800, the Steam Deck) to 2:1 (the widest stage, 1600 dp), and
     * never too small to read. Full screen (Game Mode) is not bound by it. */
    SDL_SetWindowMinimumSize(g.window, 960, 600);
    SDL_SetWindowAspectRatio(g.window, 1.6f, 2.0f);
    {
        /* Steam's Game Mode (gamescope: a Steam Deck, Big Picture): the
         * launcher fills the screen, as the game does; on a desktop it stays
         * a window. OT_LAUNCHER_FULLSCREEN=1 / 0 decides instead. */
        std::string why;
        bool full = game_mode_screen(why);
        if (full && !size_arg.empty()) otlog("window: Game Mode (%s), but --size given: a window", why.c_str());
        else if (full) otlog("window: Game Mode (%s): full screen%s", why.c_str(), SDL_SetWindowFullscreen(g.window, true) ? "" : " refused");
        else otlog("window: %s: a window", why.c_str());
    }
    g.gl = SDL_GL_CreateContext(g.window);
    if (!g.gl) {
        otlog("error: OpenGL 3.3: %s", SDL_GetError());
        return fatal(no_gl, SDL_GetError());
    }
    SDL_GL_MakeCurrent(g.window, g.gl);
    SDL_GL_SetSwapInterval(1);
    Rml::String gl_message;
    if (!RmlGL3::Initialize(&gl_message)) {
        otlog("error: OpenGL 3.3: %s", gl_message.c_str());
        return fatal(no_gl, gl_message.c_str());
    }
    otlog("%s", gl_message.c_str());
    if (const GLubyte *r = glGetString(GL_RENDERER)) g.gl_renderer = (const char *)r;
    /* Without a driver, Windows' own OpenGL 1.1 comes back from SDL as a good
     * context: the renderer's functions below would be missing, and called. */
    bool gl_ok = GLAD_GL_VERSION_3_3 != 0;
    if (!gl_ok) {
        const GLubyte *v = glGetString(GL_VERSION);
        std::string found = "OpenGL " + std::string(v ? (const char *)v : "?") + ", " + g.gl_renderer;
        otlog("error: OpenGL 3.3: found %s", found.c_str());
        return fatal(no_gl, found.c_str());
    }
    double t_gl = now_ms();

    /* RmlUi. */
    OtSystemInterface system(g.window);
    OtRenderInterface render;
    bool render_ok = (bool)render;
    if (!render_ok) {
        otlog("error: renderer: the interface's shaders were refused (%s)", g.gl_renderer.c_str());
        return fatal(no_renderer, g.gl_renderer.c_str());
    }
    g.render = &render;
    render.maker = make_texture;
    Rml::SetSystemInterface(&system);
    Rml::SetRenderInterface(&render);
    Rml::SetFileInterface(&g.files);
    Rml::Initialise();
    const char *fonts[] = { "ui/fonts/Inter-Variable.ttf", "ui/fonts/Exo2-Italic-Variable.ttf" };
    for (const char *f : fonts)
        if (!Rml::LoadFontFace(g.prog + f, f == fonts[0])) otlog("font missing: %s", f);
    /* Bullet (House Industries): the logo, PLAY and the titles; Exo 2 Black
     * Italic (free) if the file is missing. */
    g.bullet = Rml::LoadFontFace(g.prog + "ui/fonts/bullet-smallcaps.otf", false, Rml::Style::FontWeight::Normal);
    otlog("display font: %s", g.bullet ? "Bullet" : "Exo 2 Black Italic (fallback)");
    double t_rml = now_ms();

    if (g.iso.empty()) auto_pick_disc();
    load_disc();
    if (g.disc_auto) save_settings_file();      /* remembered, as a chosen one */
    if (g.have_assets) {
        int t = track_arg;
        /* A track dealt at each start (theme_next_track), with no choice
         * anywhere; --track is for the project's tests only. */
        if (t < 0) t = theme_next_track();
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
    if (!g.home || !g.settings || !g.dialog) {
        int code = fatal("OpenTricky's interface files are missing or damaged.\n\nUnzip the whole release into one folder again.", nullptr);
        g.ctx = nullptr;
        Rml::Shutdown();
        return code;
    }
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

    /* Every controller is opened (a Steam Deck's own and a plugged one): any
     * button works; the stick that moves the focus is the one of the
     * controller last used. */
    std::vector<SDL_Gamepad *> pads;
    SDL_JoystickID pad_last = 0;
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
            if (!g.game) dialog_poll();           /* a path chosen by an input before is taken before this one */
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                if (!install_running()) g.quit = true;
                else if (install_cancel()) g_quit_after_install = true;      /* closes once the download has stopped */
                else otlog("close asked during an install: it finishes first");
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!key_down(ev.key.key, ev.key.scancode)) RmlSDL::InputEventHandler(g.ctx, g.window, ev);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                fit_window(ev.window.data1, ev.window.data2);
                break;
            case SDL_EVENT_GAMEPAD_ADDED: {
                bool open = false;
                for (SDL_Gamepad *p : pads) open = open || SDL_GetGamepadID(p) == ev.gdevice.which;
                if (open) break;
                SDL_Gamepad *p = SDL_OpenGamepad(ev.gdevice.which);
                if (p) pads.push_back(p);
                if (p && !pad_last) pad_last = ev.gdevice.which;
                otlog("controller: %s (%d open)", p ? SDL_GetGamepadName(p) : "?", (int)pads.size());
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (size_t i = 0; i < pads.size(); i++)
                    if (SDL_GetGamepadID(pads[i]) == ev.gdevice.which) {
                        SDL_CloseGamepad(pads[i]);
                        pads.erase(pads.begin() + (long)i);
                        break;
                    }
                if (pad_last == ev.gdevice.which) pad_last = pads.empty() ? 0 : SDL_GetGamepadID(pads[0]);
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (ev.gaxis.value > 20000 || ev.gaxis.value < -20000) pad_last = ev.gaxis.which;
                RmlSDL::InputEventHandler(g.ctx, g.window, ev);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
                if (ev.gbutton.which) pad_last = ev.gbutton.which;      /* 0: a test step's button */
                SDL_GamepadType t = SDL_GetGamepadTypeForID(ev.gbutton.which);
                g.pad_is_ps = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
                g.pad_game_ps = pad_is_ps_game(t);
                g.pad_pressed = true;
                update_hint_style();
                if (!g.game) pad_button(ev.gbutton.button);
                break;
            }
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                break;   /* the layout already follows the window's size (1280 x 800 dp stage) */
            case SDL_EVENT_MOUSE_MOTION:
                if (focus_shown())                  /* back on the mouse: no focus highlight */
                    if (Rml::Element *f = g.ctx->GetFocusElement()) f->SetPseudoClass("focus-visible", false);
                g.kbd_nav = false;
                set_pad_active(false);
                RmlSDL::InputEventHandler(g.ctx, g.window, ev);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                g.mouse_click = true;              /* a click dispatched now is the mouse's */
                RmlSDL::InputEventHandler(g.ctx, g.window, ev);
                g.mouse_click = false;
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                focus_back();
                RmlSDL::InputEventHandler(g.ctx, g.window, ev);
                break;
            case SDL_EVENT_DROP_FILE:
                if (ev.drop.data) file_dropped(ev.drop.data);
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
                if (g_pages.tick_hidden) g_pages.tick_hidden();
                continue;                       /* nothing to draw while the game runs */
            }
        }
        if (g.skip_mode && !g.game) { if (g.quit) break; continue; }
        if (!g.game && g.cap_until > 0 && g.modal == "capture" && now_ms() > g.cap_until) close_modal();
        if (g.modal != "capture") g.cap_until = 0;
        if (g.upd_fail_until > 0 && now_ms() > g.upd_fail_until) { g.upd_fail_until = 0; update_apply(); }
        stick_update(pad_last ? SDL_GetGamepadFromID(pad_last) : nullptr);
        dialog_poll();
        update_poll();
        install_poll();
        if (g_pages.poll) g_pages.poll();
        script_step();
        refit_texts();

        g.ctx->Update();
        fit_sized_pictures();
        if (g.focus_in_view) {
            g.focus_in_view = false;
            Rml::Element *f = g.ctx->GetFocusElement();
            if (f && f->IsPseudoClassSet("focus-visible")) {   /* the keyboard's or a controller's (refit_texts) */
                f->ScrollIntoView(Rml::ScrollAlignment::Nearest);
                g.ctx->Update();
            }
        }
        if (g.note_in_view >= 0) {
            /* the row and its note share one line of the list (settings.rml): laid out by the
             * Update above, that line is scrolled into view only if a part of it is out */
            Rml::Element *r = g.page == "settings" && g.settings ?
                g.settings->GetElementById("r" + std::to_string(g.note_in_view)) : nullptr;
            Rml::Element *line = r ? r->GetParentNode() : nullptr;
            g.note_in_view = -1;
            if (line && !in_view(line)) {
                line->ScrollIntoView(Rml::ScrollAlignment::Nearest);
                g.ctx->Update();
            }
        }
        if (g.focus_first_row && g.page == "settings" && g.settings && g.modal.empty()) {
            Rml::ElementList list;
            g.settings->QuerySelectorAll(list, ".row, .cell");
            if (!list.empty()) {
                list[0]->Focus(focus_shown());
                list[0]->ScrollIntoView(Rml::ScrollAlignment::Nearest);
                update_help(list[0]->GetAttribute<int>("rid", -1));
            }
            g.focus_first_row = false;
            g.ctx->Update();
        }
        if (g.focus_modal && g.dialog) {
            /* the first button, in view: Find my disc image's list opens again scrolled where it was left */
            Rml::ElementList list;
            g.dialog->QuerySelectorAll(list, ".sec .mbtn");
            for (Rml::Element *e : list)
                if (e->IsVisible(true)) {
                    e->Focus(focus_shown());
                    e->ScrollIntoView(Rml::ScrollAlignment::Nearest);
                    break;
                }
            g.focus_modal = false;
            g.ctx->Update();
        }
        render.Clear();
        render.BeginFrame();
        g.ctx->Render();
        render.EndFrame();
        if (!g.shot_pending.empty()) {
            if (!g.boxes_pending.empty()) save_boxes(g.boxes_pending);
            g.boxes_pending.clear();
            save_shot(g.shot_pending);
            g.shot_pending.clear();
        }
        SDL_GL_SwapWindow(g.window);
        frames++;
#ifdef _WIN32
        if (no_activate()) keep_out_of_front();
#endif
        if (first) {
            if (!g.skip_mode && !g.game) {
                show_launcher_window(false);
                g.shown = true;
            }
            first = false;
            if (!g.skip_mode) update_start();      /* after the first frame: never in its way */
            if (g.recovered && g.modal.empty()) {
                show_modal("notice");             /* its texts were set at start (ot_install_recover) */
            } else if (!g.started_after.empty() && g.modal.empty()) {
                std::string v;
                g.notice_kick = "UPDATES";
                g.notice_head = (g.started_restored ? "BACK TO v" : "UPDATED TO v") + g.started_after;
                g.notice_text = g.started_restored ? "The previous version is back. Your settings and saves are as they were."
                              : ot_install_has_backup(g.base, v) ? "Your settings and saves stay (a preset takes its new values). The previous version (v" + v +
                                                                     ") is kept: Settings \xE2\x80\xBA Advanced \xE2\x80\xBA Restore."
                                                                 : "Your settings and saves stay (a preset takes its new values).";
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
    for (SDL_Gamepad *p : pads) SDL_CloseGamepad(p);
    if (g_pages.shutdown) g_pages.shutdown();
    g.ctx = nullptr;
    Rml::Shutdown();
    RmlGL3::Shutdown();
    free_hd_cards();
    if (g.have_assets) ot_disc_free(&g.assets);
    SDL_GL_DestroyContext(g.gl);
    SDL_DestroyWindow(g.window);
    SDL_Quit();
    free(g.set);
    instance_unlock();
    return 0;
}
