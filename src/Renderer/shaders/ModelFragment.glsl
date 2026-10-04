#version 460 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in mat3 vTBN;
in vec4 vColor; // #113
out vec4 FragColor;

in float vHidden; // ModelVertex: the share of the vertex moved by uHideBones
in vec3 vBindPos;    // the surface in the mesh's bind-pose space (ModelVertex)
in vec3 vBindNormal;

// Every uniform and shading function (lights, shadows, IBL, fog): shared with the blood decals.
#include "ModelShading.glsl"

// Blood splats on this mesh: the BloodRenderer's list for the entity, in the mesh's bind-pose
// space. Each is a box like a world decal - projected along its normal, read from the decal atlas - that turns
// what it covers into blood before the surface is lit. uBloodSplatCount 0 (every draw that has none) costs a branch.
struct BloodSplat {
    vec4 CenterRadius; // xyz centre, w the box's half-size (bind units)
    vec4 NormalDepth;  // xyz out of the surface, w the projection's half-depth
    vec4 Tangent;      // xyz the decal's u axis, w unused
    vec4 Rect;         // atlas rect (offset, size)
    vec4 Params;       // x cutout, y dryness, z opacity, w unused
};
layout(std430, binding = 6) readonly buffer BloodSplats { BloodSplat uBloodSplats[]; };
layout(binding = 28) uniform sampler2D uBloodNormAtlas; // 28, 29: reserved past the material units (ShaderAsset)
layout(binding = 29) uniform sampler2D uBloodMaskAtlas;
uniform int uBloodSplatFirst;
uniform int uBloodSplatCount;
uniform vec4 uBloodFresh; // BloodPalette: rgb fresh, a its roughness on cloth / skin
uniform vec4 uBloodDried; // rgb dried, a its roughness

// How much of this fragment is under blood (x), its pooled core (y) and how dried (z).
vec3 BloodSplatCover() {
    vec3 acc = vec3(0.0);
    vec3 bn = normalize(vBindNormal);
    // The atlas is read inside a loop that skips splats per fragment, where implicit derivatives are undefined:
    // the mip comes from the surface's own footprint instead (bind units per pixel), taken out here.
    float footprint = max(length(fwidth(vBindPos)), 1e-7);
    vec2 atlasSize = vec2(textureSize(uBloodNormAtlas, 0));
    for (int i = 0; i < uBloodSplatCount; ++i) {
        BloodSplat s = uBloodSplats[uBloodSplatFirst + i];
        vec3 d = vBindPos - s.CenterRadius.xyz;
        float r = s.CenterRadius.w;
        if (dot(d, d) > 3.0 * r * r) continue;
        vec3 n = s.NormalDepth.xyz, t = s.Tangent.xyz, b = cross(n, t);
        vec3 l = vec3(dot(d, t), dot(d, n), dot(d, b)) / vec3(2.0 * r, 2.0 * s.NormalDepth.w, 2.0 * r);
        if (any(greaterThan(abs(l), vec3(0.5)))) continue;
        float facing = smoothstep(-0.2, 0.35, dot(bn, n)); // wraps a little round the curve of a limb
        vec2 uv = vec2(l.x, -l.z) + 0.5;
        vec2 tc = s.Rect.xy + vec2(uv.x, 1.0 - uv.y) * s.Rect.zw;
        float lod = log2(max(footprint / (2.0 * r) * max(s.Rect.z * atlasSize.x, s.Rect.w * atlasSize.y), 1e-4));
        float coverage = clamp(textureLod(uBloodNormAtlas, tc, lod).a * 2.0, 0.0, 1.0);
        vec3 mask = textureLod(uBloodMaskAtlas, tc, lod).rgb;
        float a = clamp((mask.r - s.Params.x) * 20.0, 0.0, 1.0) * coverage * facing * s.Params.z;
        float core = clamp((mask.r - s.Params.x) * 5.0, 0.0, 1.0) * coverage * mask.b;
        if (a > acc.x) acc.z = s.Params.y;
        acc.x = max(acc.x, a);
        acc.y = max(acc.y, core * facing);
    }
    return acc;
}

