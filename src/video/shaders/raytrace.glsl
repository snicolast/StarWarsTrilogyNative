// Ray traced effects (r3d_raytrace.h): the opaque world of this image in an
// acceleration structure, and the texture of its cut-out triangles. The
// includer defines RT_SET (the descriptor set of the 3D pass, whose bindings
// 6 and 7 these are) and declares texture_ram and replacements in it.
#include "r3d_texture.glsl"

// R3DRayTracer instance masks: casters near the ground, what is higher up, and a
// native scene's own casters (DOGFIGHT's cockpit: solid triangles only, in the last viewport's view space)
const uint RT_LOW = 1u, RT_HIGH = 2u, RT_FIXED = 4u;

layout(set = RT_SET, binding = 6) uniform accelerationStructureEXT world_rays;
struct Cutout {
    vec2 uv[3];
    uint tex, flags, replace;
    float alpha;
};
layout(std430, set = RT_SET, binding = 7) readonly buffer Cutouts { Cutout cutouts[]; };

// Whether a ray stops on a cut-out triangle where it crossed it: its texture's
// alpha there, nearest texel of the top level, as the alpha test would see it
bool cutout_blocks(uint index, vec2 bary) {
    Cutout c = cutouts[index];
    vec2 uv = c.uv[0] * (1.0 - bary.x - bary.y) + c.uv[1] * bary.x + c.uv[2] * bary.y;
    float a;
    if (c.replace != 0u) {
        if ((c.flags & MIRROR_U) != 0u) { float m = mod(uv.x, 2.0); uv.x = m > 1.0 ? 2.0 - m : m; }
        if ((c.flags & MIRROR_V) != 0u) { float m = mod(uv.y, 2.0); uv.y = m > 1.0 ? 2.0 - m : m; }
        a = textureLod(replacements[nonuniformEXT(c.replace)], uv, 0.0).a;
    } else {
        ivec2 base = ivec2(int(c.tex & 2047u), int((c.tex >> 11) & 2047u));
        ivec2 size = ivec2(32 << int((c.tex >> 22) & 7u), 32 << int((c.tex >> 25) & 7u));
        ivec2 t = ivec2(floor(uv * vec2(size)));
        t.x = wrap(t.x, size.x, (c.flags & MIRROR_U) != 0u);
        t.y = wrap(t.y, size.y, (c.flags & MIRROR_V) != 0u);
        ivec2 p = ivec2((base.x + t.x) & 2047, (base.y & 1024) | ((base.y + t.y) & 1023));
        a = decode(texelFetch(texture_ram, p, 0).r, (c.tex >> 28) & 15u).a;
    }
    // Alpha test: what the 3D pass keeps; texture alpha: only its solid texels
    // are in the opaque pass
    return (c.flags & ALPHA_TEST) != 0u ? a >= 32.0 / 255.0 : a * c.alpha >= 0.5;
}

// Distance to the first surface along `dir` (view space, normalised) from
// `origin` within `range`, or a negative number if there is none. mask: the
// instances to look at (RT_LOW, RT_HIGH); any: the first surface found will
// do (is it hidden?) rather than the nearest one
float rt_trace(vec3 origin, vec3 dir, float range, uint mask, bool any) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, world_rays, any ? gl_RayFlagsTerminateOnFirstHitEXT : gl_RayFlagsNoneEXT, mask, origin,
                          0.0, dir, range);
    while (rayQueryProceedEXT(q)) {
        // Only cut-out triangles are candidates; the instance's custom index is where its cut-outs start
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            cutout_blocks(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false) +
                               rayQueryGetIntersectionPrimitiveIndexEXT(q, false)),
                          rayQueryGetIntersectionBarycentricsEXT(q, false)))
            rayQueryConfirmIntersectionEXT(q);
    }
    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT
               ? rayQueryGetIntersectionTEXT(q, true)
               : -1.0;
}
