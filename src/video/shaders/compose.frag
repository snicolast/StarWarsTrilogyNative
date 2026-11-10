#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// Final composition: background 2D layers, 3D image and foreground 2D layers,
// with black bars to preserve the Model 3 aspect ratio, plus an optional
// high-visibility crosshair drawn at the real mouse position.
// The 2D layers are upscaled with xBR (Hyllian's xBR-lv2), which rebuilds
// smooth edges from the pixel art instead of blurring or blocking it.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D scene3d;
layout(set = 0, binding = 1) uniform sampler2D layer2d_front;
layout(set = 0, binding = 2) uniform sampler2D layer2d_back;
#ifdef FEW_IMAGES
// GPUs that take few images per shader (r3d_renderer.cpp): 1 = no 2D replacement pictures
layout(constant_id = 1) const int OVERLAY_SLOTS = 64;
layout(set = 0, binding = 3) uniform sampler2D overlays[OVERLAY_SLOTS];
#else
layout(set = 0, binding = 3) uniform sampler2D overlays[64];   // 2D replacement pictures (pack2d.h)
#endif
#define ATMOSPHERE_BINDING 4
#include "atmosphere.glsl"
layout(set = 0, binding = 5) uniform sampler2D text_atlas;   // high-resolution text (text_hd.h)
layout(set = 0, binding = 6) uniform sampler2D text_cells;
layout(set = 0, binding = 8) uniform sampler2D bloom;   // blurred glow of luminous surfaces (bloom.frag)
layout(set = 0, binding = 7) uniform sampler2D scene3d_alpha;   // alpha of the 3D image, in red
layout(set = 0, binding = 9) uniform sampler2D sky_coords;   // backdrop replacements (sky_pack.h, tilegen.h)
layout(set = 0, binding = 10) uniform sampler2D light_shafts;   // rays.frag
layout(set = 0, binding = 12) uniform sampler2D diffusion;   // the picture behind the HUD, blurred (bloom.frag)
layout(set = 0, binding = 11) uniform sampler2D exposure_log;   // 1x1: average log brightness of the 3D image, eased
layout(set = 0, binding = 13) uniform sampler2D bloom_tight;   // the same glow, blurred only a little (halos)

layout(push_constant) uniform Compose {
    vec2 scale;       // image size relative to the window
    vec2 offset;      // top-left corner
    vec2 crosshair;   // position in window pixels; negative = hidden
    vec2 window;      // window size in pixels
    vec2 layer2d;     // x offset and width of the 4:3 2D area inside the 3D view
    vec2 filter2d;    // x: 1 = xBR, 0 = plain pixels; y: window pixels per 2D pixel
    vec4 hud;         // x: shift of the top HUD band to the screen edges (2D pixels); y: 1 = HDR lighting;
                      // z: brightness offset of the foreground layers (fades), applied to the overlays;
                      // w: 1 = background layers continue beside the 4:3 area
    vec4 sky;         // x: seconds since launch (drift of the procedural clouds);
                      // y > 0: glow mode, for the bloom: only the bright part of the sky, times y;
                      // z: 1 = a sun in the procedural sky; w: 1 = mask mode, for the light shafts:
                      // the visible sky around the sun; 2 = measure mode: average log brightness of the 3D image;
                      // 3 = scene mode: the picture behind the HUD (backdrop and 3D), for the diffusion
    vec4 effects;     // x: bloom strength (0 = off); y: colour saturation (1 = original);
                      // z: gamma (1 = original, higher = brighter mid-tones); w: 1 = light shafts
    vec4 motion;      // x: interpolation of the back layers: the fraction of each row's scroll
                      // since the previous game frame (in sky_coords) to draw them back by;
                      // y: cinematic grade of space scenes (0 = off, 1 = full);
                      // z: 1 = Star Wars Trilogy's HUD bands (score / level on top, dialogue / shield below)
    vec4 crosshair2;  // xy: player 2's crosshair, like crosshair; z: sharpening (0 = off); w: 1 = space (full glow)
} pc;

// Rows of the 2D screen holding the score (rows 9-30); in widescreen its left
// half moves to the left edge and its right half to the right edge. The points
// won ("200") appear from row 33 down and stay where the game puts them.
const float HUD_TOP_ROWS = 32.0;

const ivec2 SIZE_2D = ivec2(496, 384);
const int BACK_MARGIN = 200;   // tilegen.h: the back layers extend beyond both sides
const ivec2 SIZE_BACK = ivec2(496 + 2 * BACK_MARGIN, 384);

// Crosshair: ring and cross with a dark outline so it reads on any background;
// player 1's yellow, player 2's cyan
vec4 crosshair(vec2 frag, vec2 at, vec3 tint) {
    vec2 d = frag - at;
    float size = max(10.0, pc.window.y / 60.0);
    float r = length(d);
    float ring = abs(r - size);
    float cross = min(abs(d.x), abs(d.y));
    bool in_cross = max(abs(d.x), abs(d.y)) < size * 1.6 && r > size * 0.35;
    float core = min(ring, in_cross ? cross : 1e9);
    if (core < 1.2) return vec4(tint, 1.0);
    if (core < 3.0) return vec4(0.0, 0.0, 0.0, 0.85);   // outline
    if (r < 1.8) return vec4(tint, 1.0);                // centre dot
    return vec4(0.0);
}

// ---- xBR-lv2 ---------------------------------------------------------------

const vec4 Ao = vec4(1.0, -1.0, -1.0, 1.0);
const vec4 Bo = vec4(1.0, 1.0, -1.0, -1.0);
const vec4 Co = vec4(1.5, 0.5, -0.5, 0.5);
const vec4 Ax = vec4(1.0, -1.0, -1.0, 1.0);
const vec4 Bx = vec4(0.5, 2.0, -0.5, -2.0);
const vec4 Cx = vec4(1.0, 1.0, -0.5, 0.0);
const vec4 Ay = vec4(1.0, -1.0, -1.0, 1.0);
const vec4 By = vec4(2.0, 0.5, -2.0, -0.5);
const vec4 Cy = vec4(2.0, 0.0, -1.0, 0.5);
const vec4 Ci = vec4(0.25);
const float EQ_THRESHOLD = 15.0;
const float LV2_COEFFICIENT = 2.0;

