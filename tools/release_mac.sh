#!/bin/sh
# Build LuminousQuake.app for other Macs: universal (Apple silicon and Intel),
# macOS 11 or later, with its own SDL3. Sign it with Developer ID and the hardened
# runtime, notarize and staple it, and wrap it in a signed, notarized disk image.
# GPL-2.0-or-later.
#
#   tools/release_mac.sh --identity "Developer ID Application: NAME (TEAMID)" \
#       --notary-profile luminousquake
#
# One-time setup, done by the developer (the password never passes through here):
#   xcrun notarytool store-credentials luminousquake --apple-id ID --team-id TEAMID
# --skip-notarize signs and packages only; use it to try the build with any
# signing identity (Apple Development builds run only on registered Macs).
set -eu

identity=""
profile=""
notarize=1
while [ $# -gt 0 ]; do
    case "$1" in
        --identity) identity="$2"; shift 2 ;;
        --notary-profile) profile="$2"; shift 2 ;;
        --skip-notarize) notarize=0; shift ;;
        *) echo "usage: $0 --identity NAME [--notary-profile PROFILE | --skip-notarize]" >&2; exit 2 ;;
    esac
done
[ -n "$identity" ] || { echo "--identity is required (see: security find-identity -v -p codesigning)" >&2; exit 2; }
[ "$notarize" = 0 ] || [ -n "$profile" ] || { echo "--notary-profile is required unless --skip-notarize" >&2; exit 2; }

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build-mac-release"
sdl_tag=release-3.4.18
sdl_commit=829a65d769d935c4852f8159e964312c0957260a
archs="arm64;x86_64"
target=11.0
# Homebrew libraries are single-architecture; build only against the SDK and our SDL.
ignore="/opt/homebrew;/usr/local"

mkdir -p "$out"
if [ ! -f "$out/sdl-prefix/lib/libSDL3.0.dylib" ]; then
    rm -rf "$out/SDL3" "$out/sdl-build"
    git clone --quiet --depth 1 --branch "$sdl_tag" https://github.com/libsdl-org/SDL.git "$out/SDL3"
    [ "$(git -C "$out/SDL3" rev-parse HEAD)" = "$sdl_commit" ] || { echo "SDL $sdl_tag is not the pinned commit" >&2; exit 1; }
    cmake -S "$out/SDL3" -B "$out/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$archs" -DCMAKE_OSX_DEPLOYMENT_TARGET=$target \
        -DCMAKE_IGNORE_PREFIX_PATH="$ignore" -DCMAKE_INSTALL_PREFIX="$out/sdl-prefix" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$out/sdl-build" --parallel
    cmake --install "$out/sdl-build"
fi

cmake -S "$root" -B "$out/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_OSX_ARCHITECTURES="$archs" -DCMAKE_OSX_DEPLOYMENT_TARGET=$target \
    -DCMAKE_IGNORE_PREFIX_PATH="$ignore" -DCMAKE_PREFIX_PATH="$out/sdl-prefix"
cmake --build "$out/build" --parallel --target luminousquake-app

dist="$out/dist"
app="$dist/LuminousQuake.app"
rm -rf "$dist"
mkdir -p "$dist"
ditto "$out/build/LuminousQuake.app" "$app"
for binary in "$app/Contents/MacOS/LuminousQuake" "$app/Contents/Frameworks/libSDL3.0.dylib"; do
    for arch in arm64 x86_64; do
        lipo "$binary" -verify_arch $arch || { echo "$binary lacks $arch" >&2; exit 1; }
    done
done
if otool -L "$app/Contents/MacOS/LuminousQuake" | grep -q -e /opt/homebrew -e /usr/local; then
    echo "The executable still links a library outside the bundle" >&2; exit 1
fi

# Sign from the inside out: the bundled library, then the app.
codesign --force --timestamp --options runtime --sign "$identity" "$app/Contents/Frameworks/libSDL3.0.dylib"
codesign --force --timestamp --options runtime --sign "$identity" "$app"
codesign --verify --strict --deep --verbose=2 "$app"

version=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$app/Contents/Info.plist")
zip="$out/LuminousQuake-$version.zip"
dmg="$out/LuminousQuake-$version.dmg"
if [ "$notarize" = 1 ]; then
    rm -f "$zip"
    ditto -c -k --keepParent "$app" "$zip"
    xcrun notarytool submit "$zip" --keychain-profile "$profile" --wait
    xcrun stapler staple "$app"
    rm -f "$zip"
fi

stage="$out/dmg"
rm -rf "$stage" "$dmg"
mkdir -p "$stage"
ditto "$app" "$stage/LuminousQuake.app"
cat > "$stage/Read Me.txt" <<'EOF'
LuminousQuake needs your own copy of Quake: the 1996 shareware or registered
release. The 2021 rerelease also runs, with some features missing.

Drag LuminousQuake.app into your Quake folder, the one that contains id1, and
double-click it. Placed anywhere else, it asks you to choose your Quake folder.
Hold Option while it opens to choose a different folder.

The game data is only read, never changed. Settings and saves go to
~/Library/Application Support/LuminousQuake.

LuminousQuake is free software under the GNU GPL, version 2 or later; see
LuminousQuake.app/Contents/Resources/GPL.txt.
EOF
hdiutil create -quiet -volname "LuminousQuake $version" -srcfolder "$stage" -format UDZO -ov "$dmg"
codesign --force --timestamp --sign "$identity" "$dmg"
if [ "$notarize" = 1 ]; then
    xcrun notarytool submit "$dmg" --keychain-profile "$profile" --wait
    xcrun stapler staple "$dmg"
    spctl --assess --type execute --verbose=2 "$app"
    spctl --assess --type open --context context:primary-signature --verbose=2 "$dmg"
fi
echo "Built $dmg"
