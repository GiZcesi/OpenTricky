/*
 * settings.c -- the settings registry and settings.ini (see settings.h).
 *
 * Plain C99: the table, the checks, the file format. Only the file calls
 * and settings_game_dir / settings_docs_dir know about Windows, where every
 * path is UTF-8 here and wide for the system.
 */
#include "settings.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <io.h>
#define SEP '\\'
#else
#include <sys/stat.h>
#include <errno.h>
#define SEP '/'
#endif

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define CH(a) (a), COUNT(a)

/* ── Values ────────────────────────────────────────────────────────── */

static const SettingChoice k_display_mode[] = {
    { "window", "Window" }, { "fullscreen", "Borderless fullscreen" },
};
/* Same order as LAUNCHER_ASPECT_* (launcher.h). */
static const SettingChoice k_aspect[] = {
    { "4:3", "4:3" }, { "16:9", "16:9" }, { "21:9", "21:9" }, { "32:9", "32:9" },
    { "auto", "Auto" },
};
static const SettingChoice k_menus[] = {
    { "4:3", "Original 4:3" }, { "16:9", "Xbox 16:9" },
};
/* Tokens as aspect.c names them (ASPECT_FOV_*). */
static const SettingChoice k_fov[] = {
    { "NoStretch", "No stretch" }, { "Balanced", "Balanced" }, { "Full", "Full" },
};
/* hud_anchor.h sizes (100, 90, 85, 80, 70 %), then the Xbox's stretched HUD. */
static const SettingChoice k_race_hud[] = {
    { "100", "100%" }, { "90", "90%" }, { "85", "85%" }, { "80", "80%" }, { "70", "70%" },
    { "xbox", "Stretched (Xbox)" },
};
static const SettingChoice k_fps_limit[] = {
    { "monitor", "Match screen" }, { "60", "60 (Xbox)" }, { "120", "120" },
    { "144", "144" }, { "240", "240" }, { "unlimited", "Unlimited" },
};
static const SettingChoice k_vsync[] = {
    { "on", "On" }, { "adaptive", "Adaptive" }, { "off", "Off" },
};
/* HD textures: launcher.c LAUNCHER_HD_* order. Files of earlier versions have
 * 0 / 1, or off / hud / all (normalize: 1 and all = on, 0 and hud = off; a
 * file's "hud" also turns HD HUD on: settings_parse). */
static const SettingChoice k_hd_textures[] = {
    { "off", "Off" }, { "on", "On" },
};
static const SettingChoice k_tex_filter[] = {
    { "off", "Off (Xbox)" }, { "2", "2x" }, { "4", "4x" }, { "8", "8x" }, { "16", "16x" },
};
/* Same order as XBOX_SMAA_PRESET's levels (0 = off). */
static const SettingChoice k_smaa[] = {
    { "off", "Off" }, { "low", "Low" }, { "medium", "Medium" }, { "high", "High" }, { "ultra", "Ultra" },
};
/* The 3D drawn smaller or larger than the resolution (XBOX_RENDER_SCALE);
 * the HUD, menus and text stay at the resolution. Same steps as d3d8_rscale. */
static const SettingChoice k_render_scale[] = {
    { "50", "50%" }, { "67", "67%" }, { "75", "75%" }, { "85", "85%" }, { "100", "100%" },
    { "125", "125%" }, { "150", "150%" }, { "175", "175%" }, { "200", "200%" },
};
/* The width of the soft shadows' edge, in percent of the first one
 * (XBOX_SOFT_SHADOW_WIDTH); 60 = the "40 % less" the edge was tried at. */
static const SettingChoice k_shadow_softness[] = {
    { "40", "40%" }, { "60", "60%" }, { "80", "80%" }, { "100", "100%" },
    { "125", "125%" }, { "150", "150%" },
};
static const SettingChoice k_draw_distance[] = {
    { "original", "Original" }, { "far", "Far" }, { "max", "Max" },
};
/* XBOX_SNOW_LOOK (the hidden "mixed" is not offered). The token stays "ps2"
 * (settings.ini, XBOX_SNOW_LOOK); only the label shown is "Soft". */
static const SettingChoice k_snow_look[] = {
    { "original", "Original" }, { "ps2", "Soft" },
};
/* XBOX_WEATHER (weather.h): the original game's snowfall fog, or the
 * snowfall without it, as in the PS2 version. */
static const SettingChoice k_weather[] = {
    { "original", "Original" }, { "ps2", "PS2" },
};
static const SettingChoice k_speakers[] = {
    { "auto", "Auto" }, { "stereo", "Stereo" }, { "5.1", "5.1" },
};
/* Audio buffers of 5.33 ms (XBOX_AUDIO_QUEUE); the delay is ~ (n - 1) x 5.33 + 39 ms. */
static const SettingChoice k_audio_delay[] = {
    { "4", "Short (55 ms)" }, { "8", "Normal (76 ms)" },
    { "12", "Long (98 ms)" }, { "16", "Longer (119 ms)" },
};
static const SettingChoice k_launcher_sounds[] = {
    { "off", "Off" }, { "low", "Low" }, { "medium", "Medium" },
};
/* Same order as XBOX_BUTTON_ICONS's names (launcher.c k_icons_name). */
static const SettingChoice k_button_style[] = {
    { "auto", "Auto" }, { "modern", "Xbox modern" }, { "playstation", "PlayStation modern" },
    { "ps2", "PS2 original" },
};
/* Same order as PAD_* (controls.h). */
static const SettingChoice k_pad[] = {
    { "None", "None" }, { "A", "A" }, { "B", "B" }, { "X", "X" }, { "Y", "Y" },
    { "LB", "Left bumper" }, { "RB", "Right bumper" }, { "LT", "Left trigger" }, { "RT", "Right trigger" },
    { "Start", "Start" }, { "Back", "Back" }, { "LS", "Left stick press" }, { "RS", "Right stick press" },
    { "DpadUp", "D-pad up" }, { "DpadDown", "D-pad down" }, { "DpadLeft", "D-pad left" },
    { "DpadRight", "D-pad right" },
};
/* "unlockall": a save of its own (UnlockAll in the save folder) with the
 * game's own unlock-everything cheat on (XBOX_UNLOCK_ALL=1). */
static const SettingChoice k_save_profile[] = {
    { "mine", "My save" }, { "unlockall", "Unlock all" },
};
static const SettingChoice k_check_updates[] = {
    { "ask", "Ask before installing" }, { "never", "Never" },
};
static const SettingChoice k_update_channel[] = {
    { "stable", "Stable" }, { "unstable", "Beta" },
};
/* The launcher's theme: a track's colours, emblem and card (read from the disc). */
static const SettingChoice k_theme[] = {
    { "random", "Random" }, { "alaska", "Alaska" }, { "aloha", "Aloha Ice Jam" },
    { "elysium", "Elysium Alps" }, { "garibaldi", "Garibaldi" }, { "merqury", "Merqury City Meltdown" },
    { "mesablanca", "Mesablanca" }, { "pipedream", "Pipedream" }, { "snowdream", "Snowdream" },
    { "tokyo", "Tokyo Megaplex" }, { "untracked", "Untracked" },
};

static const char *const k_key_names[] = {
    "Space", "Enter", "Escape", "Tab", "Backspace", "CapsLock",
    "Up", "Down", "Left", "Right",
    "LeftShift", "RightShift", "Shift", "LeftCtrl", "RightCtrl", "Ctrl",
    "Insert", "Delete", "Home", "End", "PageUp", "PageDown",
    "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9",
    "Num*", "Num+", "Num-", "Num.", "Num/",
    ";", "=", ",", "-", ".", "/", "`", "[", "\\", "]", "'",
    NULL
};

const char *const *settings_key_names(void) { return k_key_names; }

/* ── The registry ──────────────────────────────────────────────────── */

/* Preset columns: Original Xbox, Recommended, Steam Deck. */
#define P(ox, rec, deck) { ox, rec, deck }
#define NOP { NULL, NULL, NULL }

#define T_DISPLAY  SETTINGS_TAB_DISPLAY
#define T_GRAPHICS SETTINGS_TAB_GRAPHICS
#define T_AUDIO    SETTINGS_TAB_AUDIO
#define T_CONTROLS SETTINGS_TAB_CONTROLS
#define T_GAME     SETTINGS_TAB_GAME
#define T_ADVANCED SETTINGS_TAB_ADVANCED
#define T_NONE     SETTINGS_TAB_NONE

#define BOOL_(def) SETTING_BOOL, NULL, 0, 0, 1, def
#define CHOICE(tbl, def) SETTING_CHOICE, CH(tbl), 0, 0, def

/* Keyboard and controller bindings, in the order of controls.h CTL_*:
 * key in the file, label, default key, default controller input. */
#define BIND_KEY(k, lab, dk) \
    { "Keyboard", k, T_CONTROLS, "KEYBOARD", lab, NULL, SETTING_KEY, NULL, 0, 0, 0, dk, NULL, 0, NOP }
