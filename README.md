<img width="1280" height="320" alt="BANNIERE-FINALE-1280x320" src="https://github.com/user-attachments/assets/eb23ea38-f97d-43a1-b22c-e58a80f90aa7" />

<h1 align="center">OpenTricky</h1>


<p align="center">
  <b>SSX Tricky (Xbox, 2001), running natively on Windows</b><br>
  A recompiled PC port with modern display options. It is not an emulator. Bring your own disc.
</p>

<p align="center">
  <a href="https://github.com/GiZcesi/OpenTricky/releases"><img alt="Release" src="https://img.shields.io/github/v/release/GiZcesi/OpenTricky?include_prereleases&label=release&color=orange"></a>
  <img alt="Status: unstable" src="https://img.shields.io/badge/status-unstable-orange">
  <img alt="Platform: Windows 10/11 x64" src="https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue">
  <img alt="Graphics: Direct3D 11" src="https://img.shields.io/badge/graphics-Direct3D%2011-blue">
  <a href="LICENSE"><img alt="License: GPL-3.0-only" src="https://img.shields.io/badge/license-GPL--3.0--only-green"></a>
  <a href="https://discord.gg/r38sThsqnj"><img alt="Discord" src="https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white"></a>
  <a href="https://ko-fi.com/giz_music/"><img alt="Ko-fi" src="https://img.shields.io/badge/Ko--fi-buy%20me%20a%20coffee-FF5E5B?logo=ko-fi&logoColor=white"></a>
</p>

<p align="center"><b>Join the <a href="https://discord.gg/r38sThsqnj">OpenTricky Discord</a></b> for help, screenshots and news about the next builds.</p>

<!-- TODO screenshot (hero): 21:9 race start on Garibaldi — Smooth edges High, Soft shadows On, Draw distance Max -->

> [!IMPORTANT]
> **You need your own game.** This repository contains source code only: no disc image, no game data, music or video, and no translated game code.
> The release contains the compiled executable only: no disc image, game data, music or video.
> To play, you still need a disc image (`.iso`) of your own ***SSX Tricky* for the original Xbox, USA release**.

