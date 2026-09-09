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
    uint uDirectionalCount; // directional lights are packed at the front of uLights[] (#188)
    Light uLights[];
};

// Clustered-forward light culling (#120). The view frustum is diced into a fixed
// 16 x 9 x 24 grid of froxels; a per-frame compute pass (ClusterGrid) fills, for each
// froxel, a {offset, count} range into one global compacted light-index list (#208) covering
// the point/spot lights whose range reaches it. This fragment finds its own froxel from
// gl_FragCoord + view depth and loops only that froxel's [offset, offset+count) slice of the
// global list. uClusterEnabled == 0 (the offscreen model preview, which runs no compute pass)
// falls back to looping every light.
struct ClusterRange { uint offset; uint count; };
layout(std430, binding = 3) readonly buffer ClusterCounts { ClusterRange uClusterRange[]; };
layout(std430, binding = 4) readonly buffer ClusterIndex  { uint uClusterLightIndices[]; };
uniform int  uClusterEnabled;
uniform vec2 uClusterScreenSize;
uniform vec4 uClusterZParams; // (near, far, GZ/ln(far/near), -GZ*ln(near)/ln(far/near))
const uint C_GX = 16u, C_GY = 9u, C_GZ = 24u;

// Cascaded shadow maps for the directional sun (see CascadedShadowMap). uView is also used to
// pick the cascade by view-space depth.
uniform mat4 uView;
uniform int  uShadowEnabled;
uniform int  uShadowCascadeCount;
uniform vec4 uCascadeSplits;          // per-cascade far distance, view space (positive)
uniform mat4 uShadowMatrices[4];
uniform vec4  uShadowTexelWorld;      // world units per shadow texel, per cascade (#117)
uniform float uShadowSoftness;        // PCF kernel radius in shadow-map texels (from the sun's angular size)
// Per-light shadow multipliers (#140 phase 2). All default 1.0 -> identical to pre-phase-2.
uniform float uSunShadowBias;        // x the sun's texel-proportional depth bias
uniform float uSunShadowNormalBias;  // x the sun's normal-offset term
uniform sampler2DArrayShadow uShadowMap;
// Resolution is already known CPU-side (CascadedShadowMap::Configure) — a uniform instead of a
// per-fragment textureSize() call (#190).
uniform float uShadowMapResolution;

// Spot-light shadow maps (#119): one perspective depth layer per casting spot, indexed by the
// light's Params.y. The map stores LINEAR distance-to-light / far, so the compare below is
// against distance(fragment, uSpotShadowPos) / uSpotShadowFar — bias uniform in world space,
// shadow reaches the full light Range. Unit 9 (the sun CSM is unit 8, material maps 1..7).
uniform int  uSpotShadowCount;
uniform mat4 uSpotShadowVP[4];
uniform vec3 uSpotShadowPos[4];
uniform float uSpotShadowFar[4];
uniform float uSpotShadowHalfTan[4]; // tan(half-FOV) of each spot's map — the world texel footprint
                                     // at distance d is 2*d*halfTan/res, NOT the 2*d/res that a
                                     // 90° cube face gives; a narrow spot was over-offsetting (#134)
uniform float uSpotShadowBias[4];       // per-light x depth bias   (#140 phase 2, default 1.0)
uniform float uSpotShadowNormalBias[4]; // per-light x normal offset
uniform float uSpotShadowSoftness[4];   // per-light x PCF tap spread
uniform sampler2DArrayShadow uSpotShadowMap;
uniform float uSpotShadowMapResolution; // known CPU-side (SpotShadowMap::Configure) (#190)

// Point-light cube shadow maps (#119): one depth cube per casting point light, indexed by the
// light's Params.y. Also stores linear distance / uPointShadowFar[slot] (the light's Range).
// Unit 10.
uniform int   uPointShadowCount;
uniform float uPointShadowFar[2];
uniform float uPointShadowBias[2];       // per-light x depth bias   (#140 phase 2, default 1.0)
uniform float uPointShadowNormalBias[2]; // per-light x normal offset
uniform samplerCubeArrayShadow uPointShadowMap;
uniform float uPointShadowMapResolution; // known CPU-side (PointShadowMap::Configure) (#190)

// Image-based lighting probes baked from the procedural sky (#196, see IblProbe.h). Explicit
// binding qualifiers rather than SetInt(): every path that uses this shader (the real scene,
// ModelPreviewRenderer's offscreen thumbnails, ChannelPreviewRenderer) then agrees on the units
// without each having to remember to assign them, and units 11-13 can never collide with the
// sampler2D on unit 0. uIBLEnabled == 0 (the GLSL default for a never-set uniform int) falls
// back to the old constant ambient, which is exactly what the preview renderers want.
uniform int uIBLEnabled;
uniform float uIBLIntensity;             // scene's Ambient intensity control (World::SkyAmbientIntensity)
uniform float uIBLSpecularMaxLod;        // kSpecularMips - 1
layout(binding = 11) uniform samplerCube uIrradianceMap;
layout(binding = 12) uniform samplerCube uPrefilteredMap;
layout(binding = 13) uniform sampler2D uBrdfLut;

// PR14 — Reflection probes: parallax box projection (#ifdef _REFLECTION_PROBES; zero-keyword: dead code)
#ifdef _REFLECTION_PROBES
uniform int   uProbeCount;        // 0-2 probes bound this draw call
uniform vec3  uProbeCenter[2];    // world-space probe origin
uniform vec3  uProbeHalfSize[2];  // half-extents of the capture box
uniform float uProbeBlend[2];     // normalised blend weights (sum == 1)
#endif

// Fresnel with a roughness term: a rough surface's grazing-angle reflectance must not exceed
// its own specular colour, which the plain Schlick form (which goes to white at 90 degrees)
// gets badly wrong for ambient, where every direction is grazing for someone.
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Set for the editor's Unlit shading mode: skips all lighting and shows flat albedo, so
// geometry/UV problems read clearly without shading hiding them.
uniform int uUnlit;

