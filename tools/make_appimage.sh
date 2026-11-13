#!/bin/sh
# Makes dist/StarWarsTrilogy-<arch>.AppImage from the folder build_linux.sh
# --package left in dist/ (build_linux.sh --appimage runs it). One file to
# copy to another of your own computers and run; it holds your ROM's data and
# code: keep it to yourself.
# appimagetool and the AppImage runtime (static: no libfuse2 needed) are
# fetched once into build/tools/appimage, at the versions and hashes below.
# Usage: tools/make_appimage.sh x86_64|aarch64
set -e
cd "$(dirname "$0")/.."
ARCH=$1
[ "$ARCH" = x86_64 ] || [ "$ARCH" = aarch64 ] || { echo "Usage: $0 x86_64|aarch64" >&2; exit 1; }
SRC=dist/StarWarsTrilogy-linux-$ARCH
[ -x "$SRC/StarWarsTrilogy" ] || { echo "$SRC is missing: run build_linux.sh --package first" >&2; exit 1; }
for tool in curl file sha256sum; do   # file: appimagetool asks for it
    command -v $tool >/dev/null 2>&1 || { echo "The $tool command is missing: install it and run this again." >&2; exit 1; }
done

TOOLS=build/tools/appimage
mkdir -p "$TOOLS"
fetch() {   # file URL SHA256
    if [ ! -f "$TOOLS/$1" ] || ! echo "$3  $TOOLS/$1" | sha256sum -c --quiet - 2>/dev/null; then
        echo "Fetching $1..."
        curl -sfL -o "$TOOLS/$1" "$2"
        echo "$3  $TOOLS/$1" | sha256sum -c --quiet - || { rm -f "$TOOLS/$1"; echo "$1: wrong checksum" >&2; exit 1; }
    fi
}
# appimagetool for the computer this runs on (it packs for either)
HOST=$(uname -m)
case $HOST in
x86_64) TOOL_HASH=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 ;;
aarch64) TOOL_HASH=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158 ;;
*) echo "appimagetool runs on x86_64 or aarch64, not $HOST" >&2; exit 1 ;;
esac
TOOL=appimagetool-$HOST.AppImage
fetch "$TOOL" https://github.com/AppImage/appimagetool/releases/download/1.9.1/$TOOL "$TOOL_HASH"
case $ARCH in
x86_64) RUNTIME_HASH=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d ;;
aarch64) RUNTIME_HASH=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444 ;;
esac
fetch runtime-$ARCH https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-$ARCH "$RUNTIME_HASH"
chmod +x "$TOOLS/$TOOL"

# The AppDir: the program, then what it plays from (what Linux does not use left out:
# the Windows icon and resources, the splash video; the menus' music is made at the first start)
APPDIR=build/appdir-$ARCH
SHARE=$APPDIR/usr/share/StarWarsTrilogyNative
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$SHARE/data" "$SHARE/third_party"
cp "$SRC/StarWarsTrilogy" "$APPDIR/usr/bin/"
command -v llvm-strip >/dev/null 2>&1 && llvm-strip "$APPDIR/usr/bin/StarWarsTrilogy"
cp "$SRC"/data/*.bin "$SHARE/data/"
cp -R "$SRC/res" "$SRC/news.txt" "$SRC/README.txt" "$SHARE/"
rm -f "$SHARE/res/app.ico" "$SHARE/res/app.rc" "$SHARE/res/menu/splash_snicolast.mp4"
cp -R "$SRC/third_party/fonts" "$SHARE/third_party/"
[ -d "$SRC/third_party/menu" ] && cp -R "$SRC/third_party/menu" "$SHARE/third_party/"
cp tools/linux/AppRun "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
python3 tools/ico_to_png.py res/app.ico "$APPDIR/StarWarsTrilogyNative.png"
cat > "$APPDIR/StarWarsTrilogyNative.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Star Wars Trilogy Arcade
Comment=Sega's 1998 arcade game, recompiled (Star Wars Trilogy Native)
Exec=StarWarsTrilogy
Icon=StarWarsTrilogyNative
Categories=Game;ArcadeGame;
Terminal=false
EOF

OUT=dist/StarWarsTrilogy-$ARCH.AppImage
rm -f "$OUT"
# Unpacked rather than mounted: no FUSE needed where this runs (containers too)
ARCH=$ARCH APPIMAGE_EXTRACT_AND_RUN=1 "$TOOLS/$TOOL" --no-appstream \
    --runtime-file "$TOOLS/runtime-$ARCH" "$APPDIR" "$OUT"
rm -rf "$APPDIR"
echo "AppImage: $OUT"
