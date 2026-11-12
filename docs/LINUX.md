# Star Wars Trilogy Native on Linux

The Linux build is maintained by contributors. It runs on x86_64 and arm64 (aarch64) with X11 or Wayland.

## Build

You need a GPU with a Vulkan 1.3 driver, Python 3, a C++20 compiler (clang is used if there is one, else g++), CMake
3.27 or later and Ninja (or `build_linux.sh` fetches pinned copies of those two into `build/`), and the development
files SDL3 needs for the window, the sound and the gamepads, plus FreeType and libcurl. On Debian or Ubuntu:

```sh
sudo apt install clang cmake ninja-build python3 pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev \
    libpulse-dev libpipewire-0.3-dev libasound2-dev libudev-dev libdbus-1-dev \
    libfreetype-dev libcurl4-openssl-dev libvulkan1
```

On Arch: `sudo pacman -S --needed clang cmake ninja python pkgconf libx11 libxext libxrandr libxcursor libxi libxss
libxkbcommon wayland wayland-protocols libdecor libpulse pipewire alsa-lib systemd-libs dbus freetype2 curl vulkan-icd-loader`.

1. Put `swtrilgy.zip` in the folder (or in `roms/`, or pass its path: `./build_linux.sh ~/roms/swtrilgy.zip`).
2. Run `./build_linux.sh`. It downloads SDL3 and miniz on the first run and leaves `StarWarsTrilogy` in the folder.
3. Play with `./StarWarsTrilogy`, from this folder or anywhere else (it goes to its own folder).

Settings, records and saves are kept in the folder, as on Windows.

## arm64 from an x86_64 PC

`./build_linux.sh --arch aarch64` cross-compiles with clang. It needs the arm64 versions of the libraries above and
`qemu-user` (the build runs its shader compiler and the tests for arm64 through it). On Debian or Ubuntu:

```sh
sudo dpkg --add-architecture arm64 && sudo apt update
sudo apt install lld qemu-user libc6-dev:arm64 libstdc++-14-dev:arm64 libgcc-14-dev:arm64 \
    libx11-dev:arm64 libxext-dev:arm64 libxrandr-dev:arm64 libxcursor-dev:arm64 libxi-dev:arm64 libxss-dev:arm64 \
    libxfixes-dev:arm64 libxtst-dev:arm64 libxkbcommon-dev:arm64 libwayland-dev:arm64 libdecor-0-dev:arm64 \
    libegl-dev:arm64 libpulse-dev:arm64 libpipewire-0.3-dev:arm64 libasound2-dev:arm64 libudev-dev:arm64 \
    libdbus-1-dev:arm64 libfreetype-dev:arm64 libcurl4-openssl-dev:arm64
```

On an arm64 computer itself, `./build_linux.sh` builds for it as on x86_64.

## Another of your computers

`./build_linux.sh --package` (with `--arch aarch64` or not) also makes `dist/StarWarsTrilogy-linux-<arch>/`: the
program with everything it plays from (the data extracted from your ROM included). Copy that folder whole to another
of your own computers and run `./StarWarsTrilogy` in it. It holds your ROM's data and code: keep it to yourself.

## Compared with Windows

- Ray tracing works as on Windows, with a driver that has ray queries (NVIDIA, recent AMD and Intel with Mesa).
- No DLAA yet: picking it uses MSAA, as on a GPU without it.
- No splash video: the splash picture shows in its place.
- One mouse: a second player uses a gamepad.
- UPDATE NOW opens the game's page: update the folder (`git pull`) and run `build_linux.sh` again.

F11 or Alt+Enter toggles fullscreen.

For how the port works, see `docs/ARCHITECTURE.md`.
