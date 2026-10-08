/*
 * launcher.h -- the game's settings file. The launcher itself is
 * OpenTricky.exe, a separate program.
 *
 * The settings live in settings.ini, the file
 * of the settings registry (settings.h) that the separate launcher
 * (OpenTricky.exe) writes: beside the executables, or in
 * Documents\My Games\SSX Tricky when that folder cannot be written. The old
 * "SSX Tricky.ini" is not read. LauncherConfig is the game's view of it.
 */
#ifndef SSX_LAUNCHER_H
#define SSX_LAUNCHER_H

#include <windows.h>
#include <stdint.h>

#include "controls.h"

typedef struct LauncherConfig {
    char iso[MAX_PATH];   /* disc image; empty until the player chooses one */
    char hdd[MAX_PATH];   /* emulated hard disk folder: relative to the exe, or absolute;
                           * empty = automatic (launcher_save_kind) */
    int  width, height;   /* render resolution */
    int  aspect;          /* LAUNCHER_ASPECT_*: 4:3, 16:9 (the game's own widescreen
                           * mode), 21:9, 32:9, Auto (the monitor's shape) */
    int  wide_fov;        /* ASPECT_FOV_* (aspect.h): field of view wider than 16:9 */
    int  hud_shape;       /* HUD_SHAPE_* (hud_anchor.h): race HUD in proportion or like the Xbox */
    int  hud_size;        /* index of the race HUD size (hud_anchor.h: 100, 90, 85, 80, 70 %) */
    int  fullscreen;      /* borderless; Alt+Enter toggles in game */
    int  aniso;           /* 0 = the game's own texture filtering, or 2/4/8/16 */
    int  msaa;            /* anti-aliasing samples: 1 (off), 2, 4, 8 */
    int  show_fps;        /* frame rate in the title bar */
    int  log_file;        /* write "<exe name>.log" for bug reports */
    ControlMap controls;  /* keyboard and controller bindings */
    /* The fork's own options ([Fork] in the .ini). Applied by
     * launcher_fork_apply() as the XBOX_* variables the code already reads;
     * a variable already set in the environment wins. */
    int  smaa;            /* 0 off, 1..4 = low, medium, high, ultra (XBOX_SMAA, _PRESET) */
    int  fidelity;        /* 1 = the Xbox fidelity fixes below (default); 0 = all off */
    int  fix_cullwind;    /* XBOX_FIX_CULLWIND, .ini only, default 1 */
    int  fix_occlusion;   /* XBOX_FIX_OCCLUSION */
    int  fix_texpassthru; /* XBOX_FIX_TEXPASSTHRU */
    int  fix_gamma;       /* XBOX_FIX_GAMMA */
    int  audio_queue;     /* XBOX_AUDIO_QUEUE: 2..20 buffers of 5.33 ms, default 8 */
    int  audio_out;       /* XBOX_AUDIO_OUTPUT: 0 auto (5.1 when the device has the speakers,
                           * default), 1 stereo, 2 5.1 */
    int  fix_kickwait;    /* XBOX_FIX_KICKWAIT: 1 = no wait on the WBC flush (default) */
    int  fps_cap;         /* XBOX_FPS_CAP: 60 (default), 120, 144, 240, -1 = unlimited,
                           * -2 = the monitor's refresh rate (XBOX_FPS_CAP=monitor) */
    int  sync;            /* XBOX_SYNC: 0 legacy (.ini only), 1 off, 2 vsync, 3 adaptive */
    int  soft_shadows;    /* XBOX_SOFT_SHADOWS: 0 = hard, as on Xbox (default), 1 = soft edges */
    int  menu_sounds;     /* the game's menu sounds in the launcher: 0 off, 1 low (default), 2 medium
                           * ([Launcher] MenuSounds; an old 1 = "on" becomes low) */
    int  launcher_music;  /* the game's title music, quietly, in the launcher ([Launcher] LauncherMusic, 1) */
    int  draw_dist;       /* XBOX_DRAW_DISTANCE: 0 Original (default), 1 Far, 2 Max */
    int  hd_textures;     /* XBOX_HD_TEXTURES: 0 = the game's textures (default), 1 = the pack's */
    int  hd_menus;        /* XBOX_HD_TEXTURES_MENUS: 1 = menu pictures too (.ini only, default 0) */
    char hd_path[MAX_PATH]; /* the player's HD texture pack folder (absolute); empty = none chosen */
    int  menus;           /* XBOX_WIDE_MENUS: ASPECT_MENUS_169 or _43, default ASPECT_MENUS_DEFAULT;
                           * applied by aspect_set_menus() before aspect_init, not by launcher_fork_apply */
    int  btn_icons;       /* XBOX_BUTTON_ICONS: 0 Auto (default: Xbox modern until the pad
                           * type is detected), 1 Xbox modern, 2 PlayStation modern,
                           * 3 PS2 original (in a build without it: not offered, a saved one is Auto) */
    int  save_backup;     /* XBOX_SAVE_BACKUP: 1 = a copy of the previous save is kept (default) */
    int  smooth_motion;   /* XBOX_FPS_INTERP: 1 = frames in between interpolated above 60 (default);
                           * a hidden setting, for troubleshooting */
} LauncherConfig;

