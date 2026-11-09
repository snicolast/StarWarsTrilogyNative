#include "dogfight.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

std::atomic<int> g_dogfight{0};
DogfightControls g_dogfight_controls;

namespace {

// World axes: x right, y down, z forward (the models': x left, y up, z forward)
struct Vec {
    float x = 0, y = 0, z = 0;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec unit(Vec a) {
    float l = std::sqrt(dot(a, a));
    return l > 0 ? a * (1 / l) : Vec{0, 0, 1};
}

// An orientation: where the right, down and forward of a ship point
struct Frame {
    Vec right{1, 0, 0}, down{0, 1, 0}, fwd{0, 0, 1};
    // Turned about its own axes (radians): pitch up, yaw right, roll right
    void turn(float pitch, float yaw, float roll) {
        Vec f = fwd + down * -pitch + right * yaw;     // small angles: a step a frame
        Vec r = right + down * roll + fwd * -yaw;
        fwd = unit(f);
        down = unit(cross(fwd, r));
        right = cross(down, fwd);
    }
};
Frame facing(float yaw, float pitch, float roll) {   // radians, from the world's axes
    Frame f;
    const int steps = 64;   // (turn() is for small angles)
    for (int i = 0; i < steps; i++) f.turn(0, yaw / steps, 0);
    for (int i = 0; i < steps; i++) f.turn(pitch / steps, 0, 0);
    for (int i = 0; i < steps; i++) f.turn(0, 0, roll / steps);
    return f;
}

// The game's models (VROM addresses, out/model_catalog.txt; seen with
// --view-model). An X-wing is three models on one origin
struct Shape {
    uint32_t models[4];
    int count;
};
const Shape XWING{{0xEE9EB4, 0xEEC840, 0xEE1A15}, 3};
const Shape YWING{{0xEEF1E4}, 1};
const Shape TIE{{0xEDB841}, 1};
const Shape TIE_ADVANCED{{0xEBA43F}, 1};
const Shape FALCON{{0xF0B18D}, 1};
const Shape DESTROYER{{0xE17F78}, 1};

struct Ship {
    const Shape* shape;
    Vec pos;
    Frame at;
};

struct Sim {
    Vec pos;
    Frame at;
    float speed = 60, throttle = 0.4f;
    std::vector<Ship> ships;
};
Sim g_sim;
std::atomic<float> g_shown_speed{0}, g_shown_throttle{0};   // for the HUD, drawn on another thread

// Speeds in the game's units (an X-wing is about 11 long: metres, roughly) per second
constexpr float SPEED_MIN = 20, SPEED_MAX = 160;
constexpr float TURN_RATE = 1.1f, ROLL_RATE = 2.2f;   // radians per second at the stick's end

uint32_t g_seed = 0x5EED1234;
float rnd(float lo, float hi) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * float(g_seed >> 8) / float(1 << 24);
}

void reset() {
    g_seed = 0x5EED1234;
    g_sim = Sim{};
    g_shown_speed = g_sim.speed, g_shown_throttle = g_sim.throttle;
    auto add = [](const Shape& s, Vec pos, Frame at) { g_sim.ships.push_back({&s, pos, at}); };
    // Two capital ships ahead, one side on and one coming
    add(DESTROYER, {600, 250, 4200}, facing(1.9f, 0, 0.1f));
    add(DESTROYER, {-2200, -400, 6500}, facing(2.9f, 0.05f, 0));
    // The squadron around the player
    add(XWING, {-28, 4, 45}, Frame{});
    add(XWING, {30, -3, 60}, Frame{});
    add(YWING, {-55, 12, 20}, Frame{});
    add(FALCON, {70, 18, 140}, facing(-0.15f, 0, 0.1f));
    // TIE fighters between here and the capital ships, turned every way
    for (int i = 0; i < 14; i++) {
        Vec p{rnd(-500, 500), rnd(-220, 220), rnd(250, 2600)};
        add(i % 7 == 6 ? TIE_ADVANCED : TIE, p, facing(rnd(-3.1f, 3.1f), rnd(-0.5f, 0.5f), rnd(-0.6f, 0.6f)));
    }
}

void step(float dt) {
    const DogfightControls& c = g_dogfight_controls;
    Sim& s = g_sim;
    if (c.absolute) s.throttle = std::clamp(c.throttle.load(), 0.0f, 1.0f);
    else s.throttle = std::clamp(s.throttle + c.throttle * 0.6f * dt, 0.0f, 1.0f);
    float target = SPEED_MIN + (SPEED_MAX - SPEED_MIN) * s.throttle;
    s.speed += (target - s.speed) * std::min(1.0f, 1.5f * dt);
    // Turns best at half throttle, a third slower flat out or crawling
    float agile = 1.0f - 0.33f * std::fabs(s.throttle - 0.5f) * 2.0f;
    s.at.turn(c.pitch * TURN_RATE * agile * dt, c.yaw * TURN_RATE * agile * dt, c.roll * ROLL_RATE * dt);
    s.pos = s.pos + s.at.fwd * (s.speed * dt);
    g_shown_speed = s.speed, g_shown_throttle = s.throttle;
}

}  // namespace

