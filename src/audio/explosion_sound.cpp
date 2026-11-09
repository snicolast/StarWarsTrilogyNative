#include "explosion_sound.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto MAX_AGE = std::chrono::milliseconds(300);

struct Event {
    float pan;
    Clock::time_point t;
};
std::atomic<bool> g_on{false};
std::mutex g_mutex;
std::deque<Event> g_events;

void drop_old(Clock::time_point now) {
    while (!g_events.empty() && now - g_events.front().t > MAX_AGE) g_events.pop_front();
}
}  // namespace

void explosion_sound_enable(bool on) {
    if (g_on.exchange(on) && !on) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_events.clear();
    }
}

bool explosion_sound_enabled() { return g_on.load(std::memory_order_relaxed); }

void explosion_sound_event(float pan) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Clock::time_point now = Clock::now();
    drop_old(now);
    if (g_events.size() < 16) g_events.push_back({pan, now});
}

bool explosion_sound_take(float& pan) {
    std::lock_guard<std::mutex> lock(g_mutex);
    drop_old(Clock::now());
    if (g_events.empty()) return false;
    pan = g_events.front().pan;
    g_events.pop_front();
    return true;
}

bool explosion_sound_is_sample(const uint8_t* ram, uint32_t mask, uint32_t base) {
    // FNV-1a of the first 256 bytes (LW_SNDLOG's hash column): the three
    // explosions of Yavin (1.5, 2 and 3 s), and nothing else in a stage
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 256; i++) h = (h ^ ram[(base + i) & mask]) * 16777619u;
    return h == 0xE6E8F1F0u || h == 0xCDB04F4Fu || h == 0x0880F73Bu;
}
