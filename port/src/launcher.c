/*
 * launcher.c -- the game's side of the settings.
 *
 * The launcher window that used to be built into this executable is gone:
 * the launcher is OpenTricky.exe, a separate program. What
 * stays here is what the game needs at start: settings.ini read through the
 * settings registry (settings.h) into a LauncherConfig, the display shapes,
 * the save folder, the fork options set as XBOX_* variables, and the check
 * that a disc image is the build this executable was recompiled from.
 */
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <math.h>

#include "launcher.h"
#include "aspect.h"
#include "hud_anchor.h"
#include "settings.h"
#include "version.h"
#include "kernel/xbox_xdvdfs.h"

/* Path separators: Windows paths use '\', others '/'. The last separator of either
 * kind, so a path written on one host still splits on the other. */
#if defined(_WIN32)
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#include <sys/stat.h>
#endif
static char *last_sep(char *s)
{
    char *a = strrchr(s, '\\'), *b = strrchr(s, '/');
    return a > b ? a : b;
}

static BOOL to_path(const WCHAR *w, char *out, size_t n);   /* below */

/* ── Settings file ─────────────────────────────────────────────────── */

typedef struct { int w, h; } Res;

/* Render sizes offered, per shape. The first 4:3 entry is the console's own. */
static const Res k_res43[] = {
    { 640, 480 }, { 960, 720 }, { 1280, 960 }, { 1440, 1080 },
    { 1920, 1440 }, { 2560, 1920 }, { 2880, 2160 },
};
static const Res k_res169[] = {
    { 854, 480 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 },
    { 2560, 1440 }, { 3840, 2160 },
};
/* The usual monitor sizes of the wider shapes. */
static const Res k_res219[] = {
    { 2560, 1080 }, { 3440, 1440 }, { 3840, 1600 }, { 5120, 2160 },
};
static const Res k_res329[] = {
    { 3840, 1080 }, { 5120, 1440 }, { 7680, 2160 },
};
/* Auto: the primary monitor's own size, then the same shape at the common
 * heights below it. Filled by res_list. */
static Res s_res_auto[6];

static const char *const k_aspect_name[LAUNCHER_ASPECT_COUNT] = { "4:3", "16:9", "21:9", "32:9", "Auto" };

const char *launcher_aspect_name(int aspect)
{
    return (aspect >= 0 && aspect < LAUNCHER_ASPECT_COUNT) ? k_aspect_name[aspect] : k_aspect_name[0];
}

/* The presets offered for a shape. */
static int res_list(int aspect, const Res **list)
{
    switch (aspect) {
    case LAUNCHER_ASPECT_169: *list = k_res169; return (int)(sizeof k_res169 / sizeof k_res169[0]);
    case LAUNCHER_ASPECT_219: *list = k_res219; return (int)(sizeof k_res219 / sizeof k_res219[0]);
    case LAUNCHER_ASPECT_329: *list = k_res329; return (int)(sizeof k_res329 / sizeof k_res329[0]);
    case LAUNCHER_ASPECT_AUTO: {
        static const int heights[] = { 2160, 1440, 1200, 1080, 720 };
        int i, n = 0, mw = 1920, mh = 1080;
#if defined(_WIN32)
        DEVMODEW dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) {
            mw = (int)dm.dmPelsWidth;
            mh = (int)dm.dmPelsHeight;
        }
#endif
        {
            /* Test only: XBOX_MONITOR_SIZE=3440x1440 stands in for the monitor. */
            const char *e = getenv("XBOX_MONITOR_SIZE");
            int tw, th;
            if (e && sscanf(e, "%dx%d", &tw, &th) == 2 && tw >= 640 && th >= 480) { mw = tw; mh = th; }
        }
        for (i = (int)(sizeof heights / sizeof heights[0]) - 1; i >= 0; i--) {
            if (heights[i] < mh) {
                s_res_auto[n].h = heights[i];
                s_res_auto[n].w = ((int)((double)heights[i] * mw / mh + 0.5) + 1) & ~1;
                n++;
            }
        }
        s_res_auto[n].w = mw;
        s_res_auto[n].h = mh;
        *list = s_res_auto;
        return n + 1;
    }
    default: *list = k_res43; return (int)(sizeof k_res43 / sizeof k_res43[0]);
    }
}

double launcher_display_aspect(const LauncherConfig *cfg)
{
    if (cfg->aspect == LAUNCHER_ASPECT_43) return 4.0 / 3.0;
    if (cfg->aspect == LAUNCHER_ASPECT_169 || cfg->width <= 0 || cfg->height <= 0) return 16.0 / 9.0;
    return (double)cfg->width / (double)cfg->height;
}
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static uint32_t s_expected_entry = 0;

void launcher_init(uint32_t expected_entry_point)
{
    s_expected_entry = expected_entry_point;
}

/* UTF-8 (settings.ini) <-> the ANSI paths the runtime opens. A path outside
 * the code page becomes its short 8.3 form, or empty if it has none. */
#if !defined(_WIN32)
#include "launcher_posix.inc"
#endif
#if defined(_WIN32)
static BOOL u8_to_path(const char *u8, char *out, size_t n)
{
    WCHAR w[MAX_PATH];
    out[0] = '\0';
    if (!u8[0]) return TRUE;
    if (!MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, MAX_PATH)) return FALSE;
    if (!to_path(w, out, n)) { out[0] = '\0'; return FALSE; }
    return TRUE;
}
#endif /* _WIN32: u8_to_path */

#if defined(_WIN32)
static void path_to_u8(const char *ansi, char *out, size_t n)
{
    WCHAR w[MAX_PATH];
    out[0] = '\0';
    if (ansi[0] && MultiByteToWideChar(CP_ACP, 0, ansi, -1, w, MAX_PATH))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
}
#endif /* _WIN32: path_to_u8 */

/* The install root (settings_install_root): the folder of OpenTricky.exe and
 * settings.ini, where the saves (hdd\, portable.txt, Saves\) and a relative
 * HD pack or disc image are -- the parent of bin\ when the game is there. */
#if defined(_WIN32)
static void install_dir(char *out, size_t n)
{
    char u8[SETTINGS_VALUE_MAX];
    out[0] = '\0';
    if (settings_install_root(u8, sizeof u8)) u8_to_path(u8, out, n);
}
#endif /* _WIN32: install_dir */

