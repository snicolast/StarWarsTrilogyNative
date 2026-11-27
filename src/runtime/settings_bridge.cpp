#include "settings_bridge.h"

#include <algorithm>

#include "../ui/frontend_settings.h"

const float BLOOM_STRENGTH[4] = {0.0f, 0.6f, 1.0f, 1.6f};
// Ultrawide monitors are 2.37 (2560x1080) to 2.39 (3440x1440), wider than
// 21/9 = 2.33, so 21:9 allows up to 2.4. 32:9 monitors are 3.56; three
// monitors side by side 5.33, more with bezel correction, so 48:9 allows 6.
// The 3D view never gets wider than the window, so a level wider than the
// monitor only leaves room, and AUTO (fills the window whatever its shape) is 48:9's
const float MAX_ASPECT[6] = {4.0f / 3.0f, 16.0f / 9.0f, 2.4f, 3.6f, 6.0f, 6.0f};

int aspect_level(float max_aspect) {
    return max_aspect < 1.5f ? 0 : max_aspect < 2.0f ? 1 : max_aspect < 3.0f ? 2 : max_aspect < 4.5f ? 3 : 4;
}

int aspect_level_of(int aspect) { return std::min(aspect, 4); }

void options_from_settings(Options& o, const FrontendSettings& ini) {
#ifdef __APPLE__
    o.msaa = ini.aa == 1 ? 8 : ini.aa == 2 ? 1 : 4;
#else
    o.dlaa = ini.aa == 1 || (ini.aa >= 3 && ini.aa <= 6);
    o.dlss = ini.aa >= 3 && ini.aa <= 6 ? ini.aa - 2 : 0;   // 3 QUALITY .. 6 ULTRA PERFORMANCE
    o.fxaa = ini.aa == 7;
    if (ini.aa == 2 || ini.aa == 7) o.msaa = 1;
#endif
    o.arcade = ini.experience == 1;   // (also none of the new effects that have no row: app.cpp)
    live_options_from_settings(o, ini);
}

void live_options_from_settings(Options& o, const FrontendSettings& ini) {
    o.volume = float(ini.volume);
    o.music = float(ini.music);
    o.speakers = ini.speakers;
    o.cubic_samples = ini.cubic_samples;
    o.rear_boost = ini.rear_boost;
    o.supersample = ini.render_scale / 100.0f;   // RENDER SCALE (--scale wins)
    int aspect = std::clamp(ini.aspect, 0, 5);
    if (aspect_level_of(aspect) != aspect_level(o.max_aspect)) o.max_aspect = MAX_ASPECT[aspect];   // --aspect 2.6 stays as 21:9
    o.haze = ini.haze ? (o.haze > 0 ? o.haze : 1.0f) : 0.0f;
    o.bloom = BLOOM_STRENGTH[ini.bloom];
    o.ao = ini.ao ? (o.ao > 0 ? o.ao : 1.0f) : 0.0f;
    o.sun = ini.sun;
    o.shadows = ini.shadows;
    o.ray_tracing = ini.ray_tracing;
    o.explosions = ini.explosions;
    o.hdr = ini.hdr;
    o.sharpen = ini.sharpen;
    o.interp = ini.smooth;
    o.saturation = ini.saturation / 100.0f;
    o.gamma = ini.gamma / 100.0f;
    o.telemetry = ini.telemetry;   // (telemetry_frame opens or closes the socket at the next frame)
}

void settings_from_options(FrontendSettings& s, const Options& o) {
    s.volume = std::clamp(int(o.volume + 0.5f), 0, 10);
    s.music = std::clamp(int(o.music + 0.5f), 0, 10);
    s.speakers = std::clamp(o.speakers, 0, 2);
    s.cubic_samples = o.cubic_samples;
    s.rear_boost = std::clamp(o.rear_boost / 3 * 3, 0, 12);
#ifdef __APPLE__
    s.aa = o.msaa >= 8 ? 1 : o.msaa <= 1 ? 2 : 0;
#else
    s.aa = o.dlss ? 2 + std::clamp(o.dlss, 1, 4) : o.dlaa ? 1 : o.fxaa ? 7 : o.msaa <= 1 ? 2 : 0;
#endif
    s.render_scale = std::clamp(int(o.supersample * 100 + 0.5f), 50, 200);
    if (aspect_level_of(s.aspect) != aspect_level(o.max_aspect))   // (AUTO stays AUTO)
        s.aspect = aspect_level(o.max_aspect);   // --aspect 2.6 shows as 21:9 and is kept until changed
    s.haze = o.haze > 0;
    s.bloom = o.bloom <= 0 ? 0 : o.bloom < 0.8f ? 1 : o.bloom < 1.3f ? 2 : 3;
    s.ao = o.ao > 0;
    s.sun = o.sun;
    s.shadows = o.shadows;
    s.ray_tracing = o.ray_tracing;
    s.explosions = o.explosions;
    s.hdr = o.hdr;
    s.sharpen = o.sharpen;
    s.smooth = o.interp;
    s.saturation = std::clamp(int(o.saturation * 100 + 0.5f), 0, 200);
    s.gamma = std::clamp(int(o.gamma * 100 + 0.5f), 50, 150);
    s.telemetry = o.telemetry;
}