// Scene-view debug draw modes (#236 R2). 0 = off (normal path).
//   1 Normals   — world-space normal as RGB.
//   2 Cascades  — tint by which sun-shadow cascade covers this fragment.
//   3 Mip       — texture LOD of the albedo map as a colour ramp.
uniform int uDebugView;

// 0 (the real scene path): output LINEAR HDR — the shared Tonemapper pass does exposure +
// curve + gamma once, after MSAA resolve. 1 (offscreen model preview / any target without a
// tonemap pass): keep the old baked Reinhard + gamma so those thumbnails look unchanged.
uniform int uApplyTonemap;

uniform vec3 uBaseColor;
uniform float uMetallic;
uniform float uRoughness;
uniform vec3 uEmissiveColor;

// World-space triplanar projection (#checker-materials): projects each of the 3 axis-aligned
// planes' UVs from world position and blends by how much the surface normal faces that axis,
// instead of using the mesh's own (possibly badly-tiling) UVs. Used for level geometry like a
// non-uniformly-scaled cube where 0..1 mesh UVs stretch a texture unrecognisably on long faces.
uniform int uTriplanar;
uniform float uTriplanarScale;

// PR 9 — transparent queue. uAlphaBlend = 0 (opaque, default) keeps alpha = 1 (bit-identical).
uniform int   uAlphaBlend;  // 0 = opaque pass, 1 = transparent pass
uniform float uOpacity;     // material surface alpha (only used when uAlphaBlend != 0)

uniform int uHasAlbedoMap;             uniform sampler2D uAlbedoMap;
uniform int uHasNormalMap;             uniform sampler2D uNormalMap;
uniform int uHasMetallicRoughnessMap;  uniform sampler2D uMetallicRoughnessMap;
uniform int uHasMetallicMap;           uniform sampler2D uMetallicMap;
uniform int uHasRoughnessMap;          uniform sampler2D uRoughnessMap;
uniform int uHasAOMap;                 uniform sampler2D uAOMap;
uniform int uHasEmissiveMap;           uniform sampler2D uEmissiveMap;

// PR10 — Clear Coat (#ifdef _CLEARCOAT; zero-keyword variant: dead code, bit-identical to PR9)
#ifdef _CLEARCOAT
uniform float uClearCoat;           // layer strength [0,1]
uniform float uClearCoatRoughness;  // CC microfacet roughness [0,1]
uniform int   uHasClearCoatMap;
uniform sampler2D uClearCoatMap;    // masks uClearCoat via .r channel
#endif

// PR10 — Anisotropy (#ifdef _ANISO; zero-keyword variant: dead code, bit-identical to PR9)
#ifdef _ANISO
uniform float uAnisotropy;          // [-1,1]: +1 = highlight along T, -1 = along B
uniform float uAnisotropyRotation;  // [0,1] maps to [0°,360°] rotation in tangent plane
#endif

// PR11 — Sheen/cloth (#ifdef _SHEEN; zero-keyword variant: dead code, bit-identical to PR10)
#ifdef _SHEEN
uniform vec3  uSheen;               // tint color (rgb); zero = sheen disabled
uniform float uSheenRoughness;      // cloth microfacet roughness [0,1]
#endif

// PR11 — Subsurface translucency (#ifdef _SUBSURFACE; zero-keyword variant: dead code)
#ifdef _SUBSURFACE
uniform vec3  uSubsurfaceColor;     // transmitted tint
uniform float uThickness;           // surface thickness scalar [0,1]
uniform int   uHasThicknessMap;
uniform sampler2D uThicknessMap;    // per-texel thickness (.r channel)
#endif

// Viewport size in pixels — used by Transmission (PR12) for NDC->UV and SSAO (PR15) for screenUV.
uniform vec2 uScreenSize;

// PR12 — Transmission + refraction (#ifdef _TRANSMISSION; zero-keyword variant: dead code)
#ifdef _TRANSMISSION
uniform sampler2D uOpaqueColor;         // resolved opaque scene color with mip chain (unit 14)
uniform float     uTransmissionStrength;// [0,1]
uniform float     uIOR;                 // index of refraction
#endif

// PR15 — Screen-space ambient occlusion. uSSAOEnabled == 0 (the GL default) = no occlusion.
uniform sampler2D uSSAOMap;   // blurred R8 occlusion (unit 15); only read when uSSAOEnabled == 1
uniform int       uSSAOEnabled;

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

// PR10 — Clear coat lobe: isotropic GGX with fixed F0 = 0.04. Returns scalar specular term.
#ifdef _CLEARCOAT
float ClearCoatLobe(vec3 N, vec3 V, vec3 L, float ccRoughness) {
    vec3 H = normalize(V + L);
    float NDF = DistributionGGX(N, H, ccRoughness);
    float G   = GeometrySmith(N, V, L, ccRoughness);
    float F   = FresnelSchlick(max(dot(H, V), 0.0), vec3(0.04)).r;
    return (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4);
}

// CC attenuation factor on the base lobe: FresnelSchlick(NdotV, 0.04) * strength.
// Applied once to Lo and ambient after all lights are accumulated.
float ClearCoatFresnel(float NdotV, float ccStrength) {
    return FresnelSchlick(NdotV, vec3(0.04)).r * ccStrength;
}
#endif

// PR10 — Anisotropic BRDF (Burley / Filament §4.9). Only compiled in _ANISO variants.
#ifdef _ANISO
// Anisotropic GGX distribution (Heitz / Filament).
float D_GGX_Aniso(float NdotH, float HdotT, float HdotB, float at, float ab) {
    float a2 = at * ab;
    vec3 v = vec3(ab * HdotT, at * HdotB, a2 * NdotH);
    float v2 = dot(v, v);
    return a2 * a2 / max(PI * v2 * v2, 1e-7);
}

// Smith height-correlated masking-shadowing for anisotropic GGX (Heitz 2014).
float V_SmithGGX_Aniso(float NdotV, float VdotT, float VdotB,
                        float NdotL, float LdotT, float LdotB, float at, float ab) {
    float GGX_V = NdotL * length(vec3(at * VdotT, ab * VdotB, NdotV));
    float GGX_L = NdotV * length(vec3(at * LdotT, ab * LdotB, NdotL));
    return 0.5 / max(GGX_V + GGX_L, 1e-5);
}

