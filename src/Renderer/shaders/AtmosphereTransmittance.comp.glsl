#version 460 core
// Transmittance LUT (see AtmosphereCommon.glsl): optical depth from altitude r along a direction
// with zenith cosine mu, out to the top of the atmosphere. Rebuilt only when the atmosphere's
// parameters change.
#include "AtmosphereCommon.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba16f) writeonly uniform image2D uOut;

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, ivec2(kTransmittanceLutSize)))) return;
    vec2 uv = (vec2(px) + 0.5) / kTransmittanceLutSize;
    float r, mu;
    RMuFromTransmittanceUv(uv, r, mu);

    vec3 ro = vec3(0.0, r, 0.0);
    vec3 rd = vec3(0.0, mu, sqrt(max(1.0 - mu * mu, 0.0)));
    float t0, t1;
    RaySphere(ro, rd, uAtmRadii.y, t0, t1);
    float tMax = max(t1, 0.0);

    const int kSteps = 40;
    float dt = tMax / float(kSteps);
    vec3 opticalDepth = vec3(0.0);
    for (int i = 0; i < kSteps; ++i) {
        vec3 p = ro + rd * ((float(i) + 0.5) * dt);
        opticalDepth += SampleMedium(p).extinction * dt;
    }
    imageStore(uOut, px, vec4(exp(-opticalDepth), 1.0));
}
