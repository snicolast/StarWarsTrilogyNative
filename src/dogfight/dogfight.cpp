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
    float radius;   // the model's (out/model_catalog.txt)
    int hits;       // cannon hits it takes (0: it only sparks)
};
const Shape XWING{{0xEE9EB4, 0xEEC840, 0xEE1A15}, 3, 7.2f, 0};
const Shape YWING{{0xEEF1E4}, 1, 9.0f, 0};
const Shape TIE{{0xEDB841}, 1, 5.2f, 2};
const Shape TIE_ADVANCED{{0xEBA43F}, 1, 4.7f, 3};
const Shape FALCON{{0xF0B18D}, 1, 16.1f, 0};
const Shape DESTROYER{{0xE17F78}, 1, 823.0f, 0};

struct Ship {
    const Shape* shape;
    Vec pos;
    Frame at;
    uint32_t id = 0;   // its own for the whole flight (the renderer follows it from frame to frame)
    int hits = 0;      // taken so far
};

// A cannon bolt: a short streak flying straight
struct Bolt {
    Vec pos, vel;
    float life = 0;   // seconds left
    float flown = 0;  // how far it has come from the cannon (its tail is no longer than that)
    uint32_t id = 0;
};
// An explosion: where, how big, and how far along it is (the renderer's
// fireball takes the frames of the game's sprite: 2 to 46)
struct Blast {
    Vec pos;
    float radius = 1;
    float frame = 2;
    uint32_t id = 0;
};

struct Sim {
    Vec pos;
    Frame at;
    float speed = 60, throttle = 0.4f;
    std::vector<Ship> ships;
    std::vector<Bolt> bolts;
    std::vector<Blast> blasts;
    uint32_t next_id = 1;
    float reload = 0;   // seconds until the next bolt
    int cannon = 0;     // the one to fire next, of four
    int kills = 0;
    int sounds = 0;        // DOGFIGHT_SOUND_* since they were last taken
    uint32_t target = 0;   // the id of the ship the target lock holds (0: none)
    bool lock_was = false;
    double time = 0;
    // The cockpit round the pilot: it leads the turns a little and is left
    // behind by a push of the engines, then settles (dogfight_cockpit_add's
    // sway: a turn and a shift in the view's axes)
    float sway[6] = {};
};
Sim g_sim;
std::atomic<float> g_shown_speed{0}, g_shown_throttle{0}, g_shown_nearest{0};
std::atomic<int> g_shown_ships{0}, g_shown_kills{0};
std::atomic<bool> g_shown_locked{false};   // for the HUD, drawn on another thread

// Speeds in the game's units (an X-wing is about 11 long: metres, roughly) per second
constexpr float SPEED_MIN = 20, SPEED_MAX = 160;
constexpr float TURN_RATE = 1.1f, ROLL_RATE = 2.2f;   // radians per second at the stick's end
// The cannons: one of the four every RELOAD seconds, in turn, their bolts
// meeting CONVERGE ahead of the nose
constexpr float BOLT_SPEED = 1400, BOLT_LIFE = 1.6f, BOLT_LENGTH = 42, RELOAD = 0.11f, CONVERGE = 450;

uint32_t g_seed = 0x5EED1234;
float rnd(float lo, float hi) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * float(g_seed >> 8) / float(1 << 24);
}

void reset() {
    g_seed = 0x5EED1234;
    g_sim = Sim{};
    g_shown_speed = g_sim.speed, g_shown_throttle = g_sim.throttle;
    auto add = [](const Shape& s, Vec pos, Frame at) { g_sim.ships.push_back({&s, pos, at, g_sim.next_id++, 0}); };
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
    g_shown_ships = int(g_sim.ships.size()), g_shown_kills = 0;
}

void blast(Sim& s, Vec pos, float radius) {
    if (s.blasts.size() >= 16) s.blasts.erase(s.blasts.begin());
    s.blasts.push_back({pos, radius, 2.0f, s.next_id++});
}

