#include "ChannelPreviewRenderer.h"
#include "Texture.h"
#include "Shader.h"
#include "gl.h"

namespace {

const char* kVertexSrc = R"(
#version 330 core
out vec2 vUV;
void main() {
    // "Big triangle" trick: 3 vertices covering the whole viewport with no VBO at all - the
    // same no-attribute-VAO approach Grid.cpp/Sky.cpp use for their own fullscreen geometry.
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); // (0,0), (2,0), (0,2)
    vUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFragmentSrc = R"(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform int uChannel; // -1 = combined passthrough, 0..3 = isolate R/G/B/A as grayscale
void main() {
    vec4 texel = texture(uTex, vUV);
    if (uChannel < 0) { FragColor = texel; return; }
    float v = texel[uChannel];
    FragColor = vec4(v, v, v, 1.0);
}
)";

} // namespace

ChannelPreviewRenderer::ChannelPreviewRenderer() = default;

ChannelPreviewRenderer::~ChannelPreviewRenderer() {
    if (m_ColorTex) glDeleteTextures(1, &m_ColorTex);
    if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
    if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
}

unsigned int ChannelPreviewRenderer::Render(const Texture& source, int channel, int previewW, int previewH) {
    if (!m_Shader) {
        m_Shader = std::make_unique<Shader>(kVertexSrc, kFragmentSrc);
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
