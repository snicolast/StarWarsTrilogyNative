#include "r3d_catalog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

#include "r3d_scene.h"
#include "r3d_screen.h"

namespace {

constexpr int MAX_PARENTS = 4;
constexpr int MAX_TEXTURES = 6;

struct Entry {
    uint32_t polygons = 0;       // the most drawn at once (LODs and culled polygons draw fewer)
    float radius = 0;            // farthest vertex from the model's origin, in world units as drawn
    float model_scale = 1, matrix_scale = 1;
    int priority = 0;
    uint32_t viewport = 0;
    uint32_t parents[MAX_PARENTS] = {};
    int parent_count = 0;
    uint32_t parent_sets = 0;    // how many times the node right above it changed (a rebuilt display list, or several ships)
    uint32_t textures[MAX_TEXTURES] = {};   // texture words of its polygons (as out/scene.txt prints them)
    int texture_count = 0;
    uint32_t flags = 0;          // R3D_* of its vertices, all together
    uint64_t first_frame = 0, last_frame = 0, nearest_frame = 0;
    uint64_t draws = 0;          // times drawn (several copies a frame count each)
    uint32_t most_copies = 0;    // the most copies in one frame
    uint32_t copies_now = 0;
    float nearest = 1e30f;       // nearest it came to the camera
    bool opaque = false, see_through = false;
};

// stage (top state), model, texture x, texture y, page
using Key = std::tuple<int, uint32_t, int, int, int>;

std::mutex g_mutex;
std::map<Key, Entry> g_models;
uint64_t g_written_at = 0;
bool g_dirty = false;

}  // namespace

bool r3d_catalog_on() {
    static const bool on = getenv("SWT_MODEL_CATALOG") != nullptr;
    return on;
}

void r3d_catalog_add(const R3DCatalogModel& m, const R3DVertex* opaque, uint32_t opaque_count, const R3DVertex* trans,
                     uint32_t trans_count, uint64_t frame) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Entry& e = g_models[Key{r3d_swt_stage(), m.model & 0xFFFFFF, m.tex_x, m.tex_y, m.page}];
    if (!e.draws) e.first_frame = frame;
    if (e.last_frame != frame) e.copies_now = 0;
    e.last_frame = frame;
    e.draws++;
    e.most_copies = std::max(e.most_copies, ++e.copies_now);
    e.polygons = std::max(e.polygons, m.polygons);
    e.model_scale = m.model_scale;
    e.matrix_scale = m.matrix_scale;
    e.priority = m.priority;
    e.viewport = m.viewport;
    if (m.parent_count && (!e.parent_count || e.parents[0] != m.parents[0])) e.parent_sets++;
    e.parent_count = std::min(m.parent_count, MAX_PARENTS);
    for (int i = 0; i < e.parent_count; i++) e.parents[i] = m.parents[i];
    e.opaque |= opaque_count > 0;
    e.see_through |= trans_count > 0;
    float dist = std::sqrt(m.origin[0] * m.origin[0] + m.origin[1] * m.origin[1] + m.origin[2] * m.origin[2]);
    if (dist < e.nearest) e.nearest = dist, e.nearest_frame = frame;
    auto take = [&](const R3DVertex* v, uint32_t count) {
        for (uint32_t i = 0; i < count; i++) {
            float d[3] = {v[i].pos[0] - m.origin[0], v[i].pos[1] - m.origin[1], v[i].pos[2] - m.origin[2]};
            e.radius = std::max(e.radius, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
            e.flags |= v[i].flags;
            if (!(v[i].flags & R3D_TEXTURED) || e.texture_count >= MAX_TEXTURES) continue;
            if (std::find(e.textures, e.textures + e.texture_count, v[i].tex) == e.textures + e.texture_count)
                e.textures[e.texture_count++] = v[i].tex;
        }
    };
    take(opaque, opaque_count);
    take(trans, trans_count);
    g_dirty = true;
}

void r3d_catalog_frame(uint64_t frame) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_dirty || (frame >= g_written_at && frame - g_written_at < 600)) return;
        g_written_at = frame;
    }
    r3d_catalog_write();
}

void r3d_catalog_write() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_dirty) return;
    FILE* f = fopen("out/model_catalog.txt", "w");
    if (!f) return;
    g_dirty = false;
    fprintf(f, "# Models drawn, by stage (the game's top state; -1: not playing), viewport priority and size.\n"
               "# model: address (RAM = polygon RAM, loaded per stage; ROM = VROM). polys: the most drawn at once.\n"
               "# radius: farthest vertex from the origin, world units. tex: texture offsets (x, y, page) from its nodes.\n"
               "# parents: the nodes above it, nearest first (models under the same node are parts of one object);\n"
               "#   sets: times the nearest one changed. copies: the most in one frame. nearest: distance and its frame.\n"
               "# pass: O opaque, T see-through. textures: texture words of its polygons.\n");
    int stage = -2;
    std::vector<std::pair<Key, const Entry*>> rows;
    for (const auto& [key, e] : g_models) rows.push_back({key, &e});
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (std::get<0>(a.first) != std::get<0>(b.first)) return std::get<0>(a.first) < std::get<0>(b.first);
        if (a.second->priority != b.second->priority) return a.second->priority < b.second->priority;
        return a.second->polygons > b.second->polygons;
    });
    for (const auto& [key, e] : rows) {
        if (std::get<0>(key) != stage) {
            stage = std::get<0>(key);
            fprintf(f, "\n== stage %d ==\n", stage);
        }
        uint32_t model = std::get<1>(key);
        fprintf(f, "prio %d vp %06X  model %06X %s  polys %5u  radius %9.2f  scale %g x %g  tex %4d %4d %d  pass %s%s  flags %08X\n",
                e->priority, e->viewport, model, model < 0x100000 ? "RAM" : "ROM", e->polygons, e->radius, e->model_scale,
                e->matrix_scale, std::get<2>(key), std::get<3>(key), std::get<4>(key), e->opaque ? "O" : "", e->see_through ? "T" : "",
                e->flags);
        fprintf(f, "    parents");
        for (int i = 0; i < e->parent_count; i++) fprintf(f, " %06X", e->parents[i]);
        fprintf(f, " (sets %u)  copies %u  draws %llu  frames %llu-%llu  nearest %.1f at %llu\n", e->parent_sets, e->most_copies,
                (unsigned long long)e->draws, (unsigned long long)e->first_frame, (unsigned long long)e->last_frame, e->nearest,
                (unsigned long long)e->nearest_frame);
        if (e->texture_count) {
            fprintf(f, "    textures");
            for (int i = 0; i < e->texture_count; i++) fprintf(f, " %08X", e->textures[i]);
            fprintf(f, "\n");
        }
    }
    fclose(f);
}
