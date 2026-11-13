// Foliage wind, shared by ModelVertex.glsl and ShadowDepth.vert.glsl: both MUST apply it identically, or the shadows
// (and the depth pre-pass) would not move with the leaves. Opt-in per material (Material::Foliage -> uFoliage); with
// it off the offset is exactly zero and nothing else about the vertex changes.
//
// Three layers, coarse to fine, all smooth functions of position (no per-cell hashes: neighbouring vertices of one
// twig or leaf card always move together, so nothing tears or shivers):
//   1. Lean: the whole plant bends downwind with the scene's gusts (WindCommon.glsl) on a height mask, keeping each
//      vertex's distance from the base so the top arcs instead of stretching.
//   2. Branches: away from the trunk axis, branches bob and sway on a ~2.5 m wavelength field. The same for the bark
//      and the leaf materials of a plant, so the leaves stay on their branches.
//   3. Flutter (leaf materials only, uWindFlutter > 0): a few cm along the leaf normal on a ~0.3 m wavelength field.
// The fine layers fade with distance from the camera (2: 60-90 m, 3: 25-40 m); far plants only lean, like impostors.
#include "WindCommon.glsl"
uniform int   uFoliage;
uniform float uWindStrength;    // metres the top of uWindHeight leans in a mean gust (x the scene's uWindGlobal)
uniform float uWindFrequency;   // the plant's own sway rate
uniform float uWindFlutter;     // leaf flutter amount: 0 none (bark), 1 ~6 cm
uniform float uWindHeight;      // metres of local Y that map to a full mask
uniform int   uWindVertexMask;  // 1 = the mask is the vertex colour's red (painted: trunk 0, canopy tips 1)

// World-space offset for one vertex. lp / ln: the vertex position and normal in the mesh's own space; model: its
// transform (the plant's base is model[3], so the whole plant shares one gust and one phase).
vec3 FoliageWindOffsetAt(vec3 lp, vec3 ln, vec4 color, mat4 model, float t) {
    if (uFoliage == 0 || (uWindStrength <= 0.0 && uWindFlutter <= 0.0)) return vec3(0.0); // still: moss, debris
    precise vec3 origin = model[3].xyz;
    precise float H = max(uWindHeight, 0.01);
    precise float h = uWindVertexMask == 1 ? color.r : clamp(lp.y / H, 0.0, 1.0);

    // 1. Lean, length-preserving about the base.
    float g;
    precise vec2 lean = WindMainLean(origin.xz, t, uWindStrength, uWindFrequency, g) * (h * h);
    precise vec3 rel = mat3(model) * lp;
    precise vec3 bent = rel + vec3(lean.x, 0.0, lean.y);
    bent *= length(rel) / max(length(bent), 1e-4);
    precise vec3 off = bent - rel;

    precise float d = distance(origin, uWindCamPos);
    precise float tf = t * uWindFrequency;

    // 2. Branches: grows with the distance from the trunk axis (full at 30% of the plant's height) and up the plant.
    precise float branch = clamp(length(lp.xz) / (0.3 * H), 0.0, 1.0) * smoothstep(0.1, 0.5, h)
                         * (1.0 - smoothstep(60.0, 90.0, d));
    if (branch > 0.0) {
        precise float a = 0.3 * uWindStrength * uWindGlobal * branch * (0.4 + 0.6 * g);
        precise float bob = sin(tf * 2.6 + dot(lp, vec3(2.1, 1.3, 1.7)));
        precise float sw  = sin(tf * 1.9 + dot(lp, vec3(-1.1, 1.9, 0.8)));
        off += vec3(uWindDir.x * sw * 0.5, bob, uWindDir.y * sw * 0.5) * a;
    }

    // 3. Leaf flutter: mostly along the leaf's normal, a little downwind, stronger in a gust.
    precise float fl = uWindFlutter * smoothstep(0.1, 0.5, h) * (1.0 - smoothstep(25.0, 40.0, d));
    if (fl > 0.0) {
        precise vec3 n = mat3(model) * ln;
        n *= inversesqrt(max(dot(n, n), 1e-8));
        precise float f = sin(tf * 13.0 + dot(lp, vec3(13.0, 17.0, 11.0)) + 2.0 * g); // the gust shifts the beat
        precise float a = 0.06 * fl * uWindGlobal * (0.3 + 0.7 * g);
        off += (n * 0.75 + vec3(uWindDir.x, 0.0, uWindDir.y) * 0.25) * (f * a);
    }
    return off;
}

vec3 FoliageWindOffset(vec3 lp, vec3 ln, vec4 color, mat4 model) {
    return FoliageWindOffsetAt(lp, ln, color, model, uWindTime);
}
