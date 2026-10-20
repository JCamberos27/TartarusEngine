#version 460 core
// One covering triangle at the far plane: under GL_LEQUAL it only lands where nothing nearer
// was drawn, i.e. on the sky (VolumetricFog::DrawSky).
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
}
