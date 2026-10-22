# macOS test drivers

`drive.py` starts the game with the diag console (`XBOX_DIAG_PORT`, port 47011 by default) and runs a script of steps:

    python3 macos/tests/drive.py "$(cat macos/tests/to_venue.txt); press A 200; wait 10; shot a1; wait 60; shot a2"

Steps: `wait s`, `press BUTTON [ms]`, `hold BUTTON s`, `shot name` (PNG into --out), `seq BUTTON@every x count`, `burst name count interval`,
`waitlog text_with_underscores timeout`, `until text BUTTON every timeout` (press BUTTON until the game log has the text; e.g. `until level_load A 4 140` then
`until 3_->_4 A 3 100` walks from the menus to the end of the start countdown, whatever the menu timing).
`to_venue.txt` walks a fresh profile from the title screen to the venue list (Single Event, Race, Amateur, Garibaldi).
Exit 1 and the crash report if the game died. `--bin` picks another build, e.g. the bounds-checked one:

    cmake -S port -B ../build/ubsan -G Ninja -DCMAKE_BUILD_TYPE=Release -DOT_GPU=ON -DOT_HEADLESS=ON -DOT_WINDOW=ON \
      -DCMAKE_C_FLAGS="-Wno-error=int-conversion -w -fsanitize=bounds,object-size" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=bounds,object-size"

`hdr_test`, `clip_test`: see their sources. The diag console accepts `press`, `shot`, `help`, `quit`.

`pacing_check.py` runs the real game twice (pacing on, then the original) and checks the loading-screen hold and the start-countdown length from the `[PACING]` log lines;
it fails (exit 1) if the pacing is broken (checked with a control run that expects 1.5x while the game runs at 1x).