void main() {
    if (uNearHide > 0.0) {
        vec3 d = vWorldPos - uViewPos;
        if (uNearHideWidth > 0.0) { // FirstPersonBodyNearHidden
            float side = dot(d, uNearHideRight);
            vec3 rest = d - uNearHideRight * side;
            if ((side * side) / (uNearHideWidth * uNearHideWidth) + dot(rest, rest) / (uNearHide * uNearHide) < 1.0) discard;
        } else if (dot(d, d) < uNearHide * uNearHide) {
            discard;
        }
    }
    if (vHidden > 0.5) discard;
    // Triplanar mode skips the mesh's own UVs entirely (they're what's stretching), sampling
    // every map from world position/normal instead. Normal maps are the one exception - proper
    // triplanar normal blending needs a whiteout-blend reconstruction per plane, which no
    // material here currently needs, so uHasNormalMap still just uses the mesh UVs.
    bool tri = uTriplanar == 1;
    vec3 triW = tri ? TriplanarWeights(normalize(vNormal)) : vec3(0.0);
    // #102 — per-material UV tiling / offset, then parallax offset from a height map.
    vec2 uv = vUV * (uUVTiling == vec2(0.0) ? vec2(1.0) : uUVTiling) + uUVOffset;
    bool backFace = uDoubleSided == 1 && !gl_FrontFacing;
    if (uHasHeightMap == 1 && !tri && uParallaxScale > 0.0) {
        // Parallax occlusion mapping: march the view ray through the height field in tangent
        // space (more layers at grazing angles), then interpolate between the last two samples.
        vec3 Vt = normalize(transpose(vTBN) * (uViewPos - vWorldPos));
        if (backFace) Vt.z = -Vt.z;
        float layers = mix(24.0, 8.0, clamp(Vt.z, 0.0, 1.0));
        float layerStep = 1.0 / layers;
        vec2 delta = Vt.xy / max(Vt.z, 0.1) * uParallaxScale * layerStep;
        float layerDepth = 0.0;
        float depthHere = 1.0 - texture(uHeightMap, uv).r;
        vec2 cur = uv;
        for (int k = 0; k < 32 && layerDepth < depthHere; ++k) {
            cur -= delta;
            depthHere = 1.0 - texture(uHeightMap, cur).r;
            layerDepth += layerStep;
        }
        vec2 prev = cur + delta;
        float after = depthHere - layerDepth;
        float before = (1.0 - texture(uHeightMap, prev).r) - layerDepth + layerStep;
        float w = after / (after - before + 1e-5);
        uv = mix(cur, prev, clamp(w, 0.0, 1.0));
    }
    vec4 albedoSample = tri ? SampleTriplanar(uAlbedoMap, vWorldPos, triW, uTriplanarScale)
                             : texture(uAlbedoMap, uv);
    if (uUseVertexColor == 1) albedoSample.a *= vColor.a; // #113
    if (uAlphaClip == 1 && uHasAlbedoMap == 1 && albedoSample.a < uAlphaCutoff) discard; // #101
    vec3 albedo = (uHasAlbedoMap == 1 ? albedoSample.rgb : vec3(1.0)) * uBaseColor;
    if (uUseVertexColor == 1) albedo *= vColor.rgb; // #113
    vec2 detailUV = vUV * (uDetailTiling == vec2(0.0) ? vec2(1.0) : uDetailTiling);
    // Detail albedo, Unity-style "x2" in linear space: 50% grey (0.2159 linear) leaves the colour unchanged.
    if (uHasDetailAlbedoMap == 1) albedo *= texture(uDetailAlbedoMap, detailUV).rgb * 4.6317;

    // #102 — glTF / Unity semantics: a map is SCALED by its factor (factor * texture), it no
    // longer replaces it. Materials saved before this carry factor 1 where a map is set (see
    // MaterialAsset / SceneSerializer), so they render exactly as before.
    float metallic = uMetallic;
    float roughness = uRoughness;
    if (uHasMetallicRoughnessMap == 1) {
        vec3 mr = (tri ? SampleTriplanar(uMetallicRoughnessMap, vWorldPos, triW, uTriplanarScale)
                       : texture(uMetallicRoughnessMap, uv)).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    } else {
        if (uHasRoughnessMap == 1) roughness *= (tri ? SampleTriplanar(uRoughnessMap, vWorldPos, triW, uTriplanarScale)
                                                      : texture(uRoughnessMap, uv)).r;
        if (uHasMetallicMap == 1) metallic *= (tri ? SampleTriplanar(uMetallicMap, vWorldPos, triW, uTriplanarScale)
                                                    : texture(uMetallicMap, uv)).r;
    }
    float ao = uHasAOMap == 1 ? (tri ? SampleTriplanar(uAOMap, vWorldPos, triW, uTriplanarScale)
                                      : texture(uAOMap, uv)).r : 1.0;
    // A garment's inside (a collar's, a tucked shirt's past the waistband) is lit by what little gets in:
    // lit like the outside it read as a bright band.
    if (backFace && uClothInterior == 1) { albedo *= 0.3; ao *= 0.4; }

    vec3 N = normalize(vNormal);
    if (uHasNormalMap == 1 || uHasDetailNormalMap == 1) {
        // #102 — strength (scales the tangent-space slope), DirectX green flip, and a detail
        // normal map combined with whiteout blending.
        vec3 tn = uHasNormalMap == 1 ? texture(uNormalMap, uv).rgb * 2.0 - 1.0 : vec3(0.0, 0.0, 1.0);
        if (uNormalFlipY == 1) tn.y = -tn.y;
        if (uHasDetailNormalMap == 1) {
            vec3 dn = texture(uDetailNormalMap, detailUV).rgb * 2.0 - 1.0;
            if (uNormalFlipY == 1) dn.y = -dn.y;
            tn = vec3(tn.xy + dn.xy, tn.z * dn.z);
        }
        tn.xy *= uNormalStrength > 0.0 ? uNormalStrength : 1.0;
        mat3 tbn = vTBN;
        if (backFace) tbn[2] = -tbn[2];
        N = normalize(tbn * tn);
    } else if (backFace) {
        N = -N;
    }
    if (uBloodSplatCount > 0) {
        // Soaked in: fabric and skin take the blood's colour, darker where it pooled, glossy while wet.
        vec3 blood = BloodSplatCover();
        if (blood.x > 0.0) {
            vec3 bloodAlbedo = mix(uBloodFresh.rgb, uBloodDried.rgb, blood.z) * mix(1.0, 0.6, blood.y);
            albedo = mix(albedo, bloodAlbedo, blood.x * 0.95);
            roughness = mix(roughness, mix(uBloodFresh.a, uBloodDried.a, blood.z), blood.x);
            metallic = mix(metallic, 0.0, blood.x);
        }
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
            float lod = uHasAlbedoMap == 1 ? textureQueryLod(uAlbedoMap, uv).x : 0.0;
            float t = clamp(lod / 6.0, 0.0, 1.0);
            FragColor = vec4(mix(vec3(0.15,0.5,1.0), vec3(1.0,0.25,0.15), t), 1.0);
        }
        return;
    }

    // #102 — the map is TINTED by Emissive Color x Strength (uEmissiveColor carries both), the
    // same as Unity and glTF; it used to replace them, so the colour/strength controls did
    // nothing once a map was assigned.
    vec3 emissiveEarly = uHasEmissiveMap == 1 ? (tri ? SampleTriplanar(uEmissiveMap, vWorldPos, triW, uTriplanarScale)
                                                      : texture(uEmissiveMap, uv)).rgb * uEmissiveColor : uEmissiveColor;
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
    if (uHasClearCoatMap == 1) ccStrength *= texture(uClearCoatMap, uv).r;
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
    if (uHasThicknessMap == 1) sssThickness *= texture(uThicknessMap, uv).r;
