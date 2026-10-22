#version 460 core
// A stamp or a letter (Graffiti::Quad): the image's box, grown by the outline so it has room.
#include "GraffitiCommon.glsl"

uniform vec2 uCentre, uHalf; // texels
uniform float uAngle;
uniform float uGrow;         // texels added round the box (the outline)

out vec2 vUV;      // 0..1 across the image (past it in the grown border)
out vec2 vCanvas;  // canvas texels, for the gradient

void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)) * 2.0 - 1.0;
    vec2 ext = uHalf + vec2(uGrow);
    vec2 off = corner * ext;
    vUV = vec2(0.5) + off / (2.0 * max(uHalf, vec2(1e-3)));
    float c = cos(uAngle), s = sin(uAngle);
    vCanvas = uCentre + vec2(off.x * c - off.y * s, off.x * s + off.y * c);
    gl_Position = CanvasToClip(vCanvas);
}
