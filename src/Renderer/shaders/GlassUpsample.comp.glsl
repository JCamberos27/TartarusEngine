#version 460 core
// Glass backdrop, pass 2 of 3: walk back up the pyramid with a 3x3 tent filter (GlassBackdrop.cpp).
// Each step doubles the resolution and widens the blur, so after N levels down and N-1 up the
// result is a smooth, wide Gaussian-like blur for the cost of a handful of tiny dispatches.
// Pure blur, not bloom: the finer levels are not added back in, or the glass would re-sharpen.
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 0, rgba16f) uniform writeonly image2D uDst;

uniform float uSrcLod;
uniform vec2  uSrcTexel;
uniform ivec2 uDstSize;
uniform float uRadius; // tent radius in source texels (1 = the classic 3x3 tent)

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uDstSize.x || p.y >= uDstSize.y) return;
    vec2 uv = (vec2(p) + 0.5) / vec2(uDstSize);
    vec2 t = uSrcTexel * uRadius;

    vec3 sum = textureLod(uSrc, uv, uSrcLod).rgb * 4.0;
    sum += (textureLod(uSrc, uv + vec2(-t.x, 0.0), uSrcLod).rgb +
            textureLod(uSrc, uv + vec2( t.x, 0.0), uSrcLod).rgb +
            textureLod(uSrc, uv + vec2(0.0, -t.y), uSrcLod).rgb +
            textureLod(uSrc, uv + vec2(0.0,  t.y), uSrcLod).rgb) * 2.0;
    sum += textureLod(uSrc, uv + vec2(-t.x, -t.y), uSrcLod).rgb +
           textureLod(uSrc, uv + vec2( t.x, -t.y), uSrcLod).rgb +
           textureLod(uSrc, uv + vec2(-t.x,  t.y), uSrcLod).rgb +
           textureLod(uSrc, uv + vec2( t.x,  t.y), uSrcLod).rgb;
    imageStore(uDst, p, vec4(sum * (1.0 / 16.0), 1.0));
}
