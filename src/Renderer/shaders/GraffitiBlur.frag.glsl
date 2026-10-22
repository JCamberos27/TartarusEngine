#version 460 core
// One axis of a Gaussian over a single channel (a layer's coverage, for its outline and shadow).
// 9 taps on linear filtering; uStep scales them to the wanted width.
in vec2 vUV;
uniform sampler2D uSrc;
uniform int uChannel; // 3: alpha of an RGBA source, 0: red
uniform vec2 uStep;   // uv per tap

out vec4 oValue;

float Read(vec2 uv) {
    vec4 t = texture(uSrc, uv);
    return uChannel == 3 ? t.a : t.r;
}

void main() {
    vec2 o1 = uStep * 1.3846153846, o2 = uStep * 3.2307692308;
    float v = Read(vUV) * 0.2270270270;
    v += (Read(vUV + o1) + Read(vUV - o1)) * 0.3162162162;
    v += (Read(vUV + o2) + Read(vUV - o2)) * 0.0702702703;
    oValue = vec4(v, 0.0, 0.0, 1.0);
}
