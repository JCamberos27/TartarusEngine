#version 460 core
// The HUD batch: pixel positions from the top-left, per-vertex colour, an atlas coordinate.
layout(location = 0) in vec4 aPosUV;
layout(location = 1) in vec4 aColor;
uniform vec2 uSize;
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aPosUV.zw;
    vColor = aColor;
    gl_Position = vec4(aPosUV.x / uSize.x * 2.0 - 1.0, 1.0 - aPosUV.y / uSize.y * 2.0, 0.0, 1.0);
}
