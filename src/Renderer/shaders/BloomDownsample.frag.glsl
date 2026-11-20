#version 460 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrc;

// 2x2 box filter of the source block this destination texel covers, in one fetch: a destination texel centre sits on
// the shared corner of its four source texels, so one bilinear tap there averages them equally (four taps at the
// texel centres cost 4x the fetches for the same value). Avoids the aliasing a point sample would introduce.
void main() {
    FragColor = vec4(texture(uSrc, vUV).rgb, 1.0);
}
