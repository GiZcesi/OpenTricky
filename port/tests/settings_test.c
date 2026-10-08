/*
 * settings_test.c -- host tests of the settings registry and settings.ini
 * (port/src/settings.c). No game, no window: the table, presets, reading,
 * writing, and where the file goes.
 *
 *   gcc -std=c99 -Wall -Wextra -I port/src port/tests/settings_test.c port/src/settings.c \
 *       -lshell32 -lole32 -o settings_test.exe && ./settings_test.exe <scratch folder>
 *
 * (or cmake -DSSX_BUILD_TESTS=ON: target settings_test). The scratch folder
 * is created if missing; the test writes only inside it.
 */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { const char *a_ = (a), *b_ = (b); s_checks++; \
    if (strcmp(a_, b_)) { s_fail++; printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_, b_); } } while (0)

static Settings A, B;   /* large: static */

/* ── helpers (UTF-8 paths) ── */
#ifdef _WIN32
static void to_w(const char *s, wchar_t *w, int n) { MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); }
static void mk(const char *dir) { wchar_t w[1024]; to_w(dir, w, 1024); CreateDirectoryW(w, NULL); }
static int exists(const char *p) { wchar_t w[1024]; to_w(p, w, 1024); return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES; }
static void rm(const char *p) { wchar_t w[1024]; to_w(p, w, 1024); SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL); DeleteFileW(w); }
static void rmdir_(const char *p) { wchar_t w[1024]; to_w(p, w, 1024); RemoveDirectoryW(w); }
static void set_readonly(const char *p, int on)
{
    wchar_t w[1024];
    to_w(p, w, 1024);
    SetFileAttributesW(w, on ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL);
}
static void write_file(const char *p, const char *text)
{
    wchar_t w[1024];
    FILE *f;
    to_w(p, w, 1024);
    f = _wfopen(w, L"wb");
    if (f) { fputs(text, f); fclose(f); }
}
#else
#include <sys/stat.h>
#include <unistd.h>
static void mk(const char *dir) { mkdir(dir, 0755); }
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static void rm(const char *p) { chmod(p, 0644); remove(p); }
static void rmdir_(const char *p) { rmdir(p); }
static void set_readonly(const char *p, int on) { chmod(p, on ? 0444 : 0644); }
static void write_file(const char *p, const char *text) { FILE *f = fopen(p, "wb"); if (f) { fputs(text, f); fclose(f); } }
#endif

static void path(char *out, const char *dir, const char *name)
{
#ifdef _WIN32
    sprintf(out, "%s\\%s", dir, name);
#else
    sprintf(out, "%s/%s", dir, name);
#endif
}

/* ── the table ── */
static void test_registry(void)
{
    const SettingDef *r = settings_registry();
    int i, j, p, keys = 0, pads = 0;
    char tmp[SETTINGS_VALUE_MAX];

    for (i = 0; i < SETTINGS_COUNT; i++) {
        const SettingDef *d = &r[i];
        CHECK(d->section && d->key && d->label && d->def);
        if (!(d->section && d->key && d->label && d->def)) { printf("  entry %d incomplete\n", i); continue; }
        /* unique place in the file */
        for (j = 0; j < i; j++)
            if (r[j].section && r[j].key && !strcmp(r[j].section, d->section) && !strcmp(r[j].key, d->key)) {
                printf("  duplicate [%s] %s\n", d->section, d->key);
                CHECK(0);
            }
        CHECK(settings_find(d->section, d->key) == i);
        /* the default is valid and already normalized */
        settings_defaults(&A);
        CHECK(settings_set(&A, i, d->def));
        CHECK_STR(settings_get(&A, i), d->def);
        /* preset values are valid */
        for (p = 0; p < SETTINGS_PRESET_COUNT; p++)
            if (d->preset[p]) {
                CHECK(settings_set(&A, i, d->preset[p]));
                CHECK_STR(settings_get(&A, i), d->preset[p]);
            }
        /* a preset names every key another one names, so switching presets restores them */
        for (p = 1; p < SETTINGS_PRESET_COUNT; p++)
            CHECK((d->preset[0] != NULL) == (d->preset[p] != NULL));
        /* shown settings have help and a tab; hidden ones none */
        if (d->flags & SETTING_HIDDEN) CHECK(d->tab == SETTINGS_TAB_NONE);
        else CHECK(d->tab >= 0 && d->tab < SETTINGS_TAB_COUNT);
        if (d->type == SETTING_CHOICE) {
            CHECK(d->choices && d->nchoices > 0);
            for (j = 0; j < d->nchoices; j++) {
                int k;
                CHECK(d->choices[j].token && d->choices[j].label);
                for (k = 0; k < j; k++) CHECK(strcmp(d->choices[k].token, d->choices[j].token));
            }
        }
        /* a PC addition is turned off (or at the Xbox's value) by Original Xbox, when presets name it */
        if (d->type == SETTING_KEY) keys++;
        if (!strcmp(d->section, "Controller")) pads++;
        (void)tmp;
    }
    CHECK(keys == SETTINGS_KEY_CONTROLS);
    CHECK(pads == SETTINGS_PAD_CONTROLS);
    CHECK(!strcmp(r[S_KEY_FIRST].key, "Cross") && !strcmp(r[S_PAD_FIRST + 9].key, "Select"));
    CHECK(settings_find("display", "RESOLUTION") == S_RESOLUTION);     /* case-insensitive */
    CHECK(settings_find("Display", "Nope") == -1);
}

