#version 460 core
// The sky's ambient terms for one view, taken from its fresh sky-view LUTs once per frame (they
// are the same for every pixel, and each costs a dozen or more LUT reads):
//   texel 0  irradiance on an upward-facing surface (the planet's ground beyond the scene)
//   texel 1  zenith radiance (sky light on the tops of the clouds)
//   texel 2  radiance around the horizon (standing in for the light reaching cloud bases)
// Read with SkyAmbientTerm() in AtmosphereCommon.glsl.
#define ATMOSPHERE_LUTS
#define SKY_VIEW_LUTS
#include "AtmosphereCommon.glsl"

layout(local_size_x = 1) in;
layout(binding = 0, rgba16f) writeonly uniform image2D uOut;

void main() {
    vec3 cam = ClampToAtmosphere(uAtmCamera.xyz);
    imageStore(uOut, ivec2(0, 0), vec4(SkyIrradianceUp(cam), 1.0));
    imageStore(uOut, ivec2(1, 0), vec4(SkyRadiance(cam, normalize(cam)), 1.0));
    imageStore(uOut, ivec2(2, 0), vec4(SkyRing(cam, 0.05), 1.0));
}
