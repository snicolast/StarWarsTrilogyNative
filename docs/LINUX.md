# Star Wars Trilogy Native on Linux

The Linux build is maintained by contributors. It runs on x86_64 and arm64 (aarch64) with X11 or Wayland.

## Build

You need a GPU with a Vulkan 1.3 driver (or 1.2 with `VK_KHR_dynamic_rendering`), Python 3, a C++20 compiler (clang is used if there is one, else g++), CMake
3.27 or later and Ninja (or `build_linux.sh` fetches pinned copies of those two into `build/`), and the development
files SDL3 needs for the window, the sound and the gamepads. On Debian or Ubuntu:

```sh
sudo apt install clang cmake ninja-build python3 pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev \
    libpulse-dev libpipewire-0.3-dev libasound2-dev libudev-dev libdbus-1-dev libvulkan1
```

On Arch: `sudo pacman -S --needed clang cmake ninja python pkgconf libx11 libxext libxrandr libxcursor libxi libxss
libxkbcommon wayland wayland-protocols libdecor libpulse pipewire alsa-lib systemd-libs dbus vulkan-icd-loader`.

1. Put `swtrilgy.zip` in the folder (or in `roms/`, or pass its path: `./build_linux.sh ~/roms/swtrilgy.zip`).
2. Run `./build_linux.sh`. It downloads SDL3, miniz and FreeType on the first run (all three built into the program)
   and leaves `StarWarsTrilogy` in the folder.
3. Play with `./StarWarsTrilogy`, from this folder or anywhere else (it goes to its own folder).

Settings, records and saves are kept in the folder, as on Windows. The check for updates uses the system's libcurl
when there is one (`libcurl.so.4`); without it the game plays the same.

## arm64 from an x86_64 PC

`./build_linux.sh --arch aarch64` cross-compiles with clang. It needs the arm64 versions of the libraries above and
`qemu-user` (the build runs its shader compiler and the tests for arm64 through it). On Debian or Ubuntu:

```sh
sudo dpkg --add-architecture arm64 && sudo apt update
sudo apt install lld qemu-user libc6-dev:arm64 libstdc++-14-dev:arm64 libgcc-14-dev:arm64 \
    libx11-dev:arm64 libxext-dev:arm64 libxrandr-dev:arm64 libxcursor-dev:arm64 libxi-dev:arm64 libxss-dev:arm64 \
    libxfixes-dev:arm64 libxtst-dev:arm64 libxkbcommon-dev:arm64 libwayland-dev:arm64 libdecor-0-dev:arm64 \
    libegl-dev:arm64 libpulse-dev:arm64 libpipewire-0.3-dev:arm64 libasound2-dev:arm64 libudev-dev:arm64 \
    libdbus-1-dev:arm64
```

On an arm64 computer itself, `./build_linux.sh` builds for it as on x86_64.

## Older systems

A program built as above needs a system at least as recent as the one it was built on: its C library (glibc) and
its C++ library. `./build_linux.sh --glibc 2.34` (with `--arch aarch64` or not) builds one that runs on anything
with that glibc or a later one (2.34: RHEL 9, Ubuntu 22.04, Debian 12, Raspberry Pi OS Bookworm), with the C++
library inside it. It compiles with `zig cc`, fetched once into `build/tools` (it needs `python3-venv`); the
development files are the same.

## Raspberry Pi 5

The arm64 build is made with the Raspberry Pi 5 in mind (built on it with `./build_linux.sh`, or on a PC with
`--arch aarch64`), with these limits:

- The GPU's driver (Mesa's v3dv) has Vulkan 1.3 from Mesa 24.3 (**Raspberry Pi OS Trixie**); with Mesa 24.2
  (Bookworm, updated) it has Vulkan 1.2 and dynamic rendering as an extension, which the game takes too (it then
  says `Vulkan 1.2 with VK_KHR_dynamic_rendering`). With an older driver the game names it and its Vulkan version
  when it starts. If the log says the picture is drawn `on the processor` (Mesa's llvmpipe), no GPU driver was
  usable: update the system.
- Built on the Pi itself the program fits its system; built on a PC, `--glibc 2.34` makes one for Bookworm and
  Trixie alike.
- The GPU has no descriptor indexing: the game runs without the texture pack and the 2D replacement pictures (the
  game's own textures and text), and without ray tracing (the shadow map draws the sun shadows).
- It is far slower than a desktop GPU: start with **Options > Display > Quality > LOW**, anti-aliasing off, a
  1280x720 or 1920x1080 screen.

## Another of your computers

`./build_linux.sh --package` (with `--arch aarch64` or not) also makes `dist/StarWarsTrilogy-linux-<arch>/`: the
program with everything it plays from (the data extracted from your ROM included). Copy that folder whole to another
of your own computers and run `./StarWarsTrilogy` in it. It holds your ROM's data and code: keep it to yourself.

Or `./build_linux.sh --appimage` (with `--arch aarch64` or not): the same as one file,
`dist/StarWarsTrilogy-<arch>.AppImage` (made by `tools/make_appimage.sh`, which fetches a pinned appimagetool once),
built for glibc 2.34 as above unless `--glibc` says another (`--glibc host`: this system's own compiler). It
needs the `curl` and `file` commands.
Run it from anywhere; it plays from `~/.local/share/StarWarsTrilogyNative` (or `$SWT_HOME`), where settings, records
and saves stay from one version to the next. It holds your ROM too: keep it to yourself.

## Compared with Windows

- Ray tracing works as on Windows, with a driver that has ray queries (NVIDIA, recent AMD and Intel with Mesa).
- No DLAA yet: picking it uses MSAA, as on a GPU without it. **Options > Display > Supersampling** (1.5x, 2x) draws the 3D
  larger and scales it down, for smoother edges still on a fast GPU.
- No splash video: the splash picture shows in its place.
- One mouse: a second player uses a gamepad.
- UPDATE NOW opens the game's page: update the folder (`git pull`) and run `build_linux.sh` again.

F11 or Alt+Enter toggles fullscreen.

For how the port works, see `docs/ARCHITECTURE.md`.
