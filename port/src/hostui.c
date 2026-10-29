/*
 * hostui.c -- the game window's own keys.
 *
 *   F12        screenshot of the image shown, into Screenshots\ in the install
 *              root (beside OpenTricky.exe), with a .txt of the same name
 *   Shift+F12  burst: the next N images shown, one per present (XBOX_SHOT_BURST,
 *              60 by default), into a folder of their own, with context.txt
 *   F11        fullscreen on/off (Alt+Enter too, handled by the runtime)
 *   Alt+F4     exit (Windows)
 *
 * The window has no menu bar any more, like a game of its time;
 * what the menu offered lives in the launcher's Settings (window size = the
 * resolution, textures, anti-aliasing, frame rate in the title, controls,
 * the screenshots / saves / log folders, about). The window itself is the
 * runtime's (xboxrecomp d3d8_device.c); these hooks run on its thread.
 *
 * A screenshot is the image the window shows, at the window's
 * size, exactly as presented (d3d8_RequestShownShot: the back buffer after
 * the scaling and the palette), written in the
 * background so the game does not stutter. The .txt says when and where
 * (track, race tick and state, version, the settings in effect: the XBOX_*
 * variables and settings.ini, without the player's paths) so the moment can
 * be found again; the runtime
 * adds the image's own lines (sizes, present times, one line per image).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "miniz.h"
#include "recomp/recomp_types.h"
#include "launcher.h"
#include "controls.h"
#include "version.h"
#include "hostui.h"
#include "crashreport.h"

/* Runtime (xboxrecomp/src/d3d/d3d8_xbox.h), declared here to keep this file
 * free of the runtime's Xbox-flavoured headers. */
typedef struct D3D8HostUiHooks {
    HMENU (*create_menu)(void);
    void  (*on_command)(HWND hwnd, UINT id);
    void  (*on_init_menu)(HMENU menu);
    int   (*on_key)(HWND hwnd, UINT vk);
    void  (*on_menu_loop)(int entering);
} D3D8HostUiHooks;
void d3d8_SetHostUiHooks(const D3D8HostUiHooks *hooks);
void d3d8_HostSetFullscreen(int on);
int  d3d8_HostIsFullscreen(void);
void d3d8_HostToast(const wchar_t *text);
int  d3d8_RequestShownShot(const wchar_t *path, unsigned n, const wchar_t *notes);
int  d3d8_ShownShotBusy(void);
unsigned d3d8_ShotBurstCount(void);
void d3d8_SetShotInfoHook(void (*fn)(char *buf, size_t n));
void d3d8_SetPngEncoder(void *(*fn)(const unsigned char *rgb, unsigned w, unsigned h, size_t *len));

#define APP_GLOBAL  0x001E3C7Cu
#define TRACK_ID    0x001DEC90u     /* the race's track */

/* The race object (app -> level -> race), or 0 in the menus. */
static uint32_t race_obj(void)
{
    uint32_t app = MEM32(APP_GLOBAL), lvl, race;
    if (app < 0x1000u) return 0;
    lvl = MEM32(app + 0x72Cu);
    if (lvl < 0x1000u) return 0;
    race = MEM32(lvl + 0x1Cu);
    return race < 0x1000u ? 0 : race;
}

/* Render thread, once per captured image: its line in the description. */
static void shot_info(char *buf, size_t n)
{
    uint32_t race = race_obj();
    if (race) snprintf(buf, n, "race tick %u, state %u", MEM32(race + 0x18u), MEM32(race + 0x1Cu));
    else snprintf(buf, n, "no race");
}

/* Worker threads: the PNG of one image (miniz, deflate level XBOX_SHOT_LEVEL,
 * 1 by default: about a third of the raw size, ~120 ms at 2000x1126 on one
 * thread; lossless at every level). */
static void *shot_png(const unsigned char *rgb, unsigned w, unsigned h, size_t *len)
{
    static int level = -1;
    if (level < 0) {
        const char *e = getenv("XBOX_SHOT_LEVEL");
        int v = e && e[0] ? atoi(e) : 1;
        level = v < 0 ? 0 : v > 9 ? 9 : v;
    }
    *len = 0;
    return tdefl_write_image_to_png_file_in_memory_ex(rgb, (int)w, (int)h, 3, len, (mz_uint)level, MZ_FALSE);
}

static int wcmp(const void *a, const void *b)
{
    return wcscmp(*(const WCHAR *const *)a, *(const WCHAR *const *)b);
}

/* The track's name in the .txt ("snowdream"), or its number. */
static const char *track_name(uint32_t track)
{
    static const char *const names[12] = {
        "garibaldi", "snowdream", "elysium", "mesablanca", "merqury", "aloha",
        NULL, NULL, "tokyo", NULL, NULL, "alaska"
    };
    static char buf[16];
    if (track < 12u && names[track]) return names[track];
    snprintf(buf, sizeof buf, "track%u", (unsigned)track);
    return buf;
}

