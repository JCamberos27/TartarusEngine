#include "Upscaler.h"
#include "GLFramebufferCheck.h"
#include "GLStateCache.h"
#include "Log.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <string>

Upscaler::Upscaler() = default;

Upscaler::~Upscaler() {
    if (m_SharpFbo) glDeleteFramebuffers(1, &m_SharpFbo);
    if (m_SharpTex) glDeleteTextures(1, &m_SharpTex);
    if (m_Vao) glDeleteVertexArrays(1, &m_Vao);
}

void Upscaler::InternalSize(int w, int h, int targetHeight, int& outW, int& outH) {
    outW = w > 0 ? w : 1;
    outH = h > 0 ? h : 1;
    if (targetHeight <= 0 || outH <= targetHeight) return;
    outW = std::max(1, (int)std::lround((double)outW * targetHeight / outH));
    outH = targetHeight;
}

bool Upscaler::EnsureCreated() {
    if (m_Failed) return false;
    if (m_Upscale) return true;
    try {
        m_Rcas = std::make_unique<Shader>(ShaderLibrary::ReadFile("ChannelPreview.vert.glsl"),
                                          ShaderLibrary::ReadFile("Rcas.frag.glsl"), "Rcas");
        m_Upscale = std::make_unique<Shader>(ShaderLibrary::ReadFile("ChannelPreview.vert.glsl"),
                                             ShaderLibrary::ReadFile("Upscale.frag.glsl"), "Upscale");
    } catch (const std::exception& e) {
        Log::Error(std::string("Upscaler: ") + e.what());
        m_Failed = true;
        return false;
    }
    glGenVertexArrays(1, &m_Vao);
    return true;
}

void Upscaler::Apply(unsigned int srcTex, int srcW, int srcH, unsigned int dstFbo,
                     int dstX, int dstY, int dstW, int dstH, float sharpness) {
    if (srcTex == 0 || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 || !EnsureCreated()) return;

    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_Vao);

    unsigned int src = srcTex;
    if (sharpness > 0.0f) {
        if (m_SharpW != srcW || m_SharpH != srcH || !m_SharpTex) {
            if (m_SharpFbo) glDeleteFramebuffers(1, &m_SharpFbo);
            if (m_SharpTex) glDeleteTextures(1, &m_SharpTex);
            glCreateTextures(GL_TEXTURE_2D, 1, &m_SharpTex);
            glTextureStorage2D(m_SharpTex, 1, GL_RGB8, srcW, srcH);
            glTextureParameteri(m_SharpTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(m_SharpTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(m_SharpTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(m_SharpTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glCreateFramebuffers(1, &m_SharpFbo);
            glNamedFramebufferTexture(m_SharpFbo, GL_COLOR_ATTACHMENT0, m_SharpTex, 0);
            GLFramebufferCheck::Complete("Upscaler sharpen", m_SharpFbo, srcW, srcH);
            m_SharpW = srcW;
            m_SharpH = srcH;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_SharpFbo);
        glViewport(0, 0, srcW, srcH);
        m_Rcas->Bind();
        m_Rcas->SetInt("uSrc", 0);
        m_Rcas->SetFloat("uSharpness", sharpness);
        glBindTextureUnit(0, srcTex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        src = m_SharpTex;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(dstX, dstY, dstW, dstH);
    m_Upscale->Bind();
    m_Upscale->SetInt("uSrc", 0);
    m_Upscale->SetVec2("uSrcSize", glm::vec2((float)srcW, (float)srcH));
    m_Upscale->SetVec2("uDstSize", glm::vec2((float)dstW, (float)dstH));
    m_Upscale->SetVec2("uDstOffset", glm::vec2((float)dstX, (float)dstY));
    glBindTextureUnit(0, src); // linear + clamp-to-edge (Framebuffer and m_SharpTex both are)
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    GLStateCache::Invalidate();
}