#if defined(_WIN32)
BOOL launcher_install_root_w(WCHAR *out, size_t n)
{
    char u8[SETTINGS_VALUE_MAX];
    size_t k;
    if (!settings_install_root(u8, sizeof u8) || !MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, (int)n)) {
        if (n) out[0] = 0;
        return FALSE;
    }
    k = wcslen(out);
    if (k && out[k - 1] == L'\\') out[k - 1] = 0;      /* "D:\" -> "D:": callers add "\<name>" */
    return TRUE;
}
#endif /* _WIN32: launcher_install_root_w */

#if defined(_WIN32)
static BOOL is_absolute(const char *p)
{
    return (p[0] && p[1] == ':') || (p[0] == '\\' && p[1] == '\\');
}
#endif /* _WIN32: is_absolute */

#if defined(_WIN32)
void launcher_resolve_path(const char *path, char *out, size_t out_sz)
{
    char base[MAX_PATH], joined[2 * MAX_PATH];
    if (!path[0] || is_absolute(path)) { snprintf(out, out_sz, "%s", path); return; }
    install_dir(base, sizeof base);
    snprintf(joined, sizeof joined, "%s\\%s", base, path);
    if (!base[0] || !GetFullPathNameA(joined, (DWORD)out_sz, out, NULL)) snprintf(out, out_sz, "%s", path);
}
#endif /* _WIN32: launcher_resolve_path */

/* settings.ini (settings.h, settings_locate): in the install root, beside
 * OpenTricky.exe, or in Documents\My Games\SSX Tricky when only that one exists. */
static void config_path_u8(char *out, size_t out_sz)
{
    if (settings_locate(NULL, NULL, 0, out, out_sz) == SETTINGS_AT_NONE)
        snprintf(out, out_sz, "%s", SETTINGS_FILE_NAME);
}

void launcher_config_path(char *out, size_t out_sz)
{
    char u8[SETTINGS_VALUE_MAX];
    config_path_u8(u8, sizeof u8);
    if (!u8_to_path(u8, out, out_sz) || !out[0]) snprintf(out, out_sz, "%s", SETTINGS_FILE_NAME);
}

/* The largest preset of the given shape whose window fits the primary
 * monitor's work area: a sensible first-run default. */
#if defined(_WIN32)
static void default_resolution(int aspect, int *w, int *h)
{
    const Res *list;
    int n = res_list(aspect, &list);
    RECT work, frame = { 0, 0, 0, 0 };
    int i, best = 0;

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    for (i = 0; i < n; i++) {
        if (list[i].w + (frame.right - frame.left) <= work.right - work.left &&
            list[i].h + (frame.bottom - frame.top) <= work.bottom - work.top)
            best = i;
    }
    *w = list[best].w;
    *h = list[best].h;
}
#else
/* No window frame to allow for: the largest preset that fits 1920x1080. */
static void default_resolution(int aspect, int *w, int *h)
{
    const Res *list;
    int n = res_list(aspect, &list), i, best = 0;
    for (i = 0; i < n; i++)
        if (list[i].w <= 1920 && list[i].h <= 1080) best = i;
    *w = list[best].w;
    *h = list[best].h;
}
#endif /* _WIN32: default_resolution */

/* Resolution=auto (settings.ini): in fullscreen the monitor's own size (the
 * largest preset of the shape that fits it, for a fixed shape); in a window
 * the largest preset that fits, as for a new player before. */
static void auto_resolution(int aspect, int fullscreen, int *w, int *h)
{
    const Res *list;
    int n, i, mw, mh, best = 0;
    if (!fullscreen) { default_resolution(aspect, w, h); return; }
    n = res_list(LAUNCHER_ASPECT_AUTO, &list);
    mw = list[n - 1].w;
    mh = list[n - 1].h;
    if (aspect == LAUNCHER_ASPECT_AUTO) { *w = mw; *h = mh; return; }
    n = res_list(aspect, &list);
    for (i = 0; i < n; i++)
        if (list[i].w <= mw && list[i].h <= mh) best = i;
    *w = list[best].w;
    *h = list[best].h;
}

/* ── Fork options ───────────────────────────────────────────── */

/* The defaults are the program's own with no variable set: no SMAA, every
 * fidelity fix on, 8 audio buffers -- for test runs whose XBOX_FORK_INI
 * names no file. A player's defaults are the settings registry's (settings.h). */
static void fork_defaults(LauncherConfig *c)
{
    c->smaa = 0;
    c->fidelity = 1;
    c->save_backup = 1;                 /* XBOX_SAVE_BACKUP's default */
    c->smooth_motion = 1;               /* XBOX_FPS_INTERP's default */
    c->smooth_legs = 0;                 /* XBOX_FPS_SMOOTH_LEGS's default */
    c->unlock_all = 0;                  /* XBOX_UNLOCK_ALL's default: My save */
    c->fix_cullwind = c->fix_occlusion = c->fix_texpassthru = c->fix_gamma = 1;
    c->audio_queue = 8;
    c->audio_out = 0;                   /* auto */
    c->fix_kickwait = 1;
    c->fps_cap = -2;                    /* the monitor's refresh rate: 60 on a 60 Hz screen */
    c->sync = LAUNCHER_SYNC_DEFAULT;
    c->soft_shadows = 0;
    c->draw_dist = 0;
    c->hd_textures = 0;
    c->hd_menus = 0;
    c->btn_icons = 0;                   /* Auto */
    c->menus = ASPECT_MENUS_DEFAULT;
    c->hd_path[0] = '\0';
}

/* SMAA presets, as XBOX_SMAA_PRESET names them; index = LauncherConfig.smaa. */
static const char *const k_smaa_name[] = { "off", "low", "medium", "high", "ultra" };
/* Presentation, as Sync and XBOX_SYNC name them; index = LauncherConfig.sync.
 * legacy = the original port's (no launcher choice, .ini or XBOX_SYNC only). */
