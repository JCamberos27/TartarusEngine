#version 460 core
// Octahedral impostors' depth (ImpostorRenderer.h): the camera's depth pre-pass and the sun's cascades.
#include "ImpostorSurface.glsl"

void main() {
    ImpostorFrag f;
    if (!ImpostorResolve(f)) discard;
}
