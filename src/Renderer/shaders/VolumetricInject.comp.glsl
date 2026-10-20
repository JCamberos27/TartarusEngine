#version 460 core
// Froxel volumetric fog, pass 1 of 2 (VolumetricFog.h): one thread per froxel of the view's
// W x H x D grid. Places a sample in the froxel (its depth jittered by blue noise that moves every
// frame), evaluates the participating medium there (density with height falloff, albedo) and the
// light it scatters toward the eye - every directional light (the one that owns the cascaded map
// is shadowed by it), the point and spot lights of the clustered-forward cell this froxel sits in
// (spot / point shadow maps when the light has one), and the sky ambient - through a
// Henyey-Greenstein phase. The result is blended with last frame's, reprojected, which is what
// turns the per-frame jitter into a smooth, alias-free volume.
// Output texel: rgb = in-scattered radiance per metre (sigma_s * L_in), a = extinction sigma_t.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0, rgba16f) writeonly uniform image3D uOut;

layout(binding = 6) uniform sampler3D uHistory;      // last frame's output of this pass
layout(binding = 7) uniform sampler2D uBlueNoise;    // 32 x 32 void-and-cluster ranks

uniform mat4  uInvView;       // view -> world
uniform mat4  uView;
uniform vec4  uProjParams;    // P[0][0], P[1][1], P[2][0], P[2][1]: the view ray through an NDC point
uniform vec4  uSliceParams;   // x = near, y = ln(far / near)
uniform vec3  uCameraPos;
uniform mat4  uPrevViewProj;
uniform int   uHistoryValid;  // 0 = first frame / resize / cut: take this frame as is
uniform float uBlend;         // weight of this frame against the reprojected history
uniform float uFrameJitter;   // per-frame offset of the blue noise (golden-ratio sequence)

uniform float uDensity;        // extinction at the base height, per metre
uniform float uHeightFalloff;  // 0 = uniform; > 0 thins out above uBaseHeight
uniform float uBaseHeight;
uniform vec3  uAlbedo;         // single-scattering albedo (sigma_s / sigma_t)
uniform float uAnisotropy;     // Henyey-Greenstein g
uniform float uAmbientScale;   // x the sky ambient's in-scatter
// The sky's irradiance probe (the model shader's unit 11): its up and down texels, averaged and
// divided by pi, are the sky radiance the medium scatters isotropically.
uniform int uIBLEnabled;
uniform float uIBLIntensity;
layout(binding = 11) uniform samplerCube uIrradianceMap;

// --- lights: the scene's light SSBO and the cluster lists (same layout as ModelShading.glsl) ---
struct Light {
    vec4 PositionType; // xyz = world pos; w = type: 0 directional, 1 point, 2 spot
    vec4 ColorRange;   // rgb = colour * intensity; a = range
    vec4 DirCutoff;    // xyz = aim; w = spot outer cos
    vec4 Params;       // x = spot inner cos; y = shadow slot; z = excluded layers; w = volumetric intensity
};
layout(std430, binding = 0) readonly buffer LightBuffer {
    uint uLightCount;
    uint uDirectionalCount;
    Light uLights[];
};
struct ClusterRange { uint offset; uint count; };
layout(std430, binding = 3) readonly buffer ClusterCounts { ClusterRange uClusterRange[]; };
layout(std430, binding = 4) readonly buffer ClusterIndex  { uint uClusterLightIndices[]; };
uniform int  uClusterEnabled;
uniform vec4 uClusterZParams; // (near, far, GZ/ln(far/near), -GZ*ln(near)/ln(far/near))
const uint C_GX = 16u, C_GY = 9u, C_GZ = 24u;

// --- shadows (the model shader's maps and matrices) ---
uniform int  uShadowEnabled;
uniform int  uShadowCascadeCount;
uniform vec4 uCascadeSplits;
uniform mat4 uShadowMatrices[4];
uniform vec4 uShadowTexelWorld;
uniform float uShadowMapResolution;
layout(binding = 8) uniform sampler2DArrayShadow uShadowMap;

uniform int  uSpotShadowCount;
uniform mat4 uSpotShadowVP[16];
uniform vec3 uSpotShadowPos[16];
uniform float uSpotShadowFar[16];
uniform float uSpotShadowHalfTan[16];
uniform float uSpotShadowMapResolution;
layout(binding = 9) uniform sampler2DArrayShadow uSpotShadowMap;

