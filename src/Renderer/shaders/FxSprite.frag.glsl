#version 460 core
// Flipbook particles (docs/BLOOD_FX.md, "v2"). Three looks, all output premultiplied (dst = src + dst * (1 - a)):
//   0 blood    - Knife's "Liquid Blood Errosion": the cell's white shape thresholded by an erosion that
//                rises over the life, so a burst breaks into droplets and thins away; the colour is the
//                tint, glossy (the entry's smoothness) and normal-mapped, lit like the meshes.
//   1 lit      - smoke, dust and debris: the cell's colour x tint, lit soft (half-Lambert sun and sky).
//   2 additive - muzzle flashes and sparks: shape x colour, no coverage.
// Soft against the scene's depth so nothing cuts a hard line where it meets a surface.

vec3 vWorldPos; // ModelShading's functions read it: here the sprite's fragment
#include "ModelShading.glsl"

struct FxSprite {
    vec4 PosSize;
    vec4 AxisRot;
    vec4 Color;
    vec4 Params;
    ivec4 Tex;
    ivec4 Info;
    vec4 Extra;
};
layout(std430, binding = 10) readonly buffer FxSprites { FxSprite uSprites[]; };

layout(binding = 17) uniform sampler2D uSceneDepth;      // single-sample resolve of the scene so far
layout(binding = 21) uniform sampler2DArray uFxColor;    // the sprite library (KnifeFxLibrary): BC3
layout(binding = 22) uniform sampler2DArray uFxNormal;   // BC5 normals
uniform mat4 uProj;
uniform vec4 uViewport;
uniform int uSoft;

in vec2 vUV;
in vec3 vWorld;
in vec3 vRight;
in vec3 vUp;
in vec3 vFacing;
in float vViewDepth;
flat in int vSprite;
out vec4 FragColor;

vec3 CellUV(int cell, ivec2 grid, int layer) {
    vec2 c = vec2(float(cell % grid.x), float(cell / grid.x));
    // The library keeps each image top row first; v runs up the quad.
    return vec3((c + vec2(vUV.x, 1.0 - vUV.y)) / vec2(grid), float(layer));
}

float LinearDepth(float d) {
    return uProj[3][2] / ((d * 2.0 - 1.0) + uProj[2][2]);
}

