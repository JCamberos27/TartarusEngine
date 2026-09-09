#version 460 core
out vec2 vUV;
void main() {
    // Single covering triangle from gl_VertexID: (0,0) (2,0) (0,2) in UV, mapped to clip.
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
