// The clustered-forward grid (ClusterGrid.h GRID_X / GRID_Y / GRID_Z): the one GLSL copy, included by the build, cull,
// decal cull, model shading and volumetric-inject shaders. Keep it equal to the C++ constants.
#pragma once
const uint C_GX = 16u, C_GY = 9u, C_GZ = 24u;
