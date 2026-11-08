#include "settings_bridge.h"

#include <algorithm>

#include "../ui/frontend.h"

const float BLOOM_STRENGTH[4] = {0.0f, 0.6f, 1.0f, 1.6f};
// Ultrawide monitors are 2.37 (2560x1080) to 2.39 (3440x1440), wider than
// 21/9 = 2.33, so 21:9 allows up to 2.4. 32:9 monitors are 3.56; three
// monitors side by side 5.33, more with bezel correction, so 48:9 allows 6.
// The 3D view never gets wider than the window, so a level wider than the
// monitor only leaves room
const float MAX_ASPECT[5] = {4.0f / 3.0f, 16.0f / 9.0f, 2.4f, 3.6f, 6.0f};

int aspect_level(float max_aspect) {
    return max_aspect < 1.5f ? 0 : max_aspect < 2.0f ? 1 : max_aspect < 3.0f ? 2 : max_aspect < 4.5f ? 3 : 4;
}

void options_from_settings(Options& o, const FrontendSettings& ini) {
    o.volume = float(ini.volume);
    o.music = float(ini.music);
    o.speakers = ini.speakers;
    o.cubic_samples = ini.cubic_samples;
    o.rear_boost = ini.rear_boost;
#ifdef __APPLE__
    o.msaa = ini.aa == 1 ? 8 : ini.aa == 2 ? 1 : 4;
    o.supersample = float(ini.supersample) / 100.0f;
#else
    o.dlaa = ini.aa == 1;
    if (ini.aa == 2) o.msaa = 1;
#endif
    int aspect = std::clamp(ini.aspect, 0, 4);
    if (aspect != aspect_level(o.max_aspect)) o.max_aspect = MAX_ASPECT[aspect];   // --aspect 2.6 stays as 21:9
    o.haze = ini.haze ? 1.0f : 0.0f;
    o.bloom = BLOOM_STRENGTH[ini.bloom];
    o.ao = ini.ao ? 1.0f : 0.0f;
    o.sky_procedural = ini.sky;
    o.sun = ini.sun;
    o.shadows = ini.shadows;
    o.ray_tracing = ini.ray_tracing;
    o.shafts = ini.shafts;
    o.soft = ini.soft;
    o.hdr = ini.hdr;
    o.relief = ini.relief;
    o.sharpen = ini.sharpen;
    o.classic_text = !ini.hd_text;
    o.show_crosshair = ini.crosshair;
    o.interp = ini.smooth;
    o.several_mice = ini.second_mouse;
    o.saturation = ini.saturation / 100.0f;
    o.gamma = ini.gamma / 100.0f;
}

void settings_from_options(FrontendSettings& s, const Options& o) {
    s.volume = std::clamp(int(o.volume + 0.5f), 0, 10);
    s.music = std::clamp(int(o.music + 0.5f), 0, 10);
    s.speakers = std::clamp(o.speakers, 0, 2);
    s.cubic_samples = o.cubic_samples;
    s.rear_boost = std::clamp(o.rear_boost / 3 * 3, 0, 12);
#ifdef __APPLE__
    s.aa = o.msaa >= 8 ? 1 : o.msaa <= 1 ? 2 : 0;
    s.supersample = o.supersample >= 2.0f ? 200 : o.supersample >= 1.5f ? 150 : 100;
#else
    s.aa = o.dlaa ? 1 : o.msaa <= 1 ? 2 : 0;
#endif
    s.aspect = aspect_level(o.max_aspect);   // --aspect 2.6 shows as 21:9 and is kept until changed
    s.haze = o.haze > 0;
    s.bloom = o.bloom <= 0 ? 0 : o.bloom < 0.8f ? 1 : o.bloom < 1.3f ? 2 : 3;
    s.ao = o.ao > 0;
    s.sky = o.sky_procedural;
    s.sun = o.sun;
    s.shadows = o.shadows;
    s.ray_tracing = o.ray_tracing;
    s.shafts = o.shafts;
    s.soft = o.soft;
    s.hdr = o.hdr;
    s.relief = o.relief;
    s.sharpen = o.sharpen;
    s.hd_text = !o.classic_text;
    s.crosshair = o.show_crosshair;
    s.smooth = o.interp;
    s.second_mouse = o.several_mice;
    s.saturation = std::clamp(int(o.saturation * 100 + 0.5f), 0, 200);
    s.gamma = std::clamp(int(o.gamma * 100 + 0.5f), 50, 150);
}
