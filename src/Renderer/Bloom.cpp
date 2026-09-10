#include "Bloom.h"
#include "Shader.h"
#include "gl.h"
#include <glm/glm.hpp>
#include <algorithm>

// GL_ONE — not exposed by the engine's minimal gl.h loader (only GL_SRC_ALPHA /
// GL_ONE_MINUS_SRC_ALPHA are defined there); needed for additive (GL_ONE, GL_ONE) blending
// when accumulating the upsample pyramid.
#ifndef GL_ONE
#define GL_ONE 1
#endif

Bloom::~Bloom() { Release(); }

void Bloom::Release() {
    if (m_Vao) { glDeleteVertexArrays(1, &m_Vao); m_Vao = 0; }
    for (auto& mip : m_Mips) {
        if (mip.Fbo) { glDeleteFramebuffers(1, &mip.Fbo); mip.Fbo = 0; }
        if (mip.Tex) { glDeleteTextures(1, &mip.Tex); mip.Tex = 0; }
        mip.W = mip.H = 0;
    }
    m_Width = m_Height = 0;
}

void Bloom::Create(int width, int height) {
    m_Width  = width;
    m_Height = height;

    const GLenum kColor0 = GL_COLOR_ATTACHMENT0;
    int w = std::max(width / 2, 1), h = std::max(height / 2, 1);
    for (int i = 0; i < kMipCount; ++i) {
        m_Mips[i].W = w;
        m_Mips[i].H = h;
        glCreateTextures(GL_TEXTURE_2D, 1, &m_Mips[i].Tex);
        glTextureStorage2D(m_Mips[i].Tex, 1, GL_RGBA16F, w, h);
        glTextureParameteri(m_Mips[i].Tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_Mips[i].Tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_Mips[i].Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Mips[i].Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCreateFramebuffers(1, &m_Mips[i].Fbo);
        glNamedFramebufferTexture(m_Mips[i].Fbo, GL_COLOR_ATTACHMENT0, m_Mips[i].Tex, 0);
        glNamedFramebufferDrawBuffers(m_Mips[i].Fbo, 1, &kColor0);
        w = std::max(w / 2, 1);
        h = std::max(h / 2, 1);
    }

    glGenVertexArrays(1, &m_Vao);
}

void Bloom::Resize(int width, int height) {
    if (m_Mips[0].Fbo != 0 && width == m_Width && height == m_Height) return;
    Release();
    Create(width, height);
}

void Bloom::Compute(Shader& threshShader, Shader& downsampleShader, Shader& upsampleShader,
                    unsigned int hdrTex, float threshold, float knee) {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_Vao);

    // Pass 1 — soft-knee threshold: extract above-threshold HDR energy into mip 0.
    glBindFramebuffer(GL_FRAMEBUFFER, m_Mips[0].Fbo);
    glViewport(0, 0, m_Mips[0].W, m_Mips[0].H);
    threshShader.Bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdrTex);
    threshShader.SetInt("uHdr", 0);
    threshShader.SetFloat("uThreshold", threshold);
    threshShader.SetFloat("uKnee", knee);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Pass 2 — downsample chain: each mip is a 4-tap box filter of the previous, larger mip.
    downsampleShader.Bind();
    downsampleShader.SetInt("uSrc", 0);
    for (int i = 1; i < kMipCount; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_Mips[i].Fbo);
        glViewport(0, 0, m_Mips[i].W, m_Mips[i].H);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_Mips[i - 1].Tex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Pass 3 — upsample chain: walk back up, tent-filtering each mip and additively blending it
    // into the next larger one (which still holds its own downsampled contents from pass 2).
    upsampleShader.Bind();
    upsampleShader.SetInt("uSrc", 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    for (int i = kMipCount - 1; i >= 1; --i) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_Mips[i - 1].Fbo);
        glViewport(0, 0, m_Mips[i - 1].W, m_Mips[i - 1].H);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_Mips[i].Tex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glDisable(GL_BLEND);

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
