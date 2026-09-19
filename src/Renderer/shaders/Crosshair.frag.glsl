#version 460 core
// Play-mode crosshair, drawn over the finished (tonemapped) game image: a dot at the centre, a
// ring while the gravity gun holds something, and an arc filling clockwise from the top while a
// throw charges (white -> orange). Everything is sized in pixels scaled by uScale.
in vec2 vUV;
out vec4 FragColor;

uniform vec2  uSize;     // target size, pixels
uniform float uScale;    // pixel scale (1 at 1080p)
uniform int   uHolding;
uniform float uCharge;   // 0..1, < 0 = not charging

// Coverage of a band [r0, r1] around the centre, anti-aliased over ~1 px.
float band(float d, float r0, float r1) {
    return clamp(d - r0 + 0.5, 0.0, 1.0) * clamp(r1 - d + 0.5, 0.0, 1.0);
}

void main() {
    vec2 p = gl_FragCoord.xy - uSize * 0.5;
    float d = length(p);
    float s = uScale;
    vec4 col = vec4(0.0);

    // Centre dot with a dark rim, readable on bright sky and dark floors alike.
    float dotA = clamp(2.6 * s - d + 0.5, 0.0, 1.0);
    float rimA = clamp(3.8 * s - d + 0.5, 0.0, 1.0);
    col = mix(col, vec4(0.0, 0.0, 0.0, 0.55), rimA);
    col = mix(col, vec4(1.0, 1.0, 1.0, 0.95), dotA);

    if (uHolding == 1) {
        float ring = band(d, 13.0 * s, 15.0 * s);
        col = mix(col, vec4(1.0, 1.0, 1.0, 0.35), ring);
    }
    if (uCharge >= 0.0) {
        // Angle clockwise from straight up, 0..1.
        float a = atan(p.x, p.y) / 6.2831853;
        if (a < 0.0) a += 1.0;
        float arc = band(d, 12.0 * s, 16.0 * s) * step(a, uCharge);
        vec3 c = mix(vec3(1.0), vec3(1.0, 0.55, 0.12), uCharge);
        col = mix(col, vec4(c, 0.95), arc);
    }
    if (col.a <= 0.001) discard;
    FragColor = col;
}
