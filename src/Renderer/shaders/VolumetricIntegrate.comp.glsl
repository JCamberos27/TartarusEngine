#version 460 core
// Froxel volumetric fog, pass 2 of 2 (VolumetricFog.h): one thread per froxel column, walking its
// slices front to back. Each slice's in-scatter is integrated analytically over its thickness
// (Hillaire's energy-conserving form, so a thick slice of dense fog doesn't over-brighten) and
// attenuated by everything in front of it.
// Output texel k: rgb = light scattered toward the eye between the camera and slice k's far
// boundary, a = transmittance over the same span. A surface at that depth is shaded c * a + rgb.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0, rgba16f) writeonly uniform image3D uOut;
layout(binding = 6) uniform sampler3D uScatter; // pass 1's output

uniform vec4  uProjParams;  // P[0][0], P[1][1], P[2][0], P[2][1]
uniform vec4  uSliceParams; // x = near, y = ln(far / near)
uniform float uInscatterCap; // soft cap on the accumulated in-scatter's luminance (0 = off), VolumetricFog::CapInscatter

// Identity up to cap / 2, then a roll-off that stays below cap (hue kept: the colour is scaled, not clipped).
vec3 SoftCapLuminance(vec3 c, float cap) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float k = cap * 0.5;
    if (cap <= 0.0 || l <= k) return c;
    float span = cap - k;
    float capped = k + span * (1.0 - exp(-(l - k) / span));
    return c * (capped / l);
}

void main() {
    ivec3 uGrid = imageSize(uOut); // W, H, D
    ivec2 id = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(id, uGrid.xy))) return;

    // Slices are spaced in view depth; the ray through this column is longer by its slant.
    vec2 ndc = (vec2(id) + 0.5) / vec2(uGrid.xy) * 2.0 - 1.0;
    vec3 rayVS = vec3((ndc.x + uProjParams.z) / uProjParams.x, (ndc.y + uProjParams.w) / uProjParams.y, -1.0);
    float slant = length(rayVS);

    vec3 scattered = vec3(0.0);
    float transmittance = 1.0;
    float prevDepth = 0.0;
    for (int k = 0; k < uGrid.z; ++k) {
        float depth = uSliceParams.x * exp(float(k + 1) / float(uGrid.z) * uSliceParams.y);
        float ds = (depth - prevDepth) * slant;
        prevDepth = depth;
        vec4 s = texelFetch(uScatter, ivec3(id, k), 0);
        float sigmaT = max(s.a, 1e-7);
        float sliceT = exp(-sigmaT * ds);
        scattered += transmittance * (s.rgb - s.rgb * sliceT) / sigmaT;
        transmittance *= sliceT;
        imageStore(uOut, ivec3(id, k), vec4(SoftCapLuminance(scattered, uInscatterCap), transmittance));
    }
}