static const char *const k_sync_name[] = { "legacy", "off", "vsync", "adaptive" };
/* Speaker output, as AudioOutput and XBOX_AUDIO_OUTPUT name them; index = LauncherConfig.audio_out. */
static const char *const k_audioout_name[] = { "auto", "stereo", "5.1" };
/* Draw distance, as DrawDistance and XBOX_DRAW_DISTANCE name them. */
static const char *const k_drawdist_name[] = { "original", "far", "max" };
/* Button style, as ButtonIcons and XBOX_BUTTON_ICONS name them (nv2a_btnicons.h). */
static const char *const k_icons_name[] = { "auto", "modern", "playstation", "ps2" };
#define N_ICONS 4
#define ICONS_PS2 3
/* The PS2 button style is built in unless CMake has
 * -DSSX_PS2_BUTTONS=OFF. In such a build that choice is only announced,
 * greyed ("coming soon"), and cannot be picked; a saved "ps2" reads as Auto
 * (PS2_ORIGINAL_READY in nv2a_btnicons.c says the same to the game). */
#ifdef SSX_PS2_BUTTONS_READY
#define ICONS_PS2_READY 1
#else
#define ICONS_PS2_READY 0
#endif

/* settings.ini -> the options the code reads as XBOX_* variables
 * (launcher_fork_apply). The choices' order in settings.c is the order of
 * the k_*_name lists above. */
static void fork_from_settings(LauncherConfig *c, const Settings *s)
{
    const char *fps = settings_get(s, S_FPS_LIMIT), *vs = settings_get(s, S_VSYNC);
    int i;

    i = settings_choice_index(s, S_SMAA);
    c->smaa = i > 0 ? i : 0;
    c->fidelity        = settings_get_bool(s, S_LOOK_LIKE_XBOX);
    c->fix_cullwind    = settings_get_bool(s, S_FIX_CULL_WINDING);
    c->fix_occlusion   = settings_get_bool(s, S_FIX_OCCLUSION);
    c->fix_texpassthru = settings_get_bool(s, S_FIX_TEX_PASSTHROUGH);
    c->fix_gamma       = settings_get_bool(s, S_FIX_GAMMA);
    c->fix_kickwait    = settings_get_bool(s, S_FIX_KICKWAIT);
    c->audio_queue = settings_get_int(s, S_AUDIO_DELAY);
    if (c->audio_queue < 2 || c->audio_queue > 20) c->audio_queue = 8;
    i = settings_choice_index(s, S_SPEAKERS);
    c->audio_out = i > 0 ? i : 0;
    c->fps_cap = !strcmp(fps, "monitor") ? -2 : !strcmp(fps, "unlimited") ? -1 : atoi(fps);
    if (c->fps_cap != -1 && c->fps_cap != -2 && (c->fps_cap < 60 || c->fps_cap > 1000)) c->fps_cap = 60;
    c->sync = !strcmp(vs, "off") ? LAUNCHER_SYNC_OFF : !strcmp(vs, "adaptive") ? LAUNCHER_SYNC_ADAPTIVE : LAUNCHER_SYNC_VSYNC;
    c->soft_shadows = settings_get_bool(s, S_SOFT_SHADOWS);
    i = settings_choice_index(s, S_DRAW_DISTANCE);
    c->draw_dist = i > 0 ? i : 0;
    c->hd_textures = settings_get_bool(s, S_HD_TEXTURES);
    c->hd_menus    = settings_get_bool(s, S_HD_TEXTURES_MENUS);
    c->menus = aspect_menus_parse(settings_get(s, S_MENUS), ASPECT_MENUS_DEFAULT);   /* "4:3" / "16:9" */
    u8_to_path(settings_get(s, S_HD_PACK_FOLDER), c->hd_path, sizeof c->hd_path);
    /* The controls are always in the PS2 layout (ctlscheme.h). A saved "ps2"
     * is Auto in a build without the PS2 buttons. */
    i = settings_choice_index(s, S_BUTTON_STYLE);
    c->btn_icons = (i > 0 && (ICONS_PS2_READY || i != ICONS_PS2)) ? i : 0;
    c->save_backup   = settings_get_bool(s, S_SAVE_BACKUP);
    c->smooth_motion = settings_get_bool(s, S_SMOOTH_MOTION);
    c->smooth_legs   = settings_get_bool(s, S_SMOOTH_LEGS);
    c->unlock_all    = settings_choice_index(s, S_SAVE_PROFILE) == 1;   /* "unlockall" */
}

/* A path setting from the LauncherConfig: kept as it is in the file (relative,
 * or with characters outside the code page) when it still names the same path,
 * or when this side could not have it at all (no ANSI form: left empty here). */
static void path_to_settings(Settings *s, int id, const char *ansi)
{
    char now[MAX_PATH], u8[SETTINGS_VALUE_MAX];
    BOOL ok = u8_to_path(settings_get(s, id), now, sizeof now);
    if ((ok && !strcmp(now, ansi)) || (!ok && !ansi[0])) return;
    path_to_u8(ansi, u8, sizeof u8);
    settings_set(s, id, u8);
}

static void fork_to_settings(Settings *s, const LauncherConfig *c)
{
    char v[16];
    settings_set(s, S_SMAA, k_smaa_name[(c->smaa >= 0 && c->smaa <= 4) ? c->smaa : 0]);
    settings_set(s, S_LOOK_LIKE_XBOX, c->fidelity ? "1" : "0");
    settings_set(s, S_FIX_CULL_WINDING, c->fix_cullwind ? "1" : "0");
    settings_set(s, S_FIX_OCCLUSION, c->fix_occlusion ? "1" : "0");
    settings_set(s, S_FIX_TEX_PASSTHROUGH, c->fix_texpassthru ? "1" : "0");
    settings_set(s, S_FIX_GAMMA, c->fix_gamma ? "1" : "0");
    settings_set(s, S_FIX_KICKWAIT, c->fix_kickwait ? "1" : "0");
    snprintf(v, sizeof v, "%d", c->audio_queue);
    settings_set(s, S_AUDIO_DELAY, v);                 /* not one of the choices: unchanged */
    settings_set(s, S_SPEAKERS, k_audioout_name[(c->audio_out >= 0 && c->audio_out <= 2) ? c->audio_out : 0]);
    if (c->fps_cap == -2) snprintf(v, sizeof v, "monitor");
    else if (c->fps_cap == -1) snprintf(v, sizeof v, "unlimited");
    else snprintf(v, sizeof v, "%d", c->fps_cap);
    settings_set(s, S_FPS_LIMIT, v);
    settings_set(s, S_VSYNC, c->sync == LAUNCHER_SYNC_OFF ? "off" : c->sync == LAUNCHER_SYNC_ADAPTIVE ? "adaptive" : "on");
    settings_set(s, S_SOFT_SHADOWS, c->soft_shadows ? "1" : "0");
    settings_set(s, S_DRAW_DISTANCE, k_drawdist_name[(c->draw_dist >= 0 && c->draw_dist <= 2) ? c->draw_dist : 0]);
    settings_set(s, S_HD_TEXTURES, c->hd_textures ? "1" : "0");
    settings_set(s, S_HD_TEXTURES_MENUS, c->hd_menus ? "1" : "0");
    settings_set(s, S_MENUS, aspect_menus_name(c->menus));
    path_to_settings(s, S_HD_PACK_FOLDER, c->hd_path);
    settings_set(s, S_BUTTON_STYLE, k_icons_name[(c->btn_icons >= 0 && c->btn_icons < N_ICONS) ? c->btn_icons : 0]);
    settings_set(s, S_SAVE_BACKUP, c->save_backup ? "1" : "0");
    settings_set(s, S_SMOOTH_MOTION, c->smooth_motion ? "1" : "0");
    settings_set(s, S_SMOOTH_LEGS, c->smooth_legs ? "1" : "0");
    settings_set(s, S_SAVE_PROFILE, c->unlock_all ? "unlockall" : "mine");
}

