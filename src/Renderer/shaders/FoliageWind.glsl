// Foliage wind, shared by ModelVertex.glsl and ShadowDepth.vert.glsl: both MUST apply it identically, or the shadows
// (and the depth pre-pass) would not move with the leaves. Opt-in per material (Material::Foliage -> uFoliage); with
// it off the offset is exactly zero and nothing else about the vertex changes.
uniform int   uFoliage;
uniform float uWindTime;        // seconds, one value per frame (Material::WindTime)
uniform vec2  uWindDir;         // world-space gust direction on XZ, unit length
uniform float uWindStrength;    // metres the top of uWindHeight moves in a full gust
uniform float uWindFrequency;   // gust speed multiplier
uniform float uWindFlutter;     // 0 = slow bend only, 1 = leaf flutter only
uniform float uWindHeight;      // metres of local Y that map to a full mask
uniform int   uWindVertexMask;  // 1 = the mask is the vertex colour's red (painted: trunk 0, canopy tips 1)

float WindHash(vec3 p) {
    precise float h = fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    return h;
}

// Everything is `precise`: the depth pre-pass (ShadowDepth.vert) and the main pass (ModelVertex) are different shaders that must
// land a vertex on bit-identical depth, or the main pass fails GL_LEQUAL against the pre-pass and the leaf is left black.
// World-space offset for one vertex. originWorld is the object's origin (the whole tree shares one gust phase from it,
// so the trunk bends as one piece while the gust still travels across a hedge row).
vec3 FoliageWindOffset(vec3 localPos, vec4 color, vec3 originWorld) {
    if (uFoliage == 0) return vec3(0.0);
    precise float mask = uWindVertexMask == 1 ? color.r : clamp(localPos.y / max(uWindHeight, 0.01), 0.0, 1.0);
    precise float t = uWindTime * uWindFrequency;
    precise float along = dot(originWorld.xz, uWindDir);
    // Gust: a slow travelling wave plus two faster ones; ~[0.1, 1].
    precise float gust = 0.55 + 0.30 * sin(along * 0.35 - t * 1.1) + 0.15 * sin(along * 0.9 - t * 2.3 + 1.7);
    vec2 side = vec2(-uWindDir.y, uWindDir.x);
    precise vec2 bend = (uWindDir * gust + side * 0.25 * sin(t * 0.7 + along * 0.2)) * (mask * mask);
    // Leaf flutter: fast and small, a different phase per vertex; none near the trunk.
    precise float ph = WindHash(floor(localPos * 8.0)) * 6.2831853;
    precise vec3 fl = vec3(sin(t * 7.0 + ph), sin(t * 8.3 + ph * 1.7) * 0.6, sin(t * 6.1 + ph * 2.3));
    precise float flAmp = smoothstep(0.15, 0.6, mask) * (0.4 + 0.6 * gust);
    precise vec3 off = (vec3(bend.x, 0.0, bend.y) * (1.0 - uWindFlutter) * 1.4 + fl * flAmp * uWindFlutter * 0.6) * uWindStrength;
    return off;
}
