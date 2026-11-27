// The sight's hooks (tools/recomp.py, ppc_rt.h; moved from runtime.cpp): the
// lever's aim widened for widescreen and made finer than the game's steps
#define WIN32_LEAN_AND_MEAN   // runtime.h brings windows.h: no min/max macros (std::max)
#define NOMINMAX
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "env_switches.h"
#include "inputs.h"
#include "ppc_rt.h"
#include "runtime.h"

// SWT_NARROW_AIM and SWT_LEVER_STEPS, read once
struct AimSwitches {
    bool narrow = swt_env_on("SWT_NARROW_AIM"), steps = swt_env_on("SWT_LEVER_STEPS");
};
static const AimSwitches& aim_switches() {
    static const AimSwitches s;
    return s;
}

// The lever's position between the game's steps, in steps (signed the game's
// way): the game saw `step`; k is the step's direction against the lever's,
// as last seen, kept up to date here
static double between_steps(int axis, int step, double& k) {
    int raw = axis ? g_inputs.p[0].gun_y : g_inputs.p[0].gun_x;
    if (step != 0 && raw != 128) k = ((step > 0) == (raw > 128)) ? 1.0 : -1.0;
    return k * lever_fraction(axis);
}

// The game turns the lever's x into the sight's yaw (fn_0007C894: x * 28,
// 0x10000 = 360 degrees, about +-19.5 degrees at full tilt: the 4:3 screen).
// With the view widened for widescreen the yaw is widened too, keeping the
// sight at the same fraction of the (wider) screen: tan(yaw) * the factor.
// SWT_NARROW_AIM keeps the original travel. The lever's 256 steps are also
// filled in (each one was a visible jump of the sight with a pad or mouse):
// the game's step (its table maps the lever about 1:1, inverted or not) moves
// on by the fraction it did not see. SWT_LEVER_STEPS keeps the steps
double sight_reach(int axis) {
    const double half = axis ? 0.125 : 0.097;   // the sight's half size over the screen's (F9 of Yavin)
    double k = std::clamp(double(g_sight_scale.load()), 0.3, 1.0);
    return (1.0 - k * half) / (1.0 - half);
}

uint32_t rt_aim_angle(int axis, uint32_t angle) {
    const int mul = axis ? 21 : 28;
    double a = double(int32_t(angle));
    if (!aim_switches().steps) {
        static double k[2] = {-1.0, -1.0};   // the lever's direction (inverted by default), as last seen
        a += between_steps(axis, int32_t(angle) / mul, k[axis]) * mul;   // (the game's signed step, -128..127)
    }
    float w = std::clamp(g_wide_applied.load(), 1.0f, 3.0f);
    // The full tilt puts the sight's edge on the screen's (its half size is
    // about a tenth of the half width, an eighth of the half height): drawn
    // smaller (SIGHT SIZE), it goes that much farther so it still gets there
    double reach = sight_reach(axis);
    if (aim_switches().narrow) w = 1.0f;
    if (w > 1.001f || reach > 1.0001) {
        constexpr double UNIT = 6.283185307179586 / 65536.0;
        a = std::atan(std::tan(a * UNIT) * (axis == 0 ? w : 1.0f) * reach) / UNIT;
    }
    return uint32_t(int32_t(std::lround(a)));
}

// On the ground (Hoth, Endor) the sight is a position in front of the camera:
// fn_00053F78 turns the lever (negated x, y; 0x2260E0) into x * 0.647 and
// y * 0.490 (constants at 0x1B6CF8). x is widened like the view, so the
// sight reaches the sides of a widescreen screen (SWT_NARROW_AIM keeps it),
// and both get the lever's precision between steps (SWT_LEVER_STEPS keeps the steps).
// k: the step's direction against the lever's, as last seen (per hook)
static double sight_position(int axis, double v, double mul, double (&k)[2]) {
    if (!aim_switches().steps && mul != 0)
        v += between_steps(axis, int(std::lround(v / mul)), k[axis]) * mul;   // (the game's signed step)
    float w = std::clamp(g_wide_applied.load(), 1.0f, 3.0f);
    if (axis == 0 && !aim_switches().narrow) v *= w;
    return v * sight_reach(axis);
}

double rt_ground_aim(int axis, double v) {
    static double k[2] = {-1.0, 1.0};
    return sight_position(axis, v, f2d(rd32(axis ? 0x1B6CFC : 0x1B6CF8)), k);
}

// In Hoth's base (the tunnels and the hangar) fn_000E2998 places the sight
// instead, from the lever's bytes (x negated) times its own multipliers
// (244.8 and 188.8 / 128): the same widening, or the sight kept to the 4:3
// middle of a widescreen view (issue #150)
double rt_base_aim(int axis, double v, double mul) {
    static double k[2] = {-1.0, 1.0};
    return sight_position(axis, v, mul, k);
}
