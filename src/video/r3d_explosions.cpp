#include "r3d_explosions.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../audio/explosion_sound.h"
#include "r3d_screen.h"

namespace {

std::atomic<bool> g_enhanced{false};

constexpr float FRAME_S = 1.0f / 57.524f;   // one game frame
constexpr int FIRST_FRAME = 2, LAST_FRAME = 46;   // the fireball's frames of the flipbook (0, 1: sparkles; 47: another sprite)
constexpr float QUAD = 1.4f;   // the fireball's quad, in sprite radii (room for its billows)
constexpr size_t MAX_TRACKS = 24;
constexpr size_t MAX_PARTICLES = 240;   // per explosion

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a) {
    float l = length(a);
    return l > 1e-12f ? a * (1.0f / l) : V3{0, 0, 1};
}
V3 load(const float* p) { return {p[0], p[1], p[2]}; }
void store(float* p, V3 a) { p[0] = a.x, p[1] = a.y, p[2] = a.z; }

uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

struct Rng {
    uint32_t s;
    float f() { return float((s = hash32(s + 0x9E3779B9u)) >> 8) * (1.0f / 16777216.0f); }
    float r(float a, float b) { return a + (b - a) * f(); }
    V3 dir() {   // uniform over the sphere
        float z = r(-1, 1), a = r(0, 6.2831853f), s = std::sqrt(std::max(0.0f, 1 - z * z));
        return {s * std::cos(a), s * std::sin(a), z};
    }
};

// The orientation of space in view space (the star field's axes): particles
// live in it, so they keep their place in the world when the camera turns
struct Frame {
    V3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    V3 view(V3 l) const { return x * l.x + y * l.y + z * l.z; }
    V3 local(V3 v) const { return {dot(v, x), dot(v, y), dot(v, z)}; }
};

// Turns v about the unit axis a by `angle` (Rodrigues)
V3 rotate(V3 v, V3 a, float angle) {
    float c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(a, v) * s + a * (dot(a, v) * (1 - c));
}

enum Kind : uint32_t { FIREBALL = 0, SPARK = 1, SMOKE = 2, DEBRIS = 3 };

struct Particle {
    explicit Particle(Kind k) : kind(k) {}
    Kind kind;
    bool ember = false;   // a burning piece: a spark that leaves smoke behind
    V3 p, v;              // relative to the explosion, space's orientation; units per second
    float age = 0, life = 1, size = 1, grow = 0, drag = 0;
    float bright = 1;
    uint32_t color = 0xFFFFFFFFu;
    uint32_t seed = 0;
    V3 axis{0, 0, 1};     // debris: tumbling
    float spin = 0, angle = 0;
    float puff = 0;       // ember: time to its next puff of smoke
    V3 prev_view;
    bool has_prev = false;
};

// A piece of the ship that blew up: its own polygons (textures and all),
// flung out and tumbling, charred, trailing smoke
struct Chunk {
    std::vector<R3DVertex> verts;   // pos and nrm about its centre, in space's orientation
    V3 p, v;                        // centre relative to the explosion; units per second
    V3 axis{0, 0, 1};
    float spin = 0, angle = 0, age = 0, life = 3, puff = 0, size = 1;
    std::vector<V3> prev_view;      // where its vertices were drawn last frame
};

// A ship as drawn in a recent frame, to break up if it vanishes into an explosion
struct Ship {
    uint32_t key = 0;
    V3 origin;                      // space's orientation (view position turned)
    V3 axes[3];                     // its model axes, the same way
    float radius = 0;
    std::vector<R3DVertex> verts;   // its opaque triangles, pos and nrm about the origin, space's orientation
};

struct Track {
    uint32_t id = 0, seed = 0;
    V3 anchor, vel;       // centre in space's orientation, and its motion per game frame
    float radius = 1;     // the sprite's
    float fog = 0;
    int frame = 0;
    int unseen = 0;       // game frames since the sprite was last drawn
    bool matched = false;
    bool cloud = false;   // the smoke left when the fireball has burnt out
    uint32_t build_index = 0;   // the viewport it is drawn in (R3DViewport::build_index)
    std::vector<Particle> parts;
    std::vector<Chunk> chunks;
    bool broken = false;   // its ship was found and broken into chunks
    int age = 0;           // game frames since it appeared
    // Its look, from its seed, so no two are quite alike
    enum Style { ROUND, STRETCHED, CLUSTER } style = ROUND;
    V3 axis{0, 0, 1};      // space's orientation: the way a stretched one is drawn out, and its sparks fly
    float stretch = 1;     // the fireball's length over its width
    float scale = 1;       // the fireball's size over the sprite's
    uint32_t tint = 0xFFFFFFFFu;   // the fire's colour (white = as drawn)
    float sparks = 1, speed = 1, smoke = 1;
};

