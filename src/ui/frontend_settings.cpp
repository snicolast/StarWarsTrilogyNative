// The menu's settings in save/settings.ini (frontend_settings.h, moved from frontend.cpp)
#include "frontend_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

static_assert(std::size(ASPECT_WORDS) == ASPECT_COUNT);

void FrontendSettings::load(const char* path, const char* section) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    const bool top = section[0] == 0;
    std::string in_section;   // "" the first section, else the name of the last [name] line
    char line[512];
    int old_fullscreen = -1;   // an older file's fullscreen = 1: the borderless one, unless display_mode is there
    bool mode_saved = false;
    while (fgets(line, sizeof(line), f)) {
        char name[64];
        if (sscanf(line, " [%63[^]]]", name) == 1) {
            in_section = name;
            continue;
        }
        if (top ? !in_section.empty() : in_section != section) continue;
        char key[64], value[64];
        if (sscanf(line, " %63[^= ] = %63s", key, value) != 2) continue;
        std::string k = key, v = value;
        bool on = v == "1" || v == "on" || v == "true";
        // (the keys of the whole game only in the first section)
        if (!top && (k == "experience" || k == "telemetry")) continue;
        if (k == "experience") experience = v == "arcade" ? 1 : 0;
        else if (k == "fullscreen") old_fullscreen = on;
        else if (k == "display_mode") display_mode = v == "fullscreen" ? 2 : v == "borderless" ? 1 : 0, mode_saved = true;
        else if (k == "resolution") {
            if (sscanf(value, "%dx%d", &res_w, &res_h) != 2 || res_w <= 0 || res_h <= 0) res_w = res_h = 0;
        }
        else if (k == "vsync") vsync = v == "adaptive" ? 2 : on ? 1 : 0;
        else if (k == "max_fps") max_fps = v == "unlimited" ? -1 : v == "monitor" ? 0 : std::clamp(atoi(value), 0, 1000);
        else if (k == "background_fps") background_fps = on;
        else if (k == "aspect") {
            for (int i = 0; i < ASPECT_COUNT; i++)
                if (v == ASPECT_WORDS[i]) aspect = i;
        }
        else if (k == "volume") volume = std::clamp(atoi(value), 0, 10);
        else if (k == "music") music = std::clamp(atoi(value), 0, 10);
        else if (k == "speakers") speakers = v == "surround" ? 1 : v == "headphones" ? 2 : 0;
        else if (k == "rear_boost") rear_boost = std::clamp(atoi(value) / 3 * 3, 0, 12);
        else if (k == "sample_interpolation") cubic_samples = v != "linear";
        else if (k == "aa") {
            aa = v == "dlaa" || v == "msaa8" ? 1 : (v == "none" || v == "off") ? 2 : 0;
#ifndef __APPLE__   // the DLSS modes (NVIDIA)
            if (v == "dlss" || v == "dlss_quality") aa = 3;
            else if (v == "dlss_balanced") aa = 4;
            else if (v == "dlss_performance") aa = 5;
            else if (v == "dlss_ultra") aa = 6;
            else if (v == "fxaa") aa = 7;
#endif
        }
        else if (k == "render_scale" || k == "supersample") render_scale = std::clamp(atoi(value), 50, 200);   // (supersample: before 50-200 %)
        else if (k == "haze") haze = on;
        else if (k == "bloom") bloom = v == "low" ? 1 : v == "medium" ? 2 : v == "high" ? 3 : on ? 2 : 0;
        else if (k == "ao") ao = on;
        else if (k == "sun") sun = on;
        else if (k == "shadows") shadows = on;
        else if (k == "ray_tracing") ray_tracing = v == "shadows" ? 1 : v == "off" || v == "0" ? 0 : 2;
        else if (k == "explosions") explosions = v == "enhanced";
        else if (k == "hdr") hdr = on;
        else if (k == "sharpen") sharpen = on;
        else if (k == "motion") smooth = v != "original";
        else if (k == "saturation") saturation = std::clamp(atoi(value), 0, 200);
        else if (k == "gamma") gamma = std::clamp(int(atof(value) * 100 + 0.5), 50, 150);
        else if (k == "pad_speed") pad_speed = std::clamp(atoi(value), 1, 10);
        else if (k == "gyro") gyro = on;
        else if (k == "gyro_speed") gyro_speed = std::clamp(atoi(value), 1, 10);
        else if (k == "trigger_rumble") trigger_rumble = std::clamp(atoi(value) / 10 * 10, 0, 100);
        else if (k == "vibration") vibration = v == "low" ? 1 : v == "medium" ? 2 : v == "high" ? 3 : 0;
        else if (k == "aim_assist") aim_assist = v == "low" ? 1 : v == "medium" ? 2 : v == "high" ? 3 : 0;
        else if (k == "splash_screens") splash = on;
        else if (k == "menu_music") menu_music = on;
        else if (k == "pointer_aim") pointer_sight = v != "lever";
        else if (k == "pad_aim") pad_lever = v == "lever";
        else if (k == "pad_invert") pad_invert = v == "x" ? 1 : v == "y" ? 2 : v == "both" ? 3 : 0;
        else if (k == "duel_invert") duel_invert = v == "x" ? 1 : v == "y" ? 2 : v == "both" ? 3 : 0;
        else if (k == "sight_size") sight_size = std::clamp(atoi(value) / 10 * 10, 50, 100);
        else if (k == "telemetry") telemetry = on;
    }
    if (!mode_saved && old_fullscreen >= 0) display_mode = old_fullscreen;
    fclose(f);
}

