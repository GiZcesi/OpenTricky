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
        DEVMODEW dm;
        int i, n = 0, mw = 1920, mh = 1080;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480) {
            mw = (int)dm.dmPelsWidth;
            mh = (int)dm.dmPelsHeight;
        }
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

static void exe_dir(char *out, size_t n)
{
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n);
    char *slash;
    if (k == 0 || k >= n) { out[0] = '\0'; return; }
    slash = strrchr(out, '\\');
    if (slash) *slash = '\0';
}

/* UTF-8 (settings.ini) <-> the ANSI paths the runtime opens. A path outside
 * the code page becomes its short 8.3 form, or empty if it has none. */
static BOOL u8_to_path(const char *u8, char *out, size_t n)
{
    WCHAR w[MAX_PATH];
    out[0] = '\0';
    if (!u8[0]) return TRUE;
    if (!MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, MAX_PATH)) return FALSE;
    if (!to_path(w, out, n)) { out[0] = '\0'; return FALSE; }
    return TRUE;
}

static void path_to_u8(const char *ansi, char *out, size_t n)
{
    WCHAR w[MAX_PATH];
    out[0] = '\0';
    if (ansi[0] && MultiByteToWideChar(CP_ACP, 0, ansi, -1, w, MAX_PATH))
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL);
}

/* settings.ini (settings.h, settings_locate): beside the executables, or in
 * Documents\My Games\SSX Tricky when only that one exists. */
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

/* The defaults are those of fork/main with no variable set: no SMAA, every
 * fidelity fix on, 8 audio buffers -- for test runs whose XBOX_FORK_INI
 * names no file. A player's defaults are the settings registry's (settings.h). */
static void fork_defaults(LauncherConfig *c)
{
    c->smaa = 0;
    c->fidelity = 1;
    c->save_backup = 1;                 /* XBOX_SAVE_BACKUP's default */
    c->smooth_motion = 1;               /* XBOX_FPS_INTERP's default */
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
/* The PS2 game's own buttons are built in unless CMake has
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

    /* settings.ini may name the HD pack relative to the game's folder ("hd-pack") */
    hd[0] = '\0';
    if (cfg->hd_path[0] && !(cfg->hd_path[1] == ':' || (cfg->hd_path[0] == '\\' && cfg->hd_path[1] == '\\'))) {
        char base[MAX_PATH], joined[2 * MAX_PATH];
        exe_dir(base, sizeof base);
        snprintf(joined, sizeof joined, "%s\\%s", base, cfg->hd_path);
        if (!GetFullPathNameA(joined, sizeof hd, hd, NULL)) hd[0] = '\0';
    }
    if (!hd[0]) snprintf(hd, sizeof hd, "%s", cfg->hd_path);

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
    /* Original = aucun hook. */
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
    u8_to_path(settings_get(s, S_SAVE_FOLDER), c->hdd, sizeof c->hdd);    /* "": automatic */
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
    path_to_settings(s, S_SAVE_FOLDER, c->hdd);
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

/* "SSX Tricky.log", in the folder of settings.ini (beside the game, or in
 * Documents when the game's folder cannot be written). */
void launcher_log_path(char *out, size_t out_sz)
{
    char ini[MAX_PATH], exe[MAX_PATH], *slash, *name, *dot;
    DWORD k;
    launcher_config_path(ini, sizeof ini);
    slash = strrchr(ini, '\\');
    if (slash) slash[1] = '\0';
    else ini[0] = '\0';
    k = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (k == 0 || k >= sizeof exe) snprintf(exe, sizeof exe, "SSX Tricky.exe");
    name = strrchr(exe, '\\');
    name = name ? name + 1 : exe;
    dot = strrchr(name, '.');
    if (dot) *dot = '\0';
    snprintf(out, out_sz, "%s%s.log", ini, name);
}

/* A folder with something in it (the saves of an earlier version). */
static BOOL folder_has_files(const char *dir)
{
    char pat[MAX_PATH + 4];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    BOOL any = FALSE;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) any = TRUE;
    } while (!any && FindNextFileA(h, &fd));
    FindClose(h);
    return any;
}

/* Documents\My Games\SSX Tricky\Saves, created if missing. The
 * real Documents folder (moved, or synced by OneDrive, it still works); FALSE
 * if it cannot be had as a path the game's ANSI file calls can open. */
