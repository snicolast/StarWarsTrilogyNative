// Keyboard, mouse and gamepads, and where they aim (moved from app.cpp)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "gamepad.h"
#include "inputs.h"
#include "options.h"
#include "mouse_confine.h"
#include "runtime.h"
#include "../video/r3d_renderer.h"
#include "../video/r3d_scene.h"
#include "../video/tilegen.h"

#include "app_internal.h"
#include "ppc_rt.h"
#include "env_switches.h"

// Keyboard and mouse (only while the window is active):
//   5 = coin, 1 = start, mouse = the lever (left button fires, right is the
//   second trigger), 6 / 2 = the second coin slot and start, F2 = test, F3 = service
bool g_menu_open = false;     // a menu has the controls
bool g_block_buttons = false; // after a menu closes: buttons ignored until released
static int g_p1_pad = -1;            // the gamepad aiming for player 1; -1: the mouse (whichever moved last)

bool g_pointer_sight = true;   // Star Wars Trilogy: the game's sight goes where the mouse / light gun points
static const bool g_narrow_aim = swt_env_on("SWT_NARROW_AIM");   // the lever over the 4:3 area only
void pointer_sight(float gx, float gy, float& fx, float& fy);

// Window pixels -> fraction of the 4:3 game screen (accounting for the black bars)
static void aim_at(HWND hwnd, POINT pt) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    ScreenLayout lay = compute_layout(float(rc.right), float(rc.bottom), g_opt.max_aspect);
    if (g_pointer_sight && !g_narrow_aim) {
        // The lever is set so that the game's sight lands under the pointer
        // (a light gun such as a Sinden shoots where it points)
        float fx, fy;
        pointer_sight((pt.x - lay.ox) / lay.scale - lay.game_x0, (pt.y - lay.oy) / lay.scale, fx, fy);
        set_player_aim(0, fx, fy);
        return;
    }
    // The lever's whole travel spans the whole (wider) view once the sight's
    // yaw is widened (rt_aim_yaw); SWT_NARROW_AIM: the original 4:3 area only
    float x = g_narrow_aim ? (pt.x - lay.ox - lay.game_x0 * lay.scale) / (SCREEN_W * lay.scale)
                           : (pt.x - lay.ox) / (lay.view_w * lay.scale);
    float y = (pt.y - lay.oy) / (SCREEN_H * lay.scale);
    set_player_aim(0, x, y);
}

void poll_keyboard_mouse(HWND hwnd) {
    // Scripted input (--auto) takes over the controls
    if (GetForegroundWindow() != hwnd || !g_auto.events.empty() || g_arcade_entry) {
        if (g_auto.events.empty() && !g_arcade_entry)   // in the background: no gun stays held
            for (PlayerInputs& pl : g_inputs.p) pl.trigger = pl.offscreen = pl.event = false;
        mouse_confine::update(hwnd, false);
        return;
    }
    mouse_confine::update(hwnd, g_pointer_sight && g_p1_pad < 0 && !g_menu_open);
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (g_menu_open) {
        g_inputs.test = g_inputs.service = false;
        for (PlayerInputs& pl : g_inputs.p) pl.coin = pl.start = pl.trigger = pl.offscreen = pl.event = false;
        return;
    }
    PlayerInputs& p1 = g_inputs.p[0];
    p1.coin = down('5');
    p1.start = down('1');
    g_inputs.p[1].coin = down('6');
    g_inputs.p[1].start = down('2');
    g_inputs.test = down(VK_F2);
    g_inputs.service = down(VK_F3);
    POINT cursor;
    GetCursorPos(&cursor);
    static POINT last_cursor{-100000, -100000};
    if (cursor.x != last_cursor.x || cursor.y != last_cursor.y) g_p1_pad = -1;   // the mouse moved: it aims again
    last_cursor = cursor;
    p1.trigger = down(VK_LBUTTON);
    p1.offscreen = down(VK_RBUTTON);
    g_inputs.p[1].trigger = g_inputs.p[1].offscreen = false;   // a second pad adds its buttons after this
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    if (g_p1_pad < 0) aim_at(hwnd, pt);
    // Arrow keys: the lever at full tilt; Space / Ctrl are the triggers, E or
    // the middle mouse button the EVENT button
    p1.event = down('E') || down(VK_MBUTTON);
    int kx = down(VK_RIGHT) - down(VK_LEFT), ky = down(VK_DOWN) - down(VK_UP);
    if (kx || ky) set_player_aim(0, 0.5f + 0.5f * kx, 0.5f + 0.5f * ky);
    if (down(VK_SPACE)) p1.trigger = true;
    if (down(VK_CONTROL)) p1.offscreen = true;
    if (g_block_buttons) {   // the click that closed the menu does not fire
        bool held = false;
        for (PlayerInputs& pl : g_inputs.p) held |= pl.trigger || pl.offscreen;
        if (held) for (PlayerInputs& pl : g_inputs.p) pl.trigger = pl.offscreen = pl.event = false;
        else g_block_buttons = false;
    }
}