struct Found {
    V3 c, prev;
    float radius;
    int frame;
    float fog;
    uint32_t viewport;   // index in out.viewports
    std::vector<uint32_t> trans, opaque;   // first vertex of each of its triangles
    uint32_t track_id = 0;
};

std::vector<Track> g_tracks;
std::vector<std::vector<Ship>> g_ships;   // the last frames' ships, newest first
uint64_t g_last_frame = 0;
uint32_t g_next_id = 1;

// The flipbook's frame, if the texture word is one of the explosion's
bool explosion_frame(uint32_t tex, int& frame) {
    if ((tex >> 28) != 7 || ((tex >> 22) & 7) != 1 || ((tex >> 25) & 7) != 1) return false;   // ARGB4444, 64x64
    uint32_t x = tex & 2047, y = (tex >> 11) & 2047;
    if (y < 576 || y >= 768 || x >= 1024 || (x & 63) || (y & 63)) return false;
    frame = int((y - 576) / 64 * 16 + x / 64);
    return frame >= FIRST_FRAME && frame <= LAST_FRAME;
}

uint32_t rgba(float r, float g, float b, float a = 1) {
    auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(r) | c(g) << 8 | c(b) << 16 | c(a) << 24;
}

// Its look: round (as the sprite), drawn out along an axis (its sparks
// thrown mostly that way), or a cluster of a few smaller blasts
void pick_style(Track& t, Rng& rng) {
    float pick = rng.f();
    t.style = pick < 0.4f ? Track::ROUND : pick < 0.75f ? Track::STRETCHED : Track::CLUSTER;
    t.axis = rng.dir();
    t.stretch = t.style == Track::STRETCHED ? rng.r(1.5f, 2.1f) : rng.r(1.0f, 1.15f);
    t.scale = t.style == Track::CLUSTER ? rng.r(0.8f, 0.95f) : rng.r(0.9f, 1.15f);
    t.sparks = rng.r(0.6f, 1.5f);
    t.speed = rng.r(0.8f, 1.3f);
    t.smoke = rng.r(0.6f, 1.4f);
    float warm = rng.f();   // 0 as drawn .. 1 a deeper orange
    warm *= warm;
    t.tint = rgba(1.0f, 1.0f - 0.12f * warm, 1.0f - 0.3f * warm);
}

// A direction to throw something: along the axis for a stretched one
V3 throw_dir(const Track& t, Rng& rng) {
    V3 d = rng.dir();
    if (t.style != Track::STRETCHED) return d;
    return normalize(d + t.axis * ((rng.f() < 0.5f ? -1.0f : 1.0f) * 1.6f));
}

void burst(Track& t, Rng& rng) {
    float R = t.radius;
    // Sparks: fast thin streaks, white-hot to yellow
    int n_sparks = int(28 * t.sparks);
    for (int i = 0; i < n_sparks; i++) {
        Particle p(SPARK);
        p.v = throw_dir(t, rng) * (R * rng.r(1.6f, 4.2f) * t.speed);
        p.p = p.v * 0.02f;
        p.life = rng.r(0.25f, 0.6f);
        p.size = R * rng.r(0.012f, 0.022f);
        p.drag = 1.6f;
        p.bright = rng.r(1.5f, 3.0f);
        p.color = rgba(1.0f, rng.r(0.75f, 0.92f), rng.r(0.45f, 0.7f));
        p.seed = hash32(t.seed + i);
        t.parts.push_back(p);
    }
    // Burning pieces, trailing smoke
    for (int i = 0; i < 6; i++) {
        Particle p(SPARK);
        p.ember = true;
        p.v = throw_dir(t, rng) * (R * rng.r(0.9f, 2.0f) * t.speed);
        p.life = rng.r(1.0f, 1.7f);
        p.size = R * rng.r(0.025f, 0.04f);
        p.drag = 0.5f;
        p.bright = rng.r(1.4f, 2.0f);
        p.color = rgba(1.0f, rng.r(0.55f, 0.7f), 0.25f);
        p.seed = hash32(t.seed + 100 + i);
        p.puff = rng.r(0.0f, 0.05f);
        t.parts.push_back(p);
    }
    // A cluster: a few smaller blasts about the first, a moment after it
    if (t.style == Track::CLUSTER) {
        int n = 2 + int(rng.f() * 2.0f);
        for (int i = 0; i < n; i++) {
            Particle p(FIREBALL);
            p.p = rng.dir() * (R * rng.r(0.5f, 0.85f));
            p.v = p.p * rng.r(0.3f, 0.8f);
            p.drag = 1.0f;
            p.age = -rng.r(0.04f, 0.22f);   // not drawn until it is >= 0
            p.life = rng.r(0.55f, 0.8f);
            p.size = R * rng.r(0.4f, 0.6f);
            p.color = t.tint;
            p.seed = hash32(t.seed + 400 + i);
            t.parts.push_back(p);
        }
    }
    // Pieces of hull, tumbling, glowing hot at first
    int n_debris = 5 + int(rng.f() * 6.0f);
    for (int i = 0; i < n_debris; i++) {
        Particle p(DEBRIS);
        p.v = rng.dir() * (R * rng.r(0.6f, 1.9f));
        p.life = rng.r(1.2f, 2.2f);
        p.size = R * rng.r(0.025f, 0.06f);
        p.drag = 0.15f;
        p.axis = rng.dir();
        p.spin = rng.r(4.0f, 14.0f) * (rng.f() < 0.5f ? -1.0f : 1.0f);
        p.angle = rng.r(0, 6.2831853f);
        float grey = rng.r(0.28f, 0.42f);
        p.color = rgba(grey, grey * 1.01f, grey * 1.06f);
        p.seed = hash32(t.seed + 200 + i);
        t.parts.push_back(p);
    }
}

