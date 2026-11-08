#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#ifdef RAY_TRACING
#extension GL_EXT_ray_query : require
#endif
// Real3D polygon shading: texture decode from the raw 16-bit texture RAM,
// sun + ambient lighting, fixed shading, specular and fog
// (docs/real3d_spec.md sections 6 and 7). Textures are filtered with the
// mip levels the game uploads (trilinear) plus anisotropic taps.
layout(location = 0) in vec3 view_pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec4 color;
layout(location = 4) flat in uint tex;
layout(location = 5) flat in uint flags;
layout(location = 6) in vec3 params;
layout(location = 7) flat in uint replace;
layout(location = 8) in vec4 clip_now;
layout(location = 9) in vec4 clip_before;

layout(set = 0, binding = 0) uniform usampler2D texture_ram;   // 2048x2048 R16_UINT
#ifdef FEW_IMAGES
// GPUs that take few images per shader (r3d_renderer.cpp): the texture pack gets what fits
layout(constant_id = 0) const int REPLACE_SLOTS = 4096;
layout(set = 0, binding = 1) uniform sampler2D replacements[REPLACE_SLOTS];
#else
layout(set = 0, binding = 1) uniform sampler2D replacements[4096];   // texture pack, RGBA8 with mips
#endif

layout(push_constant) uniform Viewport {
    mat4 proj;
    vec4 sun;
    vec4 fog_color;
    vec4 params;
    vec4 quality;
} vp;

layout(set = 0, binding = 3) uniform sampler2D layer2d_back;   // 2D backdrop, for the distance haze
layout(set = 0, binding = 4) uniform sampler2D shadow_map;     // sun shadows (depth seen from the sun)
layout(set = 0, binding = 5) uniform sampler2D scene_depth;    // the opaque world's depth (soft particles)

#define ATMOSPHERE_BINDING 2
#define ATMOSPHERE_BACKDROP
#include "atmosphere.glsl"

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_motion;   // DLAA only: pixels from here to the previous image
layout(location = 2) out vec4 out_glow;     // bloom: the bright part of luminous world surfaces

#include "r3d_texture.glsl"
#ifdef RAY_TRACING
#define RT_SET 0
#include "raytrace.glsl"
#endif

// Sun shadows (not in the original): how much of the sun reaches this point
// of the game world, 0..1, from the shadow map drawn from the sun this frame
#ifdef RAY_TRACING
float traced_shadow(vec3 n);
#endif
float sun_shadow(vec3 n) {
    if (atm.shadow.x <= 0.0 || vp.sun.w <= 0.0 || (flags & (WORLD | NO_SHADOW)) != WORLD) return 1.0;
#ifdef RAY_TRACING
    // rt.x 2: traced on the left half only, for comparing (LW_RTSPLIT)
    if (atm.rt.x > 1.5 ? gl_FragCoord.x * atm.backdrop.x < 0.5 : atm.rt.x > 0.0) return traced_shadow(n);
#endif
    float nl = length(n);
    vec3 pos = view_pos + (nl > 0.0 ? n / nl : vec3(0.0)) * atm.shadow.z;
    vec4 lc = atm.shadow_matrix * vec4(pos, 1.0);
    vec2 uv = lc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || lc.z <= 0.0) return 1.0;
    float lit = 0.0;
    vec2 t = vec2(atm.shadow.y);
#ifdef PLATFORM_MAC
    // 4x4 texels around the point, compared one by one, each weighted by how
    // close it is (a tent): soft edges that slide smoothly from texel to texel
    vec2 st = uv / t - 0.5, cell = floor(st), f = st - cell;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            vec4 d = textureGather(shadow_map, (cell + vec2(x, y) * 2.0) * t, 0);
            vec4 hit = vec4(greaterThan(d, vec4(lc.z + atm.shadow.w)));
            // Space: a caster far in front of the surface (a ship before a
            // planet or the Death Star) is not near enough to shadow it
            if (atm.soft.w > 0.0) hit *= vec4(lessThan(d, vec4(lc.z + atm.soft.w)));
            vec2 w0 = vec2(x == 0 ? 1.0 - f.x : 1.0, y == 0 ? 1.0 - f.y : 1.0);
            vec2 w1 = vec2(x == 0 ? 1.0 : f.x, y == 0 ? 1.0 : f.y);
            // textureGather's order: (0,1) (1,1) (1,0) (0,0)
            lit += dot(hit, vec4(w0.x * w1.y, w1.x * w1.y, w1.x * w0.y, w0.x * w0.y));
        }
    lit = 1.0 - lit / 9.0;
#else
    // 4x4 texels around the point, compared one by one (soft edges)
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            vec4 d = textureGather(shadow_map, uv + (vec2(x, y) - 0.5) * 2.0 * t, 0);
            vec4 hit = vec4(greaterThan(d, vec4(lc.z + atm.shadow.w)));
            // Space: a caster far in front of the surface (a ship before a
            // planet or the Death Star) is not near enough to shadow it
            if (atm.soft.w > 0.0) hit *= vec4(lessThan(d, vec4(lc.z + atm.soft.w)));
            lit += dot(hit, vec4(1.0));
        }
    lit = 1.0 - lit / 16.0;