/* ── defaults and presets ── */
static void test_defaults_presets(void)
{
    settings_defaults(&A);
    /* the decided defaults */
    CHECK_STR(settings_get(&A, S_SMAA), "high");
    CHECK(settings_get_bool(&A, S_SOFT_SHADOWS));
    CHECK(settings_get_bool(&A, S_SAVE_BACKUP));
    CHECK(settings_get_bool(&A, S_SMOOTH_MOTION));
    CHECK(settings_def(S_SMOOTH_MOTION)->flags & SETTING_HIDDEN);
    CHECK(settings_find("Display", "AntiAliasing") < 0 && settings_find("Graphics", "MSAA") < 0);   /* MSAA removed */
    CHECK_STR(settings_get(&A, S_MENUS), "4:3");
    CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);

    settings_apply_preset(&A, SETTINGS_PRESET_STEAM_DECK);
    CHECK(settings_current_preset(&A) == SETTINGS_PRESET_STEAM_DECK);
    {
        int w = 0, h = 0;
        CHECK(settings_get_resolution(&A, S_RESOLUTION, &w, &h) && w == 1280 && h == 800);
    }
    CHECK_STR(settings_get(&A, S_DISPLAY_MODE), "fullscreen");
    CHECK_STR(settings_get(&A, S_FPS_LIMIT), "60");
    CHECK_STR(settings_get(&A, S_VSYNC), "on");
    CHECK_STR(settings_get(&A, S_SMAA), "high");
    CHECK(!settings_get_bool(&A, S_SOFT_SHADOWS));
    CHECK(!settings_get_bool(&A, S_HD_TEXTURES));
    CHECK_STR(settings_get(&A, S_DRAW_DISTANCE), "original");
    CHECK_STR(settings_get(&A, S_MENUS), "4:3");

    settings_apply_preset(&A, SETTINGS_PRESET_ORIGINAL_XBOX);
    CHECK(settings_current_preset(&A) == SETTINGS_PRESET_ORIGINAL_XBOX);
    /* Original Xbox turns off every PC addition it names */
    {
        int i;
        for (i = 0; i < SETTINGS_COUNT; i++) {
            const SettingDef *d = settings_def(i);
            if ((d->flags & SETTING_PC) && d->preset[SETTINGS_PRESET_ORIGINAL_XBOX] && d->type == SETTING_BOOL)
                CHECK(!settings_get_bool(&A, i));
        }
    }
    CHECK_STR(settings_get(&A, S_SMAA), "off");
    CHECK_STR(settings_get(&A, S_FPS_LIMIT), "60");
    CHECK_STR(settings_get(&A, S_TEX_FILTER), "off");

    settings_apply_preset(&A, SETTINGS_PRESET_RECOMMENDED);
    CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
    CHECK(settings_set(&A, S_SMAA, "ultra"));
    CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
    /* a setting no preset names leaves the preset as it is */
    settings_apply_preset(&A, SETTINGS_PRESET_RECOMMENDED);
    CHECK(settings_set(&A, S_FOV, "Full"));
    CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
    CHECK_STR(settings_preset_name(SETTINGS_PRESET_CUSTOM), "Custom");
}