#define BIND_PAD(k, lab, dp) \
    { "Controller", k, T_CONTROLS, "CONTROLLER", lab, NULL, CHOICE(k_pad, dp), NULL, 0, NOP }

static const SettingDef k_reg[SETTINGS_COUNT] = {
    /* ── DISPLAY ── */
    [S_MONITOR] = { "Display", "Monitor", T_DISPLAY, "SCREEN", "Screen",
        "The screen the game opens on.",
        SETTING_INT, NULL, 0, 0, 16, "0", "XBOX_MONITOR", SETTING_PENDING, NOP },
    [S_RESOLUTION] = { "Display", "Resolution", T_DISPLAY, "SCREEN", "Resolution",
        "The size of the picture. Auto uses your screen's size in fullscreen, or the largest size that fits in a window.",
        SETTING_RESOLUTION, NULL, 0, 0, 0, "auto", NULL, 0, P("auto", "auto", "1280x800") },
    [S_RENDER_SCALE] = { "Display", "RenderScale", T_DISPLAY, "SCREEN", "Render scale",
        "Draws the 3D below or above your resolution. Lower is lighter on the graphics card but softer; higher looks smoother and finer but is much heavier. Menus and the race display stay sharp.",
        CHOICE(k_render_scale, "100"), "XBOX_RENDER_SCALE", SETTING_PC, P("100", "100", "100") },
    [S_DISPLAY_MODE] = { "Display", "DisplayMode", T_DISPLAY, "SCREEN", "Display mode",
        "Window or borderless fullscreen. Alt+Enter (or F11) switches while playing.",
        CHOICE(k_display_mode, "fullscreen"), NULL, 0, P("fullscreen", "fullscreen", "fullscreen") },
    [S_ASPECT] = { "Display", "ScreenShape", T_DISPLAY, "SCREEN", "Screen shape",
        "Match your screen. 16:9 is the game's own widescreen mode; 21:9 and 32:9 widen it further. Auto follows your screen.",
        CHOICE(k_aspect, "auto"), NULL, 0, P("auto", "auto", "auto") },
    [S_MENUS] = { "Display", "Menus", T_DISPLAY, "WIDE SCREENS", "Menus",
        "Original 4:3 shows the menus as in the original game, centred. Xbox 16:9 uses the game's widescreen menus. Races look the same either way.",
        CHOICE(k_menus, "4:3"), "XBOX_WIDE_MENUS", 0, P("4:3", "4:3", "4:3") },
    [S_FOV] = { "Display", "FieldOfView", T_DISPLAY, "WIDE SCREENS", "Field of view",
        "How much you see on screens wider than 16:9. No stretch keeps the 16:9 view; Full shows more, stretched at the edges; Balanced is in between.",
        CHOICE(k_fov, "NoStretch"), "XBOX_WIDE_FOV", 0, NOP },
    [S_RACE_HUD] = { "Display", "RaceHud", T_DISPLAY, "WIDE SCREENS", "Race HUD size",
        "The size of the race display (timer, position, boost). Stretched (Xbox) spreads it across the screen, as in the original game.",
        CHOICE(k_race_hud, "85"), "XBOX_HUD_SHAPE, XBOX_HUD_SIZE", 0, P("xbox", "85", "85") },
    [S_FPS_LIMIT] = { "Display", "FrameRateLimit", T_DISPLAY, "FRAME RATE", "Frame rate limit",
        "The highest frame rate shown. Above 60, extra frames are smoothed in between; the game itself always runs at 60, like the original game. Started on a 60 Hz screen with Match screen or VSync, the game stays at 60: restart it on a faster screen.",
        CHOICE(k_fps_limit, "monitor"), "XBOX_FPS_CAP", SETTING_PC, P("60", "monitor", "60") },
    [S_VSYNC] = { "Display", "VSync", T_DISPLAY, "FRAME RATE", "VSync",
        "On: no tearing. Adaptive: smoother when the frame rate drops, with some tearing. Off: lowest input delay, with tearing.",
        CHOICE(k_vsync, "on"), "XBOX_SYNC", 0, P("on", "on", "on") },

    /* ── GRAPHICS ── */
    [S_HD_HUD] = { "Graphics", "HdHud", T_GRAPHICS, "TEXTURES", "HD HUD",
        "The race display in HD: timer, gauges, positions and their text, sharper at any resolution. Off: the original display.",
        BOOL_("1"), "XBOX_HD_HUD", SETTING_PC, P("0", "1", "1") },
    [S_HD_TEXTURES] = { "Graphics", "HdTextures", T_GRAPHICS, "TEXTURES", "HD textures",
        "Sharper textures from your HD pack, except the menu pictures and the race display (see HD HUD).",
        CHOICE(k_hd_textures, "off"), "XBOX_HD_TEXTURES, XBOX_HD_TEXTURES_HUD", SETTING_PC, P("off", "off", "off") },
    [S_HD_PACK_FOLDER] = { "Graphics", "HdPackFolder", T_GRAPHICS, "TEXTURES", "HD pack folder",
        "The folder that holds your HD pack.",
        SETTING_PATH, NULL, 0, 0, 0, "", "XBOX_HD_TEXTURES", 0, NOP },
    [S_TEX_FILTER] = { "Graphics", "TextureFiltering", T_GRAPHICS, "TEXTURES", "Texture filtering",
        "Sharper textures on distant slopes. Off looks like the original game.",
        CHOICE(k_tex_filter, "16"), "XBOX_ANISO", SETTING_PC, P("off", "16", "16") },
    [S_SMAA] = { "Graphics", "SmoothEdges", T_GRAPHICS, "EDGES AND SHADOWS", "Smooth edges (SMAA)",
        "Smooths jagged edges in races. Menus and the race display stay sharp.",
        CHOICE(k_smaa, "high"), "XBOX_SMAA, XBOX_SMAA_PRESET", SETTING_PC, P("off", "high", "high") },
    [S_SOFT_SHADOWS] = { "Graphics", "SoftShadows", T_GRAPHICS, "EDGES AND SHADOWS", "Soft shadows",
        "Softens the edges of shadows. Off gives hard edges, like the original game.",
        BOOL_("1"), "XBOX_SOFT_SHADOWS", SETTING_PC, P("0", "1", "0") },
    [S_SHADOW_SOFTNESS] = { "Graphics", "ShadowSoftness", T_GRAPHICS, "EDGES AND SHADOWS", "Shadow softness",
        "The width of the shadows' soft edge, with Soft shadows on. Lower is crisper, higher is softer.",
        CHOICE(k_shadow_softness, "100"), "XBOX_SOFT_SHADOW_WIDTH", SETTING_PC, P("100", "100", "100") },
    [S_DRAW_DISTANCE] = { "Graphics", "DrawDistance", T_GRAPHICS, "WORLD", "Draw distance",
        "How far ahead the scenery appears. Original is the game's own distance; Far and Max show more of each track.",
        CHOICE(k_draw_distance, "original"), "XBOX_DRAW_DISTANCE", SETTING_PC, P("original", "original", "original") },
    [S_SNOW_LOOK] = { "Graphics", "SnowLook", T_GRAPHICS, "WORLD", "Snow look",
        "Soft: smoother snow without grain, with visible board trails. Original: the original grainy snow.",
        CHOICE(k_snow_look, "ps2"), "XBOX_SNOW_LOOK", SETTING_PC, P("original", "ps2", "ps2") },
    [S_WEATHER] = { "Graphics", "Weather", T_GRAPHICS, "WORLD", "Weather",
        "PS2: no thick fog when it snows, the night sky stays in view, and Garibaldi, Elysium and Untracked always stay clear, as in the PS2 version. Original: the thick fog that comes with snowfall in the original game.",
        CHOICE(k_weather, "ps2"), "XBOX_WEATHER", SETTING_PC, P("original", "ps2", "ps2") },
    [S_LIGHTMAP_SMOOTH] = { "Graphics", "LightmapSmoothing", T_GRAPHICS, "WORLD", "Lightmap smoothing",
        "Smooths the light baked into the snow and the slopes: no more diamond-shaped steps in it. The light itself stays the same.",
        BOOL_("1"), "XBOX_LIGHTMAP_FILTER", SETTING_PC, P("0", "1", "0") },
    /* The fixes of the port's picture (fog volumes, lens flares, board tops,
     * gamma): always on, no line, in no preset; the key of earlier versions is
     * still read (a file is not "not understood") but changes nothing. Each fix
     * can still be turned off alone for troubleshooting (the Fix* keys below). */
    [S_LOOK_LIKE_XBOX] = { "Graphics", "LookLikeXbox", T_NONE, NULL, "Picture fixes",
        "Always on: the fog, lens flares, board tops and colours drawn as the game draws them. Ignored (see the Fix keys).",
        BOOL_("1"), "XBOX_FIX_CULLWIND, XBOX_FIX_OCCLUSION, XBOX_FIX_TEXPASSTHRU, XBOX_FIX_GAMMA", SETTING_HIDDEN, NOP },

    /* ── AUDIO ── */
    [S_SPEAKERS] = { "Audio", "Speakers", T_AUDIO, "GAME SOUND", "Speakers",
        "Auto follows Windows. 5.1 plays the game's surround mix; for headphones, turn on Windows Sonic or Dolby Atmos.",
        CHOICE(k_speakers, "stereo"), "XBOX_AUDIO_OUTPUT", SETTING_PC, NOP },
    [S_AUDIO_DELAY] = { "Audio", "AudioDelay", T_AUDIO, "GAME SOUND", "Audio delay",
        "Shorter is more responsive; make it longer if the sound crackles.",
        CHOICE(k_audio_delay, "8"), "XBOX_AUDIO_QUEUE", 0, NOP },
    [S_LAUNCHER_MUSIC] = { "Launcher", "Music", T_NONE, NULL, "Launcher music",
        "Ignored: the launcher plays no music.",
        BOOL_("1"), NULL, SETTING_LAUNCHER | SETTING_HIDDEN, NOP },
    [S_LAUNCHER_SOUNDS] = { "Launcher", "Sounds", T_NONE, NULL, "Launcher sounds",
        "Ignored: the launcher plays no sounds.",
        CHOICE(k_launcher_sounds, "low"), NULL, SETTING_LAUNCHER | SETTING_HIDDEN, NOP },

    /* ── CONTROLS ── */
    [S_BUTTON_STYLE] = { "Controls", "ButtonStyle", T_CONTROLS, "LOOK", "Button style",
        "How buttons are drawn in the game's menus and help. Auto shows the Xbox modern buttons, or the PlayStation ones when you play with a PlayStation controller with the PlayStation controllers setting on.",
        CHOICE(k_button_style, "auto"), "XBOX_BUTTON_ICONS", 0, NOP },
    /* Off: XBOX_PS_PADS is not set and the game reads XInput only (pad_sdl.h).
     * Its id is the last one (settings.h): shown after the other CONTROLS rows. */
    [S_PS_CONTROLLERS] = { "Controls", "PlayStationControllers", T_CONTROLS, "CONTROLLERS", "PlayStation controllers (experimental)",
        "A DualShock 4 or DualSense works without any other tool, and with Button style Auto the game shows PlayStation buttons. Each controller keeps its player until it is unplugged. Xbox controllers are not affected. Leave it off with Steam Input, DS4Windows, or Steam open with its PlayStation support: they already make the controller work, and it would be read twice.",
        BOOL_("0"), "XBOX_PS_PADS", SETTING_PC, NOP },
    [S_KEY_FIRST + 0]  = BIND_KEY("Cross",           "Cross",           "Space"),
    [S_KEY_FIRST + 1]  = BIND_KEY("Circle",          "Circle",          "Escape"),
    [S_KEY_FIRST + 2]  = BIND_KEY("Square",          "Square",          "C"),
    [S_KEY_FIRST + 3]  = BIND_KEY("Triangle",        "Triangle",        "V"),
    [S_KEY_FIRST + 4]  = BIND_KEY("R1",              "R1",              "R"),
    [S_KEY_FIRST + 5]  = BIND_KEY("L1",              "L1",              "F"),
    [S_KEY_FIRST + 6]  = BIND_KEY("L2",              "L2",              "Q"),
    [S_KEY_FIRST + 7]  = BIND_KEY("R2",              "R2",              "E"),
    [S_KEY_FIRST + 8]  = BIND_KEY("Start",           "Start",           "Enter"),
    [S_KEY_FIRST + 9]  = BIND_KEY("Select",          "Select",          "Tab"),
    [S_KEY_FIRST + 10] = BIND_KEY("L3",              "L3 (left stick press)",  "X"),
    [S_KEY_FIRST + 11] = BIND_KEY("R3",              "R3 (right stick press)", "None"),
    [S_KEY_FIRST + 12] = BIND_KEY("DpadUp",          "D-pad up",        "Up"),
    [S_KEY_FIRST + 13] = BIND_KEY("DpadDown",        "D-pad down",      "Down"),
    [S_KEY_FIRST + 14] = BIND_KEY("DpadLeft",        "D-pad left",      "Left"),
    [S_KEY_FIRST + 15] = BIND_KEY("DpadRight",       "D-pad right",     "Right"),
    [S_KEY_FIRST + 16] = BIND_KEY("LeftStickUp",     "Left stick up",   "Up"),
    [S_KEY_FIRST + 17] = BIND_KEY("LeftStickDown",   "Left stick down", "Down"),
    [S_KEY_FIRST + 18] = BIND_KEY("LeftStickLeft",   "Left stick left", "Left"),
    [S_KEY_FIRST + 19] = BIND_KEY("LeftStickRight",  "Left stick right", "Right"),
    [S_KEY_FIRST + 20] = BIND_KEY("RightStickUp",    "Right stick up",  "None"),
    [S_KEY_FIRST + 21] = BIND_KEY("RightStickDown",  "Right stick down", "None"),
    [S_KEY_FIRST + 22] = BIND_KEY("RightStickLeft",  "Right stick left", "None"),
    [S_KEY_FIRST + 23] = BIND_KEY("RightStickRight", "Right stick right", "None"),
    [S_PAD_FIRST + 0]  = BIND_PAD("Cross",     "Cross",     "A"),
    [S_PAD_FIRST + 1]  = BIND_PAD("Circle",    "Circle",    "B"),
    [S_PAD_FIRST + 2]  = BIND_PAD("Square",    "Square",    "X"),
    [S_PAD_FIRST + 3]  = BIND_PAD("Triangle",  "Triangle",  "Y"),
    [S_PAD_FIRST + 4]  = BIND_PAD("R1",        "R1",        "RB"),
    [S_PAD_FIRST + 5]  = BIND_PAD("L1",        "L1",        "LB"),
    [S_PAD_FIRST + 6]  = BIND_PAD("L2",        "L2",        "LT"),
    [S_PAD_FIRST + 7]  = BIND_PAD("R2",        "R2",        "RT"),
    [S_PAD_FIRST + 8]  = BIND_PAD("Start",     "Start",     "Start"),
    [S_PAD_FIRST + 9]  = BIND_PAD("Select",    "Select",    "Back"),
    [S_PAD_FIRST + 10] = BIND_PAD("L3",        "L3 (left stick press)",  "LS"),
    [S_PAD_FIRST + 11] = BIND_PAD("R3",        "R3 (right stick press)", "RS"),
    [S_PAD_FIRST + 12] = BIND_PAD("DpadUp",    "D-pad up",    "DpadUp"),
    [S_PAD_FIRST + 13] = BIND_PAD("DpadDown",  "D-pad down",  "DpadDown"),
    [S_PAD_FIRST + 14] = BIND_PAD("DpadLeft",  "D-pad left",  "DpadLeft"),
    [S_PAD_FIRST + 15] = BIND_PAD("DpadRight", "D-pad right", "DpadRight"),

    /* ── GAME ── */
    [S_DISC_IMAGE] = { "Game", "DiscImage", T_GAME, "DISC AND SAVES", "Disc image",
        "Your SSX Tricky (USA) Xbox disc image (.iso or .xiso). PLAY stays off until it is set.",
        SETTING_PATH, NULL, 0, 0, 0, "", NULL, 0, NOP },
    [S_SAVE_FOLDER] = { "Game", "SaveFolder", T_GAME, "DISC AND SAVES", "Save folder",
        "Where your saves are kept. Automatic uses Documents\\My Games\\SSX Tricky\\Saves, or, beside OpenTricky.exe, the Saves folder (with portable.txt) or an earlier version's hdd folder.",
        SETTING_PATH, NULL, 0, 0, 0, "", NULL, 0, NOP },
    [S_SAVE_PROFILE] = { "Game", "SaveProfile", T_GAME, "DISC AND SAVES", "Save profile",
        "My save: your own progress. Unlock all: everything open, in a separate save.",
        CHOICE(k_save_profile, "mine"), "XBOX_UNLOCK_ALL", 0, NOP },
    [S_SAVE_BACKUP] = { "Game", "SaveBackup", T_GAME, "DISC AND SAVES", "Save backup copy",
        "Keeps a copy of your previous save, so a damaged save can be recovered.",
        BOOL_("1"), "XBOX_SAVE_BACKUP", 0, NOP },
    [S_PAUSE_IN_BACKGROUND] = { "Game", "PauseInBackground", T_GAME, "IN THE BACKGROUND", "Pause the game",
        "Pauses the game while another window is in front.",
        BOOL_("1"), "XBOX_BACKGROUND_PAUSE", SETTING_PENDING, NOP },
    [S_MUTE_IN_BACKGROUND] = { "Game", "MuteInBackground", T_GAME, "IN THE BACKGROUND", "Mute the game",
        "Mutes the game while another window is in front.",
        BOOL_("1"), "XBOX_BACKGROUND_MUTE", SETTING_PENDING, NOP },
    [S_TC_GHOST] = { "Game", "TimeChallengeGhost", T_NONE, NULL, "Ghost of your best run",
        "Your best Time Challenge run on the track rides along, see-through. It cannot touch you or change your race.",
        BOOL_("0"), "XBOX_TC_GHOST", SETTING_PC | SETTING_HIDDEN, P("0", "0", "0") },

    /* ── ADVANCED ── */
    [S_CHECK_UPDATES] = { "Launcher", "CheckForUpdates", T_ADVANCED, "UPDATES", "Check for updates",
        "Looks for a new version and asks before installing. Never turns off the automatic check.",
        CHOICE(k_check_updates, "ask"), NULL, SETTING_LAUNCHER, NOP },
    [S_UPDATE_CHANNEL] = { "Launcher", "UpdateChannel", T_ADVANCED, "UPDATES", "Update channel",
        "Stable releases only, or betas too (newer, less tested).",
        CHOICE(k_update_channel, "stable"), NULL, SETTING_LAUNCHER, NOP },
    [S_SKIP_LAUNCHER] = { "Launcher", "SkipLauncher", T_ADVANCED, "LAUNCH", "Skip launcher next time",
        "Starts the game directly.",
        BOOL_("0"), NULL, SETTING_LAUNCHER, NOP },
    [S_SHOW_FPS] = { "Advanced", "ShowFrameRate", T_ADVANCED, "LAUNCH", "Show frame rate",
        "Shows the frame rate in the window's title bar (window mode only).",
        BOOL_("0"), NULL, 0, NOP },
    [S_THEME] = { "Launcher", "Theme", T_NONE, NULL, "Launcher theme",
        "Ignored: the launcher picks its track at each start.",
        CHOICE(k_theme, "random"), NULL, SETTING_LAUNCHER | SETTING_HIDDEN, NOP },
    [S_SMOOTH_LEGS] = { "Advanced", "SmoothLegs", T_ADVANCED, "SMOOTHING", "Smoother legs in turns",
        "Smooths the leg and board shake riders have when carving, a quirk of the original game. Visual only.",
        BOOL_("1"), "XBOX_FPS_SMOOTH_LEGS", SETTING_PC, P("0", "1", "0") },
    [S_SMOOTH_SEAMS] = { "Advanced", "SmoothTerrainSeams", T_ADVANCED, "SMOOTHING", "Smoother terrain seams",
        "Hides most of the lines between pieces of terrain, a quirk of the original game. Visual only.",
        BOOL_("1"), "XBOX_TERRAIN_BLEND, XBOX_TERRAIN_BLEND_WIDTH, XBOX_TERRAIN_CLAMP", SETTING_PC, P("0", "1", "0") },
    [S_LOG_FILE] = { "Advanced", "LogFile", T_ADVANCED, "TROUBLESHOOTING", "Log file",
        "Keeps a log of the game for bug reports.",
        BOOL_("1"), NULL, 0, NOP },

    /* ── hidden: troubleshooting only, written only when changed ── */
    [S_SMOOTH_MOTION] = { "Advanced", "SmoothMotion", T_NONE, NULL, "Smooth motion",
        "Above 60 frames a second, the frames in between are interpolated (1, default); 0 shows each game step until the next.",
        BOOL_("1"), "XBOX_FPS_INTERP", SETTING_HIDDEN, NOP },
    [S_HD_TEXTURES_MENUS] = { "Graphics", "HdTexturesMenus", T_NONE, NULL, "HD textures in menus",
        "1 = the HD pack also replaces the menu and interface pictures.",
        BOOL_("0"), "XBOX_HD_TEXTURES_MENUS", SETTING_HIDDEN, NOP },
    [S_FIX_CULL_WINDING] = { "Graphics", "FixCullWinding", T_NONE, NULL, "Back-face culling winding",
        "A fix of the picture: fog volumes. 0 = off, for troubleshooting only.",
        BOOL_("1"), "XBOX_FIX_CULLWIND", SETTING_HIDDEN, NOP },
    [S_FIX_OCCLUSION] = { "Graphics", "FixOcclusion", T_NONE, NULL, "Occlusion queries",
        "A fix of the picture: lens flares. 0 = off, for troubleshooting only.",
        BOOL_("1"), "XBOX_FIX_OCCLUSION", SETTING_HIDDEN, NOP },
    [S_FIX_TEX_PASSTHROUGH] = { "Graphics", "FixTexturePassThrough", T_NONE, NULL, "Pass-through texture stages",
        "A fix of the picture: board tops, terrain fog. 0 = off, for troubleshooting only.",
        BOOL_("1"), "XBOX_FIX_TEXPASSTHRU", SETTING_HIDDEN, NOP },
    [S_FIX_GAMMA] = { "Graphics", "FixGamma", T_NONE, NULL, "Gamma ramp",
        "A fix of the picture: the title's gamma ramp. 0 = off, for troubleshooting only.",
        BOOL_("1"), "XBOX_FIX_GAMMA", SETTING_HIDDEN, NOP },
    [S_FIX_KICKWAIT] = { "Advanced", "FixKickWait", T_NONE, NULL, "No wait on the GPU flush",
        "1 = the game does not busy-wait on the GPU write-combine flush (default); 0 = the original port's wait.",
        BOOL_("1"), "XBOX_FIX_KICKWAIT", SETTING_HIDDEN, NOP },
    [S_TC_GHOST_OPACITY] = { "Game", "TimeChallengeGhostOpacity", T_NONE, NULL, "Ghost opacity",
        "How solid the Time Challenge ghost looks, in percent (10 to 90).",
        SETTING_INT, NULL, 0, 10, 90, "45", "XBOX_TC_GHOST_ALPHA", SETTING_HIDDEN | SETTING_PC, NOP },
};

