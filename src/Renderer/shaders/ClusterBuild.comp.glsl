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
