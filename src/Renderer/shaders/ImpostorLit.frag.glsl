#version 460 core
// Octahedral impostors, lit (ImpostorRenderer.h): the blended frames' albedo, normal and mask lit as ModelFragment.glsl
// lights a foliage surface - the sun with its shadow and leaf transmission, the clustered lamps, the sky's ambient and
// reflections with SSAO, then the fog. Drawn GL_EQUAL over the pre-pass's depth (ImpostorSurface.glsl's offset).
layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec2 oVelocity;
vec3 vWorldPos; // ModelShading.glsl reads it: the surface ImpostorResolve finds
#include "ModelShading.glsl"
#define IMPOSTOR_HAS_VIEW
#include "ImpostorSurface.glsl"
// The view's own depth (multisampled; depth writes are off in this pass, so reading it is safe): a pixel every sample
// of which is nearer than this surface is dropped before the lighting - the program writes gl_FragDepth, so GL_EQUAL
// only runs after it. uImpDepthSamples 0: no depth to read.
layout(binding = 3) uniform sampler2DMS uImpViewDepth;
uniform int uImpDepthSamples;
uniform mat4 uPrevViewProj; // TAA (ModelVertex.glsl's): last frame's, unjittered
uniform vec2 uTaaJitter;
const float kVelocityUnknown = 4.0;

void main() {
    ImpostorFrag f;
    if (!ImpostorResolve(f)) discard;
    if (uImpDepthSamples > 0) {
        float farthest = 0.0;
        for (int i = 0; i < uImpDepthSamples; ++i) farthest = max(farthest, texelFetch(uImpViewDepth, ivec2(gl_FragCoord.xy), i).r);
        if (gl_FragDepth > farthest) discard; // every sample is something nearer (equal: this surface, kept)
    }
    vWorldPos = f.WorldPos;

    vec3 albedo = vec3(0.0), nObj = vec3(0.0);
    vec4 mask = vec4(0.0);
    for (int k = 0; k < 3; ++k) {
        float w = f.Weight[k] / max(f.WeightSum, 1e-5);
        albedo += textureLod(uImpAlbedo, f.Uv[k], f.Lod).rgb * w;
        nObj += (textureLod(uImpNormalDepth, f.Uv[k], f.Lod).rgb * 2.0 - 1.0) * w;
        mask += textureLod(uImpMask, f.Uv[k], f.Lod) * w;
    }
    float ao = mask.r, roughness = mask.g, translucency = mask.b, metallic = mask.a;
    vec3 N = normalize(ImpostorQRot(vRot, nObj) + vec3(0.0, 1e-6, 0.0));
    vec3 V = normalize(uViewPos - vWorldPos);
    // A blended normal can lean away from the viewer: bent back to the view's horizon, as a two-sided leaf's is.
    float nv = dot(N, V);
    if (nv < 0.02) N = normalize(N + V * (0.02 - nv));

    oVelocity = vec2(0.0);
    if (uTaaVelocity == 1) {
        vec4 cur = uProj * (uView * vec4(vWorldPos, 1.0));
        cur.xy -= uTaaJitter * cur.w;
        vec4 prev = uPrevViewProj * vec4(vWorldPos, 1.0);
        oVelocity = cur.w > 1e-5 && prev.w > 1e-5 ? (cur.xy / cur.w - prev.xy / prev.w) * 0.5 : vec2(kVelocityUnknown);
    }

#ifndef _ENGINE_NO_DEBUG_VIEW
    if (uDebugView == 4) albedo = vec3(0.75);
    if (uDebugView == 5) { FragColor = vec4(albedo, 1.0); return; }
    if (uDebugView == 1) { FragColor = vec4(N * 0.5 + 0.5, 1.0); return; }
#endif

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 Lo = vec3(0.0);
    for (uint i = 0u; i < uDirectionalCount; ++i) {
        if (!LightAffects(i)) continue;
        vec3 L = normalize(-uLights[i].DirCutoff.xyz);
        bool sunShadowed = uLights[i].Params.y >= 0.0;
        bool sunFront = dot(N, L) > 0.0;
        float sunVis = 1.0;
        if (sunShadowed)
            sunVis = (sunFront || translucency > 0.0) ? SunShadow(vWorldPos, sunFront ? N : -N, L) * CloudShadow(vWorldPos) : 0.0;
        vec3 radiance = uLights[i].ColorRange.rgb * sunVis;
        Lo += ShadeLight(N, V, L, radiance, albedo, F0, metallic, roughness);
        if (translucency > 0.0) {
            float back = max(dot(-N, L), 0.0);
            float fwd = pow(max(dot(-V, L), 0.0), 3.0);
            Lo += albedo * radiance * (back * (1.0 + 1.4 * fwd) * translucency / PI);
        }
    }
    bool clustered = uClusterEnabled == 1;
    uint lightOffset = 0u, lightCount = uLightCount;
    if (clustered) {
        uint cluster = clusterIndex();
        lightOffset = uClusterRange[cluster].offset;
        lightCount = uClusterRange[cluster].count;
    }
    for (uint j = 0u; j < lightCount; ++j) {
        uint li = clustered ? uClusterLightIndices[lightOffset + j] : j;
        if (!clustered && int(uLights[li].PositionType.w) == 0) continue;
        if (!LightAffects(li)) continue;
        vec3 L, radiance;
        if (!PointSpotLight(li, N, L, radiance)) continue;
        Lo += ShadeLight(N, V, L, radiance, albedo, F0, metallic, roughness);
    }

    float ssaoFactor = uSSAOEnabled == 1
        ? pow(texture(uSSAOMap, gl_FragCoord.xy / uScreenSize).r, uSSAOIntensity > 0.0 ? uSSAOIntensity : 1.0)
        : 1.0;
    vec3 ambient;
    if (uIBLEnabled == 1) {
        float NdotV = max(dot(N, V), 0.0);
        vec3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
        vec3 kD = (1.0 - F) * (1.0 - metallic);
        vec3 irradiance = texture(uIrradianceMap, N).rgb * uIBLIntensity;
        if (translucency > 0.0) irradiance += texture(uIrradianceMap, -N).rgb * uIBLIntensity * translucency;
        // Leaves keep the sky's reflections (ModelFragment.glsl: a canopy never sees what a room probe saw).
        vec3 prefiltered = textureLod(uPrefilteredMap, reflect(-V, N), roughness * uIBLSpecularMaxLod).rgb;
        vec2 ab = texture(uBrdfLut, vec2(NdotV, roughness)).rg;
        vec3 specularIBL = uNoGlossyReflections == 1 ? vec3(0.0) : prefiltered * (F * ab.x + ab.y);
        ambient = (kD * irradiance * albedo + specularIBL * uIBLIntensity) * ao;
    } else {
        ambient = vec3(0.03) * albedo * ao;
    }
    vec3 color = ambient * ssaoFactor + Lo;
    color = ApplyFog(color);
    color = ApplyAerialPerspective(color);
    color = ApplyVolumetricFog(color);
    if (uApplyTonemap == 1) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    }
    FragColor = vec4(color, 1.0);
}
