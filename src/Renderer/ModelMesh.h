#pragma once
#include <cstdint>
#include <vector>
#include "ModelVertex.h"
#include "Material.h"

// One drawable sub-mesh of an imported Model (own vertex/index buffers, own PBR material
// as extracted from the source file's own material assignment).
class ModelMesh {
public:
    ModelMesh(const std::vector<ModelVertex>& vertices, const std::vector<unsigned int>& indices);
    // Deferred: keeps the vertices on the CPU and creates no GL objects, so an import can run on
    // a worker thread (AsyncAssetLoader). FinishUpload() creates the buffers later, main thread.
    ModelMesh(std::vector<ModelVertex>&& vertices, const std::vector<unsigned int>& indices, bool deferUpload);
    ~ModelMesh();

    bool NeedsUpload() const { return m_VAO == 0; }
    void FinishUpload();

    // `instances` > 1 draws that many instances (the sun's layered cascade pass).
    void Draw(int instances = 1) const;
    // Draws `count` indices at byte `offset` of another element buffer over this mesh's vertices (an outfit
    // piece's visible triangles, VisibleIndexBuffer); the mesh's own buffer is bound again after.
    void DrawIndices(unsigned elementBuffer, std::uint32_t offset, std::uint32_t count, int instances = 1) const;

    Material Mat;

    // Local-space (bind-pose) vertex positions, kept on the CPU alongside the GPU buffer
    // for vertex-snapping queries and mesh/convex collider cooking (#185 PR 6). Not re-skinned
    // per animation frame — snapping / static colliders target the rest pose, which is what
    // you want for static props anyway.
    const std::vector<glm::vec3>& LocalPositions() const { return m_LocalPositions; }
    // #98 — a skinned mesh's GPU vertices stay in mesh space (the bone palette places them), so
    // Model replaces this CPU copy with the bind-pose positions used for bounds, picking,
    // snapping and collider cooking.
    void SetLocalPositions(std::vector<glm::vec3> positions) { m_LocalPositions = std::move(positions); }

    // The triangle index list (matches LocalPositions()), kept for triangle-mesh collider
    // cooking (#185 PR 6). size() == IndexCount().
    const std::vector<unsigned int>& LocalIndices() const { return m_LocalIndices; }

    // A skinned mesh's vertices as the vertex shader takes them (mesh space, up to four bone influences), kept on
    // the CPU for queries of the posed surface (the first-person body's camera probe). Empty when not skinned.
    struct SkinVertex {
        glm::vec3 Position{0.0f};
        int BoneIDs[MAX_BONE_INFLUENCE] = {-1, -1, -1, -1};
        float Weights[MAX_BONE_INFLUENCE] = {0, 0, 0, 0};
    };
    const std::vector<SkinVertex>& SkinVertices() const { return m_Skin; }

    // Geometry counts for the editor's statistics overlay.
    unsigned int IndexCount() const { return m_IndexCount; }
    unsigned int VertexCount() const { return (unsigned int)m_LocalPositions.size(); }

private:
    void CreateGpu(const std::vector<ModelVertex>& vertices);

    std::vector<ModelVertex> m_PendingVertices; // deferred meshes only, until FinishUpload
    unsigned int m_VAO = 0, m_VBO = 0, m_EBO = 0;
    unsigned int m_IndexCount = 0;
    std::vector<glm::vec3> m_LocalPositions;
    std::vector<unsigned int> m_LocalIndices;
    std::vector<SkinVertex> m_Skin;
    void KeepSkin(const std::vector<ModelVertex>& vertices);
};