// Gamepads (gamepad.h), after the keyboard and mouse: their buttons are added
// to those of the player pad_player() gives them. The first pad shares player
// 1 with the mouse, and whichever moved last aims. START joins the game, takes
// player 1's sight from the mouse or, pressed by the pad that plays, pauses
// it; BACK pauses. Returns true when a pad asks for the pause menu.
Gamepads g_pads;
GamepadSettings g_pad_settings;

// The aim assist's targets: a box around each enemy the game placed on
// screen, from the point it stands on up the body, sized with its distance.
// In world units (SWT_AIM_BOX=up,half_width,half_height to try others)
// Star Wars Trilogy: the targets come from the 3D scene. The enemies are
// drawn in their own full-screen viewport (not the world's): the models of
// 100 polygons or more there (TIE fighters, walkers; the small ones are
// lasers, sparks and debris). Screen positions in Model 3 pixels
struct SceneTargets {
    std::vector<std::array<float, 3>> enemies;   // x, y, half size
    float sight_x = 0, sight_y = 0;
    bool sight = false;
    int kind = 0;   // which sight: 0 space (turned by the lever, 1000 units away), 1 the snow's, 2 Hoth base's
    float lever_x = 0.5f, lever_y = 0.5f;   // player 1's aim (lever 0..1) when the frame was made
    uint32_t serial = 0;                    // one more each game frame
};
static std::mutex g_scene_targets_mutex;
static SceneTargets g_scene_targets;

// The lever's curve: set_player_aim's 0..255 goes through the game's
// calibration (struct 0x221520: x max +0x10, min +0x12, centre +0x14; y the
// odd bytes) and its table (pointer at +0x08 / +0x0C, one 256-byte table of
// signed steps in ROM at 0x1884A8 for both axes: a dead zone of 11 in the
// middle, gentle near it and at the ends, steeper between), and the sight
// moves with the step. Here: the step for each lever value, negated so it
// grows with the lever (-127..128); false while the game has not set it up
static bool lever_steps(int axis, float (&out)[256]) {
    const uint32_t cal = 0x221520;
    uint32_t table = rd32(cal + (axis ? 0x0C : 0x08));
    if (table == 0 || table > RAM_SIZE - 256) return false;
    int hi = rd8(cal + 0x10 + axis), lo = rd8(cal + 0x12 + axis), c = rd8(cal + 0x14 + axis);
    if (!(lo < c && c < hi)) return false;
    for (int v = 0; v < 256; v++) {   // fn_00004120
        int idx = v < c ? (std::max(v, lo) - lo) * 127 / (c - lo) : (std::min(v, hi) - c) * 127 / (hi - c) + 128;
        out[v] = -float(int8_t(rd8(table + uint32_t(idx))));
        if (v > 0 && out[v] < out[v - 1]) return false;   // not the table expected
    }
    return true;
}

