#version 460 core
// One spray footprint per instance (Graffiti::Splat, 64 bytes): a quad round the ellipse and its
// overspray ring.
#include "GraffitiCommon.glsl"

layout(location = 0) in vec4 aShape;  // centre xy, radius along / across
layout(location = 1) in vec4 aParams; // angle, flow, overspray, hardness
layout(location = 2) in vec4 aColor;  // linear rgb, thickness
layout(location = 3) in uvec2 aSeedFlags;
layout(location = 4) in vec2 aTimeRing;

out vec2 vLocal; // ellipse space: the core is the unit disc
flat out vec4 vParams;
flat out vec4 vColor;
flat out uint vSeed;
flat out float vRing;

void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)) * 2.0 - 1.0;
    float ring = max(aTimeRing.y, 1.0);
    vLocal = corner * ring;
    float c = cos(aParams.x), s = sin(aParams.x);
    vec2 off = vec2(vLocal.x * aShape.z, vLocal.y * aShape.w);
    vec2 p = aShape.xy + vec2(off.x * c - off.y * s, off.x * s + off.y * c);
    vParams = aParams;
    vColor = aColor;
    vSeed = aSeedFlags.x;
    vRing = ring;
    gl_Position = CanvasToClip(p);
}