/* XBOX_FORK_INI (test runs): the options of a settings.ini (settings.h);
 * the code's own defaults when there is no such file, or for an old .ini. */
void launcher_fork_load(LauncherConfig *cfg, const char *ini)
{
    static Settings s;
    char u8[SETTINGS_VALUE_MAX];
    int r;

    fork_defaults(cfg);
    if (!ini || GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;
    path_to_u8(ini, u8, sizeof u8);
    r = settings_load(&s, u8, NULL);
    if (r != SETTINGS_LOAD_OK && r != SETTINGS_LOAD_NEWER) {
        printf("Settings:  %s is not a settings.ini v%d: the code's defaults\n", ini, SETTINGS_VERSION);
        return;
    }
    fork_from_settings(cfg, &s);
}

/* Set one variable unless the environment has it; report the effective value. */
static void fork_set(char *line, size_t n, const char *name, const char *value,
                     const char *source)
{
    const char *had = getenv(name);
    size_t len = strlen(line);
    if (had) {
        snprintf(line + len, n - len, " %s=%s (env)", name, had);
        return;
    }
    if (!value || !source) {          /* nothing to set: the code's default */
        snprintf(line + len, n - len, " %s=- (default)", name);
        return;
    }
    _putenv_s(name, value);
    snprintf(line + len, n - len, " %s=%s (%s)", name, value, source);
}

void launcher_fork_apply(const LauncherConfig *cfg, const char *source)
{
    char line[768 + MAX_PATH] = "Fork:      ", q[16], cap[16], hd[MAX_PATH];
    int on = cfg->fidelity != 0;
    int smaa = (cfg->smaa >= 0 && cfg->smaa <= 4) ? cfg->smaa : 0;

    /* settings.ini may name the HD pack relative to the install root ("hd-pack") */
    launcher_resolve_path(cfg->hd_path, hd, sizeof hd);

    fork_set(line, sizeof line, "XBOX_SMAA", smaa ? "1" : "0", source);
    /* the preset only means something with SMAA on */
    fork_set(line, sizeof line, "XBOX_SMAA_PRESET", smaa ? k_smaa_name[smaa] : NULL, source);
    fork_set(line, sizeof line, "XBOX_FIX_CULLWIND",    (on && cfg->fix_cullwind)    ? "1" : "0", source);
    fork_set(line, sizeof line, "XBOX_FIX_OCCLUSION",   (on && cfg->fix_occlusion)   ? "1" : "0", source);
    fork_set(line, sizeof line, "XBOX_FIX_TEXPASSTHRU", (on && cfg->fix_texpassthru) ? "1" : "0", source);
    fork_set(line, sizeof line, "XBOX_FIX_GAMMA",       (on && cfg->fix_gamma)       ? "1" : "0", source);
    /* Performance, not fidelity -- independent of FidelityFixes. */
    fork_set(line, sizeof line, "XBOX_FIX_KICKWAIT", cfg->fix_kickwait ? "1" : "0", source);
    snprintf(q, sizeof q, "%d", cfg->audio_queue);
    fork_set(line, sizeof line, "XBOX_AUDIO_QUEUE", q, source);
    fork_set(line, sizeof line, "XBOX_AUDIO_OUTPUT",
             k_audioout_name[(cfg->audio_out >= 0 && cfg->audio_out <= 2) ? cfg->audio_out : 0], source);
    /* -1 = no limit, which XBOX_FPS_CAP writes as 0; 60 = hook not installed. */
    if (cfg->fps_cap == -2) snprintf(cap, sizeof cap, "monitor");
    else snprintf(cap, sizeof cap, "%d", cfg->fps_cap == -1 ? 0 : cfg->fps_cap);
    fork_set(line, sizeof line, "XBOX_FPS_CAP", cap, source);
    fork_set(line, sizeof line, "XBOX_SYNC", k_sync_name[(cfg->sync >= 0 && cfg->sync <= 3) ? cfg->sync : 0], source);
    /* Visual enhancement, Off by default (as on the Xbox). */
    fork_set(line, sizeof line, "XBOX_SOFT_SHADOWS", cfg->soft_shadows ? "1" : "0", source);
    /* the save backup copy and the frames in between above 60 */
    fork_set(line, sizeof line, "XBOX_SAVE_BACKUP", cfg->save_backup ? "1" : "0", source);
    fork_set(line, sizeof line, "XBOX_FPS_INTERP", cfg->smooth_motion ? "1" : "0", source);
    fork_set(line, sizeof line, "XBOX_FPS_SMOOTH_LEGS", cfg->smooth_legs ? "1" : "0", source);
    /* Save profile: Unlock all = the game's own cheat and a save folder of its own (main.c) */
    fork_set(line, sizeof line, "XBOX_UNLOCK_ALL", cfg->unlock_all ? "1" : "0", source);
    /* Original = no hook. */
    fork_set(line, sizeof line, "XBOX_DRAW_DISTANCE",
             k_drawdist_name[(cfg->draw_dist >= 0 && cfg->draw_dist <= 2) ? cfg->draw_dist : 0], source);
    /* No pack folder: nothing; "0" = off (no index read). */
    fork_set(line, sizeof line, "XBOX_HD_TEXTURES",
             (cfg->hd_textures && hd[0]) ? hd : "0", source);
    fork_set(line, sizeof line, "XBOX_HD_TEXTURES_MENUS", cfg->hd_menus ? "1" : "0", source);
    /* Never set: the PS2 layout; reported when a debug run sets it. */
    fork_set(line, sizeof line, "XBOX_CONTROL_SCHEME", NULL, NULL);
    fork_set(line, sizeof line, "XBOX_BUTTON_ICONS",
             k_icons_name[(cfg->btn_icons >= 0 && cfg->btn_icons < N_ICONS) ? cfg->btn_icons : 0], source);
    {   /* the shader disk cache, beside settings.ini (the path is not reported) */
        char dir[MAX_PATH], *slash;
        launcher_config_path(dir, sizeof dir);
        slash = last_sep(dir);
        if (slash) slash[1] = '\0';
        else dir[0] = '\0';
        if (!getenv("XBOX_SHADER_CACHE_DIR") && strlen(dir) + 14 < sizeof dir) {
            strcat(dir, "cache" PATH_SEP "shaders");
            _putenv_s("XBOX_SHADER_CACHE_DIR", dir);
        }
    }
    {   /* menus: applied before aspect_init (aspect_set_menus); the effective one */
        size_t len = strlen(line);
        snprintf(line + len, sizeof line - len, " Menus=%s", g_aspect_menus43 && g_aspect_hook_on ? "4:3" : "16:9");
    }
    printf("%s\n", line);
}

/* The whole configuration from settings.ini (settings.h): the choices'
 * order in settings.c is that of LAUNCHER_ASPECT_*, the k_*_name lists
 * above and PAD_* (controls.h). */
static void config_from_settings(LauncherConfig *c, const Settings *s)
{
    const char *hud = settings_get(s, S_RACE_HUD);
    int i;

    memset(c, 0, sizeof *c);
    u8_to_path(settings_get(s, S_DISC_IMAGE), c->iso, sizeof c->iso);
    snprintf(c->hdd, sizeof c->hdd, "%s", settings_get(s, S_SAVE_FOLDER));  /* UTF-8; "": automatic */
    i = settings_choice_index(s, S_ASPECT);
    c->aspect = i >= 0 ? i : LAUNCHER_ASPECT_AUTO;
    c->wide_fov = aspect_fov_parse(settings_get(s, S_FOV), ASPECT_FOV_NOSTRETCH);
    if (!strcmp(hud, "xbox")) {
        c->hud_shape = HUD_SHAPE_XBOX;
        c->hud_size = HUD_SIZE_DEFAULT;
    } else {
        c->hud_shape = HUD_SHAPE_PROPORTIONAL;
        c->hud_size = hud_anchor_size_parse(hud, HUD_SIZE_DEFAULT);
    }
    c->fullscreen = settings_choice_index(s, S_DISPLAY_MODE) == 1;
    if (!settings_get_resolution(s, S_RESOLUTION, &c->width, &c->height))
        auto_resolution(c->aspect, c->fullscreen, &c->width, &c->height);
    c->aniso = settings_get_int(s, S_TEX_FILTER);           /* "off" -> 0 */
    c->msaa = 1;                                             /* MSAA is no longer offered */
    c->show_fps = settings_get_bool(s, S_SHOW_FPS);
    c->log_file = settings_get_bool(s, S_LOG_FILE);
    i = settings_choice_index(s, S_LAUNCHER_SOUNDS);
    c->menu_sounds = i >= 0 ? i : 1;
    c->launcher_music = settings_get_bool(s, S_LAUNCHER_MUSIC);
    controls_from_settings(&c->controls, s);
    fork_from_settings(c, s);
}

/* The other way, onto the values read from the file, so what this window
 * does not show (the separate launcher's own settings, hidden ones) stays. */
static void config_to_settings(Settings *s, const LauncherConfig *c)
{
    char v[32];
    int w, h;

    path_to_settings(s, S_DISC_IMAGE, c->iso);
    settings_set(s, S_SAVE_FOLDER, c->hdd);
    settings_set(s, S_ASPECT, launcher_aspect_name(c->aspect));
    settings_set(s, S_FOV, aspect_fov_name(c->wide_fov));
    if (c->hud_shape == HUD_SHAPE_XBOX) snprintf(v, sizeof v, "xbox");
    else snprintf(v, sizeof v, "%d", hud_anchor_size_value(c->hud_size));
    settings_set(s, S_RACE_HUD, v);
    settings_set(s, S_DISPLAY_MODE, c->fullscreen ? "fullscreen" : "window");
    /* Resolution=auto stays auto while the size is still the automatic one */
    auto_resolution(c->aspect, c->fullscreen, &w, &h);
    if (!(settings_get_resolution(s, S_RESOLUTION, NULL, NULL) == 0 && w == c->width && h == c->height)) {
        snprintf(v, sizeof v, "%dx%d", c->width, c->height);
        settings_set(s, S_RESOLUTION, v);
    }
    if (c->aniso) snprintf(v, sizeof v, "%d", c->aniso);
    else snprintf(v, sizeof v, "off");
    settings_set(s, S_TEX_FILTER, v);
    settings_set(s, S_SHOW_FPS, c->show_fps ? "1" : "0");
    settings_set(s, S_LOG_FILE, c->log_file ? "1" : "0");
    settings_set(s, S_LAUNCHER_SOUNDS, c->menu_sounds <= 0 ? "off" : c->menu_sounds == 1 ? "low" : "medium");
    settings_set(s, S_LAUNCHER_MUSIC, c->launcher_music ? "1" : "0");
    controls_to_settings(&c->controls, s);
    fork_to_settings(s, c);
}

/* "SSX Tricky.log", in the folder of settings.ini (the install root, or
 * Documents when it cannot be written). */
void launcher_log_path(char *out, size_t out_sz)
{
    char ini[MAX_PATH], exe[MAX_PATH], *slash, *name, *dot;
    DWORD k;
    launcher_config_path(ini, sizeof ini);
    slash = last_sep(ini);
    if (slash) slash[1] = '\0';
    else ini[0] = '\0';
    k = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (k == 0 || k >= sizeof exe) snprintf(exe, sizeof exe, "SSX Tricky.exe");
    name = last_sep(exe);
    name = name ? name + 1 : exe;
    dot = strrchr(name, '.');
    if (dot) *dot = '\0';
    snprintf(out, out_sz, "%s%s.log", ini, name);
}

/* A folder with something in it (the saves of an earlier version). */
#if defined(_WIN32)
static BOOL folder_has_files(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH + 4];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    BOOL any = FALSE;
    _snwprintf(pat, MAX_PATH + 4, L"%ls\\*", dir);
    pat[MAX_PATH + 3] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) any = TRUE;
    } while (!any && FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}
