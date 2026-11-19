#version 460 core
#include "IblCubeDir.glsl"
in vec2 vUV;
out vec4 FragColor;

uniform samplerCube uEnvMap;
uniform float uRoughness;
uniform float uEnvResolution; // base face size of uEnvMap, for the mip-selection heuristic
#include "IblEnvSample.glsl"

#include "IblSampling.glsl"
const uint kSampleCount = 128u;

float DistributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

void main() {
    vec3 N = FaceDirection(vUV);
    // Roughness 0 is a mirror: every GGX sample below is H == N, L == N, at mip 0, so the
    // weighted average of kSampleCount identical fetches is just the one.
    if (uRoughness == 0.0) {
        FragColor = vec4(ClampRadiance(textureLod(uEnvMap, RotateEnv(N), 0.0).rgb), 1.0);
        return;
    }
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

        prefiltered += ClampRadiance(textureLod(uEnvMap, RotateEnv(L), max(mip, 0.0)).rgb) * NdotL;
        totalWeight += NdotL;
    }

    FragColor = vec4(prefiltered / max(totalWeight, 1e-4), 1.0);
}