// The smoke left where the fireball was
void smoke_cloud(Track& t, Rng& rng) {
    float R = t.radius;
    int n = int(8 * t.smoke + 0.5f);
    for (int i = 0; i < n; i++) {
        Particle p(SMOKE);
        p.p = throw_dir(t, rng) * (R * rng.r(0.1f, 0.5f) * t.stretch);
        p.v = rng.dir() * (R * rng.r(0.05f, 0.2f));
        p.life = rng.r(1.2f, 2.0f);
        p.size = R * rng.r(0.2f, 0.35f);
        p.grow = 0.5f;
        p.bright = 1;
        float g = rng.r(0.1f, 0.15f);
        p.color = rgba(g, g * 0.97f, g * 0.94f, 0.3f);
        p.seed = hash32(t.seed + 300 + i);
        t.parts.push_back(p);
    }
}

void simulate(Track& t, Rng& rng, float dt) {
    std::vector<Particle> puffs;
    for (Particle& p : t.parts) {
        p.age += dt;
        p.v = p.v * std::exp(-p.drag * dt);
        p.p = p.p + p.v * dt;
        p.angle += p.spin * dt;
        if (p.ember && p.age < p.life * 0.85f && (p.puff -= dt) <= 0) {
            p.puff += 0.06f;
            Particle s(SMOKE);
            s.p = p.p;
            s.v = p.v * 0.08f + rng.dir() * (t.radius * 0.03f);
            s.life = rng.r(0.6f, 1.0f);
            s.size = p.size * rng.r(1.4f, 2.0f);
            s.grow = 2.5f;
            float g = rng.r(0.1f, 0.15f);
            s.color = rgba(g, g * 0.96f, g * 0.92f, 0.3f * (1.0f - p.age / p.life));
            s.seed = hash32(p.seed + uint32_t(p.age * 1000.0f));
            puffs.push_back(s);
        }
    }
    for (Chunk& c : t.chunks) {
        c.age += dt;
        c.v = c.v * std::exp(-0.1f * dt);
        c.p = c.p + c.v * dt;
        c.angle += c.spin * dt;
        if (c.age < c.life * 0.6f && (c.puff -= dt) <= 0) {   // burning: a trail of smoke
            c.puff += 0.045f;
            Particle s(SMOKE);
            s.p = c.p + rng.dir() * (c.size * 0.3f);
            s.v = c.v * 0.05f;
            s.life = rng.r(0.7f, 1.2f);
            s.size = std::min(c.size, t.radius * 0.4f) * rng.r(0.35f, 0.55f);
            s.grow = 1.5f;
            float g = rng.r(0.1f, 0.15f);
            s.color = rgba(g, g * 0.96f, g * 0.92f, 0.35f * (1.0f - c.age / (c.life * 0.6f)));
            s.seed = hash32(uint32_t(c.age * 977.0f) + uint32_t(c.verts.size()) * 31u);
            puffs.push_back(s);
        }
    }
    t.chunks.erase(std::remove_if(t.chunks.begin(), t.chunks.end(), [](const Chunk& c) { return c.age >= c.life; }),
                   t.chunks.end());
    for (const Particle& s : puffs)
        if (t.parts.size() < MAX_PARTICLES) t.parts.push_back(s);
    t.parts.erase(std::remove_if(t.parts.begin(), t.parts.end(), [](const Particle& p) { return p.age >= p.life; }),
                  t.parts.end());
}

R3DVertex vertex(V3 pos, V3 prev, V3 nrm, float u, float v, Kind kind, uint32_t seed, float shade, float spec,
                 float fog, uint32_t color) {
    R3DVertex o{};
    store(o.pos, pos);
    store(o.prev, prev);
    store(o.nrm, nrm);
    o.uv[0] = u, o.uv[1] = v;
    o.color = color;
    o.tex = uint32_t(kind) | (seed & 0xFFFFu) << 8;
    o.flags = R3D_EXPLOSION | R3D_WORLD | R3D_NO_SHADOW;
    o.shade = shade;
    o.fog = fog;
    o.spec = spec;
    o.replace = 0;
    return o;
}

