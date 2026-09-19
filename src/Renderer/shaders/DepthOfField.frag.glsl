#version 460 core
// #162 - depth of field (Unity's Gaussian/Bokeh DOF, as one gather pass). Each pixel averages a
// golden-angle disk of neighbours; a neighbour contributes only if its own circle of confusion
// reaches this pixel ("scatter as gather"), so an in-focus subject keeps a crisp silhouette
// while a blurred foreground still bleeds over it. Background samples can't blur more than the
// pixel they land on, which stops a blurry background smearing across a sharp subject.
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHdr;
uniform sampler2D uDepth;     // resolved depth, [0,1]
uniform float uProjA;         // proj[2][2]
uniform float uProjB;         // proj[3][2]
uniform int   uOrtho;
uniform float uFocus;         // metres from the camera that are perfectly sharp
uniform float uRange;         // width of the in-focus band; blur reaches max one Range past it
uniform float uMaxRadius;     // pixels
uniform vec2  uTexel;

float LinearDepth(vec2 uv) {
    float z = texture(uDepth, uv).r * 2.0 - 1.0;
    return uOrtho != 0 ? (uProjB - z) / uProjA : uProjB / (z + uProjA);
}

// Blur radius in pixels for a surface at distance d.
float CocRadius(float d) {
    return clamp((abs(d - uFocus) - 0.5 * uRange) / max(uRange, 0.01), 0.0, 1.0) * uMaxRadius;
}

const int kSamples = 48;
const float kGoldenAngle = 2.39996323;

void main() {
    float d0 = LinearDepth(vUV);
    float coc0 = CocRadius(d0);
    vec3 sum = texture(uHdr, vUV).rgb;
    float wsum = 1.0;
    for (int i = 0; i < kSamples; ++i) {
        float r = sqrt((float(i) + 0.5) / float(kSamples)) * uMaxRadius;
        float a = float(i) * kGoldenAngle;
        vec2 uv = vUV + vec2(cos(a), sin(a)) * r * uTexel;
        float ds = LinearDepth(uv);
        float cocS = CocRadius(ds);
        if (ds > d0) cocS = min(cocS, coc0);
        float w = clamp(cocS - r + 1.0, 0.0, 1.0);
        sum += texture(uHdr, uv).rgb * w;
        wsum += w;
    }
    FragColor = vec4(sum / wsum, 1.0);
}
