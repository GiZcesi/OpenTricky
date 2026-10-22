# macOS launcher

`bash macos/launcher/make_app.sh` builds `dist/SSX Tricky.app` around the GPU build (`../build/gpu/SSX Tricky`; pass another binary and output folder as arguments).

The app is a thin wrapper (`ssx-launcher.sh`):

- asks for your Xbox disc image with a file dialog the first time, and remembers it in `~/Library/Application Support/OpenTricky/disc-image.txt`;
  change it later with `open -a "SSX Tricky" --args --choose-iso`;
- runs the game from `~/Library/Application Support/OpenTricky`, so `settings.ini`, the save folder `hdd`, and `crash.txt` live there (the game keeps its files next to the program it was started as; the wrapper starts it through a link in that folder);
- keeps the last run's console output in `last-run.log` (`last-run.log.prev` for the run before);
- shows a dialog if Homebrew libraries are missing (`brew install sdl3 sdl2-compat openssl@4`) or the game ends abnormally.

Not covered: the game binary still links the Homebrew copies of SDL3, SDL2-compat and OpenSSL (not bundled, so the app is not self-contained and cannot be given to someone without them); it is ad hoc signed only (a copy downloaded from elsewhere would need Gatekeeper approval); the file-dialog, missing-library and crash dialogs were not exercised (they need a person at the screen).

## Display settings (widescreen, resolution, full screen, screenshots)

By default the game is told the shape of your desktop (4:3 below 1.5, 16:9 up to 1.95, wider shapes as they are, e.g. 21:9) and the GPU draws at 1080 rows (960 at 4:3); the window starts at that shape.
Change it with a file `~/Library/Application Support/OpenTricky/display.env` (read by the app, or export the same names in Terminal):

    OT_SHAPE=21:9        # auto (default) | 4:3 | 16:9 | 21:9 | 32:9 | a number such as 2.39
    OT_RENDER_H=1440     # rows the GPU draws (480 or more; the width follows the shape)
    OT_FULLSCREEN=1      # start in full screen
    OT_BUTTONS=playstation   # button pictures in menus and HUD: modern | playstation | ps2 (default: the game's own Xbox glyphs)
    OT_HD_TEXTURES=/path/to/pack   # your HD texture pack folder (DDS files, not included); OT_HD_MENUS=1: menus too
    OT_HDR=on            # HDR / wide-gamut output (see below); tune with OT_HDR_GAMUT (0..1), OT_HDR_WARM (0..0.5, default 0.1), OT_HDR_BOOST (0..1, default 0.35), OT_HDR_PEAK (headroom used, default 1.5)
    OT_HDR_SCENE=0.45    # share of the HDR gamut expansion the 3D picture gets (1 = like the HUD and menus)
    OT_SMAA=high         # smoother edges (SMAA) on the 3D: off (default) | low | medium | high | ultra; HUD and menus stay sharp
    OT_SOFT_SHADOWS=on   # soft shadow edges (default off)
    OT_FPS=120           # frame rate limit: 60 (default) | 120 | 144 | 240 | a number | 0 = no limit | monitor
    OT_VSYNC=vsync       # vsync | adaptive | off (default: vsync at 60, present at once above 60)

In the game window: **F11** or **Alt+Enter** toggles full screen, **F12** saves a PNG of the game picture at the render size to `~/Pictures/OpenTricky/`.
The shape is fixed at start (the game is told its screen shape then), so changing `OT_SHAPE` needs a restart. Menus keep the game's own 16:9 frame on wider screens; movies keep 4:3.

Frame rates above 60: the game keeps its logic at 60 Hz and draws extra frames in between, interpolating the camera and riders (port/src/fps_cap.c). Set `OT_FPS`; extra frames are only visible if your display refreshes faster than 60 Hz (on a 60 Hz display the frames are produced and presented but cannot all be shown; `OT_VSYNC=vsync` then caps the rate to the display). Tearing is possible with `off` or above 60 in full screen.

HDR and wide gamut (`OT_HDR=on`): the game's picture is SDR, made for a TV. On a wide-gamut HDR display the app asks for an HDR (extended linear) window and runs a colour pass (`xboxrecomp/src/d3d/d3d8_gpu_out.c`) that reads the colours as Display P3 instead of sRGB (saturated oranges, yellows and reds reach what the panel can show; greys and white stay put), adds a little more chroma to oranges and yellows (`OT_HDR_WARM`), and lifts bright, saturated highlights (fire, lamps, the game's gold) towards the display's HDR headroom, which macOS reports per window and changes with the display's brightness (1.2 on the development machine, so the lift is small there). The 3D picture gets only a share of the gamut expansion (`OT_HDR_SCENE`, default 0.45: photographic colours look oversaturated at full strength, flat graphics do not): at the frame-end marker the 3D is moved part of the way back before the final pass, so the HUD and menus keep the full expansion. Without an HDR display the option is ignored. `macos/hdr_test/hdr_test.c` checks the colour maths on known colours.