// Whether the point p lies in the ship. Fighters: a ball a little bigger
// than the model (generous: they are small and far). The capital ship: a
// wedge about its length (a guess from its radius: bow ahead, stern behind,
// widest at the stern, flat)
bool inside(const Ship& ship, Vec p) {
    Vec d = p + ship.pos * -1;
    float r = ship.shape->radius;
    if (ship.shape != &DESTROYER) return dot(d, d) < r * r * 1.6f * 1.6f;
    float x = dot(d, ship.at.right), y = dot(d, ship.at.down), z = dot(d, ship.at.fwd);
    if (std::fabs(z) > r) return false;
    float wide = 0.58f * r * (r - z) / (2 * r);
    return std::fabs(x) < wide && y < 0.14f * r && y > -0.22f * r;   // (its bridge stands above the hull)
}

// The cannons fire while the button is held; the bolts fly, and what they
// meet sparks, or blows up when it has taken enough
void cannons(Sim& s, bool fire, float dt) {
    s.reload = std::max(0.0f, s.reload - dt);
    if (fire && s.reload <= 0) {
        s.reload = RELOAD;
        s.sounds |= DOGFIGHT_SOUND_LASER;
        // The four wing tips: right and left, up and down, in turn
        static const float tip[4][2] = {{5.4f, -1.9f}, {-5.4f, 1.9f}, {-5.4f, -1.9f}, {5.4f, 1.9f}};
        Vec from = s.pos + s.at.right * tip[s.cannon][0] + s.at.down * tip[s.cannon][1] + s.at.fwd * 4.0f;
        Vec dir = unit(s.pos + s.at.fwd * CONVERGE + from * -1);
        s.cannon = (s.cannon + 1) & 3;
        s.bolts.push_back({from, dir * BOLT_SPEED + s.at.fwd * s.speed, BOLT_LIFE, 0, s.next_id++});
    }
    for (Bolt& b : s.bolts) {
        Vec move = b.vel * dt;
        b.life -= dt;
        // Along its path in steps shorter than a fighter
        float len = std::sqrt(dot(move, move));
        int steps = std::max(1, int(len / 4.0f) + 1);
        for (int i = 1; i <= steps && b.life > 0; i++) {
            Vec p = b.pos + move * (float(i) / float(steps));
            for (size_t k = 0; k < s.ships.size(); k++) {
                Ship& ship = s.ships[k];
                // (far ones are skipped by a ball about them first)
                Vec d = p + ship.pos * -1;
                float reach = ship.shape->radius * 1.7f;
                if (dot(d, d) > reach * reach || !inside(ship, p)) continue;
                b.life = 0;
                if (ship.shape->hits && ++ship.hits >= ship.shape->hits) {
                    blast(s, ship.pos, ship.shape->radius * 2.2f);
                    if (ship.id == s.target) s.target = 0;
                    s.ships.erase(s.ships.begin() + long(k));
                    s.kills++;
                    s.sounds |= DOGFIGHT_SOUND_KILL;
                } else {
                    blast(s, p + unit(b.vel) * -1.5f, ship.shape == &DESTROYER ? 6.0f : 2.5f);
                    s.sounds |= ship.shape->hits ? DOGFIGHT_SOUND_DAMAGE : DOGFIGHT_SOUND_HIT;
                }
                break;
            }
        }
        b.pos = b.pos + move;
        b.flown += len;
    }
    s.bolts.erase(std::remove_if(s.bolts.begin(), s.bolts.end(), [](const Bolt& b) { return b.life <= 0; }), s.bolts.end());
}

bool foe(const Ship& ship) { return ship.shape == &TIE || ship.shape == &TIE_ADVANCED; }

