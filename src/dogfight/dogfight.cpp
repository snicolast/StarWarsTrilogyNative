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
    float speed = 0;   // units a second (the enemy fighters fly; the rest stand still for now)
    // An enemy fighter's pilot
    struct Pilot {
        enum Mode { CHASE, BREAK, EVADE, OVERSHOOT } mode = CHASE;   // (OVERSHOOT: shaken off, it flies straight on)
        float timer = 0;      // seconds left of a break or an evasion
        Vec way{0, 0, 1};     // where it flies meanwhile
        float jink = 0;       // evading: seconds until it changes its way
        float dread = 0;      // seconds the player has been on its tail
        float aimed = 0;      // seconds it has had the player in its sights
        float reload = 0, pause = 0;   // seconds until its next shot, and until its next burst
        int burst = 0;        // shots left of this one
        float skill = 0.5f;   // 0 a novice, 1 an ace: how soon it reacts, how tight it turns and shoots
        bool engaged = false; // one of the few that attack at a time
        bool passer = false;  // of those, the one that makes passes (the other goes for the tail)
        float fooled = 0;     // seconds until a hard turn of the player's can shake it off again
    } ai;
};

// A cannon bolt: a short streak flying straight
struct Bolt {
    Vec pos, vel;
    float life = 0;   // seconds left
    float flown = 0;  // how far it has come from the cannon (its tail is no longer than that)
    uint32_t id = 0;
    bool foe = false;   // an enemy's: green, and it is the player it can hit
    uint32_t owner = 0; // ... and whose
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
    float shield = 1;      // the player's, 0..1; it comes back slowly when nothing has hit for a while
    float unhurt = 99;     // seconds since the last hit
    bool warning = false;  // an enemy has the player in its sights
    uint32_t threat = 0;   // the enemy to worry about: aiming, firing, or the last to hit (0: none)
    float threat_hold = 0; // seconds since it last was a danger
    uint32_t hitter = 0;   // whose bolt hit last, and when
    double hit_time = -99;
    float hard = 0;        // seconds the player has been turning hard
    bool threat_was = false;
    float downed = 0;      // seconds left of the notice after being shot down (the flight starts again)
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
std::atomic<bool> g_shown_locked{false}, g_shown_warning{false}, g_shown_downed{false};
std::atomic<float> g_shown_shield{1};
bool g_ai = true;   // for the HUD, drawn on another thread

// Speeds in the game's units (an X-wing is about 11 long: metres, roughly) per second
constexpr float SPEED_MIN = 20, SPEED_MAX = 160;
constexpr float TURN_RATE = 1.1f, ROLL_RATE = 2.2f;   // radians per second at the stick's end
// The cannons: one of the four every RELOAD seconds, in turn, their bolts
// meeting CONVERGE ahead of the nose
constexpr float BOLT_SPEED = 1400, BOLT_LIFE = 1.6f, BOLT_LENGTH = 42, RELOAD = 0.11f, CONVERGE = 450;
// The enemy fighters: a little nimbler than the X-wing at its best, never
// faster; their bolts slower than the player's, so they can be dodged. Only
// ATTACKERS go for the player at a time; the rest circle and wait
constexpr float TIE_MIN = 50, TIE_MAX = 150, TIE_TURN = 1.25f, TIE_BOLT = 900, TIE_RELOAD = 0.16f;
constexpr int ATTACKERS = 2;
constexpr float HIT = 0.07f, SHIELD_BACK = 0.03f, SHIELD_WAIT = 4;   // a hit's worth of shield; a second's worth coming back, after that long unhurt

uint32_t g_seed = 0x5EED1234;
float rnd(float lo, float hi) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * float(g_seed >> 8) / float(1 << 24);
}

