// Small shared noises. Guarded: several includers can meet in one program.
#ifndef NOISE_GLSL
#define NOISE_GLSL

// Interleaved gradient noise (Jimenez 2014): a cheap, even per-pixel dither (LOD fades, banding, ray jitter). Offset
// the pixel by 5.588238 * (frame % 64) to animate it for temporal accumulation.
float Ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

// The puddle noise: ModelFragment's wetness and PuddleSsr's mask read the same one, so the mask agrees with the shading.
float WetHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float WetNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(WetHash(i), WetHash(i + vec2(1, 0)), u.x), mix(WetHash(i + vec2(0, 1)), WetHash(i + vec2(1, 1)), u.x), u.y);
}

#endif // NOISE_GLSL
