# Changelog

All notable changes to OpenTricky. Versions follow [Semantic Versioning](https://semver.org/); `-unstable` marks test releases.

## [0.1.1-unstable] — 2026-10-08

A new launcher, the original PS2 controls, wide screens handled from the HUD to the menus, and a round of stability fixes, including a rare save loss.

### Fixed
- **Saves**: overwriting a save could rarely fail ("Save Failed.") after the old save had already been deleted, losing it. 0.1.0 is most likely affected too.
- A rare **freeze or crash near the finish line**, most likely when the frame rate is not capped: a crowd chant could be picked past the end of its list.
- A rare **freeze at start-up**, on a still or black screen just before the first logo video.
- A rare **freeze on a black screen at the end of a start-up video**. A safety net in the video stream reader stays in place too.
- The rare **crash while a race loads or during its intro** (a list of computer riders could run past its end). The same guard now covers the list of players.
- **Trick book**: rider portraits, chapter pictures and trick thumbnails are shown again instead of red boxes, and so are the trick tutorial's thumbnails.
- The **flash when your rider respawns**: the game's fades now cover the whole screen, in races and in the trick tutorial.
- **Pause panels** reach the edges of the screen in every case.
- **Pieces of scenery** (beams, oversized spectators) that could flash in the wrong place for one frame, mostly on Snowdream.
- **Free space for saves** is read from the drive that holds them.
- **Memory set-up at start-up** is more robust, which removes a cause of rare crashes on some PCs.
- **Error handling**: a stack overflow is now reported instead of closing the game without a message.
- Four known issues listed for 0.1.0 are fixed: the rare crash while a race loads, the stretched race HUD on 21:9 / 32:9, the flash when your rider respawns, and the missing VSync option.

### New
- **Launcher, `OpenTricky.exe`**, separate from the game; `SSX Tricky.exe` starts the game straight away with your settings.
  - Home page themed after one of the game's tracks, picked at random at each start, with a *What's new* panel and links to the release notes, GitHub and Ko-fi.
  - Disc image check: clear messages when the disc image is missing, moved or not the right one; PLAY stays greyed out until it is (`.iso` or `.xiso`).
  - Settings in six tabs (*Display*, *Graphics*, *Audio*, *Controls*, *Game*, *Advanced*), with presets: *Original Xbox* (every PC extra off), *Recommended* and *Steam Deck* (1280×800, 60 FPS, no soft shadows, no HD textures).
  - Full keyboard and controller remapping, with a warning for keys already in use. The launcher works with keyboard, mouse or controller.
  - **Updates**: at start, the launcher asks GitHub whether a newer version is out and offers to install it (*Check for updates*: *Ask before installing*, the default, or *Never*: no request at all; *Update channel*: *Stable* or *Unstable*). The download is checked against the release's `SHA256SUMS.txt`, nothing is changed if it can't be, and the previous version is kept for *Restore* (*Settings → Advanced*). Saves, settings and your disc image are never touched.
  - *Skip launcher next time*: the game starts straight away; hold Shift (or Select on a controller) at start to see the launcher again.
  - The launcher hides while you play. If the game closes on an error, it says so and can *Prepare a bug report* (a zip on your Desktop; nothing is sent). *Open logs* opens the log folder.
  - *About OpenTricky*, with the version, the credits and the licences.
- ***Button style*** (*Controls* tab): *Auto* (Xbox modern for now), *Xbox modern*, *PlayStation modern* and *PS2 original* (the PS2 look: pixel buttons at their original size, and the PS2 layout of the trick text). The buttons in the tricks panel, trick book, replay help and menus follow it.
- **Controller pictures in each style**: the trick tutorial and *Configure Controller* show the PS2 controller, a DualShock 4-style or an Xbox One-style controller, with the PS2 labels in every style.
- ***Speakers*** (*Audio* tab): *Auto*, *Stereo* or *5.1*. The game's six-speaker mix reaches 5.1 speakers: voices in the centre, the rear speakers during races. Stereo is unchanged. Not yet tested on real 5.1 speakers: reports are welcome.
- ***VSync***: On (default), *Adaptive* (a late frame shows at once) or *Off*. The frame rate limit now defaults to your screen's refresh rate, detected automatically.
- ***Menus*** (*Display* tab): *Original 4:3* (default), the menus exactly as the Xbox drew them, centred with black bars; or *Xbox 16:9*, the game's own widescreen menus, in a centred 16:9 frame on wider screens. Races use the whole width with both.
- ***Race HUD size***: 100, 90, 85 (default), 80 or 70 %, or *Like the Xbox* (stretched).
- **Save backup**: before a save is overwritten, the game keeps a copy of the previous one in a `UDATA-backup` folder next to your saves (*Save backup copy*, on by default).
- **Crash and freeze reports**: if the game stops on an error or shows nothing new for 30 seconds, it saves a report in a `CrashReports` folder (end of the log, settings, a small crash dump; user name and folders removed). Nothing is sent.
- **Optional HD textures** (off by default): point the launcher to your own copy of the HD textures by Bl4ckH4nd (not included).

### Better
- **PS2 controls everywhere**: all 15 grabs on the shoulder buttons and triggers (LB / RB / LT / RT = L1 / R1 / L2 / R2), and the launcher's *Controls* tab names each button the PlayStation way.
- **Played like the PS2 game**: Boost and Tweak on Square only (Circle does nothing); spins and flips on the D-pad in the *Default* preset (in the air, the left stick leans the rider); L3 cancels a jump (keyboard: X); Triangle is the reverse camera in the *Pro* preset, shown as "combat cam" in *Configure Controller*.
- **PS2 lessons and help**: the trick book lessons and the trick text follow the PS2 layout (PS2 shoulder order, Tweak shown as Square alone); the loading-screen controller picture describes the PS2 actions; the Xbox *Basic Controls* screen after the first logo video is skipped.
- **Modern button styles**: LB / RB (L1 / R1) as tall and as sharp as the triggers, with matching letters; each button centred where the original one was; sticks shown in the replay help; in the menus, the face buttons sit level with their text.
- **Race HUD**: keeps its proportions on every screen shape, 16:9 included, instead of being stretched; each part sits at its own edge or in the centre.
- **Wide screens**: the start-up videos keep their 4:3 shape with black bars; the 2D shown during a race outside the HUD (finish banner and time, pause, Top 5, results) keeps its proportions; menu panels stretch their dark background and gold bars to the edges while their text and icons keep their shape.
- ***No stretch* field of view**: on screens wider than 16:9, the chase camera sits further back so your rider keeps its 16:9 size and place.
- **Faster**: the game waits less for the renderer, and the renderer sends each draw to the graphics card in a single upload. The busiest moments of Garibaldi, Snowdream and Elysium gain the most; light scenes gain little.
- **Smoother high frame rates**: interpolated frames are timed on the game's own ticks, so riders and the camera jump much less during a hitch.
- **Draw distance *Far* / *Max***: mist banks follow the longer draw distance instead of popping in close to you. *Original* is unchanged.
- **PC texts**: the save and controller messages are reworded for a PC (save folder, free disk space; no more *Xbox*, *hard disk* or *Press A*). When there is not enough room to save, *Exit to dashboard* reads *Quit game* and closes the game. The empty memory unit lines are gone from the Save / Load screens.
- **Faster start-up**: the save check messages no longer stay on screen longer than the check; the first logo video comes up to about 4 seconds sooner.
- **New defaults**: borderless fullscreen at your screen's size, *Auto* screen shape (your screen's own shape), *Smooth edges* on *High*, *Soft shadows* on, *Log file* on.
- Both programs show their version in Windows file properties, and the log starts with the version and the settings file used.