const SettingDef *settings_registry(void) { return k_reg; }

const SettingDef *settings_def(int id)
{
    return (id >= 0 && id < SETTINGS_COUNT) ? &k_reg[id] : NULL;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

int settings_find(const char *section, const char *key)
{
    int i;
    for (i = 0; i < SETTINGS_COUNT; i++)
        if (ieq(k_reg[i].section, section) && ieq(k_reg[i].key, key)) return i;
    return -1;
}

const char *settings_tab_name(int tab)
{
    static const char *const names[SETTINGS_TAB_COUNT] = {
        "DISPLAY", "GRAPHICS", "AUDIO", "CONTROLS", "GAME", "ADVANCED"
    };
    return (tab >= 0 && tab < SETTINGS_TAB_COUNT) ? names[tab] : "";
}

const char *settings_preset_name(int preset)
{
    static const char *const names[SETTINGS_PRESET_COUNT + 1] = {
        "Original Xbox", "Recommended", "Steam Deck",
        "Custom"
    };
    return (preset >= 0 && preset <= SETTINGS_PRESET_COUNT) ? names[preset] : names[SETTINGS_PRESET_CUSTOM];
}

/* ── Checking a value ──────────────────────────────────────────────── */

static int copy_value(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= SETTINGS_VALUE_MAX) return 0;
    memcpy(dst, src, n + 1);
    return 1;
}

