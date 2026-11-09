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

struct Cockpit {
    bool on = false;
    std::shared_ptr<const std::vector<R3DNativeScene::Mesh>> meshes;
    Panel panel;
    float fov = 52;
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

}  // namespace

bool dogfight_cockpit_on() { return g_cockpit.on; }

std::vector<std::string> dogfight_cockpit_textures() {
    std::vector<std::string> out;
    if (g_cockpit.on)
        for (const R3DNativeScene::Mesh& m : *g_cockpit.meshes)
            if (!m.texture.empty()) out.push_back(m.texture);
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
        R3DNativeScene::Mesh mesh;
        if (texture[0]) mesh.texture = base + "/" + texture;
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            V3 p[3];
            for (int k = 0; k < 3; k++) p[k] = at(&raw[(i + k) * 8]) - eye;
            // The eye never moves in it: what faces away from it, or lies
            // all behind it, is never seen
            if (dot(cross(p[1] - p[0], p[2] - p[0]), p[0]) >= 0) continue;
            V3 q[3] = {turn(p[0]), turn(p[1]), turn(p[2])};
            if (q[0].z > 0 && q[1].z > 0 && q[2].z > 0) continue;
            for (int k = 0; k < 3; k++) {
                Vertex v;
                V3 nrm = unit(turn(at(&raw[(i + k) * 8 + 3])));
                v.pos[0] = q[k].x * SCALE, v.pos[1] = q[k].y * SCALE, v.pos[2] = q[k].z * SCALE;
                v.nrm[0] = nrm.x, v.nrm[1] = nrm.y, v.nrm[2] = nrm.z;
                v.uv[0] = raw[(i + k) * 8 + 6], v.uv[1] = raw[(i + k) * 8 + 7];
                mesh.verts.push_back(v);
            }
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
    g_cockpit.meshes = meshes;
    g_cockpit.fov = std::clamp(fov, 20.0f, 120.0f);
    g_cockpit.on = true;
    printf("[dogfight] cockpit: %zu triangles in view%s (eye %g,%g,%g tilt %g, field of view %g)\n", triangles,
           g_cockpit.panel.found ? ", with a screen" : "", right, up, back, tilt, g_cockpit.fov);
    return true;
}

void dogfight_cockpit_add(R3DNativeScene& scene) {
    if (!g_cockpit.on) return;
    scene.fixed = g_cockpit.meshes;
    scene.fov = g_cockpit.fov;
    if (!g_cockpit.panel.found) return;
    // The screen: 100 pixels high. Its corners are round under the model's
    // glass, so a margin is left
    Display d{g_cockpit.panel, scene.lights, 100 * g_cockpit.panel.aspect, 100};
    d.box(0, 0, d.w, d.h, GROUND, 0);
    const float x = 12, wide = d.w - 2 * x;
    d.text(x, 9, "FREE FLIGHT", GREEN);
    d.box(x, 19, x + wide, 20, DIM);
    char number[32];
    d.text(x, 26, "SPEED", GREEN);
    snprintf(number, sizeof(number), "%03d", std::clamp(int(dogfight_speed() + 0.5f), 0, 999));
    d.text(x + 40, 23, number, YELLOW, 2);
    d.text(x, 45, "THR", GREEN);
    const float bar = x + 24, bar_w = wide - 24;
    d.box(bar, 45, bar + bar_w, 52, DIM);
    d.box(bar, 45, bar + bar_w * std::clamp(dogfight_throttle(), 0.0f, 1.0f), 52, YELLOW, 2);
    snprintf(number, sizeof(number), "CONTACTS %d", dogfight_ships());
    d.text(x, 62, number, GREEN);
    snprintf(number, sizeof(number), "NEAREST %d", std::clamp(int(dogfight_nearest() + 0.5f), 0, 99999));
    d.text(x, 74, number, GREEN);
}
