#include "PlayerHudOverlay.h"
#include "Combat/PlayerVitals.h"
#include "GLStateCache.h"
#include "Log.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

#include <algorithm>
#include <string>

PlayerHudOverlay::PlayerHudOverlay() = default;

PlayerHudOverlay::~PlayerHudOverlay() {
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
}

void PlayerHudOverlay::Draw(unsigned int dstFbo, int width, int height, const PlayerHudState& st, float time) {
    if (width <= 0 || height <= 0 || !st.Visible || m_Failed) return;
    if (!m_Shader) {
        try {
            m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("ChannelPreview.vert.glsl"),
                                                ShaderLibrary::ReadFile("PlayerHud.frag.glsl"), "PlayerHud");
        } catch (const std::exception& e) {
            Log::Error(std::string("Player HUD: ") + e.what());
            m_Failed = true;
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
    m_Shader->SetFloat("uHealth", st.Health01);
    m_Shader->SetFloat("uHurt", st.HurtFlash);
    m_Shader->SetInt("uDead", st.Dead ? 1 : 0);
    m_Shader->SetFloat("uDeathFade", st.DeathFade);
    m_Shader->SetFloat("uRespawn", st.RespawnProgress);
    m_Shader->SetInt("uProtected", st.Protected ? 1 : 0);
    m_Shader->SetInt("uArcs", st.Arcs);
    m_Shader->SetFloatArray("uArcAngle", 8, st.ArcAngle);
    m_Shader->SetFloatArray("uArcAlpha", 8, st.ArcAlpha);
    m_Shader->SetFloat("uHitmarker", st.Hitmarker);
    m_Shader->SetInt("uHitKill", st.HitmarkerKill ? 1 : 0);
    m_Shader->SetInt("uHitHead", st.HitmarkerHead ? 1 : 0);
    m_Shader->SetFloat("uTime", time);
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
