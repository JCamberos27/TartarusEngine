#version 460 core
// Grass scatter and cull (GrassRenderer): one workgroup per chunk the CPU picked (GrassChunks: area, first cell,
// cells per side). Each invocation takes one cell of the area's grid, places a blade at a jittered spot in it, and
// keeps it where the area's patch noise and edge fade let it grow, where the distance thinning keeps it, and where
// its bounds touch the view. Kept blades go to the near list (curved blades) or the far list (single triangles),
// counted straight into the two DrawArraysIndirect commands' instance counts. Everything is a hash of the cell
// and the seed, so a blade is the same every frame and in every view.
layout(local_size_x = 256) in;

struct GrassArea { vec4 Origin; vec4 Axes; vec4 Size; vec4 Shape; vec4 Base; vec4 Tip; vec4 Dry; vec4 Lod; vec4 Wind; vec4 Map; };
layout(std430, binding = 30) readonly buffer GrassAreas { GrassArea uAreas[]; };
layout(std430, binding = 31) readonly buffer GrassChunks { ivec4 uChunks[]; };
struct GrassBlade { vec4 A; vec4 B; vec4 C; };
layout(std430, binding = 32) writeonly buffer GrassBlades { GrassBlade uBlades[]; };
layout(std430, binding = 33) buffer GrassCmd { uint uCmd[8]; };
// The scene's terrain (TerrainGrid), for areas with Follow Terrain (Wind.y): heights row-major along X.
layout(std430, binding = 34) readonly buffer GrassTerrain { float uTerrainH[]; };
uniform int  uHasTerrain;
uniform vec4 uTerrainRect; // world x, z of the grid's (0, 0) sample, cell size x, z
uniform vec3 uTerrainRes;  // samples along x, z, the terrain entity's world y
// The areas' density masks (GrassDensityMap), 4 samples a uint; an area's Map: first byte, res x, res z, has one.
layout(std430, binding = 35) readonly buffer GrassMaps { uint uGrassMaps[]; };

uniform vec4 uFrustum[6]; // world-space planes (normal, d): inside where dot(n, p) + d >= 0
uniform vec3 uEye;
uniform int  uNearCap;
uniform int  uFarCap;

shared uint sNear, sFar, sNearBase, sFarBase;

const float kMinKeep = 0.1; // GrassRenderer::kMinKeep

uint Hash(uint x) { // lowbias32
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
uint Hash3(uint a, uint b, uint c) { return Hash(a ^ Hash((b + 0x9e3779b9u) ^ Hash(c))); }
float U01(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }

float ValueNoise(vec2 p, uint seed) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    ivec2 c = ivec2(i);
    float a = U01(Hash3(uint(c.x), uint(c.y), seed));
    float b = U01(Hash3(uint(c.x + 1), uint(c.y), seed));
    float d = U01(Hash3(uint(c.x), uint(c.y + 1), seed));
    float e = U01(Hash3(uint(c.x + 1), uint(c.y + 1), seed));
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}

float KeepFraction(float dist, float nearDist, float drawDist) { // GrassRenderer::KeepFraction
    if (!(dist < drawDist)) return 0.0;
    if (dist <= nearDist || drawDist <= nearDist) return 1.0;
    float f = 1.0 - clamp((dist - nearDist) / (drawDist - nearDist), 0.0, 1.0);
    return kMinKeep + (1.0 - kMinKeep) * f * f;
}

// TerrainGrid::Height: the same two triangles per cell (split from (i+1, j) to (i, j+1)) as the mesh.
float TerrainHeight(vec2 wxz) {
    ivec2 res = ivec2(uTerrainRes.xy);
    vec2 g = clamp((wxz - uTerrainRect.xy) / uTerrainRect.zw, vec2(0.0), vec2(res - 1));
    ivec2 c = min(ivec2(g), res - 2);
    vec2 f = g - vec2(c);
    int k = c.y * res.x + c.x;
    float h00 = uTerrainH[k], h10 = uTerrainH[k + 1], h01 = uTerrainH[k + res.x], h11 = uTerrainH[k + res.x + 1];
    float h = f.x + f.y <= 1.0 ? h00 + f.x * (h10 - h00) + f.y * (h01 - h00)
                               : h11 + (1.0 - f.x) * (h01 - h11) + (1.0 - f.y) * (h10 - h11);
    return uTerrainRes.z + h;
}

float MapByte(uint k) { return float((uGrassMaps[k >> 2u] >> ((k & 3u) * 8u)) & 0xFFu) * (1.0 / 255.0); }

// GrassDensityMap::Sample: bilinear over the mask at uv in [0, 1] across the area.
float MaskDensity(vec4 M, vec2 uv) {
    ivec2 res = ivec2(M.yz);
    vec2 g = clamp(uv, 0.0, 1.0) * vec2(res - 1);
    ivec2 c = min(ivec2(g), res - 2);
    vec2 f = g - vec2(c);
    uint k = uint(M.x) + uint(c.y * res.x + c.x);
    float a = mix(MapByte(k), MapByte(k + 1u), f.x);
    float b = mix(MapByte(k + uint(res.x)), MapByte(k + uint(res.x) + 1u), f.x);
    return mix(a, b, f.y);
}

