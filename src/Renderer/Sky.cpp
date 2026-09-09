#include "Sky.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

Sky::Sky() {
    m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("Sky.vert.glsl"),
                                        ShaderLibrary::ReadFile("Sky.frag.glsl"));
    glGenVertexArrays(1, &m_VAO); // no attributes: the vertex shader generates positions from gl_VertexID
}

Sky::~Sky() {
    glDeleteVertexArrays(1, &m_VAO);
}

void Sky::Draw(const glm::mat4& view, const glm::mat4& proj,
               const glm::vec3& horizonColor, const glm::vec3& zenithColor) {
    glm::mat4 invViewProj = glm::inverse(proj * view);

    // Save exactly what this pass mutates (#112) — the unconditional glEnable(GL_DEPTH_TEST)
    // at the end is wrong if depth test was off on entry; it only works because of call order.
    const GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint prevDepthMask = GL_TRUE;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &prevDepthMask);

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);

    m_Shader->Bind();
    m_Shader->SetMat4("uInvViewProj", invViewProj);
    m_Shader->SetVec3("uHorizonColor", horizonColor);
    m_Shader->SetVec3("uZenithColor", zenithColor);

    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (wasDepthTest) glEnable(GL_DEPTH_TEST);
    glDepthMask((GLboolean)prevDepthMask);
}
