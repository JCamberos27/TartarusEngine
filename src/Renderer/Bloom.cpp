#include "Bloom.h"
#include "GLStateScope.h"
#include "Shader.h"
#include "gl.h"
#include <algorithm>

void Bloom::Resize(int width, int height) {
    // Mip 0 is half the source; ColorTarget::Ensure is a no-op while the sizes hold.
    int w = std::max(width / 2, 1), h = std::max(height / 2, 1);
    m_Valid = true;
    for (ColorTarget& mip : m_Mips) {
        if (!mip.Ensure(w, h, GL_RGBA16F, "Bloom mip")) m_Valid = false; // audit #358
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
    }
}

void Bloom::Compute(Shader& threshShader, Shader& downsampleShader, Shader& upsampleShader,
                    unsigned int hdrTex, float threshold, float knee) {
    FullscreenPass pass; // #160 - depth/blend/cull back as the caller had them afterwards

    // Pass 1 — soft-knee threshold: extract above-threshold HDR energy into mip 0.
    m_Mips[0].Bind();
    threshShader.Bind();
    glBindTextureUnit(0, hdrTex);
    threshShader.SetInt("uHdr", 0);
    threshShader.SetFloat("uThreshold", threshold);
    threshShader.SetFloat("uKnee", knee);
    FullscreenPass::Draw();

    // Pass 2 — downsample chain: each mip is a 4-tap box filter of the previous, larger mip.
    downsampleShader.Bind();
    downsampleShader.SetInt("uSrc", 0);
    for (int i = 1; i < kMipCount; ++i) {
        m_Mips[i].Bind();
        glBindTextureUnit(0, m_Mips[i - 1].Texture());
        FullscreenPass::Draw();
    }

    // Pass 3 — upsample chain: walk back up, tent-filtering each mip and additively blending it
    // into the next larger one (which still holds its own downsampled contents from pass 2).
    // The caller's blend function comes back afterwards (it used to be left at ONE, ONE).
    GLBlendFuncScope blendFunc;
    upsampleShader.Bind();
    upsampleShader.SetInt("uSrc", 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    for (int i = kMipCount - 1; i >= 1; --i) {
        m_Mips[i - 1].Bind();
        glBindTextureUnit(0, m_Mips[i].Texture());
        FullscreenPass::Draw();
    }
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
