/*
 * launcher.c -- the window the game opens with, drawn in the game's own
 * style.
 *
 * PLAY is enabled only once a disc image has been chosen and checked to be
 * the build this executable was recompiled from. SETTINGS has four tabs
 * (Video, Audio, Controls, Advanced) written to an .ini beside the
 * executable; the file format is unchanged.
 *
 * The window is drawn, not built from controls: the logo, the front-end
 * backdrop, the menu bar and icons, a random rider's loading card, the
 * tracks' cards sliding behind and the title lettering are all read from
 * the player's disc image each time (discart.h) -- nothing of the game is
 * in the executable. Before a disc image is chosen, a plain page asks for
 * one. Mouse, keyboard (arrows, Enter, Esc, Tab / Page Up / Page Down for
 * tabs) and a controller (D-pad or stick, A, B, Start, LB / RB) all work.
 */
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <xinput.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <math.h>

#include "launcher.h"
#include "aspect.h"
#include "discart.h"
#include "kernel/xbox_xdvdfs.h"

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

void launcher_config_path(char *out, size_t out_sz)
{
    char exe[MAX_PATH];
    char *dot;
    DWORD k = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (k == 0 || k >= sizeof exe) { snprintf(out, out_sz, "settings.ini"); return; }
    dot = strrchr(exe, '.');
    if (dot && !strchr(dot, '\\')) *dot = '\0';
    snprintf(out, out_sz, "%s.ini", exe);
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

/* ── Fork options ───────────────────────────────────────────── */

/* The defaults are those of fork/main with no variable set: no SMAA, every
 * fidelity fix on, 8 audio buffers. An absent .ini changes nothing. */
static void fork_defaults(LauncherConfig *c)
{
    c->smaa = 0;
    c->fidelity = 1;
    c->fix_cullwind = c->fix_occlusion = c->fix_texpassthru = c->fix_gamma = 1;
    c->audio_queue = 8;
    c->fix_kickwait = 1;
    c->fps_cap = 60;
    c->soft_shadows = 0;
    c->draw_dist = 0;
}

/* SMAA presets, as XBOX_SMAA_PRESET names them; index = LauncherConfig.smaa. */
static const char *const k_smaa_name[] = { "off", "low", "medium", "high", "ultra" };
/* Draw distance, as DrawDistance and XBOX_DRAW_DISTANCE name them. */
static const char *const k_drawdist_name[] = { "original", "far", "max" };

void launcher_fork_load(LauncherConfig *cfg, const char *ini)
{
    char buf[32];
    int i, v;

    fork_defaults(cfg);
    if (!ini || GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;
    GetPrivateProfileStringA("Fork", "SMAA", "off", buf, sizeof buf, ini);
    for (i = 0; i < 5; i++)
        if (!_stricmp(buf, k_smaa_name[i])) cfg->smaa = i;
    cfg->fidelity        = GetPrivateProfileIntA("Fork", "FidelityFixes", 1, ini) != 0;
    cfg->fix_cullwind    = GetPrivateProfileIntA("Fork", "FixCullWinding", 1, ini) != 0;
    cfg->fix_occlusion   = GetPrivateProfileIntA("Fork", "FixOcclusion", 1, ini) != 0;
    cfg->fix_texpassthru = GetPrivateProfileIntA("Fork", "FixTexturePassThrough", 1, ini) != 0;
    cfg->fix_gamma       = GetPrivateProfileIntA("Fork", "FixGamma", 1, ini) != 0;
    cfg->fix_kickwait    = GetPrivateProfileIntA("Fork", "FixKickWait", 1, ini) != 0;
    v = GetPrivateProfileIntA("Fork", "AudioQueue", 8, ini);
    cfg->audio_queue = (v >= 2 && v <= 20) ? v : 8;
    /* FrameRateCap : absent ou 0 = 60 (le jeu d'origine), -1 = sans limite
     * (XBOX_FPS_CAP=0), sinon 60..1000. */
    v = GetPrivateProfileIntA("Fork", "FrameRateCap", 60, ini);
    cfg->fps_cap = (v == -1 || (v >= 60 && v <= 1000)) ? v : 60;
    cfg->soft_shadows = GetPrivateProfileIntA("Fork", "SoftShadows", 0, ini) != 0;
    GetPrivateProfileStringA("Fork", "DrawDistance", "original", buf, sizeof buf, ini);
    for (i = 0; i < 3; i++)
        if (!_stricmp(buf, k_drawdist_name[i])) cfg->draw_dist = i;
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
    char line[768] = "Fork:      ", q[16], cap[16];
    int on = cfg->fidelity != 0;
    int smaa = (cfg->smaa >= 0 && cfg->smaa <= 4) ? cfg->smaa : 0;

    fork_set(line, sizeof line, "XBOX_SMAA", smaa ? "1" : "0", source);
    /* the preset only means something with SMAA on */
    fork_set(line, sizeof line, "XBOX_SMAA_PRESET", smaa ? k_smaa_name[smaa] : NULL, source);
    fork_set(line, sizeof line, "XBOX_FIX_CULLWIND",    (on && cfg->fix_cullwind)    ? "1" : "0", source);
    fork_set(line, sizeof line, "XBOX_FIX_OCCLUSION",   (on && cfg->fix_occlusion)   ? "1" : "0", source);
    fork_set(line, sizeof line, "XBOX_FIX_TEXPASSTHRU", (on && cfg->fix_texpassthru) ? "1" : "0", source);
    fork_set(line, sizeof line, "XBOX_FIX_GAMMA",       (on && cfg->fix_gamma)       ? "1" : "0", source);
    /* Performance, pas fidélité -- indépendant de FidelityFixes. */
    fork_set(line, sizeof line, "XBOX_FIX_KICKWAIT", cfg->fix_kickwait ? "1" : "0", source);
    snprintf(q, sizeof q, "%d", cfg->audio_queue);
    fork_set(line, sizeof line, "XBOX_AUDIO_QUEUE", q, source);
    /* -1 = sans limite, que XBOX_FPS_CAP écrit 0 ; 60 = hook non installé. */
    snprintf(cap, sizeof cap, "%d", cfg->fps_cap == -1 ? 0 : cfg->fps_cap);
    fork_set(line, sizeof line, "XBOX_FPS_CAP", cap, source);
    /* Amélioration visuelle, Off par défaut (comme la Xbox). */
    fork_set(line, sizeof line, "XBOX_SOFT_SHADOWS", cfg->soft_shadows ? "1" : "0", source);
    /* Original = aucun hook. */
    fork_set(line, sizeof line, "XBOX_DRAW_DISTANCE",
             k_drawdist_name[(cfg->draw_dist >= 0 && cfg->draw_dist <= 2) ? cfg->draw_dist : 0], source);
    printf("%s\n", line);
}

static void config_defaults(LauncherConfig *c)
{
    memset(c, 0, sizeof *c);           /* hdd "": the automatic save folder */
    c->aspect = LAUNCHER_ASPECT_43;
    c->wide_fov = ASPECT_FOV_NOSTRETCH;
    c->fullscreen = 0;
    c->aniso = 0;
    c->msaa = 1;
    c->show_fps = 0;
    c->log_file = 0;
    c->menu_sounds = 1;
    default_resolution(LAUNCHER_ASPECT_43, &c->width, &c->height);
    controls_defaults(&c->controls);
    fork_defaults(c);
}

void launcher_log_path(char *out, size_t out_sz)
{
    char ini[MAX_PATH];
    size_t n;
    launcher_config_path(ini, sizeof ini);
    n = strlen(ini);
    if (n > 4 && !_stricmp(ini + n - 4, ".ini")) ini[n - 4] = '\0';
    snprintf(out, out_sz, "%s.log", ini);
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

void launcher_config_load(LauncherConfig *cfg)
{
    char ini[MAX_PATH], buf[64];
    int w = 0, h = 0, v;

    config_defaults(cfg);
    launcher_config_path(ini, sizeof ini);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;

    GetPrivateProfileStringA("Game", "DiscImage", "", cfg->iso, sizeof cfg->iso, ini);
    /* empty or missing: automatic (launcher_save_kind) */
    GetPrivateProfileStringA("Game", "SaveFolder", "", cfg->hdd, sizeof cfg->hdd, ini);

    GetPrivateProfileStringA("Display", "AspectRatio", "4:3", buf, sizeof buf, ini);
    cfg->aspect = LAUNCHER_ASPECT_43;
    for (v = 1; v < LAUNCHER_ASPECT_COUNT; v++)
        if (!_stricmp(buf, k_aspect_name[v])) cfg->aspect = v;
    GetPrivateProfileStringA("Display", "WideFieldOfView", "", buf, sizeof buf, ini);
    cfg->wide_fov = aspect_fov_parse(buf, ASPECT_FOV_NOSTRETCH);
    GetPrivateProfileStringA("Display", "Resolution", "", buf, sizeof buf, ini);
    if (sscanf(buf, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240 && w <= 7680 && h <= 4320) {
        cfg->width = w;
        cfg->height = h;
    } else {
        default_resolution(cfg->aspect, &cfg->width, &cfg->height);
    }
    cfg->fullscreen = GetPrivateProfileIntA("Display", "Fullscreen", 0, ini) != 0;
    v = GetPrivateProfileIntA("Display", "AnisotropicFiltering", 0, ini);
    cfg->aniso = (v == 2 || v == 4 || v == 8 || v == 16) ? v : 0;
    v = GetPrivateProfileIntA("Display", "AntiAliasing", 1, ini);
    cfg->msaa = (v == 2 || v == 4 || v == 8) ? v : 1;
    cfg->show_fps = GetPrivateProfileIntA("Display", "ShowFrameRate", 0, ini) != 0;
    cfg->log_file = GetPrivateProfileIntA("Troubleshooting", "LogFile", 0, ini) != 0;
    cfg->menu_sounds = GetPrivateProfileIntA("Launcher", "MenuSounds", 1, ini) != 0;
    controls_load(&cfg->controls, ini);
    launcher_fork_load(cfg, ini);
}

/* Written whole, with comments, so the file explains itself to anyone who
 * opens it; the Get/WritePrivateProfile API cannot write comments. */
BOOL launcher_config_save(const LauncherConfig *cfg)
{
    char ini[MAX_PATH], tmp[MAX_PATH + 8];
    FILE *f;

    launcher_config_path(ini, sizeof ini);
    snprintf(tmp, sizeof tmp, "%s.new", ini);
    f = fopen(tmp, "w");
    if (!f) return FALSE;
    fprintf(f,
        "; SSX Tricky (recompiled) settings, written by the launcher's Settings.\n"
        "; Safe to edit by hand while the game is closed.\n"
        "\n"
        "[Game]\n"
        "; Xbox disc image (.iso) of SSX Tricky (USA). PLAY stays disabled\n"
        "; until this is set.\n"
        "DiscImage=%s\n"
        "; Folder used as the Xbox hard disk (game saves). A relative path is\n"
        "; relative to the folder the game is in. Empty = automatic: hdd\\ beside\n"
        "; the game if an earlier version left saves there, else Saves\\ beside it\n"
        "; when portable.txt is there, else Documents\\My Games\\SSX Tricky\\Saves.\n"
        "SaveFolder=%s\n"
        "\n"
        "[Display]\n"
        "; Render resolution, WIDTHxHEIGHT.\n"
        "Resolution=%dx%d\n"
        "; 4:3, or 16:9 to use the game's own widescreen mode; 21:9, 32:9 or\n"
        "; Auto (the monitor's shape) widen that view to the resolution's shape.\n"
        "AspectRatio=%s\n"
        "; Field of view wider than 16:9 (21:9, 32:9, Auto): NoStretch keeps the\n"
        "; 16:9 width of view, so the edges stretch no more than at 16:9;\n"
        "; Full widens it to the whole screen (stretched edges); Balanced is\n"
        "; halfway. XBOX_WIDE_FOV, when set, takes priority.\n"
        "WideFieldOfView=%s\n"
        "; 1 = borderless fullscreen. Alt+Enter switches while playing.\n"
        "Fullscreen=%d\n"
        "; 0 = the game's own texture filtering, or 2, 4, 8, 16 (anisotropic).\n"
        "AnisotropicFiltering=%d\n"
        "; Samples per pixel: 1 (off), 2, 4 or 8.\n"
        "AntiAliasing=%d\n"
        "; 1 = frame rate in the title bar.\n"
        "ShowFrameRate=%d\n"
        "\n"
        "[Troubleshooting]\n"
        "; 1 = write everything the game reports to a .log file beside it.\n"
        "LogFile=%d\n"
        "\n"
        "[Launcher]\n"
        "; 1 = the launcher plays the game's own menu sounds (read from the disc).\n"
        "MenuSounds=%d\n",
        cfg->iso, cfg->hdd, cfg->width, cfg->height,
        launcher_aspect_name(cfg->aspect), aspect_fov_name(cfg->wide_fov),
        cfg->fullscreen ? 1 : 0,
        cfg->aniso, cfg->msaa, cfg->show_fps ? 1 : 0, cfg->log_file ? 1 : 0, cfg->menu_sounds ? 1 : 0);
    fprintf(f,
        "\n"
        "[Fork]\n"
        "; Options of this fork. A matching XBOX_* environment variable, when set,\n"
        "; takes priority over the line here.\n"
        "; SMAA edge smoothing after rendering: off, low, medium, high or ultra\n"
        "; (XBOX_SMAA, XBOX_SMAA_PRESET). Combines with AntiAliasing above; the HUD\n"
        "; and menu text stay sharp.\n"
        "SMAA=%s\n"
        "; 1 = the Xbox fidelity fixes below (recommended); 0 = all of them off.\n"
        "FidelityFixes=%d\n"
        "; One by one (used only when FidelityFixes=1): back-face culling winding\n"
        "; (fog volumes), occlusion queries (lens flare), pass-through texture\n"
        "; stages (board tops, terrain fog), the title's gamma ramp.\n"
        "FixCullWinding=%d\n"
        "FixOcclusion=%d\n"
        "FixTexturePassThrough=%d\n"
        "FixGamma=%d\n"
        "; 1 = the game does not busy-wait on the GPU write-combine flush, as on\n"
        "; xemu (the game and the GPU translator work in parallel; default).\n"
        "; 0 = the original port's wait (XBOX_FIX_KICKWAIT).\n"
        "FixKickWait=%d\n"
        "; Audio buffers queued, 2..20, 5.33 ms each: 8 = default, 16 = the\n"
        "; original port (more latency).\n"
        "AudioQueue=%d\n"
        "; Frame rate cap (XBOX_FPS_CAP): 60 = as on Xbox (default), 120, 144, 240,\n"
        "; or -1 = unlimited. The game logic stays at 60 updates a second; the frames\n"
        "; in between repeat the last one for now (no smoothing yet).\n"
        "FrameRateCap=%d\n"
        "; Soft shadow edges (XBOX_SOFT_SHADOWS): 0 = hard edges, as on the Xbox\n"
        "; (default); 1 = the edges of every shadow fade over a few pixels.\n"
        "SoftShadows=%d\n"
        "; Draw distance (XBOX_DRAW_DISTANCE): original (default), far (x1.5) or\n"
        "; max (x2), always kept within what each track's cell list can hold.\n"
        "DrawDistance=%s\n",
        k_smaa_name[(cfg->smaa >= 0 && cfg->smaa <= 4) ? cfg->smaa : 0],
        cfg->fidelity ? 1 : 0, cfg->fix_cullwind ? 1 : 0, cfg->fix_occlusion ? 1 : 0,
        cfg->fix_texpassthru ? 1 : 0, cfg->fix_gamma ? 1 : 0, cfg->fix_kickwait ? 1 : 0,
        cfg->audio_queue, cfg->fps_cap, cfg->soft_shadows ? 1 : 0,
        k_drawdist_name[(cfg->draw_dist >= 0 && cfg->draw_dist <= 2) ? cfg->draw_dist : 0]);
    controls_write(&cfg->controls, f);
    if (fclose(f) != 0) { DeleteFileA(tmp); return FALSE; }
    if (!MoveFileExA(tmp, ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        return FALSE;
    }
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
        snprintf(why, why_sz, "The disc image could not be found: %s", path);
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

static UINT window_dpi(HWND h)
{
    static GetDpiForWindowFn fn = NULL;
    static int looked = 0;
    if (!looked) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        fn = u ? (GetDpiForWindowFn)(void *)GetProcAddress(u, "GetDpiForWindow") : NULL;
        looked = 1;
    }
    if (fn && h) { UINT d = fn(h); if (d) return d; }
    {
        HDC dc = GetDC(NULL);
        int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) ReleaseDC(NULL, dc);
        return d > 0 ? (UINT)d : 96;
    }
}

/* ── Path helpers ──────────────────────────────────────────────────── */

static void to_wide(const char *s, WCHAR *w, int n)
{
    if (!MultiByteToWideChar(CP_ACP, 0, s, -1, w, n)) w[0] = 0;
}

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

static void file_name_of(const char *path, char *out, size_t n)
{
    const char *s = strrchr(path, '\\');
    const char *t = strrchr(path, '/');
    if (t && (!s || t > s)) s = t;
    snprintf(out, n, "%s", s ? s + 1 : path);
}

/* A modern file or folder picker, starting in `start` (or the game's
 * folder). Returns FALSE if the player cancelled. */
static BOOL pick_path(HWND owner, BOOL folder, const WCHAR *title,
                      const char *start, char *out, size_t n)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL, *dir = NULL;
    PWSTR wpath = NULL;
    BOOL ok = FALSE;
    DWORD opts = 0;
    WCHAR wstart[MAX_PATH];
    char s[MAX_PATH];

    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&dlg)) || !dlg)
        return FALSE;
    IFileOpenDialog_GetOptions(dlg, &opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) opts |= FOS_PICKFOLDERS;
    else        opts |= FOS_FILEMUSTEXIST;
    IFileOpenDialog_SetOptions(dlg, opts);
    IFileOpenDialog_SetTitle(dlg, title);
    if (!folder) {
        static const COMDLG_FILTERSPEC types[] = {
            { L"Xbox disc image (*.iso)", L"*.iso" },
            { L"All files (*.*)",         L"*.*" },
        };
        IFileOpenDialog_SetFileTypes(dlg, 2, types);
    }

    /* Start where the current choice is, else beside the game. */
    s[0] = '\0';
    if (start && start[0]) {
        char full[MAX_PATH];
        char *slash;
        if (!(start[0] && start[1] == ':') && !(start[0] == '\\' && start[1] == '\\')) {
            char base[MAX_PATH];
            exe_dir(base, sizeof base);
            snprintf(full, sizeof full, "%s\\%s", base, start);
        } else {
            snprintf(full, sizeof full, "%s", start);
        }
        snprintf(s, sizeof s, "%s", full);
        if (!folder && (slash = strrchr(s, '\\')) != NULL) *slash = '\0';
    }
    if (!s[0] || GetFileAttributesA(s) == INVALID_FILE_ATTRIBUTES)
        exe_dir(s, sizeof s);
    to_wide(s, wstart, MAX_PATH);
    if (SUCCEEDED(SHCreateItemFromParsingName(wstart, NULL, &IID_IShellItem, (void **)&dir)) && dir) {
        IFileOpenDialog_SetFolder(dlg, dir);
        IShellItem_Release(dir);
    }

    if (SUCCEEDED(IFileOpenDialog_Show(dlg, owner)) &&
        SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item)) && item) {
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wpath)) && wpath) {
            if (to_path(wpath, out, n)) ok = TRUE;
            else MessageBoxW(owner, L"That path contains characters the game cannot open.\n"
                                    L"Move the file to a folder with a plain name and try again.",
                             L"SSX Tricky", MB_ICONWARNING);
            CoTaskMemFree(wpath);
        }
        IShellItem_Release(item);
    }
    IFileOpenDialog_Release(dlg);
    return ok;
}

