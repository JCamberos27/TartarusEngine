// Baked bounce light (GI Volumes, GIVolumes.h): the scene's volumes in a std140 block on binding 4 and every baked
// volume x layer as a brick of a 3D atlas on unit 28 - nx x ny x 3nz texels, the R, G and B channels' cosine-convolved
// L1 SH (vec4: DC, then the world-axis linear band) one slab after another along z. A layer's weight is its circuit's
// state this frame (the Outside's is always 1). uGIEnabled == 0 (the GL default: preview renderers, no bake) skips it.
#define GI_MAX_VOLUMES 32
#define GI_MAX_LAYERS 4
struct GIVolume {
    vec4 BoxX, BoxY, BoxZ; // world -> box rows: xyz unit axis, w = -dot(axis, centre)
    vec4 HalfFade;         // xyz half extents, w fade metres
    vec4 Dims;             // xyz probe counts, w layer count
    vec4 Params;           // x intensity, y replaces the sky's ambient, z normal bias (m)
    vec4 Brick[GI_MAX_LAYERS]; // xyz brick origin in the atlas (texels), w weight
};
layout(std140, binding = 4) uniform GIBlock {
    ivec4 uGICount;   // x volumes
    vec4  uGIAtlasInv; // xyz 1 / atlas size
    GIVolume uGIVolumes[GI_MAX_VOLUMES];
};
layout(binding = 28) uniform sampler3D uGIAtlas;
uniform int uGIEnabled;
// --bake-gi's captures (GIBaker): emission scaled by uGIBakeEmissive, one-sided back faces marked (alpha 0).
uniform int uGIBake;
uniform float uGIBakeEmissive;

// The baked irradiance at p for world normal n (E / pi, like the sky's irradiance cube: x albedo), summed over the
// volumes holding p, each faded at its faces like a light's Bounds. `fade` = how much of p is inside volumes (0-1);
// `replaceSky` = how much of that is in volumes with an Outside layer (they carry the sky's light themselves).
// Overlapping volumes (a doorway) blend by their fades.
vec3 SampleGI(vec3 p, vec3 n, out float fade, out float replaceSky) {
    vec3 sum = vec3(0.0);
    float wSum = 0.0, skySum = 0.0;
    fade = 0.0;
    replaceSky = 0.0;
    if (uGIEnabled != 1) return sum;
    int count = min(uGICount.x, GI_MAX_VOLUMES);
    for (int v = 0; v < count; ++v) {
        // A little off the surface along its normal: a wall's own probes sit in or behind it.
        vec3 pb = p + n * uGIVolumes[v].Params.z;
        vec3 q = vec3(dot(uGIVolumes[v].BoxX.xyz, pb) + uGIVolumes[v].BoxX.w,
                      dot(uGIVolumes[v].BoxY.xyz, pb) + uGIVolumes[v].BoxY.w,
                      dot(uGIVolumes[v].BoxZ.xyz, pb) + uGIVolumes[v].BoxZ.w);
        vec4 hf = uGIVolumes[v].HalfFade;
        vec3 in3 = hf.xyz - abs(q);
        float m = min(in3.x, min(in3.y, in3.z));
        float f = hf.w > 0.0 ? clamp(m / hf.w, 0.0, 1.0) : (m >= 0.0 ? 1.0 : 0.0);
        if (f <= 0.0) continue;
        vec3 dims = uGIVolumes[v].Dims.xyz;
        // Probe-grid coordinate 0..dims-1 (probes sit on the faces), +0.5 to the texel centre.
        vec3 g = clamp((q + hf.xyz) / max(2.0 * hf.xyz, vec3(1e-4)), 0.0, 1.0) * (dims - 1.0) + 0.5;
        vec3 e = vec3(0.0);
        int layers = int(uGIVolumes[v].Dims.w);
        vec4 nb = vec4(1.0, n);
        for (int l = 0; l < layers && l < GI_MAX_LAYERS; ++l) {
            vec4 br = uGIVolumes[v].Brick[l];
            if (br.w <= 0.0) continue;
            vec3 t = br.xyz + g;
            vec4 cr = texture(uGIAtlas, t * uGIAtlasInv.xyz);
            vec4 cg = texture(uGIAtlas, (t + vec3(0.0, 0.0, dims.z)) * uGIAtlasInv.xyz);
            vec4 cb = texture(uGIAtlas, (t + vec3(0.0, 0.0, 2.0 * dims.z)) * uGIAtlasInv.xyz);
            e += max(vec3(dot(cr, nb), dot(cg, nb), dot(cb, nb)), vec3(0.0)) * br.w;
        }
        sum += e * uGIVolumes[v].Params.x * f;
        wSum += f;
        skySum += f * uGIVolumes[v].Params.y;
    }
    if (wSum <= 0.0) return sum;
    fade = min(wSum, 1.0);
    replaceSky = skySum / wSum;
    return wSum > 1.0 ? sum / wSum : sum;
}