vec3 ShadeLightAniso(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, vec3 F0,
                     float metallic, float roughness, vec3 T, vec3 B) {
    float aniso = clamp(uAnisotropy, -0.99, 0.99);
    float at = max(roughness * (1.0 + aniso), 0.001);
    float ab = max(roughness * (1.0 - aniso), 0.001);
    vec3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float D   = D_GGX_Aniso(NdotH, dot(H, T), dot(H, B), at, ab);
    float Vis = V_SmithGGX_Aniso(NdotV, dot(V, T), dot(V, B),
                                  NdotL, dot(L, T), dot(L, B), at, ab);
    vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 specular = D * Vis * F;
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * radiance * NdotL;
}
#endif

// PR11 — Sheen/cloth (Charlie D + Neubelt V + Estevez-Kulla analytic DFG, no new LUT)
#ifdef _SHEEN
// Charlie inverted-sine distribution (Estevez & Kulla 2017).
float D_Charlie(float NdotH, float roughness) {
    float r2 = roughness * roughness;
    float sin2h = max(1.0 - NdotH * NdotH, 0.0078125);
    return (2.0 + 1.0 / r2) * pow(sin2h, 0.5 / r2) / (2.0 * PI);
}

// Neubelt visibility for cloth (numerically stable, no division by NdotV*NdotL).
float V_Neubelt(float NdotV, float NdotL) {
    return clamp(1.0 / (4.0 * (NdotL + NdotV - NdotL * NdotV)), 0.0, 1.0);
}

// Estevez-Kulla analytic DFG for sheen (avoids a separate BRDF-LUT sample).
// Approximates the sheen directional albedo as a function of NdotV and roughness.
float SheenDFG(float NdotV, float roughness) {
    return mix(0.0, clamp(1.0 - pow(1.0 - NdotV, 2.0 + 4.0 * roughness), 0.0, 1.0), roughness);
}

// Sheen lobe contribution for one light. Returns vec3 (colored by uSheen tint).
vec3 SheenLobe(vec3 N, vec3 V, vec3 L, float sheenRoughness) {
    vec3 H = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float D   = D_Charlie(NdotH, max(sheenRoughness, 0.045));
    float Vis = V_Neubelt(NdotV, NdotL);
    return uSheen * D * Vis * NdotL;
}
#endif

// PR11 — Subsurface translucency: wrapped diffuse + back-lit thin-surface transmission.
#ifdef _SUBSURFACE
// Wrapped diffuse NdotL — softens the terminator into the shadow side.
float WrappedDiffuse(float NdotL, float wrap) {
    return clamp((NdotL + wrap) / ((1.0 + wrap) * (1.0 + wrap)), 0.0, 1.0);
}

// Thin-surface back-lit transmission: light punching through from behind.
// Only non-zero when the light is on the far side of the surface (-N·L > 0).
vec3 SubsurfaceTransmission(vec3 N, vec3 L, vec3 radiance, float thickness) {
    float backDot = max(dot(-N, L), 0.0);
    float atten = backDot * (1.0 - thickness); // thinner surface = more light through
    return uSubsurfaceColor * radiance * atten;
}
#endif

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
// Most fragments are either fully lit or fully shadowed, not near a penumbra edge - a cheap
// 4-tap probe (spread across the disk) first, and if all four agree, the full 16 almost
// certainly would too, so return immediately (#189). The 4 probe taps are indices 0/4/8/12; the
// loop below sums the other 12 and combines them with the already-taken 4, so a fragment that
// DOES need the full kernel gets an identical result to always sampling all 16 - only the
// early-out path skips work, the average itself is unchanged.
float SampleCascade(int c, vec2 uv, float ref, float radiusTexels, float rot) {
    vec2 texel = 1.0 / vec2(uShadowMapResolution);
    float s = sin(rot), co = cos(rot);
    mat2 R = mat2(co, s, -s, co);

    float v0 = texture(uShadowMap, vec4(uv + (R * kPoisson16[0])  * radiusTexels * texel, float(c), ref));
    float v1 = texture(uShadowMap, vec4(uv + (R * kPoisson16[4])  * radiusTexels * texel, float(c), ref));
    float v2 = texture(uShadowMap, vec4(uv + (R * kPoisson16[8])  * radiusTexels * texel, float(c), ref));
    float v3 = texture(uShadowMap, vec4(uv + (R * kPoisson16[12]) * radiusTexels * texel, float(c), ref));
    float probeSum = v0 + v1 + v2 + v3;
    if (probeSum <= 0.0) return 0.0; // all 4 probes fully shadowed
    if (probeSum >= 4.0) return 1.0; // all 4 probes fully lit

    float vis = probeSum;
    for (int i = 0; i < 16; ++i) {
        if (i == 0 || i == 4 || i == 8 || i == 12) continue; // already sampled above
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
    // Normal offset + depth bias both scale with the selected cascade's world texel size (#117).
    // The CSM pass now stores the LIGHT-FACING surface (back-face cull, #134), like spot/point,
    // so a caster's own lit side can self-shadow and the depth bias below carries the acne
    // protection. Keep the normal offset tiny — just enough to clear PCF kernel bleed at edges;
    // any more and the shadow's contact edge visibly retreats from the base of what cast it.
    float texel = uShadowTexelWorld[c];
    // Bias is expressed in world units (multiples of the cascade texel) and divided into the
    // cascade's [0,1] depth span so it stays a constant physical offset regardless of cascade
    // size. Span = 2*radius + pullback; 2*radius == texel*resolution, pullback == 50.
    vec3 offsetPos = worldPos + N * (texel * (0.5 + 0.5 * (1.0 - ndl)) * uSunShadowNormalBias);

    vec4 lp = uShadowMatrices[c] * vec4(offsetPos, 1.0);
    vec3 proj = (lp.xyz / lp.w) * 0.5 + 0.5;
    if (proj.z >= 1.0) return 1.0;

    float bias = (texel * (1.0 + 2.0 * (1.0 - ndl)) * uSunShadowBias) / (texel * uShadowMapResolution + 50.0);
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
            float texel2 = uShadowTexelWorld[c + 1];
            vec3 offsetPos2 = worldPos + N * (texel2 * (0.5 + 0.5 * (1.0 - ndl)) * uSunShadowNormalBias);
            vec4 lp2 = uShadowMatrices[c + 1] * vec4(offsetPos2, 1.0);
            vec3 p2 = (lp2.xyz / lp2.w) * 0.5 + 0.5;
            if (p2.z < 1.0) {
                float bias2 = (texel2 * (1.0 + 2.0 * (1.0 - ndl)) * uSunShadowBias) / (texel2 * uShadowMapResolution + 50.0);
                float v2 = SampleCascade(c + 1, p2.xy, p2.z - bias2,
                                         max(uShadowSoftness, 0.5) * (1.0 + 0.35 * float(c + 1)), rot);
                vis = mix(vis, v2, smoothstep(edge - band, edge, viewDepth));
            }
        }
    }
    return vis;
}

