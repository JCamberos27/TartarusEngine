#include "Sky.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

namespace {
// Sky pixels only: depth test GL_LEQUAL against the far-plane quad, no depth writes. Saves
// exactly what it mutates (#112) and restores it on scope exit.
struct SkyDepthScope {
    GLboolean WasDepthTest;
    GLint PrevMask = GL_TRUE, PrevFunc = GL_LESS;
    SkyDepthScope() {
        WasDepthTest = glIsEnabled(GL_DEPTH_TEST);
        glGetIntegerv(GL_DEPTH_WRITEMASK, &PrevMask);
        glGetIntegerv(GL_DEPTH_FUNC, &PrevFunc);
        glDepthMask(GL_FALSE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    }
    ~SkyDepthScope() {
        glDepthFunc((GLenum)PrevFunc);
        if (!WasDepthTest) glDisable(GL_DEPTH_TEST);
        glDepthMask((GLboolean)PrevMask);
        GLStateCache::Invalidate(); // raw VAO / texture binds below
    }
};
}

Sky::Sky() {
    m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("Sky.vert.glsl"),
                                        ShaderLibrary::ReadFile("Sky.frag.glsl"));
    m_HdriShader = std::make_unique<Shader>(ShaderLibrary::ReadFile("Sky.vert.glsl"),
                                             ShaderLibrary::ReadFile("SkyHdri.frag.glsl"));
    glGenVertexArrays(1, &m_VAO); // no attributes: the vertex shader generates positions from gl_VertexID
}

Sky::~Sky() {
    glDeleteVertexArrays(1, &m_VAO);
}

void Sky::Draw(const glm::mat4& view, const glm::mat4& proj,
               const glm::vec3& horizonColor, const glm::vec3& zenithColor) {
    glm::mat4 invViewProj = glm::inverse(proj * view);

    SkyDepthScope depth;

    m_Shader->Bind();
    m_Shader->SetMat4("uInvViewProj", invViewProj);
    m_Shader->SetVec3("uHorizonColor", horizonColor);
    m_Shader->SetVec3("uZenithColor", zenithColor);

    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Sky::DrawHdri(unsigned int cubeTex, float rotationRadians,
                   const glm::mat4& view, const glm::mat4& proj) {
    glm::mat4 invViewProj = glm::inverse(proj * view);

    SkyDepthScope depth;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubeTex);
    m_HdriShader->Bind();
    m_HdriShader->SetMat4("uInvViewProj", invViewProj);
    m_HdriShader->SetInt("uEnvMap", 0);
    m_HdriShader->SetFloat("uRotation", rotationRadians);

    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