// Edge-detection value of a texel: luma, with transparency far from any colour
float val(vec4 c) {
    return c.a < 0.5 ? -128.0 : dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)) * 255.0;
}
vec4 df(vec4 a, vec4 b) { return abs(a - b); }
bvec4 eq(vec4 a, vec4 b) { return lessThan(df(a, b), vec4(EQ_THRESHOLD)); }
bvec4 neq(vec4 a, vec4 b) { return not(eq(a, b)); }
bvec4 band(bvec4 a, bvec4 b) { return bvec4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w); }
bvec4 bor(bvec4 a, bvec4 b) { return bvec4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w); }
vec4 wdist(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) {
    return df(a, b) + df(a, c) + df(d, e) + df(d, f) + 4.0 * df(g, h);
}
float cdist(vec4 a, vec4 b) { return dot(abs(a - b), vec4(1.0)); }

vec4 texel(sampler2D layer, ivec2 size, ivec2 p) { return texelFetch(layer, clamp(p, ivec2(0), size - 1), 0); }

vec4 xbr(sampler2D layer, vec2 q, ivec2 size) {
    vec2 pos = q * vec2(size);
    ivec2 e0 = ivec2(floor(pos));
    vec2 fp = pos - vec2(e0);
    //    A1 B1 C1
    // A0 A  B  C  C4
    // D0 D  E  F  F4
    // G0 G  H  I  I4
    //    G5 H5 I5
    vec4 A1 = texel(layer, size, e0 + ivec2(-1, -2)), B1 = texel(layer, size, e0 + ivec2(0, -2)), C1 = texel(layer, size, e0 + ivec2(1, -2));
    vec4 A0 = texel(layer, size, e0 + ivec2(-2, -1)), A = texel(layer, size, e0 + ivec2(-1, -1)), B = texel(layer, size, e0 + ivec2(0, -1));
    vec4 C = texel(layer, size, e0 + ivec2(1, -1)), C4 = texel(layer, size, e0 + ivec2(2, -1));
    vec4 D0 = texel(layer, size, e0 + ivec2(-2, 0)), D = texel(layer, size, e0 + ivec2(-1, 0)), E = texel(layer, size, e0);
    vec4 F = texel(layer, size, e0 + ivec2(1, 0)), F4 = texel(layer, size, e0 + ivec2(2, 0));
    vec4 G0 = texel(layer, size, e0 + ivec2(-2, 1)), G = texel(layer, size, e0 + ivec2(-1, 1)), H = texel(layer, size, e0 + ivec2(0, 1));
    vec4 I = texel(layer, size, e0 + ivec2(1, 1)), I4 = texel(layer, size, e0 + ivec2(2, 1));
    vec4 G5 = texel(layer, size, e0 + ivec2(-1, 2)), H5 = texel(layer, size, e0 + ivec2(0, 2)), I5 = texel(layer, size, e0 + ivec2(1, 2));

    // The four corner rotations are evaluated at once in the vector lanes
    vec4 b = vec4(val(B), val(D), val(H), val(F));
    vec4 c = vec4(val(C), val(A), val(G), val(I));
    vec4 e = vec4(val(E));
    vec4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;
    vec4 i4 = vec4(val(I4), val(C1), val(A0), val(G5));
    vec4 i5 = vec4(val(I5), val(C4), val(A1), val(G0));
    vec4 h5 = vec4(val(H5), val(F4), val(B1), val(D0));
    vec4 f4 = h5.yzwx;

    // Straight-line inequations for 45, 30 and 60 degree edges
    vec4 fx = Ao * fp.y + Bo * fp.x;
    vec4 fx_left = Ax * fp.y + Bx * fp.x;
    vec4 fx_up = Ay * fp.y + By * fp.x;

    bvec4 restriction_lv1 = band(neq(e, f), neq(e, h));
    bvec4 lv2_extra = bor(bor(band(neq(f, b), neq(f, c)), band(neq(h, d), neq(h, g))),
                          bor(band(eq(e, i), bor(band(neq(f, f4), neq(f, i4)), band(neq(h, h5), neq(h, i5)))),
                              bor(eq(e, g), eq(e, c))));
    restriction_lv1 = band(restriction_lv1, lv2_extra);
    bvec4 restriction_left = band(neq(e, g), neq(d, g));
    bvec4 restriction_up = band(neq(e, c), neq(b, c));

    // Edge smoothing about one window pixel wide
    vec4 delta = vec4(1.0 / max(pc.filter2d.y, 1.0));
    vec4 deltaL = vec4(0.5, 1.0, 0.5, 1.0) * delta;
    vec4 deltaU = deltaL.yxwz;
    vec4 fx45i = clamp((fx + delta - Co - Ci) / (2.0 * delta), 0.0, 1.0);
    vec4 fx45 = clamp((fx + delta - Co) / (2.0 * delta), 0.0, 1.0);
    vec4 fx30 = clamp((fx_left + deltaL - Cx) / (2.0 * deltaL), 0.0, 1.0);
    vec4 fx60 = clamp((fx_up + deltaU - Cy) / (2.0 * deltaU), 0.0, 1.0);

    vec4 wd1 = wdist(e, c, g, i, h5, f4, h, f);
    vec4 wd2 = wdist(h, d, i5, f, i4, b, e, i);
    bvec4 edri = band(lessThanEqual(wd1, wd2), restriction_lv1);
    bvec4 edr = band(lessThan(wd1, wd2), restriction_lv1);
    bvec4 edr_left = band(band(lessThanEqual(LV2_COEFFICIENT * df(f, g), df(h, c)), restriction_left), edr);
    bvec4 edr_up = band(band(greaterThanEqual(df(f, g), LV2_COEFFICIENT * df(h, c)), restriction_up), edr);

    fx45 = vec4(edr) * fx45;
    fx30 = vec4(edr_left) * fx30;
    fx60 = vec4(edr_up) * fx60;
    fx45i = vec4(edri) * fx45i;

    bvec4 px = lessThanEqual(df(e, f), df(e, h));
    vec4 maximos = max(max(fx30, fx60), max(fx45, fx45i));

    vec4 res1 = E;
    res1 = mix(res1, px.x ? F : H, maximos.x);
    res1 = mix(res1, px.z ? D : B, maximos.z);
    vec4 res2 = E;
    res2 = mix(res2, px.y ? B : F, maximos.y);
    res2 = mix(res2, px.w ? H : D, maximos.w);
    return cdist(E, res2) >= cdist(E, res1) ? res2 : res1;
}

