#version 460 core
#include "IblCubeDir.glsl"
in vec2 vUV;
out vec4 FragColor;

uniform samplerCube uEnvMap;
uniform float uRoughness;
uniform float uEnvResolution; // base face size of uEnvMap, for the mip-selection heuristic

const float PI = 3.14159265359;
const uint kSampleCount = 128u;

float RadicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint n) { return vec2(float(i) / float(n), RadicalInverseVdC(i)); }

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float DistributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

void main() {
    vec3 N = FaceDirection(vUV);
    vec3 R = N;
    vec3 V = N; // the split-sum approximation's standard N == V == R assumption

    vec3 prefiltered = vec3(0.0);
    float totalWeight = 0.0;

    for (uint i = 0u; i < kSampleCount; ++i) {
        vec2 Xi = Hammersley(i, kSampleCount);
        vec3 H = ImportanceSampleGGX(Xi, N, uRoughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;

        // Sample from a mip chosen by the sample's solid angle vs. a texel's, so sparse
        // high-roughness samples read a blurred mip instead of aliasing the base level.
        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 0.0);
        float D = DistributionGGX(NdotH, uRoughness);
        float pdf = (D * NdotH / (4.0 * max(HdotV, 1e-4))) + 1e-4;
        float saTexel = 4.0 * PI / (6.0 * uEnvResolution * uEnvResolution);
        float saSample = 1.0 / (float(kSampleCount) * pdf);
        float mip = uRoughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

        prefiltered += textureLod(uEnvMap, L, max(mip, 0.0)).rgb * NdotL;
        totalWeight += NdotL;
    }

    FragColor = vec4(prefiltered / max(totalWeight, 1e-4), 1.0);
}