#endif

    vec3 Lo = vec3(0.0);

    // Directional lights are not clustered (infinite extent) — one cheap pass for type 0.
    // Packed at the front of uLights[] (#188), so this loops exactly uDirectionalCount entries
    // instead of scanning the whole buffer and skipping non-directional ones by type.
    for (uint i = 0u; i < uDirectionalCount; ++i) {
        if (!LightAffects(i)) continue; // #203
        vec3 L = normalize(-uLights[i].DirCutoff.xyz); // DirCutoff.xyz travels forward; L points back
        // #102 — only the directional light that owns the cascade map samples it (Params.y 0);
        // any other directional used to be shadowed by the primary sun's map.
        vec3 radiance = uLights[i].ColorRange.rgb * (uLights[i].Params.y >= 0.0 ? SunShadow(vWorldPos, N, L) * CloudShadow(vWorldPos) : 1.0);
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
            if (!LightAffects(li)) continue; // #203
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
            if (int(uLights[i].PositionType.w) == 0 || !LightAffects(i)) continue;
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
        ? pow(texture(uSSAOMap, gl_FragCoord.xy / uScreenSize).r, uSSAOIntensity > 0.0 ? uSSAOIntensity : 1.0)
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
        vec3 specularIBL = uNoGlossyReflections == 1 ? vec3(0.0) : prefiltered * (F * ab.x + ab.y);

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
        // #102 — the bend is how far the refracted ray deviates from the straight-through view
        // ray, measured in VIEW space so it maps onto the screen (it used the world-space ray's
        // xy, which pointed the wrong way and rotated with the camera).
        vec3 viewIn  = normalize(mat3(uView) * -V);
        vec3 viewOut = normalize(mat3(uView) * refrDir);
        float screenAspect = uScreenSize.x / max(uScreenSize.y, 1.0);
        vec2 refrOffset = (viewOut.xy - viewIn.xy) * vec2(1.0 / screenAspect, 1.0) * 0.25 * uTransmissionStrength;
        vec2 screenUV = gl_FragCoord.xy / uScreenSize;
        float maxLod = log2(max(uScreenSize.x, uScreenSize.y));
        float refrLod = roughness * maxLod * 0.5;
        vec3 refrColor = textureLod(uOpaqueColor, clamp(screenUV + refrOffset, vec2(0.001), vec2(0.999)), refrLod).rgb;
        color = mix(color, refrColor, uTransmissionStrength * (1.0 - metallic));
    }
#endif
    color = ApplyFog(color); // #162
    color = ApplyAerialPerspective(color);

    if (uApplyTonemap == 1) {
        color = color / (color + vec3(1.0)); // Reinhard tonemap
        color = pow(color, vec3(1.0 / 2.2)); // gamma correct
    }
    // else: leave linear HDR for the shared Tonemapper pass.

    // #102 — the albedo map's alpha drives transparency too (a PNG with alpha fades where it's
    // transparent), multiplied by the material's Opacity.
    float surfaceAlpha = uOpacity * (uHasAlbedoMap == 1 ? albedoSample.a : 1.0);
    FragColor = vec4(color, uAlphaBlend != 0 ? surfaceAlpha : 1.0);
}
// OIT (order-independent transparency) is explicitly out of scope for PR 9.
// Transparent materials use back-to-front painter's algorithm via the transparent draw list.
