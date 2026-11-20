// The scene's lights for shading: every light (directional sun, point, spot) in one std430 SSBO filled by
// LightBuffer on the CPU (binding 0, directional lights packed at the front, #188), and the clustered-forward
// lists ClusterGrid builds each frame (bindings 3/4, #120/#208). Read by the model shader (ModelShading.glsl)
// and the volumetric fog's light injection.
#pragma once
#include "LightTypes.glsl"
layout(std430, binding = 0) readonly buffer LightBuffer {
    uint uLightCount;
    uint uDirectionalCount;
    Light uLights[];
};
layout(std430, binding = 3) readonly buffer ClusterCounts { ClusterRange uClusterRange[]; };
layout(std430, binding = 4) readonly buffer ClusterIndex  { uint uClusterLightIndices[]; };

// The falloff every point/spot light shares, wherever it is shaded (surfaces, fog).

// The light's bounds (Light component > Bounds) at world point p: 1 well inside the box, falling to 0 at its faces.
float LightBoxFade(uint i, vec3 p) {
    vec4 h = uLights[i].BoxHalf;
    if (h.w < 0.0) return 1.0;
    vec3 q = vec3(dot(uLights[i].BoxX.xyz, p) + uLights[i].BoxX.w, dot(uLights[i].BoxY.xyz, p) + uLights[i].BoxY.w,
                  dot(uLights[i].BoxZ.xyz, p) + uLights[i].BoxZ.w);
    vec3 in3 = h.xyz - abs(q);
    return clamp(min(in3.x, min(in3.y, in3.z)) / h.w, 0.0, 1.0);
}
// Windowed inverse-square: the contribution reaches exactly zero at range, no hard clip.
float LightDistanceAtten(float dist, float range) {
    float t = dist / max(range, 1e-4);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    return window * window / (1.0 + dist * dist);
}
// A spot light's cone toward L (unit, surface -> light): 0 outside the outer cone, 1 inside the inner one.
float SpotConeAtten(uint i, vec3 L) {
    float cosAngle = dot(-uLights[i].DirCutoff.xyz, L); // aim normalised on upload (LightBuffer)
    float outerCos = uLights[i].DirCutoff.w;
    if (cosAngle < outerCos) return 0.0;
    return smoothstep(outerCos, max(uLights[i].Params.x, outerCos + 1e-3), cosAngle);
}
