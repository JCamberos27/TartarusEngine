#version 460 core
layout(local_size_x = 64) in;

// Bins the visible decals (DecalSystem: SSBO 12, world-space oriented boxes, sorted bottom to top)
// into the froxel grid the light cull just used (binding 2, view-space AABBs). Two dispatches of this
// one program, both from ClusterGrid::Cull right after the light cull:
//   uPass 0, one thread per decal: each box into view space once (binding 19, ClusterGrid's scratch).
//   uPass 1, one workgroup per cluster: its 64 threads split the decals between them and set the hits in a
//     shared bitmask (one bit per decal, so the list comes out in sort order with no second test),
//     then write the survivors into the cluster's own block of the index list.
// A cluster hit by more than MAX_PER_CLUSTER decals keeps the top-most ones (the last in sort order).
// DecalSystem::BinCluster is the CPU mirror the unit tests check against.

struct AABB { vec4 mn; vec4 mx; };
struct DecalBounds { vec4 Centre; vec4 AxisX; vec4 AxisY; vec4 AxisZ; }; // Decals::GpuDecalBounds
struct ClusterRange { uint offset; uint count; };
// A box in view space: its centre, the half-size of its own view-space AABB, and each axis as a
// unit direction (xyz) and half-length (w; 0 when degenerate, which the SAT skips).
struct ViewBox { vec4 Centre; vec4 Extent; vec4 Axis[3]; };

layout(std430, binding = 2) readonly buffer ClusterAABBs { AABB uClusters[]; };
layout(std430, binding = 12) readonly buffer DecalBoundsBuffer { DecalBounds uBounds[]; };
layout(std430, binding = 13) writeonly buffer DecalRanges { ClusterRange uRange[]; };
// Header cleared to {0, 0} by ClusterGrid::Cull: the entries in use, and the overflow word: bit 0 = some
// cluster went past MAX_PER_CLUSTER, bits 8+ = how many clusters were clamped to it. (Bit 1, the list
// full, can't happen any more: each cluster has its own MAX_PER_CLUSTER block.)
layout(std430, binding = 14) buffer DecalIndices { uint uCursor; uint uOverflow; uint uIndex[]; };
layout(std430, binding = 19) buffer DecalViewBoxes { ViewBox uBox[]; }; // ClusterGrid::kBindDecalViewBoxes
// ClusterGrid's: the groups finished this cull (cleared to 0 before it), and the CPU's mapped readback
// ring, 4 uints a slot: {entries, overflow, serial, -}.
layout(std430, binding = 18) coherent buffer DecalCullState { uint uDone; };       // kBindDecalCullState
layout(std430, binding = 17) coherent buffer DecalReadback { uint uReadback[]; }; // kBindDecalReadback

uniform mat4 uView;
uniform int uDecalCountI;  // int: Shader::SetInt
uniform int uPass;         // 0: decals into view space, 1: bin per cluster
uniform int uReadbackSlot; // this cull's ring slot
uniform int uSerial;       // and the tag that marks it landed

const uint CLUSTERS = 16u * 9u * 24u;
const uint MAX_VISIBLE = 4096u;    // Decals::MAX_VISIBLE: the hit bitmask's size
const uint MAX_PER_CLUSTER = 256u; // Decals::MAX_PER_CLUSTER
const uint WORDS = MAX_VISIBLE / 32u; // 128: two per thread

#define uDecalCount min(uint(max(uDecalCountI, 0)), MAX_VISIBLE)

void Prepare() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= uDecalCount) return;
    mat3 r = mat3(uView);
    vec3 ax = r * uBounds[i].AxisX.xyz, ay = r * uBounds[i].AxisY.xyz, az = r * uBounds[i].AxisZ.xyz;
    ViewBox b;
    b.Centre = vec4((uView * vec4(uBounds[i].Centre.xyz, 1.0)).xyz, 0.0);
    b.Extent = vec4(abs(ax) + abs(ay) + abs(az), 0.0);
    vec3 a[3] = vec3[3](ax, ay, az);
    for (int k = 0; k < 3; ++k) {
        float len = length(a[k]);
        b.Axis[k] = len < 1e-6 ? vec4(0.0) : vec4(a[k] / len, len);
    }
    uBox[i] = b;
}

