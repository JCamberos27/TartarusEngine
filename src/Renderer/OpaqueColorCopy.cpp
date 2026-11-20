#include "GpuMemTrack.h"
#include "OpaqueColorCopy.h"
#include "GLFramebufferCheck.h"
#include "gl.h"

OpaqueColorCopy::~OpaqueColorCopy() {
    GpuMemTrack::Set(GpuMemTrack::Key("OpaqueColorCopy", this), 0);
    if (m_ReadFbo) glDeleteFramebuffers(1, &m_ReadFbo);
}

void OpaqueColorCopy::CopyFrom(unsigned int srcTex, int width, int height) {
    if (width <= 0 || height <= 0 || !srcTex) return;

    const bool sizeChanged = m_Copy.Width() != width || m_Copy.Height() != height || !m_Copy.Fbo();
    m_Copy.Ensure(width, height, GL_RGBA16F, "OpaqueColorCopy draw", true, 0); // attached once, at allocation
    if (sizeChanged)
        GpuMemTrack::Set(GpuMemTrack::Key("OpaqueColorCopy", this), (size_t)width * height * 8 * 4 / 3);

    if (!m_ReadFbo) glCreateFramebuffers(1, &m_ReadFbo);
    glNamedFramebufferTexture(m_ReadFbo, GL_COLOR_ATTACHMENT0, srcTex, 0);
    if (sizeChanged) // audit #358 — validate once per resize, not every frame
        GLFramebufferCheck::Complete("OpaqueColorCopy read", m_ReadFbo, width, height);

    glBlitNamedFramebuffer(m_ReadFbo, m_Copy.Fbo(),
                           0, 0, width, height,
                           0, 0, width, height,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glGenerateTextureMipmap(m_Copy.Texture());
}