vec4 layer(sampler2D tex, vec2 q, ivec2 size) {
    return pc.filter2d.x > 0.5 ? xbr(tex, q, size) : texture(tex, q);
}

// ---- Backdrop replacements ---------------------------------------------------
// The back layer pixels that show a replaced backdrop say which picture and
// which point of its 512x512 layer they show; the picture is sampled in
// between at full resolution, following the game's scroll.

const uint SKY_FIRST_SLOT = 56u;        // SkyPack::FIRST_SLOT

// Procedural sky (SKY: PROCEDURAL): a gradient with soft clouds, on the
// layers as a panorama that wraps around the camera (the lower one with the
// horizon, the upper one above it).
// sky_pack.cpp has the same sky without the drift (keep both in step).
float sky_hash(ivec2 p) {
    uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFFFu) / 16777216.0;
}

// Value noise that repeats every `period` cells along x (the layer wraps around)
float sky_noise(vec2 p, int period) {
    ivec2 i = ivec2(floor(p));
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    int x0 = ((i.x % period) + period) % period, x1 = (x0 + 1) % period;
    return mix(mix(sky_hash(ivec2(x0, i.y)), sky_hash(ivec2(x1, i.y)), f.x),
               mix(sky_hash(ivec2(x0, i.y + 1)), sky_hash(ivec2(x1, i.y + 1)), f.x), f.y);
}

float sky_fbm(vec2 p, int period, int octaves) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < octaves; i++) {
        s += a * sky_noise(p, period);
        p = p * 2.0 + vec2(5.3, 9.7);
        period *= 2;
        a *= 0.5;
    }
    return s;
}

float sky_density(vec2 p) { return max(sky_fbm(p, 8, 6) - 0.45, 0.0) * 10.0; }

// at: x and y in the 512x512 lower layer (the upper one has y from -512 to 0)
// sky, lit: colours of the clear sky and of the lit clouds there
vec3 sky_procedural(vec2 at, float drift, vec3 sky, vec3 lit) {
    const float HORIZON = 500.0, HEIGHT = 1012.0;
    float t = clamp((HORIZON - at.y) / HEIGHT, 0.0, 1.0);   // 0 at the horizon, 1 at the top
    // More depth than the original: richer colour, darker towards the top,
    // a pale glow at the horizon, and whiter cloud tops
    float l = dot(sky, vec3(0.2126, 0.7152, 0.0722));
    sky = max(vec3(l) + (sky - vec3(l)) * 1.3, 0.0) * (1.0 - 0.3 * smoothstep(0.1, 0.8, t))
          + 0.06 * (1.0 - smoothstep(0.0, 0.15, t));
    lit = mix(lit, vec3(1.0), 0.3);
    // The layer is a cylinder around the camera: u around it (8 cells), v up
    // it, squeezed towards the horizon by perspective
    float el = 0.03 + 1.5 * t;   // elevation (radians)
    vec2 p = vec2(at.x * (8.0 / 512.0) + drift, 6.0 * tanh(2.5 / tan(el) / 6.0) - 6.0 * t);
    p += 0.5 * vec2(sky_fbm(p * 0.5 + vec2(3.1, 0.0), 4, 5), sky_fbm(p * 0.5 + vec2(0.0, 7.7), 4, 5));
    // Cloud density, then the light that reaches each point from above
    // through the cloud: lit tops, shaded bases, bright thin edges
    float density = sky_density(p);
    float above = 0.0;
    for (int i = 1; i <= 4; i++) above += sky_density(p - vec2(0.0, 0.06 * float(i)));
    float light = exp(-above * 0.9);
    float cover = (1.0 - exp(-density * 3.0)) * smoothstep(0.0, 0.12, t);
    vec3 cloud = mix((sky * 0.6 + lit * 0.3) * 0.75, min(lit * 1.05, vec3(1.0)), light) + (1.0 - cover) * 0.15;
    cloud *= 0.8 + 0.4 * sky_fbm(p * 4.0 + vec2(1.7, 2.9), 32, 3);   // finer billows
    cloud = mix(cloud, sky, smoothstep(0.17, 0.0, t) * 0.6);
    return mix(sky, cloud, cover);
}

