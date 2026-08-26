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
    // for vertex-snapping queries. Not re-skinned per animation frame — snapping targets
    // the rest pose, which is what you want for static props anyway.
    const std::vector<glm::vec3>& LocalPositions() const { return m_LocalPositions; }

private:
    unsigned int m_VAO = 0, m_VBO = 0, m_EBO = 0;
    unsigned int m_IndexCount = 0;
    std::vector<glm::vec3> m_LocalPositions;
};
