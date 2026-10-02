#include "ModelMesh.h"
#include "gl.h"
#include "GLStateCache.h"
#include "meshoptimizer.h"
#include <cstddef>
#include <vector>

// Triangle order for the GPU's post-transform vertex cache: a vertex shared by neighbouring triangles
// is shaded once instead of once per triangle. Imported meshes arrive in authoring order (the Quantum
// heads shaded ~2.1 vertices per triangle, ~0.65 in this order), and skinning makes every vertex
// invocation expensive, in every pass that draws the mesh. Only the triangles move: vertex IDs (the
// hide/collar bit buffers) and what's drawn are unchanged.
static void OptimizeTriangleOrder(std::vector<unsigned int>& indices, size_t vertexCount) {
    if (indices.size() < 3 || indices.size() % 3 != 0 || vertexCount == 0) return;
    meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertexCount);
}

// Imported geometry is static, so the buffers use immutable storage (flags 0) and the VAO is
// configured entirely through Direct State Access — no glBind* to edit. Building a ModelMesh
// mid-frame (e.g. a drag-drop import) therefore leaves the renderer's bound VAO / array-buffer
// / element-buffer untouched, which the old bind-to-edit path silently clobbered (#97).
ModelMesh::ModelMesh(const std::vector<ModelVertex>& vertices, const std::vector<unsigned int>& indices) {
    m_IndexCount = static_cast<unsigned int>(indices.size());

    m_LocalPositions.reserve(vertices.size());
    for (const auto& v : vertices) m_LocalPositions.push_back(v.Position);
    m_LocalIndices = indices; // kept for #185 PR 6 mesh-collider cooking
    OptimizeTriangleOrder(m_LocalIndices, vertices.size());
    KeepSkin(vertices);
    CreateGpu(vertices);
}

ModelMesh::ModelMesh(std::vector<ModelVertex>&& vertices, const std::vector<unsigned int>& indices, bool deferUpload) {
    m_IndexCount = static_cast<unsigned int>(indices.size());
    m_LocalPositions.reserve(vertices.size());
    for (const auto& v : vertices) m_LocalPositions.push_back(v.Position);
    m_LocalIndices = indices;
    OptimizeTriangleOrder(m_LocalIndices, vertices.size());
    KeepSkin(vertices);
    if (deferUpload) m_PendingVertices = std::move(vertices);
    else CreateGpu(vertices);
}

ModelMesh::ModelMesh(const ModelMesh& source, ShareGeometry)
    : Mat(source.Mat), m_VAO(source.m_VAO), m_VBO(source.m_VBO), m_EBO(source.m_EBO), m_GpuOwner(source.m_GpuOwner),
      m_IndexCount(source.m_IndexCount), m_LocalPositions(source.m_LocalPositions), m_LocalIndices(source.m_LocalIndices),
      m_Skin(source.m_Skin) {}

void ModelMesh::KeepSkin(const std::vector<ModelVertex>& vertices) {
    bool skinned = false;
    for (const auto& v : vertices)
        if (v.BoneIDs[0] >= 0) { skinned = true; break; }
    if (!skinned) return;
    m_Skin.resize(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        m_Skin[i].Position = vertices[i].Position;
        for (int k = 0; k < MAX_BONE_INFLUENCE; ++k) {
            m_Skin[i].BoneIDs[k] = vertices[i].BoneIDs[k];
            m_Skin[i].Weights[k] = vertices[i].Weights[k];
        }
    }
}

void ModelMesh::FinishUpload() {
    if (m_VAO) return;
    CreateGpu(m_PendingVertices);
    m_PendingVertices.clear();
    m_PendingVertices.shrink_to_fit();
}

void ModelMesh::CreateGpu(const std::vector<ModelVertex>& vertices) {
    const std::vector<unsigned int>& indices = m_LocalIndices;
    // glNamedBufferStorage rejects a zero size; a degenerate empty sub-mesh still needs a
    // valid buffer name for the VAO bindings, so floor the allocation at one element.
    const GLsizeiptr vboBytes = static_cast<GLsizeiptr>(
        (vertices.empty() ? 1 : vertices.size()) * sizeof(ModelVertex));
    const GLsizeiptr eboBytes = static_cast<GLsizeiptr>(
        (indices.empty() ? 1 : indices.size()) * sizeof(unsigned int));

    glCreateBuffers(1, &m_VBO);
    glCreateBuffers(1, &m_EBO);
    glNamedBufferStorage(m_VBO, vboBytes, vertices.empty() ? nullptr : vertices.data(), 0);
    glNamedBufferStorage(m_EBO, eboBytes, indices.empty() ? nullptr : indices.data(), 0);

    glCreateVertexArrays(1, &m_VAO);
    glVertexArrayVertexBuffer(m_VAO, 0, m_VBO, 0, sizeof(ModelVertex));
    glVertexArrayElementBuffer(m_VAO, m_EBO);

    auto floatAttrib = [&](GLuint index, GLint size, std::size_t offset) {
        glEnableVertexArrayAttrib(m_VAO, index);
        glVertexArrayAttribFormat(m_VAO, index, size, GL_FLOAT, GL_FALSE, static_cast<GLuint>(offset));
        glVertexArrayAttribBinding(m_VAO, index, 0);
    };
    floatAttrib(0, 3, offsetof(ModelVertex, Position));
    floatAttrib(1, 3, offsetof(ModelVertex, Normal));
    floatAttrib(2, 2, offsetof(ModelVertex, UV));
    floatAttrib(3, 3, offsetof(ModelVertex, Tangent));

    glEnableVertexArrayAttrib(m_VAO, 4);
    glVertexArrayAttribIFormat(m_VAO, 4, 4, GL_INT, static_cast<GLuint>(offsetof(ModelVertex, BoneIDs)));
    glVertexArrayAttribBinding(m_VAO, 4, 0);

    floatAttrib(5, 4, offsetof(ModelVertex, Weights));
    floatAttrib(6, 1, offsetof(ModelVertex, TangentSign));
    floatAttrib(7, 4, offsetof(ModelVertex, Color)); // #113

    struct Ids { unsigned vao, vbo, ebo; };
    m_GpuOwner = std::shared_ptr<void>(new Ids{m_VAO, m_VBO, m_EBO}, [](void* p) {
        Ids* ids = static_cast<Ids*>(p);
        glDeleteBuffers(1, &ids->vbo);
        glDeleteBuffers(1, &ids->ebo);
        glDeleteVertexArrays(1, &ids->vao);
        delete ids;
    });
}

ModelMesh::~ModelMesh() = default; // m_GpuOwner frees the buffers with the last mesh using them (none while deferred)

void ModelMesh::DrawIndices(unsigned elementBuffer, std::uint32_t offset, std::uint32_t count, int instances) const {
    if (!m_VAO || !count) return;
    GLStateCache::BindVertexArray(m_VAO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementBuffer); // VAO state: put the mesh's own back below
    const void* at = reinterpret_cast<const void*>((std::uintptr_t)offset);
    if (instances > 1)
        glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_INT, at, instances);
    else
        glDrawElements(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_INT, at);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
}

void ModelMesh::Draw(int instances) const {
    if (!m_VAO) return; // still deferred
    GLStateCache::BindVertexArray(m_VAO);
    if (instances > 1)
        glDrawElementsInstanced(GL_TRIANGLES, m_IndexCount, GL_UNSIGNED_INT, nullptr, instances);
    else
        glDrawElements(GL_TRIANGLES, m_IndexCount, GL_UNSIGNED_INT, nullptr);
}
