# Star Wars Trilogy Native on macOS

The macOS build is maintained by contributors. It isn't tested by the project's author, who has no Mac.

## Build

You need a Mac with Apple silicon, macOS 13 or later and the command line tools (`xcode-select --install`).
Python 3 comes with those tools. CMake (3.27 or later) and Ninja are used if you have them, else `build_mac.sh`
fetches pinned copies into `build/`.

1. Put `swtrilgy.zip` in the folder (or in `roms/`, or pass its path: `./build_mac.sh ~/roms/swtrilgy.zip`).
2. Run `./build_mac.sh`. It downloads SDL3 and MoltenVK (Vulkan on Metal) on the first run and makes
   `Star Wars Trilogy.app` in the folder.
3. Play with `Star Wars Trilogy.app`.

## Where things are

The build copies the data extracted from your ROM to `~/Library/Application Support/Star Wars Trilogy Native`.
Settings and records are kept there too, and survive a rebuild. The application holds no ROM data and does nothing
without that folder, but its program is compiled from the ROM's code, so keep it to yourself.

## Compared with Windows

- No ray tracing: MoltenVK has no ray queries. The sun shadows and ambient occlusion use a sharper shadow map and
  the depth instead.
- No DLAA: MSAA 8x by default, and **Options > Display > Supersampling** for smoother still, on a fast Mac.
- One mouse: a second player uses a gamepad.
- UPDATE NOW opens the game's page instead of updating by itself.

Option+Return, Cmd+Return or F11 toggle fullscreen. On most Mac keyboards the function keys need Fn held.

For how the port works, see `docs/ARCHITECTURE.md`.