/* The XBE entry point this executable was recompiled from. A disc image is
 * accepted only if its default.xbe has the same one: the translated code is
 * for exactly one build, and any other would crash rather than run. */
void launcher_init(uint32_t expected_entry_point);

/* Per-monitor DPI awareness, so windows are sized in real pixels. Call once,
 * before any window is created. */
void launcher_enable_dpi_awareness(void);

/* settings.ini as read at start (settings_locate), as an ANSI path. */
void launcher_config_path(char *out, size_t out_sz);
void launcher_log_path(char *out, size_t out_sz);     /* "<exe name>.log" beside settings.ini */
/* Resolve the save folder setting to an absolute path (an empty setting is
 * automatic -- see launcher_save_kind). */
void launcher_hdd_path(const LauncherConfig *cfg, char *out, size_t out_sz);
/* Which save folder applies. */
enum { LAUNCHER_SAVES_CHOSEN,      /* SaveFolder set in the .ini */
       LAUNCHER_SAVES_OLD_HDD,     /* hdd\ beside the game, with saves from an earlier version */
       LAUNCHER_SAVES_PORTABLE,    /* Saves\ beside the game (portable.txt) */
       LAUNCHER_SAVES_DOCUMENTS }; /* Documents\My Games\SSX Tricky\Saves */
int launcher_save_kind(const LauncherConfig *cfg);
/* Open a folder (created if missing) or a file in Explorer / its program. */
void launcher_open_path(HWND owner, const char *path, BOOL folder);
/* Display shapes. 21:9, 32:9 and Auto show the frame at the
 * resolution's own shape (3440x1440 = 2.389), with the game's widescreen
 * mode widened to it (aspect.h); 4:3 and 16:9 are the two original ones. */
enum { LAUNCHER_ASPECT_43, LAUNCHER_ASPECT_169, LAUNCHER_ASPECT_219,
       LAUNCHER_ASPECT_329, LAUNCHER_ASPECT_AUTO, LAUNCHER_ASPECT_COUNT };
const char *launcher_aspect_name(int aspect);       /* "4:3", ..., "Auto" (the .ini value) */
/* Presentation (LauncherConfig.sync / XBOX_SYNC): default of the launcher
 * and of a new .ini. Without the launcher (no XBOX_SYNC) the code keeps legacy. */
enum { LAUNCHER_SYNC_LEGACY, LAUNCHER_SYNC_OFF, LAUNCHER_SYNC_VSYNC, LAUNCHER_SYNC_ADAPTIVE };
#define LAUNCHER_SYNC_DEFAULT LAUNCHER_SYNC_VSYNC
double launcher_display_aspect(const LauncherConfig *cfg);  /* width / height shown */

void launcher_config_load(LauncherConfig *cfg);     /* the registry's defaults when missing or old */
/* Write settings.ini (beside the game, else in Documents); FALSE if neither can be written. */
BOOL launcher_config_save(const LauncherConfig *cfg);

/* Read only the fork options (the ones launcher_fork_apply sets)
 * of the settings.ini `ini` into cfg: the code's own defaults when there is
 * no such file or it is not a settings.ini v1. For main.c, XBOX_FORK_INI. */
void launcher_fork_load(LauncherConfig *cfg, const char *ini);
/* Set the XBOX_* variables for cfg's fork options, each one only
 * if the environment does not already have it, then print one line with the
 * effective values and where each came from. Call before anything reads them
 * (pass_tags_init, the first frame, the audio and kernel set-up).
 * source == NULL sets nothing and only reports the environment (test runs). */
void launcher_fork_apply(const LauncherConfig *cfg, const char *source);

/* TRUE if `path` is a disc image of the build this executable runs. On
 * failure `why` holds one sentence for the player. */
BOOL launcher_check_iso(const char *path, char *why, size_t why_sz);

#endif /* SSX_LAUNCHER_H */
