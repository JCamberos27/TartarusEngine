#include "ScreenBloodOverlay.h"

#include "Combat/ScreenBlood.h"
#include "GLStateCache.h"
#include "KnifeFxLibrary.h"
#include "Log.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

#include <cmath>
#include <string>

ScreenBloodOverlay::ScreenBloodOverlay() = default;

ScreenBloodOverlay::~ScreenBloodOverlay() {
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
}

void ScreenBloodOverlay::Draw(unsigned int dstFbo, int width, int height, const ScreenBlood& blood) {
    if (width <= 0 || height <= 0 || blood.Splats().empty() || m_Failed) return;
    const KnifeFxLibrary& lib = KnifeFxLibrary::Get();
    if (m_Entry == -2 && lib.Loaded()) m_Entry = lib.Find("blood_side");
    const KnifeFxLibrary::Entry* e = lib.At(m_Entry);
    if (!e || e->Lib != KnifeFxImport::Library::Sprite) return;
    if (!m_Shader) {
        try {
            m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("ScreenBlood.vert.glsl"), ShaderLibrary::ReadFile("ScreenBlood.frag.glsl"),
                                                "ScreenBlood");
        } catch (const std::exception& ex) {
            Log::Error(std::string("Screen blood: ") + ex.what());
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
    glBindTextureUnit(0, lib.ColorArray(KnifeFxImport::Library::Sprite));
    glBindTextureUnit(1, lib.NormalArray(KnifeFxImport::Library::Sprite));
    m_Shader->SetInt("uColor", 0);
    m_Shader->SetInt("uNormal", 1);
    m_Shader->SetFloat("uAspect", (float)width / (float)height);
    m_Shader->SetInt("uColorLayer", e->ColorLayer);
    m_Shader->SetInt("uNormalLayer", e->NormalLayer);
    m_Shader->SetVec2("uGrid", glm::vec2((float)e->Cols, (float)e->Rows));
    glBindVertexArray(m_VAO);
    for (const ScreenBlood::Splat& s : blood.Splats()) {
        int frame = 0;
        float opacity = 1.0f;
        ScreenBlood::FrameAt(s.Age, s.Life, e->Frames, frame, opacity);
        if (opacity * s.Strength <= 0.01f) continue;
        m_Shader->SetVec4("uSplat", glm::vec4(s.Pos, s.Size, s.Rot));
        m_Shader->SetInt("uFrame", frame);
        m_Shader->SetInt("uFlip", s.Flip ? 1 : 0);
        m_Shader->SetFloat("uOpacity", opacity * s.Strength);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(0);
    blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int)prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    GLStateCache::Invalidate();
}
