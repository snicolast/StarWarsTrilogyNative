#include "r3d_screen.h"

#include <atomic>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// The cutscene letterbox is two flat black quads drawn in an overlay viewport
// (higher priority than the scene) across the top and bottom of the screen.
// They are collapsed to nothing so the scene shows through, which also lets the
// widescreen view use the whole window height.
// Screen bounding box of a triangle in native pixels: x0, y0, x1, y1
static bool screen_bbox(const R3DViewport& v, const R3DVertex* tri, float* b) {
    b[0] = b[1] = 1e9f;
    b[2] = b[3] = -1e9f;
    const float* m = v.proj;
    for (int k = 0; k < 3; k++) {
        const float* p = tri[k].pos;
        float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        if (cw <= 1e-6f) return false;
        float sx = v.x + ((m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / cw + 1) / 2 * v.w;
        float sy = v.y + ((m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / cw + 1) / 2 * v.h;
        b[0] = std::min(b[0], sx), b[2] = std::max(b[2], sx), b[1] = std::min(b[1], sy), b[3] = std::max(b[3], sy);
    }
    return true;
}

// The ammo counter is 3D geometry in an overlay viewport, drawn inside the
// left (or right) margin of the 4:3 screen. Those triangles are tagged so the
// vertex shader moves them out to the edges of a widescreen view. The gun
// sight lives in the same viewport but much closer to the camera (z = 1), and
// must stay where the gun points, so near geometry is left alone.
void r3d_anchor_hud(R3DScene& out) {
    if (out.viewports.empty()) return;
    int scene_priority = out.viewports.front().priority;
    const float band = 72.0f;   // native pixels from each side of the 4:3 screen
    for (const R3DViewport& v : out.viewports) {
        if (v.priority == scene_priority) continue;
        for (int pass = 0; pass < 2; pass++) {
            uint32_t first = pass ? v.trans_first : v.opaque_first, count = pass ? v.trans_count : v.opaque_count;
            for (uint32_t t = 0; t + 2 < count; t += 3) {
                R3DVertex* tri = &out.vertices[first + t];
                float b[4];
                if (-tri[0].pos[2] < 10.0f || !screen_bbox(v, tri, b)) continue;
                uint32_t flag = b[2] < v.x + band ? R3D_HUD_LEFT : b[0] > v.x + v.w - band ? R3D_HUD_RIGHT : 0;
                for (int k = 0; k < 3; k++) tri[k].flags |= flag;
            }
        }
    }
}

void r3d_strip_letterbox(R3DScene& out) {
    if (out.viewports.empty()) return;
    int scene_priority = out.viewports.front().priority;
    for (const R3DViewport& v : out.viewports) {
        if (v.priority == scene_priority) continue;
        for (uint32_t t = 0; t + 2 < v.opaque_count; t += 3) {
            R3DVertex* tri = &out.vertices[v.opaque_first + t];
            if ((tri[0].color & 0xFFFFFF) != 0 || (tri[0].flags & R3D_TEXTURED)) continue;
            float b[4];
            if (!screen_bbox(v, tri, b)) continue;
            float x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
            bool wide = x1 - x0 >= 0.9f * v.w;
            bool band = y1 - y0 < 0.5f * v.h;   // full-screen fades are kept
            bool edge = y0 <= v.y + 1 || y1 >= v.y + v.h - 1;
            if (!(wide && band && edge)) continue;
            for (int k = 1; k < 3; k++) memcpy(tri[k].pos, tri[0].pos, sizeof(tri[0].pos));
        }
    }
}

static std::atomic<bool> g_duel{false};   // a lightsaber duel (r3d_set_swt_duel)
static std::atomic<int> g_stage{-1};      // the top state while playing (r3d_set_swt_stage)

void r3d_share_world(R3DScene& out) {
    auto& vps = out.viewports;
    if (vps.empty()) return;
    auto full = [](const R3DViewport& v) { return v.x <= 0.5f && v.w >= 495.5f; };
    // A star field behind everything (the lowest viewport: thousands of tiny
    // luminous polygons) makes it space, whatever else the frame holds: a
    // world the tracker finds there for a few frames must not get the haze
    // and the warm sunlight of a planet (the Death Star flashed white)
    const R3DViewport& back = vps.front();
    // (the star texture 0x0920xxxx: the Emperor's throne room, Vader's duel,
    // is thousands of luminous polygons in the lowest viewport too, and was
    // replaced by the procedural space)
    // The desert of the duel with Boba Fett (Tatooine): one 64x64 texture
    // drawn luminous and see-through, flat from near to far. Tagged so the
    // shader adds grains and wind ripples (SWT_PLAIN_SAND keeps it as it was)
    static const bool plain_sand = getenv("SWT_PLAIN_SAND") != nullptr;
    if (!plain_sand)
        for (const R3DViewport& v : vps)
            for (uint32_t k = v.trans_first; k < v.trans_first + v.trans_count; k++)
                if (out.vertices[k].tex == 0x72620380u) out.vertices[k].flags |= R3D_SAND;
    // Vader in his duel: the character pages of the duels (Luke's arms use
    // them too, but they are drawn over the world, priority 1) in the
    // world's viewports. The shader gives the black of the suit and helmet a
    // gloss and a reflection of the room (SWT_PLAIN_VADER keeps them matt)
    static const bool plain_vader = getenv("SWT_PLAIN_VADER") != nullptr;
    if (g_duel && !plain_vader) {
        int low = vps.front().priority;
        auto vader = [](uint32_t t) {
            switch (t >> 16) {
            case 0x0013: case 0x0014: case 0x0050: case 0x0052: case 0x0212: case 0x0250: case 0x028C: case 0x0450: case 0x0488:
                return true;
            default: return false;
            }
        };
        for (const R3DViewport& v : vps) {
            if (v.priority != low) continue;
            for (uint32_t k = v.opaque_first; k < v.opaque_first + v.opaque_count; k++)
                if (vader(out.vertices[k].tex) && (out.vertices[k].flags & (R3D_TEXTURED | R3D_LUMINOUS)) == R3D_TEXTURED)
                    out.vertices[k].flags |= R3D_VADER;
        }
    }
    auto star = [&](uint32_t k) {
        const R3DVertex& x = out.vertices[back.opaque_first + k];
        return (x.flags & R3D_LUMINOUS) && (x.tex >> 16) == 0x0920u;
    };
    bool stars = full(back) && back.opaque_count >= 3 * 5000 && star(0) && star(back.opaque_count / 2);
    if (stars) vps.front().stars = true;
    int w = -1;
    for (size_t i = 0; i < vps.size(); i++)
        if (vps[i].world) w = int(i);
    if (w >= 0 && stars) vps[w].space = true;
    if (w < 0) {
        // Space: the full-screen viewport with the most opaque polygons
        // between the backdrop (the lowest priority: the stars) and the HUD
        // (the highest: the sight and the markers)
        int lo = vps.front().priority, hi = vps.back().priority;
        uint32_t best = 0;
        for (size_t i = 0; i < vps.size(); i++) {
            const R3DViewport& v = vps[i];
            if (v.priority == lo || v.priority == hi || !full(v)) continue;
            if (v.opaque_count > best) best = v.opaque_count, w = int(i);
        }
        // Only in space (a star field behind): on the ground a frame the
        // tracker misses (Hoth's base, now and then) became "space", with the
        // full glow of space and an occlusion radius for open space: white blots
        if (w < 0 || !stars || best < 3 * 30) return;   // in space, a ship or two
        R3DViewport& v = vps[w];
        v.world = v.space = true;
        float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        memcpy(v.world_axes, id, sizeof(id));
        memset(v.camera, 0, sizeof(v.camera));
        v.ground = 0;
        v.scenery_far = 1e6f;
        for (uint32_t k = v.opaque_first; k < v.opaque_first + v.opaque_count; k++) out.vertices[k].flags |= R3D_WORLD;
        for (uint32_t k = v.trans_first; k < v.trans_first + v.trans_count; k++) out.vertices[k].flags |= R3D_WORLD;
    }
    const R3DViewport& wv = vps[w];
    auto close = [](float a, float b) { return std::fabs(a - b) <= 1e-3f * std::max(std::fabs(a), std::fabs(b)) + 1e-6f; };
    for (size_t i = 0; i < vps.size(); i++) {
        R3DViewport& v = vps[i];
        if (int(i) == w || v.priority != wv.priority || full(v) != full(wv) || !close(v.proj[0], wv.proj[0]) ||
            !close(v.proj[5], wv.proj[5]) || !close(v.proj[8], wv.proj[8]) || !close(v.proj[9], wv.proj[9]))
            continue;
        v.shares_world = true;
        // No light at all (sun and ambient both 0): the actors at the end of
        // Hoth's base (C-3PO, R2-D2, the trooper by the Falcon) came out
        // black, where the arcade has them lit as the world (issue #15).
        // They are drawn with the world's camera, so its light fits them
        if (v.sun_intensity <= 0 && v.ambient <= 0) {
            memcpy(v.sun, wv.sun, sizeof(v.sun));
            v.sun_intensity = wv.sun_intensity;
            v.ambient = wv.ambient;
        }
        for (uint32_t k = v.opaque_first; k < v.opaque_first + v.opaque_count; k++) out.vertices[k].flags |= R3D_WORLD;
        for (uint32_t k = v.trans_first; k < v.trans_first + v.trans_count; k++) out.vertices[k].flags |= R3D_WORLD;
    }
    // The Death Star (its textures, from F9 snapshots of Yavin) in the world
    // and the viewports sharing it. Only in Yavin (top state 2/3) and the last
    // space stage (8/9): the texture words are slots that each stage fills with
    // its own pictures, and in Hoth's ice cave (snowtroopers before the Falcon)
    // 0x04A80200 is its rock, which came out as the Death Star's night side,
    // almost black (issue #23). Unknown (-1: a replay, the attract): as before
    static const bool no_ds = getenv("SWT_PLAIN_DEATH_STAR") != nullptr;
    if (no_ds) return;
    int stage = g_stage;
    if (stage >= 0 && !(stage >= 2 && stage <= 3) && !(stage >= 8 && stage <= 9)) return;
    // The trench (its walls: texture page 0x0278, thousands of triangles; F9
    // snapshots of Yavin's third phase): the game builds it a stretch at a
    // time, the new stretch popping up at the far end. It fades into the dark
    // over the last of the tracked scenery's distance, as the trench in the film
    uint32_t trench = 0;
    R3DViewport* world_vp = nullptr;
    for (R3DViewport& v : vps) {
        if (v.world) world_vp = &v;
        if (!v.world && !v.shares_world) continue;
        for (uint32_t k = v.opaque_first; k < v.opaque_first + v.opaque_count; k++)
            trench += (out.vertices[k].tex >> 16) == 0x0278u;
    }
    static const bool no_fade = getenv("SWT_NO_TRENCH_FADE") != nullptr;
    if (world_vp && !no_fade && trench >= 3 * 500 && world_vp->scenery_far > 200.0f && world_vp->scenery_far < 1e5f)
        world_vp->depth_fade = world_vp->scenery_far;
    for (const R3DViewport& v : vps) {
        if (!v.world && !v.shares_world) continue;
        for (uint32_t k = v.opaque_first; k < v.opaque_first + v.opaque_count; k++) {
            uint32_t t = out.vertices[k].tex;
            if (t == 0x06E00000u || t == 0x04A80200u || t == 0x04E00400u || t == 0x02740240u)
                out.vertices[k].flags |= R3D_DEATH_STAR | R3D_NO_SHADOW;
            else if (t == 0x06E00300u)   // the red planet of Yavin's opening: in fact a sphere just behind the fleet
                out.vertices[k].flags |= R3D_NO_SHADOW;
        }
    }
}

void r3d_widen_screen_fills(R3DScene& out, float wide) {
    if (wide <= 1.001f) return;
    for (const R3DViewport& v : out.viewports) {
        if (v.world || v.shares_world || v.x > 0.5f || v.w < 495.5f) continue;
        for (int pass = 0; pass < 2; pass++) {
            uint32_t first = pass ? v.trans_first : v.opaque_first, count = pass ? v.trans_count : v.opaque_count;
            for (uint32_t t = 0; t + 2 < count; t += 3) {
                R3DVertex* tri = &out.vertices[first + t];
                float b[4];
                if (!screen_bbox(v, tri, b)) continue;
                if (b[0] > v.x + 2 || b[2] < v.x + v.w - 2 || b[1] > v.y + 2 || b[3] < v.y + v.h - 2) continue;
                for (int k = 0; k < 3; k++) tri[k].pos[0] *= wide;
            }
        }
    }
}

void r3d_set_swt_duel(bool on) { g_duel = on; }
void r3d_set_swt_stage(int top_state) { g_stage = top_state; }
int r3d_swt_stage() { return g_stage; }

void r3d_anchor_swt_hud(R3DScene& out) {
    if (out.viewports.empty()) return;
    int top = out.viewports.back().priority;
    // The lightsaber duels draw their top HUD in 3D, in the viewport under the
    // top one (F9 of Boba Fett's duel): SUCCESS / FAILURE at the left (x 0..140),
    // the enemy's bar and name at the right (318..477), all above row 56. The
    // sight's ring is in the top viewport and stays
    if (g_duel) {
        // The fade from black at the duel's start is a dark quad over the whole
        // screen, and the player's arms are drawn after it: they get as dark
        // as the quad covers, so they come out of the black with the rest
        float cover = 0;
        size_t fill_vp = out.viewports.size();
        for (size_t i = 0; i < out.viewports.size() && fill_vp == out.viewports.size(); i++) {
            const R3DViewport& v = out.viewports[i];
            if (v.x > 0.5f || v.w < 495.5f) continue;
            for (int pass = 0; pass < 2; pass++) {
                uint32_t first = pass ? v.trans_first : v.opaque_first, count = pass ? v.trans_count : v.opaque_count;
                for (uint32_t t = 0; t + 2 < count; t += 3) {
                    const R3DVertex* tri = &out.vertices[first + t];
                    float b[4];
                    uint32_t c = tri[0].color;
                    if ((c & 0xFF) > 0x20 || ((c >> 8) & 0xFF) > 0x20 || ((c >> 16) & 0xFF) > 0x20) continue;
                    if (!screen_bbox(v, tri, b) || b[0] > v.x + 2 || b[2] < v.x + v.w - 2 || b[1] > v.y + 2 || b[3] < v.y + v.h - 2)
                        continue;
                    cover = std::max(cover, pass ? float(c >> 24) / 255.0f : 1.0f);
                    fill_vp = i;
                }
            }
        }
        if (cover > 0.01f && fill_vp < out.viewports.size())
            for (size_t i = fill_vp + 1; i < out.viewports.size(); i++) {
                const R3DViewport& v = out.viewports[i];
                if (v.priority >= top - 1) continue;   // the HUD
                auto darken = [&](uint32_t first, uint32_t count) {
                    for (uint32_t k = first; k < first + count; k++) {
                        uint32_t& c = out.vertices[k].color;
                        float f = 1.0f - cover;
                        uint32_t r = uint32_t((c & 0xFF) * f), g = uint32_t(((c >> 8) & 0xFF) * f), bl = uint32_t(((c >> 16) & 0xFF) * f);
                        c = (c & 0xFF000000u) | (bl << 16) | (g << 8) | r;
                    }
                };
                darken(v.opaque_first, v.opaque_count);
                darken(v.trans_first, v.trans_count);
            }
        for (const R3DViewport& v : out.viewports) {
            if (v.priority != top - 1 || v.world || v.shares_world || v.x > 0.5f || v.w < 495.5f) continue;
            for (int pass = 0; pass < 2; pass++) {
                uint32_t first = pass ? v.trans_first : v.opaque_first, count = pass ? v.trans_count : v.opaque_count;
                for (uint32_t t = 0; t + 2 < count; t += 3) {
                    R3DVertex* tri = &out.vertices[first + t];
                    float b[4];
                    if (!screen_bbox(v, tri, b) || b[3] > 60.0f) continue;
                    uint32_t flag = b[2] < 170.0f ? R3D_HUD_LEFT : b[0] > 290.0f ? R3D_HUD_RIGHT : 0;
                    for (int k = 0; k < 3; k++) tri[k].flags |= flag;
                }
            }
        }
    }
    // The Rebel emblem bottom left (its own small viewport, 16,320 48x32) goes
    // to the left edge, beside the dialogue box
    for (R3DViewport& v : out.viewports)
        if (v.priority == top && v.x < 100.0f && v.y >= 280.0f && v.w <= 80.0f && v.h <= 64.0f) v.hud_left = true;
    for (const R3DViewport& v : out.viewports) {
        if (v.priority != top || v.x > 0.5f || v.w < 495.5f) continue;
        for (uint32_t t = 0; t + 2 < v.opaque_count; t += 3) {
            R3DVertex* tri = &out.vertices[v.opaque_first + t];
            float b[4];
            if (-tri[0].pos[2] > 100.0f || !screen_bbox(v, tri, b)) continue;
            if (b[0] >= 380.0f && b[1] >= 260.0f)
                for (int k = 0; k < 3; k++) tri[k].flags |= R3D_HUD_RIGHT | R3D_HUD_LOWER;
        }
    }
}

// Hoth's base hangs its lamps and wires on long, very thin untextured black
// polygons (a cable a pixel or two wide), which break up into flickering
// dark threads across the ceiling. Indoors (a small world) those slivers are
// dropped; the thick cables along the tunnels stay. SWT_KEEP_WIRES keeps them
void r3d_hide_wires(R3DScene& out) {
    static const bool keep = getenv("SWT_KEEP_WIRES") != nullptr;
    if (keep) return;
    bool indoors = false;
    for (const R3DViewport& v : out.viewports)
        if (v.world && !v.space && v.scenery_far > 0 && v.scenery_far < 120.0f) indoors = true;
    if (!indoors) return;
    for (const R3DViewport& v : out.viewports) {
        if (!v.world && !v.shares_world) continue;
        for (uint32_t t = 0; t + 2 < v.opaque_count; t += 3) {
            R3DVertex* tri = &out.vertices[v.opaque_first + t];
            if (tri[0].flags & R3D_TEXTURED) continue;
            uint32_t c = tri[0].color;
            if (((c >> 16) & 0xFF) > 0x30 || ((c >> 8) & 0xFF) > 0x30 || (c & 0xFF) > 0x30) continue;
            // Thinness in view space: twice the area over the longest edge squared
            const float *a = tri[0].pos, *b = tri[1].pos, *d = tri[2].pos;
            float e1[3], e2[3], e3[3];
            for (int k = 0; k < 3; k++) e1[k] = b[k] - a[k], e2[k] = d[k] - a[k], e3[k] = d[k] - b[k];
            float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
            float area2 = std::sqrt(cx * cx + cy * cy + cz * cz);
            auto len2 = [](const float* e) { return e[0] * e[0] + e[1] * e[1] + e[2] * e[2]; };
            float longest = std::max(len2(e1), std::max(len2(e2), len2(e3)));
            if (longest <= 0 || area2 > 0.02f * longest) continue;   // a sliver: width under 2% of its length
            for (int k = 1; k < 3; k++) memcpy(tri[k].pos, tri[0].pos, sizeof(tri[0].pos));
        }
    }
}

bool r3d_hide_sky_dome(R3DScene& out) {
    bool found = false;
    for (const R3DViewport& v : out.viewports) {
        if (!v.world || v.space) continue;
        for (uint32_t t = 0; t + 2 < v.opaque_count; t += 3) {
            R3DVertex* tri = &out.vertices[v.opaque_first + t];
            if (tri[0].tex != 0x06C00200u && tri[0].tex != 0x06C00300u) continue;
            for (int k = 1; k < 3; k++) memcpy(tri[k].pos, tri[0].pos, sizeof(tri[0].pos));
            found = true;
        }
    }
    return found;
}

// Glowing things as lights, one per model drawn (a lightsabre's blade, a
// laser bolt, an explosion): its luminous polygons of a strong colour (not
// the white core: the glow round it), or the luminous textured ones of an
// explosion's fireball. The long axis of the model's glowing part is a
// segment of light (a blade, a bolt; short for a fireball). Only in the
// world's viewports, and in a duel Luke's arms over it, never the HUD (the
// sight is a red ring). SWT_NO_GLOW_LIGHT turns it off
void r3d_find_glows(R3DScene& out, const std::vector<std::vector<R3DInstance>>& instances) {
    static const bool off = getenv("SWT_NO_GLOW_LIGHT") != nullptr;
    if (off || out.viewports.empty()) return;
    int low = out.viewports.front().priority;
    struct Found { R3DScene::Glow g; float depth; };
    std::vector<Found> found;
    std::vector<uint32_t> verts;
    for (const R3DViewport& v : out.viewports) {
        if (v.build_index >= instances.size()) continue;
        bool world = v.world || v.shares_world;
        if (!world && !(g_duel && v.priority <= low + 1 && v.x <= 0.5f && v.w >= 495.5f)) continue;
        for (const R3DInstance& in : instances[v.build_index]) {
            verts.clear();
            double rgb[3] = {0, 0, 0};
            bool fire = false;
            auto scan = [&](uint32_t first, uint32_t from, uint32_t to) {
                for (uint32_t i = from; i < to; i++) {
                    uint32_t k = first + i;
                    if (k >= out.vertices.size()) return;
                    const R3DVertex& x = out.vertices[k];
                    if (!(x.flags & R3D_LUMINOUS)) continue;
                    uint32_t c = x.color;
                    float r = float(c & 0xFF), g = float((c >> 8) & 0xFF), b = float((c >> 16) & 0xFF);
                    float hi = std::max(r, std::max(g, b)), lo = std::min(r, std::min(g, b));
                    if (x.flags & R3D_TEXTURED) {
                        // an explosion's fireball: see-through luminous sprites
                        // (tinted: white sprites are Endor's light through the trees)
                        if (!(x.flags & R3D_TEX_ALPHA) || hi < 150 || lo > 0.6f * hi) continue;
                        fire = true;
                    } else if (hi < 150 || lo > 0.4f * hi) {
                        continue;   // not a strong colour (the white core)
                    }
                    verts.push_back(k);
                    rgb[0] += r, rgb[1] += g, rgb[2] += b;
                }
            };
            scan(v.trans_first, in.trans[0], in.trans[1]);
            if (verts.size() < 6) continue;
            double m[3] = {0, 0, 0};
            for (uint32_t k : verts)
                for (int a = 0; a < 3; a++) m[a] += out.vertices[k].pos[a];
            for (int a = 0; a < 3; a++) m[a] /= double(verts.size());
            if (m[2] > -0.05) continue;   // behind the eye
            double cov[3][3] = {};
            for (uint32_t k : verts) {
                double d[3];
                for (int a = 0; a < 3; a++) d[a] = out.vertices[k].pos[a] - m[a];
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) cov[i][j] += d[i] * d[j];
            }
            double ax[3] = {0.3, 0.9, 0.2};   // the long axis (power iteration)
            for (int it = 0; it < 12; it++) {
                double nx[3];
                for (int i = 0; i < 3; i++) nx[i] = cov[i][0] * ax[0] + cov[i][1] * ax[1] + cov[i][2] * ax[2];
                double len = std::sqrt(nx[0] * nx[0] + nx[1] * nx[1] + nx[2] * nx[2]);
                if (len < 1e-12) break;
                for (int i = 0; i < 3; i++) ax[i] = nx[i] / len;
            }
            double t0 = 1e30, t1 = -1e30, side = 0;
            for (uint32_t k : verts) {
                double d[3], t = 0;
                for (int a = 0; a < 3; a++) d[a] = out.vertices[k].pos[a] - m[a], t += d[a] * ax[a];
                t0 = std::min(t0, t), t1 = std::max(t1, t);
                double s2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - t * t;
                side = std::max(side, s2);
            }
            double len = t1 - t0, width = 2 * std::sqrt(std::max(side, 0.0));
            if (!(len > 1e-4)) continue;
            // A glow as big as its distance is not a fireball but a halo (the
            // red planet's of Yavin's opening, reaching behind the eye)
            double far = -m[2];
            bool behind = false;
            for (uint32_t k : verts) behind |= out.vertices[k].pos[2] > -0.01f;
            if (behind || std::max(len, width) > (fire ? 0.35 : 1.5) * far) continue;
            // A blade or a bolt is long and thin: the small coloured lights of
            // Vader's chest box and the panels of the room are not lights
            if (!fire && len < 4.0 * width) continue;
            Found f{};
            R3DScene::Glow& g = f.g;
            if (fire) {   // a ball of light at its middle, warm whatever the sprite's tint
                for (int a = 0; a < 3; a++) g.a[a] = g.b[a] = float(m[a]);
                g.color[0] = 1.0f, g.color[1] = 0.55f, g.color[2] = 0.22f;
                g.radius = float(std::max(len, width)) * 0.8f;
                g.strength = 0.7f;
            } else {
                for (int a = 0; a < 3; a++) g.a[a] = float(m[a] + ax[a] * t0), g.b[a] = float(m[a] + ax[a] * t1);
                double n = double(verts.size());
                float hi = float(std::max(rgb[0], std::max(rgb[1], rgb[2])) / n);
                for (int a = 0; a < 3; a++) g.color[a] = float(rgb[a] / n) / std::max(hi, 1.0f);
                g.radius = float(len) * 0.4f;
                g.strength = 0.45f;
            }
            f.depth = float(-m[2]);
            found.push_back(f);
        }
    }
    // The nearest ones, as many as the shader takes
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.depth < b.depth; });
    for (size_t i = 0; i < found.size() && int(out.glows.size()) < R3DScene::MAX_GLOWS; i++) out.glows.push_back(found[i].g);
}