#endif
    // Fades out towards the edge of the shadow map
    vec2 edge = min(uv, 1.0 - uv);
    float fade = smoothstep(0.0, 0.08, min(edge.x, edge.y));
    return mix(1.0, lit, atm.shadow.x * fade);
}

#ifdef RAY_TRACING
// Sun shadows traced through the world: a few rays spread over the sun's
// disc, so the shadow is sharp where the caster touches the ground and
// softens with distance from it. Most pixels are fully lit or fully in
// shadow: when the first two rays agree, the others are not traced (only
// the penumbra takes them all)
float traced_shadow(vec3 n) {
    float nl = length(n);
    vec3 N = nl > 0.0 ? n / nl : vec3(0.0);
    vec3 L = normalize(vp.sun.xyz);
    // Off the surface by a little more as it gets farther (depth precision)
    vec3 origin = view_pos + (N * 1.5 + L * 0.5) * (0.02 + 0.0015 * length(view_pos));
    vec3 X = normalize(cross(abs(L.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), L));
    vec3 Y = cross(L, X);
    int rays = int(atm.rt.z);
    float turn = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + atm.rt.w) * 6.2831853;
    float lit = 0.0;
    for (int i = 0; i < rays; i++) {
        // Points of a golden-angle spiral over the disc, turned per pixel
        float r = sqrt((float(i) + 0.5) / float(rays)) * atm.rt.y;
        float a = float(i) * 2.3999632 + turn;
        vec3 dir = normalize(L + (X * cos(a) + Y * sin(a)) * r);
        if (rt_trace(origin, dir, atm.soft.w > 0.0 ? 600.0 : 2000.0, RT_LOW, true) < 0.0) lit += 1.0;
        if (i == 1 && rays > 2 && (lit == 0.0 || lit == 2.0)) return mix(1.0, lit * 0.5, atm.shadow.x);
    }
    return mix(1.0, lit / float(rays), atm.shadow.x);
}
#endif

vec4 fetch(ivec2 base, ivec2 size, ivec2 c, uint fmt) {
    c.x = wrap(c.x, size.x, (flags & MIRROR_U) != 0u);
    c.y = wrap(c.y, size.y, (flags & MIRROR_V) != 0u);
    ivec2 p = ivec2((base.x + c.x) & 2047, (base.y & 1024) | ((base.y + c.y) & 1023));
    vec4 t = decode(texelFetch(texture_ram, p, 0).r, fmt);
    // Polygons without texture alpha show every texel, including the ones
    // marked transparent: their colour must survive the premultiplied filter
    if ((flags & (TEX_ALPHA | ALPHA_TEST)) == 0u) t.a = 1.0;
    return t;
}

// Mip level positions inside a texture page (spec 6.4)
const int MIP_X[12] = int[](0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047);
const int MIP_Y[11] = int[](0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023);

// Bilinear sample of one mip level; st in level-0 texels. Premultiplied
// result: the colour of transparent texels (often sky blue in the texture
// data) must not bleed into the edges of foliage.
vec4 bilinear(ivec2 base, ivec2 size, vec2 st, int level, uint fmt) {
    ivec2 b = ivec2(MIP_X[level] + (base.x >> level), (base.y & 1024) | (MIP_Y[level] + ((base.y & 1023) >> level)));
    ivec2 s = max(size >> level, ivec2(1));
    vec2 p = st / float(1 << level) - 0.5;
    ivec2 i0 = ivec2(floor(p));
    vec2 f = p - vec2(i0);
    ivec2 i1 = i0 + 1;
    // Axes without wrap smoothing (and all of an alpha-tested texture) repeat
    // without blending across the edge: both taps stay in the repetition the
    // sample falls in (spec 6.6)
    bvec2 smooth_edge = bvec2((flags & SMOOTH_U) != 0u, (flags & SMOOTH_V) != 0u);
    if ((flags & ALPHA_TEST) != 0u) smooth_edge = bvec2(false);
    ivec2 first = ivec2(floor((p + 0.5) / vec2(s))) * s;
    for (int k = 0; k < 2; k++)
        if (!smooth_edge[k]) {
            i0[k] = clamp(i0[k], first[k], first[k] + s[k] - 1);
            i1[k] = clamp(i1[k], first[k], first[k] + s[k] - 1);
        }
    vec4 a = fetch(b, s, i0, fmt);
    vec4 bb = fetch(b, s, ivec2(i1.x, i0.y), fmt);
    vec4 c = fetch(b, s, ivec2(i0.x, i1.y), fmt);
    vec4 d = fetch(b, s, i1, fmt);
    a.rgb *= a.a;
    bb.rgb *= bb.a;
    c.rgb *= c.a;
    d.rgb *= d.a;
    return mix(mix(a, bb, f.x), mix(c, d, f.x), f.y);
}

