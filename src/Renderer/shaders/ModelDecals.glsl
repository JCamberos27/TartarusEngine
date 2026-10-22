// Clustered PBR decals (docs/DECALS.md), applied to the surface after its material and Grunge are
// sampled and before blood and lighting. DecalSystem uploads the visible decals (SSBO 11),
// ClusterDecalCull.comp.glsl bins them per froxel (13, 14), and each fragment walks its froxel's
// list bottom to top, so a higher SortOrder lands on top. Included from ModelFragment.glsl only.

// Mirrors Decals::GpuDecal (DecalTypes.h), 128 bytes std430.
struct GpuDecal {
    vec4 WorldToBox[3]; // rows: world -> unit box [-0.5, 0.5]^3, +Z out of the surface
    vec4 Rect;          // slot uv offset + size in the layer (atlas frame folded in)
    uvec4 Ids;          // x layer | maxLod << 8 | channels << 16; y receiver layers; z flags | heightBlend << 24; w sort
    uvec4 H0;           // half2: (tint.rg) (tint.b, albedoOp) (normalOp, roughOp) (metalOp, aoOp)
    uvec4 H1;           // (emissive.rg) (emissive.b, emissiveOp) (angleCosStart, angleCosEnd) (edgeFade, heightContrast)
    uvec4 H2;           // (rough, metal) (relief, wear) (fade, bleach) (peel, buff)
};
layout(std430, binding = 11) readonly buffer DecalBuffer { GpuDecal uDecals[]; };
struct DecalClusterRange { uint offset; uint count; };
layout(std430, binding = 13) readonly buffer DecalClusterRanges { DecalClusterRange uDecalRange[]; };
layout(std430, binding = 14) readonly buffer DecalClusterIndex { uint uDecalCursor; uint uDecalOverflow; uint uDecalIndex[]; };

layout(binding = 22) uniform sampler2DArray uDecalAlbedo;  // sRGB BC3: rgb colour, a opacity
layout(binding = 23) uniform sampler2DArray uDecalNormal;  // BC5 tangent xy
layout(binding = 24) uniform sampler2DArray uDecalSurface; // BC3: r rough, g metal, b AO, a height
layout(binding = 25) uniform sampler2DArray uDecalAux;     // BC4: emissive mask, or the peel mask

uniform int uDecalCount;        // visible decals this view; 0 skips everything below in one uniform branch
uniform int uDecalStatic;       // this draw: 1 the static world, 0 skinned / rigidbody
uniform int uReceiveDecals;     // the material's Receive Decals
uniform float uDecalBlacklight; // how much UV-paint glows (no UV light exists yet: 0)

// Decals::Channel / Decals::Flag (DecalTypes.h).
const uint DECAL_CH_ALBEDO = 1u, DECAL_CH_NORMAL = 2u, DECAL_CH_ROUGH = 4u, DECAL_CH_METAL = 8u, DECAL_CH_AO = 16u,
           DECAL_CH_EMISSIVE = 32u, DECAL_CH_HEIGHT = 64u, DECAL_CH_PAINT = 128u;
const uint DECAL_STATIC = 1u, DECAL_DYNAMIC = 2u, DECAL_UV_PAINT = 4u, DECAL_HAS_NORMAL = 8u, DECAL_HAS_SURFACE = 16u,
           DECAL_HAS_AUX = 32u, DECAL_LUMA = 64u, DECAL_INVERT = 128u;
const uint DECAL_SUB_AUTO = 256u;
const float DECAL_PAGE = 4096.0; // Decals::PAGE_SIZE

// ---- Substrates (Substrate.h): the PBR material under the paint -----------------------------------
// Mirrors Decals::GpuSubstrate (DecalTypes.h), 80 bytes std430. Its maps share one slot of the
// decal arrays: albedo, normal, surface (r roughness, b AO).
struct GpuSubstrate {
    vec4 Rect;         // slot uv offset + size in the layer
    vec4 Crop;         // the image rect that repeats: x0, y0, x1, y1 (v down)
    vec4 TintStrength; // rgb tint, a strength
    vec4 Params;       // x metres per repeat, y/z roughness min/max, w 1 = mirrored repeat
    uvec4 Ids;         // x layer, y maxLod, z has-normal / has-surface flags
};
layout(std430, binding = 16) readonly buffer SubstrateBuffer { GpuSubstrate uSubstrates[]; };
uniform int uSubstrateOn;       // the table and the arrays are bound (views that prepared decals)
uniform int uMaterialSubstrate; // this material's substrate: index + 1, 0 = none

