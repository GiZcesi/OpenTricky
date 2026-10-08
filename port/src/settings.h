/*
 * settings.h -- the player's settings: one registry, one file (settings.ini).
 *
 * Every setting the player can change, and the few hidden ones kept for
 * troubleshooting, is one entry of a single table (settings.c): its place in
 * the file, the launcher tab and group it shows in, its English label and
 * help, its values, its default, the XBOX_* variable it stands for in the
 * game, whether it is a PC addition (not on the original Xbox), and its value
 * in each preset. The launcher (OpenTricky.exe) builds its pages from it, the
 * file is read and written through it, and the game (SSX Tricky.exe) reads
 * the same file at start. Adding a setting = adding one entry.
 *
 * The file is "settings.ini" beside OpenTricky.exe (the game's folder). When
 * that folder cannot be written (Program Files), it is
 * Documents\My Games\SSX Tricky\settings.ini instead, and the launcher says
 * so (settings_locate). It starts with [Meta] Version=1; a file without it
 * (the old "SSX Tricky.ini") is not read.
 *
 * Values are kept as text, as the file has them (UTF-8). This file depends
 * on nothing but the C library, so the launcher, the game and the host tests
 * share it; only settings_locate and the file calls know about Windows.
 */
#ifndef SSX_SETTINGS_H
#define SSX_SETTINGS_H

#include <stddef.h>

#define SETTINGS_FILE_NAME   "settings.ini"
#define SETTINGS_VERSION     1
#define SETTINGS_VALUE_MAX   1024       /* bytes per value, terminator included (paths in UTF-8) */

/* Launcher tabs. SETTINGS_TAB_NONE: not shown (hidden keys, launcher state). */
enum {
    SETTINGS_TAB_DISPLAY, SETTINGS_TAB_GRAPHICS, SETTINGS_TAB_AUDIO,
    SETTINGS_TAB_CONTROLS, SETTINGS_TAB_GAME, SETTINGS_TAB_ADVANCED,
    SETTINGS_TAB_COUNT,
    SETTINGS_TAB_NONE = -1
};

/* Presets. Custom = none of them (settings_current_preset). */
enum {
    SETTINGS_PRESET_ORIGINAL_XBOX, SETTINGS_PRESET_RECOMMENDED, SETTINGS_PRESET_STEAM_DECK,
    SETTINGS_PRESET_COUNT,
    SETTINGS_PRESET_CUSTOM = SETTINGS_PRESET_COUNT
};

/* Kinds of value. */
enum {
    SETTING_BOOL,       /* "0" / "1" (also read: on/off, true/false, yes/no) */
    SETTING_CHOICE,     /* one of the choices' tokens (case-insensitive when read) */
    SETTING_INT,        /* a whole number in [min, max] */
    SETTING_RESOLUTION, /* "auto" or WIDTHxHEIGHT */
    SETTING_PATH,       /* a file or folder; may be empty */
    SETTING_KEY         /* a keyboard key name (settings_key_names), or "None" */
};

/* Flags. */
#define SETTING_PC        0x01u /* a PC addition, not on the original Xbox: "PC" label;
                                 * the Original Xbox preset turns these off */
#define SETTING_HIDDEN    0x02u /* not shown by the launcher; written only when it is not the default */
#define SETTING_LAUNCHER  0x04u /* the launcher's own (the game does not use it) */
#define SETTING_PENDING   0x08u /* not applied by the game yet (its variable is not read) */

typedef struct SettingChoice {
    const char *token;          /* in the file */
    const char *label;          /* shown */
} SettingChoice;

typedef struct SettingDef {
    const char *section;        /* [Section] in the file */
    const char *key;            /* Key= in the file */
    int         tab;            /* SETTINGS_TAB_* */
    const char *group;          /* the small heading above it in its tab, or NULL */
    const char *label;          /* English */
    const char *help;           /* English, one or two sentences (also the file's comment) */
    int         type;           /* SETTING_* */
    const SettingChoice *choices;
    int         nchoices;
    int         min, max;       /* SETTING_INT */
    const char *def;            /* default value, as in the file */
    const char *env;            /* the XBOX_* variable(s) of the game it stands for, or NULL */
    unsigned    flags;          /* SETTING_* flags */
    const char *preset[SETTINGS_PRESET_COUNT];  /* value in each preset; NULL = left as it is */
} SettingDef;

/* The keyboard and controller bindings: one per control, in the order of the
 * controls (controls.h CTL_*), named after the PS2 button in that place. */
#define SETTINGS_KEY_CONTROLS 24   /* CTL_COUNT */
#define SETTINGS_PAD_CONTROLS 16   /* CTL_PAD_COUNT: the sticks are not bound on a controller */

