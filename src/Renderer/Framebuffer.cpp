#include "Framebuffer.h"
#include "GLFramebufferCheck.h"
#include "GLRetire.h"
#include "Log.h"
#include "gl.h"

#include <string>

Framebuffer::~Framebuffer() { Release(); }

void Framebuffer::Release() {
    if (m_ColorTexture) { glDeleteTextures(1, &m_ColorTexture); m_ColorTexture = 0; }
    if (m_Fbo) { glDeleteFramebuffers(1, &m_Fbo); m_Fbo = 0; }
}

void Framebuffer::Create(int width, int height) {
    // The colour texture being replaced may already be in this frame's ImGui draw list (the Game view's image is
    // recorded before its target resizes): it goes at the frame's end (GLRetire), not now. The FBO is never shown.
    GLRetire::Texture(m_ColorTexture);
    m_ColorTexture = 0;
    Release();
    m_Width = width;
    m_Height = height;

    // Direct State Access + immutable colour storage (#96) — set up without disturbing the
    // currently-bound framebuffer / texture.
    glCreateTextures(GL_TEXTURE_2D, 1, &m_ColorTexture);
    glTextureStorage2D(m_ColorTexture, 1, GL_RGB8, width, height);
    glTextureParameteri(m_ColorTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_ColorTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_ColorTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_ColorTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateFramebuffers(1, &m_Fbo);
    glNamedFramebufferTexture(m_Fbo, GL_COLOR_ATTACHMENT0, m_ColorTexture, 0);

    // #160 - the shared check (#358) logs the status as a named enum; this used to print
    // "0x" followed by the DECIMAL value.
    GLFramebufferCheck::Complete("Framebuffer", m_Fbo, width, height);
}

void Framebuffer::Resize(int width, int height) {
    width = width > 0 ? width : 1;
    height = height > 0 ? height : 1;
    if (m_Fbo != 0 && width == m_Width && height == m_Height) return;
    Create(width, height);
}

void Framebuffer::Bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glViewport(0, 0, m_Width, m_Height);
}

void Framebuffer::BindDefault(int windowWidth, int windowHeight) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, windowWidth, windowHeight);
}
