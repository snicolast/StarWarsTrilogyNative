# Native port architecture

Goal: a PC executable that runs the original logic of Star Wars Trilogy
translated to C++, with its own engine for graphics, sound and controls. The
game still needs your `swtrilgy.zip` to load its data; the repository never
contains ROM data (`data/` is in `.gitignore`). The code base is the finished
Lost World port (Model 3 Step 1.5); this file lists what changed for Step 2.1.

## Original hardware (Model 3 Step 2.1) and what changes from Lost World

| Part | Star Wars Trilogy | Lost World (Step 1.5) | In the port |
|---|---|---|---|
| CPU | PowerPC 603ev, 166 MHz | 603e, 100 MHz | Same static recompiler, cycle budget per frame for 166 MHz |
| Fixed CROM | 8 MB at `0xFF800000` | 2 MB at `0xFFE00000` | `rom_ptr` maps it (done) |
| Banked CROM | 48 MB, 8 MB window at `0xFF000000` | 64 MB | Bank register in the Step 2 map |
| PCI bridge | MPC106, config at `0xFEC00000` / `0xFEE00000` | MPC105 at `0xF0800CF8` | Emulated |
| System, inputs, MIDI, RTC | `0xFExxxxxx` (`0xFE040000` inputs, `0xFE080000` sound, `0xFE100000` system/IRQ...) | `0xF0xxxxxx` | Emulated |
| GPU | Real3D Pro-1000, transfers by DMA at `0xC2000000` | Same chip, SCSI 53C810 at `0xC1000000` | Renderer reused, DMA emulated |
| 2D layer | Tilegen at `0xF1000000` | Same | Reused |
| Sound | 68000 + 2x SCSP, plus Digital Sound Board 2 (68000 + MPEG) for music and voices | 68000 + 2x SCSP | SCSP reused; DSB2 added (second Musashi context + MPEG Layer 2 decoder) |
| Force feedback | Drive board, Z80 (`epr-21119`) | none | Gamepad rumble |
| Controls | Analog stick, trigger and buttons | Light guns | Mouse, light gun, gamepad and gyro mapped to the stick |

## What we already know about the program

- The reset vector (`0xFFF00100`) branches to `0xFFF00000`, which calls the PCI
  setup (`0xFFF01618`, a table of MPC106 registers), copies the first **2 MB of
  the CROM to RAM 0** (`0xFFF018D0`) and jumps to `0x100`. As in Lost World,
  **the whole game runs from RAM**.
- `analyze.py` finds about **268,000 reachable instructions** and `recomp.py`
  keeps **3,407 functions** (594 prologue guesses dropped for landing in data),
  with 89 indirect `bctr` jumps. The 32 untranslated words all lie in data.
- Hardware bases loaded with `lis`: `0xFE040000` (x164), `0xF10F0000` (x756,
  tilegen), `0xC2000000` (x97), `0xFE100000`, `0xFE140000`, `0x88000000`,
  `0x90000000`, `0x94000000`, `0x98000000` (Real3D).
- The game hooks (the sight and lever widened for widescreen, scores and phase
  results for the achievements) are listed with their addresses at the top of
  `tools/recomp.py`; the runtime side is in `src/runtime/runtime.cpp` and
  `stats.cpp`. The vblank wait loops are not hooked yet (`IDLE_LOOPS`).

## Pipeline

```
swtrilgy.zip ──romtool.py──▶ data/*.bin   (StarWarsTrilogy.exe also reads the zip directly: rom_zip.cpp)
data/crom.bin ──analyze.py──▶ out/listing.asm, functions.txt, report.txt
              ──recomp.py──▶ build/gen/*.cpp  (one C++ function per PPC function)
gen/*.cpp + runtime/ (memory, Real3D, SCSP, input) ──CMake/MSVC──▶ StarWarsTrilogy.exe
```

## macOS

The generated C++ and the game's hardware are the same on both systems; only the layer under them differs.
On macOS (`src/platform/mac/`, `build_mac.sh`) a `windows.h` of our own gives the Win32 names the code uses,
so its Windows paths stay as they are:

| Windows | macOS |
|---|---|
| Win32 window, messages, raw input | SDL3 (`vk_window_mac.cpp`), turned into the same messages; one mouse (`raw_mice_mac.cpp`) |
| Vulkan driver | MoltenVK 1.4, loaded from next to the program; no ray queries, so `RAY_TRACING` shaders stay unused |
| WASAPI | SDL3 audio at the device's own rate (`audio_out_mac.cpp`) |
| GDI text | Core Text (`win32_mac.mm`) |
| WIC | ImageIO (`image_io_mac.mm`) |
| Media Foundation, MCI, PlaySound | AVFoundation (`splash_video_mac.mm`, `menu_music_mac.mm`, `win32_mac.mm`) |
| WinHTTP | NSURLSession (`win32_mac.mm`) |
| NGX (DLAA) | none (`dlss_mac.cpp`) |

The floating point of the game must stay as the Windows build has it: `-ffp-contract=off` keeps `a*b+c` as two
operations (Arm has a fused one the compiler would otherwise pick), with `-fno-strict-aliasing` and `-fwrapv`
for what MSVC allows.

The game reads and writes relative to the current folder. `Star Wars Trilogy.app` carries what it reads in
`Contents/Resources` and starts in `~/Library/Application Support/Star Wars Trilogy Native`, where links lead to it
(`mac_enter_game_folder`). Reading from a folder like the Desktop instead would hold the program, with no window,
until macOS has its answer to a privacy prompt, and again after each rebuild, as an ad-hoc signature changes.

Ray tracing and DLAA are missing there, and Apple's GPUs make MSAA cheap, so macOS spends differently: MSAA 8x and
16x anisotropic filtering by default, a supersampling option, a 4096 shadow map (2048 on Windows) read through a
tent filter, and alpha-tested coverage dithered per pixel so soft alpha does not show one ring per sample
(`PLATFORM_MAC` in `r3d.frag`).

## Status

Playable from start to end: every stage of the arcade game in true widescreen,
with sound, music and voices, controls, force feedback as rumble, the Lost World
renderer (ray tracing, AO, DLAA, high refresh rate), main menu, options,
achievements and statistics.
