#version 460 core
// A Fill layer: the whole canvas in the layer's colour or gradient (Clip keeps it inside the paint
// below, in the composite).
#include "GraffitiCommon.glsl"

in vec2 vUV;
uniform vec3 uFill; // linear

layout(location = 0) out vec4 oColor;
layout(location = 1) out vec4 oThick;

void main() {
    vec2 canvasPx = vec2(vUV.x, 1.0 - vUV.y) * uCanvas;
    oColor = vec4(GradientAt(canvasPx, uFill), 1.0);
    oThick = vec4(0.6, 0.0, 0.0, 1.0);
}