std::string FrontendSettings::text(bool section) const {
#ifdef __APPLE__
    static const char* const AA[3] = {"msaa", "msaa8", "none"};
#else
    static const char* const AA[AA_COUNT] = {"msaa", "dlaa", "none", "dlss", "dlss_balanced", "dlss_performance", "dlss_ultra",
                                             "fxaa"};
#endif
    static const char* const BLOOM[4] = {"off", "low", "medium", "high"};   // also the vibration and aim assist levels
    static const char* const SPEAKERS[3] = {"stereo", "surround", "headphones"};
    static const char* const RT_MODES[3] = {"off", "shadows", "ao"};
    static const char* const INVERT[4] = {"off", "x", "y", "both"};
    static const char* const MODES[3] = {"window", "borderless", "fullscreen"};
    std::string out;
    char buf[2048];
    if (!section) out += experience ? "experience = arcade\n" : "experience = pc\n";
    std::string fps = max_fps < 0 ? "unlimited" : max_fps == 0 ? "monitor" : std::to_string(max_fps);
    snprintf(buf, sizeof(buf), "display_mode = %s\nresolution = %dx%d\nvsync = %s\nmax_fps = %s\nbackground_fps = %d\n",
             MODES[std::clamp(display_mode, 0, 2)], res_w, res_h, vsync == 2 ? "adaptive" : vsync ? "on" : "off", fps.c_str(),
             background_fps);
    out += buf;
    snprintf(buf, sizeof(buf),
             "volume = %d\nmusic = %d\naa = %s\nhaze = %d\nbloom = %s\nao = %d\nsun = %d\nshadows = %d\nhdr = %d\nsharpen = %d\n"
             "saturation = %d\ngamma = %.2f\nmotion = %s\nspeakers = %s\nrear_boost = %d\nsample_interpolation = %s\n"
             "pad_speed = %d\ngyro = %d\ngyro_speed = %d\nvibration = %s\naim_assist = %s\nray_tracing = %s\n"
             "splash_screens = %d\nmenu_music = %d\npointer_aim = %s\nsight_size = %d\npad_aim = %s\npad_invert = %s\n",
             volume, music, AA[std::clamp(aa, 0, AA_COUNT - 1)], haze, BLOOM[bloom], ao, sun, shadows, hdr, sharpen, saturation,
             gamma / 100.0, smooth ? "smooth" : "original", SPEAKERS[speakers], rear_boost, cubic_samples ? "cubic" : "linear",
             pad_speed, gyro, gyro_speed, BLOOM[vibration], BLOOM[aim_assist], RT_MODES[ray_tracing],
             splash, menu_music, pointer_sight ? "sight" : "lever", sight_size, pad_lever ? "lever" : "sight", INVERT[pad_invert]);
    out += buf;
    snprintf(buf, sizeof(buf), "duel_invert = %s\naspect = %s\nexplosions = %s\ntrigger_rumble = %d\nrender_scale = %d\n",
             INVERT[duel_invert], ASPECT_WORDS[std::clamp(aspect, 0, ASPECT_COUNT - 1)], explosions ? "enhanced" : "arcade",
             trigger_rumble, render_scale);
    out += buf;
    if (!section) out += telemetry ? "telemetry = 1\n" : "telemetry = 0\n";
    return out;
}

void FrontendSettings::save(const char* path, const FrontendSettings& pc, const FrontendSettings& arcade) {
    std::string out = pc.text(false) + "[arcade]\n" + arcade.text(true);
    if (FILE* f = fopen(path, "w")) {
        fputs(out.c_str(), f);
        fclose(f);
    }
}

FrontendSettings FrontendSettings::arcade_defaults() const {
    FrontendSettings a = *this;   // the screen, the speakers and the controls as in PC EDITION
    a.arcade_base();
    return a;
}

void FrontendSettings::arcade_base() {
    const FrontendSettings d;
    volume = d.volume;   // the effects and the music at their defaults, whatever PC EDITION has
    music = d.music;
    aa = 2;              // no anti-aliasing: the Model 3 had none
    haze = ao = sun = shadows = hdr = sharpen = false;
    bloom = 0;
    ray_tracing = 0;
    explosions = false;  // EXPLOSIONS: ARCADE
    smooth = true;       // MOTION SMOOTH (as Sega Rally 2's ARCADE ORIGINAL)
    saturation = gamma = 100;
    cubic_samples = false;   // SAMPLE QUALITY ORIGINAL: the sound chip's own interpolation
    aspect = 0;          // the cabinet's 4:3 screen
}
