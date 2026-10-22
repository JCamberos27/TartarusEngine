#version 460 core
layout(local_size_x = 64) in;

// Bins the visible decals (DecalSystem: SSBO 12, world-space oriented boxes, sorted bottom to top)
// into the froxel grid the light cull just used (binding 2, view-space AABBs). Same scheme as
// ClusterCull.comp.glsl: count, reserve one contiguous block with a single atomicAdd, write. A
// cluster hit by more than MAX_PER_CLUSTER decals keeps the top-most ones (the last in sort order).
// Dispatched by ClusterGrid::Cull right after the light cull; DecalSystem::BinCluster is the CPU
// mirror the unit tests check against.

struct AABB { vec4 mn; vec4 mx; };
struct DecalBounds { vec4 Centre; vec4 AxisX; vec4 AxisY; vec4 AxisZ; }; // Decals::GpuDecalBounds
struct ClusterRange { uint offset; uint count; };

layout(std430, binding = 2) readonly buffer ClusterAABBs { AABB uClusters[]; };
layout(std430, binding = 12) readonly buffer DecalBoundsBuffer { DecalBounds uBounds[]; };
layout(std430, binding = 13) writeonly buffer DecalRanges { ClusterRange uRange[]; };
// Header cleared to {0, 0} by ClusterGrid::Cull: the append cursor, and overflow bits (1 = some
// cluster went past MAX_PER_CLUSTER, 2 = the shared list is full).
layout(std430, binding = 14) buffer DecalIndices { uint uCursor; uint uOverflow; uint uIndex[]; };

uniform mat4 uView;
uniform int uDecalCountI; // int: Shader::SetInt
#define uDecalCount uint(max(uDecalCountI, 0))

const uint CLUSTERS = 16u * 9u * 24u;
const uint MAX_PER_CLUSTER = 256u; // Decals::MAX_PER_CLUSTER
const uint INDEX_CAPACITY = 131072u; // Decals::INDEX_CAPACITY

// One tile of decals moved into view space once per workgroup instead of once per cluster.
shared vec3 sCentre[64];
shared vec3 sAxis[64][3];
shared vec3 sExtent[64]; // the box's own view-space AABB half-size

void LoadTile(uint base) {
    uint i = base + gl_LocalInvocationID.x;
    uint t = gl_LocalInvocationID.x;
    if (i < uDecalCount) {
        mat3 r = mat3(uView);
        vec3 ax = r * uBounds[i].AxisX.xyz, ay = r * uBounds[i].AxisY.xyz, az = r * uBounds[i].AxisZ.xyz;
        sCentre[t] = (uView * vec4(uBounds[i].Centre.xyz, 1.0)).xyz;
        sAxis[t][0] = ax;
        sAxis[t][1] = ay;
        sAxis[t][2] = az;
        sExtent[t] = abs(ax) + abs(ay) + abs(az);
    }
}

// The view-AABB reject (which is also the SAT's three AABB axes), then the box's own three axes.
// Conservative: the nine edge-cross axes are skipped, so a near miss can still be binned.
bool Overlaps(uint t, vec3 bc, vec3 bh) {
    vec3 d = sCentre[t] - bc;
    if (any(greaterThan(abs(d), sExtent[t] + bh))) return false;
    for (int k = 0; k < 3; ++k) {
        vec3 a = sAxis[t][k];
        float len = length(a);
        if (len < 1e-6) continue;
        vec3 n = a / len;
        if (abs(dot(d, n)) > len + dot(bh, abs(n))) return false;
    }
    return true;
}

void main() {
    uint c = gl_GlobalInvocationID.x;
    bool live = c < CLUSTERS;
    vec3 bc = vec3(0.0), bh = vec3(-1.0);
    if (live) {
        vec3 mn = uClusters[c].mn.xyz, mx = uClusters[c].mx.xyz;
        bc = 0.5 * (mn + mx);
        bh = 0.5 * (mx - mn);
    }

    // Pass 1: count. Every invocation walks every tile (barrier() needs uniform control flow).
    uint n = 0u;
    for (uint base = 0u; base < uDecalCount; base += 64u) {
        barrier();
        LoadTile(base);
        barrier();
        uint tileCount = min(64u, uDecalCount - base);
        if (live)
            for (uint t = 0u; t < tileCount; ++t)
                if (Overlaps(t, bc, bh)) n++;
    }

    uint keep = min(n, MAX_PER_CLUSTER);
    uint offset = 0u;
    if (live && keep > 0u) offset = atomicAdd(uCursor, keep);
    uint writable = keep;
    if (offset >= INDEX_CAPACITY) writable = 0u;
    else if (offset + keep > INDEX_CAPACITY) writable = INDEX_CAPACITY - offset;
    if (live && keep < n) atomicOr(uOverflow, 1u);
    if (live && writable < keep) atomicOr(uOverflow, 2u);
    // The bottom of the stack goes first: the list stays in sort order and ends with the top-most.
    uint skip = n - writable;

    // Pass 2: the same test again, writing the survivors past the skipped ones.
    uint seen = 0u, w = 0u;
    for (uint base = 0u; base < uDecalCount; base += 64u) {
        barrier();
        LoadTile(base);
        barrier();
        uint tileCount = min(64u, uDecalCount - base);
        if (live) {
            for (uint t = 0u; t < tileCount; ++t) {
                if (!Overlaps(t, bc, bh)) continue;
                if (seen++ >= skip && w < writable) uIndex[offset + w++] = base + t;
            }
        }
    }

    if (live) {
        uRange[c].offset = offset;
        uRange[c].count = writable;
    }
}