/* ── checking values ── */
static void test_values(void)
{
    settings_defaults(&A);
    CHECK(settings_set(&A, S_SOFT_SHADOWS, "off") && !strcmp(settings_get(&A, S_SOFT_SHADOWS), "0"));
    CHECK(settings_set(&A, S_SOFT_SHADOWS, "TRUE") && !strcmp(settings_get(&A, S_SOFT_SHADOWS), "1"));
    CHECK(!settings_set(&A, S_SOFT_SHADOWS, "2"));
    CHECK(settings_set(&A, S_SMAA, "ULTRA") && !strcmp(settings_get(&A, S_SMAA), "ultra"));
    CHECK(!settings_set(&A, S_SMAA, "extreme") && !strcmp(settings_get(&A, S_SMAA), "ultra"));
    CHECK(settings_set(&A, S_FOV, "nostretch") && !strcmp(settings_get(&A, S_FOV), "NoStretch"));
    CHECK(settings_set(&A, S_RESOLUTION, "3440X1440") && !strcmp(settings_get(&A, S_RESOLUTION), "3440x1440"));
    CHECK(settings_set(&A, S_RESOLUTION, "AUTO") && !strcmp(settings_get(&A, S_RESOLUTION), "auto"));
    CHECK(!settings_get_resolution(&A, S_RESOLUTION, NULL, NULL));
    CHECK(!settings_set(&A, S_RESOLUTION, "100x100"));
    CHECK(!settings_set(&A, S_RESOLUTION, "1920x1080x2"));
    CHECK(settings_set(&A, S_MONITOR, "2") && settings_get_int(&A, S_MONITOR) == 2);
    CHECK(!settings_set(&A, S_MONITOR, "17") && !settings_set(&A, S_MONITOR, "1a") && !settings_set(&A, S_MONITOR, ""));
    CHECK(settings_set(&A, S_KEY_FIRST, "space") && !strcmp(settings_get(&A, S_KEY_FIRST), "Space"));
    CHECK(settings_set(&A, S_KEY_FIRST, "q") && !strcmp(settings_get(&A, S_KEY_FIRST), "Q"));
    CHECK(settings_set(&A, S_KEY_FIRST, "f12") && !strcmp(settings_get(&A, S_KEY_FIRST), "F12"));
    CHECK(!settings_set(&A, S_KEY_FIRST, "F25"));
    CHECK(settings_set(&A, S_KEY_FIRST, "key0xa0") && !strcmp(settings_get(&A, S_KEY_FIRST), "Key0xA0"));
    CHECK(settings_set(&A, S_KEY_FIRST, ";") && !strcmp(settings_get(&A, S_KEY_FIRST), ";"));
    CHECK(settings_set(&A, S_KEY_FIRST, "") && !strcmp(settings_get(&A, S_KEY_FIRST), "None"));
    CHECK(!settings_set(&A, S_KEY_FIRST, "Banana"));
    CHECK(settings_set(&A, S_PAD_FIRST, "lb") && !strcmp(settings_get(&A, S_PAD_FIRST), "LB"));
    CHECK(settings_choice_index(&A, S_PAD_FIRST) == 5);
    CHECK(!settings_set(&A, S_DISC_IMAGE, "a\nb"));
    CHECK(settings_set(&A, S_DISC_IMAGE, "") && settings_is_default(&A, S_DISC_IMAGE));
    CHECK(!settings_set(&A, -1, "1") && !settings_set(&A, SETTINGS_COUNT, "1"));
}

/* ── the file's text ── */
static size_t fmt(const Settings *s, char **out)
{
    size_t n = settings_format(s, NULL, 0);
    *out = (char *)malloc(n + 1);
    settings_format(s, *out, n + 1);
    return n;
}