/* A key name as controls.c reads it, in its canonical spelling. */
static int normalize_key(const char *s, char *out)
{
    int i;
    unsigned v;
    if (!s[0] || ieq(s, "None")) { strcpy(out, "None"); return 1; }
    if (!s[1] && isalnum((unsigned char)s[0])) {
        out[0] = (char)toupper((unsigned char)s[0]);
        out[1] = 0;
        return 1;
    }
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        char *end;
        long f = strtol(s + 1, &end, 10);
        if (!*end && f >= 1 && f <= 24) { sprintf(out, "F%ld", f); return 1; }
    }
    for (i = 0; k_key_names[i]; i++)
        if (ieq(s, k_key_names[i])) { strcpy(out, k_key_names[i]); return 1; }
    if ((s[0] == 'K' || s[0] == 'k') && strlen(s) == 7 && !strncmp(s + 1, "ey0", 3) &&
        (s[4] == 'x' || s[4] == 'X') && isxdigit((unsigned char)s[5]) && isxdigit((unsigned char)s[6]) &&
        sscanf(s + 5, "%2x", &v) == 1 && v > 0) {
        sprintf(out, "Key0x%02X", v);
        return 1;
    }
    return 0;
}

/* Normalize `value` for `d` into out; 0 if it is not valid. */
static int normalize(const SettingDef *d, const char *value, char *out)
{
    switch (d->type) {
    case SETTING_BOOL:
        if (!strcmp(value, "1") || ieq(value, "on") || ieq(value, "true") || ieq(value, "yes")) {
            strcpy(out, "1");
            return 1;
        }
        if (!strcmp(value, "0") || ieq(value, "off") || ieq(value, "false") || ieq(value, "no")) {
            strcpy(out, "0");
            return 1;
        }
        return 0;
    case SETTING_CHOICE: {
        int i;
        for (i = 0; i < d->nchoices; i++)
            if (ieq(value, d->choices[i].token)) { strcpy(out, d->choices[i].token); return 1; }
        if (d->choices == k_hd_textures) {      /* earlier versions: 0 / 1, then off / hud / all */
            if (!strcmp(value, "1") || ieq(value, "all") || ieq(value, "true") || ieq(value, "yes")) {
                strcpy(out, "on");
                return 1;
            }
            if (!strcmp(value, "0") || ieq(value, "hud") || ieq(value, "false") || ieq(value, "no")) {
                strcpy(out, "off");
                return 1;
            }
        }
        return 0;
    }
    case SETTING_INT: {
        char *end;
        long v;
        if (!value[0]) return 0;
        v = strtol(value, &end, 10);
        if (*end || v < d->min || v > d->max) return 0;
        sprintf(out, "%ld", v);
        return 1;
    }
    case SETTING_RESOLUTION: {
        int w, h;
        char tail;
        if (ieq(value, "auto")) { strcpy(out, "auto"); return 1; }
        if ((sscanf(value, "%dx%d%c", &w, &h, &tail) == 2 || sscanf(value, "%dX%d%c", &w, &h, &tail) == 2) &&
            w >= 320 && h >= 240 && w <= 7680 && h <= 4320) {
            sprintf(out, "%dx%d", w, h);
            return 1;
        }
        return 0;
    }
    case SETTING_PATH:
        if (strchr(value, '\n') || strchr(value, '\r')) return 0;
        return copy_value(out, value);
    case SETTING_KEY:
        return normalize_key(value, out);
    }
    return 0;
}

