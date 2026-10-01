#version 460 core
// Robust contrast-adaptive sharpening, after AMD FidelityFX FSR1 RCAS (MIT). Runs at the
// internal (low) resolution before the bicubic upscale, so the 4K output pass stays one cheap
// filter: sharpening first keeps the detail the upscale would otherwise smear.
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrc;
uniform float uSharpness; // 0 = off .. 1 = strongest

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 lim = textureSize(uSrc, 0) - 1;
    vec3 e = texelFetch(uSrc, p, 0).rgb;
    vec3 b = texelFetch(uSrc, clamp(p + ivec2(0, 1), ivec2(0), lim), 0).rgb;
    vec3 d = texelFetch(uSrc, clamp(p + ivec2(-1, 0), ivec2(0), lim), 0).rgb;
    vec3 f = texelFetch(uSrc, clamp(p + ivec2(1, 0), ivec2(0), lim), 0).rgb;
    vec3 h = texelFetch(uSrc, clamp(p + ivec2(0, -1), ivec2(0), lim), 0).rgb;

    // Max lobe weight that keeps the result inside the neighbourhood's range (no ringing).
    vec3 mn = min(min(b, d), min(f, h));
    vec3 mx = max(max(b, d), max(f, h));
    vec3 hitMin = mn / (4.0 * mx + 1e-5);
    vec3 hitMax = (1.0 - mx) / (4.0 * mn - 4.0 - 1e-5);
    vec3 lobeRgb = max(-hitMin, hitMax);
    const float kPeak = -0.1875; // 0.25 - 1/16, FSR's RCAS_LIMIT
    float lobe = max(kPeak, min(max(lobeRgb.r, max(lobeRgb.g, lobeRgb.b)), 0.0));
    lobe *= exp2(-(1.0 - clamp(uSharpness, 0.0, 1.0)) * 2.0) * (uSharpness > 0.0 ? 1.0 : 0.0);

    // Noise guard: soften the lobe on isolated single-pixel spikes (film grain, dither).
    float lb = dot(b, vec3(0.5, 1.0, 0.5)), ld = dot(d, vec3(0.5, 1.0, 0.5));
    float lf = dot(f, vec3(0.5, 1.0, 0.5)), lh = dot(h, vec3(0.5, 1.0, 0.5));
    float le = dot(e, vec3(0.5, 1.0, 0.5));
    float nz = 0.25 * (lb + ld + lf + lh) - le;
    float range = max(max(max(lb, ld), max(lf, lh)), le) - min(min(min(lb, ld), min(lf, lh)), le);
    nz = clamp(abs(nz) / max(range, 1e-5), 0.0, 1.0);
    lobe *= 1.0 - 0.5 * nz;

    vec3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
