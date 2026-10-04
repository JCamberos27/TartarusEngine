#version 460 core
// Blood decals (docs/BLOOD_FX.md): each instance is a unit box (-0.5..0.5) projected onto whatever
// static surface lies inside it. The box comes from gl_VertexID (36 vertices, no buffer).

struct BloodDecal {
    mat4 Model;    // box -> world
    mat4 InvModel; // world -> box
    vec4 RectNorm; // atlas rect of the normal / alpha map: offset, size
    vec4 RectMask; // atlas rect of the mask (r reveal order, b thick core)
    vec4 Params;   // x cutout (0 spread .. 1 gone), y dryness 0..1, z opacity, w normal strength
    vec4 Axis;     // xyz the box's +Y in world (out of the surface), w unused
};
layout(std430, binding = 9) readonly buffer BloodDecals { BloodDecal uDecals[]; };

uniform mat4 uView;
uniform mat4 uProj;

flat out int vDecal;

const int kCube[36] = int[36](0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                              2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5);

void main() {
    vDecal = gl_BaseInstance + gl_InstanceID;
    int c = kCube[gl_VertexID];
    vec3 p = vec3((c & 1) != 0 ? 0.5 : -0.5, (c & 2) != 0 ? 0.5 : -0.5, (c & 4) != 0 ? 0.5 : -0.5);
    gl_Position = uProj * (uView * (uDecals[vDecal].Model * vec4(p, 1.0)));
}
