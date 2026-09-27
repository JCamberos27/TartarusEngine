#version 460 core
// Sky-view LUT (Hillaire 2020, section 5.3): the sky's in-scattered radiance all around the
// camera, for one light, parameterized relative to that light's azimuth. Rebuilt every frame
// per view (it depends on the camera altitude and the light's zenith angle) - 192 x 108 texels,
// so it costs next to nothing, and the full-screen sky pass is then one texture fetch per light.
#define ATMOSPHERE_LUTS
#include "AtmosphereCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba16f) writeonly uniform image2D uOut;

uniform vec3 uLightDir;   // world direction toward the light
uniform vec3 uLightIllum; // illuminance above the atmosphere

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, ivec2(kSkyViewLutSize)))) return;
    vec2 uv = (vec2(px) + 0.5) / kSkyViewLutSize;

    vec3 cam = ClampToAtmosphere(uAtmCamera.xyz);
    float viewHeight = length(cam);
    vec3 up = cam / viewHeight;

    float viewZenithCos, lightViewCos;
    UvToSkyViewParams(uv, viewHeight, viewZenithCos, lightViewCos);

    // Local frame: up = +Y, the light in the XY plane.
    float lightZenithCos = clamp(dot(up, normalize(uLightDir)), -1.0, 1.0);
    vec3 lightLocal = vec3(sqrt(max(1.0 - lightZenithCos * lightZenithCos, 0.0)), lightZenithCos, 0.0);
    float viewZenithSin = sqrt(max(1.0 - viewZenithCos * viewZenithCos, 0.0));
    vec3 dir = vec3(viewZenithSin * lightViewCos, viewZenithCos,
                    viewZenithSin * sqrt(max(1.0 - lightViewCos * lightViewCos, 0.0)));
    vec3 ro = vec3(0.0, viewHeight, 0.0);

    vec3 throughput;
    vec3 L = IntegrateScattering(ro, dir, 9.0e9, 32, lightLocal, uLightIllum, false, throughput);
    imageStore(uOut, px, vec4(L, 1.0));
}