// The view-AABB reject (which is also the SAT's three AABB axes), then the box's own three axes.
// Conservative: the nine edge-cross axes are skipped, so a near miss can still be binned.
bool Overlaps(uint i, vec3 bc, vec3 bh) {
    vec3 d = uBox[i].Centre.xyz - bc;
    if (any(greaterThan(abs(d), uBox[i].Extent.xyz + bh))) return false;
    for (int k = 0; k < 3; ++k) {
        vec4 a = uBox[i].Axis[k];
        if (a.w == 0.0) continue;
        if (abs(dot(d, a.xyz)) > a.w + dot(bh, abs(a.xyz))) return false;
    }
    return true;
}

shared uint sHits[WORDS]; // bit i: decal i touches this cluster
shared uint sScan[64];    // inclusive prefix of the hits per thread (each owns words 2t, 2t + 1)

void Bin() {
    uint c = gl_WorkGroupID.x;
    uint t = gl_LocalInvocationID.x;
    uint count = uDecalCount;
    vec3 mn = uClusters[c].mn.xyz, mx = uClusters[c].mx.xyz;
    vec3 bc = 0.5 * (mn + mx), bh = 0.5 * (mx - mn);

    sHits[2u * t] = 0u;
    sHits[2u * t + 1u] = 0u;
    barrier();
    // Each thread builds a part of one hit word in a register and ORs it in once. One bit per thread
    // (a thread per decal) had a whole warp's atomicOr land on the same word: 32-way serialised where
    // the decals pile up. A word is split over `p` threads only when there are too few words to go
    // round, so a small view still keeps the group busy.
    uint words = (count + 31u) >> 5u;
    uint p = 1u; // threads per word
    while (p < 32u && p * 2u * words <= 64u) p <<= 1u;
    uint span = 32u / p, part = t % p;
    for (uint w = t / p; w < words; w += 64u / p) {
        uint bits = 0u;
        uint first = w * 32u + part * span;
        for (uint j = 0u; j < span; ++j) {
            uint i = first + j;
            if (i < count && Overlaps(i, bc, bh)) bits |= 1u << (part * span + j);
        }
        if (bits != 0u) atomicOr(sHits[w], bits);
    }
    barrier();

    uint w0 = sHits[2u * t], w1 = sHits[2u * t + 1u];
    uint mine = uint(bitCount(w0) + bitCount(w1));
    sScan[t] = mine;
    barrier();
    for (uint off = 1u; off < 64u; off <<= 1u) {
        uint v = t >= off ? sScan[t - off] : 0u;
        barrier();
        sScan[t] += v;
        barrier();
    }
    uint n = sScan[63];
    uint rank = sScan[t] - mine; // hits in the decals before this thread's words

    // Every cluster owns a fixed MAX_PER_CLUSTER block, so its list sits at the same place every frame
    // whatever order the workgroups run in, and the list can't run out.
    uint offset = c * MAX_PER_CLUSTER;
    uint writable = min(n, MAX_PER_CLUSTER);
    if (t == 0u) {
        uRange[c].offset = offset;
        uRange[c].count = writable;
        if (writable > 0u) atomicAdd(uCursor, writable); // the entries in use, for the stats and tests
        if (writable < n) {
            atomicOr(uOverflow, 1u);
            atomicAdd(uOverflow, 256u); // one more clamped cluster
        }
        // The last group to finish hands the header to the CPU: straight into this cull's slot of
        // ClusterGrid's mapped readback ring, the serial written last to mark the slot whole. (A buffer
        // copy into client memory stalled the GPU for milliseconds in a dense view.)
        memoryBarrierBuffer();
        if (atomicAdd(uDone, 1u) == CLUSTERS - 1u) {
            memoryBarrierBuffer();
            uint slot = uint(uReadbackSlot) * 4u;
            uReadback[slot] = atomicAdd(uCursor, 0u);
            uReadback[slot + 1u] = atomicAdd(uOverflow, 0u);
            memoryBarrierBuffer();
            uReadback[slot + 2u] = uint(uSerial);
        }
    }
    // The bottom of the stack goes first: the list stays in sort order and ends with the top-most.
    uint skip = n - writable;
    for (uint k = 0u; k < 2u; ++k) {
        uint bits = k == 0u ? w0 : w1;
        uint base = (2u * t + k) * 32u;
        uint cnt = uint(bitCount(bits));
        if (rank + cnt <= skip) { rank += cnt; continue; }
        while (bits != 0u) {
            uint b = uint(findLSB(bits));
            bits &= bits - 1u;
            if (rank >= skip) uIndex[offset + rank - skip] = base + b;
            ++rank;
        }
    }
}

void main() {
    if (uPass == 0) Prepare();
    else Bin();
}
