// DOGFIGHT's flight (src/dogfight/dogfight.cpp): which way each control turns
// the ship, that its axes stay square, and the throttle
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../src/dogfight/dogfight.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

// The camera's right, down and forward after `seconds` with the controls held
struct Flown {
    float pos[3], right[3], down[3], fwd[3];
};
static Flown fly(float pitch, float yaw, float roll, float throttle, float seconds) {
    DogfightControls& c = g_dogfight_controls;
    c.pitch = pitch, c.yaw = yaw, c.roll = roll, c.throttle = throttle;
    c.absolute = false;
    dogfight_reset();
    for (int i = 0; i < int(seconds * 60); i++) dogfight_step(1.0f / 60);
    auto s = dogfight_scene();
    Flown f;
    for (int i = 0; i < 3; i++) {
        f.pos[i] = s->cam_pos[i];
        f.right[i] = s->cam_axes[i], f.down[i] = s->cam_axes[3 + i], f.fwd[i] = s->cam_axes[6 + i];
    }
    return f;
}
static float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

int main() {
    dogfight_set_ai(false);   // (the enemy fighters stand still for the tests of the flight and the cannons)
    // World axes: x right, y down, z forward
    Flown straight = fly(0, 0, 0, 0, 1);
    CHECK(straight.pos[2] > 20 && std::fabs(straight.pos[0]) < 1e-3f && std::fabs(straight.pos[1] + 1) < 1e-3f);   // (the eye: 1 above the ship)
    Flown right = fly(0, 1, 0, 0, 0.5f);
    CHECK(right.fwd[0] > 0.3f && std::fabs(right.fwd[1]) < 1e-3f);   // the nose to the right, level
    Flown up = fly(1, 0, 0, 0, 0.5f);
    CHECK(up.fwd[1] < -0.3f && std::fabs(up.fwd[0]) < 1e-3f);        // stick back: the nose up (-y)
    Flown rolled = fly(0, 0, 1, 0, 0.3f);
    CHECK(rolled.right[1] > 0.3f && rolled.fwd[2] > 0.999f);         // the right wing down, the nose where it was

    // Everything held for a minute: the axes are still unit and square
    Flown long_turn = fly(0.7f, -0.5f, 1, 1, 60);
    CHECK(std::fabs(dot(long_turn.fwd, long_turn.fwd) - 1) < 1e-4f);
    CHECK(std::fabs(dot(long_turn.right, long_turn.right) - 1) < 1e-4f);
    CHECK(std::fabs(dot(long_turn.down, long_turn.down) - 1) < 1e-4f);
    CHECK(std::fabs(dot(long_turn.fwd, long_turn.right)) < 1e-4f);
    CHECK(std::fabs(dot(long_turn.fwd, long_turn.down)) < 1e-4f);
    CHECK(std::fabs(dot(long_turn.right, long_turn.down)) < 1e-4f);

    // The throttle: up to full and down to the slowest, never past either
    fly(0, 0, 0, 1, 10);
    float fast = dogfight_speed();
    CHECK(dogfight_throttle() == 1.0f && fast > 150 && fast <= 160.01f);
    fly(0, 0, 0, -1, 10);
    CHECK(dogfight_throttle() == 0.0f && dogfight_speed() < 30 && dogfight_speed() >= 19.99f);
    // A flight stick's lever is the throttle itself
    g_dogfight_controls.absolute = true;
    g_dogfight_controls.throttle = 0.25f;
    dogfight_step(1.0f / 60);
    CHECK(dogfight_throttle() == 0.25f);

    // The scene: every ship has its models and a square matrix
    auto scene = dogfight_scene();
    CHECK(int(scene->items.size()) == dogfight_ships() + 1 && dogfight_ships() > 10);   // and the player's own
    for (const auto& it : scene->items) {
        CHECK(it.count >= 1 && it.count <= 4 && it.models[0] != 0 && it.m[15] == 1);
        CHECK(std::fabs(dot(it.m, it.m) - 1) < 1e-3f && std::fabs(dot(it.m, it.m + 4)) < 1e-3f);
        // not mirrored: x cross y = z
        float z[3] = {it.m[1] * it.m[6] - it.m[2] * it.m[5], it.m[2] * it.m[4] - it.m[0] * it.m[6], it.m[0] * it.m[5] - it.m[1] * it.m[4]};
        CHECK(std::fabs(dot(z, it.m + 8) - 1) < 1e-3f);
    }
    // The cannons: bolts while the button is held, none left a while after
    {
        dogfight_reset();
        g_dogfight_controls.absolute = false;
        g_dogfight_controls.fire = true;
        fly(0, 0, 0, 0, 0.5f);
        // (the dust going by is drawn as plain streaks: not counted)
        auto bolts = [](const R3DNativeScene& sc) {
            int n = 0, dust = 0;
            for (const auto& b : sc.beams) b.plain ? dust++ : n++;
            CHECK(dust > 20);
            return n;
        };
        auto firing = dogfight_scene();
        CHECK(bolts(*firing) >= 3 && bolts(*firing) <= 8);
        g_dogfight_controls.fire = false;
        fly(0, 0, 0, 0, 3);
        CHECK(bolts(*dogfight_scene()) == 0);
        // The target lock: a press takes an enemy and marks it (brackets and
        // an arrow, ten lines, its shield's four pips and the distance's 28 strokes); another press, another enemy
        CHECK(!dogfight_locked() && dogfight_scene()->marks.size() == 10 * 6);   // (the arrow to the one attacking: there, with no size)
        g_dogfight_controls.lock = true;
        dogfight_step(1.0f / 60);
        g_dogfight_controls.lock = false;
        dogfight_step(1.0f / 60);
        CHECK(dogfight_locked() && dogfight_scene()->marks.size() == 52 * 6);
        for (const auto& v : dogfight_scene()->marks) CHECK(v.pos[2] < 0 && v.pos[2] > -0.3f);
        // Chasing the nearest TIE fighter and firing at it: it blows up
        const int before = dogfight_ships();
        bool blast_seen = false;
        float target[3] = {};
        {
            auto sc = dogfight_scene();
            float best = 1e30f;
            for (const auto& it : sc->items) {
                if (it.models[0] != 0xEDB841) continue;
                float d[3] = {it.m[12] - sc->cam_pos[0], it.m[13] - sc->cam_pos[1], it.m[14] - sc->cam_pos[2]};
                if (dot(d, d) < best) best = dot(d, d), memcpy(target, it.m + 12, sizeof(target));
            }
            CHECK(best < 1e30f);
        }
        for (int i = 0; i < 60 * 90 && dogfight_kills() == 0; i++) {
            auto sc = dogfight_scene();
            float d[3] = {target[0] - sc->cam_pos[0], target[1] - sc->cam_pos[1], target[2] - sc->cam_pos[2]};
            float len = std::sqrt(dot(d, d));
            float x = dot(d, sc->cam_axes) / len, y = dot(d, sc->cam_axes + 3) / len, z = dot(d, sc->cam_axes + 6) / len;
            g_dogfight_controls.yaw = std::clamp(4 * x, -1.0f, 1.0f);
            g_dogfight_controls.pitch = std::clamp(-4 * y, -1.0f, 1.0f);
            g_dogfight_controls.throttle = len > 400 ? 1.0f : -1.0f;
            g_dogfight_controls.fire = z > 0.999f && len < 1200;
            dogfight_step(1.0f / 60);
            blast_seen |= !dogfight_scene()->blasts.empty();
        }
        CHECK(dogfight_kills() == 1 && dogfight_ships() == before - 1 && blast_seen);
        g_dogfight_controls.fire = false;
        g_dogfight_controls.yaw = g_dogfight_controls.pitch = g_dogfight_controls.throttle = 0;
        fly(0, 0, 0, 0, 2);
        CHECK(dogfight_scene()->blasts.empty());
    }
    // The braking turn: it turns tighter and slows, while its charge lasts; the charge comes back
    {
        Flown plain = fly(0, 1, 0, 0, 1.5f);
        g_dogfight_controls.drift = true;
        Flown tight = fly(0, 1, 0, 0, 1.5f);
        CHECK(tight.fwd[2] < plain.fwd[2] - 0.2f && dogfight_speed() < 40 && dogfight_charge() < 0.5f);
        for (int i = 0; i < 60 * 3; i++) dogfight_step(1.0f / 60);
        CHECK(dogfight_charge() == 0);
        g_dogfight_controls.drift = false;
        for (int i = 0; i < 60 * 8; i++) dogfight_step(1.0f / 60);
        CHECK(dogfight_charge() == 1);
        g_dogfight_controls.yaw = 0;
    }
    // The enemy pilots: flying straight, the fighters move, stay whole
    // (orthonormal, within their speeds), fire green bolts and wear the
    // shield down; the warning comes on at some time
    {
        dogfight_set_ai(true);
        dogfight_reset();
        g_dogfight_controls.pitch = g_dogfight_controls.yaw = g_dogfight_controls.roll = g_dogfight_controls.throttle = 0;
        auto start = dogfight_scene();
        bool green = false, warned = false, tie_sound = false, lost = false;
        std::shared_ptr<const R3DNativeScene> end;   // (a few seconds in: the match may be lost, and begun again, within the minute)
        float least = 1;
        for (int i = 0; i < 60 * 60; i++) {
            dogfight_step(1.0f / 60);
            tie_sound |= (dogfight_take_sounds() & DOGFIGHT_SOUND_TIE) != 0;
            warned |= dogfight_warning();
            least = std::min(least, dogfight_shield());
            lost |= dogfight_result() == DOGFIGHT_DEFEAT;
            if (i == 60 * 5) end = dogfight_scene();
            if (i % 30) continue;
            auto sc = dogfight_scene();
            for (const auto& b : sc->beams) green |= b.color == 0xFF30FF28u;
            for (const auto& it : sc->items) {
                CHECK(std::fabs(dot(it.m, it.m) - 1) < 1e-3f && std::fabs(dot(it.m + 4, it.m + 4) - 1) < 1e-3f && std::fabs(dot(it.m + 8, it.m + 8) - 1) < 1e-3f);
                CHECK(std::fabs(dot(it.m, it.m + 4)) < 1e-3f && std::fabs(dot(it.m, it.m + 8)) < 1e-3f && std::fabs(dot(it.m + 4, it.m + 8)) < 1e-3f);
            }
        }
        int moved = 0;
        for (size_t i = 0; i < std::min(start->items.size(), end->items.size()); i++)
            if (start->items[i].models[0] == 0xEDB841 && std::fabs(start->items[i].m[12] - end->items[i].m[12]) > 1) moved++;
        printf("ai: least shield %.2f, moved %d, green %d, warned %d\n", least, moved, green, warned);
        CHECK(moved >= 5 && green && warned && tie_sound && least < 1.0f);
        // Last man standing: flying straight on, the player falls, and the
        // match begins again with both sides whole
        CHECK(lost);
        for (int i = 0; i < 60 * 300 && dogfight_result() != DOGFIGHT_DEFEAT; i++) dogfight_step(1.0f / 60);
        CHECK(dogfight_result() == DOGFIGHT_DEFEAT && dogfight_shield() == 0);
        for (int i = 0; i < 60 * 8; i++) dogfight_step(1.0f / 60);
        CHECK(dogfight_result() == 0 && dogfight_left(false) == 6 && dogfight_left(true) == 6 && dogfight_shield() == 1);
        dogfight_set_ai(false);
        dogfight_reset();
    }
    // The HUD: something drawn bottom left, nothing elsewhere, all of it opaque
    {
        const int w = 496, h = 384;
        static uint32_t front[w * h];
        fly(0, 0, 0, 1, 10);
        dogfight_draw_hud(front, w, h);
        int lit = 0, outside = 0, soft = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t p = front[y * w + x];
                if (!p) continue;
                lit++;
                if (x > 200 || y < h - 72) outside++;
                if ((p >> 24) != 0xFF) soft++;
            }
        CHECK(lit > 400 && outside == 0 && soft == 0);
    }
    if (!g_failed) printf("dogfight: ok\n");
    return g_failed ? 1 : 0;
}