/* ── Software canvas ───────────────────────────────────────
 *
 * The launcher is drawn, not built from controls: a 32-bit DIB the size of
 * the client area, composed from the disc's pictures (discart.h), then the
 * Segoe UI text through GDI on top. Pictures are premultiplied once; every
 * scale is uniform -- a picture keeps its own shape at any size (human
 * request) -- and the animated backdrop only ever moves pre-rendered
 * cards by whole pixels, so a frame costs a few copies and blends. */

typedef struct {
    int       w, h;
    uint32_t *px;           /* 0xAARRGGBB, premultiplied for overlays, opaque for the frame */
    HBITMAP   bmp;
    HDC       dc;
} Canvas;

static BOOL canvas_make(Canvas *c, int w, int h, BOOL with_dc)
{
    memset(c, 0, sizeof *c);
    if (w < 1 || h < 1) return FALSE;
    if (with_dc) {
        BITMAPINFO bi;
        void *bits = NULL;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        c->dc = CreateCompatibleDC(NULL);
        c->bmp = CreateDIBSection(c->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!c->dc || !c->bmp) {
            if (c->bmp) DeleteObject(c->bmp);
            if (c->dc) DeleteDC(c->dc);
            memset(c, 0, sizeof *c);
            return FALSE;
        }
        SelectObject(c->dc, c->bmp);
        c->px = (uint32_t *)bits;
    } else {
        c->px = (uint32_t *)calloc((size_t)w * h, 4);
        if (!c->px) return FALSE;
    }
    c->w = w;
    c->h = h;
    return TRUE;
}

static void canvas_free(Canvas *c)
{
    if (c->dc) { DeleteDC(c->dc); DeleteObject(c->bmp); }
    else free(c->px);
    memset(c, 0, sizeof *c);
}

static void premultiply(ArtImage *im)
{
    size_t i, n = (size_t)im->w * im->h;
    for (i = 0; i < n; i++) {
        uint32_t p = im->px[i], a = p >> 24;
        uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
        im->px[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* src over dst, both premultiplied. */
static uint32_t over(uint32_t d, uint32_t s)
{
    uint32_t ia = 255 - (s >> 24);
    uint32_t rb = (((d & 0x00FF00FF) * ia + 0x00800080) >> 8) & 0x00FF00FF;
    uint32_t ag = ((((d >> 8) & 0x00FF00FF) * ia + 0x00800080) >> 8) & 0x00FF00FF;
    return s + (rb | (ag << 8));
}

static uint32_t scale_px(uint32_t p, uint32_t k /* 0..256 */)
{
    uint32_t rb = ((p & 0x00FF00FF) * k >> 8) & 0x00FF00FF;
    uint32_t ag = (((p >> 8) & 0x00FF00FF) * k >> 8) & 0x00FF00FF;
    return rb | (ag << 8);
}

static uint32_t argb(int a, int r, int g, int b)    /* premultiplied */
{
    return ((uint32_t)a << 24) | ((uint32_t)(r * a / 255) << 16) | ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
}

static void fill_rect(Canvas *c, int x, int y, int w, int h, uint32_t col)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    for (j = 0; j < h; j++) {
        uint32_t *p = c->px + (size_t)(y + j) * c->w + x;
        for (i = 0; i < w; i++) p[i] = over(p[i], col);
    }
}

/* A slanted bar, like the game's tabs: the bottom edge is `slant` px to the left. */
static void fill_slanted(Canvas *c, int x, int y, int w, int h, int slant, uint32_t col)
{
    int j;
    for (j = 0; j < h; j++) {
        int off = slant * (h - j) / h;
        fill_rect(c, x + off, y + j, w, 1, col);
    }
}

/* Bilinear sample of a premultiplied image; outside is transparent. */
static uint32_t sample(const ArtImage *im, float fx, float fy)
{
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy), k;
    uint32_t fxw = (uint32_t)((fx - x0) * 256), fyw = (uint32_t)((fy - y0) * 256);
    uint32_t c[4] = { 0, 0, 0, 0 }, top_rb, top_ag, bot_rb, bot_ag;
    for (k = 0; k < 4; k++) {
        int x = x0 + (k & 1), y = y0 + (k >> 1);
        if (x >= 0 && y >= 0 && x < im->w && y < im->h) c[k] = im->px[(size_t)y * im->w + x];
    }
    top_rb = ((c[0] & 0x00FF00FF) * (256 - fxw) + (c[1] & 0x00FF00FF) * fxw) >> 8 & 0x00FF00FF;
    top_ag = (((c[0] >> 8) & 0x00FF00FF) * (256 - fxw) + ((c[1] >> 8) & 0x00FF00FF) * fxw) >> 8 & 0x00FF00FF;
    bot_rb = ((c[2] & 0x00FF00FF) * (256 - fxw) + (c[3] & 0x00FF00FF) * fxw) >> 8 & 0x00FF00FF;
    bot_ag = (((c[2] >> 8) & 0x00FF00FF) * (256 - fxw) + ((c[3] >> 8) & 0x00FF00FF) * fxw) >> 8 & 0x00FF00FF;
    return ((((top_rb * (256 - fyw) + bot_rb * fyw) >> 8) & 0x00FF00FF)) |
           ((((top_ag * (256 - fyw) + bot_ag * fyw) >> 8) & 0x00FF00FF) << 8);
}

/* Draw `im` uniformly scaled by `s` with its top-left at (x, y), rotated by
 * `deg` about its centre, at opacity `op` (0..256). */
static void draw_image(Canvas *c, const ArtImage *im, float x, float y, float s, float deg, uint32_t op)
{
    float dw = im->w * s, dh = im->h * s, cx = x + dw / 2, cy = y + dh / 2;
    float rad = deg * 3.14159265f / 180.0f, cs = cosf(rad), sn = sinf(rad);
    float ex = (fabsf(cs) * dw + fabsf(sn) * dh) / 2 + 2, ey = (fabsf(sn) * dw + fabsf(cs) * dh) / 2 + 2;
    int bx0 = (int)floorf(cx - ex), by0 = (int)floorf(cy - ey), bx1 = (int)ceilf(cx + ex), by1 = (int)ceilf(cy + ey);
    int px, py;
    if (!im->px || s <= 0) return;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > c->w) bx1 = c->w;
    if (by1 > c->h) by1 = c->h;
    for (py = by0; py < by1; py++) {
        uint32_t *row = c->px + (size_t)py * c->w;
        for (px = bx0; px < bx1; px++) {
            float rx = px + 0.5f - cx, ry = py + 0.5f - cy;
            float u = (cs * rx + sn * ry + dw / 2) / s - 0.5f, v = (-sn * rx + cs * ry + dh / 2) / s - 0.5f;
            uint32_t p;
            if (u < -1 || v < -1 || u > im->w || v > im->h) continue;
            p = sample(im, u, v);
            if (!p) continue;
            if (op < 256) p = scale_px(p, op);
            row[px] = over(row[px], p);
        }
    }
}

/* An opaque copy of `im` scaled uniformly to cover w x h (cropped, centred). */
static BOOL image_cover(const ArtImage *im, int w, int h, ArtImage *out)
{
    float s = (float)w / im->w > (float)h / im->h ? (float)w / im->w : (float)h / im->h;
    float ox = (im->w * s - w) / 2, oy = (im->h * s - h) / 2;
    int x, y;
    out->px = (uint32_t *)malloc((size_t)w * h * 4);
    if (!out->px) return FALSE;
    out->w = w; out->h = h;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float u = (x + 0.5f + ox) / s - 0.5f, v = (y + 0.5f + oy) / s - 0.5f;
            if (u < 0) u = 0;
            if (v < 0) v = 0;
            if (u > im->w - 1.001f) u = im->w - 1.001f;
            if (v > im->h - 1.001f) v = im->h - 1.001f;
            out->px[(size_t)y * w + x] = sample(im, u, v) | 0xFF000000u;
        }
    return TRUE;
}

/* A new image: `im` scaled by s, rotated by deg, at opacity op, with an
 * optional soft shadow -- rendered once, then only moved. */
static BOOL image_prerender(const ArtImage *im, float s, float deg, uint32_t op, int shadow, ArtImage *out)
{
    float dw = im->w * s, dh = im->h * s, rad = deg * 3.14159265f / 180.0f;
    int w = (int)ceilf(fabsf(cosf(rad)) * dw + fabsf(sinf(rad)) * dh) + 4 + shadow * 3;
    int h = (int)ceilf(fabsf(sinf(rad)) * dw + fabsf(cosf(rad)) * dh) + 4 + shadow * 3;
    Canvas c;
    float ox, oy;
    memset(out, 0, sizeof *out);
    if (!canvas_make(&c, w, h, FALSE)) return FALSE;
    /* draw_image turns about the picture's centre: put that centre at the
     * canvas's (less the shadow's room), so no corner is cut off */
    ox = (w - shadow * 3 - dw) / 2;
    oy = (h - shadow * 3 - dh) / 2;
    if (shadow) {
        /* a dark, slightly blurred copy offset down-right */
        ArtImage sh;
        size_t i, n;
        int k;
        sh.w = im->w; sh.h = im->h;
        sh.px = (uint32_t *)malloc((size_t)im->w * im->h * 4);
        if (sh.px) {
            n = (size_t)im->w * im->h;
            for (i = 0; i < n; i++) sh.px[i] = (im->px[i] >> 24) * 150 / 255 << 24;
            for (k = 0; k < 4; k++)
                draw_image(&c, &sh, ox + shadow + (k & 1) * 2.0f, oy + shadow * 1.4f + (k >> 1) * 2.0f, s, deg, 64);
            free(sh.px);
        }
    }
    draw_image(&c, im, ox, oy, s, deg, op);
    out->w = c.w; out->h = c.h; out->px = c.px;     /* the canvas has no DC: take its pixels */
    return TRUE;
}

/* `im` (premultiplied) at whole-pixel (x, y), no resampling. */
static void blit(Canvas *c, const ArtImage *im, int x, int y)
{
    int i, j, x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;
    int x1 = x + im->w > c->w ? c->w - x : im->w, y1 = y + im->h > c->h ? c->h - y : im->h;
    for (j = y0; j < y1; j++) {
        uint32_t *d = c->px + (size_t)(y + j) * c->w + x;
        const uint32_t *s = im->px + (size_t)j * im->w;
        for (i = x0; i < x1; i++) {
            uint32_t p = s[i];
            if (p >> 24 == 255) d[i] = p;
            else if (p) d[i] = over(d[i], p);
        }
    }
}

