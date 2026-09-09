#include "Grid.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

Grid::Grid() {
    m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("Grid.vert.glsl"),
                                        ShaderLibrary::ReadFile("Grid.frag.glsl"));
    glGenVertexArrays(1, &m_VAO); // no attributes: the vertex shader generates positions from gl_VertexID
}

Grid::~Grid() {
    glDeleteVertexArrays(1, &m_VAO);
}

void Grid::Draw(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPos,
                 float minorSpacing, float majorEvery, float fadeDistance,
                 float opacity, bool showAxisLines, float axisThickness) {
    glm::mat4 invViewProj = glm::inverse(proj * view);

    // Save exactly what this pass mutates and restore it — the fixed drawScene call order is
    // the only reason unconditional glDisable(GL_BLEND) at the end works today (#112).
    const GLboolean wasBlend = glIsEnabled(GL_BLEND);
    GLint prevDepthMask = GL_TRUE;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &prevDepthMask);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    m_Shader->Bind();
    m_Shader->SetMat4("uInvViewProj", invViewProj);
    m_Shader->SetVec3("uCameraPos", cameraPos);
    m_Shader->SetFloat("uMinorSpacing", minorSpacing);
    m_Shader->SetFloat("uMajorEvery", majorEvery);
    m_Shader->SetFloat("uFadeDistance", fadeDistance);
    m_Shader->SetFloat("uOpacity", opacity);
    m_Shader->SetInt("uShowAxes", showAxisLines ? 1 : 0);
    m_Shader->SetFloat("uAxisThickness", axisThickness);

    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthMask((GLboolean)prevDepthMask);
    if (!wasBlend) glDisable(GL_BLEND);
}
