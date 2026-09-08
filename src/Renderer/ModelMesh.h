#pragma once
#include <vector>
#include "ModelVertex.h"
#include "Material.h"

// One drawable sub-mesh of an imported Model (own vertex/index buffers, own PBR material
// as extracted from the source file's own material assignment).
class ModelMesh {
public:
    ModelMesh(const std::vector<ModelVertex>& vertices, const std::vector<unsigned int>& indices);
    ~ModelMesh();

    void Draw() const;

    Material Mat;

    // Local-space (bind-pose) vertex positions, kept on the CPU alongside the GPU buffer
    // for vertex-snapping queries and mesh/convex collider cooking (#185 PR 6). Not re-skinned
    // per animation frame — snapping / static colliders target the rest pose, which is what
    // you want for static props anyway.
    const std::vector<glm::vec3>& LocalPositions() const { return m_LocalPositions; }

    // The triangle index list (matches LocalPositions()), kept for triangle-mesh collider
    // cooking (#185 PR 6). size() == IndexCount().
    const std::vector<unsigned int>& LocalIndices() const { return m_LocalIndices; }

    // Geometry counts for the editor's statistics overlay.
    unsigned int IndexCount() const { return m_IndexCount; }
    unsigned int VertexCount() const { return (unsigned int)m_LocalPositions.size(); }

private:
    unsigned int m_VAO = 0, m_VBO = 0, m_EBO = 0;
    unsigned int m_IndexCount = 0;
    std::vector<glm::vec3> m_LocalPositions;
    std::vector<unsigned int> m_LocalIndices;
};
