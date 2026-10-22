## macOS (Apple Silicon) port

A native macOS build of OpenTricky: SDL3 window, a Metal renderer (SDL's GPU API with generated MSL), SDL3 audio, keyboard and controller input (with rumble), a self-contained app bundle (signed with a Developer ID and notarized by the person who built it), a settings window, and iCloud Drive storage for settings and saves (local folder is always the working copy).

This is a **large** change, opened as a **draft** because CONTRIBUTING asks for one change per pull request. It is easy to split if you prefer; a natural split is:
1. shared-code fixes found while porting (below),
2. the macOS platform layer (`port/src/ot_window.c`, `ot_menu_macos.m`, `xboxrecomp/src/platform/*` POSIX parts, `kernel_*` POSIX parts),
3. the Metal backend (`xboxrecomp/src/d3d/d3d8_gpu*.c`, `nv2a_psh.c` MSL mode),
4. `macos/` (app bundling, settings window, tests).

### Fidelity
With every option at its default the picture is the original's: the final pass is the plain bilinear blit, no anisotropic filtering beyond what the title asks, no SMAA, no soft shadows, no HDR. The extras are options: `OT_UPSCALE=edge|sharp`, `OT_ANISO`, `OT_SMAA`, `OT_SOFT_SHADOWS`, `OT_HDR`, `OT_FPS` (high frame rates with in-between frames), `OT_SHAPE` (widescreen), and the pacing options below. The macOS app's settings window edits them.

### Pacing options (off by default)
From an SSD a track loads in under a second (measured 0.7-1 s) where the console took many, so the animated loading screen flashes by, and the 3-2-1 feels short.
`OT_MIN_LOAD_MS=3000` holds `InGameState_LoadLevel` to at least that long; `OT_COUNTDOWN_SLOW=1.5` runs the title's clocks (interrupt time, performance counter, KeTickCount)
at 1/1.5 speed while race state 3 lasts (measured 2.5 s -> 3.7 s). Defaults are 0 and 1: the original behaviour. `macos/tests/pacing_check.py` checks both against the real game.

### Shared-code changes (not macOS-only)
- `kernel_file.c`: ISO directory enumeration (lock leak, buffer bounds), positioned reads/writes, directory-context release on close (POSIX).
- `kernel_sync.c`, `kernel_thread.c`: very long relative/absolute waits no longer wrap around into short ones.
- `nv2a_pgraph_d3d11.c`: the occlusion span table is indexed by a free slot, not by the backend's query handle (a backend that numbers queries past 255 overran the 256-entry table and corrupted memory: it showed up as crashes in different places and a failed save); vertex reads are page-checked on macOS; the substitution buffer is only honoured while a substituted draw runs.
- `kernel_hal.c`: a guest-clock scale hook (`xbox_clock_scale_hook`, unset = no change) used only by the pacing option above.
- `win32_compat.c` / `settings.c`: `OT_HOME_DIR` lets the macOS app keep settings and saves outside its bundle.

### Tested
macOS 27 on an Apple Silicon Mini: the whole game from the title to a race, saves and loading, the intro movies, 60 and 120 fps, SMAA/HDR/upscaling options, DualShock 4 (input and rumble), keyboard remapping, a bounds-checked (`-fsanitize=bounds`) build through a scripted race with no findings, the signed and notarized app and disk image. Scripted menus and races: `macos/tests/drive.py` (uses a small diag console, `XBOX_DIAG_PORT`, now also on POSIX).

### Not tested
- **Windows**: the shared-code edits above have not been built or run on Windows. Please treat them as untested there.
- Netplay (`XBOX_NET`) on macOS: it starts, but two instances closed when a race loaded; not investigated.
- A second Mac for the iCloud sync (conflict handling is tested with a simulated second side).

### No game content
No disc image, executable, game data or generated game code is included; the app asks for your own disc image. The icon is original artwork drawn by `macos/launcher/make_icon.swift`.