void dogfight_reset() { reset(); }
void dogfight_step(float dt) { step(dt); }
float dogfight_speed() { return g_shown_speed; }
float dogfight_throttle() { return g_shown_throttle; }
int dogfight_ships() { return int(g_sim.ships.size()); }

std::shared_ptr<const R3DNativeScene> dogfight_scene() {
    auto scene = std::make_shared<R3DNativeScene>();
    const Sim& s = g_sim;
    // The player's own X-wing round the camera: the eye sits in its cockpit, so
    // the nose and the cannons show ahead. Where, from the model's origin:
    // SWT_DOGFIGHT_EYE=right,up,forward (units; "off": no ship, the bare view)
    static const struct Eye {
        bool ship = true;
        float right = 0, up = 1.0f, fwd = -1.0f;
        Eye() {
            const char* e = getenv("SWT_DOGFIGHT_EYE");
            if (!e) return;
            if (sscanf(e, "%f,%f,%f", &right, &up, &fwd) != 3) ship = false, right = up = fwd = 0;
        }
    } eye;
    Vec cam = s.pos + s.at.right * eye.right + s.at.down * -eye.up + s.at.fwd * eye.fwd;
    scene->cam_pos[0] = cam.x, scene->cam_pos[1] = cam.y, scene->cam_pos[2] = cam.z;
    if (eye.ship) {
        R3DNativeScene::Item it;
        it.count = XWING.count;
        memcpy(it.models, XWING.models, sizeof(it.models));
        const Vec cols[4] = {s.at.right * -1, s.at.down * -1, s.at.fwd, s.pos};
        for (int c = 0; c < 4; c++) it.m[c * 4] = cols[c].x, it.m[c * 4 + 1] = cols[c].y, it.m[c * 4 + 2] = cols[c].z;
        it.m[15] = 1;
        it.key = 0xFFFF;
        scene->items.push_back(it);
    }
    const Vec axes[3] = {s.at.right, s.at.down, s.at.fwd};
    for (int i = 0; i < 3; i++) scene->cam_axes[i * 3] = axes[i].x, scene->cam_axes[i * 3 + 1] = axes[i].y, scene->cam_axes[i * 3 + 2] = axes[i].z;
    uint32_t key = 1;
    for (const Ship& ship : s.ships) {
        R3DNativeScene::Item it;
        it.count = ship.shape->count;
        memcpy(it.models, ship.shape->models, sizeof(it.models));
        // The game's models have y up and x to the left (seen in flight: with
        // y down the capital ships hung from their bridges)
        const Vec cols[4] = {ship.at.right * -1, ship.at.down * -1, ship.at.fwd, ship.pos};
        for (int c = 0; c < 4; c++) it.m[c * 4] = cols[c].x, it.m[c * 4 + 1] = cols[c].y, it.m[c * 4 + 2] = cols[c].z;
        it.m[15] = 1;
        it.key = key++;
        scene->items.push_back(it);
    }
    return scene;
}