uniform int   uPointShadowCount;
uniform float uPointShadowFar[8];
uniform float uPointShadowMapResolution;
layout(binding = 10) uniform samplerCubeArrayShadow uPointShadowMap;

const float PI = 3.14159265359;

float PhaseHG(float cosTheta, float g) {
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4);
    return (1.0 - g2) / (4.0 * PI * denom * sqrt(denom));
}

// One hardware-filtered tap is enough: the temporal blend averages the jittered samples.
float SunShadow(vec3 p, float viewDepth) {
    if (uShadowEnabled == 0) return 1.0;
    int c = uShadowCascadeCount - 1;
    for (int i = 0; i < uShadowCascadeCount; ++i)
        if (viewDepth < uCascadeSplits[i]) { c = i; break; }
    vec4 lp = uShadowMatrices[c] * vec4(p, 1.0);
    vec3 proj = (lp.xyz / lp.w) * 0.5 + 0.5;
    if (proj.z >= 1.0 || any(lessThan(proj.xy, vec2(0.0))) || any(greaterThan(proj.xy, vec2(1.0)))) return 1.0;
    float texel = uShadowTexelWorld[c];
    float bias = (texel * 1.5) / (texel * uShadowMapResolution + 50.0);
    return texture(uShadowMap, vec4(proj.xy, float(c), proj.z - bias));
}

float SpotShadow(int slot, vec3 p) {
    if (slot < 0 || slot >= uSpotShadowCount) return 1.0;
    vec4 lp = uSpotShadowVP[slot] * vec4(p, 1.0);
    if (lp.w <= 0.0) return 1.0;
    vec3 sp = (lp.xyz / lp.w) * 0.5 + 0.5;
    if (any(lessThan(sp.xy, vec2(0.0))) || any(greaterThan(sp.xy, vec2(1.0)))) return 1.0;
    float far = max(uSpotShadowFar[slot], 1e-3);
    float d = distance(p, uSpotShadowPos[slot]);
    if (d >= far) return 1.0;
    float texelW = 2.0 * d * uSpotShadowHalfTan[slot] / uSpotShadowMapResolution;
    return texture(uSpotShadowMap, vec4(sp.xy, float(slot), (d - 1.5 * texelW) / far));
}

float PointShadow(int slot, vec3 p, vec3 lightPos) {
    if (slot < 0 || slot >= uPointShadowCount) return 1.0;
    float far = max(uPointShadowFar[slot], 1e-3);
    vec3 dir = p - lightPos;
    float d = length(dir);
    if (d >= far) return 1.0;
    float texelW = 2.0 * d / uPointShadowMapResolution;
    return texture(uPointShadowMap, vec4(dir, float(slot)), (d - 1.5 * texelW) / far);
}

// The light a point or spot light delivers to p, already scattered toward the eye (V points from
// p to the camera): the model shader's windowed inverse-square falloff and cone, its shadow, the
// light's Volumetric Intensity, and the phase function.
vec3 PointSpotScatter(uint i, vec3 p, vec3 V) {
    Light lt = uLights[i];
    float vol = lt.Params.w;
    if (vol <= 0.0) return vec3(0.0);
    vec3 toLight = lt.PositionType.xyz - p;
    float range = lt.ColorRange.a;
    float dist2 = dot(toLight, toLight);
    if (dist2 > range * range) return vec3(0.0);
    float dist = sqrt(dist2);
    vec3 L = toLight / max(dist, 1e-4);
    float t = dist / max(range, 1e-4);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    float atten = window * window / (1.0 + dist2);
    if (int(lt.PositionType.w) == 2) {
        float cosAngle = dot(-lt.DirCutoff.xyz, L);
        float outerCos = lt.DirCutoff.w;
        if (cosAngle < outerCos) return vec3(0.0);
        atten *= smoothstep(outerCos, max(lt.Params.x, outerCos + 1e-3), cosAngle);
        if (atten <= 0.0) return vec3(0.0);
        atten *= SpotShadow(int(lt.Params.y), p);
    } else {
        if (atten <= 0.0) return vec3(0.0);
        atten *= PointShadow(int(lt.Params.y), p, lt.PositionType.xyz);
    }
    return lt.ColorRange.rgb * (atten * vol * PhaseHG(dot(-L, V), uAnisotropy));
}

float SliceDepth(float w) { return uSliceParams.x * exp(w * uSliceParams.y); }

