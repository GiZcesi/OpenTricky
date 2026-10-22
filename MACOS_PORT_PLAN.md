
## 2026-10-21: step 3 (smoothness): findings
- Shader/pipeline creation measured during a race (`[GPU]   N shaders/pipelines created` lines): about 1 ms each, at most a few ms per period. A disk cache would
  gain nothing visible; not built. (Metal's own cache covers repeats; a cold first run on another Mac is untested.)
- ProMotion / high refresh: `OT_FPS=monitor` already follows the display (120 on ProMotion) with the game's in-between frames; nothing missing.
- MetalFX: SDL's GPU API does not expose the Metal device or command buffer, so MetalFX cannot be attached without leaving SDL's renderer. Instead the SDR
  final pass now upscales with Catmull-Rom bicubic + an anti-ringing sharpen (OT_UPSCALE=sharp|edge, default off; OT_SHARPEN 0..1; d3d8_gpu_out.c `up_ps`).

## 2026-10-21: step 6
- Audio: SDL3's default-device stream already follows the system output (AirPods etc.); `OT_AUDIO_DEVICE=<name>` picks a specific output (settings window lists them, CoreAudio names).
  Spatial audio for AirPods applies at the system level to the stereo stream ("Spatialize Stereo"); nothing app-side to add.
- Netplay (port/src/netplay, XBOX_NET): initialises on macOS ("[NET] active") but is a localhost-only prototype; with host + guest both started into a race by
  `macos/tests/drive.py` both instances closed ("[WINDOW] closed: exiting") when the race loaded. Two plain instances side by side run fine, so it is netplay-specific; not investigated further.
- Upstream PR: text in macos/UPSTREAM_PR.md; not opened (it is a public action on someone else's repository).

## 2026-10-21: edge-adaptive upscaling
`OT_UPSCALE=edge` (an option; `sharp` = bicubic; the default is the plain blit, as the original, per CONTRIBUTING) adds an FSR 1 style filter to the SDR final pass (`easu` in d3d8_gpu_out.c, an original
implementation of the idea): a 4x4 patch gives the local gradient, the Lanczos-2 shaped kernel is stretched along the edge, the result is held inside the four nearest texels,
then the same mild unsharp (OT_SHARPEN). Seen against `smooth` on the title menu (480p into a 900p window): cleaner outlines and snowflake detail, no ringing.

## 2026-10-22: pacing (loading screens, start countdown)
Measured on this Mac (log lines `[PACING] ...`): a track loads in about 0.75-1 s (the console: many seconds); the start countdown (race state 3) lasts 2.5 s; the
loading screen is animated by the title itself, so it can simply be kept up.
- `OT_MIN_LOAD_MS` (default 3000): InGameState_LoadLevel (0xAC9B0) is held to at least this long after it returns; the animated "loading..." screen stays up
  (seen in a window capture during the hold).
- `OT_COUNTDOWN_SLOW` (default 1.5): while race state is 3 the title's clocks (KeQueryInterruptTime, KeQueryPerformanceCounter, KeTickCount) run at 1/factor
  (`xbox_guest_clock_adjust`, kernel_hal.c); the title's timers are armed against them, so the countdown measured 3.73 s at 1.5 and 4.70 s at 2. (Stretching the 60 Hz
  vblank alone changed nothing: the title's tick follows its own timer queue.) The guest clock stays monotonic and just stays behind by what it lost.
- Both are options in the settings window ("Pacing"); `OT_MIN_LOAD_MS=0` / `OT_COUNTDOWN_SLOW=1` give the original behaviour. The upstream PR branch should default to the original.
