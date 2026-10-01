#version 460 core
// Catmull-Rom bicubic upscale in 9 bilinear taps (the 16-tap kernel folded onto hardware
// filtering). Reads the sharpened internal-resolution image and writes the display resolution.
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSrc;     // bound with a LINEAR / CLAMP sampler
uniform vec2 uSrcSize;      // texels
uniform vec2 uDstSize;      // pixels of the viewport being written
uniform vec2 uDstOffset;    // viewport origin (letterboxing)

void main() {
    vec2 uv = (gl_FragCoord.xy - uDstOffset) / uDstSize;
    vec2 samplePos = uv * uSrcSize;
    vec2 tc1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - tc1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 off12 = w2 / w12;
    vec2 inv = 1.0 / uSrcSize;
    vec2 t0 = (tc1 - 1.0) * inv;
    vec2 t3 = (tc1 + 2.0) * inv;
    vec2 t12 = (tc1 + off12) * inv;

    vec3 c = vec3(0.0);
    c += texture(uSrc, vec2(t0.x,  t0.y)).rgb  * w0.x  * w0.y;
    c += texture(uSrc, vec2(t12.x, t0.y)).rgb  * w12.x * w0.y;
    c += texture(uSrc, vec2(t3.x,  t0.y)).rgb  * w3.x  * w0.y;
    c += texture(uSrc, vec2(t0.x,  t12.y)).rgb * w0.x  * w12.y;
    c += texture(uSrc, vec2(t12.x, t12.y)).rgb * w12.x * w12.y;
    c += texture(uSrc, vec2(t3.x,  t12.y)).rgb * w3.x  * w12.y;
    c += texture(uSrc, vec2(t0.x,  t3.y)).rgb  * w0.x  * w3.y;
    c += texture(uSrc, vec2(t12.x, t3.y)).rgb  * w12.x * w3.y;
    c += texture(uSrc, vec2(t3.x,  t3.y)).rgb  * w3.x  * w3.y;
    FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
