#!/bin/sh
# Builds Star Wars Trilogy Native on macOS (Apple silicon): extracts the ROM,
# recompiles it, builds with clang and makes "Star Wars Trilogy.app" here.
# Usage: ./build_mac.sh [path to swtrilgy.zip]   (or set SWT_ROM_ZIP, or put it
# next to this file or in roms/; only needed the first time)
set -e
cd "$(dirname "$0")"

fail() {
    echo "$1" >&2
    exit 1
}

xcrun --find clang++ >/dev/null 2>&1 || fail "The C++ tools are missing: run  xcode-select --install  and try again."
command -v python3 >/dev/null 2>&1 || fail "Python 3 not found: install it with  brew install python  (or from python.org)."

ZIP=${1:-$SWT_ROM_ZIP}
if [ -z "$ZIP" ]; then
    for z in swtrilgy.zip roms/swtrilgy.zip; do
        [ -f "$z" ] && ZIP=$z && break
    done
fi
# drive_program.bin too: data/ from an older build can lack it (a set without
# epr-21119), and the game then stops at FAILED FEEDBACK LEVER INITIALIZE
if [ ! -f data/crom.bin ] || [ ! -f data/drive_program.bin ]; then
    [ -f "$ZIP" ] || fail "ROM not found: pass the path to swtrilgy.zip (./build_mac.sh ~/roms/swtrilgy.zip), set SWT_ROM_ZIP, or put it next to build_mac.sh."
    python3 tools/romtool.py "$ZIP" data
fi
python3 tools/recomp.py data/crom.bin build/gen

# CMake and Ninja: the ones installed, else private copies under build/ (nothing
# outside this folder changes)
CMAKE=$(command -v cmake || true)
NINJA=$(command -v ninja || true)
if [ -z "$CMAKE" ] || [ -z "$NINJA" ]; then
    if [ ! -x build/tools/bin/ninja ]; then
        echo "Fetching CMake and Ninja into build/tools..."
        mkdir -p build
        python3 -m venv build/tools
        build/tools/bin/pip -q install cmake ninja
    fi
    CMAKE=$PWD/build/tools/bin/cmake
    NINJA=$PWD/build/tools/bin/ninja
fi
CTEST=$(dirname "$CMAKE")/ctest

"$CMAKE" -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0   # as LSMinimumSystemVersion below
"$CMAKE" --build build/cmake
"$CTEST" --test-dir build/cmake --output-on-failure

# The application: the program, MoltenVK and everything the game reads (the ROM
# data too: it is yours alone, do not share the application). Settings and
# records go to ~/Library/Application Support, so they outlive this rebuild
APP="Star Wars Trilogy.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/third_party"
cp build/cmake/StarWarsTrilogy build/cmake/libMoltenVK.dylib "$APP/Contents/MacOS/"
cp -R res data news.txt "$APP/Contents/Resources/"
rm -f "$APP/Contents/Resources/data/menu_music.wav"   # decoded at the first launch
cp -R third_party/fonts "$APP/Contents/Resources/third_party/"
[ -d third_party/menu ] && cp -R third_party/menu "$APP/Contents/Resources/third_party/"
python3 tools/ico_to_png.py res/app.ico build/app_icon.png
sips -s format icns build/app_icon.png --out "$APP/Contents/Resources/app.icns" >/dev/null
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Star Wars Trilogy</string>
    <key>CFBundleIdentifier</key><string>com.snicolast.starwarstrilogynative</string>
    <key>CFBundleExecutable</key><string>StarWarsTrilogy</string>
    <key>CFBundleIconFile</key><string>app</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.arcade-games</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
codesign --force --deep --sign - "$APP" >/dev/null 2>&1
echo
echo "Done: open \"$APP\""
