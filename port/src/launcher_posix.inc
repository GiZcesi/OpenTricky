/* launcher_posix.inc -- the path and folder helpers of launcher.c for hosts with
 * UTF-8 paths and '/' separators (macOS). Included in place of the Windows
 * versions, which go through wide strings, 8.3 names and the shell. Behaviour
 * that only matters to the Windows launcher window (DPI, the file manager, saves
 * left by 0.1.1) is a no-op here.
 *
 * Not checked on a real install yet: where the install root and saves land on
 * macOS is decided here and in settings.c (settings_game_dir / settings_docs_dir). */
#include <sys/stat.h>
#include <dirent.h>

static BOOL u8_to_path(const char *u8, char *out, size_t n)
{
    if (snprintf(out, n, "%s", u8) >= (int)n) { out[0] = '\0'; return FALSE; }
    return TRUE;
}

static void path_to_u8(const char *ansi, char *out, size_t n)
{
    snprintf(out, n, "%s", ansi);
}

static void install_dir(char *out, size_t n)
{
    out[0] = '\0';
    settings_install_root(out, n);
}

BOOL launcher_install_root_w(WCHAR *out, size_t n)
{
    char u8[SETTINGS_VALUE_MAX];
    size_t k;
    if (!settings_install_root(u8, sizeof u8) || !MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, (int)n)) {
        if (n) out[0] = 0;
        return FALSE;
    }
    for (k = 0; out[k]; k++) {}
    if (k > 1 && out[k - 1] == L'/') out[k - 1] = 0;
    return TRUE;
}

static BOOL is_absolute(const char *p) { return p[0] == '/'; }

void launcher_resolve_path(const char *path, char *out, size_t out_sz)
{
    char base[MAX_PATH];
    if (!path[0] || is_absolute(path)) { snprintf(out, out_sz, "%s", path); return; }
    install_dir(base, sizeof base);
    if (base[0]) snprintf(out, out_sz, "%s/%s", base, path);
    else snprintf(out, out_sz, "%s", path);
}

static BOOL dir_has_files(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    BOOL any = FALSE;
    if (!d) return FALSE;
    while (!any && (e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) any = TRUE;
    closedir(d);
    return any;
}

/* The per-user folder for saves when none is chosen: ~/Library/Application Support/OpenTricky/Saves. */
static BOOL support_saves(char *out, size_t n)
{
    const char *home = getenv("HOME");
    char parent[MAX_PATH * 2];
    if (!home || !home[0]) return FALSE;
    snprintf(parent, sizeof parent, "%s/Library/Application Support/OpenTricky", home);
    mkdir(parent, 0777);
    return snprintf(out, n, "%s/Saves", parent) < (int)n;
}

int launcher_save_kind(const LauncherConfig *cfg)
{
    char base[MAX_PATH], p[MAX_PATH * 2], d[MAX_PATH * 2];
    if (cfg->hdd[0]) return LAUNCHER_SAVES_CHOSEN;
    install_dir(base, sizeof base);
    snprintf(p, sizeof p, "%s/hdd", base);
    if (dir_has_files(p)) return LAUNCHER_SAVES_OLD_HDD;
    snprintf(p, sizeof p, "%s/portable.txt", base);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return LAUNCHER_SAVES_PORTABLE;
    if (support_saves(d, sizeof d)) return LAUNCHER_SAVES_DOCUMENTS;
    return LAUNCHER_SAVES_PORTABLE;
}

void launcher_hdd_path(const LauncherConfig *cfg, char *out, size_t out_sz)
{
    int kind = launcher_save_kind(cfg);
    out[0] = '\0';
    if (kind == LAUNCHER_SAVES_DOCUMENTS && support_saves(out, out_sz)) return;
    if (kind == LAUNCHER_SAVES_CHOSEN) launcher_resolve_path(cfg->hdd, out, out_sz);
    else launcher_resolve_path(kind == LAUNCHER_SAVES_OLD_HDD ? "hdd" : "Saves", out, out_sz);
}

void launcher_migrate_saves(const char *save_u8) { (void)save_u8; }   /* the 0.1.1 folder bug is Windows-only */

void launcher_open_path(HWND owner, const char *path, BOOL folder)
{
    (void)owner;
    if (folder) mkdir(path, 0777);
    fprintf(stderr, "[launcher] open: %s (not wired on this host)\n", path);
}

void launcher_enable_dpi_awareness(void) {}