void settings_defaults(Settings *s)
{
    int i;
    for (i = 0; i < SETTINGS_COUNT; i++) copy_value(s->v[i], k_reg[i].def);
    s->extra[0] = 0;
    s->ignored[0] = 0;
}

int settings_set(Settings *s, int id, const char *value)
{
    char tmp[SETTINGS_VALUE_MAX];
    const SettingDef *d = settings_def(id);
    if (!d || !value || strlen(value) >= SETTINGS_VALUE_MAX) return 0;
    if (!normalize(d, value, tmp)) return 0;
    strcpy(s->v[id], tmp);
    return 1;
}

const char *settings_get(const Settings *s, int id)
{
    return (id >= 0 && id < SETTINGS_COUNT) ? s->v[id] : "";
}

int settings_get_bool(const Settings *s, int id)
{
    return settings_get(s, id)[0] == '1';
}

int settings_get_int(const Settings *s, int id)
{
    return atoi(settings_get(s, id));
}

int settings_choice_index(const Settings *s, int id)
{
    const SettingDef *d = settings_def(id);
    int i;
    if (!d || d->type != SETTING_CHOICE) return -1;
    for (i = 0; i < d->nchoices; i++)
        if (!strcmp(s->v[id], d->choices[i].token)) return i;
    return -1;
}

int settings_get_resolution(const Settings *s, int id, int *w, int *h)
{
    int a, b;
    if (sscanf(settings_get(s, id), "%dx%d", &a, &b) != 2) return 0;
    if (w) *w = a;
    if (h) *h = b;
    return 1;
}

int settings_is_default(const Settings *s, int id)
{
    const SettingDef *d = settings_def(id);
    return d && !strcmp(s->v[id], d->def);
}

void settings_apply_preset(Settings *s, int preset)
{
    int i;
    if (preset < 0 || preset >= SETTINGS_PRESET_COUNT) return;
    for (i = 0; i < SETTINGS_COUNT; i++)
        if (k_reg[i].preset[preset]) settings_set(s, i, k_reg[i].preset[preset]);
}

int settings_current_preset(const Settings *s)
{
    int p, i;
    for (p = 0; p < SETTINGS_PRESET_COUNT; p++) {
        int match = 1;
        for (i = 0; i < SETTINGS_COUNT && match; i++)
            if (k_reg[i].preset[p] && strcmp(s->v[i], k_reg[i].preset[p])) match = 0;
        if (match) return p;
    }
    return SETTINGS_PRESET_CUSTOM;
}

/* ── Reading ───────────────────────────────────────────────────────── */

static char *trim(char *a, char *b)    /* [a, b) without surrounding blanks, NUL at the end */
{
    while (a < b && (*a == ' ' || *a == '\t')) a++;
    while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r')) b--;
    *b = 0;
    return a;
}

/* One more ignored line in s->ignored ("Key, Key"; ASCII, "..." once full). */
static void note_ignored(Settings *s, const char *what)
{
    char *d = s->ignored;
    size_t n = strlen(d), k = strlen(what), i;
    if (n >= 3 && !strcmp(d + n - 3, "...")) return;
    if (k > 40) k = 40;
    if (n + 2 + k + 6 > sizeof s->ignored) {
        strcpy(d + n, n ? ", ..." : "...");
        return;
    }
    if (n) { memcpy(d + n, ", ", 2); n += 2; }
    for (i = 0; i < k; i++) d[n + i] = ((unsigned char)what[i] < 0x20 || (unsigned char)what[i] > 0x7E) ? '?' : what[i];
    d[n + k] = 0;
}

/* A Key=Value line this version does not know, kept for the next save:
 * "Section\x01Key=Value\n" in s->extra (dropped when it does not fit). */
static void keep_line(Settings *s, const char *section, const char *k, const char *v)
{
    size_t n = strlen(s->extra);
    if (!section[0] || !k[0] || strchr(section, '\x01')) return;
    if (n + strlen(section) + strlen(k) + strlen(v) + 3 >= sizeof s->extra) return;
    sprintf(s->extra + n, "%s\x01%s=%s\n", section, k, v);
}

/* ── The presets' revision ──
 * A launcher writes every shown line, so a file holds the values of the
 * preset its player had chosen, as that preset was then. [Meta] Presets=2
 * marks a file written since the presets below changed (the stable build:
 * Texture filtering 16x and Lightmap smoothing on, in Recommended and Steam
 * Deck). A file without it that matched a preset of its time is moved to that
 * preset as it is now, for the changed settings and the ones it did not have
 * yet (k_since_013) only (a Modern of the
 * development builds, Lightmap smoothing on, reads as Recommended); a file
 * that matched none keeps its values (presets_upgrade: the settings it did
 * not have yet, from its closest preset). Once written again it has the mark,
 * and a value chosen since stays. Presets=3: HD HUD (on in Recommended and
 * Steam Deck), which files of revision 2 do not have yet. Presets=4: Weather
 * (PS2 in Recommended and Steam Deck), which the 0.1.4 did not have, and a
 * lighter Steam Deck (Smoother terrain seams, Lightmap smoothing and Smoother
 * legs in turns off, on in the 0.1.4's: at 60 fps the legs' return to the
 * game's pose is a one-image jump; Recommended keeps the legs on): a file
 * that matched a preset takes that preset's values,
 * a file that matched none keeps its own and takes the Weather of the preset
 * it is closest to (k_since_014). */
#define SETTINGS_PRESETS_REV 4
static const struct { int id, rev; const char *was[SETTINGS_PRESET_COUNT]; } k_presets_rev2[] = {
    /* the revision that changed it; the values each preset gave before; "a|b" = either.
     * A setting changed twice has a line for each change, the oldest first: a file is
     * compared with the values of its own revision (preset_agrees). */
    { S_TEX_FILTER,      2, { "off", "off", "off" } },
    { S_LIGHTMAP_SMOOTH, 2, { "0", "0|1", "0" } },
    { S_HD_HUD,          3, { "0", "0", "0" } },
    { S_WEATHER,         4, { "original", "original", "original" } },
    { S_LIGHTMAP_SMOOTH, 4, { "0", "1", "1" } },
    { S_SMOOTH_SEAMS,    4, { "0", "1", "1" } },
    { S_SMOOTH_LEGS,     4, { "0", "1", "1" } },
};

/* The settings with a preset value that the 0.1.3 did not have: a file without
 * the mark (revision 1) has no line for them. Such a file is matched on the
 * lines it has, and the preset it matched gives them its own values (a 0.1.3
 * at Original Xbox stays Original Xbox: Snow look Original, Lightmap
 * smoothing, terrain seams and HD HUD off). A setting the 0.1.3 had and the
 * file lacks still counts at its default: a file with almost no lines is not
 * taken for a preset. */
static const int k_since_013[] = {
    S_RENDER_SCALE, S_HD_HUD, S_SHADOW_SOFTNESS, S_SNOW_LOOK, S_LIGHTMAP_SMOOTH,
    S_TC_GHOST, S_SMOOTH_SEAMS,
};

static int since_013(int id, int rev)
{
    int k;
    if (rev >= 2) return 0;
    for (k = 0; k < COUNT(k_since_013); k++)
        if (k_since_013[k] == id) return 1;
    return 0;
}

/* The same for the settings the 0.1.4 did not have (files up to revision 3):
 * a file that matched no preset takes them from the preset it is closest to
 * (Original Xbox at 144 fps: the original weather; Recommended at another
 * resolution: the PS2's). */
