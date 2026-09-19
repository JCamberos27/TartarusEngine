#version 460 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHdr;
uniform float uExposure;      // linear multiplier (already 2^EV on the CPU)
uniform int uOperator;        // 0 Reinhard, 1 ACES, 2 AgX
uniform int uAutoExposure;    // #162 - 1 = also divide by the adapted scene EV below
uniform sampler2D uAdaptedEv; // 1x1 R32F from AutoExposureAdapt

// PR16 — bloom: blurred half-res glow added to linear HDR before the tone curve.
uniform sampler2D uBloom;
uniform int   uBloomEnabled;
uniform float uBloomIntensity;

// #162 - colour grading (linear HDR, before the curve) and vignette (display space, after).
uniform vec3  uWhiteBalance;   // LMS gains from Temperature/Tint (1,1,1 = neutral)
uniform vec3  uColorFilter;    // linear multiply
uniform float uContrast;       // 1 = neutral; scales log distance from mid grey
uniform float uSaturation;     // 1 = neutral
uniform float uVignette;       // 0 = off
uniform float uVignetteSmoothness;
uniform float uAspect;         // width / height, so the vignette is round
uniform int   uDither;         // 1 = add +-0.5 LSB noise before 8-bit quantization
uniform float uChromatic;      // #162 - chromatic aberration, 0 = off
uniform float uGrain;          // #162 - film grain, 0 = off
uniform float uGrainResponse;  // 0..1: how much bright areas are spared
uniform float uGrainSeed;      // changes every frame so the grain animates

// Unity's white-balance matrices (linear sRGB <-> LMS), row-major lists: `v * mat3(list)` = M v.
const mat3 kLinToLms = mat3(3.90405e-1, 5.49941e-1, 8.92632e-3,
                            7.08416e-2, 9.63172e-1, 1.35775e-3,
                            2.31082e-2, 1.28021e-1, 9.36245e-1);
const mat3 kLmsToLin = mat3( 2.85847e+0, -1.62879e+0, -2.48910e-2,
                            -2.10182e-1,  1.15820e+0,  3.24281e-4,
                            -4.18120e-2, -1.18169e-1,  1.06867e+0);

vec3 Grade(vec3 c) {
    c = ((c * kLinToLms) * uWhiteBalance) * kLmsToLin;
    c *= uColorFilter;
    // Contrast around 18% grey in log space, so it pivots on mid-tones and never clips HDR.
    const float kMidGrey = 0.18;
    c = kMidGrey * pow(max(c, vec3(0.0)) / kMidGrey, vec3(uContrast));
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return max(luma + (c - luma) * uSaturation, vec3(0.0));
}

// Exact sRGB OETF (was pow(1/2.2), which crushes the darkest steps).
vec3 LinearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// Interleaved gradient noise (Jimenez 2014): cheap, well-distributed per-pixel dither.
float Ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

// --- ACES (Narkowicz 2015 fitted) --------------------------------------------------------
vec3 TonemapACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// --- AgX (minimal, iolite-engine.com/blog_posts/minimal_agx_implementation) --------------
vec3 AgxDefaultContrastApprox(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return  15.5 * x4 * x2
          - 40.14 * x4 * x
          + 31.96 * x4
          - 6.868 * x2 * x
          + 0.4298 * x2
          + 0.1191 * x
          - 0.00232;
}
vec3 TonemapAgX(vec3 val) {
    const mat3 agxMat = mat3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;

    val = agxMat * val;
    val = clamp(log2(val), minEv, maxEv);
    val = (val - minEv) / (maxEv - minEv);
    val = AgxDefaultContrastApprox(val);

    // Inverse (eotf) back to display-linear, then the standard AgX matrix inverse.
    const mat3 agxMatInv = mat3(
         1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433,  1.15107367264116);
    val = agxMatInv * val;
    val = max(val, vec3(0.0));
    return pow(val, vec3(2.2)); // undo AgX's built-in ~2.2 so the shared gamma below is the only one
}

void main() {
    float exposure = uExposure;
    if (uAutoExposure != 0) exposure *= exp2(-texelFetch(uAdaptedEv, ivec2(0), 0).r);
    vec3 hdr;
    if (uChromatic > 0.0) {
        // Lateral chromatic aberration: red and blue sampled radially outward/inward from the
        // green, growing toward the edges (no shift at the centre).
        vec2 shift = (vUV - 0.5) * uChromatic * 0.02;
        hdr = vec3(texture(uHdr, vUV + shift).r, texture(uHdr, vUV).g, texture(uHdr, vUV - shift).b);
    } else {
        hdr = texture(uHdr, vUV).rgb;
    }
    hdr *= exposure;
    if (uBloomEnabled != 0)
        hdr += texture(uBloom, vUV).rgb * uBloomIntensity * (exposure / uExposure);

    hdr = Grade(hdr);

    vec3 mapped;
    if (uOperator == 1)      mapped = TonemapACES(hdr);
    else if (uOperator == 2) mapped = TonemapAgX(hdr);
    else                     mapped = hdr / (hdr + vec3(1.0)); // Reinhard

    if (uVignette > 0.0) {
        vec2 d = (vUV - 0.5) * vec2(uAspect, 1.0) * 2.0 * uVignette / max(uAspect, 1.0);
        mapped *= pow(clamp(1.0 - dot(d, d), 0.0, 1.0), max(uVignetteSmoothness * 5.0, 0.01));
    }

    if (uGrain > 0.0) {
        // Film grain: zero-mean noise, strongest in the shadows (Response spares highlights).
        float luma = dot(mapped, vec3(0.2126, 0.7152, 0.0722));
        float n = Ign(gl_FragCoord.xy + uGrainSeed * vec2(47.0, 17.0)) - 0.5;
        float weight = mix(1.0, 1.0 - sqrt(clamp(luma, 0.0, 1.0)), uGrainResponse);
        mapped = max(mapped + n * uGrain * 0.35 * weight, vec3(0.0));
    }

    vec3 display = LinearToSrgb(mapped);
    // Banding fix: +-0.5 LSB triangular-ish noise so smooth gradients (sky, fog) dither
    // instead of stepping when written to the 8-bit target.
    if (uDither != 0) display += (Ign(gl_FragCoord.xy) - 0.5) / 255.0;
    FragColor = vec4(display, 1.0);
}
