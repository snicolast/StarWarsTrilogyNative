// Keyboard, mouse and gamepads, and where they aim (moved from app.cpp)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dwmapi.h>     // DwmGetCompositionTimingInfo
#include <mmsystem.h>   // timeBeginPeriod
#include <windowsx.h>   // GET_X_LPARAM

#include "../dogfight/dogfight.h"
#include "gamepad.h"
#include "inputs.h"
#include "options.h"
#include "ppc_rt.h"
#include "raw_mice.h"
#include "rom_zip.h"
#include "runtime.h"
#include "stats.h"
#include "../audio/sound_board.h"
#include "../ui/achievement_toast.h"
#include "../ui/frontend.h"
#include "../ui/menu_music.h"
#include "../video/pack2d.h"
#include "../video/r3d_renderer.h"
#include "../video/r3d_scene.h"
#include "../video/r3d_screen.h"
#include "../video/r3d_state.h"
#include "../video/sky_pack.h"
#include "../video/text_hd.h"
#include "../video/texture_pack.h"
#include "../video/tilegen.h"
#include "../video/ui_overlay.h"
#include "../video/vk_presenter.h"


#include "app_internal.h"

// Keyboard and mouse (only while the window is active):
//   5 = coin, 1 = start, mouse = gun (left button fires, right reloads),
//   6 / 2 = player 2's coin and start, F2 = test, F3 = service
bool g_menu_open = false;     // a menu has the controls
bool g_block_buttons = false; // after a menu closes: buttons ignored until released
static int g_p1_pad = -1;            // the gamepad aiming for player 1; -1: the mouse (whichever moved last)

bool g_pointer_sight = true;   // Star Wars Trilogy: the game's sight goes where the mouse / light gun points
static const bool g_narrow_aim = getenv("SWT_NARROW_AIM") != nullptr;   // the lever over the 4:3 area only
void pointer_sight(float gx, float gy, float& fx, float& fy);

// Window pixels -> fraction of the 4:3 game screen (accounting for the black bars)
static void aim_at(int player, HWND hwnd, POINT pt) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    // The gun covers the original 4:3 area, centred in the (possibly wider) 3D view
    ScreenLayout lay = compute_layout(float(rc.right), float(rc.bottom), g_opt.max_aspect);
    if (kAnalogJoystick && player == 0 && g_pointer_sight && !g_narrow_aim) {
        // The lever is set so that the game's sight lands under the pointer
        // (a light gun such as a Sinden shoots where it points)
        float fx, fy;
        pointer_sight((pt.x - lay.ox) / lay.scale - lay.game_x0, (pt.y - lay.oy) / lay.scale, fx, fy);
        set_player_aim(player, fx, fy);
        return;
    }
    float x = (pt.x - lay.ox - lay.game_x0 * lay.scale) / (SCREEN_W * lay.scale);
    // Star Wars Trilogy's lever: its whole travel spans the whole (wider) view
    // once the sight's yaw is widened (rt_aim_yaw)
    if (kAnalogJoystick && !g_narrow_aim) x = (pt.x - lay.ox) / (lay.view_w * lay.scale);
    float y = (pt.y - lay.oy) / (SCREEN_H * lay.scale);
    set_player_aim(player, x, y);
}

