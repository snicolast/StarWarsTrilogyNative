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
    int hits;       // whether the other side's hits bring it down (0: it only sparks); the player's own shots only spark on friends
};
const Shape XWING{{0xEE9EB4, 0xEEC840, 0xEE1A15}, 3, 7.2f, 5};
const Shape YWING{{0xEEF1E4}, 1, 9.0f, 7};
const Shape TIE{{0xEDB841}, 1, 5.2f, 2};
const Shape TIE_ADVANCED{{0xEBA43F}, 1, 4.7f, 3};
const Shape FALCON{{0xF0B18D}, 1, 16.1f, 0};
const Shape DESTROYER{{0xE17F78}, 1, 823.0f, 0};

struct Ship {
    const Shape* shape;
    Vec pos;
    Frame at;
    uint32_t id = 0;   // its own for the whole flight (the renderer follows it from frame to frame)
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
        uint32_t prey = 0;    // the ship it goes for (0: the player, for an enemy; none, for a friend)
        float bored = 0;      // seconds until it thinks again about which
        bool chased = false;  // someone has it in their sights
        float harried = 0;    // ... and for how long
    } ai;
    // A fighter's shield, as the player's: 1 whole, a hit takes HIT of it, and
    // it comes back when nothing has hit for a while
    float shield = 1, unhurt = 99;
};

bool foe(const Ship& ship) { return ship.shape == &TIE || ship.shape == &TIE_ADVANCED; }
bool friendly(const Ship& ship) { return ship.shape == &XWING || ship.shape == &YWING || ship.shape == &FALCON; }

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
    float push = 0;        // how hard the engines push, -1..1 (dogfight_push), and the same followed slowly (the view widens with it)
    float surge = 0;
    float charge = 1;      // what is left for the braking turn, 0..1
    bool drifting = false, spent = false;
    float drifted = 0;     // seconds this braking turn has lasted
    float kick = 0;        // seconds left of the push out of one
    // The friends' voices: what is about to be said and in how long (a line
    // follows what it is about by a moment), what was said last and for how
    // long the screen still shows it, and the rests between lines
    int say = 0;
    float say_in = 0, message_left = 0, voice_rest = 0, help_rest = 10;
    int message = 0;
    double last_kill = -99;
    bool shaken = false;   // this hard turn has had its try at shaking them off
    float rested = 9;      // seconds since it was last used (the charge comes back after a moment)
    bool threat_was = false;
    // Last man standing: how many fighters there were at the start (the
    // player among them), how the match went (0: still on; DOGFIGHT_VICTORY,
    // DOGFIGHT_DEFEAT) and the seconds left until the next begins, how long
    // it took, and how much of the shields' return is left (it wanes as ships fall)
    int total = 0, over = 0;
    float over_left = 0, took = 0, regen = 1;
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
std::atomic<int> g_shown_message{0};
std::atomic<bool> g_shown_locked{false}, g_shown_warning{false};
std::atomic<int> g_shown_result{0}, g_shown_friends{0}, g_shown_foes{0};
std::atomic<float> g_shown_target_shield{-1}, g_shown_took{0};
std::atomic<float> g_shown_shield{1}, g_shown_push{0}, g_shown_charge{1};
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
// Last man standing: SIDE fighters each (the player one of the X-wings), all
// with the same shield: a hit's worth of it (four bring a ship down), a
// second's worth coming back after that long unhurt, while most still fly
constexpr int SIDE = 6;
// (the friends' aim against the enemy's: tighter, as the TIE is the smaller and nimbler ship; set so
// that the sides are about even with the player doing a fair share)
constexpr float FRIEND_SPREAD = 0.7f;
// ... and less from WANE_FROM seconds into the match, none WANE_FOR later: no match goes on for ever
constexpr float WANE_FROM = 90, WANE_FOR = 90;
constexpr float HIT = 0.25f, SHIELD_BACK = 0.125f, SHIELD_WAIT = 4;

uint32_t g_seed = 0x5EED1234;
float rnd(float lo, float hi) {
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * float(g_seed >> 8) / float(1 << 24);
}

