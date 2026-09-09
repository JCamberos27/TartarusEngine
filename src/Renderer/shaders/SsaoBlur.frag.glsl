#version 460 core
in vec2 vUV;
out float FragColor;

uniform sampler2D uSsao;

// 4x4 box blur. Removes the 4x4 noise-tile pattern from the raw SSAO pass.
void main() {
    vec2 texel = 1.0 / vec2(textureSize(uSsao, 0));
    float result = 0.0;
    for (int x = -2; x <= 1; ++x)
        for (int y = -2; y <= 1; ++y)
            result += texture(uSsao, vUV + vec2(float(x), float(y)) * texel).r;
    FragColor = result / 16.0;
}
