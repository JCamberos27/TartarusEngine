// One scene light as LightBuffer uploads it (LightBuffer::GpuLight, std430, 10 x vec4), and a froxel's {offset, count} slice of the
// clustered index list. Shared by the shaders that read the lights (LightBuffer.glsl) and the cull pass that
// writes the per-froxel lists (ClusterCull), so the layout lives in one place.
#pragma once
struct Light {
    vec4 PositionType; // xyz = world pos (point/spot); w = type: 0 directional, 1 point, 2 spot
    vec4 ColorRange;   // rgb = colour * intensity; a = range in metres (point/spot)
    vec4 DirCutoff;    // xyz = normalized aim direction (spot/directional); w = spot outer-cone cos (-1 = none)
    vec4 Params;       // x = spot inner-cone cos; y = shadow slot; z = excluded-layer bits (#203); w = volumetric intensity
    vec4 BoxX, BoxY, BoxZ; // light bounds: rows world -> box (xyz = unit axis, w = -dot(axis, centre))
    vec4 BoxHalf;          // xyz = half extents; w = fade metres (< 0: unbounded)
    vec4 BoxFadeNeg;       // xyz = fade metres at the -x/-y/-z faces
    vec4 BoxFadePos;       // xyz = fade metres at the +x/+y/+z faces
};
struct ClusterRange { uint offset; uint count; };
