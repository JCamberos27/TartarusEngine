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
#version 330 core
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
                skinMat += uBones[aBoneIDs[i]] * aWeights[i];
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

// Must match MAX_POINT_LIGHTS in the fragment shader below.
inline constexpr int kMaxPointLights = 16;

inline constexpr const char* kModelFragmentSrc = R"(
#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in mat3 vTBN;
out vec4 FragColor;

uniform vec3 uViewPos;
uniform vec3 uLightDir;   // direction light travels (points away from the light)
uniform vec3 uLightColor;

// Placed LightComponent entities. Fixed-size arrays (rather than a UBO/SSBO) keep this on
// plain GL 3.3 — kMaxPointLights in main.cpp must match this bound.
#define MAX_POINT_LIGHTS 16
uniform int uPointLightCount;
uniform vec3 uPointLightPos[MAX_POINT_LIGHTS];
uniform vec3 uPointLightColor[MAX_POINT_LIGHTS];   // already scaled by intensity on the CPU
uniform float uPointLightRange[MAX_POINT_LIGHTS];
uniform vec3 uPointLightDir[MAX_POINT_LIGHTS];     // spot aim; unused when cos-cutoff is -1
uniform float uPointLightCosCutoff[MAX_POINT_LIGHTS]; // -1 => omnidirectional point light

// Set for the editor's Unlit shading mode: skips all lighting and shows flat albedo, so
// geometry/UV problems read clearly without shading hiding them.
uniform int uUnlit;

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
        FragColor = vec4(pow(albedo + emissiveEarly, vec3(1.0 / 2.2)), 1.0);
        return;
    }

    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Shared Cook-Torrance evaluation for one incoming light direction — the directional sun
    // and every point/spot light differ only in L and how much radiance reaches this fragment.
    vec3 Lo = vec3(0.0);
    {
        vec3 L = normalize(-uLightDir);
        vec3 H = normalize(V + L);
        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
        vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4);
        vec3 kD = (1.0 - F) * (1.0 - metallic);
        Lo += (kD * albedo / PI + specular) * uLightColor * max(dot(N, L), 0.0);
    }

    for (int i = 0; i < uPointLightCount; ++i) {
        vec3 toLight = uPointLightPos[i] - vWorldPos;
        float dist = length(toLight);
        if (dist > uPointLightRange[i]) continue;

        vec3 L = toLight / max(dist, 1e-4);

        // Inverse-square falloff, windowed so the contribution reaches exactly zero at Range
        // instead of being clipped mid-gradient into a visible hard edge.
        float t = dist / uPointLightRange[i];
        float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
        float attenuation = window * window / (1.0 + dist * dist);

        // Spot cone: -1 marks an omnidirectional point light, which skips this entirely.
        if (uPointLightCosCutoff[i] > -0.5) {
            float cosAngle = dot(normalize(-uPointLightDir[i]), L);
            if (cosAngle < uPointLightCosCutoff[i]) continue;
            // Soft edge over the outer ~10% of the cone.
            float edge = smoothstep(uPointLightCosCutoff[i], mix(uPointLightCosCutoff[i], 1.0, 0.1), cosAngle);
            attenuation *= edge;
        }
        if (attenuation <= 0.0) continue;

        vec3 H = normalize(V + L);
        float NDF = DistributionGGX(N, H, roughness);
        float G = GeometrySmith(N, V, L, roughness);
        vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
        vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4);
        vec3 kD = (1.0 - F) * (1.0 - metallic);
        Lo += (kD * albedo / PI + specular) * uPointLightColor[i] * attenuation * max(dot(N, L), 0.0);
    }

    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 emissive = emissiveEarly;

    vec3 color = ambient + Lo + emissive;
    color = color / (color + vec3(1.0)); // Reinhard tonemap
    color = pow(color, vec3(1.0 / 2.2));  // gamma correct

    FragColor = vec4(color, 1.0);
}
)";