#endif /* _WIN32: folder_has_files */

/* Documents\My Games\SSX Tricky\Saves, created if missing. The
 * real Documents folder (moved, or synced by OneDrive, it still works).
 * `out` gets the wide path whenever Documents is known. The answer is the
 * one earlier versions gave -- TRUE only when the path also has an ANSI or
 * short 8.3 form -- so the folder chosen for a player never moves (saves
 * already beside the game stay found); the game itself opens it wide. */
#if defined(_WIN32)
static BOOL documents_saves(WCHAR *out, size_t n)
{
    PWSTR docs = NULL;
    WCHAR sh[MAX_PATH];
    char a[MAX_PATH * 2];
    BOOL lossy = FALSE;
    out[0] = L'\0';
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &docs)) || !docs) return FALSE;
    _snwprintf(out, n, L"%ls\\My Games\\SSX Tricky\\Saves", docs);
    out[n - 1] = 0;
    CoTaskMemFree(docs);
    SHCreateDirectoryExW(NULL, out, NULL);
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, out, -1, a, (int)sizeof a, NULL, &lossy) && !lossy)
        return TRUE;
    lossy = FALSE;
    return GetShortPathNameW(out, sh, MAX_PATH) &&      /* a user name outside the code page */
           WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, sh, -1, a, (int)sizeof a, NULL, &lossy) && !lossy;
}
#endif /* _WIN32: documents_saves */

