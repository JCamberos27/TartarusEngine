#version 460 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrc;

// 4-tap box filter at the midpoints of the 2x2 source block this destination texel covers —
// cheaper than a full Gaussian and avoids the aliasing a single point-sample would introduce.
void main() {
    vec2 texel = 1.0 / vec2(textureSize(uSrc, 0));
    vec3 result = texture(uSrc, vUV + texel * vec2(-0.5, -0.5)).rgb
                + texture(uSrc, vUV + texel * vec2( 0.5, -0.5)).rgb
                + texture(uSrc, vUV + texel * vec2(-0.5,  0.5)).rgb
                + texture(uSrc, vUV + texel * vec2( 0.5,  0.5)).rgb;
    FragColor = vec4(result * 0.25, 1.0);
}
