#include "engine_sound.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace {

std::atomic<float> g_speed{0}, g_push{0};
std::atomic<bool> g_on{false};

struct Engine {
    float level = 0;              // 0 silent .. 1, fading
    float speed = 0, push = 0;    // followed smoothly: no steps in the sound
    double phase[3] = {};         // two saws and the whine
    float low[2] = {}, roar[2] = {}, breath[2] = {};   // filters' memories, left and right
    uint32_t seed = 0x1234ABCD;
    float noise() {
        seed = seed * 1664525u + 1013904223u;
        return float(int32_t(seed)) / 2147483648.0f;
    }
} g;

}  // namespace

void engine_sound_set(float speed, float push) {
    g_speed = std::clamp(speed, 0.0f, 1.0f), g_push = std::clamp(push, -1.0f, 1.0f);
    g_on = true;
}
void engine_sound_off() { g_on = false; }

void engine_sound_mix(float* out, int frames, int channels, float gain) {
    const bool on = g_on;
    if (!on && g.level <= 0) return;
    const float rate = 44100.0f;
    const float to_speed = g_speed, to_push = g_push;
    // SWT_DOGFIGHT_ENGINE=level: louder or quieter (1: as made, 0: none)
    static const float level = [] {
        const char* e = getenv("SWT_DOGFIGHT_ENGINE");
        return e ? std::clamp(float(atof(e)), 0.0f, 8.0f) : 1.0f;
    }();
    gain *= level;
    for (int i = 0; i < frames; i++) {
        g.level = std::clamp(g.level + (on ? 1.0f : -1.0f) / (0.4f * rate), 0.0f, 1.0f);
        g.speed += (to_speed - g.speed) * (1.0f / (0.05f * rate));
        g.push += (to_push - g.push) * (1.0f / (0.08f * rate));
        const float thrust = std::max(g.push, 0.0f), brake = std::max(-g.push, 0.0f);
        // The drone: two saws a fifth apart, dulled; higher and brighter with the speed
        const float f0 = 55.0f + 45.0f * g.speed + 10.0f * g.push;
        g.phase[0] += f0 / rate, g.phase[1] += f0 * 1.505f / rate;
        for (int k = 0; k < 2; k++) g.phase[k] -= std::floor(g.phase[k]);
        float saw = float(g.phase[0] * 2 - 1) + 0.6f * float(g.phase[1] * 2 - 1);
        const float cut = std::min(1.0f, 6.2832f * (280.0f + 650.0f * g.speed + 700.0f * thrust) / rate);
        // The whine of the engines, far above it
        g.phase[2] += (380.0f + 820.0f * g.speed + 120.0f * g.push) / rate;
        g.phase[2] -= std::floor(g.phase[2]);
        const float whine = std::sin(float(g.phase[2]) * 6.2832f) * (0.035f + 0.05f * g.speed + 0.05f * thrust);
        const float roar_cut = std::min(1.0f, 6.2832f * (700.0f + 2600.0f * thrust) / rate);
        const float breath_cut = 6.2832f * 420.0f / rate;
        float side[2];
        for (int c = 0; c < 2; c++) {
            g.low[c] += (saw - g.low[c]) * cut;
            // Thrust: a roar (noise, opening up); braking: a breath of low noise
            const float n = g.noise();
            g.roar[c] += (n - g.roar[c]) * roar_cut;
            g.breath[c] += (n - g.breath[c]) * breath_cut;
            side[c] = g.low[c] * (0.30f + 0.25f * g.speed + 0.25f * thrust) + whine + g.roar[c] * (0.05f + 0.10f * g.speed + 0.55f * thrust) +
                      g.breath[c] * 1.6f * brake;
            side[c] *= g.level * gain;
        }
        // (the engines are behind the pilot: more of it in the rear pair)
        float* f = out + size_t(i) * size_t(channels);
        f[0] += side[0] * 0.6f, f[1] += side[1] * 0.6f;
        if (channels >= 4) f[2] += side[0], f[3] += side[1];
    }
}
