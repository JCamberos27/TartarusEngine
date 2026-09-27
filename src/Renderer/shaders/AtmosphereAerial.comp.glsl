#version 460 core
// Aerial-perspective volume (Hillaire 2020, section 5.4): a 32 x 32 x 32 camera froxel grid of
// in-scattered light (rgb) and transmittance (a) from the camera out to each slice's distance.
// The model shader looks it up per pixel and applies color * a + rgb, so distant geometry takes
// on the same haze and colour shift as the sky behind it. One thread per froxel column marches
// its 32 slices front to back. Slices are distributed quadratically (slice s ends at
// range * ((s + 1) / 32)^2) so the near field, where most of a small scene is, gets most of them.
#define ATMOSPHERE_LUTS
#include "AtmosphereCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba16f) writeonly uniform image3D uOut;

uniform mat4 uInvViewProj;
uniform vec3 uCameraWorld;

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, ivec2(int(kApSlices))))) return;
    vec2 ndc = (vec2(px) + 0.5) / kApSlices * 2.0 - 1.0;
    vec4 nearP = uInvViewProj * vec4(ndc, -1.0, 1.0);
    vec4 farP = uInvViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);

    vec3 cam = ClampToAtmosphere(WorldToAtmosphere(uCameraWorld));
    float apScale = max(uAtmParams.x, 0.0);
    float range = uAtmParams2.x; // km of world distance covered by the last slice

    vec3 sunDir = uAtmSunDir.xyz;
    vec3 moonDir = uAtmMoonDir.xyz;
    vec3 sunIllum = uAtmSunIllum.rgb;
    vec3 moonIllum = uAtmMoonIllum.rgb * uAtmMoonIllum.w * uAtmParams2.w;
    float cS = dot(dir, sunDir), cM = dot(dir, moonDir);
    float phRS = RayleighPhase(cS), phMS = MiePhase(uAtmMieExt.w, cS);
    float phRM = RayleighPhase(cM), phMM = MiePhase(uAtmMieExt.w, cM);

    vec3 L = vec3(0.0);
    vec3 throughput = vec3(1.0);
    float prevT = 0.0;
    for (int s = 0; s < int(kApSlices); ++s) {
        float f = (float(s) + 1.0) / kApSlices;
        float sliceEnd = range * f * f * apScale; // atmosphere km
        const int kSub = 2;
        float dt = (sliceEnd - prevT) / float(kSub);
        for (int k = 0; k < kSub; ++k) {
            float t = prevT + (float(k) + 0.5) * dt;
            vec3 p = ClampToAtmosphere(cam + dir * t);
            MediumSample m = SampleMedium(p);
            vec3 sampleT = exp(-m.extinction * dt);
            vec3 S = sunIllum * (TransmittanceToSpace(p, sunDir) * (m.scatRayleigh * phRS + m.scatMie * phMS)
                                 + SampleMultiScatter(p, sunDir) * m.scattering * uAtmRadii.w);
            if (moonIllum.r + moonIllum.g + moonIllum.b > 0.0)
                S += moonIllum * (TransmittanceToSpace(p, moonDir) * (m.scatRayleigh * phRM + m.scatMie * phMM)
                                  + SampleMultiScatter(p, moonDir) * m.scattering * uAtmRadii.w);
            vec3 ext = max(m.extinction, vec3(1e-7));
            L += throughput * (S - S * sampleT) / ext;
            throughput *= sampleT;
        }
        prevT = sliceEnd;
        float meanT = dot(throughput, vec3(1.0 / 3.0));
        imageStore(uOut, ivec3(px, s), vec4(L * uAtmRadii.z, meanT));
    }
}
