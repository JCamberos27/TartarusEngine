#include "DebugLineRenderer.h"

#include "GLStateCache.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

DebugLineRenderer::DebugLineRenderer() {
    m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("ColliderGizmo.vert.glsl"),
                                        ShaderLibrary::ReadFile("ColliderGizmo.frag.glsl"));
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    const GLsizei stride = 7 * (GLsizei)sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
}

DebugLineRenderer::~DebugLineRenderer() {
    if (m_VBO) glDeleteBuffers(1, &m_VBO);
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
}

void DebugLineRenderer::Draw(const glm::mat4& viewProj, const float* verts, size_t floatCount) {
    if (floatCount < 14) return; // not even one line
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    if (floatCount > m_Capacity) {
        glBufferData(GL_ARRAY_BUFFER, floatCount * sizeof(float), verts, GL_DYNAMIC_DRAW);
        m_Capacity = floatCount;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, floatCount * sizeof(float), verts);
    }

    m_Shader->Bind();
    m_Shader->SetMat4("uViewProj", viewProj);

    glEnable(GL_BLEND);
    // Straight alpha-over for everything so a pure-colour line renders as that exact colour
    // (additive would tint it toward whatever is behind it). Alpha carries the fade.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_LINES, 0, (GLsizei)(floatCount / 7));
    glDisable(GL_BLEND);
    glBindVertexArray(0);

    GLStateCache::Invalidate(); // raw program/VAO/buffer binds above bypass the cache
}