void main() {
    FxSprite s = uSprites[vSprite];
    int mode = s.Info.z;
    vWorldPos = vWorld;

    vec4 tex;
    vec2 nxy = vec2(0.0);
    bool hasNormal = false;
    if (s.Tex.x < 0) { // procedural soft dot
        vec2 q = vUV * 2.0 - 1.0;
        float r2 = dot(q, q);
        if (r2 >= 1.0) discard;
        tex = vec4(1.0, 1.0, 1.0, (1.0 - r2) * (1.0 - r2));
    } else {
        ivec2 grid = s.Info.xy;
        vec3 a = CellUV(s.Tex.z, grid, s.Tex.x);
        tex = texture(uFxColor, a);
        if (s.Params.w > 0.0) tex = mix(tex, texture(uFxColor, CellUV(s.Tex.w, grid, s.Tex.x)), s.Params.w);
        if (s.Tex.y >= 0) {
            vec3 na = CellUV(s.Tex.z, grid, s.Tex.y);
            nxy = texture(uFxNormal, na).rg * 2.0 - 1.0;
            hasNormal = true;
        }
    }

    float alpha;
    if (mode == 0) alpha = clamp((tex.a - s.Params.x) / s.Params.y, 0.0, 1.0) * s.Color.a;
    else alpha = tex.a * s.Color.a;

    float soft = 1.0;
    if (uSoft == 1) {
        float scene = LinearDepth(texelFetch(uSceneDepth, ivec2(gl_FragCoord.xy), 0).r);
        float fadeDist = max(0.02, s.PosSize.w * (mode == 1 ? 0.35 : 0.15));
        soft = clamp((scene - vViewDepth) / fadeDist, 0.0, 1.0);
    }
    alpha *= soft;
    if (alpha < 0.003) discard;

    vec3 V = normalize(uViewPos - vWorld);
    if (mode == 2) {
        vec3 glow = s.Color.rgb * tex.rgb * alpha;
        float keep = (ApplyFog(vec3(1.0)) - ApplyFog(vec3(0.0))).r;
        FragColor = vec4(glow * keep, 0.0);
        return;
    }

    vec3 F = vFacing;
    if (dot(F, V) < 0.0) F = -F;
    vec3 N = F;
    if (hasNormal) {
        float nz = sqrt(clamp(1.0 - dot(nxy, nxy), 0.0, 1.0));
        N = normalize(vRight * nxy.x + vUp * nxy.y + F * nz);
    }
    vec3 albedo = mode == 0 ? s.Color.rgb : tex.rgb * s.Color.rgb;
    float rough = clamp(1.0 - s.Extra.x, 0.04, 1.0);
    vec3 lit = vec3(0.0);
    if (mode == 0 || hasNormal) {
        // Glossy liquid (or a normal-mapped chip of debris): the meshes' own lighting.
        vec3 F0 = vec3(mode == 0 ? 0.02 : 0.04);
        for (uint i = 0u; i < uDirectionalCount; ++i) {
            vec3 L = normalize(-uLights[i].DirCutoff.xyz);
            vec3 radiance = uLights[i].ColorRange.rgb * (uLights[i].Params.y >= 0.0 ? SunShadow(vWorld, F, L) * CloudShadow(vWorld) : 1.0);
            lit += ShadeLight(N, V, L, radiance, albedo, F0, 0.0, rough);
            if (mode == 0) // thin blood glows red with the light behind it
                lit += radiance * albedo * 1.5 * pow(clamp(dot(-V, L), 0.0, 1.0), 4.0);
        }
        if (uClusterEnabled == 1) {
            uint cl = clusterIndex();
            uint off = uClusterRange[cl].offset, count = uClusterRange[cl].count;
            for (uint j = 0u; j < count; ++j) lit += ShadePointSpot(uClusterLightIndices[off + j], N, V, albedo, F0, 0.0, rough);
        }
        if (uIBLEnabled == 1) {
            float NdotV = max(dot(N, V), 0.0);
            vec3 Fr = FresnelSchlickRoughness(NdotV, F0, rough);
            vec2 ab = texture(uBrdfLut, vec2(NdotV, rough)).rg;
            lit += ((1.0 - Fr) * texture(uIrradianceMap, N).rgb * albedo
                    + textureLod(uPrefilteredMap, reflect(-V, N), rough * uIBLSpecularMaxLod).rgb * (Fr * ab.x + ab.y)) * uIBLIntensity;
        } else {
            lit += vec3(0.03) * albedo;
        }
    } else {
        // Smoke and dust: soft, light passing through - half-Lambert from the sun, the sky all round.
        for (uint i = 0u; i < uDirectionalCount; ++i) {
            vec3 L = normalize(-uLights[i].DirCutoff.xyz);
            float wrap = dot(N, L) * 0.5 + 0.5;
            vec3 radiance = uLights[i].ColorRange.rgb * (uLights[i].Params.y >= 0.0 ? SunShadow(vWorld, F, L) * CloudShadow(vWorld) : 1.0);
            lit += albedo * radiance * (wrap * wrap * 0.6 + 0.15 * pow(clamp(dot(-V, L), 0.0, 1.0), 6.0)) / PI;
        }
        if (uClusterEnabled == 1) {
            uint cl = clusterIndex();
            uint off = uClusterRange[cl].offset, count = uClusterRange[cl].count;
            for (uint j = 0u; j < count; ++j) {
                Light lt = uLights[uClusterLightIndices[off + j]];
                vec3 toL = lt.PositionType.xyz - vWorld;
                float d = length(toL);
                float t = d / max(lt.ColorRange.a, 1e-4);
                float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
                lit += albedo * lt.ColorRange.rgb * (window * window / (1.0 + d * d)) * 0.5 / PI;
            }
        }
        lit += albedo * (uIBLEnabled == 1 ? texture(uIrradianceMap, vec3(0.0, 1.0, 0.0)).rgb * uIBLIntensity : vec3(0.05));
    }
    vec3 c = ApplyAerialPerspective(ApplyFog(lit));
    FragColor = vec4(c * alpha, alpha);
}
