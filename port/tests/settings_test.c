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
    /* the soft edge's width: 100 % (the edge as first made) by default and in every preset */
    CHECK_STR(settings_get(&A, S_SHADOW_SOFTNESS), "100");
    CHECK(settings_find("Graphics", "ShadowSoftness") == S_SHADOW_SOFTNESS);
    CHECK(settings_def(S_SHADOW_SOFTNESS)->tab == SETTINGS_TAB_GRAPHICS);
    CHECK(settings_def(S_SHADOW_SOFTNESS)->flags & SETTING_PC);
    CHECK_STR(settings_def(S_SHADOW_SOFTNESS)->env, "XBOX_SOFT_SHADOW_WIDTH");
    {
        int p;
        for (p = 0; p < SETTINGS_PRESET_COUNT; p++) CHECK_STR(settings_def(S_SHADOW_SOFTNESS)->preset[p], "100");
    }
    CHECK(settings_get_bool(&A, S_SAVE_BACKUP));
    CHECK(settings_get_bool(&A, S_SMOOTH_MOTION));
    CHECK(settings_def(S_SMOOTH_MOTION)->flags & SETTING_HIDDEN);
    /* the leg smoothing: shown, PC, on by default and in Recommended, off in Original Xbox and
     * Steam Deck (Advanced) */
    CHECK(settings_get_bool(&A, S_SMOOTH_LEGS));
    CHECK_STR(settings_get(&A, S_SPEAKERS), "stereo");
    CHECK(!(settings_def(S_SMOOTH_LEGS)->flags & SETTING_HIDDEN));
    CHECK(settings_def(S_SMOOTH_LEGS)->flags & SETTING_PC);
    CHECK(settings_def(S_SMOOTH_LEGS)->tab == SETTINGS_TAB_ADVANCED);
    {
        int p;
        for (p = 0; p < SETTINGS_PRESET_COUNT; p++)
            CHECK_STR(settings_def(S_SMOOTH_LEGS)->preset[p],
                      (p == SETTINGS_PRESET_ORIGINAL_XBOX || p == SETTINGS_PRESET_STEAM_DECK) ? "0" : "1");
        CHECK_STR(settings_def(S_SMOOTH_LEGS)->def, "1");
    }
    /* Improvements of the original picture, each a line: Smoother terrain seams (ADVANCED >
     * SMOOTHING, with Smoother legs in turns) and Lightmap smoothing (GRAPHICS > WORLD): PC, on by
     * default (absent key = on), off in Original Xbox and in Steam Deck (presets revision 4) */
    {
        static const int ids[2] = { S_SMOOTH_SEAMS, S_LIGHTMAP_SMOOTH };
        int p, k;
        CHECK(settings_find("Advanced", "SmoothTerrainSeams") == S_SMOOTH_SEAMS);
        CHECK(settings_find("Graphics", "LightmapSmoothing") == S_LIGHTMAP_SMOOTH);
        CHECK_STR(settings_def(S_LIGHTMAP_SMOOTH)->env, "XBOX_LIGHTMAP_FILTER");
        CHECK(!(settings_def(S_SMOOTH_SEAMS)->flags & SETTING_HIDDEN));
        CHECK(settings_def(S_SMOOTH_SEAMS)->tab == SETTINGS_TAB_ADVANCED);
        CHECK_STR(settings_def(S_SMOOTH_SEAMS)->group, "SMOOTHING");
        CHECK_STR(settings_def(S_SMOOTH_LEGS)->group, "SMOOTHING");
        CHECK(!(settings_def(S_LIGHTMAP_SMOOTH)->flags & SETTING_HIDDEN));
        CHECK(settings_def(S_LIGHTMAP_SMOOTH)->tab == SETTINGS_TAB_GRAPHICS);
        CHECK_STR(settings_def(S_LIGHTMAP_SMOOTH)->group, "WORLD");
        /* Texture filtering: 16x by default, in Recommended and Steam Deck; Off in Original Xbox */
        CHECK_STR(settings_get(&A, S_TEX_FILTER), "16");
        CHECK_STR(settings_def(S_TEX_FILTER)->preset[SETTINGS_PRESET_ORIGINAL_XBOX], "off");
        CHECK_STR(settings_def(S_TEX_FILTER)->preset[SETTINGS_PRESET_RECOMMENDED], "16");
        CHECK_STR(settings_def(S_TEX_FILTER)->preset[SETTINGS_PRESET_STEAM_DECK], "16");
        for (k = 0; k < 2; k++) {
            const SettingDef *d = settings_def(ids[k]);
            CHECK(settings_get_bool(&A, ids[k]));
            CHECK_STR(d->def, "1");
            CHECK(!(d->flags & SETTING_PENDING));
            CHECK(d->flags & SETTING_PC);
            for (p = 0; p < SETTINGS_PRESET_COUNT; p++)
                CHECK_STR(d->preset[p], (p == SETTINGS_PRESET_ORIGINAL_XBOX || p == SETTINGS_PRESET_STEAM_DECK) ? "0" : "1");
        }
        /* Look like the Xbox: no line, in no preset (the game keeps its fixes on whatever the file says) */
        CHECK(settings_find("Graphics", "LookLikeXbox") == S_LOOK_LIKE_XBOX);
        CHECK(settings_def(S_LOOK_LIKE_XBOX)->flags & SETTING_HIDDEN);
        CHECK(settings_def(S_LOOK_LIKE_XBOX)->tab == SETTINGS_TAB_NONE);
        for (p = 0; p < SETTINGS_PRESET_COUNT; p++) CHECK(settings_def(S_LOOK_LIKE_XBOX)->preset[p] == NULL);
        CHECK(settings_get_bool(&A, S_LOOK_LIKE_XBOX));
    }
    /* Weather: GRAPHICS > WORLD right below Snow look, PC, Original / PS2, PS2 by default and in
     * Recommended and Steam Deck, Original in Original Xbox (XBOX_WEATHER) */
    {
        const SettingDef *d = settings_def(S_WEATHER);
        CHECK(S_WEATHER == S_SNOW_LOOK + 1);
        CHECK(settings_find("Graphics", "Weather") == S_WEATHER);
        CHECK_STR(d->label, "Weather");
        CHECK_STR(d->env, "XBOX_WEATHER");
        CHECK(d->tab == SETTINGS_TAB_GRAPHICS);
        CHECK_STR(d->group, "WORLD");
        CHECK(d->type == SETTING_CHOICE && d->nchoices == 2);
        CHECK_STR(d->choices[0].token, "original");
        CHECK_STR(d->choices[0].label, "Original");
        CHECK_STR(d->choices[1].token, "ps2");
        CHECK_STR(d->choices[1].label, "PS2");
        CHECK(d->flags & SETTING_PC);
        CHECK(!(d->flags & (SETTING_HIDDEN | SETTING_PENDING | SETTING_LAUNCHER)));
        CHECK_STR(d->def, "ps2");
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        CHECK_STR(d->preset[SETTINGS_PRESET_ORIGINAL_XBOX], "original");
        CHECK_STR(d->preset[SETTINGS_PRESET_RECOMMENDED], "ps2");
        CHECK_STR(d->preset[SETTINGS_PRESET_STEAM_DECK], "ps2");
        CHECK(settings_set(&A, S_WEATHER, "PS2") && !strcmp(settings_get(&A, S_WEATHER), "ps2"));
        CHECK(!settings_set(&A, S_WEATHER, "xbox"));      /* not one of the choices */
        CHECK(settings_set(&A, S_WEATHER, "original") && settings_choice_index(&A, S_WEATHER) == 0);
        settings_defaults(&A);
    }
    /* The Time Challenge ghost: a hidden row, off in every preset (the code kept) */
    {
        int p;
        CHECK(settings_find("Game", "TimeChallengeGhost") == S_TC_GHOST);
        CHECK(settings_def(S_TC_GHOST)->flags & SETTING_HIDDEN);
        CHECK(settings_def(S_TC_GHOST)->tab == SETTINGS_TAB_NONE);
        CHECK(!settings_get_bool(&A, S_TC_GHOST));
        for (p = 0; p < SETTINGS_PRESET_COUNT; p++) CHECK_STR(settings_def(S_TC_GHOST)->preset[p], "0");
    }
    CHECK(settings_find("Display", "AntiAliasing") < 0 && settings_find("Graphics", "MSAA") < 0);   /* MSAA removed */
    CHECK_STR(settings_get(&A, S_MENUS), "4:3");
    /* Save profile: My save by default, shown in GAME, the player's choice (no preset) */
    CHECK_STR(settings_get(&A, S_SAVE_PROFILE), "mine");
    CHECK(settings_find("Game", "SaveProfile") == S_SAVE_PROFILE);
    CHECK(settings_def(S_SAVE_PROFILE)->tab == SETTINGS_TAB_GAME);
    CHECK(!(settings_def(S_SAVE_PROFILE)->flags & (SETTING_HIDDEN | SETTING_PC | SETTING_PENDING)));
    CHECK_STR(settings_def(S_SAVE_PROFILE)->env, "XBOX_UNLOCK_ALL");
    {
        int p;
        for (p = 0; p < SETTINGS_PRESET_COUNT; p++) CHECK(settings_def(S_SAVE_PROFILE)->preset[p] == NULL);
        settings_set(&A, S_SAVE_PROFILE, "unlockall");
        CHECK(settings_choice_index(&A, S_SAVE_PROFILE) == 1);
        for (p = 0; p < SETTINGS_PRESET_COUNT; p++) {
            settings_apply_preset(&A, p);                 /* a preset never changes the profile */
            CHECK_STR(settings_get(&A, S_SAVE_PROFILE), "unlockall");
        }
        CHECK(!settings_set(&A, S_SAVE_PROFILE, "everything"));   /* not one of the choices */
        CHECK_STR(settings_get(&A, S_SAVE_PROFILE), "unlockall");
        settings_defaults(&A);
    }
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
    CHECK_STR(settings_get(&A, S_HD_TEXTURES), "off");
    CHECK_STR(settings_get(&A, S_DRAW_DISTANCE), "original");
    CHECK_STR(settings_get(&A, S_SNOW_LOOK), "ps2");
    CHECK_STR(settings_get(&A, S_WEATHER), "ps2");        /* no snowfall fog */
    CHECK_STR(settings_get(&A, S_RENDER_SCALE), "100");   /* the Deck at 100 % too */
    CHECK_STR(settings_get(&A, S_MENUS), "4:3");
    /* the lighter Steam Deck (presets revision 4): no smoother seams nor lightmap smoothing;
     * 16x, HD HUD and SMAA high as before */
    CHECK(!settings_get_bool(&A, S_SMOOTH_SEAMS) && !settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
    CHECK_STR(settings_get(&A, S_TEX_FILTER), "16");
    CHECK(settings_get_bool(&A, S_HD_HUD));

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
    CHECK_STR(settings_get(&A, S_SNOW_LOOK), "original");  /* the title's snow shader */
    CHECK_STR(settings_get(&A, S_WEATHER), "original");    /* the title's snowfall fog */
    CHECK_STR(settings_get(&A, S_RACE_HUD), "xbox");      /* the console's stretched HUD */
    CHECK_STR(settings_get(&A, S_RENDER_SCALE), "100");   /* render scale of the 3D: off */
    CHECK_STR(settings_get(&A, S_HD_TEXTURES), "off");

    settings_apply_preset(&A, SETTINGS_PRESET_RECOMMENDED);
    CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
    CHECK_STR(settings_get(&A, S_RACE_HUD), "85");        /* back from Original Xbox */
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
    /* shadow softness: the steps only, 60 = the "40 % less" edge */
    CHECK(settings_set(&A, S_SHADOW_SOFTNESS, "40") && settings_set(&A, S_SHADOW_SOFTNESS, "60") &&
          settings_set(&A, S_SHADOW_SOFTNESS, "150"));
    CHECK(!settings_set(&A, S_SHADOW_SOFTNESS, "70") && !settings_set(&A, S_SHADOW_SOFTNESS, "1"));
    CHECK_STR(settings_get(&A, S_SHADOW_SOFTNESS), "150");
    CHECK(settings_set(&A, S_SHADOW_SOFTNESS, "100"));
    CHECK(settings_set(&A, S_SMAA, "ULTRA") && !strcmp(settings_get(&A, S_SMAA), "ultra"));
    CHECK(!settings_set(&A, S_SMAA, "extreme") && !strcmp(settings_get(&A, S_SMAA), "ultra"));
    CHECK(settings_set(&A, S_FOV, "nostretch") && !strcmp(settings_get(&A, S_FOV), "NoStretch"));
    CHECK(settings_set(&A, S_RESOLUTION, "3440X1440") && !strcmp(settings_get(&A, S_RESOLUTION), "3440x1440"));
    CHECK(settings_set(&A, S_RESOLUTION, "AUTO") && !strcmp(settings_get(&A, S_RESOLUTION), "auto"));
    CHECK(!settings_get_resolution(&A, S_RESOLUTION, NULL, NULL));
    CHECK(!settings_set(&A, S_RESOLUTION, "100x100"));
    CHECK(!settings_set(&A, S_RESOLUTION, "1920x1080x2"));
    /* render scale: the steps only */
    CHECK(settings_set(&A, S_RENDER_SCALE, "50") && settings_set(&A, S_RENDER_SCALE, "67") &&
          settings_set(&A, S_RENDER_SCALE, "175") && settings_set(&A, S_RENDER_SCALE, "200"));
    CHECK(!settings_set(&A, S_RENDER_SCALE, "60") && !settings_set(&A, S_RENDER_SCALE, "250"));
    CHECK(settings_set(&A, S_RENDER_SCALE, "100"));
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
    for (i = 0; i < SETTINGS_COUNT; i++)                  /* pending at its default: not written */
        if (settings_def(i)->flags & SETTING_PENDING) {
            char k[96];
            snprintf(k, sizeof k, "\n%s=", settings_def(i)->key);
            CHECK(strstr(t, k) == NULL);
        }
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

    /* a 0.1.3 file (no ShadowSoftness): read without a note, the edge at 100 % */
    {
        static const char old[] = "[Meta]\nVersion=1\n[Graphics]\nSoftShadows=1\nSmoothEdges=high\n";
        r = settings_parse(&B, old, sizeof old - 1, &bad);
        CHECK(r == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_get_bool(&B, S_SOFT_SHADOWS));
        CHECK_STR(settings_get(&B, S_SHADOW_SOFTNESS), "100");
    }

    /* a pending setting changed by hand is written (the first boolean one, if any) */
    for (i = 0; i < SETTINGS_COUNT; i++) {
        const SettingDef *d = settings_def(i);
        char k[96];
        if (!(d->flags & SETTING_PENDING) || d->type != SETTING_BOOL) continue;
        settings_defaults(&A);
        settings_set(&A, i, strcmp(d->def, "1") ? "1" : "0");
        snprintf(k, sizeof k, "\n%s=%s\n", d->key, settings_get(&A, i));
        n = fmt(&A, &t);
        CHECK(strstr(t, k) != NULL);
        free(t);
        break;
    }

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
        CHECK_STR(B.ignored, "FrameRateLimit, Unknown, line 8");
    }
    /* HdTextures of earlier versions (1 / 0, then off / hud / all): 1 and all = On, 0 = Off;
     * HUD only = Off with HD HUD on (the HUD from the files the game ships) */
    {
        static const char hd1[] = "[Meta]\nVersion=1\n[Graphics]\nHdTextures=1\n";
        static const char hd0[] = "[Meta]\nVersion=1\n[Graphics]\nHdTextures=0\n";
        static const char hdh[] = "[Meta]\nVersion=1\n[Graphics]\nHdTextures=HUD\n";
        static const char hda[] = "[Meta]\nVersion=1\n[Graphics]\nHdTextures=all\n";
        static const char hdo[] = "[Meta]\nVersion=1\nPresets=3\n[Graphics]\nHdHud=0\nHdTextures=hud\n";
        r = settings_parse(&B, hd1, sizeof hd1 - 1, &bad);
        CHECK(r == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&B, S_HD_TEXTURES), "on");
        r = settings_parse(&B, hd0, sizeof hd0 - 1, &bad);
        CHECK(r == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&B, S_HD_TEXTURES), "off");
        r = settings_parse(&B, hdh, sizeof hdh - 1, &bad);
        CHECK(r == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&B, S_HD_TEXTURES), "off");
        CHECK(settings_get_bool(&B, S_HD_HUD));
        r = settings_parse(&B, hda, sizeof hda - 1, &bad);
        CHECK(r == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&B, S_HD_TEXTURES), "on");
        r = settings_parse(&B, hdo, sizeof hdo - 1, &bad);   /* a written HdHud wins */
        CHECK(r == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&B, S_HD_TEXTURES), "off");
        CHECK(!settings_get_bool(&B, S_HD_HUD));
        CHECK(!settings_set(&B, S_HD_TEXTURES, "half"));
        CHECK(settings_set(&B, S_HD_TEXTURES, "on") && !strcmp(settings_get(&B, S_HD_TEXTURES), "on"));
        /* the HD HUD line: Graphics > TEXTURES, on in Recommended and Steam Deck, off in Original Xbox */
        CHECK(settings_find("Graphics", "HdHud") == S_HD_HUD);
        CHECK(settings_def(S_HD_HUD)->tab == SETTINGS_TAB_GRAPHICS && !(settings_def(S_HD_HUD)->flags & SETTING_HIDDEN));
        CHECK_STR(settings_def(S_HD_HUD)->group, "TEXTURES");
        CHECK_STR(settings_def(S_HD_HUD)->env, "XBOX_HD_HUD");
        CHECK_STR(settings_def(S_HD_HUD)->def, "1");
        CHECK_STR(settings_def(S_HD_HUD)->preset[SETTINGS_PRESET_ORIGINAL_XBOX], "0");
        CHECK_STR(settings_def(S_HD_HUD)->preset[SETTINGS_PRESET_RECOMMENDED], "1");
        CHECK_STR(settings_def(S_HD_HUD)->preset[SETTINGS_PRESET_STEAM_DECK], "1");
    }
    /* a newer version: the keys known here are read */
    {
        static const char newer[] = "[Meta]\nVersion=2\n[Graphics]\nSmoothEdges=low\nNewThing=5\n";
        r = settings_parse(&B, newer, sizeof newer - 1, &bad);
        CHECK(r == SETTINGS_LOAD_NEWER);
        CHECK_STR(settings_get(&B, S_SMAA), "low");
        CHECK(bad == 1);
        CHECK_STR(B.ignored, "NewThing");
    }
    /* a newer version's settings are kept when the file is written again,
     * in their section or in a section of their own, and stay kept */
    {
        static const char newer[] =
            "[Meta]\nVersion=2\n"
            "[Graphics]\nSmoothEdges=low\nNewThing=5\n"
            "[Future]\nA=1\n"
            "[Display]\nResolution=1280x720\nWider = yes please\n"
            "[future]\nB=2\n"
            "[Display]\nFrameRateLimit=75\n";
        char *t2;
        size_t n2;
        r = settings_parse(&B, newer, sizeof newer - 1, &bad);
        CHECK(r == SETTINGS_LOAD_NEWER);
        CHECK(bad == 5);
        CHECK_STR(B.ignored, "NewThing, A, Wider, B, FrameRateLimit");
        CHECK_STR(settings_get(&B, S_FPS_LIMIT), "monitor");  /* not valid here: the default */
        n = fmt(&B, &t);
        CHECK(strstr(t, "[Meta]\nVersion=1\n") != NULL);
        CHECK(strstr(t, "\nNewThing=5\n") != NULL && strstr(t, "\nWider=yes please\n") != NULL);
        CHECK(strstr(t, "\n[Future]\nA=1\nB=2\n") != NULL);   /* one section, as first spelled */
        CHECK(strstr(t, "[future]") == NULL);
        CHECK(strstr(t, "FrameRateLimit=75") == NULL);
        {   /* each kept line in its own section: after [Graphics], before the next header */
            const char *g = strstr(t, "\n[Graphics]\n"), *k = strstr(t, "\nNewThing=5\n");
            CHECK(g && k && k > g && !memchr(g + 2, '[', (size_t)(k - g - 2)));
        }
        r = settings_parse(&A, t, n, &bad);
        CHECK(r == SETTINGS_LOAD_OK && bad == 4);             /* the four kept lines, no other */
        n2 = fmt(&A, &t2);
        CHECK(n2 == n && !strcmp(t, t2));                    /* stable from one save to the next */
        free(t2);
        free(t);
    }
    /* many unknown lines: the list ends with "...", the kept lines fit their buffer */
    {
        static char many[64 * 1024];
        size_t k = 0;
        int j;
        k += (size_t)sprintf(many + k, "[Meta]\nVersion=1\n[Display]\n");
        for (j = 0; j < 2000; j++) k += (size_t)sprintf(many + k, "Unknown%04d=some value\n", j);
        r = settings_parse(&B, many, k, &bad);
        CHECK(r == SETTINGS_LOAD_OK && bad == 2000);
        CHECK(strlen(B.ignored) < sizeof B.ignored && strstr(B.ignored, "...") != NULL);
        CHECK(strlen(B.extra) < sizeof B.extra && strstr(B.extra, "Unknown0000=some value\n") != NULL);
        settings_defaults(&B);
        CHECK(!B.extra[0] && !B.ignored[0]);
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

/* ── the install root: the game in bin\ below OpenTricky.exe, or everything in one folder ── */
static void test_install_root(const char *root)
{
    char top[600], inst[600], bin[600], sub[600], dev[600], build[600], lone[600], lonebin[600];
    char l1[700], l2[700], l3[700], out[1024], slash[700];

    path(top, root, "root \xC3\xA9");          /* outside ASCII, as test_files */
    path(inst, top, "OpenTricky");
    path(bin, inst, "bin");
    path(sub, inst, "tools");                 /* a subfolder not named bin */
    path(dev, top, "port");
    path(build, dev, "build");
    path(lone, top, "lone");
    path(lonebin, lone, "bin");
    mk(root); mk(top); mk(inst); mk(bin); mk(sub); mk(dev); mk(build); mk(lone); mk(lonebin);
    path(l1, inst, SETTINGS_LAUNCHER_EXE);
    path(l2, bin, SETTINGS_LAUNCHER_EXE);
    path(l3, build, SETTINGS_LAUNCHER_EXE);
    write_file(l1, "x");

    /* 1. flat (0.1.1, a test pack): the launcher's own folder */
    CHECK(settings_install_root_of(inst, out, sizeof out));
    CHECK_STR(out, inst);
    /* 2. the game in bin\ below OpenTricky.exe: the parent */
    CHECK(settings_install_root_of(bin, out, sizeof out));
    CHECK_STR(out, inst);
    sprintf(slash, "%s\\", bin);              /* a trailing separator changes nothing */
    CHECK(settings_install_root_of(slash, out, sizeof out));
    CHECK_STR(out, inst);
    /* 3. a subfolder with another name: itself */
    CHECK(settings_install_root_of(sub, out, sizeof out));
    CHECK_STR(out, sub);
    /* 4. a developer's build folder, with or without a launcher beside it: itself */
    CHECK(settings_install_root_of(build, out, sizeof out));
    CHECK_STR(out, build);
    write_file(l3, "x");
    CHECK(settings_install_root_of(build, out, sizeof out));
    CHECK_STR(out, build);
    /* 5. bin\ with no OpenTricky.exe above it: itself */
    CHECK(settings_install_root_of(lonebin, out, sizeof out));
    CHECK_STR(out, lonebin);
    /* 6. OpenTricky.exe in bin\ too: bin\ is a whole install of its own */
    write_file(l2, "x");
    CHECK(settings_install_root_of(bin, out, sizeof out));
    CHECK_STR(out, bin);
    /* 7. no folder at all */
    CHECK(!settings_install_root_of("", out, sizeof out));
    CHECK(!settings_install_root_of(NULL, out, sizeof out));
    /* 8. settings_locate reads and writes in the folder it is given */
    CHECK(settings_locate(inst, lone, 0, out, sizeof out) == SETTINGS_AT_GAME_FOLDER);
    path(slash, inst, "settings.ini");
    CHECK_STR(out, slash);

    rm(l1); rm(l2); rm(l3);
    rmdir_(lonebin); rmdir_(lone); rmdir_(build); rmdir_(dev); rmdir_(sub); rmdir_(bin); rmdir_(inst); rmdir_(top);
}

/* ── the first start on a Steam Deck: the Steam Deck preset, only under Wine ── */
static void set_env(const char *name, const char *value)    /* value NULL: unset */
{
#ifdef _WIN32
    char line[1100];
    snprintf(line, sizeof line, "%s=%s", name, value ? value : "");
    _putenv(line);
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static int first_preset(int wine, const char *vendor, const char *product, const char *steamdeck)
{
    SettingsPlatform p;
    char why[200];
    memset(&p, 0, sizeof p);
    p.wine = wine;
    snprintf(p.vendor, sizeof p.vendor, "%s", vendor);
    snprintf(p.product, sizeof p.product, "%s", product);
    snprintf(p.steamdeck, sizeof p.steamdeck, "%s", steamdeck);
    why[0] = 0;
    {
        int r = settings_first_start_preset(&p, why, sizeof why);
        CHECK(why[0] != 0);                     /* always a reason for the log */
        return r;
    }
}

static void test_first_start(const char *root)
{
    const int D = SETTINGS_PRESET_STEAM_DECK;
    char deck[600], sys[600], cls[600], dmi[600], id[600], f1[700], f2[700], f3[700], f4[700];
    SettingsPlatform p;
    char why[200];

    /* 1. the decision */
    CHECK(first_preset(0, "Valve", "Jupiter", "1") == -1);        /* never without Wine (Windows) */
    CHECK(first_preset(1, "Valve", "Jupiter", "") == D);          /* LCD */
    CHECK(first_preset(1, "Valve", "Galileo", "") == D);          /* OLED */
    CHECK(first_preset(1, "valve", "GALILEO", "") == D);          /* case does not matter */
    CHECK(first_preset(1, "Valve", "Jupiter", "0") == -1);        /* SteamDeck=0 turns it off */
    CHECK(first_preset(1, "Valve", "Fremont", "1") == -1);        /* another Valve machine: the DMI decides */
    CHECK(first_preset(1, "ASUSTeK COMPUTER INC.", "ROG Ally", "1") == -1);   /* SteamDeck=1 on another PC */
    CHECK(first_preset(1, "", "", "1") == D);                     /* no DMI: SteamDeck=1 */
    CHECK(first_preset(1, "", "", "") == -1);
    CHECK(first_preset(1, "", "", "yes") == -1);                  /* only "1" */
    CHECK(settings_first_start_preset(&(SettingsPlatform){ 0 }, NULL, 0) == -1);   /* no reason asked */

    /* 2. the DMI files, read under a root given by the test */
    path(deck, root, "deck");
    path(sys, deck, "sys");
    path(cls, sys, "class");
    path(dmi, cls, "dmi");
    path(id, dmi, "id");
    mk(root); mk(deck); mk(sys); mk(cls); mk(dmi); mk(id);
    path(f1, id, "sys_vendor");
    path(f2, id, "product_name");
    path(f3, id, "board_vendor");
    path(f4, id, "board_name");
    write_file(f1, "Valve\n");
    write_file(f2, "  Galileo \r\n");
    set_env("XBOX_TEST_DECK_ROOT", NULL);
    set_env("SteamDeck", NULL);
    settings_platform_probe(&p, deck);
    CHECK_STR(p.vendor, "Valve");
    CHECK_STR(p.product, "Galileo");
    CHECK_STR(p.steamdeck, "");
    rm(f1); rm(f2);
    write_file(f3, "Valve");                  /* no sys_vendor / product_name: the board's */
    write_file(f4, "Jupiter\n");
    settings_platform_probe(&p, deck);
    CHECK_STR(p.vendor, "Valve");
    CHECK_STR(p.product, "Jupiter");
    rm(f3); rm(f4);
    settings_platform_probe(&p, deck);        /* no DMI files at all */
    CHECK_STR(p.vendor, "");
    CHECK_STR(p.product, "");

    /* 3. this process: on Windows (no Wine), never a Steam Deck, even with SteamDeck=1 */
    set_env("SteamDeck", "1");
    settings_platform_probe(&p, NULL);
    CHECK_STR(p.steamdeck, "1");
    if (!p.wine) {
        CHECK_STR(p.vendor, "");                /* Z:\ not read */
        CHECK(settings_first_start_preset(&p, why, sizeof why) == -1);
    }

    /* 4. XBOX_TEST_DECK_ROOT (test only) stands in for Wine and its root */
    write_file(f1, "Valve\n");
    write_file(f2, "Jupiter\n");
    set_env("SteamDeck", NULL);
    set_env("XBOX_TEST_DECK_ROOT", deck);
    settings_platform_probe(&p, NULL);
    CHECK(p.wine);
    CHECK(settings_first_start_preset(&p, why, sizeof why) == D);
    CHECK(strstr(why, "Jupiter") != NULL);
    set_env("SteamDeck", "0");
    settings_platform_probe(&p, NULL);
    CHECK(settings_first_start_preset(&p, why, sizeof why) == -1);
    set_env("SteamDeck", NULL);
    set_env("XBOX_TEST_DECK_ROOT", NULL);

    /* 5. what the launcher does with it: no file, then the preset */
    path(f3, deck, "settings.ini");
    CHECK(settings_load(&A, f3, NULL) == SETTINGS_LOAD_MISSING);
    settings_apply_preset(&A, D);
    CHECK(settings_current_preset(&A) == D);

    rm(f1); rm(f2);
    rmdir_(id); rmdir_(dmi); rmdir_(cls); rmdir_(sys); rmdir_(deck);
}

/* ── the stable build: no extras; Lightmap smoothing and terrain seams without a line ── */
static int has_line(const char *text, const char *line)   /* "Key=Value" at a line's start */
{
    const char *p = text;
    size_t n = strlen(line);
    while ((p = strstr(p, line)) != NULL) {
        if ((p == text || p[-1] == '\n') && (p[n] == '\n' || p[n] == '\r' || !p[n])) return 1;
        p += n;
    }
    return 0;
}

/* src with whole lines changed: pairs of "Key=old", "Key=new" (each old line once). */
static void with_lines(char *dst, size_t cap, const char *src, const char *const *pairs, int n)
{
    int k;
    snprintf(dst, cap, "%s", src);
    for (k = 0; k + 1 < n; k += 2) {
        char *at = strstr(dst, pairs[k]);
        size_t lo = strlen(pairs[k]), ln = strlen(pairs[k + 1]), rest;
        if (!at || (at != dst && at[-1] != '\n') || (at[lo] != '\r' && at[lo] != '\n')) { CHECK(!"line to change"); continue; }
        rest = strlen(at + lo) + 1;
        if ((size_t)(at - dst) + ln + rest > cap) { CHECK(!"room"); continue; }
        memmove(at + ln, at + lo, rest);
        memcpy(at, pairs[k + 1], ln);
    }
}

static void test_stable(void)
{
    static char text[64 * 1024];
    int p, bad = 0;

    /* no Modern preset, none of the extras' keys */
    CHECK(SETTINGS_PRESET_COUNT == 3);
    CHECK_STR(settings_preset_name(SETTINGS_PRESET_COUNT), "Custom");
    CHECK(settings_find("Graphics", "DistantHaze") < 0 && settings_find("Graphics", "LightsInHaze") < 0);
    CHECK(settings_find("Graphics", "SpeedEffect") < 0 && settings_find("Graphics", "Bloom") < 0);
    CHECK(settings_find("Graphics", "Vignette") < 0 && settings_find("Graphics", "SpeedCamera") < 0);
    CHECK(settings_find("Controls", "StickDeadZone") < 0);

    /* 1. a new player: Recommended, the seams and the lightmap on, neither written */
    settings_defaults(&A);
    CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
    CHECK(settings_get_bool(&A, S_LIGHTMAP_SMOOTH) && settings_get_bool(&A, S_SMOOTH_SEAMS));
    settings_format(&A, text, sizeof text);
    CHECK(has_line(text, "LightmapSmoothing=1") && has_line(text, "SmoothTerrainSeams=1"));
    CHECK(has_line(text, "TextureFiltering=16") && !strstr(text, "LookLikeXbox="));
    CHECK(!strstr(text, "DistantHaze=") && !strstr(text, "Bloom=") && !strstr(text, "SpeedEffect="));
    CHECK(!strstr(text, "Vignette=") && !strstr(text, "SpeedCamera=") && !strstr(text, "StickDeadZone="));

    /* 2. a file of the 0.1.3 at Recommended (no mark, its TextureFiltering=off written as every
     *    line was): moved to Recommended as it is now (16x), seams and lightmap on; written with
     *    the mark, a later Off stays */
    {
        static const char f013[] = "[Meta]\nVersion=1\n[Display]\nFrameRateLimit=monitor\n[Graphics]\n"
                                   "TextureFiltering=off\nSmoothEdges=high\nSoftShadows=1\nLookLikeXbox=1\n"
                                   "[Advanced]\nSmoothLegs=1\n";
        static const char fmark[] = "[Meta]\nVersion=1\nPresets=2\n[Graphics]\nTextureFiltering=off\nLightmapSmoothing=0\n";
        static const char fcust[] = "[Meta]\nVersion=1\n[Graphics]\nTextureFiltering=off\nSmoothEdges=ultra\n";
        /* the file the public 0.1.3 launcher writes at Original Xbox (its settings_save; comments
         * and the key / button lines left out): no SnowLook, LightmapSmoothing, SmoothTerrainSeams,
         * HdHud, RenderScale or ShadowSoftness line, the 0.1.3 did not have them */
        static const char forig[] =
            "[Meta]\r\nVersion=1\r\n\r\n[Display]\r\nResolution=auto\r\nDisplayMode=fullscreen\r\nScreenShape=auto\r\n"
            "Menus=4:3\r\nFieldOfView=NoStretch\r\nRaceHud=xbox\r\nFrameRateLimit=60\r\nVSync=on\r\n\r\n"
            "[Graphics]\r\nHdTextures=0\r\nHdPackFolder=\r\nTextureFiltering=off\r\nSmoothEdges=off\r\nSoftShadows=0\r\n"
            "DrawDistance=original\r\nLookLikeXbox=1\r\n\r\n[Audio]\r\nSpeakers=stereo\r\nAudioDelay=8\r\n\r\n"
            "[Launcher]\r\nMusic=1\r\nSounds=low\r\nCheckForUpdates=ask\r\nUpdateChannel=stable\r\nSkipLauncher=0\r\n"
            "Theme=random\r\n\r\n[Controls]\r\nButtonStyle=auto\r\n\r\n[Game]\r\nDiscImage=\r\nSaveFolder=\r\n"
            "SaveProfile=mine\r\nSaveBackup=1\r\n\r\n[Advanced]\r\nShowFrameRate=0\r\nSmoothLegs=0\r\nLogFile=1\r\n";
        /* the 0.1.3's other files, made from it line by line (the 0.1.3 launcher writes the same
         * lines, only these values differ; checked against its settings_save):
         * Recommended; Original at 144 fps and Recommended at 1920x1080 (Custom); Original with
         * SMAA high and smoother legs (Custom, as close to Steam Deck as to Original Xbox) */
        static const char *const to_rec[] = { "RaceHud=xbox", "RaceHud=85", "FrameRateLimit=60", "FrameRateLimit=monitor",
                                              "SmoothEdges=off", "SmoothEdges=high", "SoftShadows=0", "SoftShadows=1",
                                              "SmoothLegs=0", "SmoothLegs=1" };
        static const char *const to_144[] = { "FrameRateLimit=60", "FrameRateLimit=144" };
        static const char *const to_1080[] = { "Resolution=auto", "Resolution=1920x1080" };
        static const char *const to_tie[] = { "SmoothEdges=off", "SmoothEdges=high", "SmoothLegs=0", "SmoothLegs=1" };
        static char frec[4096], forig144[4096], frec1080[4096], ftie[4096];
        /* a file with almost no line is not taken for Original Xbox */
        static const char fbare[] = "[Meta]\nVersion=1\n[Game]\nDiscImage=\n";
        CHECK(settings_parse(&A, f013, sizeof f013 - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&A, S_TEX_FILTER), "16");
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
        CHECK(settings_get_bool(&A, S_SMOOTH_LEGS));               /* Recommended as it is now: smoother legs */
        CHECK(settings_get_bool(&A, S_LIGHTMAP_SMOOTH) && settings_get_bool(&A, S_SMOOTH_SEAMS));
        CHECK(settings_get_bool(&A, S_HD_HUD));
        settings_format(&A, text, sizeof text);
        CHECK(has_line(text, "Presets=4") && has_line(text, "HdHud=1") && has_line(text, "Weather=ps2"));
        /* the mark: values chosen since stay */
        CHECK(settings_parse(&A, fmark, sizeof fmark - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&A, S_TEX_FILTER), "off");
        CHECK(!settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        /* no preset matched (SMAA Ultra): the file's values stay */
        CHECK(settings_parse(&A, fcust, sizeof fcust - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&A, S_TEX_FILTER), "off");
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        /* Original Xbox stays Original Xbox */
        CHECK(settings_parse(&A, forig, sizeof forig - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_ORIGINAL_XBOX);
        CHECK_STR(settings_get(&A, S_TEX_FILTER), "off");
        CHECK_STR(settings_get(&A, S_SNOW_LOOK), "original");
        CHECK(!settings_get_bool(&A, S_LIGHTMAP_SMOOTH) && !settings_get_bool(&A, S_SMOOTH_SEAMS));
        CHECK(!settings_get_bool(&A, S_HD_HUD));
        CHECK_STR(settings_get(&A, S_WEATHER), "original");
        settings_format(&A, text, sizeof text);              /* written with the mark: still Original Xbox */
        CHECK(has_line(text, "Presets=4") && has_line(text, "SnowLook=original") && has_line(text, "Weather=original"));
        CHECK(settings_parse(&B, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&B) == SETTINGS_PRESET_ORIGINAL_XBOX);
        with_lines(frec, sizeof frec, forig, to_rec, 10);
        with_lines(forig144, sizeof forig144, forig, to_144, 2);
        with_lines(frec1080, sizeof frec1080, frec, to_1080, 2);
        with_lines(ftie, sizeof ftie, forig, to_tie, 4);
        /* Recommended stays Recommended (16x, the new settings on) */
        CHECK(settings_parse(&A, frec, strlen(frec), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
        CHECK(settings_get_bool(&A, S_SMOOTH_LEGS));
        CHECK_STR(settings_get(&A, S_TEX_FILTER), "16");
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        /* Custom: its own values; the new settings from the closest preset */
        CHECK(settings_parse(&A, forig144, strlen(forig144), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        CHECK_STR(settings_get(&A, S_FPS_LIMIT), "144");
        CHECK_STR(settings_get(&A, S_SMAA), "off");
        CHECK_STR(settings_get(&A, S_SNOW_LOOK), "original");                 /* closest: Original Xbox */
        CHECK_STR(settings_get(&A, S_WEATHER), "original");
        CHECK(!settings_get_bool(&A, S_LIGHTMAP_SMOOTH) && !settings_get_bool(&A, S_SMOOTH_SEAMS));
        CHECK(!settings_get_bool(&A, S_HD_HUD));
        settings_format(&A, text, sizeof text);              /* written with the mark: the same values again */
        CHECK(settings_parse(&B, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&B, S_SNOW_LOOK), "original");
        CHECK_STR(settings_get(&B, S_WEATHER), "original");
        CHECK(!settings_get_bool(&B, S_HD_HUD) && !settings_get_bool(&B, S_LIGHTMAP_SMOOTH));
        CHECK(settings_parse(&A, frec1080, strlen(frec1080), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        CHECK_STR(settings_get(&A, S_RESOLUTION), "1920x1080");
        CHECK_STR(settings_get(&A, S_TEX_FILTER), "off");                     /* its own value */
        CHECK_STR(settings_get(&A, S_SNOW_LOOK), "ps2");                      /* closest: Recommended */
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        CHECK(settings_get_bool(&A, S_LIGHTMAP_SMOOTH) && settings_get_bool(&A, S_SMOOTH_SEAMS));
        CHECK(settings_get_bool(&A, S_HD_HUD));
        CHECK(settings_parse(&A, ftie, strlen(ftie), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        CHECK_STR(settings_get(&A, S_SMAA), "high");
        CHECK(settings_get_bool(&A, S_SMOOTH_LEGS));               /* Custom: its own value */
        CHECK_STR(settings_get(&A, S_SNOW_LOOK), "original");                 /* tie: Original Xbox */
        CHECK_STR(settings_get(&A, S_WEATHER), "original");
        CHECK(!settings_get_bool(&A, S_LIGHTMAP_SMOOTH) && !settings_get_bool(&A, S_HD_HUD));
        CHECK(settings_parse(&A, fbare, sizeof fbare - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
    }
    /* 2b. files of revision 2 (no HdHud yet): a preset gets its HD HUD value, a custom file the default */
    {
        static const char r2orig[] = "[Meta]\nVersion=1\nPresets=2\n[Display]\nRaceHud=xbox\nFrameRateLimit=60\n[Graphics]\n"
                                     "TextureFiltering=off\nSmoothEdges=off\nSoftShadows=0\nSnowLook=original\n"
                                     "LightmapSmoothing=0\n[Advanced]\nSmoothLegs=0\nSmoothTerrainSeams=0\n";
        static const char r2rec[] = "[Meta]\nVersion=1\nPresets=2\n[Graphics]\nTextureFiltering=16\nLightmapSmoothing=1\n";
        static const char r2cust[] = "[Meta]\nVersion=1\nPresets=2\n[Graphics]\nTextureFiltering=off\nLightmapSmoothing=0\n";
        CHECK(settings_parse(&A, r2orig, sizeof r2orig - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_ORIGINAL_XBOX);
        CHECK(!settings_get_bool(&A, S_HD_HUD));
        CHECK_STR(settings_get(&A, S_WEATHER), "original");
        CHECK(settings_parse(&A, r2rec, sizeof r2rec - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
        CHECK(settings_get_bool(&A, S_HD_HUD));
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        CHECK(settings_parse(&A, r2cust, sizeof r2cust - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        CHECK_STR(settings_get(&A, S_TEX_FILTER), "off");
        CHECK(settings_get_bool(&A, S_HD_HUD));
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");                        /* closest: Recommended */
    }
    /* 2c. files of revision 3 (the 0.1.4: no Weather yet): a preset gets its weather, a custom file
     *     the weather of the preset it is closest to; written with the mark, a later choice stays */
    {
        static const char r3orig[] = "[Meta]\nVersion=1\nPresets=3\n[Display]\nRaceHud=xbox\nFrameRateLimit=60\n[Graphics]\n"
                                     "HdHud=0\nTextureFiltering=off\nSmoothEdges=off\nSoftShadows=0\nSnowLook=original\n"
                                     "LightmapSmoothing=0\n[Advanced]\nSmoothLegs=0\nSmoothTerrainSeams=0\n";
        static const char r3o144[] = "[Meta]\nVersion=1\nPresets=3\n[Display]\nRaceHud=xbox\nFrameRateLimit=144\n[Graphics]\n"
                                     "HdHud=0\nTextureFiltering=off\nSmoothEdges=off\nSoftShadows=0\nSnowLook=original\n"
                                     "LightmapSmoothing=0\n[Advanced]\nSmoothLegs=0\nSmoothTerrainSeams=0\n";
        static const char r3rec[] = "[Meta]\nVersion=1\nPresets=3\n[Graphics]\nTextureFiltering=16\n";
        static const char r3r1080[] = "[Meta]\nVersion=1\nPresets=3\n[Display]\nResolution=1920x1080\n";
        static const char r3deck[] = "[Meta]\nVersion=1\nPresets=3\n[Display]\nResolution=1280x800\nFrameRateLimit=60\n"
                                     "[Graphics]\nSoftShadows=0\n";
        static const char r4orig[] = "[Meta]\nVersion=1\nPresets=4\n[Display]\nRaceHud=xbox\nFrameRateLimit=60\n[Graphics]\n"
                                     "HdHud=0\nTextureFiltering=off\nSmoothEdges=off\nSoftShadows=0\nSnowLook=original\n"
                                     "LightmapSmoothing=0\n[Advanced]\nSmoothLegs=0\nSmoothTerrainSeams=0\n";
        CHECK(settings_parse(&A, r3orig, sizeof r3orig - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_ORIGINAL_XBOX);
        CHECK_STR(settings_get(&A, S_WEATHER), "original");
        settings_format(&A, text, sizeof text);
        CHECK(has_line(text, "Presets=4") && has_line(text, "Weather=original"));
        CHECK(settings_parse(&B, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&B) == SETTINGS_PRESET_ORIGINAL_XBOX);
        CHECK(settings_set(&B, S_WEATHER, "ps2"));           /* chosen since: stays */
        settings_format(&B, text, sizeof text);
        CHECK(settings_parse(&A, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        CHECK(settings_parse(&A, r3o144, sizeof r3o144 - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        CHECK_STR(settings_get(&A, S_FPS_LIMIT), "144");
        CHECK_STR(settings_get(&A, S_WEATHER), "original");                   /* closest: Original Xbox */
        CHECK(settings_parse(&A, r3rec, sizeof r3rec - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        CHECK(settings_parse(&A, r3r1080, sizeof r3r1080 - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");                        /* closest: Recommended */
        CHECK(settings_parse(&A, r3deck, sizeof r3deck - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_STEAM_DECK);
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        CHECK(!settings_get_bool(&A, S_SMOOTH_SEAMS) && !settings_get_bool(&A, S_LIGHTMAP_SMOOTH));   /* the lighter Deck */
        /* revision 4 without a Weather line (written by hand): the default, PS2, so not Original Xbox */
        CHECK(settings_parse(&A, r4orig, sizeof r4orig - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
    }
    /* 2d. the lighter Steam Deck of revision 4 (Smoother terrain seams and Lightmap smoothing off):
     *     the 0.1.4's Steam Deck file, every shown line written as its launcher does, is moved to it,
     *     and so are the Steam Deck files of revision 2 and of the 0.1.3; a Steam Deck its player
     *     changed (Custom) keeps its own seams and lightmap and takes the Weather of the preset it
     *     is closest to; Recommended keeps both on; written with the mark, a later choice stays */
    {
        static char d014[64 * 1024], d120[64 * 1024], dseams[64 * 1024], rec014[64 * 1024], rec014l[64 * 1024];
        static const char *const to_014[] = { "Presets=4", "Presets=3", "SmoothTerrainSeams=0", "SmoothTerrainSeams=1",
                                              "LightmapSmoothing=0", "LightmapSmoothing=1", "Weather=ps2", "",
                                              "SmoothLegs=0", "SmoothLegs=1" };
        static const char *const to_120[] = { "FrameRateLimit=60", "FrameRateLimit=120" };
        static const char *const to_seams_off[] = { "SmoothTerrainSeams=1", "SmoothTerrainSeams=0" };
        static const char *const rec_to_014[] = { "Presets=4", "Presets=3", "Weather=ps2", "" };
        static const char *const to_legs_off[] = { "SmoothLegs=1", "SmoothLegs=0" };
        static const char r2deck[] = "[Meta]\nVersion=1\nPresets=2\n[Display]\nResolution=1280x800\nFrameRateLimit=60\n"
                                     "[Graphics]\nSoftShadows=0\nLightmapSmoothing=1\n[Advanced]\nSmoothTerrainSeams=1\n";
        static const char r1deck[] = "[Meta]\nVersion=1\n[Display]\nResolution=1280x800\nFrameRateLimit=60\n"
                                     "[Graphics]\nSoftShadows=0\nTextureFiltering=off\n";
        /* a Steam Deck without the mark from a development build that wrote Lightmap smoothing: read with
         * the values of its own revision (Off, before revision 2), not those of revision 3 */
        static const char r1deckl[] = "[Meta]\nVersion=1\n[Display]\nResolution=1280x800\nFrameRateLimit=60\n"
                                      "[Graphics]\nSoftShadows=0\nTextureFiltering=off\nLightmapSmoothing=0\n";
        /* the 0.1.4's Steam Deck: this Steam Deck written, then as the 0.1.4 had it */
        settings_defaults(&B);
        settings_apply_preset(&B, SETTINGS_PRESET_STEAM_DECK);
        settings_format(&B, text, sizeof text);
        CHECK(has_line(text, "Presets=4") && has_line(text, "Weather=ps2"));
        CHECK(has_line(text, "SmoothTerrainSeams=0") && has_line(text, "LightmapSmoothing=0"));
        with_lines(d014, sizeof d014, text, to_014, 10);
        CHECK(has_line(d014, "Presets=3") && has_line(d014, "SmoothTerrainSeams=1") && has_line(d014, "LightmapSmoothing=1"));
        CHECK(!strstr(d014, "Weather=") && has_line(d014, "SmoothLegs=1"));
        CHECK(settings_parse(&A, d014, strlen(d014), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_STEAM_DECK);
        CHECK(!settings_get_bool(&A, S_SMOOTH_SEAMS) && !settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
        CHECK(!settings_get_bool(&A, S_SMOOTH_LEGS));             /* the game's legs, as in the Steam Deck now */
        CHECK_STR(settings_get(&A, S_TEX_FILTER), "16");          /* the rest of the Deck as it was */
        CHECK_STR(settings_get(&A, S_SNOW_LOOK), "ps2");
        CHECK(settings_get_bool(&A, S_HD_HUD));
        CHECK_STR(settings_get(&A, S_SMAA), "high");
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        settings_format(&A, text, sizeof text);                   /* written with the mark */
        CHECK(has_line(text, "Presets=4") && has_line(text, "SmoothTerrainSeams=0") && has_line(text, "LightmapSmoothing=0"));
        CHECK(settings_parse(&B, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&B) == SETTINGS_PRESET_STEAM_DECK);
        CHECK(settings_set(&B, S_SMOOTH_SEAMS, "1"));             /* turned back on since: stays, Custom */
        settings_format(&B, text, sizeof text);
        CHECK(settings_parse(&A, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_get_bool(&A, S_SMOOTH_SEAMS) && !settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        /* the 0.1.4's Steam Deck at 120 fps (Custom): its own seams and lightmap, the Weather of
         * the preset it is closest to (Steam Deck: PS2); written with the mark, the same again */
        with_lines(d120, sizeof d120, d014, to_120, 2);
        CHECK(settings_parse(&A, d120, strlen(d120), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        CHECK_STR(settings_get(&A, S_FPS_LIMIT), "120");
        CHECK(settings_get_bool(&A, S_SMOOTH_SEAMS) && settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
        CHECK(settings_get_bool(&A, S_SMOOTH_LEGS));              /* Custom: its own legs too */
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        settings_format(&A, text, sizeof text);
        CHECK(settings_parse(&B, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&B) == SETTINGS_PRESET_CUSTOM);
        CHECK(settings_get_bool(&B, S_SMOOTH_SEAMS) && settings_get_bool(&B, S_LIGHTMAP_SMOOTH));
        /* the 0.1.4's Steam Deck with the seams turned off by its player (Custom): kept as they are */
        with_lines(dseams, sizeof dseams, d014, to_seams_off, 2);
        CHECK(settings_parse(&A, dseams, strlen(dseams), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        CHECK(!settings_get_bool(&A, S_SMOOTH_SEAMS) && settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        /* the Steam Deck of revision 2 and of the 0.1.3 (no mark, 0.1.3's Off filtering): the lighter Deck */
        CHECK(settings_parse(&A, r2deck, sizeof r2deck - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_STEAM_DECK);
        CHECK(!settings_get_bool(&A, S_SMOOTH_SEAMS) && !settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
        CHECK(settings_get_bool(&A, S_HD_HUD));
        CHECK(settings_parse(&A, r1deck, sizeof r1deck - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_STEAM_DECK);
        CHECK(!settings_get_bool(&A, S_SMOOTH_SEAMS) && !settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
        CHECK_STR(settings_get(&A, S_TEX_FILTER), "16");
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        CHECK(settings_parse(&A, r1deckl, sizeof r1deckl - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_STEAM_DECK);
        CHECK(!settings_get_bool(&A, S_SMOOTH_SEAMS) && !settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
        /* the 0.1.4's Recommended, every shown line written: Recommended, both on, the legs on */
        settings_defaults(&B);
        settings_apply_preset(&B, SETTINGS_PRESET_RECOMMENDED);
        settings_format(&B, text, sizeof text);
        with_lines(rec014, sizeof rec014, text, rec_to_014, 4);
        CHECK(has_line(rec014, "Presets=3") && has_line(rec014, "SmoothTerrainSeams=1") && !strstr(rec014, "Weather="));
        CHECK(has_line(rec014, "SmoothLegs=1"));
        CHECK(settings_parse(&A, rec014, strlen(rec014), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
        CHECK(settings_get_bool(&A, S_SMOOTH_SEAMS) && settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
        CHECK(settings_get_bool(&A, S_SMOOTH_LEGS));
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        settings_format(&A, text, sizeof text);                   /* written with the mark: On, Recommended */
        CHECK(has_line(text, "Presets=4") && has_line(text, "SmoothLegs=1"));
        CHECK(settings_parse(&B, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&B) == SETTINGS_PRESET_RECOMMENDED);
        CHECK(settings_set(&B, S_SMOOTH_LEGS, "0"));              /* turned off since: stays, Custom */
        settings_format(&B, text, sizeof text);
        CHECK(settings_parse(&A, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(!settings_get_bool(&A, S_SMOOTH_LEGS));
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        /* the 0.1.4's Recommended with Smoother legs turned off by its player (Custom in the 0.1.4):
         * its own Off kept, the Weather of the preset it is closest to (Recommended); still Custom */
        with_lines(rec014l, sizeof rec014l, rec014, to_legs_off, 2);
        CHECK(settings_parse(&A, rec014l, strlen(rec014l), &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(!settings_get_bool(&A, S_SMOOTH_LEGS));
        CHECK_STR(settings_get(&A, S_WEATHER), "ps2");
        CHECK(settings_get_bool(&A, S_SMOOTH_SEAMS) && settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        /* the 0.1.4's Steam Deck with Smoother legs turned off by its player (Custom in the 0.1.4):
         * kept as it is (its seams and lightmap on, its legs off), as the seams above */
        {
            static char d014l[64 * 1024];
            with_lines(d014l, sizeof d014l, d014, to_legs_off, 2);
            CHECK(settings_parse(&A, d014l, strlen(d014l), &bad) == SETTINGS_LOAD_OK && bad == 0);
            CHECK(!settings_get_bool(&A, S_SMOOTH_LEGS));
            CHECK(settings_get_bool(&A, S_SMOOTH_SEAMS) && settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
            CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        }
        settings_defaults(&A);
        settings_defaults(&B);
    }

    /* 3. a development file with Modern, its effects on, a smooth stick and the player's own
     *    settings: Recommended, the extras' lines kept as another version's (not used, written
     *    back), the rest read */
    {
        static const char fdev[] =
            "[Meta]\nVersion=1\n[Display]\nFieldOfView=Full\n"
            "[Graphics]\nDistantHaze=1\nLightsInHaze=1\nSpeedEffect=subtle\nBloom=subtle\nVignette=1\n"
            "SpeedCamera=1\nLightmapSmoothing=1\n[Audio]\nSpeakers=5.1\n[Controls]\nStickDeadZone=smooth_ground\n"
            "[Keyboard]\nCross=K\n[Advanced]\nSmoothTerrainSeams=1\nSmoothLegs=1\n";
        CHECK(settings_parse(&A, fdev, sizeof fdev - 1, &bad) == SETTINGS_LOAD_OK);
        CHECK(bad == 0);            /* the six effects and the stick: left out on purpose, no note */
        CHECK_STR(A.ignored, "");
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
        CHECK(settings_get_bool(&A, S_LIGHTMAP_SMOOTH) && settings_get_bool(&A, S_SMOOTH_SEAMS));
        CHECK_STR(settings_get(&A, S_FOV), "Full");
        CHECK_STR(settings_get(&A, S_SPEAKERS), "5.1");
        CHECK_STR(settings_get(&A, S_KEY_FIRST + 0), "K");
        settings_format(&A, text, sizeof text);
        CHECK(has_line(text, "DistantHaze=1") && has_line(text, "StickDeadZone=smooth_ground"));
        CHECK(has_line(text, "LightmapSmoothing=1") && has_line(text, "SmoothTerrainSeams=1"));
        CHECK(has_line(text, "FieldOfView=Full") && has_line(text, "Speakers=5.1") && has_line(text, "Cross=K"));
        CHECK(settings_parse(&B, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);   /* the same again */
        CHECK(settings_current_preset(&B) == SETTINGS_PRESET_RECOMMENDED);
    }

    /* 4. LookLikeXbox=0 of an earlier version: read, written back, the preset as it was */
    {
        static const char flook[] = "[Meta]\nVersion=1\n[Graphics]\nLookLikeXbox=0\n";
        CHECK(settings_parse(&A, flook, sizeof flook - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_RECOMMENDED);
    }

    /* 4b. Lightmap smoothing Off by hand (troubleshooting, a file with the mark): kept, written, Custom */
    {
        static const char foff[] = "[Meta]\nVersion=1\nPresets=2\n[Graphics]\nLightmapSmoothing=0\n";
        CHECK(settings_parse(&A, foff, sizeof foff - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
        CHECK(!settings_get_bool(&A, S_LIGHTMAP_SMOOTH) && settings_get_bool(&A, S_SMOOTH_SEAMS));
        CHECK(settings_current_preset(&A) == SETTINGS_PRESET_CUSTOM);
        settings_format(&A, text, sizeof text);
        CHECK(has_line(text, "LightmapSmoothing=0") && has_line(text, "SmoothTerrainSeams=1"));
        CHECK(has_line(text, "TextureFiltering=16"));
        settings_apply_preset(&A, SETTINGS_PRESET_RECOMMENDED);     /* a preset puts it back */
        CHECK(settings_get_bool(&A, S_LIGHTMAP_SMOOTH));
    }

    /* 5. Original Xbox: both off, written, read back as Original Xbox; every preset offered */
    settings_defaults(&A);
    settings_apply_preset(&A, SETTINGS_PRESET_ORIGINAL_XBOX);
    CHECK(!settings_get_bool(&A, S_LIGHTMAP_SMOOTH) && !settings_get_bool(&A, S_SMOOTH_SEAMS));
    settings_format(&A, text, sizeof text);
    CHECK(has_line(text, "LightmapSmoothing=0") && has_line(text, "SmoothTerrainSeams=0"));
    CHECK(settings_parse(&B, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
    CHECK(settings_current_preset(&B) == SETTINGS_PRESET_ORIGINAL_XBOX);
    for (p = 0; p < SETTINGS_PRESET_COUNT; p++) {
        settings_apply_preset(&A, p);
        CHECK(settings_current_preset(&A) == p);
        CHECK(settings_get_bool(&A, S_LIGHTMAP_SMOOTH) == (p != SETTINGS_PRESET_ORIGINAL_XBOX && p != SETTINGS_PRESET_STEAM_DECK));
        CHECK(settings_get_bool(&A, S_SMOOTH_SEAMS) == (p != SETTINGS_PRESET_ORIGINAL_XBOX && p != SETTINGS_PRESET_STEAM_DECK));
    }
    settings_defaults(&A);
}

/* ── PlayStation controllers: CONTROLS > CONTROLLERS, PC, off by default, in no preset (a preset
 *    never turns it on or off), XBOX_PS_PADS; written in [Controls], read back ── */
static void test_ps_controllers(void)
{
    static char text[65536];
    static const char f_on[] = "[Meta]\nVersion=1\n[Controls]\nPlayStationControllers=1\n";
    const SettingDef *d = settings_def(S_PS_CONTROLLERS);
    const char *controls, *line, *next;
    int p, bad = 0;
    settings_defaults(&A);
    CHECK(settings_find("Controls", "PlayStationControllers") == S_PS_CONTROLLERS);
    CHECK(S_PS_CONTROLLERS == SETTINGS_COUNT - 1);          /* the last id: no other one moved */
    CHECK(!settings_get_bool(&A, S_PS_CONTROLLERS));
    CHECK_STR(d->def, "0");
    CHECK(d->type == SETTING_BOOL);
    CHECK(d->tab == SETTINGS_TAB_CONTROLS);
    CHECK_STR(d->group, "CONTROLLERS");
    CHECK_STR(d->env, "XBOX_PS_PADS");
    CHECK(d->flags == SETTING_PC);
    for (p = 0; p < SETTINGS_PRESET_COUNT; p++) CHECK(d->preset[p] == NULL);
    for (p = 0; p < SETTINGS_PRESET_COUNT; p++) {
        settings_set(&A, S_PS_CONTROLLERS, "1");
        settings_apply_preset(&A, p);
        CHECK(settings_get_bool(&A, S_PS_CONTROLLERS));
    }
    /* written in [Controls], after ButtonStyle, before the next section */
    settings_format(&A, text, sizeof text);
    controls = strstr(text, "\n[Controls]\n");
    line = strstr(text, "\nPlayStationControllers=1\n");
    next = controls ? strstr(controls + 1, "\n[") : NULL;
    CHECK(controls && line && line > controls && (!next || line < next));
    CHECK(line && strstr(text, "\nButtonStyle=") && strstr(text, "\nButtonStyle=") < line);
    CHECK(settings_parse(&B, text, strlen(text), &bad) == SETTINGS_LOAD_OK && bad == 0);
    CHECK(settings_get_bool(&B, S_PS_CONTROLLERS));
    CHECK(settings_parse(&B, f_on, sizeof f_on - 1, &bad) == SETTINGS_LOAD_OK && bad == 0);
    CHECK(settings_get_bool(&B, S_PS_CONTROLLERS));
    settings_defaults(&A);
    settings_defaults(&B);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "settings_test_tmp";
    test_registry();
    test_defaults_presets();
    test_stable();
    test_values();
    test_text();
    test_files(root);
    test_install_root(root);
    test_first_start(root);
    test_ps_controllers();
    printf("%s: %d checks, %d failed\n", s_fail ? "FAIL" : "PASS", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
