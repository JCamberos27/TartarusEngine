#include "HdrTarget.h"
#include "Log.h"
#include "gl.h"

#include <algorithm>
#include <string>

namespace {
int MaxSamples() {
    static int s = []() {
        GLint m = 1;
        glGetIntegerv(GL_MAX_SAMPLES, &m);
        return m < 1 ? 1 : m;
    }();
    return s;
}
} // namespace

HdrTarget::~HdrTarget() { Release(); }

void HdrTarget::Release() {
    // Textures and framebuffers share the same delete entry points whether created via DSA or
    // not, so no special-casing here.
    if (m_ResolveFbo)   { glDeleteFramebuffers(1, &m_ResolveFbo); m_ResolveFbo = 0; }
    if (m_ResolveColor) { glDeleteTextures(1, &m_ResolveColor);   m_ResolveColor = 0; }
    if (m_ResolveDepth) { glDeleteTextures(1, &m_ResolveDepth);   m_ResolveDepth = 0; }
    if (m_MsFbo)        { glDeleteFramebuffers(1, &m_MsFbo);       m_MsFbo = 0; }
    if (m_MsColor)      { glDeleteTextures(1, &m_MsColor);         m_MsColor = 0; }
    if (m_MsDepth)      { glDeleteTextures(1, &m_MsDepth);         m_MsDepth = 0; }
}

void HdrTarget::Resize(int width, int height, int samples) {
    width  = width  > 0 ? width  : 1;
    height = height > 0 ? height : 1;
    samples = std::clamp(samples, 1, MaxSamples());
    if (m_MsFbo != 0 && width == m_Width && height == m_Height && samples == m_Samples) return;
    Create(width, height, samples);
}

void HdrTarget::Create(int width, int height, int samples) {
    Release();
    m_Width = width;
    m_Height = height;
    m_Samples = samples;

    // --- Multisample scene target: RGBA16F colour + 32F depth, both as textures so depth is
    //     readable later (SSAO / screen-space passes).
    glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &m_MsColor);
    glTextureStorage2DMultisample(m_MsColor, samples, GL_RGBA16F, width, height, GL_TRUE);

    glCreateTextures(GL_TEXTURE_2D_MULTISAMPLE, 1, &m_MsDepth);
    glTextureStorage2DMultisample(m_MsDepth, samples, GL_DEPTH_COMPONENT32F, width, height, GL_TRUE);

    glCreateFramebuffers(1, &m_MsFbo);
    glNamedFramebufferTexture(m_MsFbo, GL_COLOR_ATTACHMENT0, m_MsColor, 0);
    glNamedFramebufferTexture(m_MsFbo, GL_DEPTH_ATTACHMENT, m_MsDepth, 0);
    const GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glNamedFramebufferDrawBuffers(m_MsFbo, 1, &drawBuf);

    // --- Single-sample resolve target the tonemap pass samples.
    glCreateTextures(GL_TEXTURE_2D, 1, &m_ResolveColor);
    glTextureStorage2D(m_ResolveColor, 1, GL_RGBA16F, width, height);
    glTextureParameteri(m_ResolveColor, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_ResolveColor, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_ResolveColor, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_ResolveColor, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Single-sample depth in the same resolve FBO, so a screen-space pass after ResolveTo() has
    // a plain sampler2D depth to read instead of decoding sampler2DMS by hand (#121). NEAREST —
    // depth must not be linearly filtered.
    glCreateTextures(GL_TEXTURE_2D, 1, &m_ResolveDepth);
    glTextureStorage2D(m_ResolveDepth, 1, GL_DEPTH_COMPONENT32F, width, height);
    glTextureParameteri(m_ResolveDepth, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_ResolveDepth, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_ResolveDepth, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_ResolveDepth, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateFramebuffers(1, &m_ResolveFbo);
    glNamedFramebufferTexture(m_ResolveFbo, GL_COLOR_ATTACHMENT0, m_ResolveColor, 0);
    glNamedFramebufferTexture(m_ResolveFbo, GL_DEPTH_ATTACHMENT, m_ResolveDepth, 0);
    glNamedFramebufferDrawBuffers(m_ResolveFbo, 1, &drawBuf);

    GLenum ms = glCheckNamedFramebufferStatus(m_MsFbo, GL_FRAMEBUFFER);
    GLenum rs = glCheckNamedFramebufferStatus(m_ResolveFbo, GL_FRAMEBUFFER);
    if (ms != GL_FRAMEBUFFER_COMPLETE || rs != GL_FRAMEBUFFER_COMPLETE) {
        Log::Error("HdrTarget: incomplete framebuffer (" + std::to_string(width) + "x" +
                   std::to_string(height) + " x" + std::to_string(samples) + "  ms=0x" +
                   std::to_string(ms) + " resolve=0x" + std::to_string(rs) + ")");
        Release();
    }
}

void HdrTarget::BindForRender() const {
    glBindFramebuffer(GL_FRAMEBUFFER, m_MsFbo);
    glViewport(0, 0, m_Width, m_Height);
}

void HdrTarget::ResolveTo() const {
    if (!m_MsFbo || !m_ResolveFbo) return;
    // MSAA down-resolve (a plain copy when m_Samples == 1). Colour + depth: colour for the
    // tonemap pass, depth (#121) so any screen-space pass after this has a single-sample
    // sampler2D depth. Depth must use GL_NEAREST. m_MsDepth stays available too.
    glBlitNamedFramebuffer(m_MsFbo, m_ResolveFbo,
        0, 0, m_Width, m_Height,
        0, 0, m_Width, m_Height,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBlitNamedFramebuffer(m_MsFbo, m_ResolveFbo,
        0, 0, m_Width, m_Height,
        0, 0, m_Width, m_Height,
        GL_DEPTH_BUFFER_BIT, GL_NEAREST);
}
