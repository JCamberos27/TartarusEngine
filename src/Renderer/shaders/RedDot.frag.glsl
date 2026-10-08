#version 460 core
in vec3 vWorldPos;
in mat3 vTBN;
in float vHidden;
out vec4 FragColor;
uniform vec3 uViewPos;
uniform sampler2D uReticleMap;
uniform int uHasReticleMap;
uniform vec3 uReticleColor;
uniform float uReticleBrightness, uReticleSize;
uniform vec2 uReticleTiling, uReticleOffset;
uniform int uUseTextureColor, uBlurReticle, uBlurSamples;
uniform int uFlatProjection;
uniform mat4 uModel;
uniform vec3 uProjectionNormal, uProjectionUp;
uniform float uBlurDistance, uBlurRange;
uniform vec3 uGlassTint;
uniform float uGlassOpacity, uOpacity;
uniform int uApplyTonemap;

vec4 reticle(vec2 uv) {
    // Mesh clipping defines the lens aperture. Explicit UV clipping prevents repeated dots
    // even when the source texture uses repeat wrapping.
    if (any(lessThan(uv, vec2(0))) || any(greaterThan(uv, vec2(1)))) return vec4(0);
    if (uHasReticleMap == 0) return vec4(0);
    return texture(uReticleMap, uv);
}
void main() {
    if (vHidden > .5) discard;
    vec3 eye = normalize(transpose(vTBN) * (uViewPos - vWorldPos));
    if (uFlatProjection != 0) {
        // A curved lens has different shading normals across its aperture. Project from
        // the optical plane instead so the reticle remains collimated across that glass.
        vec3 n = normalize(mat3(uModel) * uProjectionNormal);
        vec3 r = normalize(cross(mat3(uModel) * uProjectionUp, n));
        vec3 up = cross(n, r);
        eye = normalize(transpose(mat3(r, up, n)) * (uViewPos - vWorldPos));
    }
    // Holo Sub Graph: normalize tangent View Direction, negate XY, divide by Size*.25,
    // then apply reciprocal Tiling and a centred offset. This gives collimated parallax.
    vec2 uv = vec2(.5) - eye.xy * (4.0 / max(uReticleSize, .001)) /
              max(abs(uReticleTiling), vec2(.001)) + uReticleOffset;
    vec4 sampleValue = reticle(uv);
    if (uBlurReticle != 0) {
        int count = clamp(uBlurSamples, 1, 32);
        sampleValue = vec4(0);
        for (int i = 0; i < count; ++i) {
            float angle = 6.2831853 * float(i) / float(count);
            float randomValue = fract(sin(float(i) * 735.234));
            float radius = (uBlurDistance + randomValue * uBlurRange) * .01 / max(uReticleSize, .001);
            sampleValue += reticle(uv + vec2(cos(angle), sin(angle)) * radius);
        }
        sampleValue /= float(count);
    }
    float dotAlpha = clamp(sampleValue.a, 0, 1);
    vec3 dotColor = uUseTextureColor != 0 ? sampleValue.rgb : uReticleColor;
    float glassAlpha = clamp(uGlassOpacity, 0, 1);
    float alpha = dotAlpha + glassAlpha * (1.0 - dotAlpha);
    // Premultiplied alpha keeps black/transparent reticle texels from darkening the target.
    vec3 color = dotColor * (max(uReticleBrightness, 0) * dotAlpha) + uGlassTint * glassAlpha * (1.0 - dotAlpha);
    color *= uOpacity;
    alpha *= uOpacity;
    if (uApplyTonemap != 0) color = pow(max(color, vec3(0)), vec3(1.0 / 2.2));
    FragColor = vec4(color, alpha);
}
