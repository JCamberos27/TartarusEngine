#version 460 core
// Glass backdrop, pass 1 of 3: one level of the downsample pyramid (GlassBackdrop.cpp).
// The 13-tap filter from Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare" (SIGGRAPH 2014): four overlapping 2x2 boxes plus a centre box, weighted so a single
// bright texel can't strobe as the camera moves (no aliasing "fireflies" in the glass).
//
// uDecodeSrgb: the first pass reads the tonemapped scene, an 8-bit texture holding display-encoded
// (sRGB) values. Blurring encoded values darkens and muddies every edge between light and dark,
// so the first pass decodes to linear light and the whole pyramid stays linear (RGBA16F).
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 0, rgba16f) uniform writeonly image2D uDst;

uniform float uSrcLod;      // level of uSrc to read
uniform vec2  uSrcTexel;    // 1 / size of that level
uniform ivec2 uDstSize;
uniform int   uDecodeSrgb;

vec3 SrgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 Tap(vec2 uv) {
    vec3 c = textureLod(uSrc, uv, uSrcLod).rgb;
    return uDecodeSrgb != 0 ? SrgbToLinear(c) : c;
}

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uDstSize.x || p.y >= uDstSize.y) return;
    vec2 uv = (vec2(p) + 0.5) / vec2(uDstSize);
    vec2 t = uSrcTexel;

    vec3 a = Tap(uv + t * vec2(-2.0, -2.0));
    vec3 b = Tap(uv + t * vec2( 0.0, -2.0));
    vec3 c = Tap(uv + t * vec2( 2.0, -2.0));
    vec3 d = Tap(uv + t * vec2(-2.0,  0.0));
    vec3 e = Tap(uv);
    vec3 f = Tap(uv + t * vec2( 2.0,  0.0));
    vec3 g = Tap(uv + t * vec2(-2.0,  2.0));
    vec3 h = Tap(uv + t * vec2( 0.0,  2.0));
    vec3 i = Tap(uv + t * vec2( 2.0,  2.0));
    vec3 j = Tap(uv + t * vec2(-1.0, -1.0));
    vec3 k = Tap(uv + t * vec2( 1.0, -1.0));
    vec3 l = Tap(uv + t * vec2(-1.0,  1.0));
    vec3 m = Tap(uv + t * vec2( 1.0,  1.0));

    vec3 sum = e * 0.125;
    sum += (a + c + g + i) * 0.03125;
    sum += (b + d + f + h) * 0.0625;
    sum += (j + k + l + m) * 0.125;
    imageStore(uDst, p, vec4(sum, 1.0));
}
