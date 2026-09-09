#version 460 core
out vec2 vUV;
void main() {
    // "Big triangle" trick: 3 vertices covering the whole viewport with no VBO at all.
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); // (0,0), (2,0), (0,2)
    vUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
