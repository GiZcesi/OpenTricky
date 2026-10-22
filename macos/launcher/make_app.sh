#!/bin/bash
# Build "SSX Tricky.app" around a built game binary.
#   bash macos/launcher/make_app.sh [path/to/"SSX Tricky" binary] [output folder]
# Defaults: the GPU build in ../../../build/gpu and OpenTricky/dist.
#
# The app is self-contained: SDL3, sdl2-compat and OpenSSL are copied into Contents/Frameworks and the binary is pointed at them (@rpath).
# Signing:  SIGN_ID="Developer ID Application: Name (TEAMID)"  (or SIGN_ID=auto to use the first Developer ID Application identity)
#           signs every piece with the hardened runtime; without SIGN_ID the app is signed ad hoc (runs on this Mac).
# Release:  RELEASE=1 also makes dist/SSX-Tricky-<version>.dmg (signed when SIGN_ID is set). NOTARY_PROFILE=<name> (a keychain profile from
#           `xcrun notarytool store-credentials`) submits the dmg to Apple, waits, and staples the ticket.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="${1:-$ROOT/../build/gpu/SSX Tricky}"
OUT="${2:-$ROOT/dist}"
APP="$OUT/SSX Tricky.app"
[ -x "$BIN" ] || { echo "no game binary at: $BIN (build it first: bash build.sh <iso>)"; exit 1; }
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/Resources/ssx-tricky-bin"
python3 "$HERE/make_resources.py" >/dev/null
cp -R "$ROOT/port/assets" "$APP/Contents/Resources/assets"
swiftc -O -swift-version 5 "$HERE/ot-tool.swift" -o "$APP/Contents/Resources/ot-tool" 2>&1 | grep -E "error" || true
[ -x "$APP/Contents/Resources/ot-tool" ] || echo "(settings window not built: swiftc failed; the app still starts the game)"
# app icon: drawn by make_icon.swift (original artwork), packed into AppIcon.icns
ICONS="$(mktemp -d)"
if swiftc -O "$HERE/make_icon.swift" -o "$ICONS/make_icon" 2>/dev/null && "$ICONS/make_icon" "$ICONS/icon.png"; then
    mkdir "$ICONS/AppIcon.iconset"
    for sz in 16 32 128 256 512; do
        sips -z $sz $sz "$ICONS/icon.png" --out "$ICONS/AppIcon.iconset/icon_${sz}x${sz}.png" >/dev/null
        sips -z $((sz*2)) $((sz*2)) "$ICONS/icon.png" --out "$ICONS/AppIcon.iconset/icon_${sz}x${sz}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICONS/AppIcon.iconset" -o "$APP/Contents/Resources/AppIcon.icns" || echo "(icon not packed)"
else echo "(icon not drawn: swiftc failed)"; fi
rm -rf "$ICONS"
cp "$HERE/ssx-launcher.sh" "$APP/Contents/Resources/launcher.sh"
cc -O2 "$HERE/stub.c" -o "$APP/Contents/MacOS/SSX Tricky"
chmod +x "$APP/Contents/MacOS/SSX Tricky" "$APP/Contents/Resources/ssx-tricky-bin" "$APP/Contents/Resources/launcher.sh"

