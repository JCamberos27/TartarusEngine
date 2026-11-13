#version 460 core
// Octahedral impostors (ImpostorRenderer.h): one card per instance, facing the viewer, no vertex attributes. The same
// stage serves the depth pre-pass, the lit pass and the sun's cascades: the pre-pass and the lit pass must land on
// bit-identical depth (the lit pass tests it GL_EQUAL, as the model programs do since #768), so they run this source
// with the same uniforms, everything `precise`.
invariant gl_Position;
#include "ImpostorCommon.glsl"

struct ImpostorInstance {
    vec4 CentreScale; // xyz the bounding sphere's centre (world), w the copy's scale
    vec4 Rotation;    // the group's rotation, object -> world (a quaternion, xyzw)
    vec4 OriginFade;  // xyz the group's origin (world: the wind's phase and height mask), w its LOD cross-fade
};
layout(std430, binding = 23) readonly buffer ImpostorInstances { ImpostorInstance uImpostors[]; };

uniform mat4 uView;
uniform mat4 uProj;
uniform vec3 uEye;            // the camera (perspective views)
uniform int uOrtho;           // 1: an orthographic view (a sun cascade): uLightViewProj, looking along -uOrthoToViewer
uniform vec3 uOrthoToViewer;  // world, unit
uniform mat4 uLightViewProj;
uniform float uRadius;        // the set's bounding sphere at scale 1
uniform vec3 uBoxMin;         // the LOD 0's box, relative to the sphere's centre, at scale 1
uniform vec3 uBoxMax;
uniform int uFrames;          // frames per atlas side
// The card's sway: FoliageWind.glsl's slow bend, at the set's own strength (WindStrength x (1 - WindFlutter)).
uniform float uWindTime;
uniform vec2 uWindDir;
uniform float uWindBend;
uniform float uWindHeight;

// Object space below is the group's own at scale 1, centred on the bounding sphere.
out vec3 vObj;                 // the card point
out vec3 vRay;                 // the view ray through it (unnormalised; constant for an orthographic view)
out vec3 vCardWorld;           // the card point in the world, swayed
out vec3 vEyeRay;              // world: the view ray through it (unnormalised)
flat out vec3 vToViewerObj;    // the card's normal
flat out vec3 vToViewerWorld;
flat out vec3 vFrameDir[3];    // the three frames blended: their view directions and image axes
flat out vec3 vFrameRight[3];
flat out vec3 vFrameUp[3];
flat out vec2 vFrameCell[3];   // their (column, row) in the atlas
flat out vec3 vWeights;
flat out vec4 vRot;
flat out float vScale;
flat out float vFade;

