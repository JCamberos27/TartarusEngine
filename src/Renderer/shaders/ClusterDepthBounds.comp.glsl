#version 460 core
// Depth-bounded froxels: one workgroup per screen tile reads every depth sample the view laid down so far (the
// pre-pass), finds each depth slice's nearest and farthest sample, and writes the froxel's box shrunk to that range
// (the tile's frustum between them) plus the range itself. A slice no sample falls in gets an empty range and a box
// nothing reaches. The shading side uses the shrunk list only for a fragment whose own view z lies inside its
// froxel's range, so the point it shades is inside the shrunk box: any other fragment keeps the full list.
layout(local_size_x = 16, local_size_y = 16) in;

struct AABB { vec4 mn; vec4 mx; };
layout(std430, binding = 2) writeonly buffer ClusterAABBs { AABB uClusters[]; };
layout(std430, binding = 41) writeonly buffer ClusterTightZ { vec2 uTightZ[]; }; // view z min, max (> : empty)

uniform mat4 uInvProj;
uniform vec2 uScreenSize;
uniform vec4 uZParams; // ClusterGrid::ZParams: near, far, GZ / ln(far/near), -GZ ln(near) / ln(far/near)
uniform int uSamples;  // 1: uDepth, else uDepthMS with that many samples
uniform sampler2D uDepth;
uniform sampler2DMS uDepthMS;

#include "ClusterGrid.glsl"
const uint GX = C_GX, GY = C_GY, GZ = C_GZ;

shared uint sMin[GZ], sMax[GZ]; // float bits of positive view z: ordered like the floats

vec3 screenToView(vec2 px) {
    vec4 v = uInvProj * vec4((px / uScreenSize) * 2.0 - 1.0, -1.0, 1.0);
    return v.xyz / v.w;
}

void addSample(vec2 px, float d) {
    if (d >= 1.0) return; // cleared: nothing drawn here
    vec4 v = uInvProj * vec4((px / uScreenSize) * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    float z = clamp(-v.z / v.w, uZParams.x, uZParams.y); // as clusterIndex() clamps it
    uint slice = min(uint(max(log(z) * uZParams.z + uZParams.w, 0.0)), GZ - 1u);
    atomicMin(sMin[slice], floatBitsToUint(z));
    atomicMax(sMax[slice], floatBitsToUint(z));
}

void main() {
    uint lid = gl_LocalInvocationIndex;
    uvec2 tile = gl_WorkGroupID.xy;
    if (lid < GZ) { sMin[lid] = 0x7F7FFFFFu; sMax[lid] = 0u; }
    barrier();
    // The pixels clusterIndex() puts in this tile: floor(centre / tileSize) == tile (the last row / column clamps).
    vec2 ts = uScreenSize / vec2(GX, GY);
    ivec2 lo = max(ivec2(floor(vec2(tile) * ts)) - 1, ivec2(0));
    ivec2 hi = min(ivec2(ceil(vec2(tile + 1u) * ts)) + 1, ivec2(uScreenSize));
    if (tile.x == GX - 1u) hi.x = int(uScreenSize.x);
    if (tile.y == GY - 1u) hi.y = int(uScreenSize.y);
    for (int y = lo.y + int(gl_LocalInvocationID.y); y < hi.y; y += 16) {
        for (int x = lo.x + int(gl_LocalInvocationID.x); x < hi.x; x += 16) {
            vec2 centre = vec2(x, y) + 0.5;
            if (any(notEqual(min(uvec2(centre / ts), uvec2(GX - 1u, GY - 1u)), tile))) continue;
            if (uSamples <= 1) {
                addSample(centre, texelFetch(uDepth, ivec2(x, y), 0).r);
            } else {
                for (int s = 0; s < uSamples; ++s) addSample(centre, texelFetch(uDepthMS, ivec2(x, y), s).r);
            }
        }
    }
    barrier();
    if (lid >= GZ) return;
    uint c = tile.x + tile.y * GX + lid * GX * GY;
    if (sMax[lid] == 0u) {
        uTightZ[c] = vec2(1.0, 0.0);
        // Behind the eye and out of every light's reach: the cull gives it nothing.
        uClusters[c].mn = vec4(0.0, 0.0, 1e30, 0.0);
        uClusters[c].mx = vec4(0.0, 0.0, 1e30, 0.0);
        return;
    }
    float zn = uintBitsToFloat(sMin[lid]), zf = uintBitsToFloat(sMax[lid]);
    uTightZ[c] = vec2(zn, zf);
    vec3 a = screenToView(vec2(tile) * ts), b = screenToView(vec2(tile.x + 1u, tile.y) * ts);
    vec3 d = screenToView(vec2(tile.x, tile.y + 1u) * ts), e = screenToView(vec2(tile + 1u) * ts);
    // The box a hair wider than the range, so a fragment at its very edge is inside it, rounding and all.
    float n = -zn * (1.0 - 1e-4), f = -zf * (1.0 + 1e-4);
    vec3 p0 = a * (n / a.z), p1 = b * (n / b.z), p2 = d * (n / d.z), p3 = e * (n / e.z);
    vec3 p4 = a * (f / a.z), p5 = b * (f / b.z), p6 = d * (f / d.z), p7 = e * (f / e.z);
    vec3 mn = min(min(min(p0, p1), min(p2, p3)), min(min(p4, p5), min(p6, p7)));
    vec3 mx = max(max(max(p0, p1), max(p2, p3)), max(max(p4, p5), max(p6, p7)));
    vec3 pad = vec3(1e-4 * zf);
    uClusters[c].mn = vec4(mn - pad, 0.0);
    uClusters[c].mx = vec4(mx + pad, 0.0);
}
