#version 460 core
// Generates the volumetric clouds' noise textures once, at startup (VolumetricClouds.cpp):
//
//   uMode 0 - base shape, 128^3 RGBA8: R = Perlin-Worley (billowy Perlin fBm dilated by Worley
//             fBm, the classic cumulus base), GBA = Worley fBm at increasing frequency, used to
//             erode the base into cauliflower lumps (Schneider, "The Real-time Volumetric
//             Cloudscapes of Horizon: Zero Dawn", 2015).
//   uMode 1 - detail, 32^3 RGBA8: Worley fBm at three frequencies, for the wispy edge erosion.
//   uMode 2 - weather map, 512^2 RGBA8: R = coverage field (where clouds form), G = cloud-type
//             variation, B = streaky cirrus field, A = fine cirrus detail.
//
// Everything tiles, so the textures can repeat across the sky without seams.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
layout(binding = 0, rgba8) writeonly uniform image3D uOut3D;
layout(binding = 1, rgba8) writeonly uniform image2D uOut2D;

uniform int uMode;
uniform int uSize;
uniform float uSeed;

uvec3 Pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 Hash3(ivec3 c) {
    uvec3 h = Pcg3d(uvec3(c + ivec3(4096)) + uvec3(uint(uSeed * 7919.0)));
    return vec3(h) * (1.0 / 4294967295.0);
}
ivec3 Wrap(ivec3 c, int period) { return ((c % period) + period) % period; }

// Inverted Worley (cellular) noise: 1 at feature points, falling to 0 between them. Tiles with
// a period of `freq` cells.
float Worley(vec3 p, float freq) {
    vec3 q = p * freq;
    ivec3 cell = ivec3(floor(q));
    vec3 f = fract(q);
    float minD = 1e9;
    int period = int(freq + 0.5);
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                ivec3 o = ivec3(x, y, z);
                vec3 feature = Hash3(Wrap(cell + o, period)) + vec3(o);
                vec3 d = feature - f;
                minD = min(minD, dot(d, d));
            }
    return 1.0 - clamp(sqrt(minD), 0.0, 1.0);
}

float WorleyFbm(vec3 p, float freq) {
    return Worley(p, freq) * 0.625 + Worley(p, freq * 2.0) * 0.25 + Worley(p, freq * 4.0) * 0.125;
}

// Tileable gradient (Perlin) noise in [-1, 1].
float Gradient(ivec3 c, vec3 f, int period) {
    vec3 g = Hash3(Wrap(c, period)) * 2.0 - 1.0;
    return dot(normalize(g + 1e-5), f);
}
float Perlin(vec3 p, float freq) {
    vec3 q = p * freq;
    ivec3 c = ivec3(floor(q));
    vec3 f = fract(q);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    int period = int(freq + 0.5);
    float n000 = Gradient(c + ivec3(0, 0, 0), f - vec3(0, 0, 0), period);
    float n100 = Gradient(c + ivec3(1, 0, 0), f - vec3(1, 0, 0), period);
    float n010 = Gradient(c + ivec3(0, 1, 0), f - vec3(0, 1, 0), period);
    float n110 = Gradient(c + ivec3(1, 1, 0), f - vec3(1, 1, 0), period);
    float n001 = Gradient(c + ivec3(0, 0, 1), f - vec3(0, 0, 1), period);
    float n101 = Gradient(c + ivec3(1, 0, 1), f - vec3(1, 0, 1), period);
    float n011 = Gradient(c + ivec3(0, 1, 1), f - vec3(0, 1, 1), period);
    float n111 = Gradient(c + ivec3(1, 1, 1), f - vec3(1, 1, 1), period);
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}
float PerlinFbm(vec3 p, float freq, int octaves) {
    float sum = 0.0, amp = 1.0, norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += Perlin(p, freq) * amp;
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return sum / norm; // ~[-1, 1]
}

float Remap(float x, float a, float b, float c, float d) { return c + (x - a) / (b - a) * (d - c); }

void main() {
    ivec3 id = ivec3(gl_GlobalInvocationID);
    if (uMode == 2) {
        if (id.z != 0 || any(greaterThanEqual(id.xy, ivec2(uSize)))) return;
        vec3 p = vec3((vec2(id.xy) + 0.5) / float(uSize), 0.37);
        // Coverage: low-frequency Perlin clumps, broken up by Worley cells so clouds form in
        // groups with clear sky between them.
        // Spread to cover roughly the whole 0..1 range evenly, so the Coverage slider maps onto
        // how much of the sky clouds over.
        float perlin = PerlinFbm(p, 4.0, 5) * 0.5 + 0.5;
        float cells = WorleyFbm(p, 6.0);
        float coverage = smoothstep(0.34, 0.66, 0.8 * perlin + 0.2 * cells);
        float type = clamp(PerlinFbm(p + vec3(0.21, 0.63, 0.0), 2.0, 3) * 0.75 + 0.5, 0.0, 1.0);
        // Cirrus: stretched along X (the shader rotates it to the wind) for streaks.
        vec3 ps = vec3(p.x, p.y * 1.0, 0.71);
        float streak = PerlinFbm(vec3(ps.x * 1.0, ps.y * 4.0, ps.z), 4.0, 5) * 0.5 + 0.5;
        float fine = PerlinFbm(p + vec3(0.5, 0.1, 0.3), 16.0, 4) * 0.5 + 0.5;
        imageStore(uOut2D, id.xy, vec4(coverage, type, streak, fine));
        return;
    }
    if (any(greaterThanEqual(id, ivec3(uSize)))) return;
    vec3 p = (vec3(id) + 0.5) / float(uSize);
    if (uMode == 0) {
        float perlin = PerlinFbm(p, 4.0, 7) * 0.5 + 0.5;
        perlin = abs(perlin * 2.0 - 1.0);         // billowy
        perlin = 1.0 - perlin;                    // rounded tops rather than ridges
        float w0 = WorleyFbm(p, 4.0);
        float perlinWorley = clamp(Remap(perlin, 0.0, 1.0, w0, 1.0), 0.0, 1.0);
        imageStore(uOut3D, id, vec4(perlinWorley, w0, WorleyFbm(p, 8.0), WorleyFbm(p, 16.0)));
    } else {
        imageStore(uOut3D, id, vec4(WorleyFbm(p, 1.0), WorleyFbm(p, 2.0), WorleyFbm(p, 4.0), 1.0));
    }
}
