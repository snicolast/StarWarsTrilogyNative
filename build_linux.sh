#!/bin/sh
# Builds Star Wars Trilogy Native on Linux (x86_64 or arm64): extracts the ROM,
# recompiles it, builds and leaves ./StarWarsTrilogy here, next to data/, res/
# and third_party/ which it plays from.
#
#   ./build_linux.sh [swtrilgy.zip]                  for this computer
#   ./build_linux.sh --arch aarch64 [swtrilgy.zip]   arm64 from an x86_64 PC (docs/LINUX.md)
#   ./build_linux.sh --package ...                   also a folder to copy to another of your computers
#   ./build_linux.sh --appimage ...                  also that folder as one AppImage (tools/make_appimage.sh)
#   ./build_linux.sh --glibc 2.34 ...                for systems as old as that glibc, not only ones as new as
#                                                    this one (zig cc, fetched into build/tools; --appimage
#                                                    builds for 2.34 unless told another, or "host")
#
# The ROM: the path given, else SWT_ROM_ZIP, else swtrilgy.zip here or in roms/
# (only needed the first time).
set -e
cd "$(dirname "$0")"

fail() {
    echo "$1" >&2
    exit 1
}

HOST=$(uname -m)
ARCH=$HOST
PACKAGE=0
APPIMAGE=0
GLIBC=
ZIP=$SWT_ROM_ZIP
while [ $# -gt 0 ]; do
    case "$1" in
    --arch) ARCH=$2; shift 2 ;;
    --arch=*) ARCH=${1#--arch=}; shift ;;
    --package) PACKAGE=1; shift ;;
    --appimage) PACKAGE=1; APPIMAGE=1; shift ;;
    --glibc) GLIBC=$2; shift 2 ;;
    --glibc=*) GLIBC=${1#--glibc=}; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) ZIP=$1; shift ;;
    esac
done
case "$ARCH" in
arm64) ARCH=aarch64 ;;
amd64) ARCH=x86_64 ;;
esac
[ "$ARCH" = x86_64 ] || [ "$ARCH" = aarch64 ] || fail "Unknown architecture $ARCH: x86_64 or aarch64."
# An AppImage is made to be carried to other systems: not tied to this one's glibc
[ "$APPIMAGE" = 1 ] && [ -z "$GLIBC" ] && GLIBC=2.34
[ "$GLIBC" = host ] && GLIBC=
case "$GLIBC" in
""|2.[0-9]|2.[0-9][0-9]) ;;
*) fail "Unknown glibc version $GLIBC: 2.34 for instance, or host." ;;
esac

command -v python3 >/dev/null 2>&1 || fail "Python 3 not found: install python3 with your package manager."

if [ -z "$ZIP" ]; then
    for z in swtrilgy.zip roms/swtrilgy.zip; do
        [ -f "$z" ] && ZIP=$z && break
    done
fi
# drive_program.bin too: data/ from an older build can lack it (a set without
# epr-21119), and the game then stops at FAILED FEEDBACK LEVER INITIALIZE
if [ ! -f data/crom.bin ] || [ ! -f data/drive_program.bin ]; then
    [ -f "$ZIP" ] || fail "ROM not found: pass the path to swtrilgy.zip (./build_linux.sh ~/roms/swtrilgy.zip), set SWT_ROM_ZIP, or put it next to build_linux.sh."
    python3 tools/romtool.py "$ZIP" data
fi
python3 tools/recomp.py data/crom.bin build/gen

# CMake and Ninja: the ones installed, else private copies under build/ (nothing
# outside this folder changes)
tools_pip() {
    if [ ! -x build/tools/bin/pip ]; then
        mkdir -p build
        python3 -m venv build/tools || fail "Could not make build/tools: install python3-venv (or cmake, ninja and zig themselves)."
    fi
    build/tools/bin/pip -q install "$@"
}
CMAKE=$(command -v cmake || true)
NINJA=$(command -v ninja || true)
if [ -z "$CMAKE" ] || [ -z "$NINJA" ]; then
    if [ ! -x build/tools/bin/ninja ]; then
        echo "Fetching CMake and Ninja into build/tools..."
        tools_pip cmake==4.4.4 ninja==1.13.2   # glslang needs CMake 3.27 or later
    fi
    CMAKE=$PWD/build/tools/bin/cmake
    NINJA=$PWD/build/tools/bin/ninja
fi
CTEST=$(dirname "$CMAKE")/ctest
[ "$ARCH" = "$HOST" ] || { [ "$ARCH" = aarch64 ] && [ "$HOST" = x86_64 ]; } ||
    fail "Cross-building is only set up from x86_64 to aarch64."