int launcher_unlock_all(void)
{
    const char *e = getenv("XBOX_UNLOCK_ALL");
    return e && e[0] == '1';
}

int launcher_unlock_all_folder(char *save_u8, size_t sz)
{
    WCHAR w[MAX_PATH];
    size_t n = strlen(save_u8);
    while (n && (save_u8[n - 1] == '\\' || save_u8[n - 1] == '/')) save_u8[--n] = '\0';
    if (!n || n + 1 + strlen(LAUNCHER_UNLOCK_ALL_DIR) >= sz) return 0;
    strcat(save_u8, PATH_SEP LAUNCHER_UNLOCK_ALL_DIR);
    /* with its parents: the save folder itself may not exist yet, and the
     * runtime creates only the last level (xbox_path_init) */
#if defined(_WIN32)
    if (MultiByteToWideChar(CP_UTF8, 0, save_u8, -1, w, MAX_PATH))
        SHCreateDirectoryExW(NULL, w, NULL);
#else
    (void)w;
    {
        char *c;
        for (c = save_u8 + 1; *c; c++)          /* mkdir -p */
            if (*c == '/') { *c = '\0'; mkdir(save_u8, 0777); *c = '/'; }
        mkdir(save_u8, 0777);
    }
#endif
    return 1;
}

/* Where the saves go. A SaveFolder set in the .ini (chosen in
 * Advanced, a test pack's, an older .ini's) is used as it is. Left empty, the
 * folder is automatic:
 *   1. hdd\ in the install root (beside OpenTricky.exe, the game too before
 *      bin\), if it holds saves (an earlier version's default): used where
 *      it is -- nothing is moved, nothing can be lost;
 *   2. Saves\ in the install root if portable.txt is there (a copyable install);
 *   3. Documents\My Games\SSX Tricky\Saves, as PC games do (works when the
 *      game is installed where it cannot write, such as Program Files);
 *      Saves\ in the install root if Documents cannot be used. */
#if defined(_WIN32)
int launcher_save_kind(const LauncherConfig *cfg)
{
    WCHAR base[MAX_PATH], p[MAX_PATH + 16], d[MAX_PATH];
    if (cfg->hdd[0]) return LAUNCHER_SAVES_CHOSEN;
    launcher_install_root_w(base, MAX_PATH);
    _snwprintf(p, MAX_PATH + 16, L"%ls\\hdd", base);
    p[MAX_PATH + 15] = 0;
    if (folder_has_files(p)) return LAUNCHER_SAVES_OLD_HDD;
    _snwprintf(p, MAX_PATH + 16, L"%ls\\portable.txt", base);
    p[MAX_PATH + 15] = 0;
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return LAUNCHER_SAVES_PORTABLE;
    if (documents_saves(d, MAX_PATH)) return LAUNCHER_SAVES_DOCUMENTS;
    return LAUNCHER_SAVES_PORTABLE;
}
#endif /* _WIN32: launcher_save_kind */

#if defined(_WIN32)
void launcher_hdd_path(const LauncherConfig *cfg, char *out, size_t out_sz)
{
    int kind = launcher_save_kind(cfg);
    WCHAR p[MAX_PATH], joined[MAX_PATH * 2], full[MAX_PATH];
    out[0] = '\0';
    if (kind == LAUNCHER_SAVES_DOCUMENTS && documents_saves(full, MAX_PATH)) {
        WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)out_sz, NULL, NULL);
        return;
    }
    if (kind == LAUNCHER_SAVES_CHOSEN) {
        if (!MultiByteToWideChar(CP_UTF8, 0, cfg->hdd, -1, p, MAX_PATH)) p[0] = L'\0';
    } else {
        wcscpy(p, kind == LAUNCHER_SAVES_OLD_HDD ? L"hdd" : L"Saves");
    }
    if ((p[0] && p[1] == L':') || (p[0] == L'\\' && p[1] == L'\\')) {
        wcscpy(joined, p);
    } else {
        WCHAR base[MAX_PATH];
        launcher_install_root_w(base, MAX_PATH);
        _snwprintf(joined, MAX_PATH * 2, L"%ls\\%ls", base, p);
        joined[MAX_PATH * 2 - 1] = 0;
    }
    if (!GetFullPathNameW(joined, MAX_PATH, full, NULL)) {
        wcsncpy(full, joined, MAX_PATH - 1);
        full[MAX_PATH - 1] = 0;
    }
    WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)out_sz, NULL, NULL);
}
#endif /* _WIN32: launcher_hdd_path */

