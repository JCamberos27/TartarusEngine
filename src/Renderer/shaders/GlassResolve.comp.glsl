#version 460 core
// Glass backdrop, pass 3 of 3: the blurred linear image back to display encoding (GlassBackdrop.cpp).
// ImGui's OpenGL backend samples and blends encoded values (no GL_FRAMEBUFFER_SRGB in the UI pass),
// so the texture it draws under a frosted plate must be encoded too. A touch of saturation keeps
// the blur from going grey, the way vibrancy materials do; 1.0 = none. uCeiling caps the encoded
// luminance (1.0 = no cap): the editor's glass windows hold their white text, so a bright sky
// behind a menu is compressed to a mid grey instead of washing the text out.
layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D uSrc;
layout(binding = 0, rgba8) uniform writeonly image2D uDst;

uniform float uSrcLod;
uniform vec2  uSrcTexel; // 1 / the source level's size in the texture (uDst's size is the part that holds data)
uniform vec4  uSrcClamp; // the uv range (texel centres) of it that holds data
uniform ivec2 uDstSize;
uniform float uSaturation;
uniform float uCeiling;

vec3 LinearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= uDstSize.x || p.y >= uDstSize.y) return;
    vec2 uv = clamp((vec2(p) + 0.5) * uSrcTexel, uSrcClamp.xy, uSrcClamp.zw);
    vec3 c = textureLod(uSrc, uv, uSrcLod).rgb;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = max(mix(vec3(luma), c, uSaturation), vec3(0.0));
    vec3 e = LinearToSrgb(c);
    float lumaE = dot(e, vec3(0.2126, 0.7152, 0.0722));
    if (lumaE > uCeiling) e *= uCeiling / lumaE;
    imageStore(uDst, p, vec4(e, 1.0));
}
