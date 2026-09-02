#include "Framebuffer.h"
#include "Log.h"
#include "gl.h"

#include <string>

Framebuffer::~Framebuffer() { Release(); }

void Framebuffer::Release() {
    if (m_DepthRbo) { glDeleteRenderbuffers(1, &m_DepthRbo); m_DepthRbo = 0; }
    if (m_ColorTexture) { glDeleteTextures(1, &m_ColorTexture); m_ColorTexture = 0; }
    if (m_Fbo) { glDeleteFramebuffers(1, &m_Fbo); m_Fbo = 0; }
}

void Framebuffer::Create(int width, int height) {
    Release();
    m_Width = width;
    m_Height = height;

    // Direct State Access + immutable colour storage (#96) — set up without disturbing the
    // currently-bound framebuffer / texture / renderbuffer.
    glCreateTextures(GL_TEXTURE_2D, 1, &m_ColorTexture);
    glTextureStorage2D(m_ColorTexture, 1, GL_RGB8, width, height);
    glTextureParameteri(m_ColorTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_ColorTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_ColorTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_ColorTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Renderbuffer storage still has no DSA form in core GL; the one transient bind here is
    // restored immediately and never observed by the renderer's own passes.
    glGenRenderbuffers(1, &m_DepthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_DepthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glCreateFramebuffers(1, &m_Fbo);
    glNamedFramebufferTexture(m_Fbo, GL_COLOR_ATTACHMENT0, m_ColorTexture, 0);
    glNamedFramebufferRenderbuffer(m_Fbo, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_DepthRbo);

    GLenum status = glCheckNamedFramebufferStatus(m_Fbo, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Log::Error("Framebuffer incomplete (0x" + std::to_string(status) + ") at " +
                   std::to_string(width) + "x" + std::to_string(height));
    }
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