static const int k_since_014[] = { S_WEATHER };

static int since_014(int id, int rev)
{
    int k;
    if (rev >= 4) return 0;
    for (k = 0; k < COUNT(k_since_014); k++)
        if (k_since_014[k] == id) return 1;
    return 0;
}

static int in_list(const char *list, const char *v)
{
    size_t n = strlen(v);
    while (*list) {
        const char *bar = strchr(list, '|');
        size_t k = bar ? (size_t)(bar - list) : strlen(list);
        if (k == n && !strncmp(list, v, n)) return 1;
        list += k + (bar ? 1 : 0);
    }
    return 0;
}

/* Does setting i of the file agree with preset p? -1: not counted (a setting
 * the file's version did not have, or a changed one it does not have). */
static int preset_agrees(const Settings *s, const unsigned char *seen, int rev, int p, int i)
{
    int j, changed = -1;
    if (!k_reg[i].preset[p]) return -1;
    if (!seen[i] && since_013(i, rev)) return -1;           /* not in the file's version: the preset's */
    for (j = 0; j < COUNT(k_presets_rev2) && changed < 0; j++)   /* the first change after the file's revision */
        if (k_presets_rev2[j].id == i && k_presets_rev2[j].rev > rev) changed = j;
    if (changed < 0) return !strcmp(s->v[i], k_reg[i].preset[p]);
    if (!seen[i]) return -1;
    return in_list(k_presets_rev2[changed].was[p], s->v[i]);
}

/* A file of the 0.1.3 or the 0.1.4 that matched no preset (Custom) keeps its
 * values; the settings it did not have yet take those of the preset it is
 * closest to: the most settings with that preset's value, among those the
 * presets set (Original Xbox + 144 fps: the new ones off; Recommended at
 * another resolution: on). A tie goes to the first, Original Xbox, which
 * leaves the picture as it was. */
static void presets_upgrade(Settings *s, const unsigned char *seen, int rev)
{
    int p, i, j, best = -1, best_n = -1;
    for (p = 0; p < SETTINGS_PRESET_COUNT; p++) {
        int match = 1, n = 0;
        for (i = 0; i < SETTINGS_COUNT; i++) {
            int a = preset_agrees(s, seen, rev, p, i);
            if (a == 0) match = 0;
            else if (a > 0) n++;
        }
        if (match) {
            for (j = 0; j < COUNT(k_presets_rev2); j++)
                if (k_presets_rev2[j].rev > rev)
                    settings_set(s, k_presets_rev2[j].id, k_reg[k_presets_rev2[j].id].preset[p]);
            for (i = 0; i < SETTINGS_COUNT; i++)
                if (k_reg[i].preset[p] && !seen[i] && since_013(i, rev))
                    settings_set(s, i, k_reg[i].preset[p]);
            return;
        }
        if (n > best_n) { best_n = n; best = p; }
    }
    if (best >= 0)
        for (i = 0; i < SETTINGS_COUNT; i++)
            if (k_reg[i].preset[best] && !seen[i] && (since_013(i, rev) || since_014(i, rev)))
                settings_set(s, i, k_reg[i].preset[best]);
}

/* The settings of the extras (port/extras.cmake) that a build
 * without them leaves out: a file of a build with them is read without a
 * note, its lines kept as they are. */
static int extras_key(const char *section, const char *key)
{
    static const char *const graphics[] = {
        "DistantHaze", "LightsInHaze", "SpeedEffect", "Bloom", "Vignette", "SpeedCamera",
    };
    int i;
    if (ieq(section, "Controls")) return ieq(key, "StickDeadZone");
    if (!ieq(section, "Graphics")) return 0;
    for (i = 0; i < COUNT(graphics); i++)
        if (ieq(key, graphics[i])) return 1;
    return 0;
}

int settings_parse(Settings *s, const char *text, size_t len, int *bad)
{
    char section[64] = "", *buf, *p, *end;
    int version = 0, nbad = 0, line = 0, presets_rev = 1, hd_hud_only = 0;
    unsigned char seen[SETTINGS_COUNT];

    settings_defaults(s);
    if (bad) *bad = 0;
    buf = (char *)malloc(len + 1);
    if (!buf) return SETTINGS_LOAD_ERROR;
    memcpy(buf, text, len);
    buf[len] = 0;
    p = buf;
    end = buf + len;
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;                                     /* UTF-8 byte order mark */

    /* First pass: the version, so an old file is not read at all. */
    {
        char *q = p, sec[64] = "";
        while (q < end) {
            char *nl = memchr(q, '\n', (size_t)(end - q)), *line_end = nl ? nl : end, *l = q, *eq;
            q = nl ? nl + 1 : end;
            while (l < line_end && (*l == ' ' || *l == '\t')) l++;
            if (l < line_end && *l == '[') {
                char *rb = memchr(l, ']', (size_t)(line_end - l));
                size_t n = rb ? (size_t)(rb - l - 1) : 0;
                if (n >= sizeof sec) n = sizeof sec - 1;
                memcpy(sec, l + 1, n);
                sec[n] = 0;
            } else if (ieq(sec, "Meta") && l < line_end && *l != ';' && *l != '#' &&
                       (eq = memchr(l, '=', (size_t)(line_end - l))) != NULL) {
                size_t n = (size_t)(eq - l);
                while (n && (l[n - 1] == ' ' || l[n - 1] == '\t')) n--;
                if (n == 7) {
                    char k[8];
                    memcpy(k, l, 7);
                    k[7] = 0;
                    if (ieq(k, "Version")) version = atoi(eq + 1);   /* stops at the line's end */
                }
            }
        }
    }
    if (version < 1) { free(buf); return SETTINGS_LOAD_NOT_V1; }
    memset(seen, 0, sizeof seen);

    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p)), *line_end = nl ? nl : end, *l, *eq;
        char where[24];
        l = trim(p, line_end);
        p = nl ? nl + 1 : end;
        line++;
        sprintf(where, "line %d", line);
        if (!*l || *l == ';' || *l == '#') continue;
        if (*l == '[') {
            char *rb = strchr(l, ']');
            if (rb) {
                *rb = 0;
                snprintf(section, sizeof section, "%s", trim(l + 1, rb));
            } else {
                nbad++;
                note_ignored(s, where);
            }
            continue;
        }
        eq = strchr(l, '=');
        if (!eq) { nbad++; note_ignored(s, where); continue; }
        {
            char *k = trim(l, eq), *v = eq + 1;
            int id;
            while (*v == ' ' || *v == '\t') v++;
            if (ieq(section, "Meta")) {
                if (ieq(k, "Presets")) presets_rev = atoi(v);
                continue;
            }
            id = settings_find(section, k);
            if (id >= 0) seen[id] = 1;
            if (id == S_HD_TEXTURES && ieq(v, "hud")) hd_hud_only = 1;
            if (id < 0) {                           /* another version's setting: kept */
                if (!extras_key(section, k)) {      /* not one this build leaves out on purpose */
                    nbad++;
                    note_ignored(s, k[0] ? k : where);
                }
                keep_line(s, section, k, v);
            } else if (!settings_set(s, id, v)) {   /* not valid: the default */
                nbad++;
                note_ignored(s, k);
            }
        }
    }
    free(buf);
    if (presets_rev < SETTINGS_PRESETS_REV) presets_upgrade(s, seen, presets_rev);
    if (hd_hud_only && !seen[S_HD_HUD]) settings_set(s, S_HD_HUD, "1");   /* HdTextures=hud: the HUD in HD */
    if (bad) *bad = nbad;
    return version > SETTINGS_VERSION ? SETTINGS_LOAD_NEWER : SETTINGS_LOAD_OK;
}

/* ── Files (UTF-8 paths) ───────────────────────────────────────────── */

#ifdef _WIN32
static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (w && !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) { free(w); w = NULL; }
    return w;
}
static int narrow(const wchar_t *w, char *out, size_t out_sz)
{
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_sz, NULL, NULL) > 0;
}
#endif

static FILE *u8_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *w = widen(path), wm[8];
    FILE *f = NULL;
    int i;
    for (i = 0; mode[i] && i < 7; i++) wm[i] = (wchar_t)mode[i];
    wm[i] = 0;
    if (w) f = _wfopen(w, wm);
    free(w);
    return f;
#else
    return fopen(path, mode);
#endif
}

static int u8_remove(const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    int ok = w && DeleteFileW(w);
    free(w);
    return ok;
#else
    return remove(path) == 0;
#endif
}

/* Replace `to` by `from` in one step. */
static int u8_replace(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t *a = widen(from), *b = widen(to);
    int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a);
    free(b);
    return ok;
#else
    return rename(from, to) == 0;
#endif
}

