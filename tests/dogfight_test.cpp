// DOGFIGHT's flight (src/dogfight/dogfight.cpp): which way each control turns
// the ship, that its axes stay square, and the throttle
#include <cmath>
#include <cstdio>

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
                if (x > 200 || y < h - 60) outside++;
                if ((p >> 24) != 0xFF) soft++;
            }
        CHECK(lit > 400 && outside == 0 && soft == 0);
    }
    if (!g_failed) printf("dogfight: ok\n");
    return g_failed ? 1 : 0;
}