// A quad as two triangles: corners in order round it, their uv
void quad(std::vector<R3DVertex>& out, const R3DVertex* c) {
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i : order) out.push_back(c[i]);
}

void draw(const Track& t, const Frame& f, std::vector<R3DVertex>& out) {
    V3 anchor = f.view(t.anchor);
    for (const Particle& p : t.parts) {
        V3 c = anchor + f.view(p.p);
        V3 delta = p.has_prev ? c - p.prev_view : V3{};
        float age = p.age / p.life;
        if (age < 0) continue;   // a cluster's blast still to come
        R3DVertex q[4];
        if (p.kind == FIREBALL) {   // a cluster's smaller blast
            V3 d = normalize(c * -1.0f);
            V3 r = normalize(cross(V3{0, 1, 0}, d)), u = cross(d, r);
            float s = p.size * QUAD;
            V3 corner[4] = {c - r * s - u * s, c + r * s - u * s, c + r * s + u * s, c - r * s + u * s};
            float uv[4][2] = {{-QUAD, -QUAD}, {QUAD, -QUAD}, {QUAD, QUAD}, {-QUAD, QUAD}};
            for (int k = 0; k < 4; k++)
                q[k] = vertex(corner[k], corner[k] - delta, d, uv[k][0], uv[k][1], FIREBALL, p.seed, age, p.size, t.fog, p.color);
        } else if (p.kind == SPARK) {
            // A streak along its motion, turned to face the eye
            V3 vel = f.view(p.v);
            float streak = p.ember ? 0.03f : 0.05f;
            V3 tail = c - vel * streak;
            V3 side = normalize(cross(c - tail, c));
            if (length(c - tail) < 1e-6f) side = normalize(cross(V3{0, 1, 0}, c));
            // At least about a pixel wide: thinner streaks flicker, and
            // DLAA's history smears them into long rays
            float w = std::max(p.size * (p.ember ? 1.0f : 1.0f - 0.5f * age), -c.z * 0.0025f);
            V3 n = normalize(c * -1.0f);
            V3 corner[4] = {tail - side * w, c - side * w, c + side * w, tail + side * w};
            float uv[4][2] = {{0, -1}, {1, -1}, {1, 1}, {0, 1}};
            for (int k = 0; k < 4; k++)
                q[k] = vertex(corner[k], corner[k] - delta, n, uv[k][0], uv[k][1], SPARK, p.seed, age, p.bright, t.fog, p.color);
        } else if (p.kind == SMOKE) {
            V3 d = normalize(c * -1.0f);
            V3 r = normalize(cross(V3{0, 1, 0}, d)), u = cross(d, r);
            float s = p.size * (1.0f + p.grow * p.age);
            V3 corner[4] = {c - r * s - u * s, c + r * s - u * s, c + r * s + u * s, c - r * s + u * s};
            float uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (int k = 0; k < 4; k++)
                q[k] = vertex(corner[k], corner[k] - delta, d, uv[k][0], uv[k][1], SMOKE, p.seed, age, s, t.fog, p.color);
        } else {   // DEBRIS: a flat, uneven shard turned about its axis
            V3 a = normalize(p.axis);
            V3 e1 = normalize(cross(a, std::fabs(a.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0}));
            V3 e2 = cross(a, e1);
            float cs = std::cos(p.angle), sn = std::sin(p.angle);
            // The shard's plane holds the axis and turns about it
            V3 w1 = e1 * cs + e2 * sn, n = e2 * cs - e1 * sn;
            V3 w2 = a;
            Rng shape{p.seed};
            float s = p.size;
            float k0 = shape.r(0.6f, 1.1f), k1 = shape.r(0.6f, 1.1f), k2 = shape.r(0.6f, 1.1f), k3 = shape.r(0.6f, 1.1f);
            float aspect = shape.r(0.45f, 1.0f);
            V3 lc[4] = {w1 * (-s * k0) + w2 * (-s * aspect * k1), w1 * (s * k1) + w2 * (-s * aspect * k2),
                        w1 * (s * k2) + w2 * (s * aspect * k3), w1 * (-s * k3) + w2 * (s * aspect * k0)};
            V3 nv = normalize(f.view(n));
            float heat = std::max(0.0f, 1.0f - p.age / 0.9f);
            float uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (int k = 0; k < 4; k++) {
                V3 corner = c + f.view(lc[k]);
                q[k] = vertex(corner, corner - delta, nv, uv[k][0], uv[k][1], DEBRIS, p.seed, age, heat, t.fog, p.color);
            }
        }
        quad(out, q);
    }
}