static int u8_file_exists(const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

/* The folder and its parents. */
static int u8_make_dirs(const char *dir)
{
#ifdef _WIN32
    wchar_t *w = widen(dir);
    int r = w ? SHCreateDirectoryExW(NULL, w, NULL) : -1;
    free(w);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
#else
    char tmp[SETTINGS_VALUE_MAX], *q;
    if (strlen(dir) >= sizeof tmp) return 0;
    strcpy(tmp, dir);
    for (q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
#endif
}

int settings_load(Settings *s, const char *path, int *bad)
{
    FILE *f;
    char *text;
    long n;
    size_t got;
    int r;

    settings_defaults(s);
    if (bad) *bad = 0;
    if (!path || !u8_file_exists(path)) return SETTINGS_LOAD_MISSING;
    f = u8_fopen(path, "rb");
    if (!f) return SETTINGS_LOAD_ERROR;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > 4 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return SETTINGS_LOAD_ERROR;
    }
    text = (char *)malloc((size_t)n + 1);
    if (!text) { fclose(f); return SETTINGS_LOAD_ERROR; }
    got = fread(text, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(text); return SETTINGS_LOAD_ERROR; }
    r = settings_parse(s, text, got, bad);
    free(text);
    return r;
}

/* ── Writing ───────────────────────────────────────────────────────── */

typedef struct { char *p; size_t n, cap; } Out;

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void out_printf(Out *o, const char *fmt, ...)
{
    va_list ap;
    int k;
    va_start(ap, fmt);
    k = vsnprintf(o->n < o->cap ? o->p + o->n : NULL, o->n < o->cap ? o->cap - o->n : 0, fmt, ap);
    va_end(ap);
    if (k > 0) o->n += (size_t)k;
}

/* Section names, ignoring case: p[0..n) and q[0..m). */
static int same_name(const char *p, size_t n, const char *q, size_t m)
{
    size_t i;
    if (n != m) return 0;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)q[i])) return 0;
    return 1;
}

static int same_section(const char *p, size_t n, const char *sec)
{
    return same_name(p, n, sec, strlen(sec));
}

/* The kept lines of section `sec` (s->extra), under its header if none yet. */
static void out_extra(Out *o, const Settings *s, const char *sec, int *wrote_header)
{
    const char *p = s->extra;
    while (*p) {
        const char *sep = strchr(p, '\x01'), *nl = strchr(p, '\n');
        if (!sep || !nl || sep > nl) break;
        if (same_section(p, (size_t)(sep - p), sec)) {
            if (!*wrote_header) { out_printf(o, "\n[%s]\n", sec); *wrote_header = 1; }
            out_printf(o, "%.*s\n", (int)(nl - sep - 1), sep + 1);
        }
        p = nl + 1;
    }
}

/* The comment of one value list: "a, b or c". */
static void out_values(Out *o, const SettingDef *d)
{
    int i;
    switch (d->type) {
    case SETTING_BOOL:       out_printf(o, "1 (on) or 0 (off)"); break;
    case SETTING_INT:        out_printf(o, "%d to %d", d->min, d->max); break;
    case SETTING_RESOLUTION: out_printf(o, "auto or WIDTHxHEIGHT"); break;
    case SETTING_PATH:       out_printf(o, "a path, or empty"); break;
    case SETTING_KEY:        out_printf(o, "a key name, or None"); break;
    case SETTING_CHOICE:
        for (i = 0; i < d->nchoices; i++)
            out_printf(o, "%s%s", i == 0 ? "" : i == d->nchoices - 1 ? " or " : ", ", d->choices[i].token);
        break;
    }
}

size_t settings_format(const Settings *s, char *out, size_t out_sz)
{
    Out o = { out, 0, out_sz };
    int i, j;
    const char *done[24];
    int ndone = 0;

    if (out_sz) out[0] = 0;
    out_printf(&o,
        "; OpenTricky -- SSX Tricky settings, written by the launcher (OpenTricky.exe)\n"
        "; and read by the game (SSX Tricky.exe) at start.\n"
        "; Safe to edit by hand while both are closed. A line that is missing or not\n"
        "; valid takes its default. \"PC\" = not in the original game.\n"
        "\n"
        "[Meta]\n"
        "Version=%d\n"
        "Presets=%d\n", SETTINGS_VERSION, SETTINGS_PRESETS_REV);

    /* Sections in the order they first appear in the registry. */
    for (i = 0; i < SETTINGS_COUNT; i++) {
        const char *sec = k_reg[i].section;
        int seen = 0, wrote_header = 0;
        for (j = 0; j < ndone; j++) if (!strcmp(done[j], sec)) seen = 1;
        if (seen || ndone >= COUNT(done)) continue;
        done[ndone++] = sec;
        for (j = i; j < SETTINGS_COUNT; j++) {
            const SettingDef *d = &k_reg[j];
            if (strcmp(d->section, sec)) continue;
            if ((d->flags & (SETTING_HIDDEN | SETTING_PENDING)) && !strcmp(s->v[j], d->def)) continue;
            if (!wrote_header) {
                out_printf(&o, "\n[%s]\n", sec);
                if (!strcmp(sec, "Keyboard"))
                    out_printf(&o, "; One key per control, named after the PS2 button in its place: a letter\n"
                                   "; or digit, F1-F24, Space, Enter, Escape, Tab, Up, Down, Left, Right,\n"
                                   "; LeftShift, LeftCtrl, Num0-Num9 ... or None.\n");
                else if (!strcmp(sec, "Controller"))
                    out_printf(&o, "; The controller input for each PS2 button: A B X Y LB RB LT RT Start Back\n"
                                   "; LS RS DpadUp DpadDown DpadLeft DpadRight, or None.\n");
                wrote_header = 1;
            }
            if (d->help) {
                out_printf(&o, "; %s%s: %s\n; ", d->label, (d->flags & SETTING_PC) ? " (PC)" : "", d->help);
                out_values(&o, d);
                out_printf(&o, ". Default: %s.\n", d->def[0] ? d->def : "empty");
            }
            out_printf(&o, "%s=%s\n", d->key, s->v[j]);
        }
        out_extra(&o, s, sec, &wrote_header);
    }
    /* Kept lines of the sections this version does not have, in the order read. */
    {
        const char *p = s->extra;
        while (*p) {
            const char *sep = strchr(p, '\x01'), *nl = strchr(p, '\n'), *q;
            size_t n;
            int done_already = 0;
            char sec[64];
            if (!sep || !nl || sep > nl) break;
            n = (size_t)(sep - p);
            for (i = 0; i < SETTINGS_COUNT && !done_already; i++)
                done_already = same_section(p, n, k_reg[i].section);
            for (q = s->extra; q < p && !done_already; q = strchr(q, '\n') + 1) {
                const char *qs = strchr(q, '\x01');      /* a section an earlier line wrote */
                done_already = qs && same_name(q, (size_t)(qs - q), p, n);
            }
            if (!done_already && n < sizeof sec) {
                int wrote_header = 0;
                memcpy(sec, p, n);
                sec[n] = 0;
                out_extra(&o, s, sec, &wrote_header);
            }
            p = nl + 1;
        }
    }
    return o.n;
}

int settings_save(const Settings *s, const char *path)
{
    size_t need = settings_format(s, NULL, 0), len;
    char *text, *tmp;
    FILE *f;
    int ok;

    text = (char *)malloc(need + 1);
    tmp = (char *)malloc(strlen(path) + 8);
    if (!text || !tmp) { free(text); free(tmp); return 0; }
    len = settings_format(s, text, need + 1);
    sprintf(tmp, "%s.new", path);
    f = u8_fopen(tmp, "w");             /* text mode: CRLF on Windows */
    ok = f != NULL;
    if (f) {
        ok = fwrite(text, 1, len, f) == len && fflush(f) == 0;
#ifdef _WIN32
        /* On the disk before it replaces the old file: a power cut then
         * leaves one or the other, never an empty settings.ini. */
        if (ok) FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f)));     /* best effort (a share may refuse it) */
#endif
        if (fclose(f) != 0) ok = 0;
    }
    if (ok) ok = u8_replace(tmp, path);
    if (!ok) u8_remove(tmp);
    free(text);
    free(tmp);
    return ok;
}

/* ── Where the file is ─────────────────────────────────────────────── */

int settings_game_dir(char *out, size_t out_sz)
{
#ifdef _WIN32
    wchar_t w[4096], *slash;
    DWORD k = GetModuleFileNameW(NULL, w, (DWORD)(sizeof w / sizeof w[0]));
    if (k == 0 || k >= sizeof w / sizeof w[0]) return 0;
    slash = wcsrchr(w, L'\\');
    if (slash) *slash = 0;
    return narrow(w, out, out_sz);
#else
    (void)out; (void)out_sz;
    return 0;
#endif
}

int settings_docs_dir(char *out, size_t out_sz)
{
#ifdef _WIN32
    PWSTR docs = NULL;
    char base[SETTINGS_VALUE_MAX];
    int ok;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &docs)) || !docs) return 0;
    ok = narrow(docs, base, sizeof base);
    CoTaskMemFree(docs);
    if (!ok) return 0;
    return snprintf(out, out_sz, "%s\\My Games\\SSX Tricky", base) < (int)out_sz;
