// The scene's one wind (SkySettings Wind*, set per frame by SetWindFieldUniforms): grass (Grass.vert), plants and trees
// (FoliageWind.glsl) and tree impostors (Impostor.vert) all sample this field, so they lean the same way and a gust
// visibly rolls across the grass, then the bushes, then the trees.
// Everything is `precise`: the depth pre-pass and the main pass are different programs that must land a vertex on
// bit-identical depth.
#pragma once
uniform float uWindTime;      // seconds, one value per frame (Material::WindTime)
uniform float uPrevWindTime;  // last frame's, for TAA motion vectors
uniform vec2  uWindDir;       // world XZ the wind blows toward, unit length
uniform float uWindGlobal;    // scene strength: x every bend, lean and flutter
uniform float uWindGustiness; // 0 steady .. 1 strong gusts and lulls
uniform float uWindGustSpeed; // m/s the gust fronts travel downwind
uniform vec3  uWindCamPos;    // the frame's camera: distance fade of the fine motion (identical in every pass)

vec2 WindSide() { return vec2(-uWindDir.y, uWindDir.x); }

// Gust strength at a world XZ point: mean 0.6, ~[0.1, 1.1] at full gustiness. Fronts are wide bands across the wind
// (about 40 m and 17 m apart, the long one bowed and the short one slanted, so they don't march in ruler lines) rolling
// downwind at uWindGustSpeed. Two sines: this runs for every vertex of every plant and grass blade.
float WindGust(vec2 xz, float t) {
    precise float along = dot(xz, uWindDir) - t * uWindGustSpeed;
    precise float across = dot(xz, WindSide());
    precise float w = 0.6 * sin(along * 0.157 + abs(fract(across * 0.011) - 0.5) * 6.0) // continuous chevrons, 90 m
                    + 0.4 * sin(along * 0.37 + across * 0.07 + 1.7);
    return 0.6 + 0.5 * uWindGustiness * w;
}

// A plant's whole-body lean (world XZ, metres at the top of its mask) for a plant rooted at originXZ: downwind with the
// gust, rocking on its own spring around that (harder in a gust), and a little sideways. strength = the material's
// bend in metres, freq = its sway rate. gust returns the plant's gust for the finer layers. Impostors use the same
// lean, so a tree does not jump when it swaps to its card.
vec2 WindMainLean(vec2 originXZ, float t, float strength, float freq, out float gust) {
    precise float g = WindGust(originXZ, t);
    gust = g;
    precise float ph = fract(dot(originXZ, vec2(0.1373, 0.2719))) * 6.2831853; // per-plant phase
    precise float tf = t * freq;
    precise float rock = 0.28 * sin(tf * 1.9 + ph) * (0.35 + 0.65 * g);
    precise float sway = 0.16 * sin(tf * 1.37 + ph * 0.6) * (0.3 + 0.7 * g);
    precise vec2 lean = (uWindDir * (g + rock) + WindSide() * sway) * (strength * uWindGlobal);
    return lean;
}
