// Command-line options of StarWarsTrilogy.exe. One table in options.cpp drives the
// parser and --help; the menu (frontend.h) reads and writes the same struct
// while the game runs.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct Options {
    // Run
    std::string rom_zip;             // --rom: swtrilgy.zip, read directly (SWT_ROM_ZIP when not given)
    std::string data_dir = "data";   // --data: the ROM files tools/romtool.py extracts
    int frame_limit = INT32_MAX;     // --frames; with --headless, 600 by default
    bool headless = false;           // --headless: no window, full speed, report at the end
    bool menu = true;                // the main menu and save/settings.ini; off in test runs
    bool arcade = false;             // EXPERIENCE ARCADE ORIGINAL (settings.ini): the cabinet as it was, no main menu
    bool show_boot = false;          // --boot: the power-on screens before the SEGA logo
    bool trigger_rumble = true;      // --no-trigger-rumble: Xbox pads through XInput (no motors in the triggers)
    // Picture
    float supersample = 1.0f;        // --scale: internal 3D resolution factor
    int msaa = 4;                    // --msaa: samples per pixel (1 = off)
    bool dlaa = false;               // --aa dlaa: NVIDIA DLAA instead of MSAA (also set by --aa dlss*)
    bool fxaa = false;               // --aa fxaa: FXAA over the 3D image instead of MSAA (cheap, for older cards)
    int dlss = 0;                    // --aa dlss*: 0 none (DLAA), 1 QUALITY, 2 BALANCED, 3 PERFORMANCE, 4 ULTRA PERFORMANCE
                                     // (the 3D drawn smaller and upscaled)
    int dlss_model = 0;              // --dlss-model: 0 auto, 1 K, 2 M, 3 L
    int aniso = 8;                   // --aniso: anisotropic taps (0 = no mipmaps, original look)
    float max_aspect = 16.0f / 9.0f; // --aspect: widest 3D view (4:3 = original)
    bool pixel_2d = false;           // --pixel2d: 2D layers without xBR upscaling
    bool hud_centre = false;         // --hud-centre: keep the HUD in the 4:3 area
    bool letterbox = false;          // --letterbox: keep the black cutscene bars
    bool interp = true;              // --no-interp: show game frames as they are
    float light_gain = 1.0f;         // --light-gain: light multiplier on the sun and the ambient
    float bloom = 1;                 // --bloom [N] / --no-bloom: glow around bright things
    float ao = 1;                    // --ao [N] / --no-ao: ambient occlusion (up to 1)
    float haze = 1;                  // --haze [N] / --no-haze: distance haze that hides the draw distance
    float saturation = 0.8f;         // --saturation: colour saturation (1 = original)
    float gamma = 1;                 // --gamma: 1 = original, higher = brighter
    bool hdr = true;                 // --no-hdr: the original's clipped lighting
    bool sharpen = true;             // --no-sharpen: no sharpening of the 3D image
    bool shadows = true;             // --no-shadows: no sun shadows
    int ray_tracing = 2;             // --rt off|shadows|ao (--no-rt): traced sun shadows (1) and ambient occlusion (2)
    bool sun = true;                 // --no-sun: no sun in the procedural sky
    bool explosions = true;          // --explosions arcade|enhanced: Star Wars Trilogy's new fireballs, sparks and debris (Yavin)
    bool telemetry = false;          // OGS-TS telemetry for motion rigs and haptics (settings.ini telemetry = 1; telemetry.h)
    // Sound
    float volume = 5.0f;             // --volume: output gain
    float music = 5.0f;              // --music: the music board's gain
    int speakers = 0;                // --speakers stereo|surround|headphones: 0, 1, 2 (AudioStream::Speakers)
    int rear_boost = 6;              // --rear-boost: extra dB on the rear pair (0, 3, 6, 9, 12)
    bool cubic_samples = true;       // --linear-samples: the SCSP's own linear interpolation instead of cubic
    std::string wav_path;            // --wav: record the sound output
    // Debugging and tests
    uint64_t shot_frame = 0;         // --shot N: screenshot of frame N, then exit
    std::string replay;              // --replay: a debug snapshot (or a folder of them) instead of the game
    std::vector<std::string> auto_scripts;   // --auto coin@F,start@F (each occurrence)
    bool fixed_aim = false;          // --aim X,Y: fixed raw gun ADC values
    int aim_x = 128, aim_y = 128;
    std::string diff;                // --diff FN@FRAME
    std::vector<std::string> traces; // --trace A,B,... (each occurrence)
};

// Fills `o` from the command line. Returns true to run the game; otherwise
// (--help, an unknown option or a bad value) the usage or the error has been
// printed and exit_code says how to end
bool parse_options(Options& o, int argc, char** argv, int& exit_code);

// Whether an argument asks for a test run, which leaves the menu out so tests
// always start from the same defaults: the options marked so in the table
bool options_disable_menu(int argc, char** argv);

void print_usage(FILE* out);