// duv_dx/duv_dy: screen-space derivatives of uv, taken in uniform control flow
vec4 sample_texture(vec2 duv_dx, vec2 duv_dy) {
    if (replace != 0u) {   // texture pack image: hardware trilinear + anisotropic filtering
        vec2 u = uv;
        if ((flags & MIRROR_U) != 0u) { float m = mod(u.x, 2.0); u.x = m > 1.0 ? 2.0 - m : m; }
        if ((flags & MIRROR_V) != 0u) { float m = mod(u.y, 2.0); u.y = m > 1.0 ? 2.0 - m : m; }
        return textureGrad(replacements[nonuniformEXT(replace)], u, duv_dx, duv_dy);
    }
    ivec2 base = ivec2(int(tex & 2047u), int((tex >> 11) & 2047u));
    ivec2 size = ivec2(32 << int((tex >> 22) & 7u), 32 << int((tex >> 25) & 7u));
    uint fmt = (tex >> 28) & 15u;
    vec2 st = uv * vec2(size);
    vec4 sum;
    float max_aniso = vp.quality.y;
    if (max_aniso < 1.0) {
        sum = bilinear(base, size, st, 0, fmt);   // original look: base level only
    } else {
        // Footprint of the pixel in level-0 texels: the long axis gets several
        // taps, the short one picks the mip level
        vec2 dx = duv_dx * vec2(size), dy = duv_dy * vec2(size);
        float lx = length(dx), ly = length(dy);
        float pmax = max(lx, ly), pmin = max(min(lx, ly), 1e-6);
        float taps = clamp(ceil(pmax / pmin), 1.0, max_aniso);
        // Levels are stored while both sides are >= 4 before halving
        int max_level = max(findMSB(min(size.x, size.y)) - 1, 0);
        float lod = clamp(log2(max(pmax / taps, 1e-6)), 0.0, float(max_level));
        int l0 = int(floor(lod));
        int l1 = min(l0 + 1, max_level);
        float lf = lod - float(l0);
        vec2 axis = lx > ly ? dx : dy;
        sum = vec4(0.0);
        int n = int(taps);
        for (int i = 0; i < n; i++) {
            vec2 p = st + axis * ((float(i) + 0.5) / taps - 0.5);
            vec4 s = bilinear(base, size, p, l0, fmt);
            if (lf > 0.0 && l1 != l0) s = mix(s, bilinear(base, size, p, l1, fmt), lf);
            sum += s;
        }
        sum /= taps;
    }
    sum.rgb = sum.a > 0.0 ? sum.rgb / sum.a : vec3(0.0);
    return sum;
}

// Surface detail (not in the original): the texture's own light and dark
// read as height, so the light, the shadows and the rim pick out the grain of
// stone, bark, sheet metal and skin. Brightness at an offset (uv units).
float relief_height(vec2 offset, vec2 duv_dx, vec2 duv_dy, int level) {
    vec4 s;
    if (replace != 0u) {
        s = textureGrad(replacements[nonuniformEXT(replace)], uv + offset, duv_dx, duv_dy);
    } else {
        ivec2 base = ivec2(int(tex & 2047u), int((tex >> 11) & 2047u));
        ivec2 size = ivec2(32 << int((tex >> 22) & 7u), 32 << int((tex >> 25) & 7u));
        s = bilinear(base, size, (uv + offset) * vec2(size), level, (tex >> 28) & 15u);
        s.rgb = s.a > 0.0 ? s.rgb / s.a : vec3(0.0);
    }
    return dot(s.rgb, vec3(0.2126, 0.7152, 0.0722));
}

// The normal tilted by the texture's height slopes, in the surface's own
// frame worked out from the screen derivatives (no tangents in the data)
vec3 relief_normal(vec3 n, float h0, vec3 dpx, vec3 dpy, vec2 duv_dx, vec2 duv_dy) {
    float strength = atm.relief.x * clamp(1.0 - (-view_pos.z) / atm.relief.y, 0.0, 1.0);
    float nl = length(n);
    if (strength <= 0.0 || nl <= 0.0) return n;
    vec3 N = n / nl;
    vec2 size = replace != 0u ? vec2(textureSize(replacements[nonuniformEXT(replace)], 0))
                              : vec2(32 << int((tex >> 22) & 7u), 32 << int((tex >> 25) & 7u));
    // One step = the pixel's footprint (at least a texel), at the matching mip level
    vec2 footprint = max(abs(duv_dx), abs(duv_dy)) * size;
    float texels = max(max(footprint.x, footprint.y), 1.0);
    int max_level = max(findMSB(int(min(size.x, size.y))) - 1, 0);
    int level = replace != 0u ? 0 : clamp(int(floor(log2(texels))), 0, max_level);
    float hu = relief_height(vec2(texels / size.x, 0.0), duv_dx, duv_dy, level);
    float hv = relief_height(vec2(0.0, texels / size.y), duv_dx, duv_dy, level);
    vec3 dp2perp = cross(dpy, N), dp1perp = cross(N, dpx);
    vec3 T = dp2perp * duv_dx.x + dp1perp * duv_dy.x;
    vec3 B = dp2perp * duv_dx.y + dp1perp * duv_dy.y;
    float inv = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-30));
    vec3 tilted = normalize(N - strength * ((hu - h0) * T * inv + (hv - h0) * B * inv));
    return tilted * nl;
}

