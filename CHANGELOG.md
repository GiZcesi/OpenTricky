# Changelog

All notable changes to OpenTricky. Versions follow [Semantic Versioning](https://semver.org/); `-unstable` marked the test releases before 0.1.4, the first stable one.

## [0.1.5] — 2026-11-02

Faster when the processor is the limit, and a round of fixes: the sun's glare when the graphics card falls behind, the game's videos, split screen at *Draw distance Max*, camera cuts above 60 FPS, the scene after the finish line on screens wider than 16:9, *Find my disc image*, the launcher's tidying and F12 in a read-only folder. Two new settings: *Weather* and *PlayStation controllers (experimental)*. *Smoother legs in turns* catches up without a jump above 60 FPS, and is now Off in the *Steam Deck* preset.

### Fixed
- **The sun's glare** (always on, in every preset, *Original Xbox* included): to know how much of the sun shows, the game asks the graphics card and waits for its answer. When the card fell behind (a high frame rate limit with a heavy graphics load), OpenTricky stopped that wait too early, and the sun's glare and rays dropped out on some frames (on Pipedream, for example). The card now gets as long to answer as in the original game. Where the sun is in view, this wait can cost frames per second when the graphics card is the limit (see *Known issues*).
- **The game's videos** (always on, in every preset, *Original Xbox* included): OpenTricky showed their frames on a timer of its own, about 40 times a second and unevenly, though they are made at about 30 frames per second. Each frame now goes to the screen when the game finishes it, as in the original game: on a 60 Hz screen, in our tests, almost every frame stays on screen for two refreshes (about a third did in 0.1.4).
- **Split screen at *Draw distance Max***: with two players, the game could write new graphics commands over ones that weren't drawn yet, because its own check for this never saw where the drawing had got to. The picture then lost one or two of its colours, race display included, and could turn to a single colour. That check now works, and the game waits.
- **Camera cuts above 60 FPS**: when the game cut from one shot to the next over a short distance (at the start of a Showoff, for example), the picture slid from one shot to the other for a few frames. It now cuts, as at 60 FPS.
- **Screens wider than 16:9, after the finish line**: the scene the game plays there is shown as on a 16:9 screen, in a 16:9 frame in the middle. The camera used to step back and narrow its view for the wider screen: it could end up inside the scenery (seconds of a single colour), and the shots were cropped (heads cut off). The Top 5, the results and the replay stay wide, as in 0.1.4; the race itself doesn't change. The game's 3D sounds are placed from the game's own camera, as on a 16:9 screen, and above 60 FPS a race no longer starts with the rider looking bigger for a few frames.
- **The first frame after a change of frame**: when the picture switches between a frame in the middle of the screen and the full width (at the end of a race's intro, and after the finish line on screens wider than 16:9), that first frame is drawn for its new shape, not stretched or squeezed.
- ***Find my disc image***: choosing a disc image from the list could crash the launcher. With many disc images found, the list now scrolls inside the window instead of running past its edge. In the launcher's lists, with a controller or the keyboard, the selected line always comes into view, and a press never acts on a line you can't see.
- ***Smooth edges (SMAA)* under Wine 9.0**: Wine 9.0's shader compiler refused SMAA, so the game ran without it. It now builds there too; on Windows, the shaders are the same as before, byte for byte.
- **A shader that fails to build** (as can happen under Wine or Proton) is tried once, instead of again at every draw, which could slow the game to a crawl. What it would have drawn is left out, as before.
- **A pause just before the first race of a session**: the game drew its button icons at the start line, on the thread that feeds the graphics card. They are now drawn in the background when the game starts.
- **Launcher, files next to `OpenTricky.exe`**: since 0.1.2, each start removed any `SSX Tricky.exe`, `ui` folder or `THIRD-PARTY-LICENSES.txt` there by name alone, to tidy up after 0.1.1, and went through folder links (junctions) to do so: files of those names that weren't OpenTricky's could go. Now a file goes only when it is byte for byte one of 0.1.1's, nothing is deleted through a link, and anything else stays.
- **Launcher, `bin` as a folder link** (the program moved to another drive): a failed update or *Restore*, or one undone at the next start, no longer removes the link, which left the launcher with an empty window.
- **Screenshots (F12)**: when the folder of `OpenTricky.exe` can't be written (in *Program Files*, for example), they go to `Documents\My Games\SSX Tricky\Screenshots` instead of failing.
- **The screenshot `.txt`** shows `settings.ini` when the game's folder has accents in its name (it said *not found*), and says so when a start doesn't use `settings.ini` (for example, with a disc image given on the game's command line).
- **GPU wait at 60 FPS with *VSync* on**: a signal left over from the previous frame could end the wait at once and keep a processor core busy until the graphics card was done (in our tests, it ended most of these waits). The signal is now cleared before each wait, and a wait woken too early waits for its own frame.
- **System code**, two latent defects: a wait longer than about 49 days was cut short (a wait meant to last for ever ended after about 18 days), and a disc file's name could be written past the end of a buffer too short for it (the game's own buffers are long enough).

### Improved
- **Speed, when the processor is the limit**: the part of OpenTricky that turns the game's graphics commands into Direct3D 11 does less work for the same picture. It no longer sets again what the graphics card already has, keeps more of what it has built (shaders, samplers, the work of *Smoother terrain seams*), and reuses its vertex memory instead of asking the driver for new memory each time it fills up. On our test PC (Core i7-12700, GeForce RTX 4070 Super, 3440 × 1440, the same settings for both): without a frame rate limit, 24 to 31% more frames per second than 0.1.4 on average and 9 to 29% higher 1% lows (Garibaldi Race, Pipedream Freeride in the sun, Pipedream Showoff); on a 240 Hz screen, 240 FPS held on Garibaldi Race, Snowdream Showoff and Elysium Showoff (0.1.4: 237 on Elysium), but not on Pipedream (see *Known issues*). Not measured on a Steam Deck.
- ***Steam Deck* preset**: *Smoother terrain seams* and *Lightmap smoothing*, two options that add work for the processor and the graphics chip, are now Off, and so is *Smoother legs in turns* (see *Changed*); its *Weather* is *PS2*. Settings that matched the *Steam Deck* preset move to its new values at the update; settings changed by hand stay.

### New
- **Weather** (*Graphics*, under *Snow look*): when it snows (only outside races and with one player: always on Snowdream, on some visits to Merqury City), *PS2* leaves out the thick fog the original game adds to the snowfall, which the PS2 version doesn't have: the night sky and the distant slopes stay in view, shadows fall as in clear weather, and Garibaldi, Elysium and Untracked get no weather, as in the PS2 version. The falling snow itself is the original game's. *PS2* in *Recommended* and *Steam Deck*; *Original Xbox* keeps the original fog (*Original*). A 0.1.4 `settings.ini` gets the value of its preset, or of the closest one.
- **PlayStation controllers (experimental)** (*Controls*, Off by default, in no preset): a DualShock 4 or DualSense works without any other tool, and with *Button style* *Auto* the game shows PlayStation buttons. With it On, each controller keeps its player until it is unplugged; Off, controllers are read exactly as before. Leave it Off with Steam Input, DS4Windows, or Steam open with its PlayStation support: they already make the controller work, and it would be read twice.

### Changed
- ***Smoother legs in turns*** (*Settings → Advanced*): On in *Recommended*, Off in *Original Xbox*, and now Off in *Steam Deck*. The smoothing averages the legs and the board over a few game steps, so they show a little behind the game and, after each jump, landing or trick, have to catch up with it. Above 60 FPS, they now catch up over one game step, neither jumping back in a single frame nor sliding. At 60 FPS that catch-up is still a single frame, so the *Steam Deck* preset keeps the game's own pose, as *Original Xbox* does and as in the original game. Settings that matched *Steam Deck* move to Off at the update; settings changed by hand stay.
- **Old files next to `OpenTricky.exe`** that aren't exactly 0.1.1's (an `SSX Tricky.exe` from 0.1.0, for example) now stay where they are: delete them yourself if you don't need them.

### Known issues
- **Controls above 60 FPS** respond one game step (about 17 ms) later than at 60: *Frame rate limit* at *60 (Xbox)* gives the original game's exact timing.
- **Controllers**: Xbox-style (XInput) ones work natively, and the DualShock 4 and DualSense with *PlayStation controllers (experimental)*. For others, use Steam Input or DS4Windows, and choose *PlayStation modern* or *PS2 original* in *Button style*.
- **Proton Experimental** (Linux, Steam Deck) may run slower: try Proton 10 or PortProton.
- **Screens that don't report their refresh timing** (the Steam Deck, some laptops): at 60 FPS on 60 Hz, frames can queue up and reach the screen later than in 0.1.3.
- **Pipedream on a 240 Hz screen**: on our test PC, it doesn't reach 240 FPS: about 230 in the sun and 224 in Showoff (0.1.4: 221 and 203). Where the sun is in view, the game waits for the graphics card to know how much of the sun shows, as the original game does; when the card is the limit, the frame rate drops there.
- ***Smoother terrain seams*** leaves a few of the original game's seams visible, and adds work for the processor on every frame.
- **Steam Deck with a power limit**: a player found that 0.1.4 needs more power than 0.1.3 for the same frame rate. Not measured on a Steam Deck with 0.1.5.

See also [Known issues](README.md#known-issues) in the README.

## [0.1.4] — 2026-10-29

The first stable release: everything since 0.1.3, with evenly paced frames, an HD race display, new graphics options and a round of fixes to the picture and to stability.

### Fixed
- **Flicker**: the lattice towers and the speaker pole on Garibaldi, and the lit windows of Merqury City, no longer flicker.
- **Race display above 60 FPS**: the rival icon stays on its rider; the Tricky meter spins at the original speed instead of too fast (up to 2.3 times as fast at 144 FPS in our tests).
- **Tricky meter at any *Race HUD size***: the gauge stays in one piece, and its letters stay on it, when it bounces or a trick's points fly past.
- **Pause menu with *HD HUD***: the pause screens look as in the original game, like the screens after a race; the HD display comes back on the first frame after the pause.
- **Stutter from the log file**: the game's log is now written in the background, and its periodic diagnostics are left out of it, so it causes far fewer slow frames in a race (in our tests, about three quarters fewer frames over 8 ms).
- **Thin lines** (cables, wires) keep their width at high resolutions; the **Replay** panel reaches both edges on ultrawide screens; **trick names** are centred on their button icons.
- ***Look like the Xbox***: its fixes (fog volumes, lens flares, board tops, colours) are now always on, and the setting is gone.
- **Rare freezes and crashes**: when the game has no room left for a rider's model or shadow, that piece is skipped instead of freezing the game; a disc image the game can't start from gives a message instead of a crash. Fixes to the game's internal clocks, the sound and some translated code.
- **`settings.ini`** is fully written before it replaces the old file; *PLAY* says when the settings can't be saved. Disc images and folders with accented names now open when Windows is set to use UTF-8.
- **Screenshots (F12)** keep your window's size and no longer overwrite each other in the same second; a `.txt` beside each one notes the track and your settings, without your folder names. *Shift+F12* saves a burst.

### Improved
- **Frame pacing**: at 60 FPS with *VSync* on, on a 60, 120 or 240 Hz screen that reports its refresh timing, almost every frame stays on screen for the same time, at the cost of a few milliseconds of delay (in our tests, about 3 ms on a 240 Hz screen, more on a 60 Hz one). On a screen above 60 Hz, *Match screen* follows the screen's exact refresh rate, with less delay than 0.1.3 (in our tests, 1 to 2 ms less on a 240 Hz screen); on a 60 Hz screen, it runs at 60 FPS, like *60 (Xbox)*. Above 60 FPS, with a frame rate limit, frames are evenly spaced when your PC keeps up, and each one shows the game at the time it reaches the screen, so motion stays smooth.
- **Riders** are always drawn with their most detailed model.
- **Texture filtering** is *16x* in *Recommended* and *Steam Deck*.
- **Launcher**: shorter help and the same clear focus ring on every control; every connected controller works in the launcher, not only the first one; *Find my disc image* (click the disc label or the drop area to pick from the SSX Tricky (USA) disc images found on this computer); the window's shape stays between 16:10 and 2:1 when you resize it (at least 960×600); *PLAY* says when the game is already running or when Windows won't start it; dropping a file that isn't a disc image shows a message; the launcher goes through all ten track themes before repeating one.
- **Updates**: a *Check now* button; *Cancel* stops the download and changes nothing. An update or a restore cut short is put right at the next start: finished if every file was already in place; otherwise undone, so the version you were using comes back as it was.
- **About and licences**: *About OpenTricky* now credits SSX Tricky PC, xboxrecomp, xemu, SSX-Library, SMAA and the HUD art; `THIRD-PARTY-LICENSES.txt` also includes the licences of the game's third-party code, not only the launcher's.
- **Launcher look**: clear status labels for your disc image and for updates, sharper buttons and panels on every screen.

### New
- **HD HUD** (*Graphics*): the race display in HD. HUD art by Bl4ckH4nd.
- **Render scale** (*Display*, 50 to 200%); **Lightmap smoothing**, **Snow look** and **Shadow softness** (40 to 150%) in *Graphics*; **Smoother terrain seams** in *Advanced* (the seams are cached instead of being worked out on every frame). Most new options show a preview in the launcher.

### Changed
- **Release zip name**: `OpenTricky-<version>-win64.zip` (no `-unstable`).
- *Update channel*: *Unstable* is now *Beta*. *Smoother legs in turns* is no longer experimental.
- **Shorter setting names**: in *Frame rate limit*, *The screen's refresh rate* is now *Match screen* and *60 (as on Xbox)* is *60 (Xbox)*; in *Race HUD size*, *Like the Xbox (stretched)* is now *Stretched (Xbox)*; in *Texture filtering*, *Off (as on Xbox)* is now *Off (Xbox)*; *Save games folder* is now *Save folder*.

### Known issues
- **Above 60 FPS, the controls respond one game step later** (about 17 ms): set *Frame rate limit* to *60 (Xbox)* for the original game's exact timing.
- Only Xbox-style (XInput) controllers are supported natively; with Steam Input or DS4Windows, pick *PlayStation modern* or *PS2 original* in *Button style*.
- **Linux / Steam Deck (Proton):** with Proton Experimental, performance may be lower: if so, try Proton 10 or PortProton.
- **At 60 FPS on a 60 Hz screen that doesn't report its refresh timing** (the Steam Deck, some laptops), frames can wait in a queue and reach the screen later than in 0.1.3.
- *Smoother terrain seams* hides most seams, not all.

See also [Known issues](README.md#known-issues) in the README.

## [0.1.3-unstable] — 2026-10-13

A short release for Linux and the Steam Deck, plus one fix to the music. The very low frame rate under Wine and Proton is fixed, the music now fades as in the original game, and updates are now signed.

### Fixed
- **Very low frame rate on Linux and the Steam Deck (Wine, Proton)**: the game's shaders now run on the graphics card again under Proton, and the soft shadows no longer crash Wine's shader compiler. Reported on GitHub, then confirmed by a player: 60 FPS locked, 120 at most.
- **Music volume changes**: the music now fades down and back up over one second (under the announcer's voice, on the results screen, in the menus) instead of jumping at once, as in the original game. This was a translation error of the port.

### Improved
- **Steam Deck preset**: the launcher applies it by itself at the first start on a Steam Deck.

### New
- **Signed updates**: the launcher checks the signature of the downloaded zip. A zip that isn't signed, or that was modified, is refused.
- **What's new** panel of the launcher: read from `whatsnew.txt`.

### Changed
- **Release zip name**: `OpenTricky-<version>-unstable-win64.zip`. The launcher matches a signed zip to its release by the version number.

### Known issues
- **Above 60 FPS, the controls answer one game step later** (about 17 ms): set *Frame rate limit* to *60* for the original game's exact timing.
- **Linux / Steam Deck (Proton):** 0.1.3 fixes the very low frame rate under Wine/Proton. The game now runs smoothly on Linux. With Proton Experimental, performance may still be lower: if so, try Proton 10 or PortProton, and report it with your logs.
- Only Xbox-style (XInput) controllers are supported natively, so *Button style: Auto* shows Xbox buttons.
- **5.1 surround** is not yet tested on real 5.1 speakers.
- **Smoother legs in turns** is experimental.
- **Terrain**: some pieces of terrain show visible seams, as in the original game.

See also [Known issues](README.md#known-issues) in the README.

## [0.1.2-unstable] — 2026-10-11

A round of fixes that bring the game closer to the original game: the "melting" snow, the Showoff snowflakes, the crowd and voices at the end of a race, the stretched picture at the start, long sessions. Also: the HUD at high frame rates, Start during the intro, VSync *Adaptive*, saves in folders with accents, private crash reports, a new *Save profile* with everything unlocked, and a launcher that tells you when a setting does nothing. The game now lives in a `bin` folder.

### Fixed
- **"Melting" terrain**: on Snowdream especially, the snow could look deformed or smeared and your rider sink into it. The start of the 3D scene inherited the size of the last HUD element drawn, so the *Race HUD size* setting leaked into the scenery. HUD scaling now never touches the 3D scene. This was a defect of the port, not of the original game.
- **Showoff snowflakes**: they could almost never be collected, because of a wrong collision check in the port. Fixed, as in the original game.
- **Crowd and ambience above 60 FPS**: the crowd, its chants and the track ambience were cut above 60 frames per second, most noticeably in the end-of-race cinematic. They are back, as in the original game. You may hear more ambience in races than before: that is the original.
- **Rider's voice in the end-of-race close-up**: the rider now speaks during the close-up, as in the original game.
- **Stretched picture at the start of a race**: skipping the trick screen while it opens no longer shows the picture stretched while the black bars slide away.
- **Long sessions**: after about five events in a row without quitting, the game ran out of room to open its disc files. The next track loaded without its world (or with parts missing), then the game crashed. Files are now closed properly.
- **Original behaviour restored**: three small things the port had lost (how a rider is put back on the track, one collision case and one safety case). In some situations an AI rider may be put back slightly differently than before, as in the original game.
- **HUD speed at high frame rates**: the race HUD and the race menus now run at the right speed above 60 FPS, as at 60.
- **HUD jumps during a trick**: the score, the boost and the Tricky meter no longer jump back to their old position for a frame or two.
- **Start during the intro videos**: after the EA logo, a press of Start (Enter on the keyboard) could be missed now and then on some PCs, because the video waited on the graphics driver. It is now seen at the first press. The EA logo itself can't be skipped, as in the original game.
- **VSync *Adaptive* tears less**: it only tears when the game stays slow for a while (about half a second); a load or a one-off hitch never counts. *On* and *Off* are unchanged.
- **Folders with accents**: with a save folder or a game folder whose name has accents or other letters outside English, saves could fail, or go to a wrongly named copy of the folder. Paths are now handled in full. Saves that 0.1.1 wrote to such a copy are **copied** to the right folder at the first start, only when the right folder has no saves yet; no file is replaced, moved or deleted, and the old folder stays as it is.
- **Wine and Proton (Steam Deck, Linux)**: the game could freeze before the first picture, or crash when you pressed *Play*, when Wine had already taken the memory area it needs. It now picks another one. Not confirmed by a player on a Steam Deck or Linux yet. Nothing changes on Windows.
- ***Skip launcher next time*** with a disc image the game can't use: the launcher and the game no longer reopen each other. The launcher shows up with "The game could not use this disc image" and what to do.
- **Cross and Start always have a key**: the launcher won't leave their keyboard key empty, and if `settings.ini` has no key for them, the game uses Space or Enter (when free) and says so in the log.
- **Keys of the game window**: *Alt+Enter* (full screen) no longer presses Start, and *Alt+Tab* no longer presses Select. The launcher won't bind F10, F11 or F12 (F11: full screen, F12: screenshot).
- ***Reset tab*** (*Game* tab) no longer forgets your save folder.
- **`settings.ini`**: lines the launcher doesn't know (from a newer or an older version) are kept when it saves, instead of being dropped.
- **Replay help**: Select is now called *SELECT*, as on the PS2, and its lines read *Change Camera* and *Exit*.
- **Crash reports**: folder names, possibly including your Windows user name, could stay in the crash dump (`crash.dmp`) and in `settings.txt` / `log.txt`. They are now removed in every form, and the names you chose (disc image, save folder, HD pack folder) are hidden; the names of the game's own files stay.
- Known issues listed for 0.1.1 that are now fixed: the doubled picture at the start of a race, the replay help calling Select *BACK*, and most of the presses of Start ignored during the opening logo video (the rest came from the EA logo, which can't be skipped in the original game either).

### New
- **Save profile** (launcher, *Game* tab): *My save* (your own progress, as before) or *Unlock all*, a separate save with everything unlocked (the game's own unlock cheat), kept in an `UnlockAll` folder inside your save folder. Your own save is never touched; back on *My save*, your progress is there as you left it.
- **Shader cache**: compiled shaders are kept on disk (in a `cache` folder next to `settings.ini`), so there are fewer small hitches from the second start on. Anything unusable is simply recompiled.
- **Notes under the settings** (launcher): a short yellow note when a setting has no effect on your screen, or doesn't do what you might think. For example: *Menus* at 4:3, *Field of view* up to 16:9, *Xbox 16:9* menus on a screen narrower than 16:9, a *Frame rate limit* above your screen's rate with VSync, *Show frame rate* in fullscreen, *HD textures* without a pack, *Update channel* while updates are off, a *Resolution* or *Screen shape* that doesn't match your screen.
- **A banner** at the top of the settings when `settings.ini` couldn't be read as written: lines not understood (named), a file from a newer version, no `[Meta] Version` line, or a file that can't be read. The game's log lists the ignored lines too.
- ***Restore*** now asks before going back to the previous version; *Cancel* changes nothing.
- **Signed releases**: the launcher can check a release's signature. It will be used from a later, signed release.
- The launcher's disc check now matches the game's: an *SSX Tricky* (USA) disc image with a changed game program is shown as the wrong disc image, with the reason.

### Better
- **New defaults**: *Speakers* is *Stereo*, and *Smoother legs in turns* is on (*Settings → Advanced*, marked experimental; it changes the picture only). The *Original Xbox* preset turns it off; *Recommended* and *Steam Deck* keep it on.
- **Install folder**: the game is now `bin\SSX Tricky.exe`; the top of the folder keeps `OpenTricky.exe`, `README.txt` and your own files (settings, saves, screenshots, crash reports). **Shortcuts to the old `SSX Tricky.exe` need to be made again** (to `OpenTricky.exe`, or to `bin\SSX Tricky.exe` to start the game straight away). Coming from 0.1.1: install 0.1.2 fresh in a new folder; copy the `Saves` folder (and `portable.txt`) only if your saves were inside the old folder, don't copy `settings.ini` (the new defaults are recommended), then delete the old folder.
- ***Resolution*** lists *Auto* and the sizes of the chosen *Screen shape* only. Changing the screen shape sets *Resolution* back to *Auto*.
- **Presets**: *Original Xbox* now also sets *Race HUD size* to *Like the Xbox*; *Recommended* and *Steam Deck* set it back to 85 %.
- **Key conflicts** in the launcher: the dialog offers *Swap* or *Cancel*.
- **Training**: the loading screen is black with "loading...", like the other loading screens, instead of an Xbox controller picture.

### Removed
- `SSX Tricky.exe` at the top of the folder (now in `bin\`).
- The Xbox controller picture and its labels on the Training loading screen.
- *Use … for both* in the launcher's key conflict dialog.

### Known issues
- **Performance**: frame rate drops when riders collide; Snowdream runs slower than other tracks.
- Only Xbox-style (XInput) controllers are supported natively, so *Button style: Auto* shows Xbox buttons.
- **Steam Deck and Linux** (Proton): 0.1.2 should start there now, but no player has confirmed it yet, the *Steam Deck* preset included.
- **5.1 surround** is not yet tested on real 5.1 speakers.
- **Smoother legs in turns** is experimental.
- **Terrain**: some pieces of terrain show visible seams, as in the original game.

See also [Known issues](README.md#known-issues) in the README.

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
- **Above 60 FPS, the controls answer one game step later** (about 17 ms). Set *Frame rate limit* to *60* for the Xbox's exact timing.
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