/* An XBOX_* variable worth a line in the .txt: one that changes the game
 * (the launcher's settings, the fixes' switches, a replayed run), not one
 * that only says where files go, what is logged or how the tools talk. */
static int env_in_report(const WCHAR *v)
{
    static const WCHAR *const part[] = {        /* in the name */
        L"_DIR", L"_INI", L"LOG", L"_CSV", L"_WAV", L"DUMP", L"_FILE", L"_PATH",
    };
    static const WCHAR *const start[] = {
        L"XBOX_CRASH_", L"XBOX_HANG_", L"XBOX_DIAG_", L"XBOX_NO_ACTIVATE", L"XBOX_SHOT_",
    };
    WCHAR name[96];
    const WCHAR *eq = wcschr(v, L'=');
    size_t n = eq ? (size_t)(eq - v) : wcslen(v), i;
    if (n == 0 || n >= sizeof name / sizeof name[0]) return 0;
    wmemcpy(name, v, n);
    name[n] = 0;
    for (i = 0; i < sizeof part / sizeof part[0]; i++)
        if (wcsstr(name, part[i])) return 0;
    for (i = 0; i < sizeof start / sizeof start[0]; i++)
        if (!wcsncmp(name, start[i], wcslen(start[i]))) return 0;
    return 1;
}

/* One XBOX_* variable, without any path of the player's: an absolute path
 * says what it is (crashreport_private_text, as in the crash reports), any
 * other value with a folder in it is "<path>". */
static void put_env(FILE *f, const WCHAR *w)
{
    char u8[2048], out[2048];
    const char *eq;
    int k = WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, (int)sizeof u8, NULL, NULL);
    if (k <= 1) return;
    eq = strchr(u8, '=');
    if (eq && (strchr(eq, '\\') || strchr(eq, '/'))
        && !(((eq[1] | 32) >= 'a' && (eq[1] | 32) <= 'z' && eq[2] == ':') || (eq[1] == eq[2] && (eq[1] == '\\' || eq[1] == '/')))) {
        fprintf(f, "%.*s=<path>\n", (int)(eq - u8), u8);
        return;
    }
    if (crashreport_private_text(u8, (size_t)(k - 1), 0, out, sizeof out)) fprintf(f, "%s\n", out);
}

/* The game's context, first in the .txt (the runtime appends the image's).
 * Nothing that names the player: no path of the exe, the disc image or the
 * folders, no user name (the masking of the crash reports). */
static void write_context(const WCHAR *txt, unsigned n, const SYSTEMTIME *t)
{
    FILE *f = _wfopen(txt, L"wb");
    WCHAR exe[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    char ini[MAX_PATH];
    const char *fork_ini = getenv("XBOX_FORK_INI");
    uint32_t race = race_obj();
    if (!f) return;

    fprintf(f, "OpenTricky screenshot: what was on screen, and how to find the moment again\n\n[context]\n");
    fprintf(f, "time = %04u-%02u-%02u %02u:%02u:%02u.%03u\n", t->wYear, t->wMonth, t->wDay,
            t->wHour, t->wMinute, t->wSecond, t->wMilliseconds);
    if (n > 1) fprintf(f, "kind = burst: %u consecutive images shown (000.png is the first)\n", n);
    else fprintf(f, "kind = one image\n");
    fprintf(f, "version = %s %s, built %s %s\n", OT_PRODUCT_NAME, OT_VERSION, __DATE__, __TIME__);
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) && GetFileAttributesExW(exe, GetFileExInfoStandard, &fa)) {
        FILETIME lt;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
        fprintf(f, "exe = %lu bytes, modified %04u-%02u-%02u %02u:%02u:%02u\n", (unsigned long)fa.nFileSizeLow,
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
    if (race) {
        uint32_t tr = MEM32(TRACK_ID);
        fprintf(f, "track = %s (%u)\n", track_name(tr), tr);
        fprintf(f, "race tick = %u (race+0x18, 60 per second), race state = %u (race+0x1C; 1 intro, "
                   "3 countdown, 4 race, 5 end)\n", MEM32(race + 0x18u), MEM32(race + 0x1Cu));
    } else {
        fprintf(f, "track = none (menus, loading or a video)\n");
    }
    {   /* time since the game started */
        FILETIME c, x, k, u, now;
        if (GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u)) {
            ULARGE_INTEGER a, b;
            GetSystemTimeAsFileTime(&now);
            a.LowPart = c.dwLowDateTime; a.HighPart = c.dwHighDateTime;
            b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
            fprintf(f, "game running for = %.1f s\n", (double)(b.QuadPart - a.QuadPart) / 1e7);
        }
    }

    {   /* the settings in effect: the XBOX_* variables (the launcher sets them from settings.ini) */
        WCHAR *env = GetEnvironmentStringsW(), *p;
        const WCHAR *list[512];
        int k = 0, i;
        for (p = env; p && *p && k < 512; p += wcslen(p) + 1)
            if (!wcsncmp(p, L"XBOX_", 5) && env_in_report(p)) list[k++] = p;
        qsort(list, (size_t)k, sizeof list[0], wcmp);
        fprintf(f, "\n[settings in effect: XBOX_* variables that change the game]\n");
        for (i = 0; i < k; i++) put_env(f, list[i]);
        if (env) FreeEnvironmentStringsW(env);
    }
    if (fork_ini && fork_ini[0]) snprintf(ini, sizeof ini, "%s", fork_ini);
    else launcher_config_path(ini, sizeof ini);
    {   /* settings.ini, its paths hidden (crashreport_private_text) */
        static char text[24 * 1024], clean[32 * 1024];
        WCHAR wini[MAX_PATH];
        FILE *s = NULL;
        size_t len = 0;
        if (MultiByteToWideChar(CP_UTF8, 0, ini, -1, wini, MAX_PATH) > 0) s = _wfopen(wini, L"rb");
        if (s) {
            len = fread(text, 1, sizeof text - 1, s);
            fclose(s);
        }
        fprintf(f, "\n[settings.ini%s%s]\n", fork_ini && fork_ini[0] ? " (XBOX_FORK_INI)" : "",
                s ? "" : ": not found, the code's defaults");
        if (len) {
            size_t i, o = 0, c = crashreport_private_text(text, len, 0, clean, sizeof clean);
            for (i = 0; i < c; i++)                 /* written in binary mode: one '\n' per line */
                if (clean[i] != '\r') clean[o++] = clean[i];
            fwrite(clean, 1, o, f);
            if (o && clean[o - 1] != '\n') fputc('\n', f);
        }
    }
    fclose(f);
}

