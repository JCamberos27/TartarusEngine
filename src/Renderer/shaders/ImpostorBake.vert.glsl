#version 460 core
// --bake-impostors (ImpostorBake.cpp): one frame of an impostor atlas. Static meshes only (no skinning, no wind).
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec3 aTangent;
layout (location = 6) in float aTangentSign;
layout (location = 7) in vec4 aColor;

uniform mat4 uModel;        // mesh -> the object's own space (its LOD group's, scale included)
uniform mat4 uNormalMatrix; // mat3 inverse-transpose of uModel in a mat4
uniform mat4 uViewProj;     // the frame's orthographic camera

out vec3 vPos;
out vec2 vUV;
out mat3 vTBN;
out vec4 vColor;

void main() {
    vec4 p = uModel * vec4(aPos, 1.0);
    vPos = p.xyz;
    gl_Position = uViewProj * p;
    vec3 n = normalize(mat3(uNormalMatrix) * aNormal);
    vec3 t = mat3(uModel) * aTangent;
    t = normalize(t - dot(t, n) * n + vec3(1e-6));
    vTBN = mat3(t, cross(n, t) * aTangentSign, n);
    vUV = aUV;
    vColor = aColor;
}
