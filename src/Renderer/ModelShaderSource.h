#pragma once

// PBR model shader (metallic-roughness workflow, Cook-Torrance, single directional
// light + constant ambient term — no IBL/environment reflections). Supports optional
// GPU skinning (up to 4 bone influences/vertex) for imported, animated FBX/glTF assets.
//
// Shared (as of the Inspector's model preview) by main.cpp's real scene render AND
// ModelPreviewRenderer's offscreen preview — both need this exact source, not just a similarly-
// PBR-ish shader of their own, because Model::Draw()'s BindMaterial call sets a fixed uniform
// contract (uBaseColor, uHasAlbedoMap/uAlbedoMap, uMetallic, ...) that only a shader declaring
// those exact uniform names will actually receive. `inline constexpr` (not `static`) so this
// header can be included from more than one translation unit without violating ODR.
inline constexpr const char* kModelVertexSrc = R"(
#version 460 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec3 aTangent;
layout (location = 4) in ivec4 aBoneIDs;
layout (location = 5) in vec4 aWeights;
layout (location = 6) in float aTangentSign;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform int uUseSkinning;
uniform mat4 uBones[100];

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out mat3 vTBN;

void main() {
    vec4 localPos = vec4(aPos, 1.0);
    vec3 localNormal = aNormal;
    vec3 localTangent = aTangent;

    if (uUseSkinning == 1) {
        mat4 skinMat = mat4(0.0);
        float totalWeight = 0.0;
        for (int i = 0; i < 4; ++i) {
            if (aBoneIDs[i] >= 0) {
                skinMat += uBones[clamp(aBoneIDs[i], 0, 99)] * aWeights[i]; // clamp: never index uBones[] OOB (#98)
                totalWeight += aWeights[i];
            }
        }
        if (totalWeight <= 0.0001) {
            skinMat = mat4(1.0);
        }
        localPos = skinMat * localPos;
        localNormal = mat3(skinMat) * aNormal;
        localTangent = mat3(skinMat) * aTangent;
    }

    vec4 world = uModel * localPos;
    vWorldPos = world.xyz;

    mat3 normalMat = mat3(transpose(inverse(uModel)));
    vNormal = normalize(normalMat * localNormal);
    // Tangents transform with the model matrix's linear part directly (not the
    // inverse-transpose used for normals) — using normalMat here would skew tangents
    // under non-uniform scale.
    vec3 T = normalize(mat3(uModel) * localTangent);
    T = normalize(T - dot(T, vNormal) * vNormal);
    vec3 B = cross(vNormal, T) * aTangentSign;
    vTBN = mat3(T, B, vNormal);

    vUV = aUV;
    gl_Position = uProj * uView * world;
}
)";

inline constexpr const char* kModelFragmentSrc = R"(
#version 460 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in mat3 vTBN;
out vec4 FragColor;

uniform vec3 uViewPos;

// Every scene light (directional sun, point, spot) in one std430 SSBO — filled by LightBuffer
// on the CPU, bound at binding = 0. This is the layout the clustered-forward cull pass will
// consume later, so it doesn't change again when that lands.
struct Light {
    vec4 PositionType; // xyz = world pos (point/spot); w = type: 0 directional, 1 point, 2 spot
    vec4 ColorRange;   // rgb = colour * intensity; a = range in metres (point/spot)
    vec4 DirCutoff;    // xyz = normalized aim direction (spot/directional); w = spot outer-cone cos (-1 = none)
    vec4 Params;       // x = spot inner-cone cos; yzw reserved (shadow slot, etc.)
};
layout(std430, binding = 0) readonly buffer LightBuffer {
    uint uLightCount;
    Light uLights[];
};

// Cascaded shadow maps for the directional sun (see CascadedShadowMap). uView is also used to
// pick the cascade by view-space depth.
uniform mat4 uView;
uniform int  uShadowEnabled;
uniform int  uShadowCascadeCount;
uniform vec4 uCascadeSplits;          // per-cascade far distance, view space (positive)
uniform mat4 uShadowMatrices[4];
uniform float uShadowNormalBias;      // world units, offset along N before projection
uniform float uShadowSoftness;        // PCF kernel radius in shadow-map texels (from the sun's angular size)
uniform sampler2DArrayShadow uShadowMap;