/* Text in the game's title font, `px` pixels tall, colour (r, g, b). */
static float art_text(Canvas *c, const ArtFont *f, float x, float y, float px, const char *s, int r, int g, int b, int a)
{
    float k = px / (f->line ? f->line : 16);
    for (; *s; s++) {
        int ch = (unsigned char)*s < 128 ? *s : '?';
        const ArtGlyph *gl = &f->g[ch];
        if (!gl->adv && ch >= 'a' && ch <= 'z') gl = &f->g[ch - 32];
        if (!gl->adv) { x += 6 * k; continue; }
        if (gl->w && gl->h) {
            float gx = x + gl->ox * k, gy = y + gl->oy * k, gw = gl->w * k, gh = gl->h * k;
            int x0 = (int)floorf(gx), y0 = (int)floorf(gy), x1 = (int)ceilf(gx + gw), y1 = (int)ceilf(gy + gh), px_, py_;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > c->w) x1 = c->w;
            if (y1 > c->h) y1 = c->h;
            for (py_ = y0; py_ < y1; py_++)
                for (px_ = x0; px_ < x1; px_++) {
                    float u = (px_ + 0.5f - gx) / k - 0.5f, v = (py_ + 0.5f - gy) / k - 0.5f;
                    int u0 = (int)floorf(u), v0 = (int)floorf(v), q;
                    float fu = u - u0, fv = v - v0, cov = 0;
                    for (q = 0; q < 4; q++) {
                        int uu = u0 + (q & 1), vv = v0 + (q >> 1);
                        float wgt = ((q & 1) ? fu : 1 - fu) * ((q >> 1) ? fv : 1 - fv);
                        if (uu >= 0 && vv >= 0 && uu < gl->w && vv < gl->h)
                            cov += wgt * f->cov[(size_t)(gl->y + vv) * f->w + gl->x + uu];
                    }
                    if (cov > 1) {
                        uint32_t al = (uint32_t)(cov * a / 255);
                        uint32_t *d = c->px + (size_t)py_ * c->w + px_;
                        *d = over(*d, argb((int)al, r, g, b));
                    }
                }
        }
        x += gl->adv * k;
    }
    return x;
}

static float art_text_width(const ArtFont *f, float px, const char *s)
{
    float k = px / (f->line ? f->line : 16), w = 0;
    for (; *s; s++) {
        int ch = (unsigned char)*s < 128 ? *s : '?';
        const ArtGlyph *gl = &f->g[ch];
        if (!gl->adv && ch >= 'a' && ch <= 'z') gl = &f->g[ch - 32];
        w += (gl->adv ? gl->adv : 6) * k;
    }
    return w;
}

/* ── GDI text, queued and drawn after the pictures ─────────────────── */

enum { F_LABEL, F_VALUE, F_SMALL, F_STRIP, F_SYMBOL, F_BIG, F_BODY, F_BUTTON, F_TITLE, F_COUNT };

typedef struct {
    int      font;
    COLORREF col;
    RECT     r;
    UINT     flags;
    WCHAR    text[200];
} TextOp;

#define MAX_TEXT 96

/* ── The launcher ──────────────────────────────────────────────────── */

#define UI_W 960        /* client area in DIPs, at 100 % */
#define UI_H 540
#define ANIM_MS 33        /* 30 frames a second, steady (1 ms timer resolution); motion is sub-pixel */
#define TIMER_ID 1

enum { PAGE_FIRST, PAGE_HOME, PAGE_SETTINGS };
enum { TAB_VIDEO, TAB_AUDIO, TAB_CONTROLS, TAB_ADVANCED, TAB_COUNT };
enum {
    R_RES, R_DISPLAY, R_SHAPE, R_SMAA, R_MSAA, R_TEX, R_SHADOWS, R_FPSCAP, R_SHOWFPS,
    R_AUDIO, R_MENU_SOUNDS,
    R_FIDELITY, R_DISC, R_SAVES, R_OPEN_SAVES, R_OPEN_SHOTS, R_LOG, R_OPEN_LOG, R_ABOUT,
    R_WIDEFOV,
    R_DRAWDIST,
};
static const int k_tab_rows[TAB_COUNT][12] = {
    { R_RES, R_DISPLAY, R_SHAPE, R_WIDEFOV, R_SMAA, R_MSAA, R_TEX, R_SHADOWS, R_FPSCAP, R_DRAWDIST, R_SHOWFPS, -1 },
    { R_AUDIO, R_MENU_SOUNDS, -1 },
    { -1 },                         /* Controls: its own page (build_controls) */
    { R_FIDELITY, R_DISC, R_SAVES, R_OPEN_SAVES, R_OPEN_SHOTS, R_LOG, R_OPEN_LOG, R_ABOUT, -1 },
};
static const char *const k_tab_name[TAB_COUNT] = { "VIDEO", "AUDIO", "CONTROLS", "ADVANCED" };

/* Choices, in list order. */
static const int k_aniso[] = { 0, 2, 4, 8, 16 };
static const int k_msaa[]  = { 1, 2, 4, 8 };
static const int k_smaa[]  = { 0, 1, 2, 3, 4 };
static const int k_fpscap[] = { 60, 120, 144, 240, -1 };   /* -1 = unlimited */
#define AUDIO_CHOICES 4
static const int k_audio[AUDIO_CHOICES] = { 4, 8, 12, 16 };

typedef struct {
    HWND     hwnd;
    float    k;                     /* pixels per DIP */
    Canvas   frame;                 /* what is shown */
    Canvas   overlay;               /* everything but the moving backdrop, premultiplied */
    BOOL     overlay_dirty;
    TextOp   text[MAX_TEXT];
    int      ntext;
    HFONT    font[F_COUNT];

    LauncherConfig *cfg;            /* saved settings */
    LauncherConfig  edit;           /* the settings page's copy */
    DiscArt  art;
    char     art_iso[MAX_PATH];     /* the image `art` came from */
    BOOL     iso_ok;
    char     iso_why[512];

    int      page, sel, tab, row;   /* row == nrows: the BACK / SAVE line */
    int      bottom;                /* 0 BACK, 1 SAVE */
    int      rider;                 /* random, per launch */
    WCHAR    status[256];
    int      status_level;          /* 0 hint, 1 ok, 2 error */
    int      audio[AUDIO_CHOICES + 1], naudio;
    char     saves_custom[MAX_PATH];    /* the player's own save folder, while Automatic is shown */
    POINT    mouse_at;                  /* where the mouse was when first seen */
    BOOL     mouse_seen, mouse_moved;

    /* the track-card conveyor: cards pre-rendered at this size */
    ArtImage base;                  /* backdrop, cover-scaled, opaque */
    ArtImage card[ART_TRACKS];
    float    scroll;                /* px */
    double   anim_s;                /* seconds of animation shown */
    LARGE_INTEGER last_qpc;
    BOOL     animate;

    WORD     pad_prev;
    DWORD    pad_repeat_at;
    BOOL     start, done;
    RECT     hit[48];               /* clickable areas of the page, in pixels */
    int      hit_id[48], nhit;
    int      ctl_col, ctl_top;      /* Controls tab: column (0 key, 1 controller), first line shown */
    int      capture, capture_ctl;  /* waiting for: 0 nothing, 1 a key, 2 a controller button */
    DWORD    capture_t, pad_quiet_until;
    ControlsPadSnap capture_snap;
    BOOL     reset_armed;           /* RESET pressed once: the second press within 3 s resets */
    LARGE_INTEGER fade_t0;          /* PLAY fades the window to black first */
    BOOL     fading;
    DWORD    reset_armed_t;
} LUI;

static LUI *s_ui;

static int D(const LUI *ui, float v) { return (int)floorf(v * ui->k + 0.5f); }

static void ui_text(LUI *ui, int font, COLORREF col, float x, float y, float w, float h, UINT flags, const WCHAR *s)
{
    TextOp *t;
    if (ui->ntext >= MAX_TEXT) return;
    t = &ui->text[ui->ntext++];
    t->font = font;
    t->col = col;
    SetRect(&t->r, D(ui, x), D(ui, y), D(ui, x + w), D(ui, y + h));
    t->flags = flags | DT_NOPREFIX;
    lstrcpynW(t->text, s, 200);
}

static void ui_hit(LUI *ui, int id, float x, float y, float w, float h)
{
    if (ui->nhit >= 48) return;
    SetRect(&ui->hit[ui->nhit], D(ui, x), D(ui, y), D(ui, x + w), D(ui, y + h));
    ui->hit_id[ui->nhit++] = id;
}

static void fonts_drop(LUI *ui)
{
    int i;
    for (i = 0; i < F_COUNT; i++) if (ui->font[i]) DeleteObject(ui->font[i]);
    memset(ui->font, 0, sizeof ui->font);
}

static HFONT mkfont(const LUI *ui, float px, int weight, BOOL italic, const WCHAR *face)
{
    return CreateFontW(-D(ui, px), 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, face);
}

static void fonts_build(LUI *ui)
{
    fonts_drop(ui);
    ui->font[F_LABEL]  = mkfont(ui, 18, FW_SEMIBOLD, FALSE, L"Segoe UI");
    ui->font[F_VALUE]  = mkfont(ui, 18, FW_SEMIBOLD, TRUE,  L"Segoe UI");
    ui->font[F_SMALL]  = mkfont(ui, 14, FW_NORMAL,   FALSE, L"Segoe UI");
    ui->font[F_STRIP]  = mkfont(ui, 13, FW_SEMIBOLD, FALSE, L"Segoe UI");
    ui->font[F_SYMBOL] = mkfont(ui, 16, FW_NORMAL,   FALSE, L"Segoe UI Symbol");
    ui->font[F_BIG]    = mkfont(ui, 56, FW_BOLD,     TRUE,  L"Segoe UI");
    ui->font[F_BODY]   = mkfont(ui, 17, FW_NORMAL,   FALSE, L"Segoe UI");
    ui->font[F_BUTTON] = mkfont(ui, 20, FW_BOLD,     TRUE,  L"Segoe UI");
    ui->font[F_TITLE]  = mkfont(ui, 26, FW_BOLD,     TRUE,  L"Segoe UI");
}

/* ── Settings rows ─────────────────────────────────────────────────── */

static int tab_rows(int tab, const int **rows)
{
    int n = 0;
    *rows = k_tab_rows[tab];
    while (n < (int)(sizeof k_tab_rows[0] / sizeof k_tab_rows[0][0]) && k_tab_rows[tab][n] >= 0) n++;
    return n;
}

static BOOL row_is_action(int id)
{
    return id == R_DISC || id == R_OPEN_SAVES ||
           id == R_OPEN_SHOTS || id == R_OPEN_LOG || id == R_ABOUT;
}

static const WCHAR *row_label(int id)
{
    switch (id) {
    case R_RES:        return L"Resolution";
    case R_DISPLAY:    return L"Display";
    case R_SHAPE:      return L"Screen shape";
    case R_WIDEFOV:    return L"Field of view (wide screens)";
    case R_SMAA:       return L"Smooth edges";
    case R_MSAA:       return L"Extra edge smoothing (MSAA)";
    case R_TEX:        return L"Texture sharpness";
    case R_SHADOWS:    return L"Soft shadows";
    case R_FPSCAP:     return L"Frame rate limit";
    case R_DRAWDIST:   return L"Draw distance";
    case R_SHOWFPS:    return L"Show frame rate";
    case R_AUDIO:      return L"Audio delay";
    case R_MENU_SOUNDS: return L"Menu sounds";
    case R_FIDELITY:   return L"Look like the Xbox (recommended)";
    case R_DISC:       return L"Disc image";
    case R_SAVES:      return L"Save games folder";
    case R_OPEN_SAVES: return L"Open the save games folder";
    case R_OPEN_SHOTS: return L"Open the screenshots folder";
    case R_LOG:        return L"Log file for bug reports";
    case R_OPEN_LOG:   return L"Open the log file";
    case R_ABOUT:      return L"About";
    }
    return L"";
}

static const WCHAR *row_help(int id)
{
    switch (id) {
    case R_RES:        return L"The size the game is drawn at. The window opens at this size (smaller if the screen is).";
    case R_DISPLAY:    return L"Window or fullscreen. Alt+Enter (or F11) switches while playing.";
    case R_SHAPE:      return L"16:9 is the game's own widescreen mode; 21:9, 32:9 and Auto fill wider screens.";
    case R_WIDEFOV:    return L"Wider than 16:9 only. No stretch keeps the 16:9 width of view, edges not stretched; Full shows more, stretched at the edges.";
    case R_SMAA:       return L"Removes jagged outlines after drawing; menus and the HUD stay sharp.";
    case R_MSAA:       return L"Smooths edges while drawing (costs more). Adds to Smooth edges.";
    case R_TEX:        return L"Like the Xbox, or sharper textures on slopes far away (anisotropic filtering).";
    case R_SHADOWS:    return L"Hard shadow edges like the Xbox, or edges that fade over a few pixels.";
    case R_FPSCAP:     return L"The game itself always runs at 60 steps a second; above 60, frames in between are smoothed.";
    case R_DRAWDIST:   return L"How far scenery is drawn: Far x1.5, Max x2 (fewer things popping in, costs more). Original is the Xbox.";
    case R_SHOWFPS:    return L"Frames per second in the window's title bar.";
    case R_AUDIO:      return L"Shorter is more responsive; raise it if the sound crackles.";
    case R_MENU_SOUNDS: return L"The game's own menu sounds in this launcher (read from your disc).";
    case R_FIDELITY:   return L"Restores what the Xbox shows: fog, lens flares, board tops and the original colours.";
    case R_DISC:       return L"Your own SSX Tricky (USA) disc image (.iso).";
    case R_SAVES:      return L"Automatic: Documents\\My Games\\SSX Tricky\\Saves (Saves beside the game with portable.txt). Arrows: automatic / your own folder.";
    case R_OPEN_SAVES:
    case R_OPEN_SHOTS: return L"Opens the folder in Explorer.";
    case R_LOG:        return L"Writes everything the game reports to a .log file beside it, for bug reports.";
    case R_OPEN_LOG:   return L"Opens the log file of the last game (turn the log file on first).";
    case R_ABOUT:      return L"SSX Tricky, recompiled from the Xbox game for Windows. Settings are in the .ini beside the game.";
    }
    return L"";
}

static int idx_of(const int *v, int n, int value)
{
    int i;
    for (i = 0; i < n; i++) if (v[i] == value) return i;
    return 0;
}

