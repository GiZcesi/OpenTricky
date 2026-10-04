# Changelog

All notable changes to OpenTricky. Versions follow [Semantic Versioning](https://semver.org/); `-unstable` marks test releases.

## [0.1.0-unstable] — 2026-10-04

First public release of OpenTricky, which continues [SSX Tricky PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC) v0.2.0.

### Closer to the Xbox
- Fog and mist banks over the courses are drawn again. Their faces were being culled the wrong way round.
- The sun's lens flares are back. The GPU's visibility tests (occlusion queries) are now emulated.
- Board tops are no longer black, including the Uberboard on the Board screen. The terrain's distance haze is back as well.
- The Xbox's gamma ramp is applied, so colours and brightness match the console. Before, the picture was about 12 % too bright.
- The race fly-over and the rider intros play in full instead of being skipped.
- Fixed a rare freeze at the EA logo on start-up (the audio chip's interrupt is now emulated).
- Fixed the sound crackling: the emulated audio chip now runs in step with the game's mixer.

### Performance
- New frame rate limits: 120, 144, 240 and Unlimited, in addition to 60.
  The game logic stays at 60 steps per second. The camera and riders are interpolated in between and restored exactly afterwards, so the gameplay doesn't change.
- A lighter renderer: graphics state is cached, so there are about 4× fewer Direct3D calls per frame. The renderer no longer stalls waiting on the game.
- Particles (snow spray and trails) are drawn on the GPU, which removes the CPU spikes.
- Shorter audio delay: 8 buffers by default instead of 16, without underruns.

### Display
- Ultrawide 21:9 and 32:9, plus Auto (your monitor's shape). The game draws a wider view itself, so nothing is stretched.
  The *Field of view* setting offers No stretch, Balanced and Full.
- *Smooth edges* (SMAA, Low to Ultra). Menu and HUD text are drawn after the smoothing, so they stay sharp.
- *Soft shadows* (optional): shadow edges fade over a few pixels.
- *Draw distance* (optional): Original, Far (×1.5) or Max (×2), with a safe limit per track.

### PC side
- A new launcher in the game's style: track-card background, a random rider, settings in tabs, and the game's own menu sounds.
  It has a fade when you press PLAY.
- Keyboard and controller remapping in the launcher.
- A clean borderless game window without a menu bar: `Alt+Enter` / `F11` for fullscreen, `F12` for a screenshot, `Alt+F4` to quit.
- `F12` screenshots work on every PC now. A built-in PNG writer is used when Windows' encoder is missing.
- Saves go to `Documents\My Games\SSX Tricky\Saves`. A `portable.txt` file keeps them next to the game, and you can also choose your own folder.
  Saves from older versions are found and kept.
- Every fix and extra is a setting in the launcher (section `[Fork]` of `SSX Tricky.ini`).

### Building
- One command builds everything from your disc image: `build.bat "<your .iso>"`. No other extraction tool is needed.

### Known issues
See [Known issues](README.md#known-issues) in the README.
