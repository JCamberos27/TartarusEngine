#version 460 core
// Cloud shadow map: for each texel of a square of ground around the camera, how much of the
// main light gets through the cloud layer on its way down. The model shader projects each
// fragment back along the light onto this square (see CloudShadow() in ModelFragment.glsl) and
// dims the sun/moon by it, so moving clouds cast moving shadows across the scene.
#define ATMOSPHERE_LUTS
#define SKY_VIEW_LUTS
#include "AtmosphereCommon.glsl"
#include "CloudsCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, r16f) writeonly uniform image2D uOut;

uniform ivec2 uSize;
uniform vec2 uCenterWorld;   // world XZ at the map's centre
uniform float uExtentWorld;  // world units across the map
uniform float uStrength;     // 0 = no shadows, 1 = physically dark

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, uSize))) return;
    vec2 uv = (vec2(px) + 0.5) / vec2(uSize);
    vec2 xz = uCenterWorld + (uv - 0.5) * uExtentWorld;
    vec3 ground = WorldToAtmosphere(vec3(xz.x, uAtmParams2.y, xz.y));
    vec3 L = uCloudLightDir.xyz;

    float T = 1.0;
    float i0, i1, o0, o1;
    if (L.y > 0.01 && RaySphere(ground, L, uCloudLayer.x, i0, i1) && RaySphere(ground, L, uCloudLayer.y, o0, o1)) {
        float tStart = max(i1, 0.0), tEnd = max(o1, 0.0);
        tEnd = min(tEnd, tStart + uCloudLayer.z * 4.0);
        const int kSteps = 24;
        float dt = (tEnd - tStart) / float(kSteps);
        float od = 0.0;
        for (int i = 0; i < kSteps; ++i) {
            vec3 p = ground + L * (tStart + (float(i) + 0.5) * dt);
            od += CloudDensity(p, i % 2 == 0, ShapeTexelKm() * 2.0) * dt;
        }
        // Multiple scattering lets more light through thick cloud than Beer-Lambert alone.
        T = max(exp(-od), 0.25 * exp(-od * 0.25));
        // Cirrus: a light veil.
        float c0, c1;
        if (uCloudMisc.y > 0.001 && RaySphere(ground, L, uAtmRadii.x + uCloudMisc.z, c0, c1)) {
            vec3 p = ground + L * max(c1, 0.0);
            vec2 w = uCloudWindDir.xy;
            vec2 q = vec2(dot(p.xz, w), dot(p.xz, vec2(-w.y, w.x))) + uCloudMisc2.xy;
            vec4 wm = textureLod(uCloudWeather, q * uCloudMisc.w, 0.0);
            float d = Saturate(Remap(wm.b, 1.0 - uCloudMisc.y, 1.0, 0.0, 1.0));
            T *= mix(1.0, 0.75, d);
        }
    }
    imageStore(uOut, px, vec4(mix(1.0, T, uStrength)));
}
