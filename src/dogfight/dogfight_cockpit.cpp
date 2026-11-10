// DOGFIGHT's cockpit: a 3D model of the user's (tools/import_cockpit.py makes
// models/cockpit/cockpit.mesh from the FBX they downloaded) fixed round the
// camera, and what its screen shows, drawn as glowing rectangles just over the
// model's screen (a texture would have to be uploaded again at every change).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dogfight.h"
#include "dogfight_font.h"

namespace {

using Vertex = R3DNativeScene::Vertex;

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 unit(V3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 0 ? a * (1 / l) : V3{0, 0, 1};
}
V3 at(const float* p) { return {p[0], p[1], p[2]}; }

// The model's screen in view space: a corner, its sides and what faces the eye
struct Panel {
    bool found = false;
    V3 top_left, right, down, out;   // right and down: the whole side; out: a unit
    float aspect = 1.4f;             // width over height
};

// A lamp or LED of the model: the triangles of the "Glow" group that touch one another
struct Lamp {
    V3 centre, facing;
    float area = 0;      // square metres
    uint32_t seed = 0;   // how it blinks
    float uv[2] = {};    // a place of its own in the lamps' image (the middle of its biggest triangle): its colour
    float biggest = 0;
};

struct Cockpit {
    bool on = false;
    std::shared_ptr<const std::vector<R3DNativeScene::Mesh>> meshes;
    std::shared_ptr<const std::vector<float>> casters;   // every triangle of it, three corners each (the sun's shadows inside)
    Panel panel;
    float fov = 52;
    std::string lamp_texture;
    std::vector<Vertex> lamp_verts;     // three a triangle
    std::vector<uint32_t> lamp_of;      // per triangle: its lamp
    std::vector<Lamp> lamps;
    std::vector<R3DNativeScene::Glow> glows;   // the light of the biggest lamps on what is near
};
Cockpit g_cockpit;

// The screen's picture, in its own pixels (y downwards), as rectangles
struct Display {
    const Panel& p;
    std::vector<Vertex>& out;
    float w, h;   // its size in pixels
    void box(float x0, float y0, float x1, float y1, uint32_t color, float lift = 1) {
        // (what is drawn later lies a little nearer the eye: letters over their ground)
        V3 base = p.top_left + p.out * (lift * 0.02f * std::sqrt(dot(p.down, p.down)));
        auto corner = [&](float x, float y) {
            V3 q = base + p.right * (x / w) + p.down * (y / h);
            Vertex v;
            v.pos[0] = q.x, v.pos[1] = q.y, v.pos[2] = q.z;
            v.color = color;
            return v;
        };
        Vertex a = corner(x0, y0), b = corner(x1, y0), c = corner(x1, y1), d = corner(x0, y1);
        for (const Vertex& v : {a, b, c, a, c, d}) out.push_back(v);
    }
    // Letters `size` pixels to the dot; returns the x after the text
    float text(float x, float y, const char* s, uint32_t color, float size = 1) {
        for (const char* c = s; *c; c++, x += 6 * size) {
            for (const DogfightGlyph& g : DOGFIGHT_GLYPHS) {
                if (g.c != *c) continue;
                for (int r = 0; r < 7; r++)
                    for (int i = 0; i < 5; i++) {
                        if (g.rows[r][i] != '#') continue;
                        int j = i;   // a run of lit dots is one rectangle
                        while (j < 5 && g.rows[r][j] == '#') j++;
                        box(x + i * size, y + r * size, x + j * size, y + (r + 1) * size, color, 2);
                        i = j;
                    }
                break;
            }
        }
        return x;
    }
};

// RGBA8, R in the low byte
constexpr uint32_t GROUND = 0xFF0A1C06u, GREEN = 0xFF50F040u, YELLOW = 0xFF30E0F0u, DIM = 0xFF184810u;

// The lamps of the "Glow" group: its triangles joined where they share a
// corner. The biggest light what is around them
void find_lamps() {
    Cockpit& c = g_cockpit;
    const size_t tris = c.lamp_verts.size() / 3;
    std::vector<uint32_t> parent(tris);
    for (uint32_t i = 0; i < tris; i++) parent[i] = i;
    auto root = [&](uint32_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    // Corners by place (to a tenth of a millimetre), sorted: equal neighbours are one corner
    struct Corner {
        int32_t x, y, z;
        uint32_t tri;
        bool operator<(const Corner& o) const { return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z; }
    };
    std::vector<Corner> corners;
    for (uint32_t i = 0; i < tris * 3; i++) {
        const float* p = c.lamp_verts[i].pos;
        corners.push_back({int32_t(std::lround(p[0] * 1e4f)), int32_t(std::lround(p[1] * 1e4f)), int32_t(std::lround(p[2] * 1e4f)), i / 3});
    }
    std::sort(corners.begin(), corners.end());
    for (size_t i = 1; i < corners.size(); i++)
        if (!(corners[i - 1] < corners[i])) parent[root(corners[i].tri)] = root(corners[i - 1].tri);
    std::vector<int> index(tris, -1);
    c.lamp_of.resize(tris);
    c.lamps.clear();
    for (uint32_t i = 0; i < tris; i++) {
        uint32_t r = root(i);
        if (index[r] < 0) index[r] = int(c.lamps.size()), c.lamps.push_back({});
        c.lamp_of[i] = uint32_t(index[r]);
        Lamp& l = c.lamps[size_t(index[r])];
        V3 a = at(c.lamp_verts[i * 3].pos), b = at(c.lamp_verts[i * 3 + 1].pos), d = at(c.lamp_verts[i * 3 + 2].pos);
        V3 n = cross(b - a, d - a);
        float area = 0.5f * std::sqrt(dot(n, n));
        l.centre = l.centre + (a + b + d) * (area / 3);
        if (area > l.biggest) {
            l.biggest = area;
            for (int k = 0; k < 2; k++)
                l.uv[k] = (c.lamp_verts[i * 3].uv[k] + c.lamp_verts[i * 3 + 1].uv[k] + c.lamp_verts[i * 3 + 2].uv[k]) / 3;
        }
        l.facing = l.facing + n;
        l.area += area;
    }
    for (Lamp& l : c.lamps) {
        if (l.area > 0) l.centre = l.centre * (1 / l.area);
        l.facing = unit(l.facing);
        // (the same from run to run: from where it is)
        l.seed = uint32_t(std::lround(l.centre.x * 977) * 73856093 ^ std::lround(l.centre.y * 977) * 19349663 ^
                          std::lround(l.centre.z * 977) * 83492791);
        l.seed = (l.seed ^ (l.seed >> 13)) * 0x5BD1E995u;
        l.seed ^= l.seed >> 15;
    }
    // The light of the three biggest, from a little in front of each
    std::vector<const Lamp*> big;
    for (const Lamp& l : c.lamps) big.push_back(&l);
    std::sort(big.begin(), big.end(), [](const Lamp* a, const Lamp* b) { return a->area > b->area; });
    c.glows.clear();
    for (size_t i = 0; i < big.size() && i < 3; i++) {
        R3DNativeScene::Glow g;
        V3 p = big[i]->centre + big[i]->facing * 0.04f;
        g.pos[0] = p.x, g.pos[1] = p.y, g.pos[2] = p.z;
        g.color[0] = 1.0f, g.color[1] = 0.22f, g.color[2] = 0.16f;   // the red of the model's lamps
        g.radius = 0.14f;
        g.strength = 0.4f;
        c.glows.push_back(g);
    }
}

// How bright a lamp is now, 0..1: most are steady, some blink at their own
// pace, a few flicker now and then; the big ones only breathe
float lamp_level(const Lamp& l, double time) {
    if (l.area > 4e-4f) return 0.82f + 0.08f * float(std::sin(time * 1.3 + (l.seed & 255) * 0.1));
    uint32_t kind = l.seed % 100;
    if (kind < 50) return 1.0f;
    double period = 0.5 + double((l.seed >> 8) % 1000) / 1000.0 * 2.2, phase = double((l.seed >> 18) % 1000) / 1000.0;
    double at_ = time / period + phase;
    double f = at_ - std::floor(at_);
    if (kind < 85) return f < 0.5 ? 1.0f : 0.12f;   // on and off
    return f < 0.06 || (f > 0.12 && f < 0.16) ? 0.12f : 1.0f;   // two short dips
}

}  // namespace

bool dogfight_cockpit_on() { return g_cockpit.on; }

std::vector<std::string> dogfight_cockpit_textures() {
    std::vector<std::string> out;
    if (g_cockpit.on)
        for (const R3DNativeScene::Mesh& m : *g_cockpit.meshes)
            if (!m.texture.empty()) out.push_back(m.texture);
    if (g_cockpit.on && !g_cockpit.lamp_texture.empty() && !g_cockpit.lamp_verts.empty()) out.push_back(g_cockpit.lamp_texture);
    return out;
}

bool dogfight_cockpit_load(const char* dir) {
    static bool tried = false;   // once: it is asked for when the mode is chosen and again when the flight starts
    if (tried) return g_cockpit.on;
    tried = true;
    g_cockpit = Cockpit{};
    // Where the eye sits: right, up and back from the model's origin (its
    // units), looking down by `tilt` degrees; the view's vertical field
    float right = 0, up = 3, back = 8, tilt = 0, fov = 52;
    if (const char* e = getenv("SWT_DOGFIGHT_COCKPIT"))
        if (sscanf(e, "%f,%f,%f,%f,%f", &right, &up, &back, &tilt, &fov) < 1) return false;   // "off"
    const float SCALE = 0.01f;   // the model's centimetres to the game's units (metres, roughly)

    std::string base = dir;
    FILE* f = fopen((base + "/cockpit.mesh").c_str(), "rb");
    if (!f) return false;
    char magic[8];
    uint32_t groups = 0;
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "SWTMESH1", 8) && fread(&groups, 4, 1, f) == 1 && groups <= 64;

