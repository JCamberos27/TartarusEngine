#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;     // beam: (metres from the emitter, -1..1 across); decals: -1..1 square
layout(location = 2) in vec4 aColor;  // rgb linear HDR; a = beam energy / decal seed
layout(location = 3) in float aKind;  // 0 beam, 1 laser spot, 2 bullet hole
uniform mat4 uViewProj;
out vec2 vUV;
out vec4 vColor;
flat out int vKind;
void main() {
    vUV = aUV;
    vColor = aColor;
    vKind = int(aKind + 0.5);
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