struct SubstrateSample {
    vec3 albedo;
    vec3 n;       // tangent space in the projection plane (x along U, y up the image, z out)
    float rough;
    float ao;
    float strength;
    vec3 U, V, W; // the plane: U right, V up the image, W out (U x V = W)
};

// World-space planar projection on the receiver's dominant axis: every decal and the Grunge
// reveal on one wall read the same texels, so overlapping peels show one continuous substrate.
SubstrateSample SampleSubstrate(uint idx, vec3 geoN, vec3 dpx, vec3 dpy) {
    GpuSubstrate s = uSubstrates[idx];
    SubstrateSample o;
    vec3 an = abs(geoN);
    if (an.y >= an.x && an.y >= an.z) {
        float sg = geoN.y >= 0.0 ? 1.0 : -1.0;
        o.W = vec3(0.0, sg, 0.0); o.V = vec3(0.0, 0.0, -1.0); o.U = vec3(sg, 0.0, 0.0);
    } else if (an.x >= an.z) {
        float sg = geoN.x >= 0.0 ? 1.0 : -1.0;
        o.W = vec3(sg, 0.0, 0.0); o.V = vec3(0.0, 1.0, 0.0); o.U = vec3(0.0, 0.0, -sg);
    } else {
        float sg = geoN.z >= 0.0 ? 1.0 : -1.0;
        o.W = vec3(0.0, 0.0, sg); o.V = vec3(0.0, 1.0, 0.0); o.U = vec3(sg, 0.0, 0.0);
    }
    float inv = 1.0 / max(s.Params.x, 0.01);
    vec2 t = vec2(dot(vWorldPos, o.U), -dot(vWorldPos, o.V)) * inv; // in repeats, v down the image
    bool mirror = s.Params.w > 0.5;
    vec2 f = mirror ? 1.0 - abs(mod(t, 2.0) - 1.0) : fract(t);
    vec2 flip = mirror ? 1.0 - 2.0 * mod(floor(t), 2.0) : vec2(1.0); // -1 in the mirrored repeats
    vec2 scale = inv * (s.Crop.zw - s.Crop.xy) * s.Rect.zw;
    vec2 gx = vec2(dot(dpx, o.U), -dot(dpx, o.V)) * scale;
    vec2 gy = vec2(dot(dpy, o.U), -dot(dpy, o.V)) * scale;
    // Mips past the slot's maxLod would read its neighbours (as in ApplyDecals).
    float maxLod = float(s.Ids.y);
    float lod = 0.5 * log2(max(max(dot(gx, gx), dot(gy, gy)) * DECAL_PAGE * DECAL_PAGE, 1e-12));
    if (lod > maxLod) {
        float k = exp2(maxLod - lod);
        gx *= k;
        gy *= k;
        lod = maxLod;
    }
    float inset = 0.5 * exp2(max(lod, 0.0)) / DECAL_PAGE;
    vec2 lo = s.Rect.xy + s.Crop.xy * s.Rect.zw, hi = s.Rect.xy + s.Crop.zw * s.Rect.zw;
    vec2 tc = clamp(lo + f * (hi - lo), lo + inset, hi - inset);
    vec3 tc3 = vec3(tc, float(s.Ids.x));
    o.albedo = textureGrad(uDecalAlbedo, tc3, gx, gy).rgb * s.TintStrength.rgb;
    o.n = vec3(0.0, 0.0, 1.0);
    if ((s.Ids.z & DECAL_HAS_NORMAL) != 0u) {
        o.n = UnpackNormal(vec3(textureGrad(uDecalNormal, tc3, gx, gy).rg, 0.0));
        o.n.xy *= flip;
    }
    vec4 surf = (s.Ids.z & DECAL_HAS_SURFACE) != 0u ? textureGrad(uDecalSurface, tc3, gx, gy) : vec4(1.0);
    o.rough = mix(s.Params.y, s.Params.z, surf.r);
    o.ao = surf.b;
    o.strength = s.TintStrength.a;
    return o;
}

// The substrate's normal whiteout-blended onto N in its plane, by w.
vec3 SubstrateNormal(vec3 N, SubstrateSample ss, float w) {
    vec3 nb = vec3(dot(N, ss.U), dot(N, ss.V), dot(N, ss.W));
    if (nb.z <= 0.0 || w <= 0.0) return N;
    vec3 wb = normalize(vec3(nb.xy + ss.n.xy, nb.z * ss.n.z));
    return normalize(mix(N, normalize(ss.U * wb.x + ss.V * wb.y + ss.W * wb.z), w));
}