static void test_text(void)
{
    char *t;
    size_t n;
    int bad, i, r;

    /* round trip with everything changed, hidden keys included */
    settings_defaults(&A);
    settings_apply_preset(&A, SETTINGS_PRESET_STEAM_DECK);
    settings_set(&A, S_DISC_IMAGE, "D:\\Jeux\\Disques\\SSX Tricky (USA) \xC3\xA9t\xC3\xA9.iso");   /* UTF-8 */
    settings_set(&A, S_SAVE_FOLDER, "sauvegardes\\vierge");
    settings_set(&A, S_KEY_FIRST + 1, "=");
    settings_set(&A, S_KEY_FIRST + 2, ";");
    settings_set(&A, S_PAD_FIRST + 3, "None");
    settings_set(&A, S_SMOOTH_MOTION, "0");
    settings_set(&A, S_FIX_GAMMA, "0");
    settings_set(&A, S_THEME, "snowdream");
    settings_set(&A, S_SKIP_LAUNCHER, "1");
    n = fmt(&A, &t);
    CHECK(strstr(t, "[Meta]\nVersion=1\n") != NULL);
    CHECK(strstr(t, "SmoothMotion=0\n") != NULL);
    CHECK(strstr(t, "FixGamma=0\n") != NULL);
    CHECK(strstr(t, "FixCullWinding=") == NULL);         /* hidden at its default: not written */
    CHECK(strstr(t, "HdTexturesMenus=") == NULL);
    CHECK(strstr(t, "Square=;\n") != NULL && strstr(t, "Circle==\n") != NULL);
    CHECK(strstr(t, "(PC)") != NULL);
    r = settings_parse(&B, t, n, &bad);
    CHECK(r == SETTINGS_LOAD_OK);
    CHECK(bad == 0);
    for (i = 0; i < SETTINGS_COUNT; i++)
        if (strcmp(settings_get(&A, i), settings_get(&B, i))) {
            printf("  round trip: [%s] %s \"%s\" != \"%s\"\n", settings_def(i)->section, settings_def(i)->key,
                   settings_get(&A, i), settings_get(&B, i));
            CHECK(0);
        }
    free(t);

    /* an old file (no [Meta] Version) is not read */
    {
        static const char old[] =
            "[Game]\nDiscImage=C:\\old.iso\n[Display]\nResolution=640x480\n[Fork]\nSMAA=off\n";
        r = settings_parse(&B, old, sizeof old - 1, &bad);
        CHECK(r == SETTINGS_LOAD_NOT_V1);
        CHECK_STR(settings_get(&B, S_DISC_IMAGE), "");
        CHECK_STR(settings_get(&B, S_RESOLUTION), "auto");
        CHECK_STR(settings_get(&B, S_SMAA), "high");
    }
    /* BOM, CRLF, blanks, case, comments, bad lines, unknown keys, Meta anywhere */
    {
        static const char odd[] =
            "\xEF\xBB\xBF; comment\r\n"
            "[ display ]\r\n"
            "  Resolution = 1920x1080  \r\n"
            "screenshape=16:9\r\n"
            "# another comment\r\n"
            "FrameRateLimit=999\r\n"           /* bad value: default */
            "Unknown=1\r\n"                    /* unknown key */
            "no equals sign\r\n"
            "[Meta]\r\n"
            "version = 1\r\n"
            "[Game]\r\n"
            "DiscImage=  C:\\a b\\c.iso\r\n";
        r = settings_parse(&B, odd, sizeof odd - 1, &bad);
        CHECK(r == SETTINGS_LOAD_OK);
        CHECK_STR(settings_get(&B, S_RESOLUTION), "1920x1080");
        CHECK_STR(settings_get(&B, S_ASPECT), "16:9");
        CHECK_STR(settings_get(&B, S_FPS_LIMIT), "monitor");
        CHECK_STR(settings_get(&B, S_DISC_IMAGE), "C:\\a b\\c.iso");
        CHECK(bad == 3);
    }
    /* a newer version: the keys known here are read */
    {
        static const char newer[] = "[Meta]\nVersion=2\n[Graphics]\nSmoothEdges=low\nNewThing=5\n";
        r = settings_parse(&B, newer, sizeof newer - 1, &bad);
        CHECK(r == SETTINGS_LOAD_NEWER);
        CHECK_STR(settings_get(&B, S_SMAA), "low");
        CHECK(bad == 1);
    }
    /* a buffer too small gets a truncated, terminated text and the full length */
    {
        char small[64];
        settings_defaults(&A);
        n = settings_format(&A, small, sizeof small);
        CHECK(n > sizeof small && strlen(small) == sizeof small - 1);
    }
}