// The ship broken into a few pieces by where its triangles lie in its own
// frame: each wing in an upper and a lower half, the middle in front and back
void break_ship(Track& t, const Ship& ship, Rng& rng) {
    std::vector<std::vector<uint32_t>> parts(6);
    float ext = 0;
    for (const R3DVertex& v : ship.verts) ext = std::max(ext, std::fabs(dot(load(v.pos), ship.axes[0])));
    float side = 0.35f * ext;
    for (uint32_t i = 0; i + 2 < ship.verts.size(); i += 3) {
        V3 c = (load(ship.verts[i].pos) + load(ship.verts[i + 1].pos) + load(ship.verts[i + 2].pos)) * (1.0f / 3.0f);
        float mx = dot(c, ship.axes[0]), my = dot(c, ship.axes[1]), mz = dot(c, ship.axes[2]);
        int k = mx < -side ? (my < 0 ? 0 : 1) : mx > side ? (my < 0 ? 2 : 3) : (mz < 0 ? 4 : 5);
        parts[size_t(k)].push_back(i);
    }
    for (const auto& tris : parts) {
        if (tris.empty()) continue;
        Chunk c;
        V3 centre;
        for (uint32_t i : tris)
            for (int j = 0; j < 3; j++) centre = centre + load(ship.verts[i + j].pos);
        centre = centre * (1.0f / float(tris.size() * 3));
        float size = 0;
        for (uint32_t i : tris)
            for (int j = 0; j < 3; j++) {
                R3DVertex v = ship.verts[i + j];
                V3 p = load(v.pos) - centre;
                store(v.pos, p);
                size = std::max(size, length(p));
                // charred: darker, a little warmer
                uint32_t col = v.color;
                float r = float(col & 0xFF) * 0.55f, g = float((col >> 8) & 0xFF) * 0.5f, b = float((col >> 16) & 0xFF) * 0.45f;
                v.color = (col & 0xFF000000u) | uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16;
                c.verts.push_back(v);
            }
        if (size > 3.0f * t.radius) continue;   // not a fighter's piece
        c.p = centre;
        V3 out = length(centre) > 1e-4f ? normalize(centre) : rng.dir();
        c.v = normalize(out + rng.dir() * 0.35f) * (t.radius * rng.r(0.35f, 0.9f));
        c.axis = rng.dir();
        c.spin = rng.r(1.5f, 5.0f) * (rng.f() < 0.5f ? -1.0f : 1.0f);
        c.life = rng.r(2.2f, 3.2f);
        c.puff = rng.r(0.0f, 0.05f);
        c.size = std::max(size, t.radius * 0.05f);
        t.chunks.push_back(std::move(c));
    }
}

void draw_chunks(Track& t, const Frame& f, std::vector<R3DVertex>& out) {
    V3 anchor = f.view(t.anchor);
    for (Chunk& c : t.chunks) {
        // Shrinks away at the end of its life (out of sight by then, mostly)
        float k = 1.0f - std::clamp((c.age - (c.life - 0.4f)) / 0.4f, 0.0f, 1.0f);
        V3 centre = anchor + f.view(c.p);
        bool prev = c.prev_view.size() == c.verts.size();
        c.prev_view.resize(c.verts.size());
        for (size_t i = 0; i < c.verts.size(); i++) {
            R3DVertex v = c.verts[i];
            V3 p = centre + f.view(rotate(load(v.pos), c.axis, c.angle) * k);
            store(v.pos, p);
            store(v.prev, prev ? c.prev_view[i] : p);
            store(v.nrm, f.view(rotate(load(v.nrm), c.axis, c.angle)));
            c.prev_view[i] = p;
            out.push_back(v);
        }
    }
}

}  // namespace

void r3d_set_swt_explosions(bool enhanced) { g_enhanced = enhanced; }