// The substrate's colour with a little of the mask photo's grain left in (grime in its pores).
vec3 SubstrateColour(SubstrateSample ss, vec3 photo) {
    float l = clamp(dot(photo, vec3(0.2126, 0.7152, 0.0722)) * 2.2, 0.45, 1.1);
    return ss.albedo * mix(1.0, l, 0.55);
}

float DecalHash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
// Value noise in [0, 1] for the weathering masks, in the decal's own box space so it sticks to it.
float DecalNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(DecalHash(i), DecalHash(i + vec2(1, 0)), u.x),
               mix(DecalHash(i + vec2(0, 1)), DecalHash(i + vec2(1, 1)), u.x), u.y);
}

// `geoN` is the geometric normal (facing the viewer on a back face), N the shading normal so far.
void ApplyDecals(inout vec3 albedo, inout vec3 N, inout float roughness, inout float metallic, inout float ao,
                 inout vec3 emissive, vec3 geoN) {
    if (uDecalCount == 0 || uReceiveDecals == 0 || uClusterEnabled == 0) return;
    // The loop skips decals per fragment, so implicit derivatives are undefined inside it: every
    // decal's uv gradient comes from these, the surface's own footprint, taken out here.
    vec3 dpx = dFdx(vWorldPos), dpy = dFdy(vWorldPos);
    DecalClusterRange range = uDecalRange[clusterIndex()];
    vec4 wp = vec4(vWorldPos, 1.0);
    // The last substrate sampled: layered peels on one wall share it, so it's read once.
    uint cachedSub = 0u;
    SubstrateSample ss;
    for (uint i = 0u; i < range.count; ++i) {
        GpuDecal d = uDecals[uDecalIndex[range.offset + i]];
        // Receivers: the object's lighting layer, and static world vs moving things.
        if (uObjectLayerBit != 0 && (d.Ids.y & uint(uObjectLayerBit)) == 0u) continue;
        uint flags = d.Ids.z;
        if ((flags & (uDecalStatic == 1 ? DECAL_STATIC : DECAL_DYNAMIC)) == 0u) continue;
        vec3 l = vec3(dot(d.WorldToBox[0], wp), dot(d.WorldToBox[1], wp), dot(d.WorldToBox[2], wp));
        if (any(greaterThan(abs(l), vec3(0.5)))) continue;

        vec3 T = normalize(d.WorldToBox[0].xyz), B = normalize(d.WorldToBox[1].xyz), Z = normalize(d.WorldToBox[2].xyz);
        vec2 angle = unpackHalf2x16(d.H1.z), edge = unpackHalf2x16(d.H1.w);
        float a = smoothstep(angle.y, angle.x, dot(geoN, Z));
        if (a <= 0.0) continue;
        if (edge.x > 0.0) {
            vec3 e = clamp((vec3(0.5) - abs(l)) / edge.x, 0.0, 1.0);
            a *= e.x * e.y * e.z;
        }

        // Box uv (+Y is the image's top) into the slot, with the gradient chain-ruled through the box.
        vec2 boxUV = vec2(l.x + 0.5, 0.5 - l.y);
        vec2 gx = vec2(dot(d.WorldToBox[0].xyz, dpx), -dot(d.WorldToBox[1].xyz, dpx)) * d.Rect.zw;
        vec2 gy = vec2(dot(d.WorldToBox[0].xyz, dpy), -dot(d.WorldToBox[1].xyz, dpy)) * d.Rect.zw;
        // Mips past the slot's maxLod would read its neighbours: the gradients shrink to stay at it,
        // and the uv is held half a texel (at that mip) inside the rect.
        float maxLod = float((d.Ids.x >> 8) & 0xFFu);
        float lod = 0.5 * log2(max(max(dot(gx, gx), dot(gy, gy)) * DECAL_PAGE * DECAL_PAGE, 1e-12));
        if (lod > maxLod) {
            float s = exp2(maxLod - lod);
            gx *= s;
            gy *= s;
            lod = maxLod;
        }
        float inset = 0.5 * exp2(max(lod, 0.0)) / DECAL_PAGE;
        vec2 tc = clamp(d.Rect.xy + boxUV * d.Rect.zw, d.Rect.xy + inset, d.Rect.xy + d.Rect.zw - inset);
        vec3 tc3 = vec3(tc, float(d.Ids.x & 0xFFu));
        uint ch = d.Ids.x >> 16;

        vec4 col = textureGrad(uDecalAlbedo, tc3, gx, gy);
        float cover = (flags & DECAL_LUMA) != 0u ? dot(col.rgb, vec3(0.2126, 0.7152, 0.0722)) : col.a;
        if ((flags & DECAL_LUMA) != 0u) col.rgb = vec3(1.0); // a white-on-black alpha: the tint is the colour
        if ((flags & DECAL_INVERT) != 0u) cover = 1.0 - cover;
        a *= cover;
        if (a <= 0.001) continue;

        float aux = (flags & DECAL_HAS_AUX) != 0u ? textureGrad(uDecalAux, tc3, gx, gy).r : -1.0;
        // Weathering: fade, sun-bleach, peel (the aux mask, or noise), buff (scrubbed patches), wear (chips).
        vec2 relief = unpackHalf2x16(d.H2.y), fb = unpackHalf2x16(d.H2.z), pb = unpackHalf2x16(d.H2.w);
        vec2 seed = vec2(d.WorldToBox[0].w, d.WorldToBox[1].w) * 0.173;
        a *= 1.0 - fb.x;
        if (pb.x > 0.0) {
            float m = (aux >= 0.0 && (ch & DECAL_CH_EMISSIVE) == 0u) ? aux
                    : DecalNoise(boxUV * 6.0 + seed) * 0.65 + DecalNoise(boxUV * 19.0 + seed) * 0.35;
            a *= smoothstep(pb.x - 0.06, pb.x + 0.06, m);
        }
        if (pb.y > 0.0) a *= 1.0 - pb.y * smoothstep(0.35, 0.65, DecalNoise(boxUV * 3.0 + seed.yx));
        if (relief.y > 0.0) a *= smoothstep(relief.y * 0.7 - 0.05, relief.y * 0.7 + 0.05, DecalNoise(boxUV * 37.0 + seed));
        // Height blend: the decal gathers in the receiver's cavities (low AO) and thins on its ridges.
        float hb = float(flags >> 24) / 255.0;
        if ((ch & DECAL_CH_HEIGHT) != 0u && hb > 0.0) {
            float contrast = edge.y;
            float hbA = clamp((a - 0.5 + (1.0 - ao)) * contrast + 0.5, 0.0, 1.0) * min(a * 4.0, 1.0);
            a = mix(a, hbA, hb);
        }
        if (a <= 0.001) continue;

        vec2 h00 = unpackHalf2x16(d.H0.x), h01 = unpackHalf2x16(d.H0.y), h02 = unpackHalf2x16(d.H0.z),
             h03 = unpackHalf2x16(d.H0.w);
        // Substrate: where this decal covers, the paint is gone and the material under it shows - the
        // receiver's own on Auto when it has one, else the decal's. sw = how much it replaces the decal.
        uint sub = uSubstrateOn != 0 ? d.Ids.x >> 24 : 0u;
        if (uSubstrateOn != 0 && (flags & DECAL_SUB_AUTO) != 0u && uMaterialSubstrate > 0) sub = uint(uMaterialSubstrate);
        float sw = 0.0;
        if (sub != 0u) {
            if (sub != cachedSub) {
                ss = SampleSubstrate(sub - 1u, geoN, dpx, dpy);
                cachedSub = sub;
            }
            sw = ss.strength;
        }
        if ((ch & DECAL_CH_ALBEDO) != 0u) {
            vec3 c = col.rgb * vec3(h00, h01.x);
            if (sw > 0.0) c = mix(c, SubstrateColour(ss, col.rgb), sw);
            c = mix(c, mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), vec3(1.0), 0.35), fb.y);
            albedo = mix(albedo, c, a * h01.y);
        }
        if ((ch & DECAL_CH_NORMAL) != 0u && (flags & DECAL_HAS_NORMAL) != 0u) {
            // Whiteout blend in the box's own frame (x along u, y up the image, z out of the surface).
            vec3 tn = UnpackNormal(vec3(textureGrad(uDecalNormal, tc3, gx, gy).rg, 0.0));
            if ((ch & DECAL_CH_PAINT) != 0u) tn.xy *= 1.0 + relief.x * 2.0;
            vec3 nb = vec3(dot(N, T), dot(N, B), dot(N, Z));
            if (nb.z > 0.0) {
                vec3 wb = normalize(vec3(nb.xy + tn.xy, nb.z * tn.z));
                N = normalize(mix(N, normalize(T * wb.x + B * wb.y + Z * wb.z), a * h02.x));
            }
        }
        vec4 surf = (flags & DECAL_HAS_SURFACE) != 0u ? textureGrad(uDecalSurface, tc3, gx, gy) : vec4(1.0);
        vec2 rm = unpackHalf2x16(d.H2.x);
        if ((ch & DECAL_CH_ROUGH) != 0u) roughness = mix(roughness, rm.x * surf.r, a * h02.y);
        if ((ch & DECAL_CH_METAL) != 0u) metallic = mix(metallic, rm.y * surf.g, a * h03.x);
        if ((ch & DECAL_CH_AO) != 0u) ao = mix(ao, ao * surf.b, a * h03.y);
        if (sw > 0.0) {
            // On top of the decal's own relief (the paint's curled lips, its cracks' AO): the substrate's
            // normal, roughness and AO under its coverage. Bare material isn't metal.
            float w = a * sw;
            N = SubstrateNormal(N, ss, w);
            roughness = mix(roughness, ss.rough, w);
            metallic = mix(metallic, 0.0, w);
            ao *= mix(1.0, ss.ao, w);
        }
        if ((ch & DECAL_CH_EMISSIVE) != 0u) {
            vec2 e0 = unpackHalf2x16(d.H1.x), e1 = unpackHalf2x16(d.H1.y);
            float glow = (flags & DECAL_UV_PAINT) != 0u ? uDecalBlacklight : 1.0;
            float mask = aux >= 0.0 ? aux * a : a;
            emissive += vec3(e0, e1.x) * (e1.y * mask * glow);
        }
    }
}

