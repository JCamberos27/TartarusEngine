#pragma once

// Compute shaders for clustered-forward light culling (#120). The view frustum is diced into a
// GRID_X * GRID_Y screen tiling * GRID_Z exponential depth slices = a fixed grid of froxels
// ("clusters"). Two passes run per rendered view, before the lit pass:
//
//   1. kBuildClustersCompute — one invocation per cluster, computes that cluster's view-space
//      AABB from its screen tile + depth-slice bounds (binding 2, write).
//   2. kCullLightsCompute — one invocation per cluster, tests every point/spot light's bounding
//      sphere against the cluster AABB and writes the surviving indices into a fixed-size slice
//      of the index buffer (binding 4) plus a per-cluster count (binding 3).
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

layout(std430, binding = 0) readonly  buffer LightBuffer   { uint uLightCount; Light uLights[]; };
layout(std430, binding = 2) readonly  buffer ClusterAABBs  { AABB uClusters[]; };
layout(std430, binding = 3) writeonly buffer ClusterCounts { uint uCount[]; };
layout(std430, binding = 4) writeonly buffer ClusterIndex  { uint uIndex[]; };

uniform mat4 uView;

const uint GX = 16u, GY = 9u, GZ = 24u;
const uint MAXL = 100u; // must match ClusterGrid::MAX_LIGHTS_PER_CLUSTER

float sqDistPointAABB(vec3 p, vec3 mn, vec3 mx) {
    float d = 0.0;
    for (int i = 0; i < 3; ++i) {
        float v = p[i];
        if (v < mn[i]) { float s = mn[i] - v; d += s * s; }
        if (v > mx[i]) { float s = v - mx[i]; d += s * s; }
    }
    return d;
}

void main() {
    uint c = gl_GlobalInvocationID.x;
    if (c >= GX * GY * GZ) return;

    vec3 mn = uClusters[c].mn.xyz;
    vec3 mx = uClusters[c].mx.xyz;

    uint n = 0u;
    for (uint i = 0u; i < uLightCount && n < MAXL; ++i) {
        if (int(uLights[i].PositionType.w) == 0) continue; // directional: not clustered
        vec3  pv = (uView * vec4(uLights[i].PositionType.xyz, 1.0)).xyz;
        float r  = uLights[i].ColorRange.a;                // range in metres
        if (sqDistPointAABB(pv, mn, mx) <= r * r) {
            uIndex[c * MAXL + n] = i;
            n++;
        }
    }
    uCount[c] = n;
}
)";