# bundle the libraries the game links (anything outside the system: Homebrew's SDL3, sdl2-compat, OpenSSL) into Contents/Frameworks
FW="$APP/Contents/Frameworks"; mkdir -p "$FW"
GAME="$APP/Contents/Resources/ssx-tricky-bin"
install_name_tool -add_rpath "@executable_path/../Frameworks" "$GAME" 2>/dev/null || true
bundle_deps() {          # bundle_deps <mach-o file>: copy its non-system dependencies and point it at the copies
    local f="$1" dep real base
    otool -L "$f" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        case "$dep" in /usr/lib/*|/System/*|@rpath/*|@executable_path/*|@loader_path/*) continue ;; esac
        real="$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$dep")"
        base="$(basename "$real")"
        if [ ! -f "$FW/$base" ]; then
            cp "$real" "$FW/$base"; chmod u+w "$FW/$base"
            install_name_tool -id "@rpath/$base" "$FW/$base"
            bundle_deps "$FW/$base"
        fi
        install_name_tool -change "$dep" "@rpath/$base" "$f"
    done
}
bundle_deps "$GAME"
for lib in "$FW"/*.dylib; do install_name_tool -add_rpath "@loader_path" "$lib" 2>/dev/null || true; done
# sdl2-compat loads SDL3 at start by the name "@loader_path/libSDL3.dylib" (not libSDL3.0.dylib) and aborts if it is missing
[ -f "$FW/libSDL3.0.dylib" ] && ln -sf libSDL3.0.dylib "$FW/libSDL3.dylib"
APPVER="$(sed -n "s/#define OT_VERSION_MAJOR *\([0-9]*\).*/\1/p" "$ROOT/port/src/version.h" | head -1).$(sed -n "s/#define OT_VERSION_MINOR *\([0-9]*\).*/\1/p" "$ROOT/port/src/version.h" | head -1).$(sed -n "s/#define OT_VERSION_PATCH *\([0-9]*\).*/\1/p" "$ROOT/port/src/version.h" | head -1)"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>SSX Tricky</string>
  <key>CFBundleDisplayName</key><string>SSX Tricky</string>
  <key>CFBundleIdentifier</key><string>local.opentricky.macos</string>
  <key>CFBundleExecutable</key><string>SSX Tricky</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$APPVER</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict></plist>
PLIST
# signing: a Developer ID with the hardened runtime when SIGN_ID is given, ad hoc otherwise
if [ "${SIGN_ID:-}" = "auto" ]; then SIGN_ID="$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application:[^"]*\)".*/\1/p' | head -1)"; fi
if [ -n "${SIGN_ID:-}" ]; then
    for f in "$FW"/*.dylib "$APP/Contents/Resources/ot-tool" "$GAME" "$APP/Contents/MacOS/SSX Tricky"; do
        codesign --force --timestamp --options runtime --entitlements "$HERE/entitlements.plist" -s "$SIGN_ID" "$f"
    done
    codesign --force --timestamp --options runtime --entitlements "$HERE/entitlements.plist" -s "$SIGN_ID" "$APP"
    codesign --verify --deep --strict "$APP" && echo "signed: $SIGN_ID (hardened runtime)"
else
    for f in "$FW"/*.dylib "$APP/Contents/Resources/ot-tool" "$GAME" "$APP/Contents/MacOS/SSX Tricky"; do codesign --force -s - "$f" >/dev/null 2>&1; done
    codesign --force -s - "$APP" >/dev/null 2>&1 && echo "signed ad hoc" || echo "(codesign failed; the app still runs from a local build)"
fi
# Finder and the Dock cache bundle icons by path: refresh the registration so a rebuilt app shows its current icon
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP" >/dev/null 2>&1 || true
touch "$APP"
echo "built: $APP"

if [ "${RELEASE:-}" = "1" ]; then
    V() { sed -n "s/#define OT_VERSION_$1 *\([0-9]*\).*/\1/p" "$ROOT/port/src/version.h" | head -1; }
    VER="$(V MAJOR).$(V MINOR).$(V PATCH)"
    DMG="$OUT/SSX-Tricky-$VER.dmg"; STAGE="$(mktemp -d)"
    cp -R "$APP" "$STAGE/"; ln -s /Applications "$STAGE/Applications"
    rm -f "$DMG"; hdiutil create -volname "SSX Tricky" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null; rm -rf "$STAGE"
    [ -n "${SIGN_ID:-}" ] && codesign --force --timestamp -s "$SIGN_ID" "$DMG"
    echo "disk image: $DMG"
    if [ -n "${NOTARY_PROFILE:-}" ] && [ -n "${SIGN_ID:-}" ]; then
        xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait && xcrun stapler staple "$DMG" && echo "notarized and stapled"
    else
        echo "(not notarized: needs SIGN_ID and NOTARY_PROFILE; see the header of this script)"
    fi
fi