// The material's Grunge map: triplanar grime over the whole surface, multiplied into the albedo
// (white leaves it as it is) and roughening where it's dark. Applied before the decals.
uniform int uHasGrungeMap;
layout(binding = 21) uniform sampler2D uGrungeMap;
uniform float uGrungeStrength;
uniform float uGrungeScale; // tiles per metre

//
// With a material Substrate the map's alpha - a peel mask: the cooked T_Grunge_* maps carry the
// exposed coverage there - reveals it: past a threshold Grunge Strength sets, the paint is gone and
// the substrate's colour, normal, roughness and AO show; the dirt only tints the paint that's left.
void ApplyGrunge(inout vec3 albedo, inout vec3 N, inout float roughness, inout float metallic, inout float ao,
                 vec3 geoN) {
    if (uHasGrungeMap == 0 || uGrungeStrength <= 0.0) return;
    vec4 g4 = SampleTriplanar(uGrungeMap, vWorldPos, TriplanarWeights(geoN), max(uGrungeScale, 1e-4));
    vec3 g = g4.rgb;
    float dirt = 1.0 - dot(g, vec3(0.2126, 0.7152, 0.0722));
    float reveal = 0.0;
    SubstrateSample ss;
    if (uSubstrateOn != 0 && uMaterialSubstrate > 0) {
        ss = SampleSubstrate(uint(uMaterialSubstrate - 1), geoN, dFdx(vWorldPos), dFdy(vWorldPos));
        // The mask a few mips down (~6 mm at the default tiling): hairline cracks stay dirt, only the
        // flaked-off patches open up. Strength 0.35 reveals where that passes ~0.65: the peels' cores.
        vec3 tw = TriplanarWeights(geoN);
        float sc = max(uGrungeScale, 1e-4);
        float m = texture(uGrungeMap, vWorldPos.zy * sc, 2.5).a * tw.x + texture(uGrungeMap, vWorldPos.xz * sc, 2.5).a * tw.y +
                  texture(uGrungeMap, vWorldPos.xy * sc, 2.5).a * tw.z;
        float t = 1.0 - uGrungeStrength;
        reveal = smoothstep(t - 0.1, t + 0.1, m) * ss.strength;
    }
    float keep = 1.0 - reveal;
    albedo *= mix(vec3(1.0), g, uGrungeStrength * keep);
    roughness = mix(roughness, min(roughness + 0.35, 1.0), dirt * uGrungeStrength * keep);
    ao *= mix(1.0, 1.0 - dirt * 0.5, uGrungeStrength * keep);
    if (reveal > 0.0) {
        // Not darkened by the mask photo here: its cores are near black, and on a white wall the bare
        // substrate already reads dark (a grey concrete is a third of white paint).
        albedo = mix(albedo, ss.albedo, reveal);
        N = SubstrateNormal(N, ss, reveal);
        roughness = mix(roughness, ss.rough, reveal);
        metallic = mix(metallic, 0.0, reveal);
        ao *= mix(1.0, ss.ao, reveal);
    }
}