    const float t = tilt * 3.14159265f / 180.0f;
    const V3 eye{right, up, back};
    const V3 view_up{0, std::cos(t), -std::sin(t)}, view_fwd{0, -std::sin(t), -std::cos(t)};
    auto turn = [&](V3 q) { return V3{q.x, dot(q, view_up), -dot(q, view_fwd)}; };   // to the view's axes: x right, y up, -z ahead

    auto meshes = std::make_shared<std::vector<R3DNativeScene::Mesh>>();
    auto casters = std::make_shared<std::vector<float>>();
    size_t triangles = 0;
    for (uint32_t g = 0; ok && g < groups; g++) {
        char name[33] = {}, texture[65] = {};
        uint32_t count = 0;
        ok = fread(name, 1, 32, f) == 32 && fread(texture, 1, 64, f) == 64 && fread(&count, 4, 1, f) == 1 && count <= 3000000 &&
             count % 3 == 0;
        if (!ok) break;
        std::vector<float> raw(size_t(count) * 8);
        ok = fread(raw.data(), 4, raw.size(), f) == raw.size();
        if (!ok) break;
        if (!strcmp(name, "Windows")) continue;   // the canopy's glass: not drawn
        if (!strcmp(name, "Screen")) {
            // The screen: a flat polygon. Its sides from the corners of its
            // vertices along the plane's own right and up
            V3 n{}, centre{};
            for (uint32_t i = 0; i < count; i++) n = n + turn(at(&raw[i * 8 + 3])), centre = centre + turn(at(&raw[i * 8]) - eye);
            if (!count) continue;
            n = unit(n);
            centre = centre * (1.0f / float(count));
            if (dot(n, centre) > 0) n = n * -1;   // towards the eye
            V3 r = unit(V3{1, 0, 0} - n * n.x), u = cross(n, r);
            float r0 = 1e30f, r1 = -1e30f, u0 = 1e30f, u1 = -1e30f;
            for (uint32_t i = 0; i < count; i++) {
                V3 q = turn(at(&raw[i * 8]) - eye) - centre;
                r0 = std::min(r0, dot(q, r)), r1 = std::max(r1, dot(q, r));
                u0 = std::min(u0, dot(q, u)), u1 = std::max(u1, dot(q, u));
            }
            Panel& p = g_cockpit.panel;
            p.found = r1 > r0 && u1 > u0;
            p.out = n;
            p.top_left = (centre + r * r0 + u * u1) * SCALE;
            p.right = r * ((r1 - r0) * SCALE);
            p.down = u * (-(u1 - u0) * SCALE);
            p.aspect = (r1 - r0) / std::max(u1 - u0, 1e-6f);
            continue;
        }
        const bool glow = !strcmp(name, "Glow");
        // (the lamps are drawn a hair nearer the eye than the surface they are part of)
        const float scale = glow ? SCALE * 0.997f : SCALE;
        R3DNativeScene::Mesh mesh;
        if (texture[0]) mesh.texture = base + "/" + texture;
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            V3 p[3];
            for (int k = 0; k < 3; k++) p[k] = at(&raw[(i + k) * 8]) - eye;
            // All of it stands between the sun and the inside, seen or not
            if (!glow)
                for (int k = 0; k < 3; k++) {
                    V3 q = turn(p[k]) * SCALE;
                    casters->insert(casters->end(), {q.x, q.y, q.z});
                }
            // The eye never moves in it: what faces away from it, or lies
            // all behind it, is never seen
            if (dot(cross(p[1] - p[0], p[2] - p[0]), p[0]) >= 0) continue;
            V3 q[3] = {turn(p[0]), turn(p[1]), turn(p[2])};
            if (q[0].z > 0 && q[1].z > 0 && q[2].z > 0) continue;
            for (int k = 0; k < 3; k++) {
                Vertex v;
                V3 nrm = unit(turn(at(&raw[(i + k) * 8 + 3])));
                v.pos[0] = q[k].x * scale, v.pos[1] = q[k].y * scale, v.pos[2] = q[k].z * scale;
                v.nrm[0] = nrm.x, v.nrm[1] = nrm.y, v.nrm[2] = nrm.z;
                v.uv[0] = raw[(i + k) * 8 + 6], v.uv[1] = raw[(i + k) * 8 + 7];
                mesh.verts.push_back(v);
            }
        }
        if (glow) {
            g_cockpit.lamp_texture = mesh.texture;
            g_cockpit.lamp_verts = std::move(mesh.verts);
            continue;
        }
        // The nearest triangles first: what lies behind them is then not
        // shaded at all (the dashboard is many layers deep, and at 1440p the
        // picture no longer kept up with a 120 Hz display)
        {
            const size_t count3 = mesh.verts.size() / 3;
            std::vector<std::pair<float, uint32_t>> order(count3);
            for (size_t i = 0; i < count3; i++) {
                float d = 1e30f;
                for (int k = 0; k < 3; k++) d = std::min(d, dot(at(mesh.verts[i * 3 + k].pos), at(mesh.verts[i * 3 + k].pos)));
                order[i] = {d, uint32_t(i)};
            }
            std::sort(order.begin(), order.end());
            std::vector<Vertex> sorted;
            sorted.reserve(mesh.verts.size());
            for (const auto& o : order)
                for (int k = 0; k < 3; k++) sorted.push_back(mesh.verts[size_t(o.second) * 3 + k]);
            mesh.verts.swap(sorted);
        }
        triangles += mesh.verts.size() / 3;
        if (!mesh.verts.empty()) meshes->push_back(std::move(mesh));
    }
    fclose(f);
    if (!ok || meshes->empty()) {
        printf("[dogfight] %s/cockpit.mesh could not be read\n", dir);
        g_cockpit = Cockpit{};
        return false;
    }
    find_lamps();
    g_cockpit.meshes = meshes;
    g_cockpit.casters = casters;
    g_cockpit.fov = std::clamp(fov, 20.0f, 120.0f);
    g_cockpit.on = true;
    printf("[dogfight] cockpit: %zu triangles in view%s, %zu lamps (eye %g,%g,%g tilt %g, field of view %g)\n", triangles,
           g_cockpit.panel.found ? ", with a screen" : "", g_cockpit.lamps.size(), right, up, back, tilt, g_cockpit.fov);
    return true;
}

