#version 460 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrc;

// 3x3 tent filter. The caller enables additive blending (GL_ONE, GL_ONE) so this accumulates
// onto the next-larger mip, which already holds its own downsampled contents.
void main() {
    vec2 texel = 1.0 / vec2(textureSize(uSrc, 0));
    vec3 result =
          texture(uSrc, vUV + texel * vec2(-1.0, -1.0)).rgb
        + texture(uSrc, vUV + texel * vec2( 0.0, -1.0)).rgb * 2.0
        + texture(uSrc, vUV + texel * vec2( 1.0, -1.0)).rgb
        + texture(uSrc, vUV + texel * vec2(-1.0,  0.0)).rgb * 2.0
        + texture(uSrc, vUV + texel * vec2( 0.0,  0.0)).rgb * 4.0
        + texture(uSrc, vUV + texel * vec2( 1.0,  0.0)).rgb * 2.0
        + texture(uSrc, vUV + texel * vec2(-1.0,  1.0)).rgb
        + texture(uSrc, vUV + texel * vec2( 0.0,  1.0)).rgb * 2.0
        + texture(uSrc, vUV + texel * vec2( 1.0,  1.0)).rgb;
    FragColor = vec4(result / 16.0, 1.0);
}
