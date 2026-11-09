<p align="center">
  <img src="res/menu/logo.png" alt="Star Wars Trilogy Arcade" width="60%">
</p>

<p align="center">
  <b>Sega's 1998 arcade game, running natively on PC and Mac.</b><br>
  Not an emulator: the original code, recompiled to C++, with a modern Vulkan renderer.
</p>

## What you get

- **True widescreen**, not stretched: 16:9, 21:9, 32:9 or 48:9 for three monitors.
- **Ray-traced sun shadows** and ambient occlusion, haze, bloom, light shafts and HDR lighting.
- **New looks, made for this port**:
  - New explosions in Yavin: billowing fireballs, sparks, burning debris and TIE fighters breaking
    into pieces of themselves, with a louder, deeper sound panned to where they are
    (OPTIONS > GRAPHICS > EXPLOSIONS: ARCADE for the original ones).
  - Space redrawn with a deep star field and the Milky Way; the Death Star as in the films, with
    thousands of lights in its trenches.
  - Lightsabers, lasers and explosions light what is around them; Vader's suit shines; Boba
    Fett's desert has wind ripples.
- **Sharp and smooth**: MSAA or NVIDIA DLAA, HD text, and the 3D drawn between the game's frames at 120 / 144 Hz and above.
- **Arcade sound**, Digital Sound Board music and voices included, with surround and headphone modes.
- **Mouse, gamepad (gyro too), light gun or flight stick**, with aim assist and force feedback as rumble.
- **Quality presets** from laptops to RTX cards, and **achievements and stats** in Records.

<table>
  <tr>
    <td><img src="docs/img/menu.jpg" alt="Main menu"></td>
    <td><img src="docs/img/death_star.jpg" alt="TIE fighters locked on in front of the Death Star"></td>
  </tr>
  <tr>
    <td><img src="docs/img/hoth.jpg" alt="Walkers on Hoth in true widescreen"></td>
    <td><img src="docs/img/endor.jpg" alt="Speeder bike chase on Endor"></td>
  </tr>
</table>

## Coming soon

New game modes made for this port, already waiting under **Play > Extra modes**:

- **Dogfight**: your X-wing against squadrons of TIE fighters, with random encounters and
  objectives, so no two flights are the same.

More content will follow in future updates.

## ⚠️ ROM not included

You need your own **`swtrilgy.zip`**, the *Rev. A* set (Sega Model 3 Step 2.1, files like
`epr-21379a.17`). Nothing from it is in this repository, and other versions will not work.
It must include `epr-21119.ic8` (the force feedback board), which some older sets lack: without it
the game stops at *FAILED FEEDBACK LEVER INITIALIZE*. `build.bat` says which chips are missing.

## Install

You need Windows 10/11 (64-bit) and a GPU with a Vulkan 1.3 driver (most from 2016 on).

1. Download this repository (green **Code** button > **Download ZIP**) and unzip it.
2. Put `swtrilgy.zip` in the folder.
3. Double-click `install.bat`. It installs Python 3 and the Visual Studio 2022 C++ build tools
   with winget (a few GB, accept the Windows prompt), then builds the game.
4. Play with `StarWarsTrilogy.exe`.

On a Mac with Apple silicon, see [docs/MACOS.md](docs/MACOS.md) (maintained by contributors).

<details>
<summary>Manual install</summary>

Install [Python 3.10 or later](https://www.python.org/downloads/) and
[Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
with the *Desktop development with C++* workload. The full workload is over 6 GB; the build only
needs the MSVC compiler, CMake and a Windows SDK. In a command prompt:

```
winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
```

Or in the Visual Studio Installer, choose *Individual components* instead of the workload and
tick *MSVC v143 - VS 2022 C++ x64/x86 build tools (Latest)*, *C++ CMake tools for Windows*
and one *Windows 11 SDK* (it works on Windows 10 too).

Then run `build.bat` with `swtrilgy.zip` next to it (or pass its path: `build.bat D:\roms\swtrilgy.zip`).
It downloads SDL3, miniz and NVIDIA DLSS on the first run.
</details>

If the build can't download SDL3 or miniz (a proxy, an antivirus, a flaky connection), it lists
the links: download them with your browser, put them in `third_party\downloads\` keeping the file
name and run `build.bat` again.

### Update

When there is a new version, the main menu says what is new and offers **UPDATE NOW**. Or run
`update.bat` yourself: it downloads the changed files from GitHub (no git needed) and rebuilds.
Your ROM, settings and records are kept; files you edited are backed up to `out\update_backup\`.

## Controls

| | Mouse and keyboard | Gamepad |
|---|---|---|
| Aim | Move (or the arrow keys) | Sticks, or gyro (turn on GYRO AIM in CONTROLS) |
| Fire (trigger) | Left click or Space | R2 / R1 / A |
| Second trigger | Right click or Ctrl | L2 / L1 / B |
| EVENT button | E or middle click | X |
| Coin + start | 5, then 1 | BACK, then START |
| Pause | Esc | START |

F11 or Alt+Enter toggles fullscreen. Everything else is in **Options**.

- **Mouse / light gun**: the sight goes where you point (Sinden, Gun4IR and other guns seen as a
  mouse work as is). OPTIONS > CONTROLS can make the mouse move the flight lever instead.
- **Flight sticks** (T.Flight HOTAS, Extreme 3D...) are the cabinet's lever: buttons 1 fire,
  2 second trigger, 3 EVENT, 4 coin, 5 start. Plug the stick in its PC mode.
- **Invert in duels** (OPTIONS > CONTROLS) turns the aim round in the lightsaber duels only.

## More

- [Other recompiled arcade games](https://github.com/snicolast/LostWorldNative): The Lost World, also
  in the main menu under **More recomps**.
- [How it works](docs/ARCHITECTURE.md): the PowerPC to C++ recompiler and the hardware.

## Support

If you enjoy the port and want to help with the time and tools behind it, you can buy me a coffee.
Entirely optional: it supports the work on this project, not the game, which belongs to Sega.

<a href="https://buymeacoffee.com/snicolast8u"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me a Coffee" height="50"></a>

## License

The code in this repository is under the [MIT License, with additional terms](LICENSE). The libraries and fonts in
`third_party/` keep their own licenses. The gamepad button pictures in `res/pad/` are
[Controller Input Icons](https://opengameart.org/content/controller-input-icons) by ElDuderino (CC0). The game itself (ROM, code, data, music and voices) is
not included and stays the property of its owners: you need your own copy of `swtrilgy.zip`.

**No prebuilt downloads.** The license does not allow anyone to share a compiled build of this
port, a ROM, or anything made from one. I have never published any of them: everyone builds the
game themselves from their own `swtrilgy.zip`. Any ready-to-play download you find elsewhere is
not from me, breaks the license, and is at your own risk.

<sub>A fan project for personal use, not affiliated with or endorsed by Sega or Lucasfilm.
Star Wars is a trademark of Lucasfilm Ltd.</sub>
