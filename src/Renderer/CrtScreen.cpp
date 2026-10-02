#include "CrtScreen.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

#include <cmath>
#include <glm/glm.hpp>

CrtScreen::~CrtScreen() {
    delete m_Shader;
    if (m_Vao) glDeleteVertexArrays(1, &m_Vao);
}

void CrtScreen::Curve(float& x, float& y, float curve) {
    float u = x * 2.0f - 1.0f, v = y * 2.0f - 1.0f;
    const float ox = std::fabs(v) / 6.0f, oy = std::fabs(u) / 4.6f;
    const float cu = u + u * ox * ox * curve, cv = v + v * oy * oy * curve;
    x = cu * 0.5f + 0.5f;
    y = cv * 0.5f + 0.5f;
}

void CrtScreen::Apply(unsigned int srcTex, int width, int height, unsigned int dstFbo,
                      float timeSeconds, float curve, float strength) {
    if (!m_Shader) {
        // Same full-screen triangle as the modal blur.
        m_Shader = new Shader(ShaderLibrary::ReadFile("ScreenBlur.vert.glsl"),
                              ShaderLibrary::ReadFile("CrtScreen.frag.glsl"));
        glGenVertexArrays(1, &m_Vao);
    }
    const GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, width, height);
    m_Shader->Bind();
    m_Shader->SetInt("uTex", 0);
    m_Shader->SetVec2("uSize", glm::vec2((float)width, (float)height));
    m_Shader->SetFloat("uTime", timeSeconds);
    m_Shader->SetFloat("uCurve", curve);
    m_Shader->SetFloat("uStrength", strength);
    GLStateCache::BindTexture2D(0, srcTex);
    glBindVertexArray(m_Vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    if (prevBlend) glEnable(GL_BLEND);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);
    GLStateCache::Invalidate();
}
