#version 460 core
// Multiple-scattering LUT (Hillaire 2020, section 5.5). For each (sun zenith, altitude) it
// integrates second-order scattering over 64 directions around the point, assuming the
// surrounding light field is isotropic, and sums the geometric series of all higher orders:
// Psi_ms = L_2nd / (1 - f_ms). Sampled per step by the sky-view / aerial-perspective passes.
// Rebuilt only when the atmosphere's parameters change.
#define ATMOSPHERE_LUTS
#include "AtmosphereCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba16f) writeonly uniform image2D uOut;

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, ivec2(kMultiScatterLutSize)))) return;
    vec2 uv = (vec2(px) + 0.5) / kMultiScatterLutSize;
    uv = vec2(FromSubUvToUnit(uv.x, kMultiScatterLutSize.x), FromSubUvToUnit(uv.y, kMultiScatterLutSize.y));

    float cosSunZenith = uv.x * 2.0 - 1.0;
    vec3 sunDir = vec3(0.0, cosSunZenith, sqrt(max(1.0 - cosSunZenith * cosSunZenith, 0.0)));
    float viewHeight = uAtmRadii.x + clamp(uv.y, 0.0, 1.0) * (uAtmRadii.y - uAtmRadii.x);
    viewHeight = clamp(viewHeight, uAtmRadii.x + 0.002, uAtmRadii.y - 0.002);
    vec3 ro = vec3(0.0, viewHeight, 0.0);

    const float kIsotropicPhase = 1.0 / (4.0 * PI);
    const int kDirs = 8; // 8 x 8 directions
    const int kSteps = 20;
    vec3 lumSum = vec3(0.0);
    vec3 fmsSum = vec3(0.0);
    for (int i = 0; i < kDirs; ++i) {
        for (int j = 0; j < kDirs; ++j) {
            float theta = 2.0 * PI * (float(i) + 0.5) / float(kDirs);
            float cosPhi = 1.0 - 2.0 * (float(j) + 0.5) / float(kDirs);
            float sinPhi = sqrt(max(1.0 - cosPhi * cosPhi, 0.0));
            vec3 rd = vec3(sinPhi * cos(theta), cosPhi, sinPhi * sin(theta));

            float tTop0, tTop1;
            RaySphere(ro, rd, uAtmRadii.y, tTop0, tTop1);
            float tEnd = max(tTop1, 0.0);
            float g0, g1;
            bool hitsGround = RaySphere(ro, rd, uAtmRadii.x, g0, g1) && g0 > 0.0;
            if (hitsGround) tEnd = min(tEnd, g0);

            float dt = tEnd / float(kSteps);
            vec3 throughput = vec3(1.0);
            vec3 L = vec3(0.0), fms = vec3(0.0);
            for (int s = 0; s < kSteps; ++s) {
                vec3 p = ro + rd * ((float(s) + 0.3) * dt);
                MediumSample m = SampleMedium(p);
                vec3 sampleT = exp(-m.extinction * dt);
                vec3 toSun = TransmittanceToSpace(p, sunDir);
                vec3 S = toSun * m.scattering * kIsotropicPhase;
                vec3 ext = max(m.extinction, vec3(1e-7));
                L += throughput * (S - S * sampleT) / ext;
                fms += throughput * (m.scattering - m.scattering * sampleT) / ext;
                throughput *= sampleT;
            }
            if (hitsGround) {
                vec3 p = ro + rd * g0;
                vec3 n = normalize(p);
                L += TransmittanceToSpace(p, sunDir) * throughput * max(dot(n, sunDir), 0.0) * uAtmGround.rgb / PI;
            }
            lumSum += L;
            fmsSum += fms;
        }
    }
    // Uniform sphere sampling: solid angle 4 pi / N per direction, times the isotropic phase.
    float w = 1.0 / float(kDirs * kDirs);
    vec3 inScattered = lumSum * w;
    vec3 multiScatAs1 = fmsSum * w;
    vec3 psi = inScattered / max(vec3(1.0) - multiScatAs1, vec3(1e-3));
    imageStore(uOut, px, vec4(psi, 1.0));
}