// Visibility from a spot light's shadow map. 1 = lit, 0 = shadowed. The map stores LINEAR
// distance-to-light / far, so the compare reference is distance(fragment, light) / far and a
// constant bias is uniform in world space — the shadow reaches the whole light Range. 4-tap PCF.
float SpotShadow(int slot, vec3 worldPos, vec3 N) {
    if (slot < 0 || slot >= uSpotShadowCount) return 1.0;
    // The depth pass stores the light-facing surface (back-face cull), so a caster's own lit side
    // CAN self-shadow — the bias below has to cover one shadow-texel of slope error, but no more,
    // or the contact shadow peter-panning returns.
    vec3 toL = uSpotShadowPos[slot] - worldPos;
    float d0 = length(toL);
    float nl = max(dot(N, toL / max(d0, 1e-4)), 0.0);
    // World size of one shadow texel at the receiver, using the spot's REAL half-angle (the old
    // 2*d/res assumed a 90° frustum and over-sized it for a tight cone). Both the normal nudge
    // and the depth bias are expressed as multiples of this, so they auto-scale with distance and
    // are independent of the light Range (#134).
    float texelW = 2.0 * d0 * uSpotShadowHalfTan[slot] / uSpotShadowMapResolution;
    vec3 biasedPos = worldPos + N * (texelW * (0.5 + 0.5 * (1.0 - nl)) * uSpotShadowNormalBias[slot]); // clears PCF kernel bleed at edges
    vec4 lp = uSpotShadowVP[slot] * vec4(biasedPos, 1.0);
    if (lp.w <= 0.0) return 1.0;                       // behind the light
    vec3 p = (lp.xyz / lp.w) * 0.5 + 0.5;
    if (any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0))))
        return 1.0;                                    // outside the cone -> unshadowed
    float far = max(uSpotShadowFar[slot], 1e-3);
    float d = distance(biasedPos, uSpotShadowPos[slot]);
    if (d >= far) return 1.0;                          // past the shadow range
    // Depth bias = one shadow texel of world size (a bit more at grazing angles, where the stored
    // surface slopes fastest across a texel). Range-independent, unlike the old `d/far - 0.00035`
    // whose gap grew to centimetres on a long-range light (#134).
    float ref = (d - texelW * (1.0 + 2.0 * (1.0 - nl)) * uSpotShadowBias[slot]) / far;
    vec2 texel = (1.0 / vec2(uSpotShadowMapResolution)) * max(uSpotShadowSoftness[slot], 0.0);
    float vis = 0.0;
    vis += texture(uSpotShadowMap, vec4(p.xy + vec2(-0.5, -0.5) * texel, float(slot), ref));
    vis += texture(uSpotShadowMap, vec4(p.xy + vec2( 0.5, -0.5) * texel, float(slot), ref));
    vis += texture(uSpotShadowMap, vec4(p.xy + vec2(-0.5,  0.5) * texel, float(slot), ref));
    vis += texture(uSpotShadowMap, vec4(p.xy + vec2( 0.5,  0.5) * texel, float(slot), ref));
    return vis * 0.25;
}

