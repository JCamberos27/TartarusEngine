#version 460 core
layout(local_size_x = 64) in;

struct AABB  { vec4 mn; vec4 mx; };
struct Light { vec4 PositionType; vec4 ColorRange; vec4 DirCutoff; vec4 Params; };
// Per-cluster range into the global index list (#208): replaces the old bare per-cluster count
// at a fixed slot.
struct ClusterRange { uint offset; uint count; };

layout(std430, binding = 0) readonly  buffer LightBuffer   { uint uLightCount; Light uLights[]; };
layout(std430, binding = 2) readonly  buffer ClusterAABBs  { AABB uClusters[]; };
layout(std430, binding = 3) writeonly buffer ClusterCounts { ClusterRange uRange[]; };
layout(std430, binding = 4) writeonly buffer ClusterIndex  { uint uIndex[]; };
// Cleared to {0, 0} by ClusterGrid::Cull() before this dispatch (#208, adapted from #204).
// uGlobalCounter is the shared atomic append cursor into uIndex — each cluster reserves its own
// contiguous block with a single atomicAdd of its light count, so no two clusters' blocks
// overlap even though clusters run concurrently. uOverflowFlag is atomicOr'd to 1 by any
// invocation whose reserved block ran past GLOBAL_CAPACITY, i.e. some lights were dropped this
// frame. Read back (a single uint, just the flag) once per Cull() call to drive the Stats panel
// warning — not sampled per-fragment, so it costs nothing on the shading path.
layout(std430, binding = 5) buffer ClusterGlobal { uint uGlobalCounter; uint uOverflowFlag; };

uniform mat4 uView;

const uint GX = 16u, GY = 9u, GZ = 24u;
// Total capacity of the global index list; must match ClusterGrid::GLOBAL_INDEX_CAPACITY (#208).
const uint GLOBAL_CAPACITY = 65536u;

float sqDistPointAABB(vec3 p, vec3 mn, vec3 mx) {
    float d = 0.0;
    for (int i = 0; i < 3; ++i) {
        float v = p[i];
        if (v < mn[i]) { float s = mn[i] - v; d += s * s; }
        if (v > mx[i]) { float s = v - mx[i]; d += s * s; }
    }
    return d;
}

// Conservative cone-vs-sphere test (#203). A narrow, long-range spot's sphere test (radius =
// Range) still claims every cluster within Range of its position, even ones nowhere near its
// cone. This narrows that down for spots by testing the cone against the cluster AABB's
// bounding sphere -- since the AABB is contained in that sphere, "cone misses sphere" proves
// "cone misses AABB" (safe to reject), while "cone may hit sphere" just falls back to keeping
// the cluster (safe over-inclusion, never a false reject). Standard formulation (Wronski,
// "cone vs sphere culling"); valid for outer half-angles < 90 degrees, true of every spot here.
bool coneIntersectsSphere(vec3 apex, vec3 dir, float range, float cosOuter,
                           vec3 sphereCenter, float sphereRadius) {
    vec3  v        = sphereCenter - apex;
    float lenSq    = dot(v, v);
    float v1       = dot(v, dir);                                  // distance along cone axis
    float sinOuter = sqrt(max(0.0, 1.0 - cosOuter * cosOuter));
    float distClosestPoint = cosOuter * sqrt(max(0.0, lenSq - v1 * v1)) - v1 * sinOuter;

    bool angleCull = distClosestPoint > sphereRadius;
    bool frontCull = v1 > sphereRadius + range;
    bool backCull  = v1 < -sphereRadius;
    return !(angleCull || frontCull || backCull);
}

bool lightHitsCluster(uint i, vec3 mn, vec3 mx) {
    int type = int(uLights[i].PositionType.w);
    if (type == 0) return false; // directional: not clustered
    vec3  pv = (uView * vec4(uLights[i].PositionType.xyz, 1.0)).xyz;
    float r  = uLights[i].ColorRange.a;                     // range in metres
    if (sqDistPointAABB(pv, mn, mx) > r * r) return false;  // sphere test: cheap first reject

    if (type == 2) { // spot: narrow further with a cone test
        vec3  dirV = normalize(mat3(uView) * uLights[i].DirCutoff.xyz);
        vec3  sphereCenter = 0.5 * (mn + mx);
        float sphereRadius = 0.5 * length(mx - mn);
        if (!coneIntersectsSphere(pv, dirV, r, uLights[i].DirCutoff.w, sphereCenter, sphereRadius))
            return false;
    }
    return true;
}

void main() {
    uint c = gl_GlobalInvocationID.x;
    if (c >= GX * GY * GZ) return;

    vec3 mn = uClusters[c].mn.xyz;
    vec3 mx = uClusters[c].mx.xyz;

    // Pass 1: count this cluster's surviving lights without writing anything yet, so the single
    // atomicAdd below can reserve one contiguous block sized exactly for them — no separate
    // prefix-sum pass over all clusters needed.
    uint n = 0u;
    for (uint i = 0u; i < uLightCount; ++i) {
        if (lightHitsCluster(i, mn, mx)) n++;
    }

    uint offset = 0u;
    if (n > 0u) offset = atomicAdd(uGlobalCounter, n);

    // Clamp to the list's actual capacity: a cluster whose reserved block would run past the
    // end (because earlier clusters already filled it) gets a truncated, never out-of-bounds,
    // write, and flags the same overflow warning #204 introduced — now meaning "the shared list
    // is full" rather than "this one cluster hit its 100-light cap".
    uint writable = n;
    if (offset >= GLOBAL_CAPACITY) writable = 0u;
    else if (offset + n > GLOBAL_CAPACITY) writable = GLOBAL_CAPACITY - offset;
    if (writable < n) atomicOr(uOverflowFlag, 1u);

    // Pass 2: re-run the same test and write surviving indices contiguously into the block this
    // cluster reserved.
    uint w = 0u;
    for (uint i = 0u; i < uLightCount && w < writable; ++i) {
        if (lightHitsCluster(i, mn, mx)) {
            uIndex[offset + w] = i;
            w++;
        }
    }

    uRange[c].offset = offset;
    uRange[c].count = writable;
}
