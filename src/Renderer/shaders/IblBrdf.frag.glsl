#version 460 core
in vec2 vUV;
out vec2 FragColor;

#include "IblSampling.glsl"
const uint kSampleCount = 1024u;

// IBL uses the k = a^2/2 remap, NOT the (r+1)^2/8 direct-lighting one in ModelFragment.glsl.
float GeometrySchlickGGX(float NdotV, float roughness) {
    float a = roughness;
    float k = (a * a) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

void main() {
    float NdotV = max(vUV.x, 1e-3);
    float roughness = vUV.y;

    vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    vec3 N = vec3(0.0, 0.0, 1.0);

    float A = 0.0;
    float B = 0.0;
    for (uint i = 0u; i < kSampleCount; ++i) {
        vec2 Xi = Hammersley(i, kSampleCount);
        vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        if (NdotL <= 0.0) continue;
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float G = GeometrySchlickGGX(NdotL, roughness) * GeometrySchlickGGX(NdotV, roughness);
        float GVis = (G * VdotH) / max(NdotH * NdotV, 1e-4);
        float Fc = pow(1.0 - VdotH, 5.0);

        A += (1.0 - Fc) * GVis;
        B += Fc * GVis;
    }
    FragColor = vec2(A, B) / float(kSampleCount);
}