void main() {
    ivec3 uGrid = imageSize(uOut); // W, H, D
    ivec3 id = ivec3(gl_GlobalInvocationID);
    if (any(greaterThanEqual(id, uGrid))) return;

    // Blue-noise jitter along the slice, decorrelated between slices and animated per frame.
    float noise = texelFetch(uBlueNoise, (id.xy + ivec2(id.z * 7, id.z * 13)) & 31, 0).r;
    float jitter = fract(noise + uFrameJitter);
    float w = (float(id.z) + jitter) / float(uGrid.z);
    float depth = SliceDepth(w);

    vec2 ndc = (vec2(id.xy) + 0.5) / vec2(uGrid.xy) * 2.0 - 1.0;
    vec3 rayVS = vec3((ndc.x + uProjParams.z) / uProjParams.x, (ndc.y + uProjParams.w) / uProjParams.y, -1.0);
    vec3 p = (uInvView * vec4(rayVS * depth, 1.0)).xyz;
    vec3 V = normalize(uCameraPos - p);

    float sigmaT = uDensity;
    if (uHeightFalloff > 0.0) sigmaT *= exp(-uHeightFalloff * max(p.y - uBaseHeight, 0.0));
    vec3 sigmaS = uAlbedo * sigmaT;

    vec3 inScatter = vec3(0.0);
    if (uIBLEnabled == 1) {
        vec3 sky = textureLod(uIrradianceMap, vec3(0.0, 1.0, 0.0), 0.0).rgb + textureLod(uIrradianceMap, vec3(0.0, -1.0, 0.0), 0.0).rgb;
        inScatter = sky * (0.5 / PI) * uIBLIntensity * uAmbientScale;
    }
    for (uint i = 0u; i < uDirectionalCount; ++i) {
        float vol = uLights[i].Params.w;
        if (vol <= 0.0) continue;
        vec3 L = normalize(-uLights[i].DirCutoff.xyz);
        float vis = uLights[i].Params.y >= 0.0 ? SunShadow(p, depth) : 1.0;
        inScatter += uLights[i].ColorRange.rgb * (vis * vol * PhaseHG(dot(-L, V), uAnisotropy));
    }
    if (uClusterEnabled == 1) {
        // The clustered-forward cell this froxel sits in: its 16 x 9 tile, and the slice of the
        // cluster grid's own exponential split that holds this depth.
        uvec2 tile = min(uvec2((vec2(id.xy) + 0.5) / vec2(uGrid.xy) * vec2(C_GX, C_GY)), uvec2(C_GX - 1u, C_GY - 1u));
        float z = clamp(depth, uClusterZParams.x, uClusterZParams.y);
        uint slice = min(uint(max(log(z) * uClusterZParams.z + uClusterZParams.w, 0.0)), C_GZ - 1u);
        ClusterRange r = uClusterRange[tile.x + tile.y * C_GX + slice * C_GX * C_GY];
        for (uint k = 0u; k < r.count; ++k) inScatter += PointSpotScatter(uClusterLightIndices[r.offset + k], p, V);
    } else {
        for (uint i = uDirectionalCount; i < uLightCount; ++i) inScatter += PointSpotScatter(i, p, V);
    }

    vec4 current = vec4(sigmaS * inScatter, sigmaT);

    // Reproject into last frame's volume: where this froxel's centre was on screen and in depth.
    // The centre, not the jittered sample: with a still camera that lands exactly on last frame's
    // texel, where reading at the sample filtered in a share of the slice behind every frame - and
    // the slice behind a wall is often the lit outside, which then crept through it frame by frame.
    if (uHistoryValid == 1) {
        float centreDepth = SliceDepth((float(id.z) + 0.5) / float(uGrid.z));
        vec3 centre = (uInvView * vec4(rayVS * centreDepth, 1.0)).xyz;
        vec4 prevClip = uPrevViewProj * vec4(centre, 1.0);
        if (prevClip.w > 1e-4) {
            vec2 prevUv = prevClip.xy / prevClip.w * 0.5 + 0.5;
            float prevW = log(prevClip.w / uSliceParams.x) / uSliceParams.y;
            vec3 uvw = vec3(prevUv, prevW);
            if (all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0)))) {
                vec4 history = textureLod(uHistory, uvw, 0.0);
                if (all(equal(history, history))) current = mix(history, current, uBlend); // NaN guard
            }
        }
    }
    imageStore(uOut, id, current);
}