void dogfight_cockpit_add(R3DNativeScene& scene, double time, const float* sway) {
    if (!g_cockpit.on) return;
    scene.fixed = g_cockpit.meshes;
    scene.casters = g_cockpit.casters;
    // The exposure as for a typical scene: measured, the dark inside opened
    // it as far as it goes and the ships outside went white
    scene.exposure_average = 0.17f;
    scene.fov = g_cockpit.fov;
    // A dim inside: little of the sun gets in, the lamps and the screen light it
    // (SWT_DOGFIGHT_LIGHT=floor,sun: the light in the dark and what the sun adds, 0..1)
    static const struct Light {
        float floor = 0.115f, sun = 0.345f;
        Light() {
            if (const char* e = getenv("SWT_DOGFIGHT_LIGHT")) sscanf(e, "%f,%f", &floor, &sun);
        }
    } light;
    scene.fixed_light[0] = light.floor, scene.fixed_light[1] = light.sun;
    // The sway: a turn (a small rotation vector, radians) about the eye and a
    // shift back or ahead (Rodrigues' formula; view axes: x right, y up, -z ahead)
    if (sway) {
        V3 w{sway[0], sway[1], sway[2]};
        float angle = std::sqrt(dot(w, w));
        V3 k = angle > 1e-6f ? w * (1 / angle) : V3{1, 0, 0};
        float cs = std::cos(angle), sn = std::sin(angle);
        const V3 e[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int c = 0; c < 3; c++) {
            V3 r = e[c] * cs + cross(k, e[c]) * sn + k * (dot(k, e[c]) * (1 - cs));
            scene.fixed_m[c * 4] = r.x, scene.fixed_m[c * 4 + 1] = r.y, scene.fixed_m[c * 4 + 2] = r.z;
        }
        scene.fixed_m[12] = sway[3], scene.fixed_m[13] = sway[4], scene.fixed_m[14] = sway[5];
    }
    if (!g_cockpit.lamp_verts.empty()) {
        scene.lamp_texture = g_cockpit.lamp_texture;
        scene.lamps = g_cockpit.lamp_verts;
        std::vector<uint8_t> level(g_cockpit.lamps.size());
        for (size_t i = 0; i < level.size(); i++) level[i] = uint8_t(std::clamp(lamp_level(g_cockpit.lamps[i], time), 0.0f, 1.0f) * 255.0f + 0.5f);
        for (size_t i = 0; i < scene.lamps.size(); i++) scene.lamps[i].color = 0xFF000000u | level[g_cockpit.lamp_of[i / 3]] * 0x010101u;
        // A halo round each small one that is lit: a disc in its plane, a
        // little nearer the eye, fading out to three and a half times its size
        // (the bloom is too coarse for lamps a few pixels wide)
        for (size_t i = 0; i < g_cockpit.lamps.size(); i++) {
            const Lamp& l = g_cockpit.lamps[i];
            if (l.area > 4e-4f || level[i] < 40 || dot(l.facing, l.centre) >= 0) continue;
            const float size = std::sqrt(l.area / 3.14159265f);
            const V3 mid = l.centre * 0.994f;
            const V3 r = unit(cross(l.facing, std::fabs(l.facing.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0})), u = cross(l.facing, r);
            const uint32_t inner = uint32_t(0.25f * float(level[i]) + 0.5f) << 24 | 0xFFFFFFu, outer = 0xFFFFFFu;
            auto corner = [&](float radius, int step, uint32_t color) {
                const float a = float(step) * (6.2831853f / 12);
                V3 q = mid + (r * std::cos(a) + u * std::sin(a)) * radius;
                Vertex v;
                v.pos[0] = q.x, v.pos[1] = q.y, v.pos[2] = q.z;
                v.uv[0] = l.uv[0], v.uv[1] = l.uv[1];
                v.color = color;
                return v;
            };
            const float r1 = size * 1.2f, r2 = size * 3.5f + 0.002f;
            for (int k = 0; k < 12; k++) {
                Vertex c0 = corner(0, 0, inner), a1 = corner(r1, k, inner), b1 = corner(r1, k + 1, inner);
                Vertex a2 = corner(r2, k, outer), b2 = corner(r2, k + 1, outer);
                for (const Vertex& v : {c0, a1, b1, a1, a2, b2, a1, b2, b1}) scene.halos.push_back(v);
            }
        }
        scene.glows = g_cockpit.glows;
    }
    if (!g_cockpit.panel.found) return;
    {   // the screen's own light on the dashboard, green
        const Panel& p = g_cockpit.panel;
        V3 c = p.top_left + p.right * 0.5f + p.down * 0.5f + p.out * 0.05f;
        R3DNativeScene::Glow g;
        g.pos[0] = c.x, g.pos[1] = c.y, g.pos[2] = c.z;
        g.color[0] = 0.25f, g.color[1] = 1.0f, g.color[2] = 0.3f;
        g.radius = 0.12f;
        g.strength = 0.35f;
        scene.glows.push_back(g);
    }
    // The screen: 100 pixels high. Its corners are round under the model's
    // glass, so a margin is left
    Display d{g_cockpit.panel, scene.lights, 100 * g_cockpit.panel.aspect, 100};
    d.box(0, 0, d.w, d.h, GROUND, 0);
    const float x = 12, wide = d.w - 2 * x;
    char number[32];
    if (dogfight_kills() > 0) {
        snprintf(number, sizeof(number), "KILLS %d", dogfight_kills());
        d.text(x, 9, number, YELLOW);
    } else {
        d.text(x, 9, "FREE FLIGHT", GREEN);
    }
    d.box(x, 19, x + wide, 20, DIM);
    d.text(x, 26, "SPEED", GREEN);
    snprintf(number, sizeof(number), "%03d", std::clamp(int(dogfight_speed() + 0.5f), 0, 999));
    d.text(x + 40, 23, number, YELLOW, 2);
    d.text(x, 45, "THR", GREEN);
    const float bar = x + 24, bar_w = wide - 24;
    d.box(bar, 45, bar + bar_w, 52, DIM);
    // The bar fills with the speed the ship has; a mark stands where the
    // throttle asks it to be, and the bar comes up to it
    d.box(bar, 45, bar + bar_w * dogfight_speed_part(), 52, YELLOW, 2);
    const float mark = bar + (bar_w - 2) * std::clamp(dogfight_throttle(), 0.0f, 1.0f);
    d.box(mark, 43, mark + 2, 54, GREEN, 3);
    snprintf(number, sizeof(number), "CONTACTS %d", dogfight_ships());
    d.text(x, 62, number, GREEN);
    snprintf(number, sizeof(number), "NEAREST %d", std::clamp(int(dogfight_nearest() + 0.5f), 0, 99999));
    d.text(x, 74, number, GREEN);
}