vec3 QRot(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

void main() {
    ImpostorInstance inst = uImpostors[gl_BaseInstance + gl_InstanceID];
    precise vec3 C = inst.CentreScale.xyz;
    precise float s = inst.CentreScale.w;
    precise float R = uRadius * s;
    vec4 q = inst.Rotation;
    vec4 qi = vec4(-q.xyz, q.w);
    precise vec3 toViewer = uOrtho == 1 ? uOrthoToViewer : normalize(uEye - C);
    precise vec3 vo = QRot(qi, toViewer);
    // The frames: the hemi-octahedral grid cell the view falls in, split along its diagonal, blended barycentrically.
    // Seen from below the horizon, the horizon's frames.
    vec3 vh = vec3(vo.x, max(vo.y, 0.0), vo.z);
    if (dot(vh, vh) < 1e-8) vh = vec3(1.0, 0.0, 0.0);
    float last = float(uFrames - 1);
    vec2 g = (ImpostorHemiOctEncode(normalize(vh)) * 0.5 + 0.5) * last;
    vec2 cell = clamp(floor(g), vec2(0.0), vec2(last - 1.0));
    vec2 f = clamp(g - cell, 0.0, 1.0);
    vec2 cells[3];
    if (f.x + f.y <= 1.0) {
        cells = vec2[3](cell, cell + vec2(1.0, 0.0), cell + vec2(0.0, 1.0));
        vWeights = vec3(1.0 - f.x - f.y, f.x, f.y);
    } else {
        cells = vec2[3](cell + vec2(1.0), cell + vec2(0.0, 1.0), cell + vec2(1.0, 0.0));
        vWeights = vec3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y);
    }
    // The heaviest frame first: the depth offset reads only it, and the sun's cascades take only it.
    if (vWeights.y > vWeights.x && vWeights.y >= vWeights.z) {
        cells = vec2[3](cells[1], cells[0], cells[2]);
        vWeights = vWeights.yxz;
    } else if (vWeights.z > vWeights.x && vWeights.z > vWeights.y) {
        cells = vec2[3](cells[2], cells[1], cells[0]);
        vWeights = vWeights.zyx;
    }
    for (int k = 0; k < 3; ++k) {
        vec3 d = ImpostorHemiOctDecode(cells[k] / last * 2.0 - 1.0);
        vec3 r, u;
        ImpostorFrameBasis(d, r, u);
        vFrameDir[k] = d;
        vFrameRight[k] = r;
        vFrameUp[k] = u;
        vFrameCell[k] = cells[k];
    }

    // The card: square to the viewer at the sphere's near side, so every surface point lies behind it and the depth
    // offset only ever pushes away. Its outline is the LOD 0's box as seen from here (projected along the view rays onto
    // the card): a Cedrus is ~8 m across and ~22 m tall, a third of its sphere's square from the side and a tenth of
    // it from above (the sun), and every card pixel runs the frame lookups.
    vec3 right, up;
    ImpostorFrameBasis(toViewer, right, up);
    // The sun's cascades (uOrtho) take the card through the centre instead: they keep its depth (ImpostorShadow.frag).
    precise vec3 Pc = C + toViewer * (uOrtho == 1 ? 0.0 : R);
    precise vec2 lo = vec2(1e30), hi = vec2(-1e30);
    for (int k = 0; k < 8; ++k) {
        vec3 b = vec3((k & 1) != 0 ? uBoxMax.x : uBoxMin.x, (k & 2) != 0 ? uBoxMax.y : uBoxMin.y,
                      (k & 4) != 0 ? uBoxMax.z : uBoxMin.z);
        precise vec3 w = C + QRot(q, b * s);
        if (uOrtho != 1) { // along the ray from the eye, onto the card's plane
            precise vec3 dir = w - uEye;
            precise float den = dot(dir, toViewer);
            w = uEye + dir * (dot(Pc - uEye, toViewer) / (abs(den) > 1e-6 ? den : -1e-6));
        }
        precise vec2 uv = vec2(dot(w - Pc, right), dot(w - Pc, up));
        lo = min(lo, uv);
        hi = max(hi, uv);
    }
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    precise vec3 P = Pc + right * mix(lo.x, hi.x, corner.x) + up * mix(lo.y, hi.y, corner.y);
    vObj = QRot(qi, P - C) / s;
    vRay = uOrtho == 1 ? -vo : vObj - QRot(qi, uEye - C) / s;

    // Sway: the canopy shears with the gust, as FoliageWindOffsetAt bends the tree (its height mask on the group's own
    // height, the offset in metres). The atlas lookup above stays with the still card.
    precise vec3 origin = inst.OriginFade.xyz;
    precise float mask = clamp((P.y - origin.y) / s / uWindHeight, 0.0, 1.0);
    precise float along = dot(origin.xz, uWindDir);
    precise float gust = 0.55 + 0.30 * sin(along * 0.35 - uWindTime * 1.1) + 0.15 * sin(along * 0.9 - uWindTime * 2.3 + 1.7);
    vec2 side = vec2(-uWindDir.y, uWindDir.x);
    precise vec2 bend = (uWindDir * gust + side * 0.25 * sin(uWindTime * 0.7 + along * 0.2)) * (mask * mask);
    precise vec3 world = P + vec3(bend.x, 0.0, bend.y) * (1.4 * uWindBend);

    vCardWorld = world;
    vEyeRay = uOrtho == 1 ? -toViewer : world - uEye;
    vToViewerObj = vo;
    vToViewerWorld = toViewer;
    vRot = q;
    vScale = s;
    vFade = inst.OriginFade.w;
    gl_Position = uOrtho == 1 ? uLightViewProj * vec4(world, 1.0) : uProj * (uView * vec4(world, 1.0));
}