void reset() {
    g_sim = Sim{};
    g_shown_speed = g_sim.speed, g_shown_throttle = g_sim.throttle;
    auto add = [](const Shape& s, Vec pos, Frame at) { g_sim.ships.push_back({&s, pos, at, g_sim.next_id++, 0, {}}); };
    // Two capital ships ahead, one side on and one coming
    add(DESTROYER, {600, 250, 4200}, facing(1.9f, 0, 0.1f));
    add(DESTROYER, {-2200, -400, 6500}, facing(2.9f, 0.05f, 0));
    // The squadron beside the player, and as many TIE fighters coming at
    // them from far ahead: the two sides meet head on
    static const float place[5][3] = {{-45, 6, 20}, {45, -4, 30}, {-90, 14, -25}, {90, -10, -15}, {0, 22, -60}};
    for (int i = 0; i < SIDE - 1; i++) {
        add(XWING, {place[i % 5][0] * float(1 + i / 5), place[i % 5][1], place[i % 5][2] - 60.0f * float(i / 5)}, Frame{});
        g_sim.ships.back().ai.skill = rnd(0.35f, 0.8f);
    }
    for (int i = 0; i < SIDE; i++) {
        add(TIE, {rnd(-260, 260), rnd(-120, 120), 2600 + rnd(-200, 200)}, facing(3.14159265f, 0, rnd(-0.4f, 0.4f)));
        g_sim.ships.back().ai.skill = rnd(0.35f, 0.8f);
    }
    for (Ship& ship : g_sim.ships)
        if (ship.shape->hits) ship.speed = g_ai ? 100.0f : 0.0f;
    g_sim.total = 2 * SIDE;
    g_shown_ships = int(g_sim.ships.size()), g_shown_kills = 0;
}