> [!WARNING]
> **This is an unstable release.** You can play it from start to finish, but some parts are still rough. Please read [Known issues](#known-issues) before reporting a bug.

---

## Contents

- [What is this?](#what-is-this)
- [What's new since SSX Tricky PC v0.2.0](#whats-new-since-ssx-tricky-pc-v020)
- [Screenshots](#screenshots)
- [What you need](#what-you-need)
- [Install and play](#install-and-play)
- [Controls](#controls)
- [Settings](#settings)
- [Known issues](#known-issues)
- [FAQ](#faq)
- [Building from source](#building-from-source)
- [Reporting a bug](#reporting-a-bug)
- [Community](#community)
- [Credits](#credits)
- [Legal](#legal)

---

## What is this?

OpenTricky continues [**SSX Tricky PC**](https://github.com/MatiasRiveraC/SSX-Tricky-PC) by MatiasRiveraC.
The game's own Xbox code is translated to C with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and compiled into an ordinary Windows program.
The Xbox's graphics, sound and input chips are reimplemented on Direct3D 11, XAudio2 and XInput.

The goals, in order:

1. **Look and play like the Xbox.** Everything that works on the console should work here, including the fog, lens flares, colours and intro scenes.
2. **Run well on today's PCs**: high frame rates, ultrawide screens, any resolution.
3. **Offer optional extras on top**: smoother edges, soft shadows, a longer draw distance.
   Every extra can be switched off, and the defaults look like the Xbox.

## What's new since SSX Tricky PC v0.2.0

**Closer to the Xbox**
- The fog and mist banks over the courses are back.
- The sun's lens flares are back.
- Board tops are no longer black. This fixes the Uberboard and also brings back the terrain's distance haze.
- Colours and brightness match the console. The Xbox's gamma ramp is now applied, so the picture is no longer about 12 % too bright.
- The race fly-over and the rider intros play in full.
- A rare freeze at the EA logo on start-up is fixed.

**Faster and smoother**
- **High frame rates**: 60 like the Xbox, or 120, 144, 240 or unlimited.
  The game logic still runs at 60 steps per second, exactly as on the Xbox. The frames in between are interpolated, so the gameplay doesn't change.
- **A much lighter renderer**: it does less work per frame, and particles are now drawn on the GPU. On a gaming PC at 1080p, this took the median frame rate from about 110 to about 225 FPS.
- The sound no longer crackles, and the audio delay is shorter.

**Modern display**
- **Ultrawide 21:9 and 32:9**: the game draws a wider view itself instead of stretching the picture. A field-of-view setting is included.
- **Smooth edges (SMAA)**, from Low to Ultra. Menu and HUD text stays sharp.
- **Soft shadows** (optional): shadow edges fade over a few pixels instead of the Xbox's hard edges.
- **Draw distance** (optional): Original, Far (×1.5) or Max (×2), so less scenery pops in.

**Like a real PC game**
- A new launcher in the style of the game, with the game's own menu sounds and settings in tabs.
- Keyboard and controller remapping in the launcher.
- A clean borderless window (`Alt+Enter` or `F11` for fullscreen), and `F12` screenshots.
- Saves are kept in `Documents\My Games\SSX Tricky\Saves`. A portable mode is also available.

The full list is in [CHANGELOG.md](CHANGELOG.md).

## Screenshots

<!-- TODO screenshot 1: race start, 1920×1080 16:9 — Smooth edges High, Soft shadows On, Draw distance Max -->
<!-- TODO screenshot 2: 3440×1440 21:9 — a big trick in mid-air, showing the wider view -->
<!-- TODO screenshot 3: comparison "v0.2.0 vs OpenTricky" — fog on Garibaldi, sun flare on Elysium, Uberboard, brightness -->

*Screenshots are coming soon.*

## What you need

| | |
|---|---|
| **The game** | A disc image (`.iso`) of your own ***SSX Tricky* Xbox disc, USA release**. The PS2 and GameCube versions won't work, and neither will the PAL or Japanese Xbox releases. |
| **PC** | Windows 10 or 11, 64-bit, and a Direct3D 11 graphics card. Any recent PC plays at 60 FPS. For high frame rates at high resolutions, you need a gaming PC. |
| **Controller** | Optional. Any Xbox-style (XInput) controller works, and so does the keyboard. |

## Install and play

1. Download `OpenTricky-<version>-win64.zip` from [**Releases**](https://github.com/GiZcesi/OpenTricky/releases) and extract it into a folder of its own.
2. Make a disc image (`.iso`) of your SSX Tricky Xbox disc.
3. Start **`SSX Tricky.exe`**. The first time, the launcher asks for your disc image and then remembers it.
4. Open **SETTINGS** to choose the resolution, the screen shape (the game starts in 4:3, like the Xbox) and the extras, then press **PLAY**.

> [!NOTE]
> Windows SmartScreen may warn you because the program isn't code-signed. If you trust the download, choose **More info → Run anyway**.

Settings are kept in `SSX Tricky.ini`, next to the game. Screenshots go to a `Screenshots` folder next to the game.
For a portable copy that keeps its saves next to the game, create an empty file named `portable.txt` in the game's folder.

## Controls

Everything can be remapped in **SETTINGS → CONTROLS**.

| Xbox button | Controller | Keyboard |
|---|---|---|
| A / B / X / Y | A / B / X / Y | `Space` / `Esc` / `C` / `V` |
| Black / White | RB / LB | `R` / `F` |
| Left / right trigger | LT / RT | `Q` / `E` |
| Start / Back | Start / Back | `Enter` / `Tab` |
| Left stick and D-pad | Left stick and D-pad | Arrow keys |

| In the game window | |
|---|---|
| `Alt+Enter` or `F11` | Window / fullscreen |
| `F12` | Screenshot |
| `Alt+F4` | Quit |

## Settings

All settings are in the launcher. Defaults are in **bold**. Out of the box, the game looks like the Xbox.

| Setting | Choices | What it does |
|---|---|---|
| Resolution | from 640×480 to 4K, plus ultrawide sizes | The size the game is drawn at. |
| Display | **Window**, Fullscreen | `Alt+Enter` switches while you play. |
| Screen shape | **4:3**, 16:9, 21:9, 32:9, Auto | 16:9 is the game's own widescreen mode. 21:9 and 32:9 show a wider view and aren't stretched. |
| Field of view (wide screens) | **No stretch**, Balanced, Full | For 21:9 and 32:9 only. *No stretch* keeps the 16:9 width of view. *Full* shows more, with stretched edges. |
| Smooth edges | **Off**, Low, Medium, High, Ultra | SMAA edge smoothing. Text stays sharp. |
| Extra edge smoothing (MSAA) | **Off**, 2×, 4×, 8× | Smooths edges while drawing. It costs more, and it adds to *Smooth edges*. |
| Texture sharpness | **Like the Xbox**, 2× to 16× | Anisotropic filtering: sharper slopes in the distance. |
| Soft shadows | **Off**, On | Shadow edges fade over a few pixels instead of the Xbox's hard edges. |
| Frame rate limit | **60**, 120, 144, 240, Unlimited | Above 60, the frames in between are interpolated. The game logic stays at 60. |
| Draw distance | **Original**, Far, Max | Draws scenery ×1.5 or ×2 further away. It costs CPU time. |
| Show frame rate | **Off**, On | Shows the FPS in the window title. |
| Audio delay | **normal (~76 ms)**, shorter or longer | Raise it if the sound crackles. |
| Menu sounds | **On**, Off | Plays the game's own menu sounds in the launcher, read from your disc. |
| Look like the Xbox | **On**, Off | The fixes that bring back the fog, lens flares, board tops and colours. Leave it on. |
| Save games folder | **Automatic**, a folder you choose | `Documents\My Games\SSX Tricky\Saves` by default. |
| Log file | **Off**, On | Writes `SSX Tricky.log` next to the game, for bug reports. |

## Known issues

From most to least serious. If your problem isn't listed, please [report it](#reporting-a-bug).

| Severity | Problem | What to do for now |
|---|---|---|
| High | **Rare crash while a race loads or during its intro.** It is very rare, and the cause is being investigated. | Start the game again; your saves are safe. If it happens, please send the log file. |
| Medium | **21:9 and 32:9: the race HUD is stretched.** Menus and loading screens also fill the whole width instead of keeping their original frame. A proper wide-screen HUD is in progress. | Play in 16:9 if this bothers you. The 3D view itself is correct. |
| Medium | **32:9 with *No stretch*: the view looks zoomed in.** | Use the *Balanced* field of view on 32:9 screens. |
| Medium | **A short flash on screen when your rider respawns after a fall.** | No workaround yet. A fix is in progress. |
| Low | **At very high resolutions with every extra on** (for example 3440×1440 with Soft shadows and Draw distance Max), the game runs at about 100–140 FPS, with occasional dips. | Use *Draw distance: Far* or *Original*, or a 144 FPS limit. |
| Low | **No VSync option yet.** With *Unlimited*, you may see tearing. | Set *Frame rate limit* to your monitor's refresh rate. |
| Low | **Above 60 FPS, the controls answer one game step later** (about 17 ms). The interpolated frames need this. | Use *60* for the Xbox's exact timing. |
| Low | **Split screen: if both players are on the same controller,** the second view shows the first player. The original port does this too. | Give each player their own controller. |
| Low | **The sound may crackle on some PCs.** | Raise *Audio delay* in the launcher. |
| Minor | **Only Xbox-style (XInput) controllers are supported.** PlayStation controllers aren't supported natively yet, and the on-screen buttons are Xbox ones. | Use Steam Input or DS4Windows. Native support is in progress. |
| Minor | **Snow spray and trails can look smaller or fainter than you remember.** We found no error in the port: they match what the game itself draws. | Nothing to do. We're still comparing against a real console. |
| Minor | **The Snowdream and Pipedream skies differ from v0.2.0.** The night sky with the moon, and the black sky above the Pipedream dome, are what the Xbox shows. | Nothing to do. |
| Minor | **The launcher's menu sounds are a little loud.** | Set *Menu sounds* to Off. |
| Minor | ***Far* and *Max* draw distance are new.** A track may show odd scenery far away. | Switch back to *Original*, and tell us which track it was. |

## FAQ

**Is this an emulator?**
No. The game's code was translated once into a normal Windows program. Only the console's graphics, sound and input chips are reimplemented, on Direct3D 11, XAudio2 and XInput.

**Why do I need my own disc image?**
The game belongs to Electronic Arts, so nothing from it is shared here. The launcher reads your disc image each time you play and never changes it.

**Which disc image works?**
*SSX Tricky* for the original Xbox, USA release. Both an extract-xiso image and a full disc dump work.

**Can I use the PS2 or GameCube version?**
No. The port is built from the Xbox program.

**Does it run on Linux or the Steam Deck?**
It hasn't been tested. It is a plain Direct3D 11 Windows program, so Proton or Wine may work. Reports are welcome.

**Are the high frame rates real?**
The game logic runs at 60 steps per second, as on the Xbox, so physics, AI and replays behave exactly the same. The camera and the riders are interpolated between those steps. Motion is smooth at 120, 144, 240 FPS and above.

**Where are my saves?**
In `Documents\My Games\SSX Tricky\Saves`, or in `Saves` next to the game when you use `portable.txt`. The launcher can open that folder for you.

**Is online multiplayer planned?**
It is being explored, but it isn't part of this release.

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
3. When the build is done, the game is in `dist/OpenTricky/`.

The script reads `default.xbe` from your disc image, translates it to C and applies the port's fix-up passes. It then compiles everything and copies the program and the DLLs it needs into `dist/`.
No other tool is needed. [docs/building.md](docs/building.md) has the details.

> [!CAUTION]
> Your build contains code translated from your copy of the game. Please don't share it: point people to the releases here instead.

## Reporting a bug

Please open an [issue](https://github.com/GiZcesi/OpenTricky/issues/new/choose) and include:
- the version: the release name, or the build date shown in *SETTINGS → ADVANCED → About*;
- your settings: resolution, screen shape, frame rate limit and the extras you use;
- your graphics card and CPU;
- the **log file**: turn on *SETTINGS → ADVANCED → Log file*, make the problem happen again, then attach `SSX Tricky.log`;
- an `F12` screenshot if something looks wrong, and where in the game it happened.

## Community

Questions, screenshots or just want to chat? Join the [**OpenTricky Discord**](https://discord.gg/r38sThsqnj). Bugs still go to [GitHub issues](https://github.com/GiZcesi/OpenTricky/issues), so they don't get lost.

## Credits

| Who | What |
|---|---|
| [**MatiasRiveraC**](https://github.com/MatiasRiveraC) | Creator of [SSX Tricky PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC), the port OpenTricky continues: the recompilation, runtime, renderer, launcher and the reverse-engineering work. |
| [**sp00nznet**](https://github.com/sp00nznet) | [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), the static recompiler and Xbox runtime everything is built on (MIT). |
| [**xemu**](https://xemu.app) | The GPU and audio emulation that parts of the renderer and the audio are adapted from (LGPL-2.1-or-later), and the accuracy reference. |
| **Jorge Jimenez et al.** | [SMAA](https://www.iryoku.com/smaa/) (MIT). |
| [Capstone](https://www.capstone-engine.org/), [Ghidra](https://ghidra-sre.org/) | Disassembly and reverse engineering. |
| **GiZcesi** | OpenTricky: direction, testing and playing. |

See [CONTRIBUTORS.md](CONTRIBUTORS.md).

## Legal

- OpenTricky is an unofficial fan project. It is not affiliated with or endorsed by Electronic Arts, EA Canada or Microsoft.
  *SSX* and *SSX Tricky* are trademarks of Electronic Arts, and Xbox is a trademark of Microsoft.
- **No game data is distributed.** This repository holds source code only. The release holds the compiled executable only: no disc image, game data, audio or video.
  You must own the game and supply your own disc image.
- Licences: OpenTricky's code is under the **GNU General Public License v3.0 only** (`GPL-3.0-only`, see [LICENSE](LICENSE)), unless a file says otherwise.
  `xboxrecomp/` keeps its MIT licence (`xboxrecomp/LICENSE`). Files adapted from xemu stay under LGPL-2.1-or-later, as their headers say. SMAA is MIT.
- Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first.
