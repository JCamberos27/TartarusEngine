#version 460 core
// The composite into the four decal maps (docs/DECALS.md "Textures"), 8 bits each, written as PNGs:
//   0 albedo: sRGB colour, a opacity (thin paint lets the wall show through)
//   1 normal: tangent xy from the paint's height (x along u, y up the image), z
//   2 surface: r roughness multiplier, g metal, b AO, a height
//   3 aux: the glow mask, or the peel mask (thick paint holds on longer) when nothing glows
// Fully transparent texels take the colour of the nearest paint (a push-pull over the composite's
// mips), so mips and BC blocks never bleed black round the edges.
#include "GraffitiCommon.glsl"

in vec2 vUV;
uniform sampler2D uComposite0, uComposite1; // colour (mipped), (height, glow, metal, -)
uniform float uRelief;
uniform float uShowThrough;
uniform int uGlowOn;
uniform uint uSeed;

layout(location = 0) out vec4 oAlbedo;
layout(location = 1) out vec4 oNormal;
layout(location = 2) out vec4 oSurface;
layout(location = 3) out vec4 oAux;

float Height(vec2 uv) { return texture(uComposite1, uv).r; }

// Value noise on integer lattice hashes (exact), smooth-stepped.
float Noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    uvec2 q = uvec2(ivec2(i) + 65536);
    float a = GRand(GHash(q.x * 0x9E3779B1u ^ GHash(q.y ^ uSeed)));
    float b = GRand(GHash((q.x + 1u) * 0x9E3779B1u ^ GHash(q.y ^ uSeed)));
    float c = GRand(GHash(q.x * 0x9E3779B1u ^ GHash((q.y + 1u) ^ uSeed)));
    float d = GRand(GHash((q.x + 1u) * 0x9E3779B1u ^ GHash((q.y + 1u) ^ uSeed)));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    vec4 c0 = texture(uComposite0, vUV);
    vec4 c1 = texture(uComposite1, vUV);
    vec3 colour = c0.a > 1.0 / 512.0 ? c0.rgb / c0.a : vec3(0.0);
    if (c0.a <= 1.0 / 512.0) {
        for (int lod = 1; lod <= 8; ++lod) {
            vec4 m = textureLod(uComposite0, vUV, float(lod));
            if (m.a > 1e-4) { colour = m.rgb / m.a; break; }
        }
    }
    float h = c1.r;
    float cover = clamp(c0.a, 0.0, 1.0) * mix(1.0, smoothstep(0.0, 0.7, h), uShowThrough);
    oAlbedo = vec4(LinearToSrgb(colour), cover);

    // Height gradient, central differences; the FBO's +y texel is up the image.
    vec2 t = 1.0 / uCanvas;
    float dx = Height(vUV + vec2(t.x, 0.0)) - Height(vUV - vec2(t.x, 0.0));
    float dy = Height(vUV + vec2(0.0, t.y)) - Height(vUV - vec2(0.0, t.y));
    float k = uRelief * 2.5;
    vec3 n = normalize(vec3(-dx * k, -dy * k, 1.0));
    oNormal = vec4(n * 0.5 + 0.5, 1.0);

    float metal = clamp(c1.b, 0.0, 1.0);
    float gloss = smoothstep(0.1, 1.5, h); // thick, wet-laid paint dries glossier than dusting
    float rough = mix(1.0, 0.7, gloss) * mix(1.0, 0.35, metal);
    oSurface = vec4(rough, metal, 1.0, clamp(h * 0.5, 0.0, 1.0));

    vec2 px = vec2(vUV.x, 1.0 - vUV.y) * uCanvas;
    float peel = Noise(px / 96.0) * 0.45 + Noise(px / 23.0) * 0.25 + clamp(h, 0.0, 1.0) * 0.3;
    oAux = vec4(uGlowOn != 0 ? clamp(c1.g, 0.0, 1.0) : peel, 0.0, 0.0, 1.0);
}