static void row_value(LUI *ui, int id, WCHAR *out, int n)
{
    const LauncherConfig *c = &ui->edit;
    static const WCHAR *const shapes[LAUNCHER_ASPECT_COUNT] = {
        L"4:3", L"16:9  widescreen", L"21:9  ultrawide", L"32:9  super ultrawide", L"Auto  (monitor)" };
    static const WCHAR *const smaa[] = { L"Off", L"Low", L"Medium", L"High", L"Ultra" };
    static const WCHAR *const drawdist[] = { L"Original  (like the Xbox)", L"Far", L"Max" };
    static const WCHAR *const msaa[] = { L"Off", L"2×", L"4×", L"8×" };
    static const WCHAR *const tex[]  = { L"Like the Xbox", L"Sharper  2×", L"Sharper  4×", L"Sharper  8×", L"Sharpest  16×" };
    char name[MAX_PATH];
    out[0] = 0;
    switch (id) {
    case R_RES: {
        const Res *list;
        int nres = res_list(c->aspect, &list), i, tag = 0;
        for (i = 0; i < nres; i++) if (list[i].w == c->width && list[i].h == c->height) tag = i + 1;
        swprintf(out, n, L"%d × %d%ls", c->width, c->height,
                 c->aspect == LAUNCHER_ASPECT_43 && tag == 1 ? L"  (original)" :
                 c->aspect == LAUNCHER_ASPECT_AUTO && tag == nres ? L"  (monitor)" : L"");
        break;
    }
    case R_DISPLAY:  swprintf(out, n, L"%ls", c->fullscreen ? L"Fullscreen" : L"Window"); break;
    case R_SHAPE:    swprintf(out, n, L"%ls", shapes[c->aspect >= 0 && c->aspect < LAUNCHER_ASPECT_COUNT ? c->aspect : 0]); break;
    case R_WIDEFOV: {
        static const WCHAR *const fov[ASPECT_FOV_COUNT] = {
            L"No stretch  (recommended)", L"Balanced", L"Full  (stretched edges)" };
        if (c->aspect == LAUNCHER_ASPECT_43 || c->aspect == LAUNCHER_ASPECT_169)
            swprintf(out, n, L"Like 16:9  (wider shapes only)");
        else
            swprintf(out, n, L"%ls", fov[c->wide_fov >= 0 && c->wide_fov < ASPECT_FOV_COUNT ? c->wide_fov : 0]);
        break;
    }
    case R_SMAA:     swprintf(out, n, L"%ls", smaa[idx_of(k_smaa, COUNT(k_smaa), c->smaa)]); break;
    case R_MSAA:     swprintf(out, n, L"%ls", msaa[idx_of(k_msaa, COUNT(k_msaa), c->msaa)]); break;
    case R_TEX:      swprintf(out, n, L"%ls", tex[idx_of(k_aniso, COUNT(k_aniso), c->aniso)]); break;
    case R_SHADOWS:  swprintf(out, n, L"%ls", c->soft_shadows ? L"On" : L"Off (like the Xbox)"); break;
    case R_FPSCAP:
        if (c->fps_cap == 60) swprintf(out, n, L"60  (like the Xbox)");
        else if (c->fps_cap < 0) swprintf(out, n, L"Unlimited");
        else swprintf(out, n, L"%d", c->fps_cap);
        break;
    case R_DRAWDIST: swprintf(out, n, L"%ls", drawdist[c->draw_dist >= 0 && c->draw_dist <= 2 ? c->draw_dist : 0]); break;
    case R_SHOWFPS:  swprintf(out, n, L"%ls", c->show_fps ? L"On" : L"Off"); break;
    case R_AUDIO: {
        int q = c->audio_queue;
        swprintf(out, n, L"%d ms%ls", (int)((q - 1) * 5.33 + 39 + 0.5),
                 q == 8 ? L"  (normal)" : q == 16 ? L"  (old port)" : q < 8 ? L"  (may crackle)" : L"");
        break;
    }
    case R_FIDELITY:   swprintf(out, n, L"%ls", c->fidelity ? L"On" : L"Off"); break;
    case R_DISC:
        if (!c->iso[0]) swprintf(out, n, L"Choose…");
        else { file_name_of(c->iso, name, sizeof name); to_wide(name, out, n); }
        break;
    case R_SAVES: {
        WCHAR w[MAX_PATH];
        switch (launcher_save_kind(c)) {
        case LAUNCHER_SAVES_DOCUMENTS: swprintf(out, n, L"Automatic  (Documents)"); break;
        case LAUNCHER_SAVES_PORTABLE:  swprintf(out, n, L"Automatic  (Saves, portable)"); break;
        case LAUNCHER_SAVES_OLD_HDD:   swprintf(out, n, L"Automatic  (hdd, kept)"); break;
        default:
            to_wide(c->hdd, w, MAX_PATH);
            if (wcslen(w) > 26) swprintf(out, n, L"…%ls", w + wcslen(w) - 25);
            else swprintf(out, n, L"%ls", w);
        }
        break;
    }
    case R_OPEN_SAVES:
    case R_OPEN_SHOTS:
    case R_OPEN_LOG:   swprintf(out, n, L"Open"); break;
    case R_LOG:        swprintf(out, n, L"%ls", c->log_file ? L"On" : L"Off"); break;
    case R_MENU_SOUNDS: swprintf(out, n, L"%ls", c->menu_sounds ? L"On" : L"Off"); break;
    case R_ABOUT:      swprintf(out, n, L"Built %hs", __DATE__); break;
    }
}

static int step(int i, int n, int dir) { return ((i + dir) % n + n) % n; }

static void row_change(LUI *ui, int id, int dir)
{
    LauncherConfig *c = &ui->edit;
    switch (id) {
    case R_RES: {
        const Res *list;
        int nres = res_list(c->aspect, &list), i, cur = -1;
        for (i = 0; i < nres; i++) if (list[i].w == c->width && list[i].h == c->height) cur = i;
        cur = cur < 0 ? 0 : step(cur, nres, dir);
        c->width = list[cur].w;
        c->height = list[cur].h;
        break;
    }
    case R_DISPLAY:  c->fullscreen = !c->fullscreen; break;
    case R_SHAPE: {
        /* Switching shape keeps the height where it can (1440x1080 <-> 1920x1080). */
        const Res *list;
        int nres, i, best = 0, bestd = 0x7FFFFFFF;
        c->aspect = step(c->aspect, LAUNCHER_ASPECT_COUNT, dir);
        nres = res_list(c->aspect, &list);
        for (i = 0; i < nres; i++) {
            int d = abs(list[i].h - c->height);
            if (d < bestd) { bestd = d; best = i; }
        }
        c->width = list[best].w;
        c->height = list[best].h;
        break;
    }
    case R_WIDEFOV:
        if (c->aspect != LAUNCHER_ASPECT_43 && c->aspect != LAUNCHER_ASPECT_169)
            c->wide_fov = step(c->wide_fov, ASPECT_FOV_COUNT, dir);
        break;
    case R_SMAA:     c->smaa = k_smaa[step(idx_of(k_smaa, COUNT(k_smaa), c->smaa), COUNT(k_smaa), dir)]; break;
    case R_MSAA:     c->msaa = k_msaa[step(idx_of(k_msaa, COUNT(k_msaa), c->msaa), COUNT(k_msaa), dir)]; break;
    case R_TEX:      c->aniso = k_aniso[step(idx_of(k_aniso, COUNT(k_aniso), c->aniso), COUNT(k_aniso), dir)]; break;
    case R_SHADOWS:  c->soft_shadows = !c->soft_shadows; break;
    case R_FPSCAP:   c->fps_cap = k_fpscap[step(idx_of(k_fpscap, COUNT(k_fpscap), c->fps_cap), COUNT(k_fpscap), dir)]; break;
    case R_DRAWDIST: c->draw_dist = step(c->draw_dist, 3, dir); break;
    case R_SHOWFPS:  c->show_fps = !c->show_fps; break;
    case R_AUDIO:    c->audio_queue = ui->audio[step(idx_of(ui->audio, ui->naudio, c->audio_queue), ui->naudio, dir)]; break;
    case R_FIDELITY: c->fidelity = !c->fidelity; break;
    case R_LOG:      c->log_file = !c->log_file; break;
    case R_MENU_SOUNDS: c->menu_sounds = !c->menu_sounds; break;
    case R_SAVES:
        /* Automatic <-> the player's own folder (asked for the first time) */
        if (c->hdd[0]) {
            snprintf(ui->saves_custom, sizeof ui->saves_custom, "%s", c->hdd);
            c->hdd[0] = '\0';
        } else if (ui->saves_custom[0]) {
            snprintf(c->hdd, sizeof c->hdd, "%s", ui->saves_custom);
        } else {
            char got[MAX_PATH], cur[MAX_PATH];
            launcher_hdd_path(c, cur, sizeof cur);
            if (pick_path(ui->hwnd, TRUE, L"Choose the folder for your save games", cur, got, sizeof got))
                snprintf(c->hdd, sizeof c->hdd, "%s", got);
        }
        break;
    }
}

static void set_status(LUI *ui, int level, const char *msg)
{
    to_wide(msg, ui->status, 256);
    ui->status_level = level;
    ui->overlay_dirty = TRUE;
}

/* (Re)load the disc's pictures when the disc image changed; the conveyor's
 * cards are prepared by layout. */
static void snd_close(BOOL keep);

static void art_refresh(LUI *ui, const char *iso)
{
    int i;
    char why[512];
    if (ui->art.ok && strcmp(ui->art_iso, iso) == 0) return;
    snd_close(FALSE);                           /* the voices point into the old sounds */
    discart_free(&ui->art);
    for (i = 0; i < ART_TRACKS; i++) artimage_free(&ui->card[i]);
    artimage_free(&ui->base);
    ui->art_iso[0] = '\0';
    if (!launcher_check_iso(iso, why, sizeof why)) return;
    if (discart_load(iso, &ui->art)) {
        ArtImage *all[] = { &ui->art.logo, &ui->art.selbar };
        for (i = 0; i < COUNT(all); i++) premultiply(all[i]);
        for (i = 0; i < ART_RIDERS; i++) premultiply(&ui->art.rider[i]);
        for (i = 0; i < ART_TRACKS; i++) premultiply(&ui->art.track[i]);
        snprintf(ui->art_iso, sizeof ui->art_iso, "%s", iso);
    }
}

/* Track cards on the conveyor, in DIPs: the game's 16:5 shape, uniform. */
#define CARD_W   236.0f
#define CARD_GAP_X 18.0f
#define CARD_GAP_Y 20.0f
#define CONVEYOR_DEG -16.0f
#define CONVEYOR_SPEED 28.0f        /* DIPs a second */

/* Soften a premultiplied image in place: a box blur of radius r, twice
 * (horizontal then vertical), close to a small Gaussian. */
static void image_blur(ArtImage *im, int r)
{
    uint32_t *tmp;
    int x, y, pass;
    if (r < 1 || !im->px) return;
    tmp = (uint32_t *)malloc((size_t)im->w * im->h * 4);
    if (!tmp) return;
    for (pass = 0; pass < 2; pass++) {
        int horiz = pass == 0, len = horiz ? im->w : im->h, lines = horiz ? im->h : im->w;
        for (y = 0; y < lines; y++) {
            for (x = 0; x < len; x++) {
                uint32_t acc[4] = { 0, 0, 0, 0 }, n = 0;
                int k, c;
                for (k = x - r; k <= x + r; k++) {
                    uint32_t p;
                    if (k < 0 || k >= len) { n++; continue; }      /* transparent outside */
                    p = horiz ? im->px[(size_t)y * im->w + k] : im->px[(size_t)k * im->w + y];
                    for (c = 0; c < 4; c++) acc[c] += (p >> (c * 8)) & 255;
                    n++;
                }
                {
                    uint32_t q = 0;
                    for (c = 0; c < 4; c++) q |= (acc[c] / n) << (c * 8);
                    if (horiz) tmp[(size_t)y * im->w + x] = q;
                    else tmp[(size_t)x * im->w + y] = q;
                }
            }
        }
        memcpy(im->px, tmp, (size_t)im->w * im->h * 4);
    }
    free(tmp);
}

static void conveyor_prepare(LUI *ui)
{
    int i;
    for (i = 0; i < ART_TRACKS; i++) artimage_free(&ui->card[i]);
    artimage_free(&ui->base);
    if (!ui->art.ok) return;
    if (image_cover(&ui->art.backdrop, ui->frame.w, ui->frame.h, &ui->base)) {
        size_t j, n = (size_t)ui->base.w * ui->base.h;
        for (j = 0; j < n; j++) ui->base.px[j] = scale_px(ui->base.px[j], 205) | 0xFF000000u;
    }
    /* Uniform scale of the 16:5 cards, tilted, faded and softened once: the
     * conveyor then only moves them. */
    for (i = 0; i < ART_TRACKS; i++)
        if (image_prerender(&ui->art.track[i], CARD_W * ui->k / ui->art.track[i].w, CONVEYOR_DEG, 165, 0, &ui->card[i]))
            image_blur(&ui->card[i], (int)(1.6f * ui->k + 0.5f));
}

/* `im` (premultiplied) at (x, y) to a fraction of a pixel: one bilinear tap
 * pattern for the whole image, so the cards glide instead of stepping. */
static void blit_sub(Canvas *c, const ArtImage *im, float x, float y)
{
    int ix = (int)floorf(x), iy = (int)floorf(y), i, j;
    uint32_t wx = (uint32_t)((x - ix) * 256), wy = (uint32_t)((y - iy) * 256);
    uint32_t w00 = (256 - wx) * (256 - wy) >> 8, w10 = wx * (256 - wy) >> 8;
    uint32_t w01 = (256 - wx) * wy >> 8, w11 = wx * wy >> 8;
    int i0 = ix < 0 ? -ix : 0, j0 = iy < 0 ? -iy : 0;
    int i1 = ix + im->w + 1 > c->w ? c->w - ix : im->w + 1, j1 = iy + im->h + 1 > c->h ? c->h - iy : im->h + 1;
    for (j = j0; j < j1; j++) {
        uint32_t *d = c->px + (size_t)(iy + j) * c->w + ix;
        const uint32_t *r0 = j < im->h ? im->px + (size_t)j * im->w : NULL;           /* source row j   */
        const uint32_t *r1 = j > 0 ? im->px + (size_t)(j - 1) * im->w : NULL;         /* source row j-1 */
        for (i = i0; i < i1; i++) {
            uint32_t p00 = r0 && i < im->w ? r0[i] : 0, p10 = r0 && i > 0 ? r0[i - 1] : 0;
            uint32_t p01 = r1 && i < im->w ? r1[i] : 0, p11 = r1 && i > 0 ? r1[i - 1] : 0;
            uint32_t rb, ag, p;
            if (!(p00 | p10 | p01 | p11)) continue;
            rb = ((p00 & 0x00FF00FF) * w00 + (p10 & 0x00FF00FF) * w10 +
                  (p01 & 0x00FF00FF) * w01 + (p11 & 0x00FF00FF) * w11) >> 8 & 0x00FF00FF;
            ag = (((p00 >> 8) & 0x00FF00FF) * w00 + ((p10 >> 8) & 0x00FF00FF) * w10 +
                  ((p01 >> 8) & 0x00FF00FF) * w01 + ((p11 >> 8) & 0x00FF00FF) * w11) >> 8 & 0x00FF00FF;
            p = rb | (ag << 8);
            d[i] = over(d[i], p);
        }
    }
}

