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