void poll_keyboard_mouse(HWND hwnd) {
    // Scripted input (--auto) takes over the controls
    if (GetForegroundWindow() != hwnd || !g_auto.events.empty() || g_arcade_entry) {
        if (g_auto.events.empty() && !g_arcade_entry)   // in the background: no gun stays held
            for (PlayerInputs& pl : g_inputs.p) pl.trigger = pl.offscreen = pl.event = false;
        raw_mice::update(hwnd, false, false);
        return;
    }
    // A second mouse becomes player 2's when it clicks while player 1 plays alone
    bool join_allowed = g_opt.several_mice && !g_menu_open && g_inputs.p[0].playing && !g_inputs.p[1].playing;
    // (DOGFIGHT too: a click outside the window, on another monitor, took the
    // focus away, and with it the controls)
    raw_mice::confine((kAnalogJoystick && g_pointer_sight && g_p1_pad < 0) || g_dogfight == 2);
    raw_mice::update(hwnd, join_allowed, !g_menu_open);
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
    if (raw_mice::two_mice()) {   // each mouse its own gun; player 2's click joins while player 1 plays
        for (int p = 0; p < 2; p++) {
            raw_mice::Gun g = raw_mice::gun(p, hwnd);
            PlayerInputs& pl = g_inputs.p[p];
            if (p == 1 && g.clicked && !pl.playing && p1.playing) player_join(1);
            pl.trigger = g.trigger;
            pl.offscreen = g.reload;
            if (p == 1 || g_p1_pad < 0) aim_at(p, hwnd, g.pos);
        }
    } else {
        p1.trigger = down(VK_LBUTTON);
        p1.offscreen = down(VK_RBUTTON);
        g_inputs.p[1].trigger = g_inputs.p[1].offscreen = false;   // player 2's pad adds its buttons after this
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (g_p1_pad < 0) aim_at(0, hwnd, pt);
    }
    // Arrow keys: the lever at full tilt; Space / Ctrl are the triggers, E or
    // the middle mouse button the EVENT button
    if (kAnalogJoystick) {
        p1.event = down('E') || down(VK_MBUTTON);
        int kx = down(VK_RIGHT) - down(VK_LEFT), ky = down(VK_DOWN) - down(VK_UP);
        if (kx || ky) set_player_aim(0, 0.5f + 0.5f * kx, 0.5f + 0.5f * ky);
        if (down(VK_SPACE)) p1.trigger = true;
        if (down(VK_CONTROL)) p1.offscreen = true;
    }
    if (g_block_buttons) {   // the click that closed the menu does not fire
        bool held = false;
        for (PlayerInputs& pl : g_inputs.p) held |= pl.trigger || pl.offscreen;
        if (held) for (PlayerInputs& pl : g_inputs.p) pl.trigger = pl.offscreen = pl.event = false;
        else g_block_buttons = false;
    }
    static bool c_was_down = false;   // C toggles the extra crosshair
    bool c_down = down('C');
    if (c_down && !c_was_down) g_opt.show_crosshair = !g_opt.show_crosshair;
    c_was_down = c_down;
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
// In world units (LW_AIM_BOX=up,half_width,half_height to try others)
// Star Wars Trilogy: the targets come from the 3D scene. The enemies are
// drawn in their own full-screen viewport (not the world's): the models of
// 100 polygons or more there (TIE fighters, walkers; the small ones are
// lasers, sparks and debris). Screen positions in Model 3 pixels
struct SceneTargets {
    std::vector<std::array<float, 3>> enemies;   // x, y, half size
    float sight_x = 0, sight_y = 0;
    bool sight = false;
    float lever_x = 0.5f, lever_y = 0.5f;   // player 1's aim (lever 0..1) when the frame was made
    uint32_t serial = 0;                    // one more each game frame
};
static std::mutex g_scene_targets_mutex;
static SceneTargets g_scene_targets;

// The pointer at game pixels gx, gy (Model 3 pixels from the 4:3 area's
// corner; beyond 0..496 in the wider view) -> the lever (0..1) that puts the
// game's sight there. The lever moves the sight over about 1/1.2 of the view
// (the model); what the sight's real place differs from the model's (the
// game's angles, the stage) is measured on each frame the sight is found and
// taken off, so it ends right under the pointer. Without the sight (a
// cutscene, a stage where it is not found) the model alone aims
void pointer_sight(float gx, float gy, float& fx, float& fy) {
    constexpr float K = 1.2f;
    static float bias_x = 0, bias_y = 0;   // the sight's place minus the model's, in game pixels
    static uint32_t serial = 0;
    static double seen_at = 0;
    SceneTargets t;
    {
        std::lock_guard<std::mutex> lock(g_scene_targets_mutex);
        t.sight = g_scene_targets.sight;
        t.sight_x = g_scene_targets.sight_x, t.sight_y = g_scene_targets.sight_y;
        t.lever_x = g_scene_targets.lever_x, t.lever_y = g_scene_targets.lever_y;
        t.serial = g_scene_targets.serial;
    }
    const float wide = std::clamp(g_wide_applied.load(), 1.0f, 3.0f);
    const float span_x = SCREEN_W * wide / K, span_y = SCREEN_H / K;   // the sight's travel over the lever's
    double now = qpc_seconds();
    // The game's sight trails the lever while it moves, so it is measured only
    // once the pointer has held still for a moment, and the correction kept
    // small: a wrong reading (another model taken for the sight) cannot throw
    // the sight across the screen
    static float last_fx = -1, last_fy = -1;
    static double still_since = 0;
    if (t.serial != serial) {
        serial = t.serial;
        if (t.sight && now - still_since > 0.2) {
            float mx = SCREEN_W / 2 + (t.lever_x - 0.5f) * span_x, my = SCREEN_H / 2 + (t.lever_y - 0.5f) * span_y;
            float ex = t.sight_x - mx, ey = t.sight_y - my;
            // A dead band of a few pixels and a slow gain: the lever also moves
            // the ship (in the trench the whole view), so a correction that
            // never rests would shake the picture
            float dx = ex - bias_x, dy = ey - bias_y;
            if (std::fabs(dx) < 40 && std::fabs(dy) < 40) {
                if (std::fabs(dx) > 3) bias_x = std::clamp(bias_x + 0.12f * dx, -32.0f, 32.0f);
                if (std::fabs(dy) > 3) bias_y = std::clamp(bias_y + 0.12f * dy, -32.0f, 32.0f);
            }
            seen_at = now;
        } else if (!t.sight && now - seen_at > 1.0) {   // no sight for a while: back to the model
            bias_x *= 0.95f, bias_y *= 0.95f;
        }
    }
    fx = std::clamp(0.5f + (gx - bias_x - SCREEN_W / 2) / span_x, 0.0f, 1.0f);
    fy = std::clamp(0.5f + (gy - bias_y - SCREEN_H / 2) / span_y, 0.0f, 1.0f);
    if (std::fabs(fx - last_fx) > 0.004f || std::fabs(fy - last_fy) > 0.004f) {
        still_since = now;
        last_fx = fx, last_fy = fy;
    }
    static const bool log = getenv("SWT_GUN_LOG") != nullptr;
    static double logged = 0;
    if (log && now - logged > 0.25) {
        logged = now;
        printf("[gun] pointer %.1f %.1f  sight %s %.1f %.1f  bias %.1f %.1f  lever %.3f %.3f\n", gx, gy, t.sight ? "yes" : "no",
               t.sight_x, t.sight_y, bias_x, bias_y, fx, fy);
    }
}

void collect_scene_targets(const R3DSceneBuilder& builder, R3DScene& scene) {
    static float radius = getenv("SWT_AIM_RADIUS") ? float(atof(getenv("SWT_AIM_RADIUS"))) : 5.0f;   // world units
    static const bool space_only = getenv("SWT_SPACE_AIM_ONLY") != nullptr;
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
            // The game's sight: 1000 units in front of the camera in the top
            // viewport in space, 200 units in a viewport under the HUD on the
            // snow, 128 in the top viewport in Hoth's base
            // (never in the world or beside it: a piece of the Death Star's
            // trench 1000 units ahead was once taken for it)
            auto at = [&](float d, float tol) { return std::fabs(p[2] + d) < tol; };
            auto sight_depth = [&](float tol) { return at(1000, tol) || (ground && (at(200, tol) || at(128, tol))); };
            // (its other looks too, near it in its viewport: when a shot hits,
            // yellow and red rings are drawn with it, made smaller as well)
            bool hud = v.priority > low && !v.world && !v.shares_world;
            if (hud && in.polygons >= 4 && sight_depth(40.0f)) sight_parts.push_back({in.model, sx, sy, &v});
            if (hud && in.polygons >= 30 && !t.sight && sight_depth(2.0f)) {
                t.sight_x = sx, t.sight_y = sy, t.sight = true;
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

// DOGFIGHT's controls: left stick (or a flight stick, the arrow keys, or the
// mouse away from the middle of the window when there is no pad) pitch and
// yaw, right stick's x (the twist, A / D) roll, the shoulder buttons (W / S) faster
// and slower, a flight stick's own lever the throttle itself; the cannons on
// the right trigger or A, a flight stick's trigger, the space bar or the mouse;
// the target lock on Y, a flight stick's second button, T or the right mouse
// button; the lock on the one attacking on X, the stick's third button or R;
// the braking turn on the left trigger, the stick's fourth button or Shift
static void dogfight_controls(bool focused) {
    DogfightControls& c = g_dogfight_controls;
    float pitch = 0, yaw = 0, roll = 0, throttle = 0;
    bool absolute = false, any_pad = false;
    bool fire = false, lock = false, threat = false, drift = false;
    if (focused && !g_menu_open) {
        for (int i = 0; i < Gamepads::MAX; i++) {
            const PadState& pad = g_pads.pad(i);
            if (!pad.connected) continue;
            any_pad = true;
            yaw += pad.fly[0], pitch += pad.fly[1], roll += pad.fly[2];   // (stick back = nose up)
            throttle += pad.fly_triggers[1] - pad.fly_triggers[0];
            if (pad.has_throttle) absolute = true, throttle = pad.fly_throttle;
            fire |= pad.fly_fire, lock |= pad.fly_lock, threat |= pad.fly_threat, drift |= pad.fly_drift;
        }
        if (g_pad_settings.invert & 2) pitch = -pitch;
        auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        float kx = float(key(VK_RIGHT)) - float(key(VK_LEFT)), ky = float(key(VK_DOWN)) - float(key(VK_UP));
        yaw += kx, pitch += ky;
        roll += float(key('D')) - float(key('A'));
        if (!absolute) throttle += float(key('W')) - float(key('S'));
        fire |= key(VK_SPACE) || key(VK_LBUTTON);
        lock |= key('T') || key(VK_RBUTTON);
        threat |= key('R');
        drift |= key(VK_SHIFT);
        if (!any_pad && kx == 0 && ky == 0) {   // the mouse (the lever it moves), past a dead zone round the middle
            auto axis = [](int adc) {
                float v = float(adc - 128) / 127.0f, m = std::fabs(v);
                return m < 0.12f ? 0.0f : std::copysign((m - 0.12f) / 0.88f, v);
            };
            yaw += axis(g_inputs.p[0].gun_x), pitch += axis(g_inputs.p[0].gun_y);
        }
    }
    c.pitch = std::clamp(pitch, -1.0f, 1.0f), c.yaw = std::clamp(yaw, -1.0f, 1.0f), c.roll = std::clamp(roll, -1.0f, 1.0f);
    c.absolute = absolute;
    c.fire = fire;
    c.lock = lock;
    c.lock_threat = threat;
    c.drift = drift;
    c.throttle = absolute ? throttle : std::clamp(throttle, -1.0f, 1.0f);
}

// DOGFIGHT: when the window stops being the one in front while flying, which
// window took its place is written to out/focus_log.txt (its title, class and
// program), with the time. Twice the controls went dead in a flight with
// nothing clicked; this says what took the focus
static void dogfight_focus_log(HWND hwnd, bool focused) {
    static bool was = true;
    static HWND last = nullptr;
    if (focused) {
        if (!was)
            if (FILE* f = fopen("out/focus_log.txt", "a")) {
                fprintf(f, "  back in front\n");
                fclose(f);
            }
        was = true, last = nullptr;
        return;
    }
    HWND front = GetForegroundWindow();
    if (was == false && front == last) return;   // (said already)
    if (!front && was) return;                   // (between two windows: the next poll says which)
    was = false, last = front;
    (void)hwnd;
    char title[200] = "", cls[100] = "", exe[MAX_PATH] = "";
    if (front) {
        GetWindowTextA(front, title, sizeof(title));
        GetClassNameA(front, cls, sizeof(cls));
        DWORD pid = 0;
        GetWindowThreadProcessId(front, &pid);
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            DWORD n = sizeof(exe);
            if (!QueryFullProcessImageNameA(h, 0, exe, &n)) exe[0] = 0;
            CloseHandle(h);
        }
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (FILE* f = fopen("out/focus_log.txt", "a")) {
        fprintf(f, "%02d:%02d:%02d focus lost to: title \"%s\" class \"%s\" program \"%s\"\n", t.wHour, t.wMinute, t.wSecond, title, cls, exe);
        fclose(f);
    }
}

bool poll_gamepads(HWND hwnd) {
    bool focused = GetForegroundWindow() == hwnd;
    if (g_dogfight == 2) dogfight_focus_log(hwnd, focused);
    g_pads.set_targets(aim_targets());
    g_pads.poll(g_pad_settings, focused, g_menu_open);
    if (g_dogfight == 2) dogfight_controls(focused);
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
            g_pads.rumble_triggers(i, 0.0f, 0.6f, 60);
        } else if (reload && !reload_was[i]) {
            g_pads.rumble(i, 0.35f, 0.0f, 50);
        }
        fire_was[i] = fire, reload_was[i] = reload;
        // The lever's force feedback (runtime.cpp ffb_frame): the jolts
        // on the pad that steers (SWT_NO_FFB_RUMBLE: none)
        static const bool no_ffb = getenv("SWT_NO_FFB_RUMBLE") != nullptr;
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