// Spotlight of the original (spec 7.1): a lobe around a point of the view
// (spot_lobe), fading with distance past the extent. x: lobe on the light,
// y: lobe on the fog
vec2 spotlight() {
    float z = -view_pos.z;
    if (z <= 0.0 || atm.spot.z <= 0.0) return vec2(0.0);
    float ellipse = spot_lobe(view_pos.xy / z);
    float extent = atm.spot_fog.w, range = 0.0;
    if (extent > 0.0) {
        float d = min(0.0, atm.spot_color.w + extent - z);
        float k = d / (1.0 + extent) - 1.0;
        range = 1.0 / (k * k);
    }
    return vec2(range, z >= atm.spot_color.w ? 1.0 : 0.0) * ellipse;
}

// EXPLOSIONS: ENHANCED (r3d_explosions.cpp, not in the original): the game's
// explosion sprite becomes a fireball of billowing noise, white hot at first,
// then orange, red and soot as it burns out, with a flash and a thin shock
// wave; each explosion also throws sparks (streaks), burning pieces trailing
// puffs of smoke and tumbling pieces of hull lit by the sun. tex: kind in
// bits 0-1, a seed in bits 8-23; params.x: age 0..1; params.z: the
// fireball's radius, a spark's brightness, a puff's size, a piece's heat
float expl_fbm(vec3 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; i++) {
        s += a * atm_noise(p);
        p = p * 2.07 + vec3(1.7, 9.2, 3.1);
        a *= 0.5;
    }
    return s / 0.9375;
}

// Fire by its heat: 0 soot, then deep red, orange, yellow, 1 white hot
vec3 expl_fire(float h) {
    vec3 c = mix(vec3(0.05, 0.04, 0.035), vec3(0.55, 0.07, 0.015), smoothstep(0.0, 0.3, h));
    c = mix(c, vec3(1.0, 0.38, 0.06), smoothstep(0.25, 0.55, h));
    c = mix(c, vec3(1.0, 0.78, 0.32), smoothstep(0.5, 0.8, h));
    return mix(c, vec3(1.0, 0.97, 0.9), smoothstep(0.78, 1.0, h));
}

vec4 explosion(out float glow_k) {
    uint kind = tex & 3u;
    float seed = float((tex >> 8) & 0xFFFFu) / 65535.0;
    float t = params.x;
    vec2 q = uv;
    vec4 c = vec4(0.0);
    glow_k = 1.0;
    if (kind == 0u) {   // the fireball; |q| = 1 is the sprite's radius
        float grow = 1.0 - pow(1.0 - clamp(t * 2.8, 0.0, 1.0), 3.0);
        float rad = mix(0.3, 0.95, grow) + 0.12 * t;
        float d = length(q) / rad;
        vec3 sp = vec3(q / rad, sqrt(max(1.0 - d * d, 0.0)));   // as if on a ball
        vec3 off = vec3(37.0, 11.0, 23.0) * seed;
        float n = expl_fbm(sp * 1.7 - normalize(sp + vec3(0.0, 0.0, 1e-3)) * t * 2.0 + off);   // billows rolling out
        float n2 = expl_fbm(sp * 5.5 + off.zxy + vec3(0.0, 0.0, t * 1.5));
        float shape = 1.0 - d + (n - 0.5) * 1.4 + (n2 - 0.5) * 0.4;
        float density = smoothstep(0.0, 0.3, shape);
        float cool = smoothstep(0.08, 0.8, t);
        float heat = clamp((shape * 1.1 + (n2 - 0.5) * 0.9) * (1.0 - cool) * 1.25 + (1.0 - cool) * 0.1, 0.0, 1.0);
        vec3 fire = expl_fire(heat) * (0.8 + 4.0 * heat * heat * heat);
        vec3 soot = vec3(0.07, 0.065, 0.06) * (0.6 + 0.9 * n2) + expl_fire(heat) * 0.15;
        float hot = smoothstep(0.12, 0.4, heat);
        c.rgb = mix(soot, fire, hot);
        c.a = density * mix(0.65 * (1.0 - smoothstep(0.55, 1.0, t)), 1.0, hot);
        float r2 = dot(q, q);
        float flash = (1.0 - smoothstep(0.0, 0.12, t)) * exp(-r2 * 5.0);
        c.rgb += vec3(1.0, 0.95, 0.85) * flash * 8.0;
        c.a = max(c.a, clamp(flash * 1.5, 0.0, 1.0));
        float tr = t / 0.3;
        if (tr < 1.0) {
            float rr = 0.25 + 1.3 * sqrt(tr), w = 0.04 + 0.05 * tr, x = (sqrt(r2) - rr) / w;
            float ring = exp(-x * x) * pow(1.0 - tr, 1.5) * (0.25 + 1.2 * atm_noise(vec3(q * 6.0, seed * 50.0)));
            c.rgb += vec3(0.75, 0.85, 1.0) * ring * 1.3;
            c.a = max(c.a, ring * 0.35);
        }
    } else if (kind == 1u) {   // a spark: a streak, its head (x = 1) brightest
        float across = 1.0 - q.y * q.y;
        float along = smoothstep(0.0, 1.0, q.x);
        c.rgb = color.rgb * params.z * (0.5 + 0.5 * along) * mix(1.0, 0.6, t);
        c.a = across * across * along * (1.0 - smoothstep(0.5, 1.0, t));
    } else if (kind == 2u) {   // a puff of smoke
        float n = expl_fbm(vec3(q * 1.6, seed * 40.0 + t * 0.8));
        float a = 1.0 - smoothstep(0.25, 1.0, length(q) + (n - 0.5) * 0.7);
        c.rgb = color.rgb * mix(0.75, 1.15, n) * clamp(vp.params.x + 0.3 * vp.sun.w, 0.4, 1.2);
        c.a = a * color.a * smoothstep(0.0, 0.12, t) * (1.0 - smoothstep(0.4, 1.0, t));
        glow_k = 0.0;
    } else {   // a piece of hull: lit by the sun, red hot at first
        vec3 n = normalize(normal);
        if (dot(n, view_pos) > 0.0) n = -n;
        vec3 l = length(vp.sun.xyz) > 0.0 ? normalize(vp.sun.xyz) : vec3(0.0, 1.0, 0.0);
        float s = max(dot(l, n), 0.0) * vp.sun.w + 0.5 * vp.params.x;
        c.rgb = color.rgb * min(s, 1.4);
        vec3 h = normalize(l + normalize(-view_pos));
        c.rgb += vec3(pow(max(dot(n, h), 0.0), 24.0) * vp.sun.w * 0.6);
        float heat = params.z;
        c.rgb += expl_fire(0.35 + 0.5 * heat) * heat * heat * 3.0 * (0.6 + 0.4 * atm_noise(vec3(q * 3.0, seed * 30.0)));
        c.a = 1.0 - smoothstep(0.75, 1.0, t);
        glow_k = heat;
    }
    return c;
}

