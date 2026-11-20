#include "FullscreenPass.h"
#include "GLFramebufferCheck.h"
#include "GLStateCache.h"
#include "gl.h"

#include <algorithm>

ColorTarget& ColorTarget::operator=(ColorTarget&& o) noexcept {
    if (this == &o) return *this;
    Release();
    m_Tex = o.m_Tex; m_Fbo = o.m_Fbo; m_Format = o.m_Format;
    m_Width = o.m_Width; m_Height = o.m_Height; m_Levels = o.m_Levels;
    m_Linear = o.m_Linear; m_Complete = o.m_Complete;
    o.m_Tex = o.m_Fbo = 0;
    o.m_Width = o.m_Height = o.m_Levels = 0;
    o.m_Complete = false;
    return *this;
}

void ColorTarget::Release() {
    if (m_Fbo) glDeleteFramebuffers(1, &m_Fbo);
    if (m_Tex) glDeleteTextures(1, &m_Tex);
    m_Fbo = m_Tex = 0;
    m_Width = m_Height = m_Levels = 0;
    m_Complete = false;
}

bool ColorTarget::Ensure(int width, int height, unsigned int format, const char* name, bool linear, int levels) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (levels <= 0) {
        levels = 1;
        while (std::max(width, height) >> levels) ++levels;
    }
    if (m_Fbo && width == m_Width && height == m_Height && format == m_Format && linear == m_Linear && levels == m_Levels)
        return IsValid();

    Release();
    m_Width = width;
    m_Height = height;
    m_Format = format;
    m_Linear = linear;
    m_Levels = levels;

    // Direct State Access: nothing the caller has bound is disturbed.
    glCreateTextures(GL_TEXTURE_2D, 1, &m_Tex);
    glTextureStorage2D(m_Tex, levels, format, width, height);
    const GLenum minFilter = levels > 1 ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST)
                                        : (linear ? GL_LINEAR : GL_NEAREST);
    glTextureParameteri(m_Tex, GL_TEXTURE_MIN_FILTER, (GLint)minFilter);
    glTextureParameteri(m_Tex, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateFramebuffers(1, &m_Fbo);
    glNamedFramebufferTexture(m_Fbo, GL_COLOR_ATTACHMENT0, m_Tex, 0);
    m_Complete = GLFramebufferCheck::Complete(name, m_Fbo, width, height);
    return IsValid();
}

void ColorTarget::Bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    glViewport(0, 0, m_Width, m_Height);
}

namespace {
int s_Depth = 0;           // live FullscreenPass scopes (single GL context, one thread)
unsigned int s_Vao = 0;    // the shared empty vertex array; lives as long as the GL context
} // namespace

FullscreenPass::FullscreenPass() : m_Outer(s_Depth++ == 0) {
    if (m_Outer) {
        m_DepthTest = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
        m_Blend     = glIsEnabled(GL_BLEND) == GL_TRUE;
        m_CullFace  = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
        m_Scissor   = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
        GLint mask = 1;
        glGetIntegerv(GL_DEPTH_WRITEMASK, &mask);
        m_DepthMask = mask != 0;
    }
    // Set even inside an outer scope: a pass in between (bloom's additive upsample) may have changed them.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    if (!s_Vao) glCreateVertexArrays(1, &s_Vao); // core GL draws nothing without a vertex array bound
    GLStateCache::BindVertexArray(s_Vao);
}

FullscreenPass::~FullscreenPass() {
    --s_Depth;
    if (m_Outer) {
        auto set = [](GLenum cap, bool on) { if (on) glEnable(cap); else glDisable(cap); };
        set(GL_DEPTH_TEST, m_DepthTest);
        set(GL_BLEND, m_Blend);
        set(GL_CULL_FACE, m_CullFace);
        set(GL_SCISSOR_TEST, m_Scissor);
        glDepthMask(m_DepthMask ? GL_TRUE : GL_FALSE);
        GLStateCache::BindVertexArray(0);
    }
    GLStateCache::Invalidate();
}

void FullscreenPass::Draw() { glDrawArrays(GL_TRIANGLES, 0, 3); }