// A friend's line, said `delay` seconds from now (one at a time, with a rest between)
void say(Sim& s, int what, float delay) {
    if (s.say || s.voice_rest > 0) return;
    s.say = what, s.say_in = delay;
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
            if (b.foe) {   // an enemy's: the player's ship, a ball about it, or a friend
                Vec d = p + s.pos * -1;
                if (!s.over && dot(d, d) <= 5.0f * 5.0f) {
                    b.life = 0;
                    s.shield -= HIT;
                    s.unhurt = 0;
                    s.hitter = b.owner, s.hit_time = s.time;
                    s.threat = b.owner, s.threat_hold = 0;   // whoever hits is the one to worry about, at once
                    s.sounds |= DOGFIGHT_SOUND_HURT;
                    // (the cockpit is thrown about)
                    s.sway[0] += rnd(-0.02f, 0.02f), s.sway[2] += rnd(-0.05f, 0.05f), s.sway[5] -= 0.012f;
                    break;
                }
            }
            for (size_t k = 0; k < s.ships.size(); k++) {
                Ship& ship = s.ships[k];
                // Each side's pilots hit the other's fighters alone; the player's shots strike anything
                if (b.foe ? !friendly(ship) : b.owner && !foe(ship)) continue;
                // (far ones are skipped by a ball about them first)
                Vec d = p + ship.pos * -1;
                float reach = ship.shape->radius * 1.7f;
                if (dot(d, d) > reach * reach || !inside(ship, p)) continue;
                b.life = 0;
                const bool mine = !b.foe && !b.owner;   // the player's own
                Vec away = ship.pos + s.pos * -1;
                const bool heard = mine || dot(away, away) < 1200.0f * 1200.0f;
                const bool harmed = ship.shape->hits && (!mine || foe(ship));
                if (harmed) ship.shield -= HIT, ship.unhurt = 0;
                if (harmed && ship.shield <= 0.001f) {
                    blast(s, ship.pos, ship.shape->radius * 2.2f);
                    if (ship.id == s.target) s.target = 0;
                    // (a friend shot down the one that was after the player)
                    if (!mine && foe(ship) && ship.id == s.threat) s.sounds |= DOGFIGHT_SOUND_SAVED;
                    if (mine) {
                        // It was on a friend's tail: thanks from the friend. Or
                        // the second kill in a few seconds, or now and then: a cheer
                        bool saved = false;
                        for (const Ship& other : s.ships) {
                            Vec gap = other.pos + ship.pos * -1;
                            saved |= other.id == ship.ai.prey && friendly(other) && (ship.ai.aimed > 0 || dot(gap, gap) < 450.0f * 450.0f);
                        }
                        if (saved) say(s, DOGFIGHT_VOICE_THANKS, 0.8f);
                        else if (s.time - s.last_kill < 8 || rnd(0, 1) < 0.3f) say(s, DOGFIGHT_VOICE_CHEER, 0.6f);
                        s.last_kill = s.time;
                    } else if (foe(ship) && ship.id == s.threat) {
                        s.message = DOGFIGHT_SOUND_SAVED, s.message_left = 2.5f;
                    }
                    s.ships.erase(s.ships.begin() + long(k));
                    if (mine) s.kills++;
                    if (heard) s.sounds |= DOGFIGHT_SOUND_KILL;
                } else {
                    blast(s, p + unit(b.vel) * -1.5f, ship.shape == &DESTROYER ? 6.0f : 2.5f);
                    if (heard) s.sounds |= harmed ? DOGFIGHT_SOUND_DAMAGE : DOGFIGHT_SOUND_HIT;
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
    if (best) s.target = best->id, s.sounds |= DOGFIGHT_SOUND_LOCK;
}

// The enemy fighters fly by the rules the player does (a speed, a turn rate,
// no more) and each has a pilot: it goes for the player's tail, fires short
// bursts when it has the player in its sights (aiming ahead of it, with a
// spread: wider the more the player manoeuvres, and for the worse pilots),
// breaks away when it comes too near, and jinks when the player sits on its
// own tail. `manoeuvre`: how hard the player is turning, 0..1
void pilots(Sim& s, float manoeuvre, float dt) {
    // Who attacks the player: the nearest few enemies
    std::vector<std::pair<float, Ship*>> foes;
    int friends = 0;
    for (Ship& ship : s.ships) {
        friends += ship.shape == &XWING || ship.shape == &YWING;
        ship.ai.chased = false;
        if (!foe(ship)) continue;
        Vec d = ship.pos + s.pos * -1;
        // (one already attacking counts as nearer: they do not swap at every turn)
        foes.push_back({std::sqrt(dot(d, d)) * (ship.ai.engaged ? 0.7f : 1.0f), &ship});
    }
    std::sort(foes.begin(), foes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // (one while the sides are even: the player is one of several to go for;
    // more as the friends fall, and none once the player has)
    const int attackers = s.over ? 0 : std::clamp(int(foes.size()) - friends, 1, ATTACKERS);
    for (size_t i = 0; i < foes.size(); i++) {
        foes[i].second->ai.engaged = i < size_t(attackers);
        foes[i].second->ai.passer = i == 1;
    }
    auto find = [&](uint32_t id) -> Ship* {
        for (Ship& ship : s.ships)
            if (id && ship.id == id) return &ship;
        return nullptr;
    };
    // Whom each of the others goes for: an enemy that is not on the player,
    // the nearest friend; a friend, the nearest enemy (fewer on the same
    // one), and the first of them the one that is after the player. They
    // think again every few seconds
    bool wingman = true;
    for (Ship& ship : s.ships) {
        Ship::Pilot& ai = ship.ai;
        const bool enemy = foe(ship);
        if (!enemy && !friendly(ship)) continue;
        ai.bored -= dt;
        Ship* prey = find(ai.prey);
        if (enemy && ai.engaged) {
            ai.prey = 0;
            continue;
        }
        if (!enemy && wingman && ship.shape == &XWING) {
            wingman = false;
            if (Ship* threat = find(s.threat)) {
                ai.prey = threat->id;
                continue;
            }
        }
        if (prey && ai.bored > 0) continue;
        ai.bored = rnd(4, 8);
        float best = 1e30f;
        ai.prey = 0;
        for (Ship& other : s.ships) {
            if (enemy ? !(other.shape == &XWING || other.shape == &YWING) : !foe(other)) continue;
            Vec d = other.pos + ship.pos * -1;
            float far = dot(d, d);
            for (const Ship& rival : s.ships)
                if (&rival != &ship && rival.ai.prey == other.id && foe(rival) == enemy) far *= 3;   // (taken: farther, as it were)
            if (far < best) best = far, ai.prey = other.id;
        }
    }
    // Who is in someone's sights (it will not sit still for that)
    for (Ship& ship : s.ships)
        if (ship.ai.aimed > 0.3f)
            if (Ship* prey = find(ship.ai.prey)) prey->ai.chased = true;
    // A friend that has had an enemy on it for a while calls for help
    s.help_rest -= dt;
    for (Ship& ship : s.ships) {
        ship.ai.harried = ship.ai.chased ? ship.ai.harried + dt : std::max(0.0f, ship.ai.harried - 0.5f * dt);
        if ((ship.shape == &XWING || ship.shape == &YWING) && ship.ai.harried > 2.5f && s.help_rest <= 0 && !s.say && s.voice_rest <= 0) {
            say(s, DOGFIGHT_VOICE_HELP, 0);
            s.help_rest = 25;
        }
    }
    // A hard turn held a moment shakes off the one on the tail, if its pilot is taken in
    s.hard = manoeuvre >= 0.85f ? s.hard + dt : 0;
    const bool shake = s.hard >= (s.drifting ? 0.25f : 0.6f) && !(s.shaken) ;
    s.shaken = s.hard > 0 ? s.shaken || shake : false;
    s.warning = false;
    for (Ship& ship : s.ships) {
        const bool enemy = foe(ship);
        if (!enemy && !friendly(ship)) continue;
        Ship::Pilot& ai = ship.ai;
        // What it goes for: the player, or another ship; none: an enemy
        // circles the player, a friend flies beside the player
        const Ship* prey = find(ai.prey);
        const bool on_player = enemy && ai.engaged;
        const bool hunting = on_player || prey;
        Vec quarry = s.pos, quarry_fwd = s.at.fwd;
        float quarry_speed = s.speed;
        if (prey) quarry = prey->pos, quarry_fwd = prey->at.fwd, quarry_speed = prey->speed;
        Vec to = quarry + ship.pos * -1;
        float dist = std::max(std::sqrt(dot(to, to)), 1e-3f);
        Vec dir = to * (1 / dist);
        // Where a bolt fired now would meet it
        Vec lead = quarry + quarry_fwd * (quarry_speed * dist / TIE_BOLT);
        // Each kind's best: the X-wing as the player's, the Y-wing heavy, the Falcon between
        const float top = enemy ? TIE_MAX : ship.shape == &YWING ? 115.0f : 145.0f;
        const float rate = enemy ? TIE_TURN : ship.shape == &YWING ? 0.8f : ship.shape == &FALCON ? 0.95f : 1.1f;
        Vec want = dir;
        float want_speed = top;
        bool fire = false;

        ai.timer -= dt, ai.fooled -= dt;
        if (ai.mode != Ship::Pilot::CHASE && ai.timer <= 0) ai.mode = Ship::Pilot::CHASE;
        auto aside = [&](float ahead) {
            Vec side = cross(dir, ship.at.down) * (rnd(-1, 1) < 0 ? -1.0f : 1.0f) + ship.at.down * rnd(-0.6f, 0.6f);
            return unit(ship.at.fwd * ahead + side);
        };
        if (shake && on_player && ai.mode == Ship::Pilot::CHASE && ai.fooled <= 0 && dist < 450 && dot(dir, s.at.fwd) > 0.5f) {
            ai.fooled = 6;
            if (rnd(0, 1) < 0.9f - 0.55f * ai.skill) {   // it flies on past the turn, and has to come round again
                ai.mode = Ship::Pilot::OVERSHOOT, ai.timer = rnd(2.5f, 3.5f), ai.way = ship.at.fwd;
                ai.aimed = 0, ai.burst = 0;
            } else {   // not taken in: it breaks to a side instead of flying past
                ai.mode = Ship::Pilot::BREAK, ai.timer = rnd(1.5f, 2.5f), ai.way = aside(0.5f);
            }
        }
        // The one whose line of fire it should get out of: the player, behind
        // an enemy, pointing at it, near; or whoever has it in their sights
        Vec danger = dir * -1;
        bool hunted = ai.chased;
        if (enemy) {
            Vec from = ship.pos + s.pos * -1;
            float far = std::max(std::sqrt(dot(from, from)), 1e-3f);
            if (dot(s.at.fwd, from) > 0.985f * far && far < 700 && dot(ship.at.fwd, from) > -0.3f * far) hunted = true, danger = from * (1 / far);
        }
        if (ai.mode == Ship::Pilot::CHASE) {
            ai.dread = hunted ? ai.dread + dt : 0;
            if (ai.dread > 0.9f - 0.5f * ai.skill) {
                ai.mode = Ship::Pilot::EVADE, ai.timer = rnd(1.5f, 3.0f), ai.jink = 0, ai.dread = 0;
            } else if (hunting && dist < (ai.passer && on_player ? 150 : 80)) {
                // Too near (or its pass made): away to a side, and round again
                ai.mode = Ship::Pilot::BREAK, ai.timer = ai.passer && on_player ? rnd(4.0f, 6.0f) : rnd(2.0f, 3.5f), ai.way = aside(0.6f);
            }
        }
        if (ai.mode == Ship::Pilot::BREAK || ai.mode == Ship::Pilot::OVERSHOOT) {
            want = ai.way;
        } else if (ai.mode == Ship::Pilot::EVADE) {
            ai.jink -= dt;
            if (ai.jink <= 0) {   // a new way across the line of fire
                ai.jink = rnd(0.5f, 1.1f);
                Vec r{rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)};
                ai.way = unit(r + danger * -dot(r, danger) + ship.at.fwd * 0.3f);
            }
            want = ai.way;
        } else if (hunting) {
            // How far round its tail it is: 1 right behind it
            float behind = std::clamp(dot(dir, quarry_fwd), 0.0f, 1.0f);
            Vec tail = quarry + quarry_fwd * -150.0f;
            float w = ai.passer && on_player ? 1.0f : std::clamp((behind - 0.3f) / 0.4f, 0.0f, 1.0f);   // (the one that makes passes comes straight in)
            want = unit(tail + (lead + tail * -1) * w + ship.pos * -1);
            // Fast to get there; behind it, its speed and a little more while far
            if (dist < 500) want_speed = behind > 0.6f ? quarry_speed + (dist - 200) * 0.3f : top * 0.8f;
            bool sighted = dot(ship.at.fwd, unit(lead + ship.pos * -1)) > 0.9976f && dist < 800;   // 4 degrees
            ai.aimed = sighted ? ai.aimed + dt : 0;
            fire = ai.aimed > 0.9f - 0.5f * ai.skill;
            if (on_player) s.warning |= sighted && ai.aimed > 0.3f;
        } else if (enemy) {
            // Waiting its turn: round the player at a distance, each on its own side
            float a = float(ship.id) * 2.4f + float(s.time) * 0.05f;
            Vec at = s.pos + Vec{std::cos(a) * 800, std::sin(float(ship.id)) * 250, std::sin(a) * 800};
            want = unit(at + ship.pos * -1);
            want_speed = top * 0.7f;
            ai.aimed = 0;
        } else {
            // No enemy to go for: beside the player, each in its own place
            float side = (ship.id & 1 ? 1.0f : -1.0f) * (40.0f + 25.0f * float(ship.id % 3));
            Vec at = s.pos + s.at.right * side + s.at.down * (8.0f * float(ship.id % 4) - 12.0f) + s.at.fwd * (80.0f + s.speed * 1.5f);
            Vec gap = at + ship.pos * -1;
            float far = std::sqrt(dot(gap, gap));
            want = unit(unit(gap) * std::min(1.0f, far / 150.0f) + s.at.fwd * std::max(0.0f, 1 - far / 150.0f) + s.at.fwd * 0.2f);
            want_speed = s.speed + std::clamp(dot(gap, s.at.fwd) * 0.4f, -40.0f, 60.0f);
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
        const float turn = rate * (0.75f + 0.25f * ai.skill) * dt;
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
        ship.speed += (std::clamp(want_speed, TIE_MIN, top) - ship.speed) * std::min(1.0f, 1.2f * dt);
        ship.pos = ship.pos + nose * (ship.speed * dt);

        // Its cannons: a burst, then a pause
        ai.reload -= dt, ai.pause -= dt;
        if (fire && ai.burst == 0 && ai.pause <= 0) ai.burst = 3 + int(rnd(0, 2.99f));
        if (ai.burst > 0 && ai.reload <= 0) {
            ai.reload = TIE_RELOAD;
            if (--ai.burst == 0) {
                ai.pause = rnd(2.0f, 4.0f) - ai.skill;
                // Its burst fired, it swings away and has to line up again
                if (ai.mode == Ship::Pilot::CHASE) ai.mode = Ship::Pilot::BREAK, ai.timer = rnd(2.5f, 4.0f) - ai.skill, ai.way = aside(0.5f);
            }
            float spread = (0.040f + 0.060f * (on_player ? manoeuvre : 0.3f)) * (1.5f - ai.skill) * (enemy ? 1.0f : FRIEND_SPREAD);
            Vec from = ship.pos + nose * (ship.shape->radius + 1.5f);
            Vec aim = unit(unit(lead + from * -1) + Vec{rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)} * spread);
            s.bolts.push_back({from, aim * TIE_BOLT, 2.0f, 0, s.next_id++, enemy, ship.id});
            Vec away = ship.pos + s.pos * -1;
            if (enemy && (on_player || dot(away, away) < 900.0f * 900.0f)) s.sounds |= DOGFIGHT_SOUND_TIE;
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
}

void step(float dt) {
    const DogfightControls& c = g_dogfight_controls;
    Sim& s = g_sim;
    if (c.absolute) s.throttle = std::clamp(c.throttle.load(), 0.0f, 1.0f);
    else s.throttle = std::clamp(s.throttle + c.throttle * 0.6f * dt, 0.0f, 1.0f);
    float target = SPEED_MIN + (SPEED_MAX - SPEED_MIN) * s.throttle;
    // The braking turn: held, with charge left (and not begun on an empty one)
    // (run out, it is over until the button is let go: the charge comes back then)
    if (!c.drift) s.spent = false;
    s.drifting = !s.over && c.drift && !s.spent && s.charge > (s.drifting ? 0.0f : 0.15f);
    if (s.over == DOGFIGHT_DEFEAT) target = SPEED_MIN;   // shot down: the engines are gone
    if (s.drifting) {
        s.charge = std::max(0.0f, s.charge - dt / 2.5f);
        s.spent = s.charge <= 0;
        s.rested = 0;
        s.drifted += dt;
        target = SPEED_MIN;
    } else {
        // Out of one that lasted: the engines kick the ship back up to its
        // speed, and a little over it, for a moment
        if (s.drifted > 0.4f) s.kick = 1.2f;
        s.drifted = 0;
        s.kick = std::max(0.0f, s.kick - dt);
        if (s.kick > 0) target = std::min(target + 35, SPEED_MAX * 1.12f);
        s.rested += dt;
        if (s.rested > 1 && !c.drift) s.charge = std::min(1.0f, s.charge + dt / 6.0f);
    }
    s.push = std::clamp((target - s.speed) / (SPEED_MAX - SPEED_MIN) * 2.5f, -1.0f, 1.0f);
    if (s.kick > 0) s.push = std::max(s.push, s.kick / 1.2f);   // (heard and seen as full thrust, fading)
    s.surge += (s.push - s.surge) * std::min(1.0f, 4.0f * dt);
    s.speed += (target - s.speed) * std::min(1.0f, (s.drifting ? 3.0f : s.kick > 0 ? 4.0f : 1.5f) * dt);
    // Turns best at half throttle, a third slower flat out or crawling; near twice as tight in a braking turn
    float agile = 1.0f - 0.33f * std::fabs(s.throttle - 0.5f) * 2.0f;
    if (s.drifting) agile = 1.8f;
    if (s.over == DOGFIGHT_DEFEAT) s.at.turn(0.05f * dt, 0, 0.9f * dt);   // ... and the ship rolls on, out of hand
    else s.at.turn(c.pitch * TURN_RATE * agile * dt, c.yaw * TURN_RATE * agile * dt, c.roll * ROLL_RATE * dt);
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
    if (c.lock && !s.lock_was && !s.over) lock(s);
    s.lock_was = c.lock;
    if (c.lock_threat && !s.threat_was && s.threat) s.target = s.threat, s.sounds |= DOGFIGHT_SOUND_LOCK;   // the lock on the one attacking
    s.threat_was = c.lock_threat;
    // (a braking turn counts as the hardest of manoeuvres)
    if (g_ai) pilots(s, std::clamp((std::fabs(c.pitch.load()) + std::fabs(c.yaw.load())) * (s.drifting ? 2.0f : 1.0f), 0.0f, 1.0f), dt);
    cannons(s, c.fire && s.over != DOGFIGHT_DEFEAT, dt);
    // The friends' lines, when their moment comes
    s.voice_rest -= dt, s.message_left -= dt;
    if (s.say && (s.say_in -= dt) <= 0) {
        s.sounds |= s.say;
        s.message = s.say, s.message_left = 3;
        s.say = 0, s.voice_rest = 5;
    }
    // The shields come back when nothing has hit for a while, the slower the
    // fewer ships are left (not at all with four or fewer, so the last ones
    // fall to the hits they take) and the longer the match has lasted
    int friends = 1, foes = 0;
    for (const Ship& ship : s.ships) friends += ship.shape == &XWING, foes += foe(ship);
    if (s.over == DOGFIGHT_DEFEAT) friends--;
    s.regen = std::clamp(float(friends + foes - 4) / float(std::max(1, s.total - 4)), 0.0f, 1.0f) * std::clamp(1 - (s.took - WANE_FROM) / WANE_FOR, 0.0f, 1.0f);
    s.unhurt += dt;
    if (s.unhurt > SHIELD_WAIT) s.shield = std::min(1.0f, s.shield + SHIELD_BACK * s.regen * dt);
    for (Ship& ship : s.ships) {
        ship.unhurt += dt;
        if (ship.unhurt > SHIELD_WAIT) ship.shield = std::min(1.0f, ship.shield + SHIELD_BACK * s.regen * dt);
    }
    // The match is over when the player falls or no enemy is left; the next
    // begins after a moment
    if (!s.over) {
        s.took += dt;
        if (s.shield <= 0.001f) {
            s.over = DOGFIGHT_DEFEAT, s.over_left = 6;
            s.shield = 0, s.target = 0, s.threat = 0, s.warning = false;
            blast(s, s.pos + s.at.fwd * 14.0f, 9.0f);
            s.sounds |= DOGFIGHT_SOUND_KILL;
            s.sway[0] += 0.05f, s.sway[2] += 0.12f, s.sway[5] -= 0.03f;
        } else if (g_ai && foes == 0) {
            s.over = DOGFIGHT_VICTORY, s.over_left = 8;
            s.say = 0, s.voice_rest = 0;
            say(s, DOGFIGHT_VOICE_CHEER, 1.0f);
        }
    } else if ((s.over_left -= dt) <= 0) {
        reset();
    }
    for (Blast& b : s.blasts) b.frame += 45.0f * dt;   // the fireball: a second
    s.blasts.erase(std::remove_if(s.blasts.begin(), s.blasts.end(), [](const Blast& b) { return b.frame > 46.0f; }), s.blasts.end());
    g_shown_ships = int(s.ships.size()), g_shown_kills = s.kills;
    g_shown_speed = s.speed, g_shown_throttle = s.throttle;
    g_shown_locked = s.target != 0;
    g_shown_message = s.message_left > 0 ? s.message : 0;
    g_shown_push = s.push, g_shown_charge = s.charge;
    g_shown_shield = std::max(0.0f, s.shield), g_shown_warning = s.warning;
    g_shown_result = s.over, g_shown_friends = friends, g_shown_foes = foes, g_shown_took = s.took;
    float target_shield = -1;
    for (const Ship& ship : s.ships)
        if (s.target && ship.id == s.target) target_shield = std::max(0.0f, ship.shield);
    g_shown_target_shield = target_shield;
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
    g_seed = 0x5EED1234;   // (the first match always the same; the ones after it as they fall)
    reset();
}
void dogfight_step(float dt) { step(dt); }
float dogfight_speed() { return g_shown_speed; }
float dogfight_throttle() { return g_shown_throttle; }
float dogfight_speed_part() { return std::clamp((g_shown_speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN), 0.0f, 1.0f); }
float dogfight_push() { return g_shown_push; }
int dogfight_message() { return g_shown_message; }
float dogfight_charge() { return g_shown_charge; }
int dogfight_ships() { return g_shown_ships; }
int dogfight_kills() { return g_shown_kills; }
bool dogfight_locked() { return g_shown_locked; }
float dogfight_shield() { return g_shown_shield; }
bool dogfight_warning() { return g_shown_warning; }
int dogfight_result() { return g_shown_result; }
int dogfight_left(bool foes) { return foes ? g_shown_foes : g_shown_friends; }
float dogfight_target_shield() { return g_shown_target_shield; }
float dogfight_took() { return g_shown_took; }
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
        // Its shield over the brackets: four pips, the ones it has left lit
        const int left = int(std::ceil(ship->shield * 4 - 0.01f));
        Vec mid = Vec{right, up, -ahead} * D + Vec{0, bracket + D * 0.005f, 0};
        const float pip = bracket * 2 / 4;
        for (int k = 0; k < 4; k++) {
            float x0 = mid.x - bracket + pip * float(k) + pip * 0.12f, x1 = x0 + pip * 0.76f;
            bool lit = k < left;
            line(Vec{x0, mid.y, mid.z}, Vec{front ? x1 : x0, mid.y, mid.z}, !front ? 0 : lit ? D * 0.0032f : D * 0.0009f, lit ? YELLOW : 0xFF2090A0u);
        }
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
        // The view opens a little while the engines push and closes while they hold back
        if (scene->fov > 0) scene->fov += 4.0f * s.surge;
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
    // Dust of space going by: a mote in each cell of a lattice round the ship
    // (the same ones wherever it is seen from), each a faint streak as long
    // as the ship's speed makes it. It tells how fast the ship goes
    {
        const float cell = 45;
        const int reach = 3;
        const int cx = int(std::floor(cam.x / cell)), cy = int(std::floor(cam.y / cell)), cz = int(std::floor(cam.z / cell));
        const Vec trail = s.at.fwd * (s.speed * 0.035f);
        const float glow = 0.35f + 0.65f * std::clamp((s.speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN), 0.0f, 1.0f);
        for (int x = cx - reach; x <= cx + reach; x++)
            for (int y = cy - reach; y <= cy + reach; y++)
                for (int z = cz - reach; z <= cz + reach; z++) {
                    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u;
                    h ^= h >> 13, h *= 0x5BD1E995u, h ^= h >> 15;
                    Vec p{(float(x) + float(h & 255) / 256.0f) * cell, (float(y) + float(h >> 8 & 255) / 256.0f) * cell, (float(z) + float(h >> 16 & 255) / 256.0f) * cell};
                    Vec d = p + cam * -1;
                    float far = std::sqrt(dot(d, d)) / (cell * float(reach));
                    if (far >= 1 || far < 0.04f) continue;   // (fading out at the lattice's edge; none in the cockpit)
                    R3DNativeScene::Beam beam;
                    Vec tail = p + trail;
                    beam.a[0] = tail.x, beam.a[1] = tail.y, beam.a[2] = tail.z;
                    beam.b[0] = p.x, beam.b[1] = p.y, beam.b[2] = p.z;
                    beam.width = 0.05f, beam.least = 0.0008f;
                    beam.plain = true;
                    uint32_t grey = uint32_t(150.0f * glow * (1 - far) * (1 - far));
                    beam.color = 0xFF000000u | grey | grey << 8 | std::min(255u, grey + grey / 6) << 16;
                    beam.key = 0x800000u | (h & 0x7FFFFFu);
                    scene->beams.push_back(beam);
                }
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
