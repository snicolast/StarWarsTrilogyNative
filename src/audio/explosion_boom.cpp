#include "explosion_boom.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace {

constexpr float RATE = 44100.0f;
constexpr float LENGTH = 2.5f;   // seconds
constexpr int MAX_VOICES = 6;

struct Voice {
    float t = 0, strength = 1;
    float phase = 0;
    uint32_t noise = 1;
    float body[2] = {}, tail[2] = {}, tail2[2] = {}, crack[2] = {};
    float pitch = 1;   // a little different each time
};

std::mutex g_mutex;
std::vector<float> g_pending;
std::vector<Voice> g_voices;   // sound board thread only
uint32_t g_count = 0;

float white(uint32_t& s) {
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return float(s) * (2.0f / 4294967296.0f) - 1.0f;
}

// One-pole low-pass coefficient for a cutoff in Hz
float lp(float hz) { return 1.0f - std::exp(-6.2831853f * hz / RATE); }

}  // namespace

void explosion_boom(float strength) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pending.size() < 8) g_pending.push_back(std::clamp(strength, 0.0f, 1.0f));
}

void explosion_boom_mix(float* frames, int count, int channels, float gain, float rear_gain) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (float s : g_pending) {
            if (g_voices.size() >= size_t(MAX_VOICES)) g_voices.erase(g_voices.begin());   // the oldest, mostly tail by now
            Voice v;
            v.strength = s;
            v.noise = 0x9E3779B9u * ++g_count | 1u;
            v.pitch = 0.85f + 0.3f * float((v.noise >> 8) & 255) / 255.0f;
            g_voices.push_back(v);
        }
        g_pending.clear();
    }
    if (g_voices.empty()) return;
    const float dt = 1.0f / RATE;
    for (Voice& v : g_voices) {
        float amp = 0.22f * gain * v.strength;
        for (int i = 0; i < count && v.t < LENGTH; i++, v.t += dt) {
            float t = v.t;
            float attack = 1.0f - std::exp(-t * 300.0f);
            // A sub tone falling from ~75 to ~30 Hz
            float hz = (30.0f + 45.0f * std::exp(-t * 6.0f)) * v.pitch;
            v.phase += 6.2831853f * hz * dt;
            if (v.phase > 6.2831853f) v.phase -= 6.2831853f;
            float sub = std::sin(v.phase) * std::exp(-t * 3.0f) * 0.9f;
            float out[2];
            for (int c = 0; c < 2; c++) {
                float n = white(v.noise);
                // The blast: noise darkening from ~600 to ~90 Hz
                float k = lp(90.0f + 510.0f * std::exp(-t * 7.0f));
                v.body[c] += k * (n - v.body[c]);
                // Rumble: two poles at ~70 Hz, a long tail
                v.tail[c] += lp(70.0f) * (n - v.tail[c]);
                v.tail2[c] += lp(70.0f) * (v.tail[c] - v.tail2[c]);
                // The crack at the start
                v.crack[c] += lp(2500.0f) * (n - v.crack[c]);
                out[c] = sub + v.body[c] * 2.2f * std::exp(-t * 4.5f) + v.tail2[c] * 9.0f * std::exp(-t * 1.6f) +
                         v.crack[c] * 0.5f * std::exp(-t * 45.0f);
                out[c] *= attack * amp;
            }
            float* f = frames + size_t(i) * size_t(channels);
            f[0] += out[0];
            f[1] += out[1];
            if (channels >= 4) {
                f[2] += out[0] * rear_gain * 0.7f;
                f[3] += out[1] * rear_gain * 0.7f;
            }
        }
    }
    // Voices that played this whole buffer: kept until their end
    g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(), [](const Voice& v) { return v.t >= LENGTH; }),
                   g_voices.end());
}
