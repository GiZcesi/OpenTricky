#!/bin/bash
# SSX Tricky (OpenTricky) macOS launcher: the program an app bundle runs.
#
# - remembers the Xbox disc image (asks once, with a file dialog)
# - runs the game from ~/Library/Application Support/OpenTricky, so settings.ini, the save folder (hdd) and
#   crash reports live there and not inside the app
# - keeps the last run's console output in last-run.log
# - tells you in a dialog if the game could not start or ended abnormally
#
# Change the disc image later:  open -a "SSX Tricky" --args --choose-iso
# Settings window: shown at every start unless you turn that off in it; open it anyway with Option held, or --args --settings
APP_NAME="SSX Tricky"
HERE="$(cd "$(dirname "$0")" && pwd)"
case "$HERE" in */Contents/Resources) RES="$HERE" ;; *) RES="$HERE/../Resources" ;; esac     # run as Resources/launcher.sh (the app's stub) or from MacOS/
BIN="$RES/ssx-tricky-bin"
SUPPORT="$HOME/Library/Application Support/OpenTricky"
ISO_FILE="$SUPPORT/disc-image.txt"
LOG="$SUPPORT/last-run.log"

say() {   # say <message>: a dialog (the app has no terminal)
    osascript -e 'on run argv' -e 'display dialog (item 1 of argv) with title "SSX Tricky" buttons {"OK"} default button 1 with icon caution' -e 'end run' "$1" >/dev/null 2>&1
}

mkdir -p "$SUPPORT" || { say "Could not create $SUPPORT"; exit 1; }

# settings window + iCloud mirror of the settings and game saves (macos/launcher/ot-tool.swift); both are optional extras
TOOL="$RES/ot-tool"
[ -x "$TOOL" ] || TOOL=""
[ -n "$TOOL" ] && "$TOOL" sync pull >> "$SUPPORT/sync.log" 2>&1          # newer settings / saves from iCloud Drive, if it is there
if [ -f "$SUPPORT/sync-notice.txt" ]; then          # a settings/save conflict between Macs was found by the sync: say so once
    say "$(cat "$SUPPORT/sync-notice.txt")"; rm -f "$SUPPORT/sync-notice.txt"
fi
if [ -n "$TOOL" ]; then
    gui_args="--auto"; for a in "$@"; do [ "$a" = "--settings" ] && gui_args="--settings"; done
    "$TOOL" gui $gui_args || exit 0                                       # closed without Play: do not start the game
fi

# the game needs SDL3 and OpenSSL: bundled in Contents/Frameworks in a release app, from Homebrew in a local build
missing=""
[ -d "$RES/../Frameworks" ] || for lib in /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib /opt/homebrew/opt/openssl@4/lib/libcrypto.4.dylib /opt/homebrew/opt/sdl2-compat/lib/libSDL2-2.0.0.dylib; do
    [ -e "$lib" ] || missing="$missing $(basename "$(dirname "$(dirname "$lib")")")"
done
if [ -n "$missing" ]; then
    say "Missing Homebrew libraries:$missing

Install them in Terminal with:
brew install sdl3 sdl2-compat openssl@4"
    exit 1
fi
[ -x "$BIN" ] || { say "The game program is missing from the app (expected $BIN)."; exit 1; }

choose_iso() {
    osascript -e 'POSIX path of (choose file with prompt "Choose your SSX Tricky (USA) Xbox disc image (.iso)")' 2>/dev/null
}

ISO=""
[ -f "$ISO_FILE" ] && ISO="$(cat "$ISO_FILE")"
for a in "$@"; do [ "$a" = "--choose-iso" ] && ISO=""; done
if [ -z "$ISO" ] || [ ! -f "$ISO" ]; then
    ISO="$(choose_iso)"
    [ -n "$ISO" ] || exit 0                       # cancelled
    printf '%s' "$ISO" > "$ISO_FILE"
fi

# the game keeps its files (settings.ini, saves, crash reports) in $SUPPORT: OT_HOME_DIR says so, the program itself stays in the app bundle
export OT_HOME_DIR="$SUPPORT"

