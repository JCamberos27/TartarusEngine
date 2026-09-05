#pragma once

// Compute shaders for clustered-forward light culling (#120). The view frustum is diced into a
// GRID_X * GRID_Y screen tiling * GRID_Z exponential depth slices = a fixed grid of froxels
// ("clusters"). Two passes run per rendered view, before the lit pass:
//
//   1. kBuildClustersCompute — one invocation per cluster, computes that cluster's view-space
//      AABB from its screen tile + depth-slice bounds (binding 2, write).
//   2. kCullLightsCompute — one invocation per cluster, tests every point/spot light's bounding
//      sphere against the cluster AABB twice: once to count the surviving lights and reserve a
//      right-sized, contiguous block for them in the global index list (binding 4) with a
//      single atomicAdd, then again to write the surviving indices into that block. The
//      resulting {offset, count} is recorded per cluster (binding 3) (#208).
//
// The model fragment shader then finds its own cluster from gl_FragCoord + view depth and loops
// only that cluster's light list instead of all uLightCount lights. Directional lights are not
// clustered (infinite extent) — the fragment shader still handles those in a tiny separate loop.
//
// Grid dims are compile-time constants shared with ClusterGrid.h; keep them in sync.

inline constexpr const char* kBuildClustersCompute = R"(
#version 460 core
layout(local_size_x = 64) in;

struct AABB { vec4 mn; vec4 mx; }; // .w unused, kept for std430 vec4 alignment
layout(std430, binding = 2) writeonly buffer ClusterAABBs { AABB uClusters[]; };

uniform mat4  uInvProj;
uniform vec2  uScreenSize;
uniform float uZNear;
uniform float uZFar;

const uint GX = 16u, GY = 9u, GZ = 24u;

// Pixel coordinate -> a point on the near plane in view space.
vec3 screenToView(vec2 px) {
    vec2 ndc = (px / uScreenSize) * 2.0 - 1.0;
    vec4 v = uInvProj * vec4(ndc, -1.0, 1.0);
    return v.xyz / v.w;
}

// Where the ray from the eye (origin) through p crosses the view-space plane z = zPlane.
vec3 zPlaneIntersect(vec3 p, float zPlane) {
    return p * (zPlane / p.z);
}

void main() {
    uint c = gl_GlobalInvocationID.x;
    if (c >= GX * GY * GZ) return;

    uint tx = c % GX;
    uint ty = (c / GX) % GY;
    uint tz = c / (GX * GY);

    vec2 tileSize = uScreenSize / vec2(GX, GY);
    vec3 a = screenToView(vec2(tx,      ty     ) * tileSize);
    vec3 b = screenToView(vec2(tx + 1u, ty     ) * tileSize);
    vec3 d = screenToView(vec2(tx,      ty + 1u) * tileSize);
    vec3 e = screenToView(vec2(tx + 1u, ty + 1u) * tileSize);

    // Exponential depth slices, negative view Z (view space looks down -Z).
    float nearZ = -uZNear * pow(uZFar / uZNear, float(tz)      / float(GZ));
    float farZ  = -uZNear * pow(uZFar / uZNear, float(tz + 1u) / float(GZ));

    vec3 p0 = zPlaneIntersect(a, nearZ), p1 = zPlaneIntersect(b, nearZ);
    vec3 p2 = zPlaneIntersect(d, nearZ), p3 = zPlaneIntersect(e, nearZ);
    vec3 p4 = zPlaneIntersect(a, farZ),  p5 = zPlaneIntersect(b, farZ);
    vec3 p6 = zPlaneIntersect(d, farZ),  p7 = zPlaneIntersect(e, farZ);

    vec3 mn = min(min(min(p0, p1), min(p2, p3)), min(min(p4, p5), min(p6, p7)));
    vec3 mx = max(max(max(p0, p1), max(p2, p3)), max(max(p4, p5), max(p6, p7)));

    uClusters[c].mn = vec4(mn, 0.0);
    uClusters[c].mx = vec4(mx, 0.0);
}
)";

inline constexpr const char* kCullLightsCompute = R"(
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

bool lightHitsCluster(uint i, vec3 mn, vec3 mx) {
    if (int(uLights[i].PositionType.w) == 0) return false; // directional: not clustered
    vec3  pv = (uView * vec4(uLights[i].PositionType.xyz, 1.0)).xyz;
    float r  = uLights[i].ColorRange.a;                     // range in metres
    return sqDistPointAABB(pv, mn, mx) <= r * r;
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
)";
