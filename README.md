<a href="https://opentricky.com"><img width="1280" height="320" alt="OpenTricky" src="https://github.com/user-attachments/assets/eb23ea38-f97d-43a1-b22c-e58a80f90aa7" /></a>

<h1 align="center">OpenTricky</h1>

<p align="center">
  <b>The PC port SSX Tricky never got.</b><br>
  Built from the Xbox game, with the original's features restored, and everything a modern PC release would have: today's controllers, new visual options, ultrawide, high frame rates and a real launcher.
</p>

<p align="center">
  <a href="https://opentricky.com"><img alt="Website: opentricky.com" src="https://img.shields.io/badge/website-opentricky.com-0b5cd5"></a>
  <a href="https://github.com/GiZcesi/OpenTricky/releases"><img alt="Release" src="https://img.shields.io/github/v/release/GiZcesi/OpenTricky?include_prereleases&label=release&color=orange"></a>
  <img alt="Status: unstable" src="https://img.shields.io/badge/status-unstable-orange">
  <img alt="Platform: Windows 10/11 x64" src="https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue">
  <img alt="Graphics: Direct3D 11" src="https://img.shields.io/badge/graphics-Direct3D%2011-blue">
  <a href="LICENSE"><img alt="License: GPL-3.0-only" src="https://img.shields.io/badge/license-GPL--3.0--only-green"></a>
  <a href="https://discord.gg/r38sThsqnj"><img alt="Discord" src="https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white"></a>
  <a href="https://ko-fi.com/giz_music/"><img alt="Ko-fi" src="https://img.shields.io/badge/Ko--fi-buy%20me%20a%20coffee-FF5E5B?logo=ko-fi&logoColor=white"></a>
</p>

<p align="center"><b>Join the <a href="https://discord.gg/r38sThsqnj">OpenTricky Discord</a></b> for help, screenshots and news about the next builds.</p>

> [!IMPORTANT]
> **You need your own game.** No disc image, game code, music or video is distributed: you need a disc image (`.iso` or `.xiso`) of ***SSX Tricky* for the original Xbox, USA release**.