// Set for the editor's Unlit shading mode: skips all lighting and shows flat albedo, so
// geometry/UV problems read clearly without shading hiding them.
uniform int uUnlit;

// 0 (the real scene path): output LINEAR HDR — the shared Tonemapper pass does exposure +
// curve + gamma once, after MSAA resolve. 1 (offscreen model preview / any target without a
// tonemap pass): keep the old baked Reinhard + gamma so those thumbnails look unchanged.
uniform int uApplyTonemap;

uniform vec3 uBaseColor;
uniform float uMetallic;
uniform float uRoughness;
uniform vec3 uEmissiveColor;

uniform int uHasAlbedoMap;             uniform sampler2D uAlbedoMap;
uniform int uHasNormalMap;             uniform sampler2D uNormalMap;
uniform int uHasMetallicRoughnessMap;  uniform sampler2D uMetallicRoughnessMap;
uniform int uHasMetallicMap;           uniform sampler2D uMetallicMap;
uniform int uHasRoughnessMap;          uniform sampler2D uRoughnessMap;
uniform int uHasAOMap;                 uniform sampler2D uAOMap;
uniform int uHasEmissiveMap;           uniform sampler2D uEmissiveMap;

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / max(denom, 1e-7);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 16-tap Poisson disk for the soft PCF kernel — rotated per-fragment so the penumbra dithers
// into noise instead of showing the concentric banding a fixed box filter leaves behind.
const vec2 kPoisson16[16] = vec2[](
    vec2(-0.942016, -0.399062), vec2( 0.945586, -0.768907),
    vec2(-0.094184, -0.929389), vec2( 0.344959,  0.293878),
    vec2(-0.915886,  0.457714), vec2(-0.815442, -0.879125),
    vec2(-0.382775,  0.276768), vec2( 0.974844,  0.756484),
    vec2( 0.443233, -0.975116), vec2( 0.537430, -0.473734),
    vec2(-0.264969, -0.418930), vec2( 0.791975,  0.190902),
    vec2(-0.241888,  0.997065), vec2(-0.814100,  0.914376),
    vec2( 0.199841,  0.786414), vec2( 0.143832, -0.141008)
);

// One cascade's filtered sun visibility: 16 Poisson taps, each still hardware 2x2 depth-compared.
float SampleCascade(int c, vec2 uv, float ref, float radiusTexels, float rot) {
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0).xy);
    float s = sin(rot), co = cos(rot);
    mat2 R = mat2(co, s, -s, co);
    float vis = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 o = (R * kPoisson16[i]) * radiusTexels * texel;
        vis += texture(uShadowMap, vec4(uv + o, float(c), ref));
    }
    return vis / 16.0;
}

// Sun visibility at this fragment from the cascaded shadow maps: 1 = lit, 0 = fully shadowed.
// Soft Poisson PCF whose radius comes from the sun's angular size, plus a smooth blend across
// the cascade seam so there's no hard step where the resolution changes.
float SunShadow(vec3 worldPos, vec3 N, vec3 L) {
    if (uShadowEnabled == 0) return 1.0;

    float viewDepth = abs((uView * vec4(worldPos, 1.0)).z);
    int c = uShadowCascadeCount - 1;
    for (int i = 0; i < uShadowCascadeCount; ++i) {
        if (viewDepth < uCascadeSplits[i]) { c = i; break; }
    }

    float ndl = max(dot(N, L), 0.0);
    float nBias = uShadowNormalBias * (2.0 - ndl);   // push harder along N at grazing angles
    vec3 offsetPos = worldPos + N * nBias;

    vec4 lp = uShadowMatrices[c] * vec4(offsetPos, 1.0);
    vec3 proj = (lp.xyz / lp.w) * 0.5 + 0.5;
    if (proj.z >= 1.0) return 1.0;

    float bias = mix(0.0016, 0.0005, ndl);
    // Hash gl_FragCoord to a rotation angle — turns kernel banding into per-pixel noise.
    float rot = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    // Farther cascades cover more world per texel, so widen the kernel a little to keep the
    // apparent penumbra roughly constant across the seam.
    float radius = max(uShadowSoftness, 0.5) * (1.0 + 0.35 * float(c));

    float vis = SampleCascade(c, proj.xy, proj.z - bias, radius, rot);

    // Cross-fade into the next cascade over the last slice of this one.
    if (c + 1 < uShadowCascadeCount) {
        float edge = uCascadeSplits[c];
        float band = edge * 0.12;
        if (viewDepth > edge - band) {
            vec4 lp2 = uShadowMatrices[c + 1] * vec4(offsetPos, 1.0);
            vec3 p2 = (lp2.xyz / lp2.w) * 0.5 + 0.5;
            if (p2.z < 1.0) {
                float v2 = SampleCascade(c + 1, p2.xy, p2.z - bias,
                                         max(uShadowSoftness, 0.5) * (1.0 + 0.35 * float(c + 1)), rot);
                vis = mix(vis, v2, smoothstep(edge - band, edge, viewDepth));
            }
        }
    }
    return vis;
}

