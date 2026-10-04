#version 460 core
// Blood decals. The surface under each pixel comes back out of the depth of the static
// geometry drawn so far; inside the decal's box it gets a layer of blood. The layer is lit like any mesh -
// its own albedo and wet GGX coat under the same sun, shadows, clustered lights and sky (ModelShading.glsl)
// - and covers the surface by how thick it is: the thin edges let some of the surface through, tinted by
// what the blood absorbs, the pooled core hides it. Dual-source blending: dst = Add + dst * Mul.

// ModelShading's functions read vWorldPos; here it's the reconstructed surface.
vec3 vWorldPos;
#include "ModelShading.glsl"

struct BloodDecal {
    mat4 Model;
    mat4 InvModel;
    vec4 RectNorm;
    vec4 RectMask;
    vec4 Params;
    vec4 Axis;
    ivec4 Knife;
    vec4 Grid;
    ivec4 Kind;
};
layout(std430, binding = 9) readonly buffer BloodDecals { BloodDecal uDecals[]; };

layout(binding = 17) uniform sampler2D uSceneDepth;  // single-sample resolve of the static geometry
layout(binding = 18) uniform sampler2D uDecalNorm;   // atlas: rg normal, a coverage
layout(binding = 19) uniform sampler2D uDecalMask;   // atlas: r reveal order, b thick core
layout(binding = 20) uniform sampler2D uDecalLookup; // fade across the box's depth
// The Knife decal libraries (KnifeFxLibrary): BC3 colour + BC5 normals, 2048 (pools, trails) and 1024 layers.
layout(binding = 21) uniform sampler2DArray uKnifeLargeColor;
layout(binding = 22) uniform sampler2DArray uKnifeLargeNormal;
layout(binding = 23) uniform sampler2DArray uKnifeSmallColor;
layout(binding = 24) uniform sampler2DArray uKnifeSmallNormal;
const int kEntryMask = 1, kEntryAlbedo = 2;

vec3 KnifeUV(int cell, vec2 t, vec2 grid, int layer) {
    vec2 c = vec2(float(cell % int(grid.x)), float(cell / int(grid.x)));
    return vec3((c + clamp(t, 0.002, 0.998)) / grid, float(layer));
}
uniform mat4 uInvViewProj;
uniform vec4 uViewport; // x, y, width, height in the target
uniform vec3 uFreshColor; // the layer's albedo, fresh / dried
uniform vec3 uDriedColor;
uniform vec3 uRough;      // BloodPalette: a film's roughness, a pool's, dried

flat in int vDecal;
layout(location = 0, index = 0) out vec4 oAdd;
layout(location = 0, index = 1) out vec4 oMul;

vec3 WorldAt(vec2 frag, float depth) {
    vec2 ndc = (frag - uViewport.xy) / uViewport.zw * 2.0 - 1.0;
    vec4 w = uInvViewProj * vec4(ndc, depth * 2.0 - 1.0, 1.0);
    return w.xyz / w.w;
}