> [!WARNING]
> **This is an unstable release.** You can play it from start to finish, but some parts are still rough. Please read [Known issues](#known-issues) before reporting a bug.

---

## Contents

- [What is this?](#what-is-this)
- [What's new in 0.1.1](#whats-new-in-011)
- [Install and play](#install-and-play)
- [Controls and settings](#controls-and-settings)
- [Known issues](#known-issues)
- [FAQ](#faq)
- [Building from source](#building-from-source)
- [Reporting a bug](#reporting-a-bug)
- [Credits](#credits)
- [Legal](#legal)

---

## What is this?

OpenTricky runs *SSX Tricky* (Xbox, 2001) natively on Windows. It is not an emulator: the game's own Xbox code is translated to C with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and compiled into an ordinary Windows program, and the Xbox's graphics, sound and input chips
are reimplemented on Direct3D 11, XAudio2 and XInput. OpenTricky continues [**SSX Tricky PC**](https://github.com/MatiasRiveraC/SSX-Tricky-PC) by MatiasRiveraC.

The goals:

1. **Feature complete.** Built from the Xbox version, with the console's features restored: fog, lens flares, colours, 5.1 sound.
2. **A modern PC release**: modern controllers and button styles (Xbox, PlayStation, PS2 original), the original PS2 controls with all 15 grabs, full keyboard and controller remapping, any resolution, ultrawide, high frame rates, all set up in one launcher.
3. **New options on top**: smoother edges, soft shadows, longer draw distance, optional HD textures. The *Original Xbox* preset turns them all off at once.

## What's new in 0.1.1

- **A new launcher**, `OpenTricky.exe`: settings in six tabs with presets, full keyboard and controller remapping, and updates it installs only if you say so.
- **The original PS2 controls**: all 15 grabs on the shoulder buttons and triggers, with *Xbox modern*, *PlayStation modern* or *PS2 original* buttons.
- **Wide screens done properly**: the race HUD keeps its proportions, and the menus are shown as the Xbox drew them, in 4:3.
- **More stable**: a rare save loss and rare freezes and crashes are fixed, and your previous save is kept as a backup.
- **5.1 surround**, **VSync**, smoother high frame rates and less overhead in the busiest scenes.

Everything else: the [release notes](https://github.com/GiZcesi/OpenTricky/releases) and [CHANGELOG.md](CHANGELOG.md).

## Install and play

**You need** Windows 10 or 11 (64-bit), a Direct3D 11 graphics card, and a disc image (`.iso` or `.xiso`) of your own ***SSX Tricky* Xbox disc, USA release**
(the PS2, GameCube, PAL and Japanese versions won't work). A controller is optional: Xbox-style (XInput) controllers and the keyboard work.

1. Download `OpenTricky-<version>-win64.zip` from [**Releases**](https://github.com/GiZcesi/OpenTricky/releases) and extract it into a folder of its own.
2. Start **`OpenTricky.exe`**, the launcher, and choose your disc image: the launcher checks it and remembers it.
3. Pick a preset or your own settings, then press **PLAY**.

> [!NOTE]
> Windows SmartScreen may warn you because the program isn't code-signed. If you trust the download, choose **More info → Run anyway**.

The launcher also installs the next versions for you, only if you say so. More in the [install guide](https://opentricky.com/install).

## Controls and settings

The game uses the **original PS2 layout**. Everything can be remapped in the launcher's **Controls** tab.

| PS2 button | Controller | Keyboard |
|---|---|---|
| Cross / Circle / Square / Triangle | A / B / X / Y | `Space` / `Esc` / `C` / `V` |
| L1 / R1 | LB / RB | `F` / `R` |
| L2 / R2 | LT / RT | `Q` / `E` |
| Start / Select | Start / Back | `Enter` / `Tab` |
| L3 (press the left stick) | Left stick press | `X` |
| Left stick and D-pad | Left stick and D-pad | Arrow keys |

In the game window: `Alt+Enter` or `F11` switches window / fullscreen, `F12` takes a screenshot, `Alt+F4` quits.

All settings are in the launcher, in six tabs (*Display*, *Graphics*, *Audio*, *Controls*, *Game*, *Advanced*). Three presets set the main ones at once:
**Recommended** (the default), *Original Xbox* (every PC extra off) and *Steam Deck* (1280×800, 60 FPS).
Every setting, with its choices and default, is explained in the [**Settings guide**](https://opentricky.com/settings) and the [controls page](https://opentricky.com/controls).

## Known issues

The main ones, most serious first. The full list is on [opentricky.com/known-issues](https://opentricky.com/known-issues).

| Problem | What to do for now |
|---|---|
| **At the start of a race, the picture can briefly look doubled or shifted** with some settings. We're looking into it. | Tell us your settings (Discord or an issue). |
| **During the opening logo video, the first press of Start is sometimes ignored** (Enter on the keyboard). | Press it again. If it still doesn't react, let the video play to the end, or restart the game. |
| **Frame rate drops when riders collide, and Snowdream runs slower than other tracks.** | No workaround yet. |
| **Above 60 FPS, the controls answer one game step later** (about 17 ms). | Set *Frame rate limit* to *60* for the Xbox's exact timing. |
| **Only Xbox-style (XInput) controllers are supported natively.** | Use Steam Input or DS4Windows, and choose *PlayStation modern* or *PS2 original* in *Button style*. |
| **Steam Deck, Linux (Proton) and real 5.1 speakers are not tested yet.** | Reports are welcome. |

## FAQ

Disc images, saves, updates, frame rates, controllers, Linux and the Steam Deck, help when something goes wrong:
every answer is on [**opentricky.com/faq**](https://opentricky.com/faq).

## Building from source

The game code is translated **on your own PC, from your own disc image**. It is never in this repository.

1. Install [MSYS2](https://www.msys2.org/), then run this in its **UCRT64** shell:
   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake make \
     mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-python-capstone \
     mingw-w64-ucrt-x86_64-python-numpy mingw-w64-ucrt-x86_64-python-pillow
   ```
2. Build from a Windows command prompt:
   ```bat
   build.bat "C:\path\to\SSX Tricky (USA).iso"
   ```
   Or from the UCRT64 shell: `./build.sh "/c/path/to/SSX Tricky (USA).iso"`.
3. When the build is done, start `dist/OpenTricky/OpenTricky.exe` (the launcher) and choose the same disc image.

The script reads `default.xbe` from your disc image, translates it to C, applies the port's fix-up passes, then compiles the game and the launcher into `dist/OpenTricky/`.
No other tool is needed. [docs/building.md](docs/building.md) has the details.

> [!CAUTION]
> Your build contains code translated from your copy of the game. Please don't share it: point people to the releases here instead.

## Reporting a bug

Open an [issue](https://github.com/GiZcesi/OpenTricky/issues/new/choose), or post in #bug-reports on the [Discord](https://discord.gg/r38sThsqnj), with the version
(*Settings → Advanced → About OpenTricky*), your settings, your PC, and `SSX Tricky.log` (next to `settings.ini`) or the zip made by *Prepare a bug report* in the launcher.
Nothing is ever sent by itself. More in [Help & bug reports](https://opentricky.com/faq#help).

## Credits

| Who | What |
|---|---|
| [**MatiasRiveraC**](https://github.com/MatiasRiveraC) | Creator of [SSX Tricky PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC), the port OpenTricky continues: the recompilation, runtime, renderer, launcher and the reverse-engineering work. |
| [**sp00nznet**](https://github.com/sp00nznet) | [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), the static recompiler and Xbox runtime everything is built on (MIT). |
| [**xemu**](https://xemu.app) | The GPU and audio emulation that parts of the renderer and the audio are adapted from (LGPL-2.1-or-later), and the accuracy reference. |
| **Jorge Jimenez et al.** | [SMAA](https://www.iryoku.com/smaa/) (MIT). |
| **Bl4ckH4nd** | HD textures (not included). |
| [RmlUi](https://github.com/mikke89/RmlUi), [SDL](https://www.libsdl.org/), [FreeType](https://freetype.org/) | The launcher's interface, window and input, and text (MIT, zlib, FreeType License). |
| [Capstone](https://www.capstone-engine.org/), [Ghidra](https://ghidra-sre.org/) | Disassembly and reverse engineering. |
| **GiZcesi** | OpenTricky: direction, testing and playing. |

See [CONTRIBUTORS.md](CONTRIBUTORS.md).

## Legal

- OpenTricky is an unofficial fan project. It is not affiliated with or endorsed by Electronic Arts, EA Canada or Microsoft.
  *SSX* and *SSX Tricky* are trademarks of Electronic Arts, and Xbox is a trademark of Microsoft.
- **No disc image, game code, music or video is distributed.** You need your own copy of the game.
- Licences: OpenTricky's code is under the **GNU General Public License v3.0 only** (`GPL-3.0-only`, see [LICENSE](LICENSE)), unless a file says otherwise.
  `xboxrecomp/` keeps its MIT licence (`xboxrecomp/LICENSE`). Files adapted from xemu stay under LGPL-2.1-or-later, as their headers say. SMAA is MIT.
  The launcher uses RmlUi (MIT), SDL 3 (zlib), FreeType (FreeType License) and fonts under the SIL Open Font License;
  their licences are in `port/third_party/` and `port/launcher/app/ui/fonts/`, and in `THIRD-PARTY-LICENSES.txt` in the release.
  Portions of this software are copyright © 2026 The FreeType Project (https://freetype.org). All rights reserved.
- Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first.