#else
    const char *home = getenv("XDG_CONFIG_HOME");
    if (home && home[0]) return snprintf(out, out_sz, "%s/OpenTricky", home) < (int)out_sz;
    home = getenv("HOME");
    if (!home || !home[0]) return 0;
    return snprintf(out, out_sz, "%s/.config/OpenTricky", home) < (int)out_sz;
#endif
}

static int join(char *out, size_t out_sz, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    if (n && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
        return snprintf(out, out_sz, "%s%s", dir, name) < (int)out_sz;
    return snprintf(out, out_sz, "%s%c%s", dir, SEP, name) < (int)out_sz;
}

int settings_install_root_of(const char *exe_dir, char *out, size_t out_sz)
{
    char dir[SETTINGS_VALUE_MAX], parent[SETTINGS_VALUE_MAX], f[SETTINGS_VALUE_MAX + 32], *last;
    size_t n;

    if (!exe_dir || !exe_dir[0] || snprintf(dir, sizeof dir, "%s", exe_dir) >= (int)sizeof dir) return 0;
    n = strlen(dir);
    while (n > 1 && (dir[n - 1] == '\\' || dir[n - 1] == '/') && dir[n - 2] != ':') dir[--n] = 0;
    last = dir + n;
    while (last > dir && last[-1] != '\\' && last[-1] != '/') last--;
    /* bin\ without OpenTricky.exe, below a folder with it: that folder */
    if (last > dir && ieq(last, "bin") && join(f, sizeof f, dir, SETTINGS_LAUNCHER_EXE) && !u8_file_exists(f)) {
        size_t p = (size_t)(last - dir) - 1;            /* the separator before "bin" */
        memcpy(parent, dir, p);
        parent[p] = 0;
        if (p && parent[p - 1] == ':') { parent[p] = SEP; parent[p + 1] = 0; }   /* "D:" -> "D:\" */
        if (p && join(f, sizeof f, parent, SETTINGS_LAUNCHER_EXE) && u8_file_exists(f))
            return snprintf(out, out_sz, "%s", parent) < (int)out_sz;
    }
    return snprintf(out, out_sz, "%s", dir) < (int)out_sz;
}

int settings_install_root(char *out, size_t out_sz)
{
    char exe[SETTINGS_VALUE_MAX];
    return settings_game_dir(exe, sizeof exe) && settings_install_root_of(exe, out, out_sz);
}

int settings_instance_name(const char *what, char *out, size_t out_sz)
{
    char root[SETTINGS_VALUE_MAX];
    unsigned long long h = 14695981039346656037ull;      /* FNV-1a, 64 bits */
    const char *p;
    if (!settings_install_root(root, sizeof root)) return 0;
    for (p = root; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');   /* Windows folder names: case does not count */
        h = (h ^ c) * 1099511628211ull;
    }
    return snprintf(out, out_sz, "Local\\OpenTricky-%s-%08x%08x", what, (unsigned)(h >> 32), (unsigned)h) < (int)out_sz;
}

/* Can a settings file be written in `dir`? An existing file must open for
 * writing (not read-only); otherwise a test file is created and removed. */
static int dir_writable(const char *dir, const char *file)
{
    char probe[SETTINGS_VALUE_MAX + 32];
    FILE *f;
    if (u8_file_exists(file)) {
        f = u8_fopen(file, "r+b");
        if (!f) return 0;
        fclose(f);
    }
    if (!join(probe, sizeof probe, dir, "settings.ini.probe")) return 0;
    f = u8_fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    u8_remove(probe);
    return 1;
}

int settings_locate(const char *game_dir, const char *docs_dir, int for_write, char *out, size_t out_sz)
{
    char g[SETTINGS_VALUE_MAX], d[SETTINGS_VALUE_MAX], gf[SETTINGS_VALUE_MAX + 16], df[SETTINGS_VALUE_MAX + 16];
    int have_g, have_d;

    have_g = game_dir ? snprintf(g, sizeof g, "%s", game_dir) < (int)sizeof g : settings_install_root(g, sizeof g);
    have_d = docs_dir ? snprintf(d, sizeof d, "%s", docs_dir) < (int)sizeof d : settings_docs_dir(d, sizeof d);
    have_g = have_g && g[0] && join(gf, sizeof gf, g, SETTINGS_FILE_NAME);
    have_d = have_d && d[0] && join(df, sizeof df, d, SETTINGS_FILE_NAME);
    if (out_sz) out[0] = 0;

    if (!for_write) {
        if (have_g && u8_file_exists(gf)) return snprintf(out, out_sz, "%s", gf), SETTINGS_AT_GAME_FOLDER;
        if (have_d && u8_file_exists(df)) return snprintf(out, out_sz, "%s", df), SETTINGS_AT_DOCUMENTS;
        if (have_g) return snprintf(out, out_sz, "%s", gf), SETTINGS_AT_GAME_FOLDER;
        if (have_d) return snprintf(out, out_sz, "%s", df), SETTINGS_AT_DOCUMENTS;
        return SETTINGS_AT_NONE;
    }
    if (have_g && dir_writable(g, gf)) return snprintf(out, out_sz, "%s", gf), SETTINGS_AT_GAME_FOLDER;
    if (have_d && u8_make_dirs(d) && dir_writable(d, df))
        return snprintf(out, out_sz, "%s", df), SETTINGS_AT_DOCUMENTS;
    return SETTINGS_AT_NONE;
}

/* ── The first start on a Steam Deck ───────────────────────────────── */

/* The first line of a small text file in `dir`, without surrounding blanks;
 * "" when it cannot be read. */
static void read_first_line(const char *dir, const char *name, char *out, size_t out_sz)
{
    char path[SETTINGS_VALUE_MAX + 64], *a, *b;
    FILE *f;
    size_t n;

    out[0] = 0;
    if (!join(path, sizeof path, dir, name) || !(f = u8_fopen(path, "rb"))) return;
    n = fread(out, 1, out_sz - 1, f);
    fclose(f);
    out[n] = 0;
    out[strcspn(out, "\r\n")] = 0;
    for (a = out; *a == ' ' || *a == '\t'; a++) {}
    for (b = a + strlen(a); b > a && (b[-1] == ' ' || b[-1] == '\t'); b--) {}
    *b = 0;
    memmove(out, a, (size_t)(b - a) + 1);
}

void settings_platform_probe(SettingsPlatform *p, const char *root)
{
    const char *test = getenv("XBOX_TEST_DECK_ROOT"), *deck = getenv("SteamDeck");
    char dmi[SETTINGS_VALUE_MAX];

    memset(p, 0, sizeof *p);
#ifdef _WIN32
    {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        p->wine = nt && GetProcAddress(nt, "wine_get_version") != NULL;
    }
#endif
    if (test && test[0]) {                      /* test only: Wine, and this folder as its root */
        p->wine = 1;
        if (!root) root = test;
    }
    if (deck) snprintf(p->steamdeck, sizeof p->steamdeck, "%s", deck);
    /* Never Z:\ on Windows itself: there it may be any drive, even a network one. */
#ifdef _WIN32
    if (!root && p->wine) root = "Z:\\";
    if (!root || !join(dmi, sizeof dmi, root, "sys\\class\\dmi\\id")) return;
#else
    if (!root && p->wine) root = "/";
    if (!root || !join(dmi, sizeof dmi, root, "sys/class/dmi/id")) return;
#endif
    read_first_line(dmi, "sys_vendor", p->vendor, sizeof p->vendor);
    if (!p->vendor[0]) read_first_line(dmi, "board_vendor", p->vendor, sizeof p->vendor);
    read_first_line(dmi, "product_name", p->product, sizeof p->product);
    if (!p->product[0]) read_first_line(dmi, "board_name", p->product, sizeof p->product);
}

int settings_first_start_preset(const SettingsPlatform *p, char *why, size_t why_sz)
{
    int deck = 0;
    char tmp[200];

    if (!p->wine) {
        snprintf(tmp, sizeof tmp, "not under Wine");
    } else if (!strcmp(p->steamdeck, "0")) {
        snprintf(tmp, sizeof tmp, "SteamDeck=0");
    } else if (p->vendor[0]) {
        deck = ieq(p->vendor, "Valve") && (ieq(p->product, "Jupiter") || ieq(p->product, "Galileo"));
        snprintf(tmp, sizeof tmp, "DMI \"%s\" \"%s\"%s", p->vendor, p->product, deck ? "" : ": not a Steam Deck");
    } else if (!strcmp(p->steamdeck, "1")) {
        deck = 1;
        snprintf(tmp, sizeof tmp, "SteamDeck=1, no DMI");
    } else {
        snprintf(tmp, sizeof tmp, "no DMI, no SteamDeck=1");
    }
    if (why && why_sz) snprintf(why, why_sz, "%s", tmp);
    return deck ? SETTINGS_PRESET_STEAM_DECK : -1;
}