/* The moving backdrop: the ten loading cards of the tracks on a slanted
 * lattice, gliding along their rows (one full turn = ten cards, so the
 * picture loops without a seam). */
static void draw_conveyor(LUI *ui, Canvas *c)
{
    float la = (CARD_W + CARD_GAP_X) * ui->k, lb = (CARD_W * 5 / 16 + CARD_GAP_Y) * ui->k;
    float rad = CONVEYOR_DEG * 3.14159265f / 180.0f, dx = cosf(rad), dy = sinf(rad);
    float ax = dx * la, ay = dy * la;               /* along a row (rising to the right) */
    float bx = -dy * lb, by = dx * lb;              /* to the next row */
    float u = fmodf(ui->scroll, la * ART_TRACKS);
    int i, j;
    if (ui->base.px) memcpy(c->px, ui->base.px, (size_t)c->w * c->h * 4);
    else memset(c->px, 0, (size_t)c->w * c->h * 4);
    if (!ui->card[0].px) return;
    for (j = -8; j < 16; j++)
        for (i = -8; i < 16; i++) {
            const ArtImage *card = &ui->card[((i + j * 3) % ART_TRACKS + ART_TRACKS) % ART_TRACKS];
            float cx = i * ax + j * bx + (j & 1) * ax / 2 + u * dx - la;
            float cy = i * ay + j * by + (j & 1) * ay / 2 + u * dy;
            if (cx + card->w < 0 || cy + card->h < 0 || cx > c->w || cy > c->h) continue;
            blit_sub(c, card, cx, cy);
        }
}

/* One input, from the keyboard, the controller or the mouse wheel. */
enum { IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_OK, IN_BACK, IN_START, IN_PREV_TAB, IN_NEXT_TAB, IN_CLEAR, IN_DEFAULT };

/* ── Menu sounds and the PLAY fade ─────────────────────────
 *
 * The game's own front-end sounds (discart.h, read from the disc) through
 * XAudio2, one source voice per sound, at a moderate volume: a sound starts
 * within one audio quantum (~10 ms) of the input. XAudio2 is loaded from its
 * DLL as the runtime does (MinGW has the header, not the import library).
 * PLAY fades the window to black in FADE_MS; the "choice made" sound then
 * goes on into the game's start, and the engine is released when it ends.
 * Nothing of this runs with --direct / --play: the launcher is not shown. */

#define FADE_MS 300
#define SND_VOLUME 0.45f

static IXAudio2 *s_xa2;
static IXAudio2MasteringVoice *s_xa2_master;
static IXAudio2SourceVoice *s_xa2_voice[ART_SND_COUNT];
static BOOL s_xa2_failed;

static BOOL sounds_on(const LUI *ui)
{
    const LauncherConfig *c = ui->page == PAGE_SETTINGS ? &ui->edit : ui->cfg;
    return c->menu_sounds && ui->art.ok && ui->art.sounds_ok;
}

