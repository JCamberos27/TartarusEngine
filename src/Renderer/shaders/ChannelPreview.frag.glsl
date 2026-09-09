#version 460 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform int uChannel; // -1 = combined passthrough, 0..3 = isolate R/G/B/A as grayscale
void main() {
    vec4 texel = texture(uTex, vUV);
    if (uChannel < 0) { FragColor = texel; return; }
    float v = texel[uChannel];
    FragColor = vec4(v, v, v, 1.0);
}