### Removed
- The *Control scheme* setting and the Xbox layout: the PS2 layout is the only one.
- *Button icons*, replaced by *Button style*.
- The MSAA setting.
- The launcher window inside `SSX Tricky.exe`, and its menu sounds: the launcher is now `OpenTricky.exe`.
- The old `SSX Tricky.ini`: settings are now in `settings.ini` next to `OpenTricky.exe` (or in `Documents\My Games\SSX Tricky\` if that folder can't be written). Choose your disc image and settings again; saves are not touched.
- The launcher's button to open the screenshots and saves folders (`F12` still saves to `Screenshots`; *Open logs* opens the log folder).
- *Texture sharpness* is now called *Texture filtering*.

### Known issues
- At the start of a race, the picture can briefly look doubled or shifted with some settings. We're looking into it: tell us your settings.
- During the opening logo video, the first press of Start (Enter on the keyboard) is sometimes ignored: press it again. Very rarely it ignores more presses: let the video play to the end, or restart the game.
- **Performance**: frame rate drops when riders collide; Snowdream runs slower than other tracks.
- Only Xbox-style (XInput) controllers are supported natively, so *Button style: Auto* shows Xbox buttons.
- With the PlayStation button styles, the replay help still calls the Select button *BACK*.
- **Steam Deck and Linux** (Proton): not tested yet, the *Steam Deck* preset included.
- **5.1 surround** is not yet tested on real 5.1 speakers.
- **Terrain**: some pieces of terrain show visible seams. Probably the original game too; we're checking.

See also [Known issues](README.md#known-issues) in the README.

## [0.1.0-unstable] — 2026-10-04

First public release of OpenTricky, which continues [SSX Tricky PC](https://github.com/MatiasRiveraC/SSX-Tricky-PC) v0.2.0.

### Closer to the Xbox
- Fog and mist banks over the courses are drawn again. Their faces were being culled the wrong way round.
- The sun's lens flares are back. The GPU's visibility tests (occlusion queries) are now emulated.
- Board tops are no longer black, including the Uberboard on the Board screen. The terrain's distance haze is back as well.
- The Xbox's gamma ramp is applied, so colours and brightness match the console. Before, the picture was about 12 % too bright.
- The race fly-over and the rider intros play in full instead of being skipped.
- Fixed a rare freeze at the first logo video on start-up (the audio chip's interrupt is now emulated).
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
- A new launcher in the game's style: track-card background, a random rider, settings in tabs, and menu sounds.
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
