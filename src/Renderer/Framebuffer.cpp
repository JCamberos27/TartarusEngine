#include "Framebuffer.h"
#include "gl.h"

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

    glGenFramebuffers(1, &m_Fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);

    glGenTextures(1, &m_ColorTexture);
    glBindTexture(GL_TEXTURE_2D, m_ColorTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ColorTexture, 0);

    glGenRenderbuffers(1, &m_DepthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_DepthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_DepthRbo);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
