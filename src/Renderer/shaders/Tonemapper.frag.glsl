#version 460 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHdr;
uniform float uExposure;   // linear multiplier (already 2^EV on the CPU)
uniform int uOperator;     // 0 Reinhard, 1 ACES, 2 AgX

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
    vec3 hdr = texture(uHdr, vUV).rgb * uExposure;

    vec3 mapped;
    if (uOperator == 1)      mapped = TonemapACES(hdr);
    else if (uOperator == 2) mapped = TonemapAgX(hdr);
    else                     mapped = hdr / (hdr + vec3(1.0)); // Reinhard

    FragColor = vec4(pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
