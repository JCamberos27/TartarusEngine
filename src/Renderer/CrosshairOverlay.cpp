#include "CrosshairOverlay.h"
#include "GLStateCache.h"
#include "Log.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

#include <algorithm>
#include <string>

CrosshairOverlay::CrosshairOverlay() = default;

CrosshairOverlay::~CrosshairOverlay() {
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
}

void CrosshairOverlay::Draw(unsigned int dstFbo, int width, int height, bool holding, float charge) {
    if (width <= 0 || height <= 0) return;
    if (!m_Shader) {
        try {
            m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("ChannelPreview.vert.glsl"),
                                                ShaderLibrary::ReadFile("Crosshair.frag.glsl"), "Crosshair");
        } catch (const std::exception& e) {
            Log::Error(std::string("Crosshair: ") + e.what());
            return;
        }
        glGenVertexArrays(1, &m_VAO);
    }

    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);

    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_Shader->Bind();
    m_Shader->SetVec2("uSize", glm::vec2((float)width, (float)height));
    m_Shader->SetFloat("uScale", std::max(0.75f, (float)height / 1080.0f));
    m_Shader->SetInt("uHolding", holding ? 1 : 0);
    m_Shader->SetFloat("uCharge", charge);
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int)prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    GLStateCache::Invalidate();
}
