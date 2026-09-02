#include "ModelMesh.h"
#include "gl.h"
#include <cstddef>

// Imported geometry is static, so the buffers use immutable storage (flags 0) and the VAO is
// configured entirely through Direct State Access — no glBind* to edit. Building a ModelMesh
// mid-frame (e.g. a drag-drop import) therefore leaves the renderer's bound VAO / array-buffer
// / element-buffer untouched, which the old bind-to-edit path silently clobbered (#97).
ModelMesh::ModelMesh(const std::vector<ModelVertex>& vertices, const std::vector<unsigned int>& indices) {
    m_IndexCount = static_cast<unsigned int>(indices.size());

    m_LocalPositions.reserve(vertices.size());
    for (const auto& v : vertices) m_LocalPositions.push_back(v.Position);

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
}

ModelMesh::~ModelMesh() {
    glDeleteBuffers(1, &m_VBO);
    glDeleteBuffers(1, &m_EBO);
    glDeleteVertexArrays(1, &m_VAO);
}

void ModelMesh::Draw() const {
    glBindVertexArray(m_VAO);
    glDrawElements(GL_TRIANGLES, m_IndexCount, GL_UNSIGNED_INT, nullptr);
}
