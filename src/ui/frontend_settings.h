// The menu's settings, kept in save/settings.ini: what the options pages
// change (frontend.h) and settings_bridge.h turns into the game's Options
#pragma once
#include <string>

// settings.ini's words for FrontendSettings::aspect
inline const char* const ASPECT_WORDS[] = {"4:3", "16:9", "21:9", "32:9", "48:9", "auto"};
constexpr int ASPECT_COUNT = 6;
// ANTI-ALIASING: how many values FrontendSettings::aa takes
#ifdef __APPLE__
constexpr int AA_COUNT = 3;
#else
constexpr int AA_COUNT = 8;
#endif

struct FrontendSettings {
    int display_mode = 0;      // MODE: 0 window, 1 borderless (the desktop's resolution), 2 exclusive full screen
    int res_w = 0, res_h = 0;  // RESOLUTION: the window's inside, or the full-screen mode (0: default window / the desktop's)
    int vsync = 1;             // VSYNC: 1 on (FIFO), 2 adaptive (FIFO_RELAXED), 0 off (IMMEDIATE, else MAILBOX)
    int max_fps = 0;           // MAX FPS: 0 the monitor's refresh rate, -1 UNLIMITED, else pictures a second at most
    bool background_fps = true;   // BACKGROUND FPS: 30 a second at most while another window has the focus
    int aspect = 1;    // widest 3D view: 0 4:3 (original), 1 16:9, 2 21:9, 3 32:9, 4 48:9, 5 AUTO (MAX_ASPECT in settings_bridge.cpp)
    int volume = 5;    // 0..10 (output gain): the sound effects and voices
    int music = 5;     // 0..10: the music board's gain
    int speakers = 0;  // 0 stereo, 1 surround 4.0, 2 headphones (AudioStream::Speakers)
    int rear_boost = 6;  // dB on the rear pair: 0, 3, 6, 9, 12
    bool cubic_samples = true;   // smooth (cubic) sample interpolation instead of the original linear one
#ifdef __APPLE__
    int aa = 1;        // 0 MSAA 4x, 1 MSAA 8x, 2 off (takes effect on the next launch)
#else
    int aa = 0;        // 0 MSAA, 1 DLAA, 2 off, 3 DLSS QUALITY, 4 BALANCED, 5 PERFORMANCE, 6 ULTRA PERFORMANCE,
                       // 7 FXAA (takes effect on the next launch)
#endif
    int render_scale = 100;  // RENDER SCALE, %: the 3D drawn at this share of the picture's size (--scale), 50..200
    bool haze = true;  // distance haze
    int bloom = 2;     // glow around bright things: 0 off, 1 low, 2 medium, 3 high
    bool ao = true;    // ambient occlusion
    bool sun = true;   // a sun in the procedural sky
    bool shadows = true;   // sun shadows
    int ray_tracing = 2;   // traced through the world (GPUs with ray tracing): 0 off, 1 sun shadows, 2 also ambient occlusion
    bool explosions = true;    // EXPLOSIONS: ENHANCED (new fireballs, sparks and debris; Yavin)
    bool hdr = true;       // HDR lighting
    bool sharpen = true;   // contrast adaptive sharpening
    bool smooth = true;     // the 3D drawn between game frames at the monitor's refresh rate
    int saturation = 80;    // colour saturation in % (100 = original; 80 by default)
    int gamma = 100;        // gamma x 100 (100 = original, higher = brighter)
    int pad_speed = 5;      // gamepad sight speed with the sticks, 1..10
    bool gyro = false;      // the gamepad's gyroscope moves the sight (CONTROLS > GYRO AIM turns it on)
    int gyro_speed = 5;     // 1..10
    int vibration = 2;      // gamepad vibration: 0 off, 1 low, 2 medium, 3 high
    int trigger_rumble = 100;   // the triggers' own motors (Xbox One / Series pads), % of VIBRATION's strength, steps of 10
    int aim_assist = 1;     // gamepad aim assist: 0 off, 1 low, 2 medium, 3 high
    bool pad_lever = false; // the gamepad's sticks are the flight lever (back to the centre when let go)
    int pad_invert = 0;     // the sticks' axes turned round: 0 off, 1 x, 2 y, 3 both
    int duel_invert = 0;    // the aim turned round in the lightsaber duels only (mouse, light gun, sticks): 0 off, 1 x, 2 y, 3 both
    bool splash = true;     // the logo video at launch (splash_screens = 0: straight to the menu)
    bool menu_music = true; // the game's Main Title in the menus
    int sight_size = 80;    // the game's sight drawn at this size, % (Star Wars Trilogy)
    bool pointer_sight = true; // the sight goes where the mouse / light gun points (false: the mouse is the lever)
    // EXPERIENCE (the file's first section only): 0 PC EDITION, 1 ARCADE ORIGINAL. Each
    // experience keeps its own settings: PC EDITION's at the top of the file, ARCADE
    // ORIGINAL's in its [arcade] section
    int experience = 0;
    // OGS-TS telemetry for motion rigs and haptics (runtime/telemetry.h). Off by default
    // (OPTIONS > EXPERIENCE > TELEMETRY); a key of the whole game, so in the file's first section only
    bool telemetry = false;

    // save/settings.ini; missing keys keep their current value. section: "" the
    // file's first one (PC EDITION and the keys of the whole game), "arcade" ARCADE ORIGINAL's
    void load(const char* path, const char* section = "");
    // One "key = value" line per setting (section: without the keys of the whole game)
    std::string text(bool section = false) const;
    // save/settings.ini: PC EDITION's settings (and the keys of the whole game) at the top, ARCADE ORIGINAL's in [arcade]
    static void save(const char* path, const FrontendSettings& pc, const FrontendSettings& arcade);
    // ARCADE ORIGINAL's settings before it has a section of its own: these (the
    // screen, the speakers and the controls) with the arcade's own look
    FrontendSettings arcade_defaults() const;
    // Those base values of ARCADE ORIGINAL (its [arcade] section's first time, and RESTORE ORIGINAL):
    // no new effects, no anti-aliasing, the original colours and sound, ASPECT 4:3. The
    // screen (MODE, RESOLUTION, RENDER SCALE, VSYNC, MAX FPS...), the speakers and the controls stay the player's
    void arcade_base();
};