void r3d_swt_explosions(R3DScene& out, const std::vector<std::vector<R3DInstance>>& instances) {
    // Yavin (top state 3); SWT_EXPLOSIONS_ANYWHERE=1 everywhere the sprite is
    // drawn, replays included (testing)
    static const bool anywhere = getenv("SWT_EXPLOSIONS_ANYWHERE") != nullptr;
    const bool on = g_enhanced && (anywhere || r3d_swt_stage() == 3);
    explosion_sound_enable(on);
    if (!on) {
        g_tracks.clear();
        g_ships.clear();
        g_last_frame = 0;
        return;
    }
    int steps = 1;
    if (g_last_frame) steps = int(std::clamp<int64_t>(int64_t(out.frame) - int64_t(g_last_frame), 0, 8));
    g_last_frame = out.frame;

    Frame f;
    f.x = normalize(load(out.star_axes)), f.y = normalize(load(out.star_axes + 3)), f.z = normalize(load(out.star_axes + 6));

    // The explosions drawn this frame: one per model of the sprite's triangles
    std::vector<Found> found;
    for (uint32_t vi = 0; vi < out.viewports.size(); vi++) {
        const R3DViewport& v = out.viewports[vi];
        if (!(v.world || v.shares_world) || v.build_index >= instances.size()) continue;
        for (const R3DInstance& in : instances[v.build_index]) {
            Found e{};
            e.viewport = vi;
            int frame = -1;
            auto scan = [&](uint32_t first, uint32_t from, uint32_t to, std::vector<uint32_t>& list) {
                for (uint32_t i = from; i + 2 < to; i += 3) {
                    uint32_t k = first + i;
                    if (k + 2 >= out.vertices.size()) return;
                    const R3DVertex& x = out.vertices[k];
                    int fr;
                    if ((x.flags & (R3D_TEXTURED | R3D_LUMINOUS)) != (R3D_TEXTURED | R3D_LUMINOUS) || !explosion_frame(x.tex, fr))
                        continue;
                    frame = std::max(frame, fr);
                    list.push_back(k);
                }
            };
            scan(v.trans_first, in.trans[0], in.trans[1], e.trans);
            if (e.trans.size() < 2) continue;
            scan(v.opaque_first, in.opaque[0], in.opaque[1], e.opaque);
            e.frame = frame;
            V3 c, prev;
            float n = 0;
            for (uint32_t k : e.trans)
                for (int j = 0; j < 3; j++) c = c + load(out.vertices[k + j].pos), prev = prev + load(out.vertices[k + j].prev), n++;
            e.c = c * (1.0f / n), e.prev = prev * (1.0f / n);
            float r = 0;
            for (uint32_t k : e.trans)
                for (int j = 0; j < 3; j++) r = std::max(r, length(load(out.vertices[k + j].pos) - e.c));
            if (!(r > 0) || e.c.z > -1e-3f) continue;
            e.radius = r;
            e.fog = out.vertices[e.trans[0]].fog;
            found.push_back(std::move(e));
        }
    }

    // The ships drawn this frame (fighters: small next to their distance),
    // kept a few frames to break up the one that vanishes into an explosion
    std::vector<Ship> ships;
    std::vector<uint32_t> keys_now;
    for (const R3DViewport& v : out.viewports) {
        if (!(v.shares_world || (v.world && v.space)) || v.build_index >= instances.size()) continue;
        for (const R3DInstance& in : instances[v.build_index]) {
            V3 o = load(in.origin);
            uint32_t from = v.opaque_first + in.opaque[0], to = v.opaque_first + in.opaque[1];
            if (to <= from || to - from > 9000 || to > out.vertices.size() || o.z > -1e-3f) continue;
            int fr;
            if (explosion_frame(out.vertices[from].tex, fr)) continue;
            // Its hull only: the luminous polygons are the laser bolts it has
            // fired (drawn with the ship, long red streaks once tumbling) and
            // the glow of its engines
            float r = 0;
            size_t hull = 0;
            for (uint32_t k = from; k + 2 < to; k += 3) {
                if (out.vertices[k].flags & R3D_LUMINOUS) continue;
                hull++;
                for (int j = 0; j < 3; j++) r = std::max(r, length(load(out.vertices[k + j].pos) - o));
            }
            if (!hull || r > 0.3f * -o.z) continue;   // the Death Star, the capital ships near the eye
            keys_now.push_back(in.key);
            Ship sh;
            sh.key = in.key;
            sh.origin = f.local(o);
            for (int a = 0; a < 3; a++) sh.axes[a] = normalize(f.local(load(in.axes + 3 * a)));
            sh.radius = r;
            sh.verts.reserve(hull * 3);
            for (uint32_t k = from; k + 2 < to; k += 3) {
                if (out.vertices[k].flags & R3D_LUMINOUS) continue;
                for (int j = 0; j < 3; j++) {
                    R3DVertex x = out.vertices[k + j];
                    store(x.pos, f.local(load(x.pos) - o));
                    store(x.nrm, f.local(load(x.nrm)));
                    sh.verts.push_back(x);
                }
            }
            ships.push_back(std::move(sh));
        }
    }
    std::sort(keys_now.begin(), keys_now.end());

    // Each to the explosion it was last frame, or a new one
    for (Track& t : g_tracks) t.matched = false;
    for (Found& e : found) {
        int best = -1;
        float best_d = 1e30f;
        for (int i = 0; i < int(g_tracks.size()); i++) {
            Track& t = g_tracks[i];
            if (t.matched || t.unseen > 3 || e.frame < t.frame - 2) continue;
            float d = length(f.view(t.anchor + t.vel * float(steps)) - e.c);
            if (d < 1.5f * std::max(e.radius, t.radius) && d < best_d) best = i, best_d = d;
        }
        if (best < 0) {
            if (g_tracks.size() >= MAX_TRACKS) g_tracks.erase(g_tracks.begin());
            Track t;
            t.id = g_next_id++;
            t.seed = hash32(t.id * 7919u);
            t.anchor = f.local(e.c);
            t.vel = f.local(e.c - e.prev);
            t.radius = e.radius;
            t.frame = e.frame;
            Rng rng{t.seed};
            pick_style(t, rng);
            if (e.frame <= 16) {   // seen from the start (not an explosion coming into view)
                burst(t, rng);
                // The game's own sound (explosion_sound.h), panned to where it is on the screen
                const float* p = out.viewports[e.viewport].proj;
                float ndc = (p[0] * e.c.x + p[8] * e.c.z + p[12]) / -e.c.z;
                explosion_sound_event(std::clamp(ndc * 0.8f, -0.8f, 0.8f));
                // LW_SNDLOG (with the sound board's out/voices.csv): when each began
                static FILE* log = getenv("LW_SNDLOG") ? fopen("out/explosions.csv", "w") : nullptr;
                if (log) {
                    fprintf(log, "%llu;%.4f;%.1f;%.1f\n", (unsigned long long)out.frame, e.radius / -e.c.z, e.c.x, e.c.z);
                    fflush(log);
                }
            } else {
                t.broken = true;   // its ship is long gone
            }
            g_tracks.push_back(std::move(t));
            best = int(g_tracks.size()) - 1;
        } else {
            Track& t = g_tracks[best];
            V3 a = f.local(e.c);
            if (steps > 0) t.vel = t.vel * 0.5f + (a - t.anchor) * (0.5f / float(steps));
            t.anchor = a;
        }
        Track& t = g_tracks[best];
        t.matched = true;
        t.unseen = 0;
        t.radius = e.radius;
        t.frame = e.frame;
        t.fog = e.fog;
        t.build_index = out.viewports[e.viewport].build_index;
        e.track_id = t.id;
    }
    for (Track& t : g_tracks) {
        if (t.matched) continue;
        t.unseen += steps;
        for (int s = 0; s < steps; s++) t.anchor = t.anchor + t.vel, t.vel = t.vel * 0.97f;
        if (!t.cloud && t.unseen > 0) {   // the fireball has burnt out (or gone): its smoke stays a moment
            t.cloud = true;
            if (t.frame >= 30) {
                Rng rng{t.seed ^ 0x5A5A5A5Au};
                smoke_cloud(t, rng);
            }
        }
    }
    // A ship drawn in the last frames, gone now, where an explosion began:
    // it breaks into pieces of itself
    for (Track& t : g_tracks) {
        t.age += steps;
        if (t.broken) continue;
        if (t.age > 20) {
            t.broken = true;
            continue;
        }
        V3 at = f.view(t.anchor);
        const Ship* best = nullptr;
        float best_d = 1e30f;
        for (const auto& frame : g_ships)
            for (const Ship& sh : frame) {
                if (std::binary_search(keys_now.begin(), keys_now.end(), sh.key)) continue;   // still flying
                float d = length(f.view(sh.origin) - at);
                if (d < std::max(2.0f * t.radius, 1.5f * sh.radius) && d < best_d) best = &sh, best_d = d;
            }
        if (!best) continue;
        Rng rng{t.seed ^ 0xC0FFEEu};
        uint32_t key = best->key;
        break_ship(t, *best, rng);
        t.broken = true;
        for (auto& frame : g_ships)   // one ship, one explosion
            frame.erase(std::remove_if(frame.begin(), frame.end(), [&](const Ship& sh) { return sh.key == key; }), frame.end());
    }
    g_ships.insert(g_ships.begin(), std::move(ships));
    if (g_ships.size() > 3) g_ships.resize(3);

    for (Track& t : g_tracks) {
        Rng rng{hash32(t.seed + uint32_t(out.frame))};
        for (int s = 0; s < steps; s++) simulate(t, rng, FRAME_S);
    }
    g_tracks.erase(std::remove_if(g_tracks.begin(), g_tracks.end(),
                                  [](const Track& t) { return !t.matched && ((t.parts.empty() && t.chunks.empty()) || t.unseen > 400); }),
                   g_tracks.end());
    // The sprite becomes the fireball: two of its triangles hold a quad
    // facing the eye, the rest of it (and its solid texels' copy in the
    // opaque list) collapse to nothing
    for (size_t i = 0; i < found.size(); i++) {
        Found& e = found[i];
        const Track* tr = nullptr;
        for (const Track& t : g_tracks)
            if (t.id == e.track_id) tr = &t;
        uint32_t seed = tr ? tr->seed : 0;
        auto collapse = [&](uint32_t k) {
            for (int j = 0; j < 3; j++) {
                R3DVertex& x = out.vertices[k + j];
                store(x.pos, e.c), store(x.prev, e.prev);
                x.color &= 0x00FFFFFFu;
                x.flags &= ~(R3D_TEXTURED | R3D_TEX_ALPHA | R3D_ALPHA_TEST);
            }
        };
        for (uint32_t k : e.opaque) collapse(k);
        V3 d = normalize(e.c * -1.0f);
        V3 r = normalize(cross(V3{0, 1, 0}, d)), u = cross(d, r);
        float s = e.radius * QUAD * (tr ? tr->scale : 1.0f);
        float sr = s, su = s;
        if (tr && tr->stretch > 1.0f) {   // drawn out along its axis as seen on the screen
            V3 a = f.view(tr->axis);
            V3 on = a - d * dot(a, d);
            float len = length(on);
            if (len > 0.05f) {
                r = on * (1.0f / len), u = cross(d, r);
                float k = 1.0f + (tr->stretch - 1.0f) * len;
                sr = s * k, su = s / std::sqrt(k);
            }
        }
        float t = float(e.frame - FIRST_FRAME) / float(LAST_FRAME - FIRST_FRAME);
        V3 move = e.prev - e.c;
        V3 corner[4] = {e.c - r * sr - u * su, e.c + r * sr - u * su, e.c + r * sr + u * su, e.c - r * sr + u * su};
        float uv[4][2] = {{-QUAD, -QUAD}, {QUAD, -QUAD}, {QUAD, QUAD}, {-QUAD, QUAD}};
        R3DVertex q[4];
        for (int k = 0; k < 4; k++)
            q[k] = vertex(corner[k], corner[k] + move, d, uv[k][0], uv[k][1], FIREBALL, seed, t, e.radius, e.fog, tr ? tr->tint : 0xFFFFFFFFu);
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (size_t j = 0; j < e.trans.size(); j++) {
            if (j < 2) {
                for (int c = 0; c < 3; c++) out.vertices[e.trans[j] + c] = q[order[j * 3 + c]];
            } else {
                collapse(e.trans[j]);
            }
        }
        // A brighter flash of its light at first (r3d_find_glows put a light at its middle)
        if (t < 0.3f)
            for (R3DScene::Glow& g : out.glows)
                if (length(load(g.a) - e.c) < e.radius) g.strength *= 1.0f + 2.0f * (1.0f - t / 0.3f);
    }

    // The particles at the end of the translucent list of their viewport,
    // the ship's pieces at the end of its opaque list
    std::vector<std::vector<R3DVertex>> add_trans(out.viewports.size()), add_opaque(out.viewports.size());
    for (Track& t : g_tracks) {
        if (t.parts.empty() && t.chunks.empty()) continue;
        uint32_t vi = UINT32_MAX;
        for (uint32_t i = 0; i < out.viewports.size(); i++)
            if (out.viewports[i].build_index == t.build_index && (out.viewports[i].world || out.viewports[i].shares_world)) vi = i;
        if (vi == UINT32_MAX) {
            for (Particle& p : t.parts) p.has_prev = false;
            for (Chunk& c : t.chunks) c.prev_view.clear();
            continue;
        }
        draw(t, f, add_trans[vi]);
        draw_chunks(t, f, add_opaque[vi]);
        V3 anchor = f.view(t.anchor);
        for (Particle& p : t.parts) p.prev_view = anchor + f.view(p.p), p.has_prev = true;
    }
    // Inserted from the last position back, so the earlier ones stay put (each
    // position read again as it is reached: two can meet)
    struct Insert {
        uint32_t viewport;
        bool opaque;
        uint32_t at;
    };
    std::vector<Insert> inserts;
    for (uint32_t i = 0; i < out.viewports.size(); i++) {
        const R3DViewport& v = out.viewports[i];
        if (!add_trans[i].empty()) inserts.push_back({i, false, v.trans_first + v.trans_count});
        if (!add_opaque[i].empty()) inserts.push_back({i, true, v.opaque_first + v.opaque_count});
    }
    std::sort(inserts.begin(), inserts.end(), [](const Insert& a, const Insert& b) { return a.at > b.at; });
    for (const Insert& ins : inserts) {
        const std::vector<R3DVertex>& add = ins.opaque ? add_opaque[ins.viewport] : add_trans[ins.viewport];
        R3DViewport& self_vp = out.viewports[ins.viewport];
        uint32_t at = ins.opaque ? self_vp.opaque_first + self_vp.opaque_count : self_vp.trans_first + self_vp.trans_count;
        if (at > out.vertices.size()) continue;
        uint32_t n = uint32_t(add.size());
        out.vertices.insert(out.vertices.begin() + at, add.begin(), add.end());
        for (uint32_t i = 0; i < out.viewports.size(); i++) {
            R3DViewport& v = out.viewports[i];
            if (i == ins.viewport) {   // its translucent list follows its opaque one
                if (ins.opaque) v.trans_first += n;
                continue;
            }
            // (an empty list where the vertices go belongs to the viewport before)
            bool after = v.opaque_first > at || (v.opaque_first == at && (v.opaque_count > 0 || v.trans_count > 0));
            if (after) v.opaque_first += n, v.trans_first += n;
        }
        (ins.opaque ? self_vp.opaque_count : self_vp.trans_count) += n;
    }
}