static BOOL documents_saves(char *out, size_t out_sz)
{
    PWSTR docs = NULL;
    WCHAR w[MAX_PATH], sh[MAX_PATH];
    BOOL lossy = FALSE, ok = FALSE;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &docs)) || !docs) return FALSE;
    _snwprintf(w, MAX_PATH, L"%ls\\My Games\\SSX Tricky\\Saves", docs);
    w[MAX_PATH - 1] = 0;
    CoTaskMemFree(docs);
    SHCreateDirectoryExW(NULL, w, NULL);
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, (int)out_sz, NULL, &lossy) && !lossy)
        ok = TRUE;
    else if (GetShortPathNameW(w, sh, MAX_PATH) &&       /* a user name outside the code page */
             WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, sh, -1, out, (int)out_sz, NULL, &lossy) && !lossy)
        ok = TRUE;
    return ok;
}

/* Where the saves go. A SaveFolder set in the .ini (chosen in
 * Advanced, a test pack's, an older .ini's) is used as it is. Left empty, the
 * folder is automatic:
 *   1. hdd\ beside the game, if it holds saves (an earlier version's default):
 *      used where it is -- nothing is moved, nothing can be lost;
 *   2. Saves\ beside the game if portable.txt is there (a copyable install);
 *   3. Documents\My Games\SSX Tricky\Saves, as PC games do (works when the
 *      game is installed where it cannot write, such as Program Files);
 *      Saves\ beside the game if Documents cannot be used. */
int launcher_save_kind(const LauncherConfig *cfg)
{
    char base[MAX_PATH], p[MAX_PATH + 16], d[MAX_PATH];
    if (cfg->hdd[0]) return LAUNCHER_SAVES_CHOSEN;
    exe_dir(base, sizeof base);
    snprintf(p, sizeof p, "%s\\hdd", base);
    if (folder_has_files(p)) return LAUNCHER_SAVES_OLD_HDD;
    snprintf(p, sizeof p, "%s\\portable.txt", base);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return LAUNCHER_SAVES_PORTABLE;
    if (documents_saves(d, sizeof d)) return LAUNCHER_SAVES_DOCUMENTS;
    return LAUNCHER_SAVES_PORTABLE;
}

void launcher_hdd_path(const LauncherConfig *cfg, char *out, size_t out_sz)
{
    int kind = launcher_save_kind(cfg);
    const char *p = kind == LAUNCHER_SAVES_CHOSEN ? cfg->hdd : kind == LAUNCHER_SAVES_OLD_HDD ? "hdd" : "Saves";
    char joined[MAX_PATH * 2];
    if (kind == LAUNCHER_SAVES_DOCUMENTS && documents_saves(out, out_sz)) return;
    if ((p[0] && p[1] == ':') || (p[0] == '\\' && p[1] == '\\')) {
        snprintf(joined, sizeof joined, "%s", p);
    } else {
        char base[MAX_PATH];
        exe_dir(base, sizeof base);
        snprintf(joined, sizeof joined, "%s\\%s", base, p);
    }
    if (!GetFullPathNameA(joined, (DWORD)out_sz, out, NULL))
        snprintf(out, out_sz, "%s", joined);
}

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

/* settings.ini as read at start (settings_load), for launcher_config_save:
 * what this window does not change is written back as it was. */
static Settings s_settings;

void launcher_config_load(LauncherConfig *cfg)
{
    char u8[SETTINGS_VALUE_MAX];
    int bad = 0, r;

    config_path_u8(u8, sizeof u8);
    r = settings_load(&s_settings, u8, &bad);       /* defaults when missing or old */
    config_from_settings(cfg, &s_settings);
    printf("Settings:  %s (%s", u8,
           r == SETTINGS_LOAD_OK      ? "read" :
           r == SETTINGS_LOAD_MISSING ? "none yet: defaults" :
           r == SETTINGS_LOAD_NOT_V1  ? "an older file, not read: defaults" :
           r == SETTINGS_LOAD_NEWER   ? "written by a newer version: the known settings read" :
                                        "could not be read: defaults");
    if (bad) printf(", %d line(s) ignored", bad);
    printf(", preset %s)\n", settings_preset_name(settings_current_preset(&s_settings)));
}

/* Written whole, with comments (settings_save), to settings.ini beside the
 * game, or in Documents\My Games\SSX Tricky when the game's folder cannot be
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

void launcher_enable_dpi_awareness(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    SetDpiCtxFn set = u ? (SetDpiCtxFn)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
    if (!set || !set((HANDLE)(INT_PTR)-4))
        SetProcessDPIAware();
}

/* ── Path helpers ──────────────────────────────────────────────────── */

/* The runtime opens files through the ANSI API, so a path must survive the
 * conversion. If it does not (characters outside the code page), the short
 * 8.3 form usually does; failing that the path is refused. */
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