void main() {
    ivec4 ch = uChunks[gl_WorkGroupID.x];
    GrassArea A = uAreas[ch.x];
    float cell = A.Origin.w;
    int cpc = ch.w;
    uint seed = floatBitsToUint(A.Size.w);
    int cellsX = int(ceil(A.Size.x / cell));
    int cellsZ = int(ceil(A.Size.y / cell));
    uint total = uint(cpc * cpc);
    float patchiness = A.Tip.w;

    if (gl_LocalInvocationIndex == 0u) { sNear = 0u; sFar = 0u; }
    barrier();

    for (uint base = 0u; base < total; base += 256u) {
        uint idx = base + gl_LocalInvocationIndex;
        bool keep = false, near = false;
        GrassBlade b;
        b.A = vec4(0.0); b.B = vec4(0.0); b.C = vec4(0.0);
        if (idx < total) {
            int cx = ch.y + int(idx % uint(cpc));
            int cz = ch.z + int(idx / uint(cpc));
            if (cx < cellsX && cz < cellsZ) {
                uint h = Hash3(uint(cx), uint(cz), seed);
                float r0 = U01(h), r1 = U01(Hash(h + 1u)), r2 = U01(Hash(h + 2u)), r3 = U01(Hash(h + 3u));
                vec2 local = (vec2(cx, cz) + vec2(r0, r1)) * cell - 0.5 * A.Size.xy;
                vec2 edge = 0.5 * A.Size.xy - abs(local);
                if (edge.x >= 0.0 && edge.y >= 0.0) {
                    vec2 wxz = A.Origin.xz + A.Axes.xy * local.x + A.Axes.zw * local.y;
                    vec3 root = vec3(wxz.x, A.Wind.y > 0.5 && uHasTerrain == 1 ? TerrainHeight(wxz) : A.Origin.y, wxz.y);
                    // Two octaves of patch noise: clumps a few metres across, broken up at their edges.
                    float patchN = ValueNoise(wxz * 0.16, seed * 7u + 3u) * 0.65 + ValueNoise(wxz * 0.55, seed * 7u + 5u) * 0.35;
                    float grow = mix(1.0, smoothstep(0.2, 0.55, patchN), patchiness);
                    // The mask thins the blades (under the canopy) and clears them (at logs, rocks and trunks).
                    float dens = A.Map.w > 0.5 ? MaskDensity(A.Map, local / A.Size.xy + 0.5) : 1.0;
                    grow *= dens;
                    float edgeK = A.Lod.z > 0.0 ? smoothstep(0.0, A.Lod.z, min(edge.x, edge.y)) : 1.0;
                    float dist = distance(root, uEye);
                    float kf = KeepFraction(dist, A.Lod.x, A.Lod.y);
                    if (r2 < grow * mix(0.35, 1.0, edgeK) && r3 < kf) {
                        float r4 = U01(Hash(h + 4u)), r5 = U01(Hash(h + 5u)), r6 = U01(Hash(h + 6u));
                        float r7 = U01(Hash(h + 7u)), r8 = U01(Hash(h + 8u));
                        float hScale = mix(1.0, 0.45 + 0.9 * patchN, patchiness) * mix(0.55, 1.0, edgeK) * mix(0.6, 1.0, dens);
                        float height = A.Shape.x * (1.0 + A.Shape.y * (r4 * 2.0 - 1.0)) * hScale;
                        height *= smoothstep(A.Lod.y, A.Lod.y * 0.85, dist); // sinks into the ground at the draw distance
                        vec3 c = root + vec3(0.0, 0.5 * height, 0.0);
                        float rad = 0.75 * height + 0.15;
                        bool inside = height > 0.01;
                        for (int i = 0; i < 6 && inside; ++i) inside = dot(uFrustum[i].xyz, c) + uFrustum[i].w >= -rad;
                        if (inside) {
                            keep = true;
                            near = dist < A.Lod.x;
                            // The survivors of the thinning widen, so the field keeps its cover.
                            float widen = min(inversesqrt(max(kf, 1e-3)), 3.0);
                            float dry = patchiness * (1.0 - smoothstep(0.25, 0.6, patchN)) * mix(0.6, 1.0, r8);
                            b.A = vec4(root, r5 * 6.2831853);
                            b.B = vec4(height, A.Shape.z * mix(0.75, 1.25, r6) * widen, A.Shape.w * mix(0.4, 1.6, r7), A.Lod.w);
                            b.C = vec4(r8, dry, float(ch.x), A.Wind.x);
                        }
                    }
                }
            }
        }
        uint li = 0u;
        if (keep) li = near ? atomicAdd(sNear, 1u) : atomicAdd(sFar, 1u);
        barrier();
        if (gl_LocalInvocationIndex == 0u) {
            sNearBase = sNear > 0u ? atomicAdd(uCmd[1], sNear) : 0u;
            sFarBase = sFar > 0u ? atomicAdd(uCmd[5], sFar) : 0u;
            sNear = 0u;
            sFar = 0u;
        }
        barrier();
        if (keep) {
            uint slot = (near ? sNearBase : sFarBase) + li;
            if (slot < uint(near ? uNearCap : uFarCap)) uBlades[(near ? 0u : uint(uNearCap)) + slot] = b;
        }
        barrier();
    }
}