// The lever (0..1) that puts the step at `step` (-127..128), the curve read backwards
static float lever_for_step(const float (&steps)[256], float step) {
    if (step <= steps[0]) return 0.0f;
    if (step >= steps[255]) return 1.0f;
    int a = 0, b = 255;   // the last value below the step, the first above it
    while (a < 255 && steps[a + 1] < step) a++;
    while (b > 0 && steps[b - 1] > step) b--;
    float v = b - a <= 1 ? float(a) + (step - steps[a]) / std::max(steps[b] - steps[a], 1e-3f)
              : a < 128 && b > 128 ? 128.0f              // the dead zone: the lever's middle
                                   : 0.5f * float(a + b);   // another flat run: its middle
    return std::clamp(0.5f + (v - 128.0f) / 255.0f, 0.0f, 1.0f);   // set_player_aim's 128 + (x - 0.5) * 255
}

// The pointer at game pixels gx, gy (Model 3 pixels from the 4:3 area's
// corner; beyond 0..496 in the wider view) -> the lever (0..1) that puts the
// game's sight there (a light gun such as a Sinden shoots where it points).
// The model: at full tilt the sight is `reach` pixels from the middle; on
// the ground (Hoth, Endor) it moves with the lever's step, reaching the
// sides of the view (the snow; Hoth base's goes 244.8 / 248 of it, 188.8 /
// 192 down: fn_000E2998's multipliers; measured, within 2 px); in space
// its yaw and pitch do (28 and 21 per step, 0x10000 = 360 degrees) through a
// projection of 22.5 degrees each side (x; y measured: tan 0.314), short of
// the edges (Yavin measured, within 2 px). x spans the widened view (the hooks widen the sight with it).
// What the real sight differs from it (other stages, the camera) is measured
// on the frames it is found, once the pointer holds still (it trails the
// lever), as a gain away from the middle and an offset near it, both kept
// within a share of the reach so a wrong reading cannot throw the sight
// across a wide screen (issue #150: fixed pixel limits, too small at 21:9).
// Without the sight (a cutscene) the model alone aims
void pointer_sight(float gx, float gy, float& fx, float& fy) {
    struct Axis { float gain = 1, bias = 0; };
    static Axis axes[2];
    static int kind = 0;   // the last sight found (SceneTargets::kind)
    static uint32_t serial = 0;
    static double seen_at = 0;
    SceneTargets t;
    {
        std::lock_guard<std::mutex> lock(g_scene_targets_mutex);
        t.sight = g_scene_targets.sight, t.kind = g_scene_targets.kind;
        t.sight_x = g_scene_targets.sight_x, t.sight_y = g_scene_targets.sight_y;
        t.lever_x = g_scene_targets.lever_x, t.lever_y = g_scene_targets.lever_y;
        t.serial = g_scene_targets.serial;
    }
    if (t.sight) kind = t.kind;
    const bool angular = kind == 0;
    const float wide = std::clamp(g_wide_applied.load(), 1.0f, 3.0f);
    constexpr double UNIT = 6.283185307179586 / 65536.0;
    const double tan_max[2] = {std::tan(128 * 28 * UNIT), std::tan(128 * 21 * UNIT)};
    const double tan_view[2] = {0.41421356, 0.314};
    float steps[2][256];
    for (int a = 0; a < 2; a++)
        if (!lever_steps(a, steps[a]))
            for (int v = 0; v < 256; v++) steps[a][v] = float(v - 128);   // straight until the game sets its table up
    // The sight's distance from the middle (pixels) at a step, without the
    // measured gain, and its inverse
    auto reach = [&](int a) {
        float r = float((a ? SCREEN_H : SCREEN_W * wide) / 2 * sight_reach(a));
        if (kind == 2) r *= a ? 188.8f / 192 : 244.8f / 248;
        return angular ? float(r * tan_max[a] / tan_view[a]) : r;
    };
    auto offset_at = [&](int a, float step) {
        float u = std::clamp(step / 128.0f, -1.0f, 1.0f);
        return reach(a) * (angular ? float(std::tan(u * std::atan(tan_max[a])) / tan_max[a]) : u);
    };
    auto step_for = [&](int a, float offset) {
        float u = std::clamp(offset / reach(a), -1.0f, 1.0f);
        return 128.0f * (angular ? float(std::atan(u * tan_max[a]) / std::atan(tan_max[a])) : u);
    };
    auto step_at = [&](int a, float lever) {   // the step the lever gave
        float v = std::clamp(128 + (lever - 0.5f) * 255, 0.0f, 255.0f);
        int i = std::min(int(v), 254);
        return steps[a][i] + (v - i) * (steps[a][i + 1] - steps[a][i]);
    };
    double now = qpc_seconds();
    static float last_fx = -1, last_fy = -1;
    static double still_since = 0;
    if (t.serial != serial) {
        serial = t.serial;
        if (t.sight && now - still_since > 0.25) {
            const float lever[2] = {t.lever_x, t.lever_y}, seen[2] = {t.sight_x - SCREEN_W / 2, t.sight_y - SCREEN_H / 2};
            for (int a = 0; a < 2; a++) {
                Axis& ax = axes[a];
                float r = reach(a), model = offset_at(a, step_at(a, lever[a]));
                float e = seen[a] - (ax.bias + ax.gain * model);
                // Small steps, a dead band (the lever also moves the ship, in
                // the trench the whole view: a correction that never rests
                // would shake the picture), readings far off left out
                if (std::fabs(e) < 2.0f || std::fabs(e) > 0.2f * r) continue;
                if (std::fabs(model) > 0.3f * r)
                    ax.gain = std::clamp(ax.gain + 0.12f * e / model, 0.8f, 1.25f);
                else
                    ax.bias = std::clamp(ax.bias + 0.12f * e, -0.08f * r, 0.08f * r);
            }
            seen_at = now;
        } else if (!t.sight && now - seen_at > 1.0) {   // no sight for a while: back to the model
            for (Axis& ax : axes) ax.bias *= 0.95f, ax.gain += (1.0f - ax.gain) * 0.05f;
        }
    }
    const float want[2] = {gx - SCREEN_W / 2, gy - SCREEN_H / 2};
    float f[2];
    for (int a = 0; a < 2; a++) f[a] = lever_for_step(steps[a], step_for(a, (want[a] - axes[a].bias) / axes[a].gain));
    fx = f[0], fy = f[1];
    if (std::fabs(fx - last_fx) > 0.004f || std::fabs(fy - last_fy) > 0.004f) {
        still_since = now;
        last_fx = fx, last_fy = fy;
    }
    static const bool log = swt_env_on("SWT_GUN_LOG");
    static double logged = 0;
    if (log && now - logged > 0.25) {
        logged = now;
        printf("[gun] pointer %.1f %.1f  sight %s%s %.1f %.1f  gain %.3f %.3f  bias %.1f %.1f  lever %.3f %.3f\n", gx, gy,
               t.sight ? "yes" : "no", angular ? " (space)" : "", t.sight_x, t.sight_y, axes[0].gain, axes[1].gain, axes[0].bias,
               axes[1].bias, fx, fy);
    }
}

