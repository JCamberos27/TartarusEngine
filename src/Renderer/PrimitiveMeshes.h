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
}
