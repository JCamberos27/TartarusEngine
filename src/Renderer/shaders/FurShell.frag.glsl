#version 460 core
// Shell fur (FurRenderer): one shell's fragment. The UV square is cut into uFurDensity x uFurDensity cells, each
// holding one strand at a jittered spot with its own length; a fragment is kept where this shell's height still
// lies on a strand, whose radius thins toward the tip. Lit by the scene's lights and sky through ModelShading.glsl
// (the same shadows, clusters, IBL and fog as the surface under it), with Kajiya-Kay hair highlights along the
// strand and darkening toward the roots.
in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vStrand;
in vec2 vUV;
in float vShellH;
out vec4 FragColor;

#include "ModelShading.glsl"

uniform float uFurDensity;
uniform vec3  uFurTint;
uniform float uFurRim;
uniform int   uHasFurMask;
uniform sampler2D uFurMask;
// Light scattered on through the coat: wrapped and lifted over plain Lambert, the way the surface's own
// subsurface lobe lifts the skin under it.
const float kScatter = 1.7;
uniform int   uAlphaToCoverage; // 1: the target is multisampled - the strand's edge becomes coverage, not a hard cut

vec4 Hash4(vec2 p) {
    vec4 q = vec4(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)), dot(p, vec2(419.2, 371.9)), dot(p, vec2(51.7, 97.3)));
    return fract(sin(q) * 43758.5453);
}

// Kajiya-Kay: a strand reflects into a cone around it, brightest where the half vector is square to it.
float StrandSpecular(vec3 T, vec3 H, float exponent) {
    float th = dot(T, H);
    float sinTH = sqrt(max(1.0 - th * th, 0.0));
    return smoothstep(-1.0, 0.0, th) * pow(sinTH, exponent);
}

void main() {
    vec2 uv = vUV * (uUVTiling == vec2(0.0) ? vec2(1.0) : uUVTiling) + uUVOffset;
    float mask = uHasFurMask == 1 ? texture(uFurMask, vUV).r : 1.0;
    if (mask < 0.03) discard;

    vec2 g = vUV * uFurDensity;
    vec2 cell = floor(g);
    vec2 f = fract(g);
    vec4 r = Hash4(cell);
    // Thinner mask: fewer strands and shorter ones (a sparse carapace, bare fangs).
    if (r.w > mask * 1.6) discard;
    float strandLen = mask * mix(0.55, 1.0, r.x);
    float t = vShellH / strandLen; // 0 at the root, 1 at this strand's tip
    if (t > 1.0) discard;
    // Jittered in its cell, and leaning a little its own way toward the tip, so the strands don't line up in rows.
    vec2 lean = vec2(cos(r.w * 40.0), sin(r.w * 40.0)) * 0.2;
    vec2 centre = 0.5 + (r.yz - 0.5) * 0.5 + lean * t;
    // A strand fills under a third of its cell even at the root, so the skin shows between them (a solid root
    // layer read as a dark felt), thinning to a fine point.
    float radius = 0.24 * pow(1.0 - t, 0.8) + 0.025;
    float d = length(f - centre);
    float edge = max(fwidth(d), 1e-3);
    float coverage = clamp((radius - d) / edge + 0.5, 0.0, 1.0);
    if (uAlphaToCoverage == 1) { if (coverage <= 0.02) discard; }
    else if (coverage < 0.5) discard;

    vec3 albedo = uBaseColor * uFurTint * (uHasAlbedoMap == 1 ? texture(uAlbedoMap, uv).rgb : vec3(1.0));
    float ao = uHasAOMap == 1 ? texture(uAOMap, uv).r : 1.0;
    // Each strand a shade lighter or darker than its neighbours, and the tips a touch lighter: the
    // painted coat breaks into hairs instead of reading as a fuzzy copy of the texture.
    albedo *= mix(0.8, 1.15, r.x) * mix(0.9, 1.15, t);
    // Self-shadowing: light reaching the roots has crossed the coat above them.
    float occl = mix(0.55, 1.0, smoothstep(0.0, 1.0, vShellH)) * mix(1.0, ao, 0.5);

    vec3 N = normalize(vNormal);
    vec3 T = normalize(vStrand);
    vec3 V = normalize(uViewPos - vWorldPos);
    // Two shifted lobes (Marschner-lite): a white one toward the root, a coloured one toward the tip.
    vec3 T1 = normalize(T - N * 0.12);
    vec3 T2 = normalize(T + N * 0.12);
    float fresnel = pow(1.0 - abs(dot(N, V)), 4.0);

    vec3 Lo = vec3(0.0);
    for (uint i = 0u; i < uDirectionalCount; ++i) {
        if (!LightAffects(i)) continue;
        vec3 L = normalize(-uLights[i].DirCutoff.xyz);
        vec3 radiance = uLights[i].ColorRange.rgb * (uLights[i].Params.y >= 0.0 ? SunShadow(vWorldPos, N, L) * CloudShadow(vWorldPos) : 1.0);
        float NdotL = dot(N, L);
        float tl = dot(T, L);
        float diff = kScatter * clamp((NdotL + 0.5) / 1.5, 0.0, 1.0) * mix(1.0, sqrt(max(1.0 - tl * tl, 0.0)), 0.25);
        vec3 H = normalize(L + V);
        float lit = clamp(NdotL + 0.3, 0.0, 1.0);
        vec3 spec = (vec3(0.07) * StrandSpecular(T1, H, 80.0) + albedo * 0.2 * StrandSpecular(T2, H, 18.0)) * lit * t * occl;
        vec3 back = albedo * uFurRim * fresnel * pow(clamp(dot(-V, L), 0.0, 1.0), 2.0);
        Lo += (albedo / PI * diff * occl + spec + back) * radiance;
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
        float NdotL = dot(N, L);
        float tl = dot(T, L);
        float diff = kScatter * clamp((NdotL + 0.5) / 1.5, 0.0, 1.0) * mix(1.0, sqrt(max(1.0 - tl * tl, 0.0)), 0.25);
        vec3 H = normalize(L + V);
        float lit = clamp(NdotL + 0.3, 0.0, 1.0);
        vec3 spec = (vec3(0.07) * StrandSpecular(T1, H, 80.0) + albedo * 0.2 * StrandSpecular(T2, H, 18.0)) * lit * t * occl;
        vec3 back = albedo * uFurRim * fresnel * pow(clamp(dot(-V, L), 0.0, 1.0), 2.0);
        Lo += (albedo / PI * diff * occl + spec + back) * radiance;
    }

    vec3 irradiance = uIBLEnabled == 1 ? texture(uIrradianceMap, N).rgb * uIBLIntensity : vec3(0.03);
    vec3 ambient = irradiance * albedo * occl * (1.0 + uFurRim * fresnel);
    vec3 color = ApplyVolumetricFog(ApplyAerialPerspective(ApplyFog(Lo + ambient)));
    if (uApplyTonemap == 1) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    }
    FragColor = vec4(color, uAlphaToCoverage == 1 ? coverage : 1.0);
}