void collect_scene_targets(const R3DSceneBuilder& builder, R3DScene& scene) {
    static float radius = swt_env("SWT_AIM_RADIUS") ? float(atof(swt_env("SWT_AIM_RADIUS"))) : 5.0f;   // world units
    static const bool space_only = swt_env_on("SWT_SPACE_AIM_ONLY");
    static std::vector<uint32_t> sight_models;   // the models seen as the sight (its parts, each stage's)
    struct SightPart { uint32_t model; float x, y; const R3DViewport* vp; };
    std::vector<SightPart> sight_parts;   // this frame's models where the sight is drawn
    const R3DViewport* sight_vp = nullptr;
    SceneTargets t;
    int top = scene.viewports.empty() ? 0 : scene.viewports.back().priority;
    int low = scene.viewports.empty() ? 0 : scene.viewports.front().priority;
    const R3DViewport* world = nullptr;
    for (const R3DViewport& v : scene.viewports)
        if (v.world) world = &v;
    bool space = world && world->space;
    // On the ground (Hoth) the world viewport holds the terrain, and the
    // walkers are drawn in another viewport sharing its priority and
    // projection: each model of 100 polygons or more there is a target, boxed
    // by its own vertices on screen (the walkers are tall: their origin is at
    // their feet). Models that cover half the screen or reach behind the
    // camera are terrain pieces
    bool ground = world && !world->space && !space_only;
    auto project = [](const R3DViewport& v, const float* p, float& sx, float& sy) {
        const float* m = v.proj;
        float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        if (cw < 1.0f) return false;
        sx = v.x + ((m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / cw + 1) / 2 * v.w;
        sy = v.y + ((m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / cw + 1) / 2 * v.h;
        return true;
    };
    for (const R3DViewport& v : scene.viewports) {
        if (v.x > 0.5f || v.w < SCREEN_W - 0.5f || v.build_index >= builder.instances().size()) continue;
        const float* m = v.proj;
        bool ground_targets = ground && v.shares_world && !v.world && v.priority == world->priority;
        for (const R3DInstance& in : builder.instances()[v.build_index]) {
            const float* p = in.origin;
            float cw = m[11] * p[2];   // = -z
            if (cw < 1.0f) continue;
            float sx = v.x + ((m[0] * p[0] + m[8] * p[2]) / cw + 1) / 2 * v.w;
            float sy = v.y + ((m[5] * p[1] + m[9] * p[2]) / cw + 1) / 2 * v.h;
            // The game's sight: 1000 units from the camera in the top
            // viewport in space (turned with the lever's yaw and pitch: its
            // depth alone falls short of 1000 away from the middle), 200
            // units ahead in a viewport under the HUD on the snow, 128 ahead
            // in the top viewport in Hoth's base (moved sideways there)
            // (never in the world or beside it: a piece of the Death Star's
            // trench 1000 units ahead was once taken for it)
            auto at = [&](float d, float tol) { return std::fabs(p[2] + d) < tol; };
            const float dist = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            auto sight_depth = [&](float tol) {
                return (p[2] < 0 && std::fabs(dist - 1000) < tol) || (ground && (at(200, tol) || at(128, tol)));
            };
            // (its other looks too, near it in its viewport: when a shot hits,
            // yellow and red rings are drawn with it, made smaller as well)
            bool hud = v.priority > low && !v.world && !v.shares_world;
            if (hud && in.polygons >= 4 && sight_depth(40.0f)) sight_parts.push_back({in.model, sx, sy, &v});
            if (hud && in.polygons >= 30 && !t.sight && sight_depth(2.0f)) {
                t.sight_x = sx, t.sight_y = sy, t.sight = true;
                t.kind = std::fabs(dist - 1000) < 2.0f ? 0 : at(128, 2.0f) ? 2 : 1;
                sight_vp = &v;
            } else if (space && !(v.world && !v.space) && v.priority < top && in.polygons >= 100 && sx > -SCREEN_W && sx < 2 * SCREEN_W &&
                       sy > -SCREEN_H && sy < 2 * SCREEN_H) {
                t.enemies.push_back({sx, sy, radius * std::fabs(m[0]) * v.w / 2 / cw});
            } else if (ground_targets && in.polygons >= 100) {
                float b[4] = {1e9f, 1e9f, -1e9f, -1e9f};
                bool ok = in.opaque[1] > in.opaque[0];
                for (uint32_t i = in.opaque[0]; ok && i < in.opaque[1]; i++) {
                    float x, y;
                    if (!project(v, scene.vertices[v.opaque_first + i].pos, x, y)) ok = false;
                    else b[0] = std::min(b[0], x), b[1] = std::min(b[1], y), b[2] = std::max(b[2], x), b[3] = std::max(b[3], y);
                }
                if (!ok || b[2] - b[0] > 0.5f * SCREEN_W || b[3] - b[1] > 0.5f * SCREEN_H || b[2] < 0 || b[0] > SCREEN_W ||
                    b[3] < 0 || b[1] > SCREEN_H)
                    continue;
                // the body, not the legs: the upper part of the box
                float cx = (b[0] + b[2]) / 2, cy = b[1] + (b[3] - b[1]) * 0.35f;
                t.enemies.push_back({cx, cy, std::max(b[2] - b[0], b[3] - b[1]) * 0.3f});
            }
        }
    }
    if (!t.sight && !space) t.enemies.clear();   // without the sight the walkers cannot be placed
    // The models of the sight learned: those round the sight, on screen (not
    // a stray model). When a shot hits, the sight is drawn bigger and a little
    // farther (not found then): round where it was a moment before
    static float last_x = 0, last_y = 0;
    static int last_prio = -1;
    static uint64_t last_seen = 0, frames = 0;
    frames++;
    if (t.sight) last_x = t.sight_x, last_y = t.sight_y, last_prio = sight_vp->priority, last_seen = frames;
    bool near_known = t.sight || (last_prio >= 0 && frames - last_seen < 60);
    for (const SightPart& sp : sight_parts) {
        if (!near_known || sp.vp->priority != last_prio || std::fabs(sp.x - last_x) > 100 || std::fabs(sp.y - last_y) > 100) continue;
        if (sp.x < -SCREEN_W * 0.5f || sp.x > SCREEN_W * 1.5f || sp.y < 0 || sp.y > SCREEN_H) continue;
        if (std::find(sight_models.begin(), sight_models.end(), sp.model) != sight_models.end()) continue;
        if (sight_models.size() >= 24) sight_models.erase(sight_models.begin());
        sight_models.push_back(sp.model);
    }
    // The sight drawn smaller (or bigger) round its centre: every copy of the
    // models found as the sight (also in a frame where it is not found, or
    // it would flick between the two sizes)
    float k = g_sight_scale;
    if (std::fabs(k - 1.0f) > 0.01f && !sight_models.empty())
        for (const R3DViewport& v : scene.viewports) {
            if (v.priority <= low || v.world || v.shares_world || v.build_index >= builder.instances().size()) continue;
            for (const R3DInstance& in : builder.instances()[v.build_index]) {
                if (std::find(sight_models.begin(), sight_models.end(), in.model) == sight_models.end()) continue;
                // Round the middle of its own vertices (not the model's origin:
                // a part placed from the middle of the screen would be pulled
                // in towards it, and the sight not reach the sides), and the
                // previous frame's place too (where the images between two game
                // frames are drawn from), or it pulsed between the two sizes
                auto shrink = [&](uint32_t first, uint32_t from, uint32_t to) {
                    double c[3] = {0, 0, 0}, cp[3] = {0, 0, 0};
                    uint32_t n = 0;
                    for (uint32_t i = from; i < to && first + i < scene.vertices.size(); i++, n++)
                        for (int a = 0; a < 3; a++) c[a] += scene.vertices[first + i].pos[a], cp[a] += scene.vertices[first + i].prev[a];
                    if (!n) return;
                    for (uint32_t i = from; i < to && first + i < scene.vertices.size(); i++) {
                        R3DVertex& vx = scene.vertices[first + i];
                        for (int a = 0; a < 3; a++) {
                            float m = float(c[a] / n), mp = float(cp[a] / n);
                            vx.pos[a] = m + (vx.pos[a] - m) * k;
                            vx.prev[a] = mp + (vx.prev[a] - mp) * k;
                        }
                    }
                };
                shrink(v.opaque_first, in.opaque[0], in.opaque[1]);
                shrink(v.trans_first, in.trans[0], in.trans[1]);
            }
        }
    PlayerAim aim = player_aim(0);
    t.lever_x = aim.x, t.lever_y = aim.y;
    std::lock_guard<std::mutex> lock(g_scene_targets_mutex);
    t.serial = g_scene_targets.serial + 1;
    g_scene_targets = std::move(t);
}

static std::vector<AimTarget> aim_targets() {
    // The pad moves the lever (aim 0..1), and the lever moves the game's
    // sight over about 80% of the screen: each enemy is placed where the
    // aim has to go for the sight to reach it
    constexpr float K = 1.2f;
    SceneTargets t;
    {
        std::lock_guard<std::mutex> lock(g_scene_targets_mutex);
        t = g_scene_targets;
    }
    // The lever's x spans the wider view in widescreen (rt_aim_yaw)
    const float kx = K / (g_narrow_aim ? 1.0f : std::clamp(g_wide_applied.load(), 1.0f, 3.0f));
    PlayerAim a = player_aim(0);
    float ax = a.x * SCREEN_W, ay = a.y * SCREEN_H;
    float sx = t.sight ? t.sight_x : ax, sy = t.sight ? t.sight_y : ay;
    std::vector<AimTarget> out;
    for (const auto& e : t.enemies) {
        float x = ax + (e[0] - sx) * kx, y = ay + (e[1] - sy) * K, h = std::max(e[2], 4.0f);
        out.push_back({(x - h * kx) / SCREEN_W, (y - h * K) / SCREEN_H, (x + h * kx) / SCREEN_W, (y + h * K) / SCREEN_H});
    }
    return out;
}

bool poll_gamepads(HWND hwnd) {
    bool focused = GetForegroundWindow() == hwnd;
    g_pads.set_targets(aim_targets());
    g_pads.poll(g_pad_settings, focused, g_menu_open);
    static bool blocked[Gamepads::MAX] = {}, fire_was[Gamepads::MAX] = {}, reload_was[Gamepads::MAX] = {};
    uint32_t connected = 0;
    for (int i = 0; i < Gamepads::MAX; i++)
        if (g_pads.pad(i).connected) connected |= 1u << i;
    if (g_p1_pad >= 0 && !(connected >> g_p1_pad & 1)) g_p1_pad = -1;   // unplugged: the mouse aims again
    if (g_menu_open) std::fill(std::begin(blocked), std::end(blocked), true);   // the button that closes it does not fire
    if (!focused || g_menu_open || !g_auto.events.empty()) return false;
    bool pause = false;
    for (int i = 0; i < Gamepads::MAX; i++) {
        const PadState& pad = g_pads.pad(i);
        if (!pad.connected) continue;
        int p = pad_player(i, connected);
        PlayerInputs& pl = g_inputs.p[p];
        if (pad.aimed && p == 0) g_p1_pad = i;
        if (p == 1 || g_p1_pad == i) set_player_aim(p, pad.aim_x, pad.aim_y);
        else {   // the mouse aims: the pad's sight waits where the mouse is
            PlayerAim a = player_aim(0);
            g_pads.set_aim(i, a.x, a.y);
        }
        bool fire = pad.fire, reload = pad.reload;
        if (blocked[i]) {
            if (fire || reload) fire = reload = false;
            else blocked[i] = false;
        }
        if (fire) pl.trigger = true;
        if (reload) pl.offscreen = true;
        if (pad.event && !blocked[i]) pl.event = true;
        if (fire && !fire_was[i]) {
            g_pads.rumble(i, 0.2f, 0.5f, 70);
            g_pads.rumble_triggers(i, 0.0f, 1.0f, 90);
        } else if (reload && !reload_was[i]) {
            g_pads.rumble(i, 0.35f, 0.0f, 50);
            g_pads.rumble_triggers(i, 0.8f, 0.0f, 70);
        }
        fire_was[i] = fire, reload_was[i] = reload;
        // The lever's force feedback (game_flow.cpp ffb_frame): the jolts
        // on the pad that steers (SWT_NO_FFB_RUMBLE: none)
        static const bool no_ffb = swt_env_on("SWT_NO_FFB_RUMBLE");
        if (g_p1_pad == i && !no_ffb) {
            float s = std::min(1.0f, g_ffb_shake.exchange(0.0f) * 2.5f);
            if (s > 0.08f) g_pads.rumble(i, s, 0.6f * s, 60);
        }
        // One player: BACK is the coin. START starts the game after a coin
        // (within 30 s, once per coin) and otherwise opens the pause menu
        static double coin_at = -1;
        if (pad.back) {
            press_coin();
            coin_at = qpc_seconds();
        }
        if (pad.start) {
            if (coin_at >= 0 && qpc_seconds() - coin_at < 30) {
                press_start();
                coin_at = -1;
            } else {
                pause = true;
            }
        }
    }
    return pause;
}
