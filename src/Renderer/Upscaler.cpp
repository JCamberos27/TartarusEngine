#include "Upscaler.h"
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

Upscaler::~Upscaler() = default;

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
        m_Rcas = std::make_unique<Shader>(ShaderLibrary::ReadFile("Fullscreen.vert.glsl"),
                                          ShaderLibrary::ReadFile("Rcas.frag.glsl"), "Rcas");
        m_Upscale = std::make_unique<Shader>(ShaderLibrary::ReadFile("Fullscreen.vert.glsl"),
                                             ShaderLibrary::ReadFile("Upscale.frag.glsl"), "Upscale");
    } catch (const std::exception& e) {
        Log::Error(std::string("Upscaler: ") + e.what());
        m_Failed = true;
        return false;
    }
    return true;
}

void Upscaler::Apply(unsigned int srcTex, int srcW, int srcH, unsigned int dstFbo,
                     int dstX, int dstY, int dstW, int dstH, float sharpness) {
    if (srcTex == 0 || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 || !EnsureCreated()) return;

    FullscreenPass pass;
    unsigned int src = srcTex;
    if (sharpness > 0.0f) {
        m_Sharp.Ensure(srcW, srcH, GL_RGB8, "Upscaler sharpen");
        m_Sharp.Bind();
        m_Rcas->Bind();
        m_Rcas->SetInt("uSrc", 0);
        m_Rcas->SetFloat("uSharpness", sharpness);
        glBindTextureUnit(0, srcTex);
        FullscreenPass::Draw();
        src = m_Sharp.Texture();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(dstX, dstY, dstW, dstH);
    m_Upscale->Bind();
    m_Upscale->SetInt("uSrc", 0);
    m_Upscale->SetVec2("uSrcSize", glm::vec2((float)srcW, (float)srcH));
    m_Upscale->SetVec2("uDstSize", glm::vec2((float)dstW, (float)dstH));
    m_Upscale->SetVec2("uDstOffset", glm::vec2((float)dstX, (float)dstY));
    glBindTextureUnit(0, src); // linear + clamp-to-edge (Framebuffer and m_Sharp both are)
    FullscreenPass::Draw();
}