Button pictures (`OT_BUTTONS`): the game's button prompts are redrawn as an Xbox Series pad, a PlayStation pad or the PS2 style, from the CC0 drawings in `port/assets/btnicons/` (copied into the app). HD textures (`OT_HD_TEXTURES`): point it at your copy of the community pack (a folder of `.dds` files, or the folder above its `replacements` folder; not included): the game's textures are looked up by hash in `port/assets/hdtex_index.txt` and replaced when the pack has them. Both are off by default.

## Settings window and iCloud storage

`ot-tool.swift` (built into the app as `Contents/Resources/ot-tool` by `make_app.sh`) adds two things:

- **Settings window** shown before the game starts: screen shape, render resolution, full screen, frame-rate limit, VSync, SMAA, soft shadows,
  HDR (with tuning), button pictures, HD texture pack, disc image, iCloud on/off. "Save and Play" starts the game, "Quit" does not.
  It can be switched off in the window; hold Option while opening the app, or `open -a "SSX Tricky" --args --settings`, to get it back.
  It edits `display.env` in place (comments and unknown keys are kept; a setting left at the game's default is removed from the file).
- **iCloud mirror** of `display.env` and the game saves (`hdd/`). The working copy is always local
  (`~/Library/Application Support/OpenTricky`), so the game never waits for the network. `ot-tool sync pull` runs before the window/game,
  `sync push` after the game ends (also after a crash or quit). Destination: iCloud Drive › OpenTricky
  (`~/Library/Mobile Documents/com~apple~CloudDocs/OpenTricky`, no entitlement needed). Newer file wins (2 s tolerance); a local file that a pull
  replaces is first copied to `sync-backups/<time>/`; nothing is deleted by a sync. Without iCloud Drive, or with the switch off, it stays local.
  Not synced (machine-specific): `disc-image.txt`, `launcher-prefs.txt`, logs. Log of syncs: `sync.log`.
- Tests: `OT_TEST_HOME=<dir>` points the tool at a fake home (`ot-tool sync push|pull`, `ot-tool selftest`).

In the game: F11 / Alt+Enter full screen; Cmd+1..4 window size, F9 / F10 smaller / larger (OT_WINDOW_H sets the opening height);
Cmd+, or F2, or the controller's Guide / PlayStation button (or Back+Start, i.e. Share+Options, together) open the settings window.
The macOS menu bar has **SSX Tricky › Settings… (Cmd+,)** and a **Game** menu (full screen, window sizes, screenshot, folder) -- port/src/ot_menu_macos.m.
Not done: a "Settings" entry inside the game's own 3D menus (that needs a patch to the recompiled menu code; see MACOS_PORT_PLAN.md).

The mouse pointer hides after 2 seconds without movement and returns when the mouse moves (OT_CURSOR_HIDE_MS: 0 = never; also in the settings window).

## Release build (bundled, signed, notarized)

    SIGN_ID=auto RELEASE=1 bash macos/launcher/make_app.sh                       # signed (Developer ID, hardened runtime) app + dmg
    NOTARY_PROFILE=opentricky SIGN_ID=auto RELEASE=1 bash macos/launcher/make_app.sh   # ... and submitted to Apple, stapled

The app carries SDL3, sdl2-compat and OpenSSL in Contents/Frameworks (no Homebrew needed), the main executable is a signed stub (`stub.c`)
that starts `Resources/launcher.sh`, and the game keeps its files in ~/Library/Application Support/OpenTricky (`OT_HOME_DIR`).
One-time setup for notarization: `xcrun notarytool store-credentials opentricky --apple-id <your id> --team-id <TEAMID> --password <app-specific password>`.

## Controls, save history, conflicts, diagnostics (settings window)
- **Controls**: keyboard keys and controller buttons for each PlayStation-named button, written to `settings.ini` ([Keyboard], [Controller]); the app applies them at start.
- **Save history**: after each game that changed a save, a snapshot goes to `save-history/` (the last 20); the window restores one (what it replaces is kept in `sync-backups/`).
- **Conflicts**: a setting or save changed on two Macs since the last sync keeps the newer one, keeps the other in `sync-backups/`, and the next start says so once.
- **Copy diagnostics**: version, system, settings, last crash report and the end of the log, on the clipboard for a bug report.
- Command line: `ot-tool restore <snapshot>`, `ot-tool diagnostics`.