void main() {
    BloodDecal d = uDecals[vDecal];
    ivec2 px = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(uSceneDepth, px, 0).r;
    if (depth >= 1.0) discard; // sky
    vec3 world = WorldAt(gl_FragCoord.xy, depth);
    vec3 local = (d.InvModel * vec4(world, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard;

    // The surface's own normal, from the neighbouring depths (the side with the smaller step, so a
    // silhouette edge doesn't bend it).
    vec3 px1 = WorldAt(gl_FragCoord.xy + vec2(1.0, 0.0), texelFetch(uSceneDepth, px + ivec2(1, 0), 0).r);
    vec3 px0 = WorldAt(gl_FragCoord.xy - vec2(1.0, 0.0), texelFetch(uSceneDepth, px - ivec2(1, 0), 0).r);
    vec3 py1 = WorldAt(gl_FragCoord.xy + vec2(0.0, 1.0), texelFetch(uSceneDepth, px + ivec2(0, 1), 0).r);
    vec3 py0 = WorldAt(gl_FragCoord.xy - vec2(0.0, 1.0), texelFetch(uSceneDepth, px - ivec2(0, 1), 0).r);
    vec3 dx = length(px1 - world) < length(world - px0) ? px1 - world : world - px0;
    vec3 dy = length(py1 - world) < length(world - py0) ? py1 - world : world - py0;
    vec3 Ng = normalize(cross(dx, dy));
    vec3 V = normalize(uViewPos - world);
    if (dot(Ng, V) < 0.0) Ng = -Ng;
    vec3 axis = normalize(d.Axis.xyz);
    float facing = smoothstep(0.35, 0.65, dot(Ng, axis)); // only surfaces the box looks down onto

    // Unity's decal uv: the box's x / z (our z is mirrored), v up; the atlas keeps the PNGs top row first.
    vec2 uv = vec2(local.x, -local.z) + 0.5;
    vec2 t = vec2(uv.x, 1.0 - uv.y);
    if (d.Kind.w != 0) t.x = 1.0 - t.x; // mirrored: another shape out of the same stain
    float cutout = d.Params.x;
    float dry = d.Params.y;
    vec3 bx = normalize(vec3(d.Model[0])), bz = normalize(vec3(d.Model[2]));
    float alpha, core, a;
    vec3 N, albedo;
    float rough;
    vec3 F0 = vec3(0.02);
    bool knife = d.Knife.x >= 0;
    bool blood = true; // a liquid film (not a bullet hole's own material)
    if (knife) {
        // A Knife decal: the cell's own colour (or, for a mask, the film's), coverage and normal map.
        vec3 cu = KnifeUV(d.Knife.z, t, d.Grid.xy, d.Knife.x);
        vec4 col;
        vec2 nxy = vec2(0.0);
        if (d.Kind.x == 0) {
            col = texture(uKnifeLargeColor, cu);
            if (d.Grid.z > 0.0) col = mix(col, texture(uKnifeLargeColor, KnifeUV(d.Knife.w, t, d.Grid.xy, d.Knife.x)), d.Grid.z);
            if (d.Knife.y >= 0) nxy = texture(uKnifeLargeNormal, KnifeUV(d.Knife.z, t, d.Grid.xy, d.Knife.y)).rg * 2.0 - 1.0;
        } else {
            col = texture(uKnifeSmallColor, cu);
            if (d.Grid.z > 0.0) col = mix(col, texture(uKnifeSmallColor, KnifeUV(d.Knife.w, t, d.Grid.xy, d.Knife.x)), d.Grid.z);
            if (d.Knife.y >= 0) nxy = texture(uKnifeSmallNormal, KnifeUV(d.Knife.z, t, d.Grid.xy, d.Knife.y)).rg * 2.0 - 1.0;
        }
        // Spreads out from the middle: the cutout eats the thin, outer coverage first.
        float r = length(t - 0.5) * 2.0;
        float reveal = col.a - cutout * (0.6 + 0.6 * r);
        alpha = clamp(reveal * 6.0, 0.0, 1.0);
        // A bullet hole the round's size: its chipped rim stops close round it (the cell holds a much wider crater).
        if (d.Kind.z == 0 && d.Axis.w > 0.0) alpha *= 1.0 - smoothstep(d.Axis.w * 0.66, d.Axis.w, r);
        core = clamp((col.a - 0.6) * 2.5, 0.0, 1.0) * alpha;
        a = alpha * facing * texture(uDecalLookup, vec2(local.y + 0.5, 0.5)).r * d.Params.z;
        if (a < 0.004) discard;
        vec2 slope = nxy * d.Params.w;
        N = normalize(Ng + bx * slope.x - bz * slope.y);
        float gloss = d.Grid.w;
        float wet = uRough.x; // a film
        if ((d.Kind.y & kEntryAlbedo) != 0) {
            albedo = pow(col.rgb, vec3(2.2)); // authored sRGB
            blood = d.Kind.z != 0; // Real Blood's pools and prints; the bullet holes are their surface's own
            if (blood) {
                // The pack's own colour is a different red from the rest: keep only how light or dark each texel is
                // (its detail) and take the hue from the palette, so every stain is one material.
                float v = clamp(max(albedo.r, max(albedo.g, albedo.b)) / 0.25, 0.0, 1.0);
                albedo = mix(uFreshColor, uDriedColor, dry) * mix(0.8, 1.0, v); // (its baked highlights stay out)
                // The pack's alpha is its edge, not a thickness (Unity blends it as transparency): the body of the
                // stain is solid, or the floor shows through it milky at a grazing look.
                core = clamp(col.a * 1.6, 0.0, 1.0) * alpha;
                albedo *= 0.65; // pools and the prints out of them: the same deep red as a spatter's pooled core
                if (d.Kind.x == 0) wet = uRough.y; // the large library: pools
            }
        } else {
            albedo = mix(uFreshColor, uDriedColor, dry) * mix(1.0, 0.6, core);
        }
        rough = blood ? mix(wet, uRough.z, dry) : 1.0 - gloss;
        if (!blood) F0 = vec3(0.04);
    } else {
        vec4 na = texture(uDecalNorm, d.RectNorm.xy + t * d.RectNorm.zw);
        vec3 mask = texture(uDecalMask, d.RectMask.xy + t * d.RectMask.zw).rgb;
        float coverage = clamp(na.a * 2.0, 0.0, 1.0);
        alpha = clamp((mask.r - cutout) * 20.0, 0.0, 1.0) * coverage;
        core = clamp((mask.r - cutout) * 5.0, 0.0, 1.0) * coverage * mask.b; // the pooled, thicker middle
        a = alpha * facing * texture(uDecalLookup, vec2(local.y + 0.5, 0.5)).r * d.Params.z;
        if (a < 0.004) discard;
        // The film's normal: the map's slope (Unity: (x, 1, y) in box space) bent onto the real surface.
        vec2 slope = (na.xy * 2.0 - 1.0) * d.Params.w;
        N = normalize(Ng + bx * slope.x - bz * slope.y);
        rough = mix(uRough.x, uRough.z, dry);
        // Darker and browner as it dries; the pooled core darker still (more of it).
        albedo = mix(uFreshColor, uDriedColor, dry) * mix(1.0, 0.6, core);
    }
    vWorldPos = world;
    vec3 lit = vec3(0.0);
    for (uint i = 0u; i < uDirectionalCount; ++i) {
        vec3 L = normalize(-uLights[i].DirCutoff.xyz);
        vec3 radiance = uLights[i].ColorRange.rgb * (uLights[i].Params.y >= 0.0 ? SunShadow(world, Ng, L) * CloudShadow(world) : 1.0);
        lit += ShadeLight(N, V, L, radiance, albedo, F0, 0.0, rough);
    }
    if (uClusterEnabled == 1) {
        uint cl = clusterIndex();
        uint off = uClusterRange[cl].offset, count = uClusterRange[cl].count;
        for (uint j = 0u; j < count; ++j) lit += ShadePointSpot(uClusterLightIndices[off + j], N, V, albedo, F0, 0.0, rough);
    }
    if (uIBLEnabled == 1) {
        float NdotV = max(dot(N, V), 0.0);
        vec3 F = FresnelSchlickRoughness(NdotV, F0, rough);
        vec2 ab = texture(uBrdfLut, vec2(NdotV, rough)).rg;
        // Blood on the ground doesn't mirror the whole sky at a grazing look (the horizon and what stands around
        // it occlude it): specular occlusion by the view angle, or a pool goes milky pink from across the room.
        float specOcc = blood ? mix(0.25, 1.0, sqrt(NdotV)) : 1.0;
        vec3 ambient = (1.0 - F) * texture(uIrradianceMap, Ng).rgb * albedo
                     + textureLod(uPrefilteredMap, reflect(-V, N), rough * uIBLSpecularMaxLod).rgb * (F * ab.x + ab.y) * specOcc;
        lit += ambient * uIBLIntensity;
    } else {
        lit += vec3(0.03) * albedo;
    }
    // How much of the surface the layer hides: thin at the streaks' edges, opaque where it pooled.
    float cover = blood ? mix(0.6, 0.97, clamp(core * 1.5 + alpha * 0.3, 0.0, 1.0)) : 1.0;
    vec3 through = sqrt(albedo) * 0.9; // what a thin layer lets through of the surface beneath
    vec3 mul = vec3(1.0) - a * (vec3(1.0) - (1.0 - cover) * through);
    vec3 add = lit * a * cover;
    // Haze between the eye and the surface: the film fades into it the way the surface does.
    float fogKeep = (ApplyFog(vec3(1.0)) - ApplyFog(vec3(0.0))).r;
    vec3 apKeep = ApplyAerialPerspective(vec3(1.0)) - ApplyAerialPerspective(vec3(0.0));
    float keep = fogKeep * apKeep.g;
    oAdd = vec4(add * keep, 0.0);
    oMul = vec4(mix(vec3(1.0), mul, keep), 1.0);
}
