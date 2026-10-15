#version 460 core
// Glass backdrop, pass 2 of 3: walk back up the pyramid with a 3x3 tent filter (GlassBackdrop.cpp).
// Each step doubles the resolution and widens the blur, so after N levels down and N-1 up the
// result is a smooth, wide Gaussian-like blur for the cost of a handful of tiny dispatches.
// Pure blur, not bloom: the finer levels are not added back in, or the glass would re-sharpen.
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 0, rgba16f) uniform writeonly image2D uDst;

uniform float uSrcLod;
uniform vec2  uSrcTexel; // 1 / that level's size in the texture
uniform vec4  uSrcClamp; // the uv range (texel centres) of that level that holds data
uniform vec2  uSrcSize;  // the part of it that holds data, which maps onto the whole of uDst
uniform ivec2 uDstSize;
uniform float uRadius; // tent radius in source texels (1 = the classic 3x3 tent)

vec3 Tap(vec2 uv) { return textureLod(uSrc, clamp(uv, uSrcClamp.xy, uSrcClamp.zw), uSrcLod).rgb; }

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uDstSize.x || p.y >= uDstSize.y) return;
    vec2 uv = (vec2(p) + 0.5) * (uSrcSize / vec2(uDstSize)) * uSrcTexel;
    vec2 t = uSrcTexel * uRadius;

    vec3 sum = Tap(uv) * 4.0;
    sum += (Tap(uv + vec2(-t.x, 0.0)) + Tap(uv + vec2( t.x, 0.0)) +
            Tap(uv + vec2(0.0, -t.y)) + Tap(uv + vec2(0.0,  t.y))) * 2.0;
    sum += Tap(uv + vec2(-t.x, -t.y)) + Tap(uv + vec2( t.x, -t.y)) +
           Tap(uv + vec2(-t.x,  t.y)) + Tap(uv + vec2( t.x,  t.y));
    imageStore(uDst, p, vec4(sum * (1.0 / 16.0), 1.0));
}