// Sun in the procedural sky (SUN), where the game's own light comes from:
// a small bright disc with a warm halo around it. p: position in the 3D image
vec3 sky_sun(vec3 c, vec2 p) {
    if (atm.sun.w <= 0.0) return c;
    vec2 ndc = p * 2.0 - 1.0;
    vec3 dir = normalize(vec3((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y, -1.0));
    float mu = dot(dir, atm.sky_sun.xyz);
    if (mu <= 0.0) return c;
    // Clouds (grey, where the clear sky is blue) hide the disc and dim the halo
    float clear = smoothstep(0.05, 0.22, c.b - c.r);
    float disc = smoothstep(0.99990, 0.99994, mu) * clear;   // about half a degree across
    float halo = (0.55 * pow(mu, 900.0) + 0.18 * pow(mu, 60.0)) * mix(0.45, 1.0, clear);
    return min(c + disc * vec3(1.0, 0.97, 0.88) + halo * vec3(1.0, 0.86, 0.62) * atm.sun.w, vec3(1.0));
}

// Night sky (SKY: PROCEDURAL) where the game leaves a black backdrop behind
// a dark world (the T-Rex at the trailer): the procedural clouds, faintly
// moonlit on a deep blue, laid around the camera by the world's own
// directions since there is no layer scroll to follow. p: position in the 3D image
vec3 sky_night(vec2 p) {
    vec2 ndc = p * 2.0 - 1.0;
    vec3 dir = normalize(vec3((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y, -1.0));
    vec3 d = normalize(vec3(dot(atm.axes[0].xyz, dir), dot(atm.axes[1].xyz, dir), dot(atm.axes[2].xyz, dir)));
    float el = asin(clamp(d.y, -1.0, 1.0));
    float t = clamp((el - 0.03) / 1.5, 0.0, 1.0);   // as in sky_procedural
    vec2 at = vec2(fract(atan(d.z, d.x) / 6.2831853) * 512.0, 500.0 - 1012.0 * t);
    // The day sky's clouds in dark colours: made at a third of the
    // brightness, from colours three times as bright
    const float DIM = 0.33;
    vec3 sky = mix(vec3(0.050, 0.070, 0.130), vec3(0.012, 0.020, 0.050), smoothstep(0.0, 0.6, t)) / DIM;
    vec3 lit = vec3(0.17, 0.19, 0.25) / DIM;
    return min(sky_procedural(at, pc.sky.x * 0.002, sky, lit) * DIM, vec3(1.0));
}

// Hoth (SKY: PROCEDURAL): a cold pale blue sky with drifting clouds in place
// of the game's sky dome, laid around the camera by the world's directions,
// white towards the horizon like the snow's haze. p: position in the 3D image
vec3 sky_hoth(vec2 p) {
    vec2 ndc = p * 2.0 - 1.0;
    vec3 dir = normalize(vec3((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y, -1.0));
    vec3 d = normalize(vec3(dot(atm.axes[0].xyz, dir), dot(atm.axes[1].xyz, dir), dot(atm.axes[2].xyz, dir)));
    float el = asin(clamp(d.y, -1.0, 1.0));
    float t = clamp((el - 0.03) / 1.5, 0.0, 1.0);
    vec2 at = vec2(fract(atan(d.z, d.x) / 6.2831853) * 512.0, 500.0 - 1012.0 * t);
    vec3 sky = mix(vec3(0.74, 0.83, 0.93), vec3(0.40, 0.56, 0.80), smoothstep(0.0, 0.7, t));
    vec3 lit = vec3(0.98, 0.99, 1.0);
    vec3 c = sky_procedural(at, pc.sky.x * 0.004, sky, lit);
    // The haze over the snow at the horizon (and below it, behind the hills)
    c = mix(c, vec3(0.93, 0.95, 0.98), 1.0 - smoothstep(-0.02, 0.16, el));
    return min(c, vec3(1.0));
}

// ---- Procedural space (SKY: PROCEDURAL, Star Wars Trilogy) -------------------
// In place of the game's star field: stars of many brightnesses and colours,
// the Milky Way with its dust lanes and a few faint nebulae, fixed to the
// game's star field so they turn as it does
float space_h31(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
vec3 space_h33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}
float space_noise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(space_h31(i), space_h31(i + vec3(1, 0, 0)), f.x),
                   mix(space_h31(i + vec3(0, 1, 0)), space_h31(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(space_h31(i + vec3(0, 0, 1)), space_h31(i + vec3(1, 0, 1)), f.x),
                   mix(space_h31(i + vec3(0, 1, 1)), space_h31(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float space_fbm(vec3 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) {
        s += a * space_noise(p);
        p = p * 2.03 + vec3(1.7, 9.2, 5.3);
        a *= 0.5;
    }
    return s;
}
// One layer of stars: a grid on each face of a cube around the camera, at
// most one star per cell (chance), the four cells around the nearest corner
// looked at. px: size of a pixel (radians)
vec3 space_stars(vec3 d, float cells, float chance, float bright, float px, float seed) {
    vec3 a = abs(d);
    float face;
    vec2 uv;
    if (a.x >= a.y && a.x >= a.z) { face = d.x > 0.0 ? 0.0 : 1.0; uv = d.yz / a.x; }
    else if (a.y >= a.z) { face = d.y > 0.0 ? 2.0 : 3.0; uv = d.xz / a.y; }
    else { face = d.z > 0.0 ? 4.0 : 5.0; uv = d.xy / a.z; }
    vec2 g = (uv * 0.5 + 0.5) * cells;
    float rad = 2.0 / cells / (1.0 + dot(uv, uv));   // one cell in radians, about
    vec2 base = floor(g - 0.5);
    vec3 c = vec3(0.0);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec2 cell = base + vec2(i, j);
            vec3 h = space_h33(vec3(cell, face * 517.0 + seed));
            if (h.z > chance) continue;
            float k = h.z / chance;   // 0..1: brightness and colour
            float mag = bright * pow(fract(k * 7.31 + h.x), 3.0);
            vec2 at = cell + 0.05 + 0.9 * h.xy;
            float r = length(g - at) * rad;
            float sigma = px * (0.55 + 0.9 * mag);
            float lit = mag * exp(-r * r / (2.0 * sigma * sigma));
            float temp = fract(k * 3.17 + h.y * 5.3);
            vec3 tint = temp < 0.2 ? vec3(1.0, 0.72, 0.52) : temp < 0.45 ? vec3(1.0, 0.92, 0.80)
                      : temp < 0.8 ? vec3(1.0) : vec3(0.70, 0.80, 1.0);
            c += lit * tint;
        }
    return c;
}
vec3 sky_space(vec2 p) {
    vec2 ndc = p * 2.0 - 1.0;
    vec3 dir = normalize(vec3((ndc.x + atm.stars_view.z) / atm.stars_view.x,
                              (ndc.y + atm.stars_view.w) / atm.stars_view.y, -1.0));
    vec3 d = normalize(vec3(dot(atm.stars[0].xyz, dir), dot(atm.stars[1].xyz, dir), dot(atm.stars[2].xyz, dir)));
    float px = 2.0 * atm.backdrop.y / abs(atm.stars_view.y);
    // The Milky Way: a band around the sky, with dark dust lanes along it
    vec3 n = normalize(vec3(0.32, 0.86, 0.40));
    float lat = dot(d, n);
    float band = exp(-lat * lat / 0.035);
    float core = exp(-lat * lat / 0.006);
    float cloud = space_fbm(d * 3.0 + 4.1);
    float dust = smoothstep(0.42, 0.68, space_fbm(d * 7.0 + vec3(2.3, 0.4, 7.7)));
    float milky = band * (0.2 + 1.2 * smoothstep(0.3, 0.75, cloud)) * (1.0 - 0.8 * dust * band);
    vec3 col = vec3(0.004, 0.005, 0.010);
    col += milky * mix(vec3(0.104, 0.104, 0.136), vec3(0.176, 0.152, 0.120), core);
    // Faint nebulae, blue and violet, here and there
    float neb = smoothstep(0.48, 0.78, space_fbm(d * 1.6 + vec3(8.2, 3.1, 1.4)));
    float hue = space_fbm(d * 1.1 + 11.0);
    col += neb * mix(vec3(0.105, 0.038, 0.150), vec3(0.022, 0.090, 0.135), smoothstep(0.35, 0.65, hue)) *
           (0.6 + 0.8 * space_fbm(d * 5.0));
    // Stars: a few bright ones, more dim ones, and many faint ones, crowded in the Milky Way
    col += space_stars(d, 34.0, 0.45, 1.00, px, 1.0);
    col += space_stars(d, 110.0, 0.55, 0.55, px, 2.0);
    col += space_stars(d, 300.0, 0.18 + 0.6 * band, 0.30, px, 3.0);
    return min(col, vec3(1.0));
}

// Which picture and point each back layer pixel shows: sampled in between.
// dx, dy: screen derivatives of qb * SIZE_BACK / 512 (explicit, as the
// position wraps every 512 pixels). True where the sky is procedural.
bool sky_layer(vec2 qb, vec2 dx, vec2 dy, inout vec4 back) {
    vec2 pos = qb * vec2(SIZE_BACK);
    ivec2 t = clamp(ivec2(floor(pos)), ivec2(0), SIZE_BACK - 1);
    vec4 v = texelFetch(sky_coords, t, 0);
    if (v.a < 0.5) return false;
    uvec4 b = uvec4(round(v * 255.0));
    vec2 at = vec2(b.r | ((b.b & 1u) << 8), b.g | (((b.b >> 1) & 1u) << 8)) + clamp(pos - vec2(t), 0.0, 1.0);
    uint slot = SKY_FIRST_SLOT + ((b.b >> 2) & 7u);
    // Procedural: its picture is the original, for the trees on the horizon
    bool procedural = (b.b & 128u) != 0u;
    vec4 s;
    if (!procedural) {
        s = textureGrad(overlays[nonuniformEXT(slot)], fract(at / 512.0), dx, dy);
    } else {
        // Colours of this sky's band of rows, taken from its original (last row)
        float y = mod(at.y, 512.0);
        float fy = clamp(y / 16.0 - 0.5, 0.0, 31.0);
        int band = int(fy), table = 8 + int((b.b >> 2) & 7u) * 64;
        int next = min(band + 1, 31);
        vec3 sky = mix(texelFetch(sky_coords, ivec2(table + band * 2, SIZE_BACK.y), 0).rgb,
                       texelFetch(sky_coords, ivec2(table + next * 2, SIZE_BACK.y), 0).rgb, fy - float(band));
        vec3 lit = mix(texelFetch(sky_coords, ivec2(table + band * 2 + 1, SIZE_BACK.y), 0).rgb,
                       texelFetch(sky_coords, ivec2(table + next * 2 + 1, SIZE_BACK.y), 0).rgb, fy - float(band));
        if ((b.b & 64u) != 0u) {   // the sky above
            s = vec4(sky_procedural(vec2(at.x, y - 512.0), pc.sky.x * 0.005, sky, lit), 1.0);
        } else {
            // Trees on the horizon: the dark parts of the original's bottom rows
            vec4 o = textureGrad(overlays[nonuniformEXT(slot)], fract(at / 512.0), dx, dy);
            float tree = y > 455.0 ? 1.0 - smoothstep(0.10, 0.20, dot(o.rgb, vec3(0.2126, 0.7152, 0.0722))) : 0.0;
            s = vec4(mix(sky_procedural(vec2(at.x, y), pc.sky.x * 0.005, sky, lit), o.rgb * 0.9, tree), 1.0);
        }
    }
    // Fade offset of the layer pair, in the last row
    vec3 offset = (texelFetch(sky_coords, ivec2((b.b >> 5) & 1u, SIZE_BACK.y), 0).rgb * 255.0 - 128.0) * (2.0 / 255.0);
    back = vec4(clamp(s.rgb + offset, 0.0, 1.0) * s.a, s.a);
    return procedural;
}

// ---- High-resolution text ----------------------------------------------------
// Cells that hold known letters were left out of the foreground layer; the
// letter is drawn here from the vector-font atlas with the game's colours.

const int TEXT_CELL_TEXELS = 4;
const int OVERLAY_ROW = 48;          // after the cells: the replacement pictures on screen

// The 2D replacement pictures (pack2d.h) of the foreground or of the layers
// below the 3D image over `under` (premultiplied), at `pos` in 2D pixels.
// Listed in the last row of the text cells (text_hd.h): rectangle in cells,
// then slot in R and 1 in G for the layers below the 3D image.
vec4 replacement_pictures(vec4 under, vec2 pos, bool below_3d) {
    int pictures = int(texelFetch(text_cells, ivec2(0, OVERLAY_ROW), 0).r * 255.0 + 0.5);
    for (int i = 0; i < pictures; i++) {
        vec4 info = texelFetch(text_cells, ivec2(2 + 2 * i, OVERLAY_ROW), 0);
        if ((info.g > 0.5) != below_3d) continue;
        vec4 rect = round(texelFetch(text_cells, ivec2(1 + 2 * i, OVERLAY_ROW), 0) * 255.0) * 8.0;
        vec2 r = (pos - rect.xy) / (rect.zw - rect.xy);
        if (all(greaterThanEqual(r, vec2(0.0))) && all(lessThan(r, vec2(1.0)))) {
            vec4 o = texture(overlays[nonuniformEXT(uint(info.r * 255.0 + 0.5))], r);
            under = under * (1.0 - o.a) + vec4(clamp(o.rgb + pc.hud.z, 0.0, 1.0) * o.a, o.a);
        }
    }
    return under;
}
const int ATLAS_COLS = 16;           // tools/make_text_atlas.py
const float ATLAS_SLOT = 80.0;       // (8 + 2 * PAD) * SCALE
const float ATLAS_PAD = 8.0;         // PAD * SCALE
const float ATLAS_CELL = 64.0;       // 8 * SCALE

vec4 front_layer(vec2 q) {
    vec4 f = layer(layer2d_front, q, SIZE_2D);
    vec2 pos = q * vec2(SIZE_2D) / 8.0;
    ivec2 cell = clamp(ivec2(pos), ivec2(0), SIZE_2D / 8 - 1);
    ivec2 t = ivec2(cell.x * TEXT_CELL_TEXELS, cell.y);
    vec4 info = texelFetch(text_cells, t, 0);
    if (info.a < 0.5) return f;
    int slot = int(info.r * 255.0 + 0.5) + 256 * int(info.g * 255.0 + 0.5);
    vec2 local = clamp(pos - vec2(cell), 0.0, 1.0);
    vec2 corner = vec2(slot % ATLAS_COLS, slot / ATLAS_COLS) * ATLAS_SLOT + ATLAS_PAD;
    // Explicit mip level: screen derivatives jump at the cell edges, where the
    // atlas coordinates change slot
    float lod = max(log2(ATLAS_CELL / 8.0 / max(pc.filter2d.y, 1e-3)), 0.0);
    vec4 g = textureLod(text_atlas, (corner + local * ATLAS_CELL) / vec2(textureSize(text_atlas, 0)), lod);
    vec4 top = texelFetch(text_cells, t + ivec2(1, 0), 0);
    vec4 bottom = texelFetch(text_cells, t + ivec2(2, 0), 0);
    vec4 edge = texelFetch(text_cells, t + ivec2(3, 0), 0);
    float ea = g.g * edge.a, fa = g.r * top.a;
    f = f * (1.0 - ea) + vec4(edge.rgb, 1.0) * ea;   // premultiplied, like the layer
    f = f * (1.0 - fa) + vec4(mix(top.rgb, bottom.rgb, g.b), 1.0) * fa;
    return f;
}

// HDR lighting: below the knee the image is left as the original drew it;
// above it the highlights roll off smoothly towards white instead of clipping
vec3 tone_curve(vec3 x) {
    const float knee = 0.8;
    vec3 over = max(x - knee, 0.0);
    return min(x, vec3(knee)) + (1.0 - knee) * (1.0 - exp(-over / (1.0 - knee)));
}

// How much the eased average brightness opens or closes the exposure: gently,
// within a narrow range, so dark scenes stay dark and bright ones bright
float exposure() {
    float average = exp(texelFetch(exposure_log, ivec2(0), 0).r);
    return clamp(pow(0.17 / max(average, 1e-3), 0.35), 0.9, 1.15);
}

void main() {
    if (pc.sky.w > 1.5 && pc.sky.w < 2.5) {   // measure mode (one pixel): log-average brightness of the 3D image
        // (sky.y > 0: taken to be this instead, R3DScene::exposure_average)
        if (pc.sky.y > 0.0) {
            color = vec4(log(pc.sky.y));
            return;
        }
        float sum = 0.0, n = 0.0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                vec2 q = (vec2(x, y) + 0.5) / 16.0;
                vec4 s = texture(scene3d, q);
                if (texture(scene3d_alpha, q).r < 0.5) continue;
                sum += log(max(dot(s.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-3));
                n += 1.0;
            }
        color = vec4(n > 0.0 ? sum / n : log(0.17));
        return;
    }
    vec2 p = (uv - pc.offset) / pc.scale;
    vec3 c = vec3(0.0);
    if (p.x >= 0.0 && p.y >= 0.0 && p.x <= 1.0 && p.y <= 1.0) {
        vec4 s = texture(scene3d, p);
        // With DLAA the alpha comes from its own anti-aliasing pass, steady
        // from frame to frame like the colour
        s.a = texture(scene3d_alpha, p).r;
        // Star Wars Trilogy: the cracks between the modules its scenery is
        // built of (Hoth's base) let the black backdrop through as thin
        // flickering lines; a gap of up to 3 pixels with the 3D image on both
        // sides is filled from them
        if (pc.motion.z > 0.5 && s.a < 0.99) {
            vec2 t = 1.0 / vec2(textureSize(scene3d, 0));
            for (int k = 1; k <= 3; k++) {
                vec2 dx = vec2(t.x * float(k), 0.0), dy = vec2(0.0, t.y * float(k));
                bool fill_x = texture(scene3d_alpha, p - dx).r > 0.99 && texture(scene3d_alpha, p + dx).r > 0.99;
                bool fill_y = texture(scene3d_alpha, p - dy).r > 0.99 && texture(scene3d_alpha, p + dy).r > 0.99;
                if (fill_x || fill_y) {
                    vec2 d = fill_x ? dx : dy;
                    s = vec4(0.5 * (texture(scene3d, p - d).rgb + texture(scene3d, p + d).rgb), 1.0);
                    break;
                }
            }
        }
        // Sharpening (SHARPEN), contrast adaptive like AMD's CAS: detail comes
        // back after the anti-aliasing and the diffusion, flat areas and
        // strong edges are left alone (no halos)
        if (pc.crosshair2.z > 0.0 && s.a > 0.0) {
            vec2 t = 1.0 / vec2(textureSize(scene3d, 0));
            vec3 a = texture(scene3d, p - vec2(0.0, t.y)).rgb, b = texture(scene3d, p - vec2(t.x, 0.0)).rgb;
            vec3 d = texture(scene3d, p + vec2(t.x, 0.0)).rgb, e = texture(scene3d, p + vec2(0.0, t.y)).rgb;
            vec3 lo = min(s.rgb, min(min(a, b), min(d, e))), hi = max(s.rgb, max(max(a, b), max(d, e)));
            vec3 amp = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, vec3(1e-4)), 0.0, 1.0));
            vec3 w = -amp * mix(0.125, 0.2, pc.crosshair2.z);
            s.rgb = max((a + b + d + e) * w + s.rgb, 0.0) / (1.0 + 4.0 * w);
        }
        if (pc.hud.y > 0.5 && s.a > 0.0) {   // HDR lighting: exposure and tone curve (premultiplied)
            vec3 lit = s.rgb / max(s.a, 1e-4) * exposure();
            s.rgb = tone_curve(lit) * s.a;
        }
        vec2 q = vec2((p.x - pc.layer2d.x) / pc.layer2d.y, p.y);
        // The back layers (sky) were drawn wider than the screen; beyond that
        // and for the front layers the outermost column is repeated
        vec2 qb = vec2((q.x * float(SIZE_2D.x) + float(BACK_MARGIN)) / float(SIZE_BACK.x), q.y);
        // Between game frames the back layers (sky) go part of the way back to
        // where they were, like the 3D image in front of them
        if (pc.motion.x > 0.0) {
            vec4 m = texelFetch(sky_coords, ivec2(clamp(int(q.y * 384.0), 0, 383), SIZE_BACK.y + 1), 0);
            if (m.b > 0.5) qb -= (m.rg * 255.0 - 128.0) * pc.motion.x / vec2(SIZE_BACK);
        }
        qb.x = clamp(qb.x, 0.5 / float(SIZE_BACK.x), 1.0 - 0.5 / float(SIZE_BACK.x));
        q.x = clamp(q.x, 0.5 / 496.0, 1.0 - 0.5 / 496.0);
        bool inside_43 = p.x >= pc.layer2d.x && p.x <= pc.layer2d.x + pc.layer2d.y;
        vec2 sky_dx = dFdx(qb * vec2(SIZE_BACK)) / 512.0, sky_dy = dFdy(qb * vec2(SIZE_BACK)) / 512.0;
        vec4 back = vec4(0.0);
        if (pc.hud.w > 0.5 || inside_43) {
            back = layer(layer2d_back, qb, SIZE_BACK);
            if (sky_layer(qb, sky_dx, sky_dy, back) && pc.sky.z > 0.5) back.rgb = sky_sun(back.rgb, p);
            else if (atm.stars[0].w > 0.5 && max(back.r, max(back.g, back.b)) < 0.002 &&
                     all(greaterThanEqual(p, atm.stars_rect.xy)) && all(lessThanEqual(p, atm.stars_rect.zw)))
                back = vec4(sky_space(p), 1.0);
            else if (atm.day_sky.x > 0.5 && atm.view.x != 0.0 &&
                     all(greaterThanEqual(p, atm.day_rect.xy)) && all(lessThanEqual(p, atm.day_rect.zw)))
                back = vec4(sky_hoth(p), 1.0);
            else if (atm.night.x > 0.01 && atm.view.x != 0.0 && max(back.r, max(back.g, back.b)) < 0.002)
                back = vec4(sky_night(p) * atm.night.x, 1.0);
        } else if (atm.stars[0].w > 0.5 && all(greaterThanEqual(p, atm.stars_rect.xy)) &&
                   all(lessThanEqual(p, atm.stars_rect.zw))) {
            back = vec4(sky_space(p), 1.0);   // space beside the 4:3 area too
        } else if (atm.day_sky.x > 0.5 && atm.view.x != 0.0 &&
                     all(greaterThanEqual(p, atm.day_rect.xy)) && all(lessThanEqual(p, atm.day_rect.zw))) {
            back = vec4(sky_hoth(p), 1.0);
        }
        // 2D replacement pictures of the layers below the 3D image (title and
        // tutorial screens), inside the 4:3 area
        if (inside_43) back = replacement_pictures(back, q * vec2(SIZE_2D), true);
        vec4 front;
        float shift = pc.hud.x;
        float row = q.y * 384.0;
        // Where the foreground's replacement pictures are looked up (2D pixels):
        // moved with the HUD like the foreground itself (-1: none there)
        vec2 pic_pos = vec2((p.x - pc.layer2d.x) / pc.layer2d.y * 496.0, p.y * 384.0);
        if (shift > 0.0 && pc.motion.z > 0.5) {
            // Star Wars Trilogy: on top the score goes to the left edge and the
            // level to the right one; below, the dialogue box (and the credits)
            // to the left and the shield to the right, 24 rows lower (its 3D
            // ring too: R3D_HUD_LOWER). The blaster's OVERHEAT gauge (a column
            // left of x 48, rows 52 to 340, on foot) goes to the left edge too
            float x = (p.x - pc.layer2d.x) / pc.layer2d.y * 496.0;
            float xl = x + shift, xr = x - shift;
            bool top = row < 52.0;
            float rr = top ? row : row - 24.0;   // the shield's source row
            bool gauge = row >= 52.0 && row < 340.0 && xl < 48.0;
            bool left = top ? xl < 311.0 : row >= 321.5 && xl < 380.0;
            bool right = top ? xr >= 311.0 : rr >= 278.0 && (rr < 321.5 || xr >= 380.0) && !(xr < 48.0 && rr < 340.0);
            if (gauge) {
                front = xl >= 0.0 ? front_layer(vec2(max(xl, 0.5) / 496.0, q.y)) : vec4(0.0);
                pic_pos = vec2(xl, row);
            } else if (!top && row < 278.0) {
                front = x < 48.0 ? vec4(0.0) : front_layer(q);
            } else if (left && xl >= 0.0) {
                front = front_layer(vec2(max(xl, 0.5) / 496.0, q.y));
                pic_pos = vec2(xl, row);
            } else if (right && xr < 496.0) {
                front = front_layer(vec2(min(xr, 495.5) / 496.0, rr / 384.0));
                pic_pos = vec2(xr, rr);
            } else {
                front = vec4(0.0);
                pic_pos = vec2(-1.0);
            }
        } else if (shift > 0.0 && pc.motion.z < 0.5 && row < HUD_TOP_ROWS) {
            float x = (p.x - pc.layer2d.x) / pc.layer2d.y * 496.0;
            float from_left = x + shift, from_right = x - shift;
            if (from_left < 248.0)
                front = front_layer(vec2(max(from_left, 0.5) / 496.0, q.y));
            else if (from_right >= 248.0)
                front = front_layer(vec2(min(from_right, 495.5) / 496.0, q.y));
            else
                front = vec4(0.0);
        } else {
            front = front_layer(q);
        }
        // The original's scroll fog veils the 2D layers behind the 3D image
        vec2 ndc_view = p * 2.0 - 1.0;
        vec3 dir_view = vec3((ndc_view.x + atm.view.z) / atm.view.x, (ndc_view.y + atm.view.w) / atm.view.y, -1.0);
        back.rgb = scroll_fog(back.rgb, p, atm.view.x != 0.0 && atm.view.y != 0.0 ? mist_light(dir_view)
                                                                               : vec3(1.0));
        // The sky behind a game world gets the same exposure as the 3D image
        // in front of it, or fully fogged scenery (the same fog colour as the
        // fogged sky) would stand out as a slightly darker silhouette
        if (pc.hud.y > 0.5 && atm.view.x != 0.0 && back.a > 0.0)
            back.rgb = tone_curve(back.rgb / back.a * exposure()) * back.a;
        // Glow mode (r3d_renderer.cpp, bloom): the sky's bright part where the 3D leaves it visible
        if (pc.sky.w > 0.5 && pc.sky.w < 1.5) {   // light shaft mask: visible sky, more of it near the sun
            vec3 b = back.rgb * (1.0 - s.a);
            vec2 ndc = p * 2.0 - 1.0;
            vec3 dir = normalize(vec3((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y, -1.0));
            float mu = max(dot(dir, atm.sky_sun.xyz), 0.0);
            color = vec4(vec3(dot(b, vec3(0.2126, 0.7152, 0.0722)) * pow(mu, 20.0)), 0.0);
            return;
        }
        if (pc.sky.y > 0.0) {
            // A sky hidden by the scroll fog is just fog: it glows no more than
            // the fogged scenery in front of it
            vec3 b = back.rgb * (1.0 - s.a) * (1.0 - scroll_fog_amount(p));
            float peak = max(b.r, max(b.g, b.b));
            // Mostly around the sun: a bright sky all over (Hoth) glowing
            // everywhere only washed the picture out in white
            float near_sun = 0.2;
            if (atm.view.x != 0.0 && atm.view.y != 0.0) {
                vec2 ndc = p * 2.0 - 1.0;
                vec3 dir = normalize(vec3((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y, -1.0));
                near_sun = mix(0.2, 1.0, pow(max(dot(dir, atm.sky_sun.xyz), 0.0), 8.0));
            }
            color = vec4(b * smoothstep(0.55, 1.0, peak) * near_sun * pc.sky.y, 0.0);
            return;
        }
        // Transparent 2D texels are all zero, so filtered 2D colours are
        // premultiplied by their alpha
        c = back.rgb;
        // The 3D image is premultiplied too: it starts cleared to zero, MSAA
        // averages partly covered edges with that zero, and the translucent
        // pass blends over it
        c = c * (1.0 - s.a) + s.rgb;
        if (pc.sky.w > 2.5) {   // scene mode
            color = vec4(c, 0.0);
            return;
        }
        // Diffusion (part of BLOOM): the picture mixed with a blurred copy of
        // itself, light spilling into the dark around it more than the other
        // way round, like a soft-focus filter: the hard contrasts soften while
        // the overall brightness stays where it was
        if (pc.effects.x > 0.0) {
            vec3 soft = texture(diffusion, p).rgb;
            float k = 0.3 * pc.effects.x * (0.45 + 0.55 * dot(soft, vec3(0.2126, 0.7152, 0.0722)));
            c = mix(c, soft, clamp(k, 0.0, 0.5));
        }
        // Glow of fire, explosions and lights over the 3D image and the backdrop
        // (less the part every pixel of a bright scene shares: a sunlit
        // snowfield glows all over, which only veiled the picture in white)
        if (pc.effects.x > 0.0) {
            // and less what is already bright around it: glow should spill
            // into the dark, a lit explosion over sunlit snow needs none
            float average = exp(texelFetch(exposure_log, ivec2(0), 0).r);
            // In a dark scene (space) the glow of engines, lasers and
            // explosions is the light that matters: up to twice as strong,
            // and only a bright scene loses the part its surroundings share
            // Only space gets it in full: a dark tunnel on the ground (Hoth's
            // ice caves) would blow its bright exit up into a blocky white blob
            float dark = max(0.35 * (1.0 - smoothstep(0.04, 0.25, average)), pc.crosshair2.w);
            vec3 around = texture(diffusion, p).rgb * (1.0 - dark);
            vec3 cut = vec3(0.5 * average) + 0.7 * around;
            vec3 wide = max(texture(bloom, p).rgb - cut, vec3(0.0));
            // ... and a tight halo hugging lasers, engines and explosions,
            // the coloured glow of the films, strongest in the dark of space
            vec3 tight = max(texture(bloom_tight, p).rgb - cut, vec3(0.0));
            c += (wide * 0.5 * (1.0 + dark) + tight * (0.3 + 1.7 * dark)) * pc.effects.x;
        }
        // Shafts of sunlight between trees and silhouettes, warm like the sun
        if (pc.effects.w > 0.0) c += texture(light_shafts, p).r * vec3(1.0, 0.9, 0.72);
        // Space as the films show it (under the HUD): lights streak sideways
        // like through an anamorphic lens, cool shadows and warm highlights,
        // a gentle S-curve, a darker frame edge and a little film grain
        float cine = pc.motion.y;
        if (cine > 0.0) {
            vec3 streak = vec3(0.0);
            if (pc.effects.x > 0.0)
                for (int i = 1; i <= 8; i++) {
                    float d = float(i) * 0.014;
                    streak += (texture(bloom_tight, p + vec2(d, 0.0)).rgb + texture(bloom_tight, p - vec2(d, 0.0)).rgb) *
                              exp(-float(i) * 0.38);
                }
            c += dot(streak, vec3(0.3, 0.5, 0.2)) * vec3(0.35, 0.55, 1.0) * 0.22 * cine;
            float l = clamp(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
            c += cine * (vec3(-0.012, 0.0, 0.018) * (1.0 - l) * smoothstep(0.0, 0.25, l) + vec3(0.04, 0.012, -0.035) * l * l);
            vec3 s_curve = clamp(c, 0.0, 1.0);
            s_curve = s_curve * s_curve * (3.0 - 2.0 * s_curve);
            c = mix(c, s_curve, 0.22 * cine);
            vec2 e = (p - 0.5) * vec2(1.25, 1.0);
            c *= mix(1.0, 0.72 + 0.28 * smoothstep(0.85, 0.25, length(e)), cine);
            float grain = fract(sin(dot(gl_FragCoord.xy + fract(pc.sky.x * 13.7) * 517.0, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
            c += grain * 0.022 * cine * (0.35 + l);
        }
        // With the procedural space behind, the ships and the station a little
        // brighter (gamma), the sky itself as it is
        if (atm.stars[0].w > 0.5) c = mix(c, pow(max(c, vec3(0.0)), vec3(0.85)), clamp(s.a, 0.0, 1.0));
        // 2D replacement pictures of the foreground: above the 3D image, below
        // what changed in the 2D layer
        vec4 pictures = replacement_pictures(vec4(c, 1.0), pic_pos, false);
        c = pictures.rgb;
        c = c * (1.0 - front.a) + front.rgb;
    }
    // Picture settings from the menu: saturation, then gamma
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(mix(vec3(luma), c, pc.effects.y), 0.0, 1.0);
    c = pow(c, vec3(1.0 / pc.effects.z));
    if (pc.crosshair2.x >= 0.0) {
        vec4 x = crosshair(uv * pc.window, pc.crosshair2.xy, vec3(0.2, 0.9, 1.0));
        c = mix(c, x.rgb, x.a);
    }
    if (pc.crosshair.x >= 0.0) {
        vec4 x = crosshair(uv * pc.window, pc.crosshair, vec3(1.0, 0.95, 0.2));
        c = mix(c, x.rgb, x.a);
    }
    color = vec4(c, 1.0);
}
