#version 460 core
// Grass blades (GrassRenderer), lit like any surface through ModelShading.glsl: the sun's and lamps' shadows,
// clustered lights, sky ambient with SSAO, fog. The colour runs from the area's base at the root to its tip colour,
// toward its dry colour in the dry patches, a shade lighter or darker blade to blade; the roots are darkened (light
// reaching them has crossed the field), light through a blade from behind shows green, and far blades shade more
// like the ground they cover. uDepthOnly 1: the depth pre-pass - nothing to shade.
in vec3 vWorldPos;
in vec3 vNormal;
in float vT;
flat in vec4 vBlade;
in vec4 vCurClip;
in vec4 vPrevClip;
out vec4 FragColor;
layout(location = 1) out vec2 oVelocity;

#include "ModelShading.glsl"

struct GrassArea { vec4 Origin; vec4 Axes; vec4 Size; vec4 Shape; vec4 Base; vec4 Tip; vec4 Dry; vec4 Lod; vec4 Wind; vec4 Map; };
layout(std430, binding = 30) readonly buffer GrassAreas { GrassArea uAreas[]; };
uniform int uDepthOnly;

const float kVelocityUnknown = 4.0;

float gT = 0.0; // vT, clamped (main sets it first): interpolation can take it a hair below 0, and pow() of that is NaN

vec3 GrassLight(vec3 albedo, vec3 N, vec3 V, vec3 L, vec3 radiance, float occl) {
    float NdotL = dot(N, L);
    float diff = clamp((NdotL + 0.45) / 1.45, 0.0, 1.0);
    float trans = (pow(clamp(dot(V, -L), 0.0, 1.0), 5.0) * 0.9 + 0.3 * clamp(-NdotL, 0.0, 1.0)) * gT;
    vec3 H = L + V;
    H *= inversesqrt(max(dot(H, H), 1e-8)); // looking straight into the light: no half vector
    float spec = 0.05 * pow(max(dot(N, H), 0.0), 36.0) * gT;
    return (albedo / PI * (diff * occl + trans * vec3(0.9, 1.0, 0.55)) + spec) * radiance;
}

void main() {
    oVelocity = vec2(0.0);
    if (uDepthOnly == 1) { FragColor = vec4(0.0); return; }
    if (uTaaVelocity == 1) {
        oVelocity = vec2(kVelocityUnknown);
        if (vCurClip.w > 1e-5 && vPrevClip.w > 1e-5)
            oVelocity = (vCurClip.xy / vCurClip.w - vPrevClip.xy / vPrevClip.w) * 0.5;
    }

    gT = clamp(vT, 0.0, 1.0);
    GrassArea A = uAreas[int(vBlade.z + 0.5)];
    float rnd = vBlade.x, dry = vBlade.y;
    vec3 albedo = mix(A.Base.rgb, A.Tip.rgb, smoothstep(0.0, 1.0, gT));
    albedo = mix(albedo, A.Dry.rgb * mix(0.65, 1.0, gT), dry);
    albedo = max(albedo * (1.0 + (rnd - 0.5) * 2.0 * A.Base.a), vec3(0.0));
    float occl = mix(0.22, 1.0, pow(gT, 0.65));
    bool receive = A.Dry.a > 0.5;

    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 N = dot(vNormal, vNormal) > 1e-12 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    if (!gl_FrontFacing) N = -N;
    float dist = length(uViewPos - vWorldPos);
    N = normalize(mix(N, vec3(0.0, 1.0, 0.0), smoothstep(6.0, 35.0, dist) * 0.65));

    vec3 Lo = vec3(0.0);
    for (uint i = 0u; i < uDirectionalCount; ++i) {
        if (!LightAffects(i)) continue;
        vec3 L = normalize(-uLights[i].DirCutoff.xyz);
        // Looked up as if on the ground under the blade (an upward normal): a blade's own tilted, swaying normal
        // pushed the lookup around and broke its shadow into acne and crawling speckles.
        float shadow = uLights[i].Params.y >= 0.0 ? (receive ? SunShadow(vWorldPos, vec3(0.0, 1.0, 0.0), L) : 1.0) * CloudShadow(vWorldPos) : 1.0;
        Lo += GrassLight(albedo, N, V, L, uLights[i].ColorRange.rgb * shadow, occl);
    }
    bool clustered = uClusterEnabled == 1;
    uint cluster = clustered ? clusterIndex() : 0u;
    uint lightOffset = clustered ? uClusterRange[cluster].offset : 0u;
    uint lightCount = clustered ? uClusterRange[cluster].count : uLightCount;
    for (uint j = 0u; j < lightCount; ++j) {
        uint li = clustered ? uClusterLightIndices[lightOffset + j] : j;
        if (!clustered && int(uLights[li].PositionType.w) == 0) continue;
        if (!LightAffects(li)) continue;
        vec3 L, radiance;
        if (!PointSpotLight(li, N, L, radiance)) continue;
        Lo += GrassLight(albedo, N, V, L, radiance, occl);
    }

    // No SSAO: the blades aren't in its depth (SceneRenderer draws them in after its copy); the root darkening
    // (occl) is their occlusion.
    vec3 irradiance = uIBLEnabled == 1 ? texture(uIrradianceMap, N).rgb * uIBLIntensity : vec3(0.03);
    vec3 ambient = irradiance * albedo * occl;
    vec3 color = ApplyVolumetricFog(ApplyAerialPerspective(ApplyFog(Lo + ambient)));
    if (uApplyTonemap == 1) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    }
    // Never hand the post chain a NaN or Inf: bloom's mips spread one pixel of it into black blocks.
    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    FragColor = vec4(color, 1.0);
}
