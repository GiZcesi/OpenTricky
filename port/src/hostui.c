/*
 * hostui.c -- the game window's own keys.
 *
 *   F12        screenshot (Screenshots\ beside the game)
 *   F11        fullscreen on/off (Alt+Enter too, handled by the runtime)
 *   Alt+F4     exit (Windows)
 *
 * The window has no menu bar any more, like a game of its time;
 * what the menu offered lives in the launcher's Settings (window size = the
 * resolution, textures, anti-aliasing, frame rate in the title, controls,
 * the screenshots / saves / log folders, about). The window itself is the
 * runtime's (xboxrecomp d3d8_device.c); these hooks run on its thread.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "launcher.h"
#include "controls.h"
#include "hostui.h"

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
void d3d8_RequestScreenshot(const wchar_t *path);

static void exe_dir_w(WCHAR *out, size_t n)
{
    DWORD k = GetModuleFileNameW(NULL, out, (DWORD)n);
    WCHAR *slash;
    if (k == 0 || k >= n) { out[0] = 0; return; }
    slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
}

static void take_screenshot(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    SYSTEMTIME t;
    exe_dir_w(dir, MAX_PATH);
    wcsncat(dir, L"\\Screenshots", MAX_PATH - wcslen(dir) - 1);
    CreateDirectoryW(dir, NULL);
    GetLocalTime(&t);
    swprintf(path, MAX_PATH, L"%ls\\SSX Tricky %04u-%02u-%02u %02u-%02u-%02u.png", dir,
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    d3d8_RequestScreenshot(path);
}

static int on_key(HWND h, UINT vk)
{
    (void)h;
    if (vk == VK_F12) { take_screenshot(); return 1; }
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
}