/* ── Saves left by 0.1.1 and earlier in a misnamed folder ─────────── */

/* Files in `dir` and below (stops counting at `limit`). */
#if defined(_WIN32)
static int count_files(const WCHAR *dir, int limit)
{
    WCHAR pat[MAX_PATH + 4], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    _snwprintf(pat, MAX_PATH + 4, L"%ls\\*", dir);
    pat[MAX_PATH + 3] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") ||
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            _snwprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            sub[MAX_PATH - 1] = 0;
            n += count_files(sub, limit - n);
        } else {
            n++;
        }
    } while (n < limit && FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
#endif /* _WIN32: count_files */

/* A save folder with saves in it: UDATA holding at least one file. */
#if defined(_WIN32)
static BOOL has_saves(const WCHAR *dir)
{
    WCHAR u[MAX_PATH];
    _snwprintf(u, MAX_PATH, L"%ls\\UDATA", dir);
    u[MAX_PATH - 1] = 0;
    return count_files(u, 1) > 0;
}
#endif /* _WIN32: has_saves */

/* Copies `src` into `dst`, folders included; an existing file is never
 * replaced, nothing is moved or deleted. Returns the files copied. */
#if defined(_WIN32)
static int copy_tree(const WCHAR *src, const WCHAR *dst)
{
    WCHAR pat[MAX_PATH + 4], s[MAX_PATH], d[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    CreateDirectoryW(dst, NULL);
    _snwprintf(pat, MAX_PATH + 4, L"%ls\\*", src);
    pat[MAX_PATH + 3] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") ||
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        _snwprintf(s, MAX_PATH, L"%ls\\%ls", src, fd.cFileName);
        _snwprintf(d, MAX_PATH, L"%ls\\%ls", dst, fd.cFileName);
        s[MAX_PATH - 1] = d[MAX_PATH - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) n += copy_tree(s, d);
        else if (CopyFileW(s, d, TRUE)) n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
#endif /* _WIN32: copy_tree */

/* Up to 0.1.1 the save folder reached the runtime as an ANSI path read as
 * UTF-8: with an accent (a chosen folder, or the Documents folder of an
 * accented user name) the saves went to a folder whose name has U+FFFD in
 * place of the accented letters, beside the real one, when its parent
 * existed. That folder is worked out the same way here; if it holds saves
 * and the real one has none, they are copied over. The old folder is left
 * as it is, and nothing is done in any other case. */
#if defined(_WIN32)
void launcher_migrate_saves(const char *save_u8)
{
    WCHAR real[MAX_PATH], ghost[MAX_PATH];
    char ansi[MAX_PATH * 2], g8[MAX_PATH * 3];
    const unsigned char *c;
    BOOL high = FALSE;
    size_t len;
    int n;

    if (!MultiByteToWideChar(CP_UTF8, 0, save_u8, -1, real, MAX_PATH) || !real[0]) return;
    len = wcslen(real);
    if (len > 3 && real[len - 1] == L'\\') real[len - 1] = 0;
    /* the path as 0.1.1 had it: ANSI (or its short 8.3 form) ... */
    if (!to_path(real, ansi, sizeof ansi)) return;           /* 0.1.1 used another folder then */
    for (c = (const unsigned char *)ansi; *c; c++)
        if (*c >= 0x80) high = TRUE;
    if (!high) return;                                        /* plain ASCII: the same folder */
    /* ... decoded as UTF-8, as xbox_path_init did */
    if (!MultiByteToWideChar(CP_UTF8, 0, ansi, -1, ghost, MAX_PATH) || !_wcsicmp(ghost, real)) return;
    if (!has_saves(ghost) || has_saves(real)) return;
    SHCreateDirectoryExW(NULL, real, NULL);
    n = copy_tree(ghost, real);
    WideCharToMultiByte(CP_UTF8, 0, ghost, -1, g8, sizeof g8, NULL, NULL);
    printf("Saves:      %d file(s) copied from %s (an earlier version's folder, left as it is) to %s\n",
           n, g8, save_u8);
}
#endif /* _WIN32: launcher_migrate_saves */

#if defined(_WIN32)
void launcher_open_path(HWND owner, const char *path, BOOL folder)
{
    WCHAR w[MAX_PATH];
    if (folder) CreateDirectoryA(path, NULL);
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH)) return;
    if ((INT_PTR)ShellExecuteW(owner, folder ? L"explore" : L"open", w, NULL, NULL, SW_SHOWNORMAL) <= 32) {
        WCHAR msg[MAX_PATH + 64];
        swprintf(msg, MAX_PATH + 64, L"Could not open:\n%ls", w);
        MessageBoxW(owner, msg, L"SSX Tricky", MB_ICONWARNING);
    }
}
#endif /* _WIN32: launcher_open_path */

/* settings.ini as read at start (settings_load), for launcher_config_save:
 * what this window does not change is written back as it was. */
static Settings s_settings;

/* What launcher_config_load printed, for the log file (opened after it). */
static char s_settings_line[SETTINGS_VALUE_MAX + 160];
static char s_settings_ignored[SETTINGS_IGNORED_MAX + 64];
static char s_settings_deck[320];

void launcher_config_load(LauncherConfig *cfg)
{
    char u8[SETTINGS_VALUE_MAX], lines[32] = "";
    int bad = 0, r;

    config_path_u8(u8, sizeof u8);
    r = settings_load(&s_settings, u8, &bad);       /* defaults when missing or old */
    s_settings_deck[0] = '\0';
    if (r == SETTINGS_LOAD_MISSING) {   /* no file yet: a Steam Deck starts from its preset, as in the launcher */
        SettingsPlatform p;
        char why[200];
        settings_platform_probe(&p, NULL);
        if (settings_first_start_preset(&p, why, sizeof why) == SETTINGS_PRESET_STEAM_DECK) {
            settings_apply_preset(&s_settings, SETTINGS_PRESET_STEAM_DECK);
            snprintf(s_settings_deck, sizeof s_settings_deck,
                     "Settings:  first start on a Steam Deck (%s): Steam Deck preset\n", why);
        } else if (p.wine) {
            snprintf(s_settings_deck, sizeof s_settings_deck,
                     "Settings:  first start under Wine, not a Steam Deck (%s): defaults\n", why);
        }
    }
    config_from_settings(cfg, &s_settings);
    if (bad) snprintf(lines, sizeof lines, ", %d line(s) ignored", bad);
    snprintf(s_settings_line, sizeof s_settings_line, "Settings:  %s (%s%s, preset %s)\n", u8,
             r == SETTINGS_LOAD_OK      ? "read" :
             r == SETTINGS_LOAD_MISSING ? "none yet: defaults" :
             r == SETTINGS_LOAD_NOT_V1  ? "an older file, not read: defaults" :
             r == SETTINGS_LOAD_NEWER   ? "written by a newer version: the known settings read" :
                                          "could not be read: defaults",
             lines, settings_preset_name(settings_current_preset(&s_settings)));
    s_settings_ignored[0] = '\0';
    if (bad)    /* unknown keys are kept in the file, values not valid take their default */
        snprintf(s_settings_ignored, sizeof s_settings_ignored, "Settings:  not understood, not used: %s\n",
                 s_settings.ignored);
    launcher_config_log();
}

void launcher_config_log(void)
{
    fputs(s_settings_line, stdout);
    fputs(s_settings_deck, stdout);
    fputs(s_settings_ignored, stdout);
}

/* Written whole, with comments (settings_save), to settings.ini in the
 * install root, or in Documents\My Games\SSX Tricky when it cannot be
 * written (settings_locate). */
BOOL launcher_config_save(const LauncherConfig *cfg)
{
    char u8[SETTINGS_VALUE_MAX];
    int where = settings_locate(NULL, NULL, 1, u8, sizeof u8);
    if (where == SETTINGS_AT_NONE) return FALSE;
    config_to_settings(&s_settings, cfg);
    if (!settings_save(&s_settings, u8)) return FALSE;
    printf("Settings:  saved to %s%s\n", u8,
           where == SETTINGS_AT_DOCUMENTS ? " (the game's folder cannot be written)" : "");
    return TRUE;
}

/* ── Disc image check ──────────────────────────────────────────────── */

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

BOOL launcher_check_iso(const char *path, char *why, size_t why_sz)
{
    unsigned char hdr[0x1000];
    uint32_t sector = 0, size = 0, entry;
    uint8_t attrs = 0;
    DWORD fa;

    if (!path || !path[0]) {
        snprintf(why, why_sz, "No disc image has been chosen.");
        return FALSE;
    }
    fa = GetFileAttributesA(path);
    if (fa == INVALID_FILE_ATTRIBUTES || (fa & FILE_ATTRIBUTE_DIRECTORY)) {
        snprintf(why, why_sz, "Your disc image was moved or deleted: %s", path);
        return FALSE;
    }
    if (xdvdfs_is_mounted()) xdvdfs_unmount();
    if (!xdvdfs_mount(path)) {
        snprintf(why, why_sz, "This file is not an Xbox disc image.");
        return FALSE;
    }
    memset(hdr, 0, sizeof hdr);
    if (!xdvdfs_find("default.xbe", &sector, &size, &attrs) ||
        (attrs & XDVDFS_ATTR_DIRECTORY) || size < 0x200 ||
        xdvdfs_read(sector, size, 0, hdr, size < sizeof hdr ? size : sizeof hdr) < 0x200) {
        xdvdfs_unmount();
        snprintf(why, why_sz, "The disc image has no game on it (no default.xbe).");
        return FALSE;
    }
    xdvdfs_unmount();

    if (memcmp(hdr, "XBEH", 4) != 0) {
        snprintf(why, why_sz, "The game on this disc image is damaged (bad default.xbe).");
        return FALSE;
    }
    /* Retail images XOR the entry point with 0xA8FC57AB, debug ones with
     * 0x94859D4B; either is accepted if it lands on the expected address. */
    entry = rd32(hdr + 0x128);
    if (s_expected_entry &&
        (entry ^ 0xA8FC57ABu) != s_expected_entry &&
        (entry ^ 0x94859D4Bu) != s_expected_entry) {
        char title[41] = "another game";
        uint32_t base = rd32(hdr + 0x104), cert = rd32(hdr + 0x118) - base;
        if (cert + 0x0C + 80 <= sizeof hdr) {
            int i;
            for (i = 0; i < 40; i++) {
                unsigned c = hdr[cert + 0x0C + i * 2] | (hdr[cert + 0x0D + i * 2] << 8);
                if (!c) break;
                title[i] = (c >= 32 && c < 127) ? (char)c : '?';
            }
            if (i) title[i] = '\0';
        }
        if (!_stricmp(title, "SSX Tricky"))         /* the game, but not the USA version */
            snprintf(why, why_sz, "This is another version of SSX Tricky (not the USA one). "
                                  "This build runs only SSX Tricky (USA).");
        else
            snprintf(why, why_sz,
                     "This disc image is %s. This build runs only SSX Tricky (USA).",
                     title);
        return FALSE;
    }
    return TRUE;
}

/* ── DPI ───────────────────────────────────────────────────────────── */

typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);
typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);

#if defined(_WIN32)
void launcher_enable_dpi_awareness(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    SetDpiCtxFn set = u ? (SetDpiCtxFn)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
    if (!set || !set((HANDLE)(INT_PTR)-4))
        SetProcessDPIAware();
}
#endif /* _WIN32: launcher_enable_dpi_awareness */

/* ── Path helpers ──────────────────────────────────────────────────── */

/* The runtime opens files through the ANSI API, so a path must survive the
 * conversion. If it does not (characters outside the code page), the short
 * 8.3 form usually does; failing that the path is refused. */
#if defined(_WIN32)
static BOOL to_path(const WCHAR *w, char *out, size_t n)
{
    BOOL lossy = FALSE;
    if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, (int)n, NULL, &lossy))
        return FALSE;
    if (lossy) {
        WCHAR sh[MAX_PATH];
        lossy = FALSE;
        if (!GetShortPathNameW(w, sh, MAX_PATH) ||
            !WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, sh, -1, out, (int)n, NULL, &lossy) ||
            lossy)
            return FALSE;
    }
    return TRUE;
}
#endif /* _WIN32: to_path */
