#include "OpaqueColorCopy.h"
#include "../extern/glloader/gl.h"

OpaqueColorCopy::~OpaqueColorCopy() {
    if (m_ReadFbo) glDeleteFramebuffers(1, &m_ReadFbo);
    if (m_DrawFbo) glDeleteFramebuffers(1, &m_DrawFbo);
    if (m_Tex)     glDeleteTextures(1, &m_Tex);
}

void OpaqueColorCopy::CopyFrom(unsigned int srcTex, int width, int height) {
    if (width <= 0 || height <= 0 || !srcTex) return;

    if (m_Width != width || m_Height != height || !m_Tex) {
        if (m_Tex) glDeleteTextures(1, &m_Tex);
        int maxDim = width > height ? width : height;
        int numMips = 1;
        while ((maxDim >> numMips) > 0) ++numMips;

        glCreateTextures(GL_TEXTURE_2D, 1, &m_Tex);
        glTextureStorage2D(m_Tex, numMips, GL_RGBA16F, width, height);
        glTextureParameteri(m_Tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(m_Tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_Width  = width;
        m_Height = height;
    }

    // Lazy-create the blit FBOs.
    if (!m_ReadFbo) glCreateFramebuffers(1, &m_ReadFbo);
    if (!m_DrawFbo) glCreateFramebuffers(1, &m_DrawFbo);

    // Attach the source texture as the read color attachment.
    glNamedFramebufferTexture(m_ReadFbo, GL_COLOR_ATTACHMENT0, srcTex, 0);
    // Attach mip 0 of the destination as the draw color attachment.
    glNamedFramebufferTexture(m_DrawFbo, GL_COLOR_ATTACHMENT0, m_Tex, 0);

    glBlitNamedFramebuffer(m_ReadFbo, m_DrawFbo,
                           0, 0, width, height,
                           0, 0, width, height,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glGenerateTextureMipmap(m_Tex);
}
