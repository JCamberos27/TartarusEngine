#include "ChannelPreviewRenderer.h"
#include "Texture.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLFramebufferCheck.h"
#include "Log.h"
#include "Profiler.h"
#include "gl.h"

#include <string>

ChannelPreviewRenderer::ChannelPreviewRenderer() = default;

ChannelPreviewRenderer::~ChannelPreviewRenderer() {
    if (m_ColorTex) glDeleteTextures(1, &m_ColorTex);
    if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
}

unsigned int ChannelPreviewRenderer::Render(const Texture& source, int channel, int previewW, int previewH) {
    PROFILE_GPU_SCOPE("Channel Preview");
    if (!m_Shader) {
        m_Shader = std::make_unique<Shader>(ShaderLibrary::ReadFile("Fullscreen.vert.glsl"),
                                            ShaderLibrary::ReadFile("ChannelPreview.frag.glsl"));
        glGenVertexArrays(1, &m_VAO);
        glGenFramebuffers(1, &m_FBO);
        glGenTextures(1, &m_ColorTex);
    }

    if (previewW != m_TexW || previewH != m_TexH) {
        m_TexW = previewW;
        m_TexH = previewH;
        glBindTexture(GL_TEXTURE_2D, m_ColorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, previewW, previewH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // This runs mid-frame, nested inside the editor's own ImGui pass — restore whatever
    // framebuffer/viewport was bound before this call rather than assuming it was the default
    // one, so the main 3D viewport rendered elsewhere this same frame isn't left pointing at
    // this offscreen target or a stale viewport rect.
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);

    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ColorTex, 0);

    if (!GLFramebufferCheck::Complete("ChannelPreviewRenderer", m_FBO, previewW, previewH)) {
        glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int)prevFBO);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        return m_ColorTex;
    }

    glViewport(0, 0, previewW, previewH);

    m_Shader->Bind();
    source.Bind(0);
    m_Shader->SetInt("uTex", 0);
    m_Shader->SetInt("uChannel", channel);

    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, (unsigned int)prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    return m_ColorTex;
}
