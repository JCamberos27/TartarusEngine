#version 460 core
// Grass blades (GrassRenderer): no vertex buffer - instance i is blade uInstanceBase + i of the culled list
// (Grass.comp.glsl), vertex v a point on its strip: pairs up the blade (left, right) for uSegments segments, then
// the tip. The blade is a quadratic curve from the root through a control point over it to the tip, leaning its
// own way (Bend) and over with the wind - rolling gusts across the field plus a flutter of its own - keeping its
// length, tapering to a point. The depth pre-pass and the lit pass run this same program, so both put a fragment
// at exactly the same depth.
struct GrassBlade { vec4 A; vec4 B; vec4 C; }; // root, yaw | height, width, bend, wind strength | rand, dry, area, wind speed
layout(std430, binding = 32) readonly buffer GrassBlades { GrassBlade uBlades[]; };

uniform mat4  uView;
uniform mat4  uProj;
uniform int   uInstanceBase;
uniform int   uSegments;
uniform float uWindTime;
uniform float uPrevWindTime;
uniform vec2  uWindDir;
// TAA motion vectors, as ModelVertex.glsl
uniform int  uTaaVelocity;
uniform mat4 uPrevViewProj;
uniform vec2 uTaaJitter;

out vec3 vWorldPos;
out vec3 vNormal;
out float vT;              // 0 at the root, 1 at the tip
flat out vec4 vBlade;      // rand, dry, area, -
out vec4 vCurClip;
out vec4 vPrevClip;
invariant gl_Position;

vec3 BladePoint(GrassBlade b, float t, float side, float time, out vec3 tangent, out vec3 right) {
    vec3 root = b.A.xyz;
    float h = b.B.x, w = b.B.y, bend = b.B.z, windStrength = b.B.w;
    float speed = b.C.w, rnd = b.C.x;
    vec2 face = vec2(cos(b.A.w), sin(b.A.w));
    vec2 wind = dot(uWindDir, uWindDir) > 1e-6 ? normalize(uWindDir) : vec2(1.0, 0.0);
    // Gusts: long waves rolling downwind, beating against a slower one so they don't march in step.
    float phase = dot(root.xz, wind) * 0.22 - time * speed * 1.4;
    float gust = 0.5 + 0.5 * sin(phase) * (0.6 + 0.4 * sin(phase * 0.31 + 1.7));
    float flutter = 0.12 * sin(time * speed * 7.3 + rnd * 41.0);
    vec2 lean = face * bend * 0.75 + wind * windStrength * (gust + flutter);
    float len = length(lean);
    if (len > 0.92) lean *= 0.92 / len;
    vec3 tip = root + vec3(lean.x * h, h * sqrt(max(1.0 - dot(lean, lean), 0.15)), lean.y * h);
    vec3 ctrl = root + vec3(0.0, 0.55 * h, 0.0) + vec3(lean.x, 0.0, lean.y) * (0.12 * h);
    float u = 1.0 - t;
    vec3 p = u * u * root + 2.0 * u * t * ctrl + t * t * tip;
    vec3 d = 2.0 * u * (ctrl - root) + 2.0 * t * (tip - ctrl);
    tangent = dot(d, d) > 1e-12 ? normalize(d) : vec3(0.0, 1.0, 0.0);
    right = vec3(-face.y, 0.0, face.x);
    float width = w * (1.0 - t) * (0.75 + 0.5 * sqrt(max(t, 0.0))); // fullest a little above the root
    return p + right * ((side - 0.5) * width);
}

void main() {
    GrassBlade b = uBlades[uInstanceBase + gl_InstanceID];
    int v = gl_VertexID;
    float t, side;
    if (v >= 2 * uSegments) { t = 1.0; side = 0.5; }
    else { t = float(v >> 1) / float(uSegments); side = float(v & 1); }

    vec3 T, R;
    precise vec3 world = BladePoint(b, t, side, uWindTime, T, R);
    vec3 N = normalize(cross(R, T));
    N = normalize(N + R * ((side - 0.5) * 0.9)); // rounded across the blade, so it doesn't light flat
    vWorldPos = world;
    vNormal = N;
    vT = t;
    vBlade = vec4(b.C.x, b.C.y, b.C.z, 0.0);
    gl_Position = uProj * (uView * vec4(world, 1.0));
    if (uTaaVelocity == 1) {
        vCurClip = gl_Position;
        vCurClip.xy -= uTaaJitter * gl_Position.w;
        vec3 Tp, Rp;
        vec3 prev = BladePoint(b, t, side, uPrevWindTime, Tp, Rp);
        vPrevClip = uPrevViewProj * vec4(prev, 1.0);
    } else {
        vCurClip = vec4(0.0, 0.0, 0.0, 1.0);
        vPrevClip = vec4(0.0, 0.0, 0.0, 1.0);
    }
}