static BOOL snd_open(LUI *ui)
{
    typedef HRESULT (WINAPI *XA2CreateFn)(IXAudio2 **, UINT32, UINT32);
    static const char *const dlls[] = { "xaudio2_9.dll", "xaudio2_8.dll" };
    XA2CreateFn create = NULL;
    int i;
    if (s_xa2) return TRUE;
    if (s_xa2_failed) return FALSE;
    s_xa2_failed = TRUE;                        /* until it works */
    for (i = 0; i < 2 && !create; i++) {
        HMODULE lib = LoadLibraryA(dlls[i]);
        if (lib) create = (XA2CreateFn)(void *)GetProcAddress(lib, "XAudio2Create");
    }
    if (!create || FAILED(create(&s_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !s_xa2) { s_xa2 = NULL; return FALSE; }
    if (FAILED(IXAudio2_CreateMasteringVoice(s_xa2, &s_xa2_master, 0, 0, 0, NULL, NULL, 0))) {
        IXAudio2_Release(s_xa2); s_xa2 = NULL; return FALSE;
    }
    for (i = 0; i < ART_SND_COUNT; i++) {
        const ArtSound *a = &ui->art.sound[i];
        WAVEFORMATEX wf;
        memset(&wf, 0, sizeof wf);
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = (WORD)a->channels;
        wf.nSamplesPerSec = (DWORD)a->rate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)(2 * a->channels);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        if (FAILED(IXAudio2_CreateSourceVoice(s_xa2, &s_xa2_voice[i], &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)))
            s_xa2_voice[i] = NULL;
        else
            IXAudio2SourceVoice_SetVolume(s_xa2_voice[i], SND_VOLUME, XAUDIO2_COMMIT_NOW);
    }
    s_xa2_failed = FALSE;
    return TRUE;
}

static void snd_play(LUI *ui, int kind)
{
    IXAudio2SourceVoice *v;
    XAUDIO2_BUFFER b;
    const ArtSound *a;
    UINT64 played_before;
    if (kind < 0 || !sounds_on(ui) || !snd_open(ui) || !(v = s_xa2_voice[kind])) return;
    a = &ui->art.sound[kind];
    IXAudio2SourceVoice_Stop(v, 0, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_FlushSourceBuffers(v);
    memset(&b, 0, sizeof b);
    b.Flags = XAUDIO2_END_OF_STREAM;
    b.AudioBytes = (UINT32)a->frames * a->channels * 2;
    b.pAudioData = (const BYTE *)a->pcm;
    {
        XAUDIO2_VOICE_STATE st0;                /* SamplesPlayed runs on across sounds */
        IXAudio2SourceVoice_GetState(v, &st0, 0);
        played_before = st0.SamplesPlayed;
    }
    IXAudio2SourceVoice_SubmitSourceBuffer(v, &b, NULL);
    IXAudio2SourceVoice_Start(v, 0, XAUDIO2_COMMIT_NOW);
    {
        /* Test only: XBOX_LAUNCHER_SNDLOG=1 times each sound, from the input to
         * the voice's first samples played (waits for them, at most 200 ms). */
        static int log = -1;
        if (log < 0) { const char *e = getenv("XBOX_LAUNCHER_SNDLOG"); log = e && e[0] == '1'; }
        if (log) {
            static const char *const names[ART_SND_COUNT] = { "move", "change", "select", "back" };
            LARGE_INTEGER t0, t, fq;
            XAUDIO2_VOICE_STATE st;
            QueryPerformanceCounter(&t0);
            QueryPerformanceFrequency(&fq);
            do {
                IXAudio2SourceVoice_GetState(v, &st, 0);
                QueryPerformanceCounter(&t);
            } while (st.SamplesPlayed <= played_before && (t.QuadPart - t0.QuadPart) * 1000 < 200 * fq.QuadPart);
            fprintf(stderr, "[SND] %s: first samples played %.1f ms after the input\n", names[kind],
                    (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart);
        }
    }
}

static BOOL start_now(const LUI *ui) { return ui->start; }

/* Before the sounds' memory goes (a new disc image, the end of the
 * launcher). keep: the game starts -- the sound playing may finish, so the
 * engine and the samples are left to the process. */
static void snd_release(void)
{
    int i;
    for (i = 0; i < ART_SND_COUNT; i++)
        if (s_xa2_voice[i]) { IXAudio2SourceVoice_DestroyVoice(s_xa2_voice[i]); s_xa2_voice[i] = NULL; }
    if (s_xa2_master) { IXAudio2MasteringVoice_DestroyVoice(s_xa2_master); s_xa2_master = NULL; }
    IXAudio2_Release(s_xa2);
    s_xa2 = NULL;
}

/* After PLAY: the game is starting on the launcher's thread, so a small
 * thread lets the last sound finish (at most 5 s), then releases the engine
 * and the samples -- nothing of the launcher stays open during the game. */
static DWORD WINAPI snd_reaper(LPVOID arg)
{
    int16_t **pcm = (int16_t **)arg;
    DWORD t0 = GetTickCount();
    int i;
    for (;;) {
        BOOL busy = FALSE;
        for (i = 0; i < ART_SND_COUNT; i++)
            if (s_xa2_voice[i]) {
                XAUDIO2_VOICE_STATE st;
                IXAudio2SourceVoice_GetState(s_xa2_voice[i], &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (st.BuffersQueued) busy = TRUE;
            }
        if (!busy || GetTickCount() - t0 > 5000) break;
        Sleep(30);
    }
    snd_release();
    for (i = 0; i < ART_SND_COUNT; i++) free(pcm[i]);
    free(pcm);
    {
        const char *e = getenv("XBOX_LAUNCHER_SNDLOG");
        if (e && e[0] == '1')
            fprintf(stderr, "[SND] launcher audio released %lu ms after the game started\n", GetTickCount() - t0);
    }
    return 0;
}

/* Before the sounds' memory goes (a new disc image, the end of the
 * launcher). keep: the game starts -- let the sound playing finish first. */
static void snd_close(BOOL keep)
{
    int i;
    if (!s_xa2) return;
    if (keep && s_ui) {
        int16_t **pcm = (int16_t **)calloc(ART_SND_COUNT, sizeof *pcm);
        HANDLE th;
        if (pcm) {
            for (i = 0; i < ART_SND_COUNT; i++) { pcm[i] = s_ui->art.sound[i].pcm; s_ui->art.sound[i].pcm = NULL; }
            th = CreateThread(NULL, 0, snd_reaper, pcm, 0, NULL);
            if (th) { CloseHandle(th); return; }
            for (i = 0; i < ART_SND_COUNT; i++) s_ui->art.sound[i].pcm = pcm[i];   /* no thread: stop now */
            free(pcm);
        }
    }
    snd_release();
}

/* What changed with one input, to pick its sound. */
struct UiSnap { int page, sel, row, tab, bottom, ctl_col, capture; LauncherConfig edit; BOOL fading; };

static void ui_snap(const LUI *ui, struct UiSnap *s)
{
    s->page = ui->page; s->sel = ui->sel; s->row = ui->row; s->tab = ui->tab;
    s->bottom = ui->bottom; s->ctl_col = ui->ctl_col; s->capture = ui->capture;
    s->edit = ui->edit; s->fading = ui->fading;
}

static void ui_input_do(LUI *ui, int in);

/* in: IN_* for a key or a controller button, -1 for a mouse click. */
static void ui_sound_for(LUI *ui, int in, const struct UiSnap *b)
{
    int kind = -1;
    if (ui->done && !ui->start) return;                         /* QUIT: no sound */
    if (ui->fading && !b->fading) kind = ART_SND_SELECT;        /* PLAY */
    else if (b->page == PAGE_SETTINGS && ui->page != PAGE_SETTINGS) kind = ART_SND_BACK;
    else if (ui->page != b->page) kind = ART_SND_SELECT;        /* into SETTINGS */
    else if (memcmp(&b->edit, &ui->edit, sizeof b->edit) != 0) kind = ART_SND_CHANGE;
    else if (ui->capture && !b->capture) kind = ART_SND_SELECT; /* waiting for a key */
    else if (in == IN_OK && ui->status_level != 2) kind = ART_SND_SELECT;
    else if (ui->sel != b->sel || ui->row != b->row || ui->tab != b->tab ||
             ui->bottom != b->bottom || ui->ctl_col != b->ctl_col) kind = ART_SND_MOVE;
    snd_play(ui, kind);
}

/* ── Pages ─────────────────────────────────────────────────────────── */

enum { H_PLAY = 1, H_SETTINGS, H_QUIT, H_TAB0 = 10, H_ROW0 = 20, H_BACK = 40, H_SAVE };

static void left_shade(LUI *ui, Canvas *c, int strength)
{
    int x, w = (int)(c->w * 0.62f);
    for (x = 0; x < w; x++) {
        int a = strength * (w - x) / w;
        fill_rect(c, x, 0, 1, c->h, argb(a, 0, 0, 0));
    }
    (void)ui;
}

/* Only a problem is reported at the bottom (no disc image, wrong one). */
static void status_strip(LUI *ui, Canvas *c)
{
    WCHAR why[512];
    if (ui->iso_ok) return;
    fill_rect(c, 0, c->h - D(ui, 34), c->w, D(ui, 34), argb(185, 60, 0, 0));
    to_wide(ui->iso_why, why, 512);
    ui_text(ui, F_STRIP, RGB(255, 225, 210), 18, UI_H - 34, UI_W - 36, 34,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, why);
}

static void build_home(LUI *ui, Canvas *c)
{
    static const char *const items[3] = { "PLAY", "SETTINGS", "QUIT" };
    const DiscArt *a = &ui->art;
    int i;
    left_shade(ui, c, 225);
    {
        /* the random rider's loading card, 16:5 as in the game, uniform */
        const ArtImage *r = &a->rider[ui->rider];
        ArtImage card;
        float s = 410.0f * ui->k / r->w;
        if (image_prerender(r, s, 4.0f, 256, D(ui, 8), &card)) {
            blit(c, &card, c->w - card.w - D(ui, 22), D(ui, 268));
            free(card.px);
        }
    }
    draw_image(c, &a->logo, (float)D(ui, 34), (float)D(ui, 20), 1.3f * ui->k, 0, 256);
    for (i = 0; i < 3; i++) {
        float y = 262.0f + i * 60;
        BOOL on = ui->sel == i, off = i == 0 && !ui->iso_ok;
        if (on) {
            /* the game's yellow swoosh under the word */
            float s = 30.0f * ui->k / a->selbar.h;  /* uniform */
            draw_image(c, &a->selbar, 0, (float)D(ui, y + 22), s, 0, 256);
        }
        art_text(c, &a->title, (float)D(ui, 50), (float)D(ui, y + 2), 36 * ui->k, items[i], 0, 0, 0, 170);
        art_text(c, &a->title, (float)D(ui, 48), (float)D(ui, y), 36 * ui->k, items[i],
                 on ? 255 : off ? 150 : 255, on ? 214 : off ? 150 : 255, on ? 0 : off ? 150 : 255, 255);
        ui_hit(ui, H_PLAY + i, 0, y - 8, 420, 54);
    }
    status_strip(ui, c);
}

/* No disc image yet (or a wrong one): nothing of the game to show. */
static void build_first(LUI *ui, Canvas *c)
{
    static const WCHAR *const items[3] = { L"Choose disc image…", L"Settings", L"Quit" };
    WCHAR why[512];
    int i, y;
    for (y = 0; y < c->h; y++) {
        float t = (float)y / c->h;
        fill_rect(c, 0, y, c->w, 1, argb(255, (int)(235 - 60 * t), (int)(110 - 50 * t), 20));
    }
    ui_text(ui, F_BIG, RGB(255, 255, 255), 40, 40, 800, 80, DT_LEFT | DT_SINGLELINE, L"SSX Tricky");
    ui_text(ui, F_BODY, RGB(255, 240, 220), 44, 120, 860, 26, DT_LEFT | DT_SINGLELINE,
            L"Welcome. To play, choose your own SSX Tricky (USA) disc image (.iso).");
    for (i = 0; i < 3; i++) {
        float bx = 44, by = 170.0f + i * 56, bw = 300, bh = 44;
        BOOL on = ui->sel == i;
        fill_rect(c, D(ui, bx), D(ui, by), D(ui, bw), D(ui, bh), on ? argb(255, 255, 255, 255) : argb(70, 255, 255, 255));
        if (on) fill_rect(c, D(ui, bx), D(ui, by), D(ui, 6), D(ui, bh), argb(255, 255, 204, 0));
        ui_text(ui, F_BUTTON, on ? RGB(200, 70, 0) : RGB(255, 255, 255), bx + 24, by, bw - 24, bh,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE, items[i]);
        ui_hit(ui, H_PLAY + i, bx, by, bw, bh);
    }
    ui_text(ui, F_SMALL, RGB(255, 235, 215), 44, 348, 860, 44, DT_LEFT | DT_WORDBREAK,
            L"The menus then take on the look of the game itself (logo, art, lettering), read from your disc "
            L"each time: nothing from the game is stored in this program.");
    if (ui->cfg->iso[0] && !ui->iso_ok) {
        to_wide(ui->iso_why, why, 512);
        ui_text(ui, F_LABEL, RGB(255, 255, 255), 44, 400, 860, 56, DT_LEFT | DT_WORDBREAK, why);
    }
    if (ui->status[0])
        ui_text(ui, F_LABEL, RGB(255, 255, 255), 44, 460, 860, 56, DT_LEFT | DT_WORDBREAK, ui->status);
}

/* ── The Controls tab ──────────────────────────────────────
 *
 * One line per control of the Xbox pad: its keyboard key and its
 * controller button. Left / Right pick the column, Enter or A waits for the
 * new key or button (Esc cancels), Delete clears, Backspace or Y puts the
 * line back to its default; RESET (beside BACK / SAVE, pressed twice) puts
 * every line back. Same .ini sections as before ([Keyboard], [Controller]). */

#define CTL_LINES    CTL_COUNT          /* RESET is a button beside BACK / SAVE (human request) */
#define CTL_VISIBLE  9
#define CTL_Y0       142.0f
#define CTL_H        31.0f
#define CTL_KEY_X    430.0f
#define CTL_PAD_X    660.0f
#define CTL_COL_W    210.0f
enum { H_CELL0 = 100, H_RESET = 140 };  /* cells: + visible line * 2 + column; H_RESET: the RESET button */

/* Lines that share a key on purpose: the arrows drive the D-pad and the
 * left stick by default. */
static BOOL ctl_twins(int a, int b)
{
    static const int pairs[4][2] = { { CTL_DUP, CTL_LS_UP }, { CTL_DDOWN, CTL_LS_DOWN },
                                     { CTL_DLEFT, CTL_LS_LEFT }, { CTL_DRIGHT, CTL_LS_RIGHT } };
    int i;
    for (i = 0; i < 4; i++)
        if ((pairs[i][0] == a && pairs[i][1] == b) || (pairs[i][0] == b && pairs[i][1] == a)) return TRUE;
    return FALSE;
}

/* Another control on the same key (col 0) or controller button (col 1), or -1. */
static int ctl_conflict(const ControlMap *m, int ctl, int col)
{
    int j;
    if (col == 0) {
        if (!m->key[ctl]) return -1;
        for (j = 0; j < CTL_COUNT; j++)
            if (j != ctl && m->key[j] == m->key[ctl] && !ctl_twins(j, ctl)) return j;
    } else {
        if (ctl >= CTL_PAD_COUNT || m->pad[ctl] == PAD_NONE) return -1;
        for (j = 0; j < CTL_PAD_COUNT; j++)
            if (j != ctl && m->pad[j] == m->pad[ctl]) return j;
    }
    return -1;
}

static int settings_rows(LUI *ui)
{
    const int *rows;
    return ui->tab == TAB_CONTROLS ? CTL_LINES : tab_rows(ui->tab, &rows);
}

static void ctl_scroll_to(LUI *ui)
{
    if (ui->tab != TAB_CONTROLS || ui->row >= CTL_LINES) return;
    if (ui->row < ui->ctl_top) ui->ctl_top = ui->row;
    if (ui->row >= ui->ctl_top + CTL_VISIBLE) ui->ctl_top = ui->row - CTL_VISIBLE + 1;
}

static void ctl_capture_start(LUI *ui, int ctl, int col)
{
    WCHAR msg[160];
    if (col == 1 && ctl >= CTL_PAD_COUNT) {
        set_status(ui, 0, "The sticks of a controller are passed straight through.");
        return;
    }
    ui->capture = col == 0 ? 1 : 2;
    ui->capture_ctl = ctl;
    ui->capture_t = GetTickCount();
    controls_pad_snap(&ui->capture_snap);
    swprintf(msg, 160, col == 0 ? L"Press the key for %ls  (Esc cancels)" : L"Press the controller button for %ls  (Esc cancels)",
             controls_label(ctl));
    lstrcpynW(ui->status, msg, 256);
    ui->status_level = 0;
    ui->overlay_dirty = TRUE;
}

static void ctl_capture_end(LUI *ui, const WCHAR *msg)
{
    ui->capture = 0;
    ui->pad_quiet_until = GetTickCount() + 400;     /* the button just pressed must not also navigate */
    if (msg) { lstrcpynW(ui->status, msg, 256); ui->status_level = 0; }
    else ui->status[0] = 0;
    ui->overlay_dirty = TRUE;
}

/* A key while waiting for one (TRUE: consumed). */
static BOOL ctl_capture_key(LUI *ui, UINT msg, WPARAM wp, LPARAM lp)
{
    BYTE vk;
    if (!ui->capture || (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN)) return FALSE;
    if (wp == VK_ESCAPE) { ctl_capture_end(ui, NULL); return TRUE; }
    if (ui->capture != 1) return TRUE;                  /* waiting for a controller button */
    vk = controls_key_from_msg(wp, lp);
    if (!vk) {
        lstrcpynW(ui->status, L"That key belongs to the window (Alt, F10, F11, F12). Try another, or Esc.", 256);
        ui->overlay_dirty = TRUE;
        return TRUE;
    }
    ui->edit.controls.key[ui->capture_ctl] = vk;
    ctl_capture_end(ui, NULL);
    return TRUE;
}

/* Called from the timer while waiting for a controller button. */
static void ctl_capture_poll(LUI *ui)
{
    int p;
    if (ui->capture != 2) return;
    if (GetTickCount() - ui->capture_t > 8000) { ctl_capture_end(ui, L"No button was pressed."); return; }
    p = controls_pad_newly_pressed(&ui->capture_snap);
    if (p != PAD_NONE) {
        ui->edit.controls.pad[ui->capture_ctl] = (BYTE)p;
        ctl_capture_end(ui, NULL);
    }
}

static void ctl_activate(LUI *ui)
{
    if (ui->row < CTL_COUNT) ctl_capture_start(ui, ui->row, ui->ctl_col);
}

/* Backspace / Y: this line back to its default key and button. */
static void ctl_line_default(LUI *ui)
{
    ControlMap def;
    WCHAR msg[128];
    if (ui->tab != TAB_CONTROLS || ui->row >= CTL_COUNT) return;
    controls_defaults(&def);
    ui->edit.controls.key[ui->row] = def.key[ui->row];
    if (ui->row < CTL_PAD_COUNT) ui->edit.controls.pad[ui->row] = def.pad[ui->row];
    swprintf(msg, 128, L"%ls is back to its default.", controls_label(ui->row));
    lstrcpynW(ui->status, msg, 256);
    ui->status_level = 1;
    ui->overlay_dirty = TRUE;
}

/* RESET, pressed twice within 3 s: every line back to its default. */
static void ctl_reset_all(LUI *ui)
{
    DWORD now = GetTickCount();
    if (ui->reset_armed && now - ui->reset_armed_t < 3000) {
        controls_defaults(&ui->edit.controls);
        ui->reset_armed = FALSE;
        set_status(ui, 1, "Every key and button is back to its default. SAVE keeps it.");
    } else {
        ui->reset_armed = TRUE;
        ui->reset_armed_t = now;
        set_status(ui, 2, "Press RESET again to put every key and button back to its default.");
    }
}

static void ctl_clear(LUI *ui)
{
    if (ui->tab != TAB_CONTROLS || ui->row >= CTL_COUNT) return;
    if (ui->ctl_col == 0) ui->edit.controls.key[ui->row] = 0;
    else if (ui->row < CTL_PAD_COUNT) ui->edit.controls.pad[ui->row] = PAD_NONE;
    ui->overlay_dirty = TRUE;
}

static void build_controls(LUI *ui, Canvas *c)
{
    const ControlMap *m = &ui->edit.controls;
    int v, conflict = -1;
    ui_text(ui, F_SMALL, RGB(170, 175, 190), 70, 108, 300, 28, DT_LEFT | DT_VCENTER | DT_SINGLELINE, L"XBOX CONTROL");
    ui_text(ui, F_SMALL, RGB(170, 175, 190), CTL_KEY_X, 108, CTL_COL_W, 28, DT_CENTER | DT_VCENTER | DT_SINGLELINE, L"KEYBOARD");
    ui_text(ui, F_SMALL, RGB(170, 175, 190), CTL_PAD_X, 108, CTL_COL_W, 28, DT_CENTER | DT_VCENTER | DT_SINGLELINE, L"CONTROLLER");
    for (v = 0; v < CTL_VISIBLE; v++) {
        int line = ui->ctl_top + v, col;
        float y = CTL_Y0 + v * CTL_H;
        BOOL on = ui->row == line;
        if (line >= CTL_LINES) break;
        if (on) {
            fill_rect(c, D(ui, 48), D(ui, y), D(ui, UI_W - 96), D(ui, CTL_H - 3), argb(50, 255, 204, 0));
            fill_rect(c, D(ui, 48), D(ui, y), D(ui, 4), D(ui, CTL_H - 3), argb(255, 255, 204, 0));
        }
        ui_text(ui, F_LABEL, RGB(255, 255, 255), 70, y, 340, CTL_H - 3, DT_LEFT | DT_VCENTER | DT_SINGLELINE, controls_label(line));
        for (col = 0; col < 2; col++) {
            float x = col == 0 ? CTL_KEY_X : CTL_PAD_X;
            WCHAR t[64];
            BOOL cell = on && ui->ctl_col == col, waiting = ui->capture && ui->capture_ctl == line && ui->capture - 1 == col;
            BOOL fixed = col == 1 && line >= CTL_PAD_COUNT;
            int other = ctl_conflict(m, line, col);
            if (cell) {
                fill_rect(c, D(ui, x), D(ui, y + 1), D(ui, CTL_COL_W), D(ui, CTL_H - 5), argb(waiting ? 230 : 110, 255, 204, 0));
                if (other >= 0) conflict = other;
            }
            if (waiting) lstrcpyW(t, col == 0 ? L"press a key…" : L"press a button…");
            else if (fixed) lstrcpyW(t, line < CTL_RS_UP ? L"left stick" : L"right stick");
            else if (col == 0) { controls_key_label(m->key[line], t, 64); if (!t[0]) lstrcpyW(t, L"—"); }
            else { lstrcpynW(t, controls_pad_label(m->pad[line]), 64); if (!t[0]) lstrcpyW(t, L"—"); }
            ui_text(ui, F_VALUE, waiting || (cell && !fixed) ? RGB(20, 20, 28) : fixed ? RGB(140, 145, 160)
                                 : other >= 0 ? RGB(255, 140, 60) : RGB(255, 214, 60),
                    x, y, CTL_COL_W, CTL_H - 3, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, t);
            ui_hit(ui, H_CELL0 + v * 2 + col, x, y, CTL_COL_W, CTL_H - 3);
        }
    }
    /* a thin scroll bar when the list is longer than the panel */
    {
        float top = CTL_Y0, h = CTL_VISIBLE * CTL_H - 3;
        float t0 = top + h * ui->ctl_top / CTL_LINES, t1 = top + h * (ui->ctl_top + CTL_VISIBLE) / CTL_LINES;
        fill_rect(c, D(ui, UI_W - 56), D(ui, top), D(ui, 3), D(ui, h), argb(60, 255, 255, 255));
        fill_rect(c, D(ui, UI_W - 56), D(ui, t0), D(ui, 3), D(ui, t1 - t0), argb(220, 255, 204, 0));
    }
    if (!ui->status[0]) {
        WCHAR help[200];
        if (conflict >= 0)
            swprintf(help, 200, L"Also used by %ls. Enter / A: change it  ·  Delete: clear  ·  Backspace / Y: default", controls_label(conflict));
        else
            swprintf(help, 200, L"Enter / A: change  ·  Left / Right: keyboard or controller  ·  Delete: clear  ·  Backspace / Y: default  ·  Esc: back");
        ui_text(ui, F_SMALL, conflict >= 0 ? RGB(255, 170, 90) : RGB(220, 220, 220), 70, UI_H - 86, UI_W - 140, 36,
                DT_LEFT | DT_WORDBREAK, help);
    }
}

static void build_settings(LUI *ui, Canvas *c)
{
    const DiscArt *a = ui->art.ok ? &ui->art : NULL;
    const int *rows;
    int n, i;
    float x;
    if (!a) {
        int y;
        for (y = 0; y < c->h; y++) {
            float t = (float)y / c->h;
            fill_rect(c, 0, y, c->w, 1, argb(255, (int)(235 - 60 * t), (int)(110 - 50 * t), 20));
        }
    }
    fill_rect(c, 0, 0, c->w, c->h, argb(a ? 110 : 40, 0, 0, 0));
    fill_rect(c, D(ui, 40), D(ui, 102), D(ui, UI_W - 80), D(ui, UI_H - 150), argb(190, 10, 12, 20));
    if (a) {
        art_text(c, &a->title, (float)D(ui, 46), (float)D(ui, 20), 42 * ui->k, "SETTINGS", 0, 0, 0, 150);
        art_text(c, &a->title, (float)D(ui, 44), (float)D(ui, 18), 42 * ui->k, "SETTINGS", 255, 255, 255, 255);
    } else {
        ui_text(ui, F_TITLE, RGB(255, 255, 255), 44, 18, 400, 40, DT_LEFT | DT_SINGLELINE, L"SETTINGS");
    }
    /* tabs */
    x = 44;
    for (i = 0; i < TAB_COUNT; i++) {
        float tw = (a ? art_text_width(&a->title, 22 * ui->k, k_tab_name[i]) / ui->k : 110) + 34;
        BOOL on = ui->tab == i;
        if (on) fill_slanted(c, D(ui, x - 4), D(ui, 66), D(ui, tw), D(ui, 32), D(ui, 10), argb(255, 255, 204, 0));
        if (a) {
            art_text(c, &a->title, (float)D(ui, x + 10), (float)D(ui, 72), 22 * ui->k, k_tab_name[i],
                     on ? 20 : 255, on ? 20 : 255, on ? 28 : 255, on ? 255 : 210);
        } else {
            WCHAR w[16];
            MultiByteToWideChar(CP_ACP, 0, k_tab_name[i], -1, w, 16);
            ui_text(ui, F_LABEL, on ? RGB(20, 20, 28) : RGB(255, 255, 255), x + 10, 66, tw, 32,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE, w);
        }
        ui_hit(ui, H_TAB0 + i, x - 4, 64, tw, 36);
        x += tw + 8;
    }
    tab_rows(ui->tab, &rows);
    n = settings_rows(ui);
    /* rows */
    if (ui->tab == TAB_CONTROLS) build_controls(ui, c);
    else for (i = 0; i < n; i++) {
        /* 9 rows, 10, 11 stay above the help line */
        float rh = n > 10 ? 30.0f : n > 9 ? 32.0f : 36.0f;
        float y = 112.0f + i * (n > 10 ? 31 : n > 9 ? 34 : n > 8 ? 37 : 40);
        WCHAR v[128];
        BOOL on = ui->row == i, act = row_is_action(rows[i]);
        if (on) {
            fill_rect(c, D(ui, 48), D(ui, y - 4), D(ui, UI_W - 96), D(ui, rh), argb(60, 255, 204, 0));
            fill_rect(c, D(ui, 48), D(ui, y - 4), D(ui, 4), D(ui, rh), argb(255, 255, 204, 0));
        }
        ui_text(ui, F_LABEL, RGB(255, 255, 255), 70, y - 4, 440, rh, DT_LEFT | DT_VCENTER | DT_SINGLELINE, row_label(rows[i]));
        row_value(ui, rows[i], v, 128);
        ui_text(ui, F_VALUE, act ? RGB(255, 255, 255) : RGB(255, 214, 60), 560, y - 4, 300, rh,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, v);
        if (!act) {
            ui_text(ui, F_SYMBOL, RGB(255, 140, 0), 540, y - 4, 24, rh, DT_CENTER | DT_VCENTER | DT_SINGLELINE, L"◄");
            ui_text(ui, F_SYMBOL, RGB(255, 140, 0), 856, y - 4, 24, rh, DT_CENTER | DT_VCENTER | DT_SINGLELINE, L"►");
        }
        ui_hit(ui, H_ROW0 + i, 48, y - 4, UI_W - 96, rh);
    }
    /* help or status line */
    if (ui->status[0])
        ui_text(ui, F_SMALL, ui->status_level == 2 ? RGB(255, 120, 100) : ui->status_level == 1 ? RGB(140, 230, 140) : RGB(230, 230, 230),
                70, UI_H - 86, UI_W - 140, 36, DT_LEFT | DT_WORDBREAK, ui->status);
    else if (ui->row < n && ui->tab != TAB_CONTROLS)
        ui_text(ui, F_SMALL, RGB(220, 220, 220), 70, UI_H - 86, UI_W - 140, 36, DT_LEFT | DT_WORDBREAK, row_help(rows[ui->row]));
    /* BACK / SAVE */
    for (i = 0; i < (ui->tab == TAB_CONTROLS ? 3 : 2); i++) {
        float bx = i == 0 ? 52.0f : i == 1 ? UI_W - 210.0f : UI_W / 2.0f - 74.0f, by = UI_H - 42.0f;
        BOOL on = ui->row == n && ui->bottom == i;
        const char *t = i == 0 ? "BACK" : i == 1 ? "SAVE" : "RESET";
        if (on) fill_slanted(c, D(ui, bx - 8), D(ui, by - 4), D(ui, 160), D(ui, 40), D(ui, 10), argb(255, 255, 204, 0));
        if (a) {
            art_text(c, &a->title, (float)D(ui, bx + 14), (float)D(ui, by + 4), 26 * ui->k, t,
                     on ? 20 : 255, on ? 20 : (i ? 204 : 255), on ? 28 : (i ? 0 : 255), 255);
        } else {
            ui_text(ui, F_BUTTON, on ? RGB(20, 20, 28) : RGB(255, 255, 255), bx + 10, by - 4, 140, 40,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE, i == 0 ? L"Back" : i == 1 ? L"Save" : L"Reset");
        }
        ui_hit(ui, i == 0 ? H_BACK : i == 1 ? H_SAVE : H_RESET, bx - 8, by - 4, 160, 40);
    }
    if (ui->tab != TAB_CONTROLS)            /* there, RESET sits in the middle and the page's own help says it */
    ui_text(ui, F_SMALL, RGB(235, 235, 235), 260, UI_H - 44, 440, 36, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
            L"Esc: back  ·  LB / RB or Tab: tabs");
}

static void overlay_build(LUI *ui)
{
    Canvas *c = &ui->overlay;
    memset(c->px, 0, (size_t)c->w * c->h * 4);
    ui->ntext = 0;
    ui->nhit = 0;
    if (ui->page == PAGE_HOME) build_home(ui, c);
    else if (ui->page == PAGE_SETTINGS) build_settings(ui, c);
    else build_first(ui, c);
    ui->overlay_dirty = FALSE;
}

static void frame_render(LUI *ui)
{
    Canvas *f = &ui->frame;
    int i;
    size_t j, n = (size_t)f->w * f->h;
    if (!f->px) return;
    if (ui->overlay_dirty) overlay_build(ui);
    if (ui->page != PAGE_FIRST && ui->art.ok) draw_conveyor(ui, f);
    else memset(f->px, 0, n * 4);
    for (j = 0; j < n; j++) {
        uint32_t s = ui->overlay.px[j];
        if (s >> 24 == 255) f->px[j] = s;
        else if (s) f->px[j] = over(f->px[j], s);
    }
    GdiFlush();
    SetBkMode(f->dc, TRANSPARENT);
    for (i = 0; i < ui->ntext; i++) {
        TextOp *t = &ui->text[i];
        SelectObject(f->dc, ui->font[t->font]);
        if (t->font == F_LABEL || t->font == F_BUTTON || t->font == F_TITLE || t->font == F_BIG) {
            RECT r = t->r;                          /* a soft shadow for legibility */
            OffsetRect(&r, 1, 1);
            SetTextColor(f->dc, RGB(0, 0, 0));
            DrawTextW(f->dc, t->text, -1, &r, t->flags);
        }
        SetTextColor(f->dc, t->col);
        DrawTextW(f->dc, t->text, -1, &t->r, t->flags);
    }
    GdiFlush();
    if (ui->fading) {                           /* to black before the game opens */
        LARGE_INTEGER now, fq;
        double k;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&fq);
        k = (double)(now.QuadPart - ui->fade_t0.QuadPart) / (double)fq.QuadPart / (FADE_MS / 1000.0);
        if (k > 1) k = 1;
        fill_rect(f, 0, 0, f->w, f->h, argb((int)(k * 255), 0, 0, 0));
    }
}

/* Size everything for the window's DPI; the client area is UI_W x UI_H DIPs,
 * shrunk uniformly if the screen is smaller. */
static void ui_layout(LUI *ui, UINT dpi, const RECT *suggested)
{
    MONITORINFO mi;
    RECT r;
    DWORD style = (DWORD)GetWindowLongPtrW(ui->hwnd, GWL_STYLE);
    float k = dpi / 96.0f, fit;
    int cw, ch;
    {
        /* Test only: XBOX_LAUNCHER_SCALE=1.5 draws as at 150 % DPI. */
        const char *e = getenv("XBOX_LAUNCHER_SCALE");
        if (e && atof(e) >= 0.5 && atof(e) <= 4) k = (float)atof(e);
    }
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(ui->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    fit = 0.94f * (mi.rcWork.bottom - mi.rcWork.top) / UI_H;
    if (fit < k) k = fit;
    if (0.94f * (mi.rcWork.right - mi.rcWork.left) / UI_W < k) k = 0.94f * (mi.rcWork.right - mi.rcWork.left) / UI_W;
    if (k < 0.5f) k = 0.5f;
    ui->k = k;
    cw = (int)(UI_W * k + 0.5f);
    ch = (int)(UI_H * k + 0.5f);
    SetRect(&r, 0, 0, cw, ch);
    AdjustWindowRectEx(&r, style, FALSE, 0);
    if (suggested)
        SetWindowPos(ui->hwnd, NULL, suggested->left, suggested->top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    else
        SetWindowPos(ui->hwnd, NULL,
                     mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (r.right - r.left)) / 2,
                     mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (r.bottom - r.top)) / 2,
                     r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    canvas_free(&ui->frame);
    canvas_free(&ui->overlay);
    canvas_make(&ui->frame, cw, ch, TRUE);
    canvas_make(&ui->overlay, cw, ch, FALSE);
    fonts_build(ui);
    conveyor_prepare(ui);
    ui->overlay_dirty = TRUE;
}

/* ── Actions ───────────────────────────────────────────────────────── */

static void iso_refresh(LUI *ui)
{
    ui->iso_ok = launcher_check_iso(ui->cfg->iso, ui->iso_why, sizeof ui->iso_why);
    if (ui->iso_ok) {
        art_refresh(ui, ui->cfg->iso);
        conveyor_prepare(ui);
    }
    if (ui->page != PAGE_SETTINGS)
        ui->page = ui->iso_ok && ui->art.ok ? PAGE_HOME : PAGE_FIRST;
    ui->overlay_dirty = TRUE;
}

static void settings_open(LUI *ui)
{
    int i, found = 0;
    ui->edit = *ui->cfg;
    ui->page = PAGE_SETTINGS;
    ui->tab = TAB_VIDEO;
    ui->row = 0;
    ui->bottom = 1;
    ui->status[0] = 0;
    /* Delay ~ (buffers - 1) x 5.33 ms + ~39 ms in XAudio2; an .ini
     * value outside the list stays selectable, so Save keeps it. */
    for (i = 0; i < AUDIO_CHOICES; i++) {
        ui->audio[i] = k_audio[i];
        if (k_audio[i] == ui->edit.audio_queue) found = 1;
    }
    ui->naudio = AUDIO_CHOICES;
    if (!found) ui->audio[ui->naudio++] = ui->edit.audio_queue;
    ui->overlay_dirty = TRUE;
}

static void settings_close(LUI *ui)
{
    ui->status[0] = 0;
    ui->page = PAGE_HOME;
    ui->sel = 1;
    iso_refresh(ui);
}

static BOOL settings_save(LUI *ui)
{
    char why[512];
    if (ui->edit.iso[0] && !launcher_check_iso(ui->edit.iso, why, sizeof why)) {
        set_status(ui, 2, why);
        return FALSE;
    }
    if (!launcher_config_save(&ui->edit)) {
        char ini[MAX_PATH], msg[MAX_PATH + 64];
        launcher_config_path(ini, sizeof ini);
        snprintf(msg, sizeof msg, "Could not write the settings file: %s", ini);
        set_status(ui, 2, msg);
        return FALSE;
    }
    *ui->cfg = ui->edit;
    return TRUE;
}

static void choose_disc(LUI *ui, char *iso, size_t n)
{
    char got[MAX_PATH], why[512];
    if (!pick_path(ui->hwnd, FALSE, L"Choose the SSX Tricky disc image", iso, got, sizeof got)) return;
    if (!launcher_check_iso(got, why, sizeof why)) { set_status(ui, 2, why); return; }
    snprintf(iso, n, "%s", got);
    set_status(ui, 1, "Disc image OK: SSX Tricky (USA).");
}

static void row_action(LUI *ui, int id)
{
    char path[MAX_PATH];
    ui->status[0] = 0;
    switch (id) {
    case R_DISC:
        choose_disc(ui, ui->edit.iso, sizeof ui->edit.iso);
        break;
    case R_OPEN_SAVES:
        launcher_hdd_path(&ui->edit, path, sizeof path);
        launcher_open_path(ui->hwnd, path, TRUE);
        break;
    case R_OPEN_SHOTS:
        exe_dir(path, sizeof path);
        strncat(path, "\\Screenshots", sizeof path - strlen(path) - 1);
        launcher_open_path(ui->hwnd, path, TRUE);
        break;
    case R_OPEN_LOG:
        launcher_log_path(path, sizeof path);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            set_status(ui, 0, "There is no log yet: turn the log file on and play once.");
        else
            launcher_open_path(ui->hwnd, path, FALSE);
        break;
    case R_ABOUT: {
        char ini[MAX_PATH], msg[MAX_PATH + 64];
        launcher_config_path(ini, sizeof ini);
        snprintf(msg, sizeof msg, "Settings file: %s", ini);
        set_status(ui, 0, msg);
        break;
    }
    }
    ui->overlay_dirty = TRUE;
}

static void activate_home(LUI *ui, int item)
{
    if (ui->page == PAGE_FIRST) {
        if (item == 0) {
            choose_disc(ui, ui->cfg->iso, sizeof ui->cfg->iso);
            if (ui->status_level == 1) {
                launcher_config_save(ui->cfg);
                ui->status[0] = 0;
                iso_refresh(ui);
                ui->sel = 0;
            }
        } else if (item == 1) {
            settings_open(ui);
        } else {
            ui->done = TRUE;
        }
    } else {
        if (item == 0) { if (ui->iso_ok && !ui->fading) { ui->fading = TRUE; QueryPerformanceCounter(&ui->fade_t0); } }
        else if (item == 1) settings_open(ui);
        else ui->done = TRUE;
    }
    ui->overlay_dirty = TRUE;
}


static void ui_input(LUI *ui, int in)
{
    struct UiSnap before;
    if (ui->fading) return;
    ui_snap(ui, &before);
    ui_input_do(ui, in);
    ui_sound_for(ui, in, &before);
}

static void ui_input_do(LUI *ui, int in)
{
    if (ui->page != PAGE_SETTINGS) {
        if (in == IN_UP) ui->sel = step(ui->sel, 3, -1);
        else if (in == IN_DOWN) ui->sel = step(ui->sel, 3, 1);
        else if (in == IN_OK) activate_home(ui, ui->sel);
        else if (in == IN_START && ui->page == PAGE_HOME) activate_home(ui, 0);
        else if (in == IN_BACK && ui->page == PAGE_HOME) ui->sel = 2;
        ui->overlay_dirty = TRUE;
        return;
    }
    {
        const int *rows;
        int n = settings_rows(ui);
        BOOL ctl = ui->tab == TAB_CONTROLS;
        tab_rows(ui->tab, &rows);
        switch (in) {
        case IN_UP:    ui->row = step(ui->row, n + 1, -1); ui->status[0] = 0; ui->reset_armed = FALSE; ctl_scroll_to(ui); break;
        case IN_DOWN:  ui->row = step(ui->row, n + 1, 1); ui->status[0] = 0; ui->reset_armed = FALSE; ctl_scroll_to(ui); break;
        case IN_LEFT:
        case IN_RIGHT:
            if (ui->row == n && ctl) {                  /* BACK - RESET - SAVE */
                static const int order[3] = { 0, 2, 1 };
                int pos = ui->bottom == 0 ? 0 : ui->bottom == 2 ? 1 : 2;
                pos = in == IN_RIGHT ? (pos < 2 ? pos + 1 : 2) : (pos > 0 ? pos - 1 : 0);
                ui->bottom = order[pos];
            } else if (ui->row == n) ui->bottom = in == IN_RIGHT;
            else if (ctl) { ui->ctl_col = in == IN_RIGHT; ui->status[0] = 0; }
            else if (!row_is_action(rows[ui->row])) { row_change(ui, rows[ui->row], in == IN_RIGHT ? 1 : -1); ui->status[0] = 0; }
            break;
        case IN_OK:
            if (ui->row == n) {
                if (ui->bottom == 0) settings_close(ui);
                else if (ui->bottom == 2) ctl_reset_all(ui);
                else if (settings_save(ui)) settings_close(ui);
            } else if (ctl) ctl_activate(ui);
            else if (row_is_action(rows[ui->row])) row_action(ui, rows[ui->row]);
            else row_change(ui, rows[ui->row], 1);
            break;
        case IN_CLEAR: ctl_clear(ui); break;
        case IN_DEFAULT: ctl_line_default(ui); break;
        case IN_BACK:  settings_close(ui); break;
        case IN_START: if (settings_save(ui)) settings_close(ui); break;
        case IN_PREV_TAB:
        case IN_NEXT_TAB:
            ui->tab = step(ui->tab, TAB_COUNT, in == IN_NEXT_TAB ? 1 : -1);
            ui->row = 0;
            if (ui->bottom == 2) ui->bottom = 1;
            ui->ctl_top = ui->ctl_col = 0;
            ui->status[0] = 0;
            break;
        }
    }
    ui->overlay_dirty = TRUE;
}

static int hit_test(LUI *ui, int x, int y)
{
    int i;
    POINT p;
    p.x = x; p.y = y;
    for (i = 0; i < ui->nhit; i++) if (PtInRect(&ui->hit[i], p)) return ui->hit_id[i];
    return 0;
}

static void ui_mouse_do(LUI *ui, int x, int y, BOOL click);

static void ui_mouse(LUI *ui, int x, int y, BOOL click)
{
    struct UiSnap before;
    if (ui->fading) return;
    ui_snap(ui, &before);
    ui_mouse_do(ui, x, y, click);
    if (click) ui_sound_for(ui, -1, &before);
}

static void ui_mouse_do(LUI *ui, int x, int y, BOOL click)
{
    int id = hit_test(ui, x, y);
    if (!id) return;
    if (id >= H_PLAY && id <= H_QUIT) {
        if (ui->sel != id - H_PLAY) { ui->sel = id - H_PLAY; ui->overlay_dirty = TRUE; }
        if (click) activate_home(ui, ui->sel);
        return;
    }
    if (id >= H_TAB0 && id < H_TAB0 + TAB_COUNT) {
        if (click && ui->tab != id - H_TAB0) {
            ui->tab = id - H_TAB0; ui->row = 0; ui->ctl_top = ui->ctl_col = 0; ui->status[0] = 0; ui->overlay_dirty = TRUE;
        }
        return;
    }
    if (id >= H_ROW0 && id < H_ROW0 + 10) {
        const int *rows;
        int r = id - H_ROW0;
        tab_rows(ui->tab, &rows);
        if (ui->row != r) { ui->row = r; ui->status[0] = 0; ui->overlay_dirty = TRUE; }
        if (click) {
            if (row_is_action(rows[r])) row_action(ui, rows[r]);
            else row_change(ui, rows[r], x < D(ui, 700) ? -1 : 1);   /* left half: back a step */
            ui->overlay_dirty = TRUE;
        }
        return;
    }
    if (id == H_RESET) {
        int n = settings_rows(ui);
        if (ui->row != n || ui->bottom != 2) { ui->row = n; ui->bottom = 2; ui->overlay_dirty = TRUE; }
        if (click) { ctl_reset_all(ui); ui->overlay_dirty = TRUE; }
        return;
    }
    if (id >= H_CELL0 && id < H_CELL0 + CTL_VISIBLE * 2) {
        int line = ui->ctl_top + (id - H_CELL0) / 2;
        int col = (id - H_CELL0) % 2;
        if (ui->capture) return;
        if (ui->row != line || ui->ctl_col != col) { ui->row = line; ui->ctl_col = col; ui->status[0] = 0; ui->overlay_dirty = TRUE; }
        if (click) ctl_activate(ui);
        return;
    }
    if (id == H_BACK || id == H_SAVE) {
        const int *rows;
        int n = tab_rows(ui->tab, &rows);
        if (ui->row != n || ui->bottom != (id == H_SAVE)) { ui->row = n; ui->bottom = id == H_SAVE; ui->overlay_dirty = TRUE; }
        if (click) ui_input(ui, IN_OK);
    }
}

/* Controller: D-pad / left stick move, A presses, B goes back, Start plays
 * (or saves), LB / RB change tab. Polled; held directions repeat. */
static void ui_poll_pad(LUI *ui)
{
    XINPUT_STATE st;
    DWORD i, now = GetTickCount();
    WORD b = 0, pressed, dirs;
    for (i = 0; i < 4; i++) {
        memset(&st, 0, sizeof st);
        if (XInputGetState(i, &st) == ERROR_SUCCESS) {
            b |= st.Gamepad.wButtons;
            if (st.Gamepad.sThumbLY > 16000)  b |= XINPUT_GAMEPAD_DPAD_UP;
            if (st.Gamepad.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (st.Gamepad.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (st.Gamepad.sThumbLX > 16000)  b |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }
    }
    if ((LONG)(now - ui->pad_quiet_until) < 0) { ui->pad_prev = b; return; }   /* just after a capture */
    pressed = b & (WORD)~ui->pad_prev;
    dirs = b & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
    if (dirs && !(pressed & dirs) && (LONG)(now - ui->pad_repeat_at) >= 0) {
        pressed |= dirs;
        ui->pad_repeat_at = now + 90;
    } else if (pressed & dirs) {
        ui->pad_repeat_at = now + 380;
    }
    ui->pad_prev = b;
    if (!pressed || GetForegroundWindow() != ui->hwnd) return;
    if (pressed & XINPUT_GAMEPAD_DPAD_UP)    ui_input(ui, IN_UP);
    if (pressed & XINPUT_GAMEPAD_DPAD_DOWN)  ui_input(ui, IN_DOWN);
    if (pressed & XINPUT_GAMEPAD_DPAD_LEFT)  ui_input(ui, IN_LEFT);
    if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) ui_input(ui, IN_RIGHT);
    if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)  ui_input(ui, IN_PREV_TAB);
    if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) ui_input(ui, IN_NEXT_TAB);
    if (pressed & XINPUT_GAMEPAD_A)     ui_input(ui, IN_OK);
    if (pressed & XINPUT_GAMEPAD_B)     ui_input(ui, IN_BACK);
    if (pressed & XINPUT_GAMEPAD_START) ui_input(ui, IN_START);
    if (pressed & XINPUT_GAMEPAD_Y)     ui_input(ui, IN_DEFAULT);
}

static LRESULT CALLBACK ui_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LUI *ui = s_ui;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        /* No frame: anything that is not a button or a row drags the window. */
        if (ui) {
            POINT p;
            p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
            ScreenToClient(h, &p);
            return hit_test(ui, p.x, p.y) ? HTCLIENT : HTCAPTION;
        }
        break;
    case WM_NCLBUTTONDBLCLK:
        return 0;                               /* the size is fixed: no maximise on double-click */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (ui && ui->frame.dc) {
            frame_render(ui);
            BitBlt(dc, 0, 0, ui->frame.w, ui->frame.h, ui->frame.dc, 0, 0, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (ui && wp == TIMER_ID) {
            LARGE_INTEGER now, fq;
            BOOL active = GetForegroundWindow() == h && !IsIconic(h);
            double dt;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&fq);
            dt = (double)(now.QuadPart - ui->last_qpc.QuadPart) / (double)fq.QuadPart;
            ui->last_qpc = now;
            if (dt < 0 || dt > 0.1) dt = 0.1;     /* after a pause, a modal dialog, a drag */
            if (ui->fading) {
                LARGE_INTEGER fq;
                QueryPerformanceFrequency(&fq);
                InvalidateRect(h, NULL, FALSE);
                if ((double)(now.QuadPart - ui->fade_t0.QuadPart) / (double)fq.QuadPart * 1000.0 >= FADE_MS + ANIM_MS) {
                    ui->start = TRUE;           /* the last frame drawn was black */
                    ui->done = TRUE;
                }
            }
            if (ui->capture == 2) ctl_capture_poll(ui);
            else if (active && !ui->capture) ui_poll_pad(ui);
            if (ui->capture == 2 && GetForegroundWindow() != h) ctl_capture_end(ui, NULL);
            if (active && ui->page != PAGE_FIRST && ui->art.ok) {
                /* position from the real time elapsed, to a fraction of a pixel */
                ui->anim_s += dt;
                ui->scroll = (float)fmod(ui->anim_s * CONVEYOR_SPEED * ui->k,
                                         (CARD_W + CARD_GAP_X) * ui->k * ART_TRACKS);
                InvalidateRect(h, NULL, FALSE);
            } else if (ui->overlay_dirty) {
                InvalidateRect(h, NULL, FALSE);
            }
            if (ui->done) DestroyWindow(h);
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (ui && ui->capture) { ctl_capture_key(ui, msg, wp, lp); InvalidateRect(h, NULL, FALSE); return 0; }
        break;
    case WM_KEYDOWN:
        if (!ui) break;
        if (ctl_capture_key(ui, msg, wp, lp)) { InvalidateRect(h, NULL, FALSE); return 0; }
        switch (wp) {
        case VK_DELETE: ui_input(ui, IN_CLEAR); break;
        case VK_UP:     ui_input(ui, IN_UP); break;
        case VK_DOWN:   ui_input(ui, IN_DOWN); break;
        case VK_LEFT:   ui_input(ui, IN_LEFT); break;
        case VK_RIGHT:  ui_input(ui, IN_RIGHT); break;
        case VK_RETURN:
        case VK_SPACE:  ui_input(ui, IN_OK); break;
        case VK_ESCAPE:                         /* no close button: Esc leaves the launcher from its first page */
            if (ui->page == PAGE_SETTINGS) ui_input(ui, IN_BACK);
            else ui->done = TRUE;
            break;
        case VK_BACK:   ui_input(ui, ui->page == PAGE_SETTINGS && ui->tab == TAB_CONTROLS ? IN_DEFAULT : IN_BACK); break;
        case VK_PRIOR:  ui_input(ui, IN_PREV_TAB); break;
        case VK_NEXT:   ui_input(ui, IN_NEXT_TAB); break;
        case VK_TAB:    ui_input(ui, GetKeyState(VK_SHIFT) < 0 ? IN_PREV_TAB : IN_NEXT_TAB); break;
        default: return DefWindowProcW(h, msg, wp, lp);
        }
        InvalidateRect(h, NULL, FALSE);
        if (ui->done) DestroyWindow(h);
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
        if (ui) {
            /* A mouse that was already over the window when it opened (or
             * when a page changed) does not pick an item until it really moves --
             * otherwise Enter could press whatever happened to be under it. */
            POINT p;
            GetCursorPos(&p);
            if (msg == WM_MOUSEMOVE && !ui->mouse_moved) {
                if (!ui->mouse_seen) { ui->mouse_at = p; ui->mouse_seen = TRUE; return 0; }
                if (abs(p.x - ui->mouse_at.x) + abs(p.y - ui->mouse_at.y) < 4) return 0;
                ui->mouse_moved = TRUE;
            }
            ui_mouse(ui, (short)LOWORD(lp), (short)HIWORD(lp), msg == WM_LBUTTONDOWN);
            if (ui->overlay_dirty) InvalidateRect(h, NULL, FALSE);
            if (ui->done) DestroyWindow(h);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (ui) {
            ui_input(ui, (short)HIWORD(wp) > 0 ? IN_UP : IN_DOWN);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_DPICHANGED:
        if (ui) {
            ui_layout(ui, HIWORD(wp), (const RECT *)lp);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_ACTIVATE:
        if (ui) { QueryPerformanceCounter(&ui->last_qpc); InvalidateRect(h, NULL, FALSE); }
        break;
    case WM_DESTROY:
        KillTimer(h, TIMER_ID);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

BOOL launcher_run(LauncherConfig *cfg)
{
    static const WCHAR *cls = L"SsxLauncher";
    WNDCLASSEXW wc;
    LUI *ui;
    MSG msg;
    HRESULT com;
    BOOL start;
    int i;

    /* For the file pickers. Balanced below, so the game later starts on a
     * thread with no COM apartment, as before. */
    com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ui = (LUI *)calloc(1, sizeof *ui);
    if (!ui) return FALSE;
    ui->cfg = cfg;
    ui->rider = (int)((GetTickCount() ^ (GetCurrentProcessId() * 2654435761u)) % ART_RIDERS);
    s_ui = ui;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ui_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hbrBackground = NULL;
    wc.style = CS_DROPSHADOW;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    ui->hwnd = CreateWindowExW(0, cls, L"SSX Tricky",
                               WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, NULL, NULL, wc.hInstance, NULL);
    if (!ui->hwnd) {
        s_ui = NULL;
        free(ui);
        if (SUCCEEDED(com)) CoUninitialize();
        return FALSE;
    }
    {
        /* Windows 11: rounded corners like other modern windows (no-op elsewhere). */
        typedef HRESULT (WINAPI *DwmSetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        DwmSetAttrFn set = dwm ? (DwmSetAttrFn)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
        DWORD round = 2;                        /* DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND */
        if (set) set(ui->hwnd, 33, &round, sizeof round);
    }
    iso_refresh(ui);                            /* reads the disc's pictures (and sounds) */
    ui_layout(ui, window_dpi(ui->hwnd), NULL);
    ui->sel = 0;
    ShowWindow(ui->hwnd, SW_SHOW);
    UpdateWindow(ui->hwnd);
    {
        /* How long the player waited for the launcher, from process start. */
        FILETIME cr, ex, kt, ut, now;
        if (GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kt, &ut)) {
            ULARGE_INTEGER a, b;
            GetSystemTimeAsFileTime(&now);
            a.LowPart = cr.dwLowDateTime; a.HighPart = cr.dwHighDateTime;
            b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
            fprintf(stderr, "Launcher: shown %.0f ms after start, %ls, %dx%d px (x%.2f)\n",
                    (double)(b.QuadPart - a.QuadPart) / 10000.0,
                    ui->art.ok ? L"with the disc's pictures" : L"no disc pictures",
                    ui->frame.w, ui->frame.h, ui->k);
        }
    }
    SetForegroundWindow(ui->hwnd);
    QueryPerformanceCounter(&ui->last_qpc);
    timeBeginPeriod(1);                         /* a steady 30 Hz timer for the conveyor */
    SetTimer(ui->hwnd, TIMER_ID, ANIM_MS, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    timeEndPeriod(1);
    snd_close(start_now(ui));
    start = ui->start;
    canvas_free(&ui->frame);
    canvas_free(&ui->overlay);
    fonts_drop(ui);
    for (i = 0; i < ART_TRACKS; i++) artimage_free(&ui->card[i]);
    artimage_free(&ui->base);
    discart_free(&ui->art);
    s_ui = NULL;
    free(ui);
    if (SUCCEEDED(com)) CoUninitialize();
    return start;
}
