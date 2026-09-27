#version 460 core
// Volumetric clouds, one view: marches the cloud layer for every pixel of a reduced-resolution
// target (VolumetricClouds::Quality picks the scale and step counts) with a per-frame jittered
// start, then CloudTemporal.comp accumulates the frames, which turns the jitter noise into
// smooth, band-free clouds.
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

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, uSize))) return;
    vec2 ndc = (vec2(px) + 0.5) / vec2(uSize) * 2.0 - 1.0;
    vec4 nearP = uInvViewProj * vec4(ndc, -1.0, 1.0);
    vec4 farP = uInvViewProj * vec4(ndc, 1.0, 1.0);
    vec3 rd = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);
    vec3 ro = ClampToAtmosphere(WorldToAtmosphere(uCameraWorld));

    float jitter = InterleavedGradientNoise(vec2(px), uFrame);
    vec3 ambTop, ambBottom;
    CloudAmbientColors(ambTop, ambBottom);
    CloudResult c = MarchClouds(ro, rd, uSteps, uLightSteps, jitter, true, uPixelAngle, ambTop, ambBottom);

    // Cirrus sits above the main layer (or below it, seen from an aircraft above).
    vec4 cirrus = CirrusLayer(ro, rd, uCloudLightIllum.rgb, ambTop);
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
