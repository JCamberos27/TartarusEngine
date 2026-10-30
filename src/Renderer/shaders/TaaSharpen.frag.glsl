#version 460 core
// TAA's light sharpen (TemporalAA.cpp): a 5-tap unsharp mask on the resolved history, limited to the taps' own
// range so it can't ring, then back from the tone-mapped working space to linear HDR. Not fed back into the
// history, so it never compounds frame over frame.
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrc;
uniform float uSharpness; // 0..1

vec3 FromWork(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 mx = textureSize(uSrc, 0) - 1;
    vec3 c = texelFetch(uSrc, p, 0).rgb;
    vec3 c2 = c;
    if (uSharpness > 0.0) {
        vec3 n = texelFetch(uSrc, clamp(p + ivec2(0, 1), ivec2(0), mx), 0).rgb;
        vec3 s = texelFetch(uSrc, clamp(p - ivec2(0, 1), ivec2(0), mx), 0).rgb;
        vec3 e = texelFetch(uSrc, clamp(p + ivec2(1, 0), ivec2(0), mx), 0).rgb;
        vec3 w = texelFetch(uSrc, clamp(p - ivec2(1, 0), ivec2(0), mx), 0).rgb;
        vec3 lo = min(c, min(min(n, s), min(e, w)));
        vec3 hi = max(c, max(max(n, s), max(e, w)));
        c2 = clamp(c + (4.0 * c - (n + s + e + w)) * (0.25 * uSharpness), lo, hi);
    }
    FragColor = vec4(FromWork(c2), 1.0);
}
