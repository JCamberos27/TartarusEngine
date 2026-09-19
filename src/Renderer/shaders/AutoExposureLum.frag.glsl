#version 460 core
// #162 - auto exposure, pass 1: log2 luminance of the HDR scene into a small target whose mip
// chain then averages it down to a single texel (the scene's geometric-mean luminance).
in vec2 vUV;
out float FragLogLum;

uniform sampler2D uHdr;
uniform vec2 uTexel; // one destination texel (1/64): the taps below spread across it

void main() {
    // Four taps per destination texel so small bright details still register when a
    // full-resolution image is metered through a 64x64 target.
    const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
    float sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 o = (vec2(i & 1, i >> 1) - 0.5) * uTexel * 0.5;
        vec3 c = texture(uHdr, vUV + o).rgb;
        sum += log2(max(dot(max(c, vec3(0.0)), kLuma), 1e-5));
    }
    FragLogLum = sum * 0.25;
}