BUILD=build/linux-$ARCH${GLIBC:+-glibc$GLIBC}
set -- -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$NINJA"
if [ -n "$GLIBC" ]; then
    # --glibc: zig cc, a clang that carries the symbols of every glibc and links
    # its own C++ library into the program. What it builds needs nothing newer
    # than that glibc, whatever this system has
    if [ ! -x build/tools/bin/python ] || ! build/tools/bin/python -c "import ziglang" 2>/dev/null; then
        echo "Fetching zig into build/tools..."
        tools_pip ziglang==0.15.2
    fi
    ZIG="$PWD/build/tools/bin/python -m ziglang"
    ZDIR=$PWD/$BUILD/zig
    mkdir -p "$ZDIR"
    # CMake finds X11's headers and the others' in /usr/include, and that folder
    # before zig's own would bring this system's glibc headers with it: it goes
    # after them instead
    for tool in cc c++; do
        cat > "$ZDIR/$tool" <<EOF
#!/bin/sh
export ZIG_GLOBAL_CACHE_DIR="$PWD/build/zig-cache" ZIG_LOCAL_CACHE_DIR="$PWD/build/zig-cache"
skip=0
for a do
    shift
    if [ \$skip = 1 ]; then
        skip=0
        [ "\$a" = /usr/include ] || set -- "\$@" -isystem "\$a"
        continue
    fi
    case \$a in
    -I/usr/include|-isystem/usr/include) continue ;;
    -isystem) skip=1; continue ;;
    esac
    set -- "\$@" "\$a"
done
exec $ZIG $tool -target $ARCH-linux-gnu.$GLIBC -fno-sanitize=undefined -g0 "\$@" -idirafter /usr/include -idirafter /usr/include/$ARCH-linux-gnu
EOF
    done
    for tool in ar ranlib; do
        printf '#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR="%s" ZIG_LOCAL_CACHE_DIR="%s"\nexec %s %s "$@"\n' \
            "$PWD/build/zig-cache" "$PWD/build/zig-cache" "$ZIG" $tool > "$ZDIR/$tool"
    done
    chmod +x "$ZDIR/cc" "$ZDIR/c++" "$ZDIR/ar" "$ZDIR/ranlib"
    {
        [ "$ARCH" = "$HOST" ] || echo "include(\"$PWD/cmake/linux-aarch64.cmake\")"
        cat <<EOF
set(CMAKE_C_COMPILER "$ZDIR/cc")
set(CMAKE_CXX_COMPILER "$ZDIR/c++")
unset(CMAKE_C_COMPILER_TARGET)
unset(CMAKE_CXX_COMPILER_TARGET)
set(CMAKE_EXE_LINKER_FLAGS_INIT "")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "")
set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)   # zig's linker has no --dependency-file
set(CMAKE_AR "$ZDIR/ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "$ZDIR/ranlib" CACHE FILEPATH "" FORCE)
EOF
    } > "$ZDIR/toolchain.cmake"
    # -s: zig keeps debugging data unless told (six times the size)
    set -- "$@" -DCMAKE_TOOLCHAIN_FILE="$ZDIR/toolchain.cmake" -DCMAKE_EXE_LINKER_FLAGS=-s
elif [ "$ARCH" != "$HOST" ]; then
    set -- "$@" -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/linux-aarch64.cmake"
elif command -v clang++ >/dev/null 2>&1; then
    # clang as on macOS (the generated files are large: it takes them well); g++ works too
    set -- "$@" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
fi
"$CMAKE" -S . -B "$BUILD" -G Ninja "$@"
"$CMAKE" --build "$BUILD"
"$CTEST" --test-dir "$BUILD" --output-on-failure

# The program is compiled from your ROM's code: it is yours alone, do not share it
if [ "$ARCH" = "$HOST" ]; then
    cp -f "$BUILD/StarWarsTrilogy" ./StarWarsTrilogy
    echo
    echo "Done: ./StarWarsTrilogy"
fi

# A folder with everything the game plays from, for another of your own
# computers (copy it whole, then run ./StarWarsTrilogy in it). It holds your
# ROM's data and code: keep it to yourself
if [ "$PACKAGE" = 1 ]; then
    OUT=dist/StarWarsTrilogy-linux-$ARCH
    rm -rf "$OUT"
    mkdir -p "$OUT/third_party"
    cp "$BUILD/StarWarsTrilogy" "$OUT/"
    cp -R data res news.txt "$OUT/"
    cp -R third_party/fonts "$OUT/third_party/"
    [ -d third_party/menu ] && cp -R third_party/menu "$OUT/third_party/"
    NEEDS="a system as recent as the one it was built on"
    [ -n "$GLIBC" ] && NEEDS="glibc $GLIBC or later"
    cat > "$OUT/README.txt" <<EOF
Star Wars Trilogy Native for Linux $ARCH, built $(date +%Y-%m-%d) from your own swtrilgy.zip.

Run ./StarWarsTrilogy in this folder. It needs $NEEDS
and a graphics driver with Vulkan 1.3, or 1.2 and VK_KHR_dynamic_rendering
(Mesa 24.2 on a Raspberry Pi 5), with the Vulkan loader (libvulkan1).
Settings, records and saves are kept in this folder.

This folder holds data and code from your ROM: for your own computers only.
Sharing it breaks the project's license (LICENSE) and Sega's copyright.
EOF
    echo "Package: $OUT"
fi
[ "$APPIMAGE" = 1 ] && sh tools/make_appimage.sh "$ARCH"
exit 0