// Visibility from a point light's depth cube. 1 = lit, 0 = shadowed. Cube stores linear
// distance / far, so this is a direct distance compare — no dominant-axis NDC reconstruction.
float PointShadow(int slot, vec3 worldPos, vec3 lightPos, vec3 N) {
    if (slot < 0 || slot >= uPointShadowCount) return 1.0;
    float far = max(uPointShadowFar[slot], 1e-3);
    // Same as SpotShadow: the cube pass stores the light-facing surface (back-face cull), so the
    // bias covers ~one shadow texel of slope error and nothing more.
    vec3 toLight = lightPos - worldPos;
    float d0 = length(toLight);
    float nl = max(dot(N, toLight / max(d0, 1e-4)), 0.0);
    float texelW = 2.0 * d0 / uPointShadowMapResolution; // 90° cube face: 2*d/res is exact
    vec3 dir = (worldPos + N * (texelW * (0.5 + 0.5 * (1.0 - nl)) * uPointShadowNormalBias[slot])) - lightPos; // cube lookup + distance
    float d = length(dir);
    if (d >= far) return 1.0;                // past the shadow range
    // Texel-proportional, Range-independent depth bias — see SpotShadow (#134).
    float ref = (d - texelW * (1.0 + 2.0 * (1.0 - nl)) * uPointShadowBias[slot]) / far;
    return texture(uPointShadowMap, vec4(dir, float(slot)), ref);
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

// Full shading for one point or spot light (index into uLights): range check, windowed
// inverse-square falloff, spot cone + shadow, point shadow. Returns its Lo contribution, or
// zero if the fragment is out of range / outside the cone. Shared by the clustered loop and
// the non-clustered fallback.
vec3 ShadePointSpot(uint i, vec3 N, vec3 V, vec3 albedo, vec3 F0, float metallic, float roughness) {
    Light lt = uLights[i];
    int type = int(lt.PositionType.w);

    vec3 toLight = lt.PositionType.xyz - vWorldPos;
    float dist = length(toLight);
    float range = lt.ColorRange.a;
    if (dist > range) return vec3(0.0);
    vec3 L = toLight / max(dist, 1e-4);

    // Windowed inverse-square: contribution reaches exactly zero at Range, no hard clip.
    float t = dist / max(range, 1e-4);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    float atten = window * window / (1.0 + dist * dist);

    if (type == 2) {
        float cosAngle = dot(normalize(-lt.DirCutoff.xyz), L);
        float outerCos = lt.DirCutoff.w;
        float innerCos = lt.Params.x;
        if (cosAngle < outerCos) return vec3(0.0);
        atten *= smoothstep(outerCos, max(innerCos, outerCos + 1e-3), cosAngle);
        // Early-out before the shadow-map sample, not just before shading (#191) — a fragment at
        // the far edge of the cone/range falloff is already at atten==0 here, so this skips a
        // full shadow-array texture fetch that would just get multiplied away.
        if (atten <= 0.0) return vec3(0.0);
        atten *= SpotShadow(int(lt.Params.y), vWorldPos, N); // #119
    } else {
        if (atten <= 0.0) return vec3(0.0);
        atten *= PointShadow(int(lt.Params.y), vWorldPos, lt.PositionType.xyz, N); // #119
    }
    if (atten <= 0.0) return vec3(0.0);
    return ShadeLight(N, V, L, lt.ColorRange.rgb * atten, albedo, F0, metallic, roughness);
}

#ifdef _ANISO
// Anisotropic version of ShadePointSpot — identical attenuation/shadow logic, calls ShadeLightAniso.
vec3 ShadePointSpotAniso(uint i, vec3 N, vec3 V, vec3 albedo, vec3 F0, float metallic, float roughness,
                          vec3 T, vec3 B) {
    Light lt = uLights[i];
    int type = int(lt.PositionType.w);
    vec3 toLight = lt.PositionType.xyz - vWorldPos;
    float dist = length(toLight);
    float range = lt.ColorRange.a;
    if (dist > range) return vec3(0.0);
    vec3 L = toLight / max(dist, 1e-4);
    float t = dist / max(range, 1e-4);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    float atten = window * window / (1.0 + dist * dist);
    if (type == 2) {
        float cosAngle = dot(normalize(-lt.DirCutoff.xyz), L);
        float outerCos = lt.DirCutoff.w;
        float innerCos = lt.Params.x;
        if (cosAngle < outerCos) return vec3(0.0);
        atten *= smoothstep(outerCos, max(innerCos, outerCos + 1e-3), cosAngle);
        if (atten <= 0.0) return vec3(0.0);
        atten *= SpotShadow(int(lt.Params.y), vWorldPos, N);
    } else {
        if (atten <= 0.0) return vec3(0.0);
        atten *= PointShadow(int(lt.Params.y), vWorldPos, lt.PositionType.xyz, N);
    }
    if (atten <= 0.0) return vec3(0.0);
    return ShadeLightAniso(N, V, L, lt.ColorRange.rgb * atten, albedo, F0, metallic, roughness, T, B);
}
#endif

#ifdef _CLEARCOAT
// Clear coat contribution from one point/spot light — same attenuation as ShadePointSpot.
vec3 ShadePointSpotCC(uint i, vec3 N, vec3 V, float ccRough) {
    Light lt = uLights[i];
    int type = int(lt.PositionType.w);
    vec3 toLight = lt.PositionType.xyz - vWorldPos;
    float dist = length(toLight);
    float range = lt.ColorRange.a;
    if (dist > range) return vec3(0.0);
    vec3 L = toLight / max(dist, 1e-4);
    float t = dist / max(range, 1e-4);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    float atten = window * window / (1.0 + dist * dist);
    if (type == 2) {
        float cosAngle = dot(normalize(-lt.DirCutoff.xyz), L);
        float outerCos = lt.DirCutoff.w;
        float innerCos = lt.Params.x;
        if (cosAngle < outerCos) return vec3(0.0);
        atten *= smoothstep(outerCos, max(innerCos, outerCos + 1e-3), cosAngle);
        if (atten <= 0.0) return vec3(0.0);
        atten *= SpotShadow(int(lt.Params.y), vWorldPos, N);
    } else {
        if (atten <= 0.0) return vec3(0.0);
        atten *= PointShadow(int(lt.Params.y), vWorldPos, lt.PositionType.xyz, N);
    }
    if (atten <= 0.0) return vec3(0.0);
    return lt.ColorRange.rgb * atten * max(dot(N, L), 0.0) * ClearCoatLobe(N, V, L, ccRough);
}
#endif

#ifdef _SHEEN
// Sheen contribution from one point/spot light — same attenuation as ShadePointSpot.
vec3 ShadePointSpotSheen(uint i, vec3 N, vec3 V, float sheenRoughness) {
    Light lt = uLights[i];
    int type = int(lt.PositionType.w);
    vec3 toLight = lt.PositionType.xyz - vWorldPos;
    float dist = length(toLight);
    float range = lt.ColorRange.a;
    if (dist > range) return vec3(0.0);
    vec3 L = toLight / max(dist, 1e-4);
    float t = dist / max(range, 1e-4);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    float atten = window * window / (1.0 + dist * dist);
    if (type == 2) {
        float cosAngle = dot(normalize(-lt.DirCutoff.xyz), L);
        float outerCos = lt.DirCutoff.w;
        float innerCos = lt.Params.x;
        if (cosAngle < outerCos) return vec3(0.0);
        atten *= smoothstep(outerCos, max(innerCos, outerCos + 1e-3), cosAngle);
        if (atten <= 0.0) return vec3(0.0);
        atten *= SpotShadow(int(lt.Params.y), vWorldPos, N);
    } else {
        if (atten <= 0.0) return vec3(0.0);
        atten *= PointShadow(int(lt.Params.y), vWorldPos, lt.PositionType.xyz, N);
    }
    if (atten <= 0.0) return vec3(0.0);
    return SheenLobe(N, V, L, sheenRoughness) * lt.ColorRange.rgb * atten;
}
#endif

#ifdef _SUBSURFACE
// Subsurface back-lit transmission from one point/spot light.
vec3 ShadePointSpotSSS(uint i, vec3 N, float thickness) {
    Light lt = uLights[i];
    int type = int(lt.PositionType.w);
    vec3 toLight = lt.PositionType.xyz - vWorldPos;
    float dist = length(toLight);
    float range = lt.ColorRange.a;
    if (dist > range) return vec3(0.0);
    vec3 L = toLight / max(dist, 1e-4);
    float t = dist / max(range, 1e-4);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    float atten = window * window / (1.0 + dist * dist);
    if (type == 2) {
        float cosAngle = dot(normalize(-lt.DirCutoff.xyz), L);
        float outerCos = lt.DirCutoff.w;
        if (cosAngle < outerCos) return vec3(0.0);
        atten *= smoothstep(outerCos, max(lt.Params.x, outerCos + 1e-3), cosAngle);
    }
    if (atten <= 0.0) return vec3(0.0);
    return SubsurfaceTransmission(N, L, lt.ColorRange.rgb * atten, thickness);
}
#endif

// Per-axis blend weights for triplanar projection, sharpened (raised to a power) so the blend
// zone between two faces is narrow instead of muddying most of the surface.
vec3 TriplanarWeights(vec3 n) {
    vec3 w = pow(abs(n), vec3(4.0));
    return w / max(w.x + w.y + w.z, 1e-5);
}

// Blends the same texture sampled from the 3 world-space axis planes. rgba so it works for both
// colour maps and single-channel (metallic/roughness/AO) maps read via .r/.g/.b afterward.
vec4 SampleTriplanar(sampler2D tex, vec3 worldPos, vec3 w, float scale) {
    vec4 cx = texture(tex, worldPos.zy * scale);
    vec4 cy = texture(tex, worldPos.xz * scale);
    vec4 cz = texture(tex, worldPos.xy * scale);
    return cx * w.x + cy * w.y + cz * w.z;
}

// This fragment's froxel index in the 16 x 9 x 24 cluster grid (#120).
uint clusterIndex() {
    uvec2 tile = uvec2(gl_FragCoord.xy / (uClusterScreenSize / vec2(float(C_GX), float(C_GY))));
    tile = min(tile, uvec2(C_GX - 1u, C_GY - 1u));
    float viewZ = -(uView * vec4(vWorldPos, 1.0)).z;             // positive view-space distance
    viewZ = clamp(viewZ, uClusterZParams.x, uClusterZParams.y);
    uint slice = uint(max(log(viewZ) * uClusterZParams.z + uClusterZParams.w, 0.0));
    slice = min(slice, C_GZ - 1u);
    return tile.x + tile.y * C_GX + slice * C_GX * C_GY;
}

void main() {
    // Triplanar mode skips the mesh's own UVs entirely (they're what's stretching), sampling
    // every map from world position/normal instead. Normal maps are the one exception - proper
    // triplanar normal blending needs a whiteout-blend reconstruction per plane, which no
    // material here currently needs, so uHasNormalMap still just uses vUV.
    bool tri = uTriplanar == 1;
    vec3 triW = tri ? TriplanarWeights(normalize(vNormal)) : vec3(0.0);
    vec4 albedoSample = tri ? SampleTriplanar(uAlbedoMap, vWorldPos, triW, uTriplanarScale)
                             : texture(uAlbedoMap, vUV);
    vec3 albedo = (uHasAlbedoMap == 1 ? albedoSample.rgb : vec3(1.0)) * uBaseColor;

    float metallic = uMetallic;
    float roughness = uRoughness;
    if (uHasMetallicRoughnessMap == 1) {
        vec3 mr = (tri ? SampleTriplanar(uMetallicRoughnessMap, vWorldPos, triW, uTriplanarScale)
                       : texture(uMetallicRoughnessMap, vUV)).rgb;
        roughness = mr.g;
        metallic = mr.b;
    } else {
        if (uHasRoughnessMap == 1) roughness = (tri ? SampleTriplanar(uRoughnessMap, vWorldPos, triW, uTriplanarScale)
                                                     : texture(uRoughnessMap, vUV)).r;
        if (uHasMetallicMap == 1) metallic = (tri ? SampleTriplanar(uMetallicMap, vWorldPos, triW, uTriplanarScale)
                                                   : texture(uMetallicMap, vUV)).r;
    }
    float ao = uHasAOMap == 1 ? (tri ? SampleTriplanar(uAOMap, vWorldPos, triW, uTriplanarScale)
                                      : texture(uAOMap, vUV)).r : 1.0;

    vec3 N = normalize(vNormal);
    if (uHasNormalMap == 1) {
        vec3 tangentNormal = texture(uNormalMap, vUV).rgb * 2.0 - 1.0;
        N = normalize(vTBN * tangentNormal);
    }

    // --- Scene-view debug draw modes (#236 R2) ---
    if (uDebugView != 0) {
        if (uDebugView == 1) {            // Normals
            FragColor = vec4(N * 0.5 + 0.5, 1.0);
        } else if (uDebugView == 2) {     // Shadow cascades
            if (uShadowEnabled == 0) { FragColor = vec4(0.30, 0.30, 0.32, 1.0); return; }
            float vz = -(uView * vec4(vWorldPos, 1.0)).z;
            int ci = 0;
            if (vz > uCascadeSplits.x) ci = 1;
            if (vz > uCascadeSplits.y) ci = 2;
            if (vz > uCascadeSplits.z) ci = 3;
            vec3 cc[4] = vec3[4](vec3(0.95,0.35,0.35), vec3(0.35,0.9,0.4),
                                 vec3(0.4,0.6,1.0),    vec3(1.0,0.95,0.4));
            FragColor = vec4(mix(albedo, cc[ci], 0.55), 1.0);
        } else {                          // 3 = Mip / texel density
            float lod = uHasAlbedoMap == 1 ? textureQueryLod(uAlbedoMap, vUV).x : 0.0;
            float t = clamp(lod / 6.0, 0.0, 1.0);
            FragColor = vec4(mix(vec3(0.15,0.5,1.0), vec3(1.0,0.25,0.15), t), 1.0);
        }
        return;
    }

    vec3 emissiveEarly = uHasEmissiveMap == 1 ? (tri ? SampleTriplanar(uEmissiveMap, vWorldPos, triW, uTriplanarScale)
                                                      : texture(uEmissiveMap, vUV)).rgb : uEmissiveColor;
    if (uUnlit == 1) {
        vec3 flatColor = albedo + emissiveEarly;
        FragColor = vec4(uApplyTonemap == 1 ? pow(flatColor, vec3(1.0 / 2.2)) : flatColor, 1.0);
        return;
    }

    vec3 V = normalize(uViewPos - vWorldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

#ifdef _ANISO
    float anisoAngle = uAnisotropyRotation * 6.28318530718;
    float anisoC = cos(anisoAngle), anisoS = sin(anisoAngle);
    vec3 anisoT = anisoC * vTBN[0] + anisoS * vTBN[1];
    vec3 anisoB = -anisoS * vTBN[0] + anisoC * vTBN[1];
#endif
#ifdef _CLEARCOAT
    vec3 LoCc = vec3(0.0);
    float ccStrength = uClearCoat;
    if (uHasClearCoatMap == 1) ccStrength *= texture(uClearCoatMap, vUV).r;
    float ccRough = max(uClearCoatRoughness * uClearCoatRoughness, 0.001);
    float Fc = ClearCoatFresnel(max(dot(N, V), 0.0), ccStrength);
#endif
#ifdef _SHEEN
    vec3 LoSheen = vec3(0.0);
    float sheenRough = max(uSheenRoughness, 0.045);
    float sheenDFG = SheenDFG(max(dot(N, V), 0.0), sheenRough);
#endif
#ifdef _SUBSURFACE
    vec3 LoSSS = vec3(0.0);
    float sssThickness = uThickness;
    if (uHasThicknessMap == 1) sssThickness *= texture(uThicknessMap, vUV).r;
#endif

    vec3 Lo = vec3(0.0);

    // Directional lights are not clustered (infinite extent) — one cheap pass for type 0.
    // Packed at the front of uLights[] (#188), so this loops exactly uDirectionalCount entries
    // instead of scanning the whole buffer and skipping non-directional ones by type.
    for (uint i = 0u; i < uDirectionalCount; ++i) {
        vec3 L = normalize(-uLights[i].DirCutoff.xyz); // DirCutoff.xyz travels forward; L points back
        vec3 radiance = uLights[i].ColorRange.rgb * SunShadow(vWorldPos, N, L);
#ifdef _ANISO
        Lo += ShadeLightAniso(N, V, L, radiance, albedo, F0, metallic, roughness, anisoT, anisoB);
#else
        Lo += ShadeLight(N, V, L, radiance, albedo, F0, metallic, roughness);
#endif
#ifdef _CLEARCOAT
        LoCc += radiance * max(dot(N, L), 0.0) * ClearCoatLobe(N, V, L, ccRough) * ccStrength;
#endif
#ifdef _SHEEN
        LoSheen += SheenLobe(N, V, L, sheenRough) * radiance;
#endif
#ifdef _SUBSURFACE
        // Wrap the base diffuse and accumulate back-lit transmission for directional lights.
        float wrapDot = WrappedDiffuse(dot(N, L), 0.5);
        Lo += (uSubsurfaceColor * albedo / PI) * radiance * wrapDot;
        LoSSS += SubsurfaceTransmission(N, L, radiance, sssThickness);
#endif
    }

    // Point + spot lights: this fragment's froxel list when clustering is active (#120),
    // otherwise every light (offscreen preview path, no compute pass).
    if (uClusterEnabled == 1) {
        uint cl = clusterIndex();
        uint offset = uClusterRange[cl].offset;
        uint count = uClusterRange[cl].count;
        for (uint j = 0u; j < count; ++j) {
            uint li = uClusterLightIndices[offset + j];
#ifdef _ANISO
            Lo += ShadePointSpotAniso(li, N, V, albedo, F0, metallic, roughness, anisoT, anisoB);
#else
            Lo += ShadePointSpot(li, N, V, albedo, F0, metallic, roughness);
#endif
#ifdef _CLEARCOAT
            LoCc += ShadePointSpotCC(li, N, V, ccRough) * ccStrength;
#endif
#ifdef _SHEEN
            LoSheen += ShadePointSpotSheen(li, N, V, sheenRough);
#endif
#ifdef _SUBSURFACE
            LoSSS += ShadePointSpotSSS(li, N, sssThickness);
#endif
        }
    } else {
        for (uint i = 0u; i < uLightCount; ++i) {
            if (int(uLights[i].PositionType.w) == 0) continue;
#ifdef _ANISO
            Lo += ShadePointSpotAniso(i, N, V, albedo, F0, metallic, roughness, anisoT, anisoB);
#else
            Lo += ShadePointSpot(i, N, V, albedo, F0, metallic, roughness);
#endif
#ifdef _CLEARCOAT
            LoCc += ShadePointSpotCC(i, N, V, ccRough) * ccStrength;
#endif
#ifdef _SHEEN
            LoSheen += ShadePointSpotSheen(i, N, V, sheenRough);
#endif
#ifdef _SUBSURFACE
            LoSSS += ShadePointSpotSSS(i, N, sssThickness);
#endif
        }
    }

    // PR15 — SSAO: sample the blurred occlusion map at this fragment's screen position.
    // ssaoFactor == 1.0 when SSAO is off (uSSAOEnabled == 0, the GL default) — no change.
    float ssaoFactor = uSSAOEnabled == 1
        ? texture(uSSAOMap, gl_FragCoord.xy / uScreenSize).r
        : 1.0;

    // Ambient. With IBL probes bound (#196) this is the standard split-sum approximation:
    // a direction-dependent diffuse term from the cosine-convolved irradiance cube, plus a
    // specular term from the roughness-mipped prefiltered cube scaled by the BRDF LUT's
    // (scale, bias) on F0. Purely ADDITIVE with the direct lighting in Lo above - nothing
    // about the Cook-Torrance path changed, so directly lit surfaces shade exactly as before
    // and only what used to be a flat vec3(0.03) is different. Without probes (the offscreen
    // preview renderers, which have no sky to bake from) it falls back to that old constant.
    vec3 ambient;
    if (uIBLEnabled == 1) {
        float NdotV = max(dot(N, V), 0.0);
        vec3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
        // Metals have no diffuse; what isn't reflected is what's left to scatter.
        vec3 kD = (1.0 - F) * (1.0 - metallic);

        vec3 irradiance = texture(uIrradianceMap, N).rgb;
        vec3 diffuseIBL = irradiance * albedo;

        vec3 R = reflect(-V, N);
        vec3 prefiltered = textureLod(uPrefilteredMap, R, roughness * uIBLSpecularMaxLod).rgb;
#ifdef _REFLECTION_PROBES
        // Parallax box projection: for each bound probe, clip R to the probe box and sample
        // the sky specular cube with the corrected direction, then blend by weight.
        // Zero probes → uProbeCount == 0 → falls through to the sky sample above.
        if (uProbeCount > 0) {
            vec3 blendedPrefilter = vec3(0.0);
            for (int pi = 0; pi < uProbeCount; ++pi) {
                // Lagarde's box intersection: find t where R exits the probe box.
                vec3 rInv = vec3(
                    abs(R.x) > 1e-6 ? 1.0 / R.x : (R.x >= 0.0 ? 1e6 : -1e6),
                    abs(R.y) > 1e-6 ? 1.0 / R.y : (R.y >= 0.0 ? 1e6 : -1e6),
                    abs(R.z) > 1e-6 ? 1.0 / R.z : (R.z >= 0.0 ? 1e6 : -1e6));
                vec3 rbmax = (uProbeCenter[pi] + uProbeHalfSize[pi] - vWorldPos) * rInv;
                vec3 rbmin = (uProbeCenter[pi] - uProbeHalfSize[pi] - vWorldPos) * rInv;
                vec3 rbminmax;
                rbminmax.x = (R.x >= 0.0) ? rbmax.x : rbmin.x;
                rbminmax.y = (R.y >= 0.0) ? rbmax.y : rbmin.y;
                rbminmax.z = (R.z >= 0.0) ? rbmax.z : rbmin.z;
                float fa = max(min(min(rbminmax.x, rbminmax.y), rbminmax.z), 0.0);
                vec3 intersect = vWorldPos + R * fa;
                vec3 correctedR = normalize(intersect - uProbeCenter[pi]);
                blendedPrefilter += textureLod(uPrefilteredMap, correctedR,
                                               roughness * uIBLSpecularMaxLod).rgb * uProbeBlend[pi];
            }
            prefiltered = blendedPrefilter;
        }
#endif
        vec2 ab = texture(uBrdfLut, vec2(NdotV, roughness)).rg;
        vec3 specularIBL = prefiltered * (F * ab.x + ab.y);

        ambient = (kD * diffuseIBL + specularIBL) * ao * uIBLIntensity;
    } else {
        ambient = vec3(0.03) * albedo * ao;
    }
    ambient *= ssaoFactor;
    vec3 emissive = emissiveEarly;

#ifdef _CLEARCOAT
    // Clear coat intercepts energy before it reaches the base: attenuate Lo and ambient by (1-Fc).
    Lo *= (1.0 - Fc);
    ambient *= (1.0 - Fc);
#endif
#ifdef _SHEEN
    // Sheen energy conservation: reduce base by (1 - sheenDFG * max(uSheen)).
    float sheenConserve = 1.0 - sheenDFG * max(uSheen.r, max(uSheen.g, uSheen.b));
    Lo *= sheenConserve;
    ambient *= sheenConserve;
#endif
    vec3 color = ambient + Lo + emissive;
#ifdef _CLEARCOAT
    color += LoCc;
#endif
#ifdef _SHEEN
    color += LoSheen;
#endif
#ifdef _SUBSURFACE
    color += LoSSS;
#endif
#ifdef _TRANSMISSION
    {
        // Screen-space refraction: refract view ray through the surface, project to screen UVs,
        // then sample the opaque color with roughness-based LOD for frosted-glass blur.
        float eta = 1.0 / max(uIOR, 1.001);
        vec3 refrDir = refract(-V, N, eta);
        // Project the refracted tangential offset to screen UV deltas.
        float screenAspect = uScreenSize.x / max(uScreenSize.y, 1.0);
        vec2 refrOffset = refrDir.xy * vec2(1.0, screenAspect) * 0.08 * uTransmissionStrength;
        vec2 screenUV = gl_FragCoord.xy / uScreenSize;
        float maxLod = log2(max(uScreenSize.x, uScreenSize.y));
        float refrLod = roughness * maxLod * 0.5;
        vec3 refrColor = textureLod(uOpaqueColor, clamp(screenUV + refrOffset, vec2(0.001), vec2(0.999)), refrLod).rgb;
        color = mix(color, refrColor, uTransmissionStrength * (1.0 - metallic));
    }
#endif
    if (uApplyTonemap == 1) {
        color = color / (color + vec3(1.0)); // Reinhard tonemap
        color = pow(color, vec3(1.0 / 2.2)); // gamma correct
    }
    // else: leave linear HDR for the shared Tonemapper pass.

    FragColor = vec4(color, uAlphaBlend != 0 ? uOpacity : 1.0);
}
// OIT (order-independent transparency) is explicitly out of scope for PR 9.
// Transparent materials use back-to-front painter's algorithm via the transparent draw list.