// The target lock: the enemy nearest the nose; pressed again, the next nearest
void lock(Sim& s) {
    const Ship* best = nullptr;
    float best_cos = -2;
    for (const Ship& ship : s.ships) {
        if (!foe(ship) || ship.id == s.target) continue;
        float c = dot(unit(ship.pos + s.pos * -1), s.at.fwd);
        if (c > best_cos) best_cos = c, best = &ship;
    }
    if (best) s.target = best->id;
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
    s.time += dt;
    {
        // Where the controls throw it (SWT_DOGFIGHT_SWAY: how much, 1 = as made, 0 = none)
        static const float amount = [] {
            const char* e = getenv("SWT_DOGFIGHT_SWAY");
            return e ? std::clamp(float(atof(e)), 0.0f, 5.0f) : 1.0f;
        }();
        float push = (target - s.speed) / (SPEED_MAX - SPEED_MIN);   // + while it gathers speed
        const float to[6] = {
            c.pitch * agile * 0.022f * amount,    // nose up: the dashboard rises
            -c.yaw * agile * 0.028f * amount,     // to the right: it goes right
            -c.roll * 0.045f * amount,            // rolls further than the stars
            c.yaw * agile * 0.006f * amount,      // and slides a little towards the turn
            c.pitch * agile * 0.004f * amount,
            -push * 0.035f * amount,              // pushed back into the seat: the dashboard moves away (nearer when it slows)
        };
        // A spring with a little overshoot: quick to go, slower to settle
        static float speed_of[6] = {};
        if (s.time <= dt) memset(speed_of, 0, sizeof(speed_of));
        for (int i = 0; i < 6; i++) {
            float accel = (to[i] - s.sway[i]) * 60.0f - speed_of[i] * 11.0f;
            speed_of[i] += accel * dt;
            s.sway[i] += speed_of[i] * dt;
        }
    }
    if (c.lock && !s.lock_was) lock(s);
    s.lock_was = c.lock;
    cannons(s, c.fire, dt);
    for (Blast& b : s.blasts) b.frame += 45.0f * dt;   // the fireball: a second
    s.blasts.erase(std::remove_if(s.blasts.begin(), s.blasts.end(), [](const Blast& b) { return b.frame > 46.0f; }), s.blasts.end());
    g_shown_ships = int(s.ships.size()), g_shown_kills = s.kills;
    g_shown_speed = s.speed, g_shown_throttle = s.throttle;
    g_shown_locked = s.target != 0;
    float nearest = 0;
    for (const Ship& ship : s.ships) {
        Vec d = ship.pos + s.pos * -1;
        float dist = std::sqrt(dot(d, d));
        if (nearest == 0 || dist < nearest) nearest = dist;
    }
    g_shown_nearest = nearest;
}

}  // namespace

void dogfight_reset() {
    dogfight_cockpit_load("models/cockpit");
    reset();
}
void dogfight_step(float dt) { step(dt); }
float dogfight_speed() { return g_shown_speed; }
float dogfight_throttle() { return g_shown_throttle; }
float dogfight_speed_part() { return std::clamp((g_shown_speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN), 0.0f, 1.0f); }
int dogfight_ships() { return g_shown_ships; }
int dogfight_kills() { return g_shown_kills; }
bool dogfight_locked() { return g_shown_locked; }
int dogfight_take_sounds() {
    int s = g_sim.sounds;
    g_sim.sounds = 0;
    return s;
}
float dogfight_nearest() { return g_shown_nearest; }