# display settings: ~/Library/Application Support/OpenTricky/display.env (the settings window writes it; you can also edit it), lines like
#   OT_SHAPE=21:9        auto (default) | 4:3 | 16:9 | 21:9 | 32:9 | a number
#   OT_RENDER_H=1440     height of the picture the GPU draws (default 1080 wide, 960 at 4:3)
#   OT_BUTTONS=playstation  button pictures in the menus and HUD: modern (Xbox Series) | playstation | ps2 (default: the game's own)
#   OT_HD_TEXTURES=/path/to/pack   your HD texture pack folder (the DDS files; not included); OT_HD_MENUS=1 also for menus
#   OT_HDR=on            HDR / wide-gamut output on an HDR display: saturated colours (oranges, yellows) reach what the panel can show
#                        (tune: OT_HDR_GAMUT=0..1 default 1, OT_HDR_WARM=0..0.5 default 0.1, OT_HDR_BOOST=0..1 default 0.35)
#   OT_HDR_SCENE=0.45     how much of the HDR gamut expansion the 3D picture gets (the HUD and menus get all of OT_HDR_GAMUT): 1 = same as the HUD
#   OT_SMAA=high         smoother edges on the 3D picture (HUD and menus stay sharp): off (default) | low | medium | high | ultra
#   OT_SOFT_SHADOWS=on   soft shadow edges (default: off)
#   OT_FPS=120           frame rate limit: 60 (default) | 120 | 144 | 240 | a number | 0 (no limit) | monitor (your display's rate)
#   OT_VSYNC=vsync       vsync | adaptive | off (default: vsync at 60; present at once above 60)
#   OT_FULLSCREEN=1      start in full screen (F11 or Alt+Enter toggles; F12 saves a screenshot to ~/Pictures/OpenTricky)
# Read as data, never run: the file can arrive from iCloud Drive, so only KEY=value lines with an OT_ / XBOX_ name are taken.
# CR-tolerant (a file saved with Windows line endings); values may be 'single quoted' (the settings window does that) or "double quoted".
load_env() {
    local line k v m sq="'\\''" q="'"
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        [[ "$line" =~ ^[[:space:]]*((OT|XBOX)_[A-Za-z0-9_]+)[[:space:]]*=(.*)$ ]] || continue
        k="${BASH_REMATCH[1]}"; v="${BASH_REMATCH[3]}"; v="${v#"${v%%[![:space:]]*}"}"      # value without leading blanks
        if [[ "$v" =~ ^\'(.*)\'$ ]]; then m="${BASH_REMATCH[1]}"; v="${m//"$sq"/$q}"
        elif [[ "$v" =~ ^\"(.*)\"$ ]]; then v="${BASH_REMATCH[1]}"; fi
        export "$k=$v"
    done < "$1"
}
[ -f "$SUPPORT/display.env" ] && load_env "$SUPPORT/display.env"

[ -n "$TOOL" ] && export OT_TOOL="$TOOL"             # the game opens the settings window with it (Cmd+, or F2)
export OT_ASSET_DIR="$RES/assets"      # the button pictures and the HD index (resources.txt lists them)
cd "$SUPPORT" || exit 1
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.prev"
rm -f "$SUPPORT/crash.txt.old"; [ -f "$SUPPORT/crash.txt" ] && mv -f "$SUPPORT/crash.txt" "$SUPPORT/crash.txt.old"    # a report shown below is from this run
"$BIN" --direct "$ISO" > "$LOG" 2>&1 &
child=$!
trap 'kill -TERM "$child" 2>/dev/null' TERM INT HUP     # quitting the app (Dock, shutdown) stops the game too, not just this wrapper
wait "$child"
rc=$?
trap - TERM INT HUP
[ -n "$TOOL" ] && "$TOOL" sync push >> "$SUPPORT/sync.log" 2>&1         # the saves from this session to iCloud Drive (also after a crash)
# 0 = window closed; 130, 137, 143 = interrupted, force-quit, terminated by the user or the system: not a crash
if [ $rc -ne 0 ] && [ $rc -ne 130 ] && [ $rc -ne 137 ] && [ $rc -ne 143 ]; then
    say "The game ended unexpectedly (exit code $rc).

Its output is in:
$LOG
$( [ -f "$SUPPORT/crash.txt" ] && echo "and a crash report in $SUPPORT/crash.txt" )"
fi
exit $rc