void reset() {
    g_seed = 0x5EED1234;
    g_sim = Sim{};
    g_shown_speed = g_sim.speed, g_shown_throttle = g_sim.throttle;
    auto add = [](const Shape& s, Vec pos, Frame at) { g_sim.ships.push_back({&s, pos, at, g_sim.next_id++, 0, 0, {}}); };
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
        Ship& tie = g_sim.ships.back();
        // (each its own pilot; the bent-winged ones are the aces)
        tie.ai.skill = tie.shape == &TIE_ADVANCED ? rnd(0.8f, 1.0f) : rnd(0.15f, 0.75f);
        tie.speed = g_ai ? 100.0f : 0.0f;
    }
    g_shown_ships = int(g_sim.ships.size()), g_shown_kills = 0;
}

bool foe(const Ship& ship) { return ship.shape == &TIE || ship.shape == &TIE_ADVANCED; }

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
        s.bolts.push_back({from, dir * BOLT_SPEED + s.at.fwd * s.speed, BOLT_LIFE, 0, s.next_id++, false, 0});
    }
    for (Bolt& b : s.bolts) {
        Vec move = b.vel * dt;
        b.life -= dt;
        // Along its path in steps shorter than a fighter
        float len = std::sqrt(dot(move, move));
        int steps = std::max(1, int(len / 4.0f) + 1);
        for (int i = 1; i <= steps && b.life > 0; i++) {
            Vec p = b.pos + move * (float(i) / float(steps));
            if (b.foe) {   // an enemy's: the player's ship alone, a ball about it
                Vec d = p + s.pos * -1;
                if (dot(d, d) > 5.0f * 5.0f) continue;
                b.life = 0;
                s.shield -= HIT;
                s.unhurt = 0;
                s.hitter = b.owner, s.hit_time = s.time;
                s.sounds |= DOGFIGHT_SOUND_HURT;
                // (the cockpit is thrown about)
                s.sway[0] += rnd(-0.02f, 0.02f), s.sway[2] += rnd(-0.05f, 0.05f), s.sway[5] -= 0.012f;
                break;
            }
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

// The enemy fighters fly by the rules the player does (a speed, a turn rate,
// no more) and each has a pilot: it goes for the player's tail, fires short
// bursts when it has the player in its sights (aiming ahead of it, with a
// spread: wider the more the player manoeuvres, and for the worse pilots),
// breaks away when it comes too near, and jinks when the player sits on its
// own tail. `manoeuvre`: how hard the player is turning, 0..1
void pilots(Sim& s, float manoeuvre, float dt) {
    // Who attacks: the nearest few
    std::vector<std::pair<float, Ship*>> foes;
    for (Ship& ship : s.ships) {
        if (!foe(ship)) continue;
        Vec d = ship.pos + s.pos * -1;
        // (one already attacking counts as nearer: they do not swap at every turn)
        foes.push_back({std::sqrt(dot(d, d)) * (ship.ai.engaged ? 0.7f : 1.0f), &ship});
    }
    std::sort(foes.begin(), foes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < foes.size(); i++) {
        foes[i].second->ai.engaged = i < size_t(ATTACKERS);
        foes[i].second->ai.passer = i == 1;
    }
    // A hard turn held a moment shakes off the one on the tail, if its pilot is taken in
    const bool hard_was = s.hard >= 0.6f;
    s.hard = manoeuvre >= 0.85f ? s.hard + dt : 0;
    const bool shake = s.hard >= 0.6f && !hard_was;
    const Vec player_vel = s.at.fwd * s.speed;
    s.warning = false;
    for (Ship& ship : s.ships) {
        if (!foe(ship)) continue;
        Ship::Pilot& ai = ship.ai;
        Vec to = s.pos + ship.pos * -1;
        float dist = std::max(std::sqrt(dot(to, to)), 1e-3f);
        Vec dir = to * (1 / dist);
        // Where a bolt fired now would meet the player
        Vec lead = s.pos + player_vel * (dist / TIE_BOLT);
        Vec want = dir;
        float want_speed = TIE_MAX;
        bool fire = false;

        ai.timer -= dt, ai.fooled -= dt;
        if (ai.mode != Ship::Pilot::CHASE && ai.timer <= 0) ai.mode = Ship::Pilot::CHASE;
        if (shake && ai.mode == Ship::Pilot::CHASE && ai.engaged && ai.fooled <= 0 && dist < 450 && dot(dir, s.at.fwd) > 0.5f) {
            ai.fooled = 6;
            if (rnd(0, 1) < 0.9f - 0.55f * ai.skill) {   // it flies on past the turn, and has to come round again
                ai.mode = Ship::Pilot::OVERSHOOT, ai.timer = rnd(2.5f, 3.5f), ai.way = ship.at.fwd;
                ai.aimed = 0, ai.burst = 0;
            }
        }
        if (ai.mode == Ship::Pilot::CHASE) {
            // The player behind it, pointing at it, near: it will not sit still for that
            bool hunted = dot(s.at.fwd, dir * -1) > 0.985f && dist < 700 && dot(ship.at.fwd, dir) < 0.3f;
            ai.dread = hunted ? ai.dread + dt : 0;
            if (ai.dread > 0.9f - 0.5f * ai.skill) {
                ai.mode = Ship::Pilot::EVADE, ai.timer = rnd(1.5f, 3.0f), ai.jink = 0, ai.dread = 0;
            } else if (dist < (ai.passer ? 150 : 80)) {
                // Too near (or its pass made): away to a side, and round again
                ai.mode = Ship::Pilot::BREAK, ai.timer = ai.passer ? rnd(4.0f, 6.0f) : rnd(2.0f, 3.5f);
                Vec side = cross(dir, ship.at.down) * (rnd(-1, 1) < 0 ? -1.0f : 1.0f) + ship.at.down * rnd(-0.6f, 0.6f);
                ai.way = unit(ship.at.fwd * 0.6f + side);
            }
        }
        if (ai.mode == Ship::Pilot::BREAK || ai.mode == Ship::Pilot::OVERSHOOT) {
            want = ai.way;
        } else if (ai.mode == Ship::Pilot::EVADE) {
            ai.jink -= dt;
            if (ai.jink <= 0) {   // a new way across the player's line of fire
                ai.jink = rnd(0.5f, 1.1f);
                Vec r{rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)};
                ai.way = unit(r + dir * -dot(r, dir) + ship.at.fwd * 0.3f);
            }
            want = ai.way;
        } else if (ai.engaged) {
            // How far round the player's tail it is: 1 right behind it
            float behind = std::clamp(dot(dir, s.at.fwd), 0.0f, 1.0f);
            Vec tail = s.pos + s.at.fwd * -150.0f;
            float w = ai.passer ? 1.0f : std::clamp((behind - 0.3f) / 0.4f, 0.0f, 1.0f);   // (the one that makes passes comes straight in)
            want = unit(tail + (lead + tail * -1) * w + ship.pos * -1);
            // Fast to get there; behind the player, the player's speed and a little more while far
            if (dist < 500) want_speed = behind > 0.6f ? s.speed + (dist - 200) * 0.3f : TIE_MAX * 0.8f;
            bool sighted = dot(ship.at.fwd, unit(lead + ship.pos * -1)) > 0.9976f && dist < 800;   // 4 degrees
            ai.aimed = sighted ? ai.aimed + dt : 0;
            fire = ai.aimed > 0.9f - 0.5f * ai.skill;
            s.warning |= sighted && ai.aimed > 0.3f;
        } else {
            // Waiting its turn: round the player at a distance, each on its own side
            float a = float(ship.id) * 2.4f + float(s.time) * 0.05f;
            Vec at = s.pos + Vec{std::cos(a) * 800, std::sin(float(ship.id)) * 250, std::sin(a) * 800};
            want = unit(at + ship.pos * -1);
            want_speed = TIE_MAX * 0.7f;
            ai.aimed = 0;
        }
        // Clear of the capital ships
        for (const Ship& big : s.ships) {
            if (big.shape != &DESTROYER) continue;
            Vec v = ship.pos + big.pos * -1;
            float near = std::sqrt(dot(v, v)) / (big.shape->radius * 1.1f);
            if (near < 1) want = unit(want + unit(v) * (0.3f + 2.0f * (1 - near)));
        }
        // The nose towards it, no faster than it can turn; the wings bank into the turn
        const float turn = TIE_TURN * (0.75f + 0.25f * ai.skill) * dt;
        Vec f = ship.at.fwd, nose = want;
        float c = std::clamp(dot(f, want), -1.0f, 1.0f);
        if (std::acos(c) > turn) {
            Vec across = want + f * -c;
            across = dot(across, across) > 1e-8f ? unit(across) : ship.at.right;
            nose = unit(f * std::cos(turn) + across * std::sin(turn));
        }
        Vec pull = (nose + f * -1) * (1 / std::max(turn, 1e-6f));   // which way it turns, 1 long at its hardest
        Vec down = unit(ship.at.down + (pull * -1.0f + ship.at.down * -dot(pull, pull)) * std::min(1.0f, 3.0f * dt));
        ship.at.fwd = nose;
        ship.at.right = unit(cross(down, nose));
        ship.at.down = cross(nose, ship.at.right);
        ship.speed += (std::clamp(want_speed, TIE_MIN, TIE_MAX) - ship.speed) * std::min(1.0f, 1.2f * dt);
        ship.pos = ship.pos + nose * (ship.speed * dt);

        // Its cannons: a burst, then a pause
        ai.reload -= dt, ai.pause -= dt;
        if (fire && ai.burst == 0 && ai.pause <= 0) ai.burst = 3 + int(rnd(0, 2.99f));
        if (ai.burst > 0 && ai.reload <= 0) {
            ai.reload = TIE_RELOAD;
            if (--ai.burst == 0) {
                ai.pause = rnd(2.0f, 4.0f) - ai.skill;
                // Its burst fired, it swings away and has to line up again
                if (ai.mode == Ship::Pilot::CHASE) {
                    ai.mode = Ship::Pilot::BREAK, ai.timer = rnd(2.5f, 4.0f) - ai.skill;
                    Vec side = cross(dir, ship.at.down) * (rnd(-1, 1) < 0 ? -1.0f : 1.0f) + ship.at.down * rnd(-0.6f, 0.6f);
                    ai.way = unit(ship.at.fwd * 0.5f + side);
                }
            }
            float spread = (0.040f + 0.060f * manoeuvre) * (1.5f - ai.skill);
            Vec from = ship.pos + nose * 6.0f;
            Vec aim = unit(unit(lead + from * -1) + Vec{rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)} * spread);
            s.bolts.push_back({from, aim * TIE_BOLT, 2.0f, 0, s.next_id++, true, ship.id});
            s.sounds |= DOGFIGHT_SOUND_TIE;
        }
    }
    // The one to worry about: the nearest of those aiming or firing, or the
    // last to hit; it stays the one a moment, so the arrow does not flicker
    s.threat_hold -= dt;
    {
        const Ship* worst = nullptr;
        float worst_dist = 1e30f;
        bool still = false;
        for (const Ship& ship : s.ships) {
            if (!foe(ship)) continue;
            still |= ship.id == s.threat;
            bool danger = ship.ai.burst > 0 || ship.ai.aimed > 0.2f || (ship.id == s.hitter && s.time - s.hit_time < 4);
            Vec d = ship.pos + s.pos * -1;
            if (danger && dot(d, d) < worst_dist) worst_dist = dot(d, d), worst = &ship;
        }
        // (threat_hold: seconds since it last was a danger. Another takes its
        // place after a moment of that; with no other it stays a while, so
        // the player can go after it)
        bool current = false;
        for (const Ship& ship : s.ships)
            if (ship.id == s.threat) current = ship.ai.burst > 0 || ship.ai.aimed > 0.2f || (ship.id == s.hitter && s.time - s.hit_time < 4);
        s.threat_hold = current ? 0 : s.threat_hold + 2 * dt;   // (it was counted down above: up by one a second)
        if (!still || (worst && worst->id != s.threat && s.threat_hold > 1.5f) || s.threat_hold > 8) {
            s.threat = worst ? worst->id : 0;
            s.threat_hold = 0;
        }
    }
    // More of them when few are left: from far off, any side
    if (foes.size() < 3) {
        Vec r = unit(Vec{rnd(-1, 1), rnd(-0.3f, 0.3f), rnd(-1, 1)});
        for (int i = 0; i < 3; i++) {
            Ship tie{i == 2 ? &TIE_ADVANCED : &TIE, s.pos + r * rnd(2500, 3200) + Vec{rnd(-150, 150), rnd(-150, 150), rnd(-150, 150)}, Frame{}, s.next_id++, 0, 0, {}};
            tie.at.fwd = r * -1;
            tie.at.right = unit(cross(Vec{0, 1, 0}, tie.at.fwd));
            tie.at.down = cross(tie.at.fwd, tie.at.right);
            tie.ai.skill = tie.shape == &TIE_ADVANCED ? rnd(0.8f, 1.0f) : rnd(0.15f, 0.75f);
            tie.speed = 100;
            s.ships.push_back(tie);
        }
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
    if (c.lock_threat && !s.threat_was && s.threat) s.target = s.threat;   // the lock on the one attacking
    s.threat_was = c.lock_threat;
    if (g_ai) pilots(s, std::clamp(std::fabs(c.pitch.load()) + std::fabs(c.yaw.load()), 0.0f, 1.0f), dt);
    cannons(s, c.fire, dt);
    // The shield comes back when nothing has hit for a while; with none left
    // the ship is lost and the flight starts again
    s.unhurt += dt;
    s.downed = std::max(0.0f, s.downed - dt);
    if (s.unhurt > SHIELD_WAIT) s.shield = std::min(1.0f, s.shield + SHIELD_BACK * dt);
    if (s.shield <= 0) {
        reset();
        s.downed = 4;
        s.sounds = DOGFIGHT_SOUND_KILL;
    }
    for (Blast& b : s.blasts) b.frame += 45.0f * dt;   // the fireball: a second
    s.blasts.erase(std::remove_if(s.blasts.begin(), s.blasts.end(), [](const Blast& b) { return b.frame > 46.0f; }), s.blasts.end());
    g_shown_ships = int(s.ships.size()), g_shown_kills = s.kills;
    g_shown_speed = s.speed, g_shown_throttle = s.throttle;
    g_shown_locked = s.target != 0;
    g_shown_shield = std::max(0.0f, s.shield), g_shown_warning = s.warning, g_shown_downed = s.downed > 0;
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
float dogfight_shield() { return g_shown_shield; }
bool dogfight_warning() { return g_shown_warning; }
bool dogfight_downed() { return g_shown_downed; }
void dogfight_set_ai(bool on) { g_ai = on; }
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
    const float D = 0.2f;
    const uint32_t YELLOW = 0xFF30E0F0u, RED = 0xFF2840FFu, ORANGE = 0xFF1080FFu;
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
    // An arrow at `ring` (radians from the middle) towards where (right, up) says
    auto arrow = [&](float right, float up, float ring, bool shown, uint32_t color, Vec* middle, float* length) {
        float len = std::sqrt(right * right + up * up);
        float ex = len > 1e-4f ? right / len : 0, ey = len > 1e-4f ? up / len : 1;
        const float size = shown ? D * 0.022f : 0, wide = shown ? D * 0.004f : 0;
        Vec p{D * std::sin(ring) * ex, D * std::sin(ring) * ey, -D * std::cos(ring)};
        Vec e{ex, ey, 0}, side{-ey, ex, 0};
        Vec tip = p + e * size;
        line(tip, p + e * (-size * 0.4f) + side * (size * 0.8f), wide, color);
        line(tip, p + e * (-size * 0.4f) + side * (-size * 0.8f), wide, color);
        if (middle) *middle = p + e * (-size * 2.4f);
        if (length) *length = size;
    };
    // Corner brackets round a ship (a direction from the eye, 1 long, and how
    // big it looks: radians); returns their half size
    auto brackets = [&](float right, float up, float ahead, float looks, bool shown, uint32_t color) {
        Vec v{right, up, -ahead};
        Vec r = unit(Vec{1, 0, 0} + v * -v.x);
        Vec u = cross(r, v) * -1;
        if (u.y < 0) u = u * -1;
        float half = D * std::max(looks, 0.014f);
        float arm = shown ? half * 0.45f : 0, wide = shown ? D * 0.0018f : 0;
        Vec c = shown ? v * D : Vec{0, 0, -D};
        for (int k = 0; k < 4; k++) {
            float sx = k & 1 ? 1.0f : -1.0f, sy = k & 2 ? 1.0f : -1.0f;
            Vec corner = c + r * (sx * half) + u * (sy * half);
            line(corner, corner + r * (-sx * arm), wide, color);
            line(corner, corner + u * (-sy * arm), wide, color);
        }
        return half;
    };
    // The one attacking: orange brackets round it, or when it is out of view
    // an orange arrow, farther out than the locked one's (always drawn, with
    // no size when not shown). The locked one's own marks say it when it is both
    {
        const Ship* threat = nullptr;
        for (const Ship& o : s.ships)
            if (s.threat && o.id == s.threat) threat = &o;
        float right = 0, up = 1, ahead = 1, looks = 0;
        bool shown = false, framed = false;
        if (threat) {
            Vec d = threat->pos + cam * -1;
            float dist = std::max(std::sqrt(dot(d, d)), 1e-3f);
            ahead = dot(d, s.at.fwd) / dist, right = dot(d, s.at.right) / dist, up = -dot(d, s.at.down) / dist;
            looks = threat->shape->radius * 1.8f / dist;
            shown = ahead <= 0.05f || std::fabs(right / ahead) > 0.58f || std::fabs(up / ahead) > 0.40f;
            framed = ahead > 0.05f && threat->id != s.target;
            // (blinking while it is the locked one too: the yellow arrow says the rest)
            if (shown && threat->id == s.target) shown = std::fmod(s.time, 0.4) < 0.25;
        }
        arrow(right, up, 0.27f, shown, ORANGE, nullptr, nullptr);
        brackets(right, up, ahead, looks, framed, ORANGE);
    }
    const Ship* ship = nullptr;
    for (const Ship& o : s.ships)
        if (o.id == s.target) ship = &o;
    if (!s.target || !ship) return;
    Vec d = ship->pos + cam * -1;
    float dist = std::sqrt(dot(d, d));
    if (dist < 1e-3f) return;
    const float right = dot(d, s.at.right) / dist, up = -dot(d, s.at.down) / dist, ahead = dot(d, s.at.fwd) / dist;
    // Always the same number of triangles (the display goes from one frame's
    // to the next's): what is not shown is drawn with no size
    const bool front = ahead > 0.05f;
    float bracket = 0;   // its half size
    {
        // on it: the nose points inside the brackets
        float looks = std::max(ship->shape->radius * 1.8f / dist, 0.014f);
        bool on = front && std::sqrt(right * right + up * up) < looks;
        bracket = brackets(right, up, ahead, looks, front, on ? RED : YELLOW);
    }
    {
        const bool out = !front || std::fabs(right / ahead) > 0.58f || std::fabs(up / ahead) > 0.40f;
        Vec inside;
        arrow(right, up, 0.20f, out, YELLOW, &inside, nullptr);
        // Its distance in small figures: inside the arrow, or under the
        // bracket when there is no arrow. Seven strokes a figure, four
        // figures, so their number never changes either
        const float high = D * 0.011f, across = high * 0.55f, gap = high * 0.35f;
        Vec at = out ? inside : front ? Vec{right, up, -ahead} * D + Vec{0, -(bracket + high * 1.6f), 0} : Vec{0, 0, -D};
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
            c.attacking = foe(ship) && ship.ai.aimed > 0;
            c.threat = ship.id == s.threat;
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
        if (b.foe) beam.color = 0xFF30FF28u;   // the green of the Empire's
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