void main() {
    vec2 duv_dx = dFdx(uv), duv_dy = dFdy(uv);
    vec3 dpx = dFdx(view_pos), dpy = dFdy(view_pos);   // the surface's frame, for the surface detail
    // Motion vector in pixels; NDC is linear across the screen, so its
    // derivatives give the viewport size without passing it in
    vec2 ndc = clip_now.xy / clip_now.w;
    vec2 px_per_ndc = 1.0 / vec2(dFdx(ndc.x), dFdy(ndc.y));
    out_motion = (clip_before.xy / clip_before.w - ndc) * px_per_ndc;
    gl_SampleMask[0] = -1;   // a statically written mask must be set on every path
    vec4 col = color;
    if ((flags & EXPLOSION) != 0u) {   // only in the translucent pass
        if (vp.quality.x <= -1000.0 || vp.quality.x > 0.0 || vp.fog_color.w < 0.0) discard;
        float glow_k;
        vec4 c = explosion(glow_k);
        uint kind = tex & 3u;
        if (atm.soft.x > 0.0) {   // soft where it meets the hulls behind, over its size
            float d = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
            float soft = kind == 0u || kind == 2u ? max(atm.soft.y, params.z * 0.4) : atm.soft.y;
            if (d > 0.0) c.a *= clamp((view_pos.z + 1e-3 / d) / soft, 0.0, 1.0);
        }
        if (c.a < 1.0 / 255.0) discard;
        c.rgb = min(c.rgb, vec3(atm.soft.z > 0.5 ? 8.0 : 1.0));
        float fog = params.y * clamp(vp.params.z + (-view_pos.z) * vp.params.y, 0.0, 1.0);
        c.rgb = mix(c.rgb, vp.fog_color.rgb * vp.fog_color.a, fog);
        out_color = c;
        float peak = max(c.r, max(c.g, c.b));
        out_glow = vec4(c.rgb * smoothstep(0.6, 1.0, peak) * glow_k, c.a);
        return;
    }
    if ((flags & TRANSLATOR) != 0u) col.rgb *= 16.0;

    // Sun shadow pass (fog_color.w < 0): only what stands near the ground casts
    // shadows; canopies and roofs high above would put whole scenes in the dark
    if (vp.fog_color.w < 0.0 && dot(view_pos, vp.sun.xyz) + vp.fog_color.x > vp.fog_color.y) discard;

    vec4 t = vec4(1.0);
    if ((flags & TEXTURED) != 0u) {
        t = sample_texture(duv_dx, duv_dy);
        if ((flags & INVERT_RGB) != 0u) t.rgb = 1.0 - t.rgb;
        if ((flags & (TEX_ALPHA | ALPHA_TEST)) == 0u) t.a = 1.0;
        if ((flags & ALPHA_TEST) != 0u && t.a < 32.0 / 255.0) discard;
    }

    // Space (r3d_share_world): the hulls of the films are a weathered light
    // grey, not the game's clean white: whites come down more than the
    // darker panel lines (more texture), with a faint grime from panel to
    // panel. Not the luminous parts (engines, lasers), nor a planet: it lies
    // hundreds of thousands of units away (Endor in the last stage: z 150k
    // to 950k), where ships and the Death Star (Yavin: about 15k) never are,
    // and its panels of grime were big squares over the clouds
    float hull_far = 1.0 - smoothstep(40000.0, 80000.0, length(view_pos));
    if (atm.relief.z > 0.0 && hull_far > 0.0 && (flags & (WORLD | LUMINOUS | TEXTURED)) == (WORLD | TEXTURED)) {
        float k = atm.relief.z * hull_far * ((flags & DEATH_STAR) != 0u ? 0.45 : 1.0);   // the Death Star stays lighter
        vec3 g = pow(t.rgb, vec3(1.35)) * 0.86;
        float lum = dot(g, vec3(0.2126, 0.7152, 0.0722));
        g = mix(vec3(lum), g, 0.85) * vec3(0.98, 0.985, 1.0);
        ivec2 tsize = ivec2(32 << int((tex >> 22) & 7u), 32 << int((tex >> 25) & 7u));
        vec2 panel = floor(uv * vec2(tsize) / 8.0);
        float grime = fract(sin(dot(panel, vec2(41.31, 289.17))) * 15731.743);
        g *= 0.92 + 0.12 * grime;
        t.rgb = mix(t.rgb, g, k);
    }
    // The desert of Boba Fett's duel: its texture is small and stretched, so
    // near the eye it is a flat colour. Wind ripples (a long wave bent by a
    // slow noise, a lit and a shaded side) and fine grains, both in the
    // texture's space so they lie on the ground; they fade where they would
    // be smaller than a pixel, and it loses some of the glare of a luminous
    // polygon (it was almost white towards the horizon)
    bool sand = (flags & SAND) != 0u;
    if (sand) {
        ivec2 tsize = ivec2(32 << int((tex >> 22) & 7u), 32 << int((tex >> 25) & 7u));
        vec2 st = uv * vec2(tsize);
        float texel_px = max(length(dFdx(st)), length(dFdy(st)));
        vec2 q = st * 0.35;
        float warp = atm_noise(vec3(q * 0.18, 1.7)) * 6.0 + atm_noise(vec3(q * 0.6, 4.1)) * 1.2;
        float phase = dot(q, vec2(0.92, 0.38)) + warp;
        float ripple = sin(phase * 6.2831853);
        float lit = ripple + 0.6 * sin(phase * 12.566 + 1.1);   // sharper crests on one side
        float ripple_fade = 1.0 - smoothstep(0.06, 0.25, texel_px);   // ~ 3 to 1 pixels per ripple
        float grain = atm_noise(vec3(st * 9.0, 0.3)) * 0.6 + atm_noise(vec3(st * 23.0, 2.9)) * 0.4;
        float grain_fade = 1.0 - smoothstep(0.02, 0.09, texel_px);
        float tone = 1.0 + 0.07 * lit * ripple_fade + 0.16 * (grain - 0.5) * grain_fade;
        // a slow change of colour over the dunes, also far away
        float dune = atm_noise(vec3(st * 0.045, 7.3));
        t.rgb *= tone * mix(vec3(0.93, 0.9, 0.86), vec3(1.03, 1.01, 0.97), dune);
    }
    float ambient = vp.params.x;
    float fixed_shade = params.x;
    if ((flags & LUMINOUS) != 0u)
        col.rgb *= (flags & FIXED_SHADE) != 0u ? 1.0 + fixed_shade + ambient : (sand ? 1.2 : 1.5);

    vec4 c = t * col;
    if (c.a < 1.0 / 32.0) discard;
    // Depth of the nearest see-through surface (spec 7.2); effects stay out of it
    if (vp.quality.x <= -1000.0) {
        if ((flags & (TEX_ALPHA | ALPHA_TEST | LUMINOUS)) != 0u) discard;
        return;
    }
    // Texture alpha (spec 7.2): solid texels belong to the opaque pass, the
    // see-through ones to the translucent pass
    if ((flags & TEX_ALPHA) != 0u && (vp.quality.x > 0.0 ? c.a < 1.0 : c.a >= 1.0)) discard;

    if ((flags & LUMINOUS) == 0u) {
        vec3 n = normal;
        if (atm.relief.x > 0.0 && (flags & (WORLD | TEXTURED)) == (WORLD | TEXTURED) && (flags & FIXED_SHADE) == 0u)
            n = relief_normal(n, dot(t.rgb, vec3(0.2126, 0.7152, 0.0722)), dpx, dpy, duv_dx, duv_dy);
        float s = (flags & FIXED_SHADE) != 0u ? fixed_shade : dot(vp.sun.xyz, n);
        s = clamp(s, -1.0, 1.0);
        s = max(s, 0.0);
        if ((flags & FIXED_SHADE) == 0u && s > 0.0) s *= sun_shadow(n);
        vec3 light = vec3(s * vp.sun.w + ambient);
        // Clear outdoor scenes (SUN): a stronger, warmer sun and a softer,
        // slightly bluer ambient (light from the sky) for more contrast
        if (atm.sunlight.z > 0.0 && (flags & WORLD) != 0u) {
            float w = atm.sunlight.z;
            light = s * vp.sun.w * atm.sunlight.x * vec3(1.0, 1.0 - 0.05 * w, 1.0 - 0.14 * w) +
                    ambient * atm.sunlight.y * vec3(1.0 - 0.06 * w, 1.0, 1.0 + 0.08 * w);
        }
        // The lightsabres (r3d_find_glows): a light along each blade, falling
        // off with the distance to its nearest point, wrapped a little round
        // the surfaces it meets; a glint of it on what shines
        vec3 glow_hi = vec3(0.0);
        for (int i = 0; i < int(atm.glow_info.x); i++) {
            vec3 ga = atm.glow[i * 3].xyz, gb = atm.glow[i * 3 + 1].xyz, ab = gb - ga;
            float gr = atm.glow[i * 3].w;
            float h = clamp(dot(view_pos - ga, ab) / max(dot(ab, ab), 1e-10), 0.0, 1.0);
            vec3 d = ga + ab * h - view_pos;
            float dist2 = dot(d, d);
            if (dist2 > 36.0 * gr * gr) continue;   // too far to matter (under 3%)
            float dist = sqrt(dist2);
            float att = 1.0 / (1.0 + dist * dist / (gr * gr));
            vec3 gl = d / max(dist, 1e-6);
            float ndl = length(n) > 0.0 ? max(dot(normalize(n), gl), 0.0) * 0.8 + 0.2 : 0.6;
            vec3 gcol = atm.glow[i * 3 + 2].rgb * atm.glow[i * 3 + 1].w * att;
            light += gcol * ndl;
            if (length(n) > 0.0 && (flags & (SPECULAR | VADER)) != 0u)
                glow_hi += gcol * pow(max(dot(normalize(n), normalize(gl + normalize(-view_pos))), 0.0), 48.0) *
                           ((flags & VADER) != 0u ? 1.0 : 0.5);
        }
        // The spotlight: a flat lobe, as in the original
        if ((flags & WORLD) != 0u && atm.spot.z > 0.0) light += atm.spot_color.rgb * spotlight().x;
        // The Death Star as in the films: a sunlit half and a night half
        // almost black, a thin bright edge along the lit limb, and on the
        // night side thousands of lights in the trenches of its surface
        vec3 night_lights = vec3(0.0);
        if ((flags & DEATH_STAR) != 0u && length(n) > 0.0 && length(vp.sun.xyz) > 0.01) {
            vec3 nn = normalize(n);
            float ndl = dot(nn, normalize(vp.sun.xyz));
            float day = smoothstep(-0.08, 0.35, ndl);
            light = vec3(0.035, 0.04, 0.055) + day * (light * 1.25 + vec3(0.05));
            float rim = pow(1.0 - clamp(dot(nn, normalize(-view_pos)), 0.0, 1.0), 3.0);
            light += rim * smoothstep(-0.1, 0.5, ndl) * vec3(0.55, 0.6, 0.7);
            // Lights: warm, one texel in some seventy of the darker (trench) texels,
            // their average glow where the texels are smaller than a pixel
            ivec2 size = ivec2(32 << int((tex >> 22) & 7u), 32 << int((tex >> 25) & 7u));
            vec2 st = uv * vec2(size);
            vec2 cell = floor(st);
            float h = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);
            float trench = 1.0 - smoothstep(0.22, 0.42, dot(t.rgb, vec3(0.2126, 0.7152, 0.0722)));
            float speck = step(0.985, h) * trench * (1.0 - smoothstep(0.25, 0.5, length(fract(st) - 0.5)));
            float texel_px = max(length(dFdx(st)), length(dFdy(st)));
            speck = mix(speck, 0.015 * 0.35 * trench, clamp(texel_px - 0.7, 0.0, 1.0));
            float tint = fract(h * 7.31);
            night_lights = (1.0 - day) * speck * 2.4 *
                           mix(vec3(1.0, 0.72, 0.38), vec3(1.0, 0.9, 0.7), step(0.8, tint));
        }
        c.rgb *= light;
        c.rgb += night_lights + glow_hi;
        // Vader (r3d_share_world): the black of his helmet and suit is glossy,
        // as in the films. A sharp highlight of the room's light, and the room
        // reflected at grazing angles (a cool blue from above, dark below);
        // only on the dark texels, the buttons and the grey keep their look
        if ((flags & VADER) != 0u && length(n) > 0.0) {
            vec3 nn = normalize(n), eye = normalize(-view_pos);
            float black = 1.0 - smoothstep(0.08, 0.3, dot(t.rgb, vec3(0.2126, 0.7152, 0.0722)));
            bool helmet = ((tex >> 16) & 0xFFFFu) == 0x0250u;   // its pages' helmet and mask: shinier
            float ndv = clamp(dot(nn, eye), 0.0, 1.0);
            float fres = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
            vec3 r = reflect(-eye, nn);
            float up = dot(r, atm.axes[1].xyz);
            vec3 room = mix(vec3(0.015, 0.018, 0.03), vec3(0.2, 0.27, 0.42), smoothstep(-0.3, 0.7, up));
            vec3 key = length(vp.sun.xyz) > 0.01 ? normalize(vp.sun.xyz) : normalize(atm.axes[1].xyz + eye + vec3(1e-4));
            vec3 hl = normalize(key + eye);
            float s_power = helmet ? 90.0 : 40.0;
            float hi = pow(max(dot(nn, hl), 0.0), s_power) * (helmet ? 1.6 : 0.7) * max(vp.sun.w, 0.35);
            // a broad soft sheen too, so the folds read in the dark
            float sheen = pow(max(dot(nn, hl), 0.0), 8.0) * 0.06;
            c.rgb += black * ((room * fres * (helmet ? 1.4 : 0.8)) + vec3(hi + sheen) * vec3(0.9, 0.95, 1.0));
        }
        if ((flags & SPECULAR) != 0u) {
            uint shin = (flags >> 10) & 3u;
            float powers[4] = float[](8.0, 16.0, 32.0, 64.0);
            float scales[4] = float[](1.6, 1.6, 2.4, 3.2);
            float f = pow(max(s, 0.0), powers[shin]) * scales[shin] * params.z * vp.sun.w *
                      (atm.sunlight.z > 0.0 && (flags & WORLD) != 0u ? atm.sunlight.x : 1.0);
            c.rgb += f;
        }
    }
    // The original clips the light at white; with HDR lighting the highlights
    // (sunlit surfaces, fire, specular) keep going and the composition's tone
    // curve rolls them off
    c.rgb = min(c.rgb, vec3(atm.soft.z > 0.5 ? 8.0 : 1.0));

    float fog = params.y * clamp(vp.params.z + (-view_pos.z) * vp.params.y, 0.0, 1.0);
    vec3 fog_color = vp.fog_color.rgb * vp.fog_color.a;
    if ((flags & WORLD) != 0u && fog > 0.0) fog_color = fog_color * mist_light(view_pos) + atm.spot_fog.rgb * spotlight().y;
    c.rgb = mix(c.rgb, fog_color, fog);
    // Optional distance haze
    if ((flags & WORLD) != 0u && atm.haze.a > 0.0)
        c.rgb = distance_haze(c.rgb, view_pos, float(flags >> 24u) / 255.0, fog);
    // The Death Star's trench fades into the dark ahead: its far stretches,
    // drawn as the game adds them, appear out of the black instead of popping
    if ((flags & WORLD) != 0u && atm.day_sky.y > 0.0) {
        float far = atm.day_sky.y;
        c.rgb *= 1.0 - atm.day_sky.z * smoothstep(0.35 * far, 0.95 * far, length(view_pos));
    }
    // Opaque pass: the colour written is always solid, since a filtered alpha
    // would let the 2D background show through. With MSAA, alpha-tested edges
    // become partial sample coverage instead (smooth foliage outlines).
    if (vp.quality.x > 0.0) {
        int samples = int(vp.quality.x);
        // DLAA: per-frame noise seed in the fractional part (0 = none)
        float seed = fract(vp.quality.x);
        if (samples > 1 && (flags & ALPHA_TEST) != 0u) {
#ifdef PLATFORM_MAC
            // Rounded the same way everywhere, a smooth alpha (the game's soft
            // blobs) shows one ring per sample: dithered, the rings dissolve
            float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
            int covered = clamp(int(t.a * float(samples) + dither), 0, samples);
#else
            int covered = clamp(int(t.a * float(samples) + 0.5), 0, samples);
#endif
            if (covered == 0) discard;
            gl_SampleMask[0] = (1 << covered) - 1;
        } else if (seed > 0.0 && (flags & ALPHA_TEST) != 0u) {
            // The same partial coverage spread over frames: the pixel is kept
            // with probability alpha and DLAA averages the frames into a
            // smooth outline. Interleaved gradient noise, shifted every frame.
            float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
            if (t.a < fract(noise + seed)) discard;
        }
        c.a = 1.0;
    }
    // Soft particles: smoke, fire and other translucent surfaces of the world
    // fade out where they meet what stands behind them, instead of a hard line
    if (vp.quality.x < 0.0 && atm.soft.x > 0.0 && (flags & WORLD) != 0u) {
        float d = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
        if (d > 0.0) c.a *= clamp((view_pos.z + 1e-3 / d) / atm.soft.y, 0.0, 1.0);
    }
    out_color = c;
    // Bloom source: fire, explosions and lights are drawn unlit (luminous);
    // only their bright part glows. Everything else writes no glow, so it
    // hides the glow of what is behind it.
    float peak = max(c.r, max(c.g, c.b));
    bool glows = (flags & (LUMINOUS | WORLD)) == (LUMINOUS | WORLD);
    // ... and, more gently, whatever else of the world is very bright (sunlit
    // surfaces and highlights above white with HDR lighting)
    vec3 glow = glows ? c.rgb * smoothstep(0.6, 1.0, peak)
                      : (flags & WORLD) != 0u ? c.rgb * smoothstep(0.75, 1.6, peak) * 0.6 : vec3(0.0);
    out_glow = vec4(glow, c.a);
}
