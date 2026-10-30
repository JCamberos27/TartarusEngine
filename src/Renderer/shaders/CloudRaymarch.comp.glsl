#version 460 core
// Volumetric clouds, one view: marches the cloud layer for every pixel of a reduced-resolution
// target (VolumetricClouds::Quality picks the scale and step counts) with a per-frame jittered
// start, then CloudTemporal.comp accumulates the frames, which turns the jitter noise into
// smooth, band-free clouds. Checkerboarded: a frame with history marches one texel of every
// 2x2 block (uChecker), the temporal pass reprojects the other three.
#define ATMOSPHERE_LUTS
#define SKY_VIEW_LUTS
#include "AtmosphereCommon.glsl"
#include "CloudsCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba16f) writeonly uniform image2D uOutColor; // rgb radiance, a transmittance
layout(binding = 1, r32f) writeonly uniform image2D uOutDepth;    // km

uniform mat4 uInvViewProj;
uniform vec3 uCameraWorld;
uniform ivec2 uSize;
uniform float uFrame;
uniform int uSteps;
uniform int uLightSteps;
uniform float uPixelAngle; // radians per pixel of this (reduced-resolution) target
uniform ivec2 uChecker;    // this frame's texel of each 2x2 block; (-1, -1) = every texel
uniform float uMarchIndex; // with uChecker: how many times each texel has been marched

// This view's depth (the depth pre-pass, SSAO's copy of it) at full resolution, uDivisor scene pixels per cloud texel.
layout(binding = 12) uniform sampler2D uSceneDepth;
uniform int uSceneDepthOn;
uniform ivec2 uDepthSize;
uniform int uDivisor;
// True when every scene pixel within `margin` cloud texels of `t` has geometry in front of the sky. The temporal pass
// leaves out what is hidden within 1 (nothing that reads such a texel - the sky's bilinear fetch - can show it); the
// raymarch leaves out what is hidden within 2, so the marched texel every resolved one falls back on is always there.
bool CloudTexelHidden(ivec2 t, int margin) {
    if (uSceneDepthOn == 0) return false;
    ivec2 p0 = max((t - margin) * uDivisor, ivec2(0));
    ivec2 p1 = min((t + margin + 1) * uDivisor, uDepthSize);
    vec2 inv = 1.0 / vec2(uDepthSize);
    for (int y = p0.y; y < p1.y; y += 2)
        for (int x = p0.x; x < p1.x; x += 2) {
            vec4 d = textureGather(uSceneDepth, (vec2(x, y) + 1.0) * inv); // pixels x..x+1, y..y+1
            if (max(max(d.x, d.y), max(d.z, d.w)) >= 1.0) return false;    // the far plane: sky shows here
        }
    return true;
}

uint HashTexel(uvec2 p) { // PCG-style integer hash
    uint v = p.x * 1664525u + p.y * 22695477u + 1013904223u;
    v = v * 747796405u + 2891336453u;
    v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (v >> 22u) ^ v;
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (uChecker.x >= 0) px = px * 2 + uChecker;
    if (any(greaterThanEqual(px, uSize))) return;
    if (CloudTexelHidden(px, 2)) { // not marched: alpha 2 (no transmittance is) tells the temporal pass
        imageStore(uOutColor, px, vec4(0.0, 0.0, 0.0, 2.0));
        imageStore(uOutDepth, px, vec4(0.0));
        return;
    }
    vec2 ndc = (vec2(px) + 0.5) / vec2(uSize) * 2.0 - 1.0;
    vec4 nearP = uInvViewProj * vec4(ndc, -1.0, 1.0);
    vec4 farP = uInvViewProj * vec4(ndc, 1.0, 1.0);
    vec3 rd = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);
    vec3 ro = ClampToAtmosphere(WorldToAtmosphere(uCameraWorld));

    // Checkerboarded, a texel is only marched every 4th frame, so gradient noise (a pattern built
    // to average out over consecutive frames of neighbouring pixels) leaves a crawling 2x2 dither.
    // Instead each texel walks its own golden-ratio sequence from a random start: its successive
    // marches spread evenly over the step, and neighbours stay uncorrelated.
    float jitter = uChecker.x >= 0
        ? fract(float(HashTexel(uvec2(px))) * (1.0 / 4294967296.0) + uMarchIndex * 0.61803399)
        : InterleavedGradientNoise(vec2(px), uFrame);
    vec3 ambTop, ambBottom;
    CloudAmbientColors(ambTop, ambBottom);
    CloudResult c = MarchClouds(ro, rd, uSteps, uLightSteps, jitter, true, uPixelAngle, ambTop, ambBottom);

    // Cirrus sits above the main layer (or below it, seen from an aircraft above).
    vec4 cirrus = CirrusLayer(ro, rd, uCloudLightIllum.rgb, ambTop, uPixelAngle);
    vec3 radiance;
    float T;
    bool cirrusBehind = length(ro) < uAtmRadii.x + uCloudMisc.z;
    if (cirrusBehind) {
        radiance = c.radiance + c.transmittance * cirrus.rgb;
    } else {
        radiance = cirrus.rgb + cirrus.a * c.radiance;
    }
    T = c.transmittance * cirrus.a;

    // Aerial perspective on the clouds themselves: distant ones fade into the sky's own colour,
    // which is what sells their distance near the horizon.
    if (T < 0.999) {
        vec3 sky = SkyRadiance(ro, rd);
        // Under a closed deck there's no clear sky at the horizon to fade into.
        float haze = (1.0 - exp(-c.depth * uCloudWindDir.w)) * (1.0 - 0.75 * uCloudLayer.w * uCloudLayer.w);
        radiance = mix(radiance, sky * (1.0 - T), haze);
    }
    // A non-finite sample would spread through the temporal history as a growing block.
    radiance = any(isnan(radiance)) || any(isinf(radiance)) ? vec3(0.0) : min(radiance * uAtmRadii.z, vec3(6.0e4));
    imageStore(uOutColor, px, vec4(radiance, isnan(T) ? 1.0 : T));
    imageStore(uOutDepth, px, vec4(c.depth));
}