/* F12 (n = 1) and Shift+F12 (n = the burst count). 0 = nothing asked. */
static int take_screenshot(unsigned n)
{
    WCHAR dir[MAX_PATH], stamp[64], name[96], path[MAX_PATH], txt[MAX_PATH];
    SYSTEMTIME t;
    int k, made = 0;

    if (d3d8_ShownShotBusy()) {
        d3d8_HostToast(L"Screenshot: the last burst is still being saved");
        return 0;
    }
    launcher_install_root_w(dir, MAX_PATH);
    wcsncat(dir, L"\\Screenshots", MAX_PATH - wcslen(dir) - 1);
    CreateDirectoryW(dir, NULL);
    GetLocalTime(&t);
    swprintf(stamp, 64, L"SSX Tricky %04u-%02u-%02u %02u-%02u-%02u",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    for (k = 1; k < 100 && !made; k++) {
        if (k == 1) lstrcpynW(name, stamp, 96);
        else swprintf(name, 96, L"%ls (%d)", stamp, k);
        if (n > 1) {
            swprintf(path, MAX_PATH, L"%ls\\%ls burst", dir, name);
            if (CreateDirectoryW(path, NULL)) {
                swprintf(txt, MAX_PATH, L"%ls\\context.txt", path);
                made = 1;
            }
        } else {
            swprintf(path, MAX_PATH, L"%ls\\%ls.png", dir, name);
            swprintf(txt, MAX_PATH, L"%ls\\%ls.txt", dir, name);
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(txt) == INVALID_FILE_ATTRIBUTES)
                made = 1;
        }
    }
    if (!made) {
        d3d8_HostToast(L"Screenshot failed");
        return 0;
    }
    write_context(txt, n, &t);
    if (!d3d8_RequestShownShot(path, n, txt)) {
        DeleteFileW(txt);
        if (n > 1) RemoveDirectoryW(path);
        d3d8_HostToast(L"Screenshot: the last burst is still being saved");
        return 0;
    }
    return 1;
}


static int on_key(HWND h, UINT vk)
{
    (void)h;
    if (vk == VK_F12) {
        take_screenshot(GetKeyState(VK_SHIFT) < 0 ? d3d8_ShotBurstCount() : 1);
        return 1;
    }
    if (vk == VK_F11) { d3d8_HostSetFullscreen(!d3d8_HostIsFullscreen()); return 1; }
    return 0;
}

/* While the window menu (Alt+Space) is open the arrow keys belong to it. */
static void on_menu_loop(int entering)
{
    controls_set_suspended(entering ? TRUE : FALSE);
}

void hostui_install(LauncherConfig *cfg, BOOL persist_changes)
{
    static const D3D8HostUiHooks hooks = { NULL, NULL, NULL, on_key, on_menu_loop };
    (void)cfg;
    (void)persist_changes;
    d3d8_SetHostUiHooks(&hooks);
    d3d8_SetShotInfoHook(shot_info);
    d3d8_SetPngEncoder(shot_png);
}