// Cook-Torrance contribution for one light direction L delivering `radiance` to the fragment.
// The sun and every point/spot light differ only in L and how much radiance survives to here.
vec3 ShadeLight(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, vec3 F0,
                float metallic, float roughness) {
    vec3 H = normalize(V + L);
    float NDF = DistributionGGX(N, H, roughness);
    float G   = GeometrySmith(N, V, L, roughness);
    vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * radiance * max(dot(N, L), 0.0);
}

void main() {
    vec3 albedo = (uHasAlbedoMap == 1 ? texture(uAlbedoMap, vUV).rgb : vec3(1.0)) * uBaseColor;

    float metallic = uMetallic;
    float roughness = uRoughness;
    if (uHasMetallicRoughnessMap == 1) {
        vec3 mr = texture(uMetallicRoughnessMap, vUV).rgb;
        roughness = mr.g;
        metallic = mr.b;
    } else {
        if (uHasRoughnessMap == 1) roughness = texture(uRoughnessMap, vUV).r;
        if (uHasMetallicMap == 1) metallic = texture(uMetallicMap, vUV).r;
    }
    float ao = uHasAOMap == 1 ? texture(uAOMap, vUV).r : 1.0;

    vec3 N = normalize(vNormal);
    if (uHasNormalMap == 1) {
        vec3 tangentNormal = texture(uNormalMap, vUV).rgb * 2.0 - 1.0;
        N = normalize(vTBN * tangentNormal);
    }

    vec3 emissiveEarly = uHasEmissiveMap == 1 ? texture(uEmissiveMap, vUV).rgb : uEmissiveColor;
    if (uUnlit == 1) {
        vec3 flatColor = albedo + emissiveEarly;
        FragColor = vec4(uApplyTonemap == 1 ? pow(flatColor, vec3(1.0 / 2.2)) : flatColor, 1.0);
        return;
    }

    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);
    for (uint i = 0u; i < uLightCount; ++i) {
        Light lt = uLights[i];
        int type = int(lt.PositionType.w);

        vec3 L;
        vec3 radiance = lt.ColorRange.rgb;

        if (type == 0) {
            // Directional: DirCutoff.xyz is the direction light travels; L points back at it.
            L = normalize(-lt.DirCutoff.xyz);
            radiance *= SunShadow(vWorldPos, N, L);
        } else {
            vec3 toLight = lt.PositionType.xyz - vWorldPos;
            float dist = length(toLight);
            float range = lt.ColorRange.a;
            if (dist > range) continue;
            L = toLight / max(dist, 1e-4);

            // Windowed inverse-square: contribution reaches exactly zero at Range, no hard clip.
            float t = dist / max(range, 1e-4);
            float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
            float atten = window * window / (1.0 + dist * dist);

            if (type == 2) {
                float cosAngle = dot(normalize(-lt.DirCutoff.xyz), L);
                float outerCos = lt.DirCutoff.w;
                float innerCos = lt.Params.x;
                if (cosAngle < outerCos) continue;
                atten *= smoothstep(outerCos, max(innerCos, outerCos + 1e-3), cosAngle);
            }
            if (atten <= 0.0) continue;
            radiance *= atten;
        }

        Lo += ShadeLight(N, V, L, radiance, albedo, F0, metallic, roughness);
    }

    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 emissive = emissiveEarly;

    vec3 color = ambient + Lo + emissive;
    if (uApplyTonemap == 1) {
        color = color / (color + vec3(1.0)); // Reinhard tonemap
        color = pow(color, vec3(1.0 / 2.2)); // gamma correct
    }
    // else: leave linear HDR for the shared Tonemapper pass.

    FragColor = vec4(color, 1.0);
}
)";
