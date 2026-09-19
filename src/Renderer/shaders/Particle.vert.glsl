#version 460 core
// #177 - one camera-facing quad per particle instance. The quad's corners come from
// gl_VertexID (two triangles, no vertex buffer); position/size/colour are per-instance.
layout(location = 0) in vec4 aPosSize; // xyz world position, w world-space diameter
layout(location = 1) in vec4 aColor;   // rgb (already scaled by intensity), a alpha

uniform mat4 uView;
uniform mat4 uProj;

out vec2 vCorner; // -1..1 across the quad
out vec4 vColor;

const vec2 kCorners[6] = vec2[6](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0));

void main() {
    vec2 c = kCorners[gl_VertexID];
    vec4 viewPos = uView * vec4(aPosSize.xyz, 1.0);
    viewPos.xy += c * (aPosSize.w * 0.5); // expand in view space = always faces the camera
    gl_Position = uProj * viewPos;
    vCorner = c;
    vColor = aColor;
}
