#pragma once
#include <vector>
#include "ModelVertex.h"

// Procedural unit-sized primitive geometry (cube: -0.5..0.5, sphere/cylinder/cone: 0.5
// radius, 1.0 height, plane: 1x1), built as full ModelVertex data (position, normal, UV,
// tangent) so these participate in the same PBR materials, vertex snapping, and gizmo
// manipulation as imported models — scale via the placed instance's own Scale, same as
// any imported asset.
namespace PrimitiveMeshes {
    void GenerateCube(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices);
    void GeneratePlane(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices);
    void GenerateSphere(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices, int latSegments = 24, int lonSegments = 24);
    void GenerateCylinder(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices, int segments = 24);
    void GenerateCone(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices, int segments = 24);
    // Square base (-0.5..0.5) with an apex at y = +0.5.
    void GeneratePyramid(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices);
    // Right-triangular prism / ramp: full 1x1x1 box footprint, top sloping from y=+0.5 at
    // z=-0.5 down to y=-0.5 at z=+0.5.
    void GenerateWedge(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices);
    // Ring lying in the XZ plane; outer radius 0.5, tube radius 0.15.
    void GenerateTorus(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices, int majorSegments = 32, int minorSegments = 16);
    // Capsule aligned to Y: total height 1.0, radius 0.25 (cylinder body + hemisphere caps).
    void GenerateCapsule(std::vector<ModelVertex>& vertices, std::vector<unsigned int>& indices, int segments = 24, int capRings = 8);
}