// The locked enemy on the view: corner brackets round it, and an arrow
// towards it near the middle when it is out of a 4:3 view or behind. Drawn
// a hand's width from the eye (view space: x right, y up, -z ahead), over
// the cockpit, as a sight on the pilot's visor would be
static void target_marks(R3DNativeScene& scene, const Sim& s, Vec cam) {
    const Ship* ship = nullptr;
    for (const Ship& o : s.ships)
        if (o.id == s.target) ship = &o;
    if (!s.target || !ship) return;
    Vec d = ship->pos + cam * -1;
    float dist = std::sqrt(dot(d, d));
    if (dist < 1e-3f) return;
    const float right = dot(d, s.at.right) / dist, up = -dot(d, s.at.down) / dist, ahead = dot(d, s.at.fwd) / dist;
    const float D = 0.2f;
    const uint32_t YELLOW = 0xFF30E0F0u, RED = 0xFF2840FFu;
    // A line between two points of view space, `wide` across, flat to the eye
    auto line = [&](Vec a, Vec b, float wide, uint32_t color) {
        Vec side = unit(cross(b + a * -1, a + b)) * (wide * 0.5f);
        const Vec q[6] = {a + side, b + side, b + side * -1, a + side, b + side * -1, a + side * -1};
        for (const Vec& p : q) {
            R3DNativeScene::Vertex v;
            v.pos[0] = p.x, v.pos[1] = p.y, v.pos[2] = p.z;
            v.color = color;
            scene.marks.push_back(v);
        }
    };
    // Always the same number of triangles (the display goes from one frame's
    // to the next's): what is not shown is drawn with no size
    const bool front = ahead > 0.05f;
    float bracket = 0;   // its half size
    {
        Vec v{right, up, -ahead};
        Vec r = unit(Vec{1, 0, 0} + v * -v.x);
        Vec u = cross(r, v) * -1;
        if (u.y < 0) u = u * -1;
        float half = D * std::max(ship->shape->radius * 1.8f / dist, 0.014f);
        bracket = half;
        // on it: the nose points inside the bracket
        bool on = front && std::sqrt(right * right + up * up) < half / D;
        float arm = front ? half * 0.45f : 0, wide = front ? D * 0.0018f : 0;
        Vec c = front ? v * D : Vec{0, 0, -D};
        for (int k = 0; k < 4; k++) {
            float sx = k & 1 ? 1.0f : -1.0f, sy = k & 2 ? 1.0f : -1.0f;
            Vec corner = c + r * (sx * half) + u * (sy * half);
            line(corner, corner + r * (-sx * arm), wide, on ? RED : YELLOW);
            line(corner, corner + u * (-sy * arm), wide, on ? RED : YELLOW);
        }
    }
    {
        const bool out = !front || std::fabs(right / ahead) > 0.58f || std::fabs(up / ahead) > 0.40f;
        float len = std::sqrt(right * right + up * up);
        float ex = len > 1e-4f ? right / len : 0, ey = len > 1e-4f ? up / len : 1;
        const float ring = 0.20f, size = out ? D * 0.022f : 0, wide = out ? D * 0.004f : 0;
        Vec p{D * std::sin(ring) * ex, D * std::sin(ring) * ey, -D * std::cos(ring)};
        Vec e{ex, ey, 0}, side{-ey, ex, 0};
        Vec tip = p + e * size;
        line(tip, p + e * (-size * 0.4f) + side * (size * 0.8f), wide, YELLOW);
        line(tip, p + e * (-size * 0.4f) + side * (-size * 0.8f), wide, YELLOW);
        // Its distance in small figures: inside the arrow, or under the
        // bracket when there is no arrow. Seven strokes a figure, four
        // figures, so their number never changes either
        const float high = D * 0.011f, across = high * 0.55f, gap = high * 0.35f;
        Vec at = out ? p + e * (-size * 2.4f) : front ? Vec{right, up, -ahead} * D + Vec{0, -(bracket + high * 1.6f), 0} : Vec{0, 0, -D};
        char digits[8];
        int n = snprintf(digits, sizeof(digits), "%d", std::clamp(int(dist + 0.5f), 0, 9999));
        static const uint8_t strokes[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};   // bit 0 the top, then clockwise, bit 6 the middle
        const uint32_t FAINT = 0xFF2090A0u;
        const bool shown = out || front;
        for (int k = 0; k < 4; k++) {
            uint8_t lit = shown && k < n ? strokes[digits[k] - '0'] : 0;
            float x0 = at.x + (float(k) - float(n) * 0.5f) * (across + gap) + gap * 0.5f, x1 = x0 + across;
            float y1 = at.y + high * 0.5f, ym = at.y, y0 = at.y - high * 0.5f;
            const float seg[7][4] = {{x0, y1, x1, y1}, {x1, y1, x1, ym}, {x1, ym, x1, y0}, {x0, y0, x1, y0}, {x0, ym, x0, y0}, {x0, y1, x0, ym}, {x0, ym, x1, ym}};
            for (int j = 0; j < 7; j++) {
                bool is = lit >> j & 1;
                line(Vec{seg[j][0], seg[j][1], at.z}, Vec{is ? seg[j][2] : seg[j][0], is ? seg[j][3] : seg[j][1], at.z}, is ? D * 0.0014f : 0, FAINT);
            }
        }
    }
}

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
    // (a cockpit of the user's takes the place of the game's ship round the camera)
    if (dogfight_cockpit_on()) {
        std::vector<DogfightContact> contacts;
        for (const Ship& ship : s.ships) {
            Vec d = ship.pos + s.pos * -1;
            DogfightContact c;
            c.right = dot(d, s.at.right), c.up = -dot(d, s.at.down), c.ahead = dot(d, s.at.fwd);
            c.nose[0] = dot(ship.at.fwd, s.at.right), c.nose[1] = -dot(ship.at.fwd, s.at.down), c.nose[2] = dot(ship.at.fwd, s.at.fwd);
            c.kind = ship.shape == &DESTROYER ? DogfightContact::CAPITAL : foe(ship) ? DogfightContact::FOE : DogfightContact::FRIEND;
            c.locked = ship.id == s.target;
            contacts.push_back(c);
        }
        // The battle's up (the world's y points down)
        const float level_up[3] = {-s.at.right.y, s.at.down.y, -s.at.fwd.y};
        dogfight_cockpit_add(*scene, s.time, s.sway, contacts, level_up);
    }
    else if (eye.ship) {
        R3DNativeScene::Item it;
        it.count = XWING.count;
        memcpy(it.models, XWING.models, sizeof(it.models));
        const Vec cols[4] = {s.at.right * -1, s.at.down * -1, s.at.fwd, s.pos};
        for (int c = 0; c < 4; c++) it.m[c * 4] = cols[c].x, it.m[c * 4 + 1] = cols[c].y, it.m[c * 4 + 2] = cols[c].z;
        it.m[15] = 1;
        it.key = 0xFFFF;
        scene->items.push_back(it);
    }
    target_marks(*scene, s, cam);
    const Vec axes[3] = {s.at.right, s.at.down, s.at.fwd};
    for (int i = 0; i < 3; i++) scene->cam_axes[i * 3] = axes[i].x, scene->cam_axes[i * 3 + 1] = axes[i].y, scene->cam_axes[i * 3 + 2] = axes[i].z;
    for (const Ship& ship : s.ships) {
        R3DNativeScene::Item it;
        it.count = ship.shape->count;
        memcpy(it.models, ship.shape->models, sizeof(it.models));
        // The game's models have y up and x to the left (seen in flight: with
        // y down the capital ships hung from their bridges)
        const Vec cols[4] = {ship.at.right * -1, ship.at.down * -1, ship.at.fwd, ship.pos};
        for (int c = 0; c < 4; c++) it.m[c * 4] = cols[c].x, it.m[c * 4 + 1] = cols[c].y, it.m[c * 4 + 2] = cols[c].z;
        it.m[15] = 1;
        it.key = ship.id;
        scene->items.push_back(it);
    }
    for (const Bolt& b : s.bolts) {
        if (b.flown <= 0) continue;   // (fired this instant)
        R3DNativeScene::Beam beam;
        Vec tail = b.pos + unit(b.vel) * -std::min(BOLT_LENGTH, b.flown);
        beam.a[0] = tail.x, beam.a[1] = tail.y, beam.a[2] = tail.z;
        beam.b[0] = b.pos.x, beam.b[1] = b.pos.y, beam.b[2] = b.pos.z;
        beam.key = b.id;
        scene->beams.push_back(beam);
    }
    for (const Blast& b : s.blasts) {
        R3DNativeScene::Blast o;
        o.pos[0] = b.pos.x, o.pos[1] = b.pos.y, o.pos[2] = b.pos.z;
        o.radius = b.radius;
        o.frame = int(b.frame);
        scene->blasts.push_back(o);
    }
    return scene;
}