/* Setting ids: index into settings_registry() and Settings.v. */
enum {
    /* DISPLAY */
    S_MONITOR, S_RESOLUTION, S_DISPLAY_MODE, S_ASPECT, S_MENUS, S_FOV, S_RACE_HUD,
    S_FPS_LIMIT, S_VSYNC,
    /* GRAPHICS */
    S_HD_TEXTURES, S_HD_PACK_FOLDER, S_TEX_FILTER, S_SMAA, S_SOFT_SHADOWS, S_DRAW_DISTANCE,
    S_LOOK_LIKE_XBOX,
    /* AUDIO */
    S_SPEAKERS, S_AUDIO_DELAY, S_LAUNCHER_MUSIC, S_LAUNCHER_SOUNDS,
    /* CONTROLS */
    S_BUTTON_STYLE,
    S_KEY_FIRST,
    S_PAD_FIRST = S_KEY_FIRST + SETTINGS_KEY_CONTROLS,
    /* GAME */
    S_DISC_IMAGE = S_PAD_FIRST + SETTINGS_PAD_CONTROLS, S_SAVE_FOLDER, S_SAVE_BACKUP,
    S_PAUSE_IN_BACKGROUND, S_MUTE_IN_BACKGROUND,
    /* ADVANCED */
    S_CHECK_UPDATES, S_UPDATE_CHANNEL, S_SKIP_LAUNCHER, S_SHOW_FPS, S_THEME, S_LOG_FILE,
    /* hidden */
    S_SMOOTH_MOTION, S_HD_TEXTURES_MENUS, S_FIX_CULL_WINDING, S_FIX_OCCLUSION,
    S_FIX_TEX_PASSTHROUGH, S_FIX_GAMMA, S_FIX_KICKWAIT,
    SETTINGS_COUNT
};

typedef struct Settings {
    char v[SETTINGS_COUNT][SETTINGS_VALUE_MAX];   /* ~110 KB: keep it static or on the heap */
} Settings;

/* The table: SETTINGS_COUNT entries, in the order of the ids. */
const SettingDef *settings_registry(void);
const SettingDef *settings_def(int id);
int settings_find(const char *section, const char *key);       /* id, or -1 */

const char *settings_tab_name(int tab);         /* "DISPLAY", ...; "" for none */
const char *settings_preset_name(int preset);   /* "Original Xbox", "Recommended", "Steam Deck", "Custom" */
/* Keyboard key names a SETTING_KEY accepts besides single letters, digits and
 * F1-F24 (the names controls.c knows). NULL-terminated. */
const char *const *settings_key_names(void);

/* Every setting at its default. */
void settings_defaults(Settings *s);
/* Set `id` to `value` if the value is valid for it (normalized: canonical
 * token case, "1"/"0" for a boolean); otherwise nothing changes. 1 = set. */
int  settings_set(Settings *s, int id, const char *value);
const char *settings_get(const Settings *s, int id);
int  settings_get_bool(const Settings *s, int id);
int  settings_get_int(const Settings *s, int id);          /* SETTING_INT, SETTING_BOOL */
int  settings_choice_index(const Settings *s, int id);     /* SETTING_CHOICE: index in choices, or -1 */
/* SETTING_RESOLUTION: 1 and the size when one is set, 0 for "auto". */
int  settings_get_resolution(const Settings *s, int id, int *w, int *h);
int  settings_is_default(const Settings *s, int id);

/* Presets: set every value the preset names; which preset the values match
 * (all its named values equal), SETTINGS_PRESET_CUSTOM if none. */
void settings_apply_preset(Settings *s, int preset);
int  settings_current_preset(const Settings *s);

/* ── The file ── */
enum {
    SETTINGS_LOAD_OK,           /* read (keys not known or with a bad value: their default) */
    SETTINGS_LOAD_MISSING,      /* no file: defaults */
    SETTINGS_LOAD_NOT_V1,       /* no [Meta] Version (an old .ini): not read, defaults */
    SETTINGS_LOAD_NEWER,        /* written by a newer version: the keys known here are read */
    SETTINGS_LOAD_ERROR         /* could not be read: defaults */
};
/* Defaults, then the file at `path` (UTF-8). `bad`, when not NULL, gets the
 * number of lines ignored (unknown key, invalid value). */
int  settings_load(Settings *s, const char *path, int *bad);
/* Parse a whole file already in memory (the same rules; for tests and the launcher). */
int  settings_parse(Settings *s, const char *text, size_t len, int *bad);
/* Write the whole file, with comments, through "<path>.new" then a rename,
 * so a failure never leaves half a file. Hidden settings are written only
 * when not at their default. 1 = written. */
int  settings_save(const Settings *s, const char *path);
/* The same text, into `out` (NUL-terminated). Returns the full length
 * (larger than out_sz - 1 when it did not fit). */
size_t settings_format(const Settings *s, char *out, size_t out_sz);

/* ── Where the file is ── */
enum {
    SETTINGS_AT_GAME_FOLDER,    /* beside the executables */
    SETTINGS_AT_DOCUMENTS,      /* Documents\My Games\SSX Tricky: the game's folder is read-only */
    SETTINGS_AT_NONE            /* neither can be used */
};
/* The file to read (for_write 0) or to write (for_write 1), in UTF-8:
 *  - read: <game folder>\settings.ini if it exists, else the one in
 *    Documents if it exists, else <game folder>\settings.ini (no file yet);
 *  - write: <game folder>\settings.ini if that folder can be written, else
 *    Documents\My Games\SSX Tricky\settings.ini (folder created).
 * Returns SETTINGS_AT_*. `game_dir` and `docs_dir` NULL = this executable's
 * folder and the real Documents\My Games\SSX Tricky (Windows); tests pass
 * their own. */
int  settings_locate(const char *game_dir, const char *docs_dir, int for_write, char *out, size_t out_sz);
/* This executable's folder and Documents\My Games\SSX Tricky, in UTF-8 (Windows). 1 = found. */
int  settings_game_dir(char *out, size_t out_sz);
int  settings_docs_dir(char *out, size_t out_sz);

#endif /* SSX_SETTINGS_H */