/* ── files and where they go ── */
static void test_files(const char *root)
{
    char game[600], docs[600], ro[600], f[700], g[700], d[700], out[1024], tmp[720];
    int bad, r;

    path(game, root, "game \xC3\xA9");     /* a folder name outside ASCII */
    path(docs, root, "docs");
    path(ro, root, "missing-game-folder");
    mk(root);
    mk(game);
    path(g, game, "settings.ini");
    path(d, docs, "settings.ini");
    rm(g);
    rm(d);

    /* read, nothing there: the game folder's (to be) */
    r = settings_locate(game, docs, 0, out, sizeof out);
    CHECK(r == SETTINGS_AT_GAME_FOLDER);
    CHECK_STR(out, g);
    CHECK(settings_load(&B, out, &bad) == SETTINGS_LOAD_MISSING);

    /* write: the game folder can be written */
    r = settings_locate(game, docs, 1, out, sizeof out);
    CHECK(r == SETTINGS_AT_GAME_FOLDER);
    settings_defaults(&A);
    settings_set(&A, S_SMAA, "medium");
    settings_set(&A, S_DISC_IMAGE, "E:\\\xE6\x97\xA5\xE6\x9C\xAC\\ssx.iso");
    CHECK(settings_save(&A, out));
    CHECK(exists(g));
    sprintf(tmp, "%s.new", g);
    CHECK(!exists(tmp));
    CHECK(settings_load(&B, g, &bad) == SETTINGS_LOAD_OK && bad == 0);
    CHECK_STR(settings_get(&B, S_SMAA), "medium");
    CHECK_STR(settings_get(&B, S_DISC_IMAGE), "E:\\\xE6\x97\xA5\xE6\x9C\xAC\\ssx.iso");
    /* saving again replaces the file */
    settings_set(&A, S_SMAA, "low");
    CHECK(settings_save(&A, g));
    CHECK(settings_load(&B, g, &bad) == SETTINGS_LOAD_OK);
    CHECK_STR(settings_get(&B, S_SMAA), "low");

    /* the game folder's file cannot be written (read-only): Documents, created */
    set_readonly(g, 1);
    r = settings_locate(game, docs, 1, out, sizeof out);
    CHECK(r == SETTINGS_AT_DOCUMENTS);
    CHECK_STR(out, d);
    CHECK(settings_save(&A, out));
    CHECK(exists(d));
    /* reading prefers the game folder's file when there is one */
    r = settings_locate(game, docs, 0, out, sizeof out);
    CHECK(r == SETTINGS_AT_GAME_FOLDER);
    set_readonly(g, 0);
    rm(g);
    /* ... else the one in Documents */
    r = settings_locate(game, docs, 0, out, sizeof out);
    CHECK(r == SETTINGS_AT_DOCUMENTS);
    CHECK_STR(out, d);

    /* a game folder that cannot be written at all (here: missing) */
    r = settings_locate(ro, docs, 1, out, sizeof out);
    CHECK(r == SETTINGS_AT_DOCUMENTS);

    /* an old .ini in the file's place: not read */
    write_file(g, "[Game]\r\nDiscImage=C:\\x.iso\r\n[Fork]\r\nSMAA=ultra\r\n");
    CHECK(settings_load(&B, g, &bad) == SETTINGS_LOAD_NOT_V1);
    CHECK_STR(settings_get(&B, S_SMAA), "high");

    rm(g);
    rm(d);
    rmdir_(docs);
    rmdir_(game);
    (void)f;
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "settings_test_tmp";
    test_registry();
    test_defaults_presets();
    test_values();
    test_text();
    test_files(root);
    printf("%s: %d checks, %d failed\n", s_fail ? "FAIL" : "PASS", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
