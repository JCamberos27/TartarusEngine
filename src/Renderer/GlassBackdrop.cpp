#include "GlassBackdrop.h"

#include "GLStateCache.h"
#include "Log.h"
#include "Profiler.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <exception>
#include <string>

namespace {
constexpr int kGroup = 8; // local_size_x/y of the three Glass*.comp.glsl programs
unsigned int Groups(int n) { return (unsigned int)((n + kGroup - 1) / kGroup); }
int LevelSize(int base, int level) { return std::max(1, base >> level); }
} // namespace

GlassBackdrop::GlassBackdrop() = default;

GlassBackdrop::~GlassBackdrop() { Release(); }

void GlassBackdrop::ReleaseTargets() {
    if (m_DownTex) glDeleteTextures(1, &m_DownTex);
    if (m_UpTex) glDeleteTextures(1, &m_UpTex);
    if (m_Out) glDeleteTextures(1, &m_Out);
    m_DownTex = m_UpTex = m_Out = 0;
    m_W = m_H = m_Levels = 0;
}

void GlassBackdrop::Release() {
    ReleaseTargets();
    m_Down.reset();
    m_Up.reset();
    m_Resolve.reset();
    m_ProgramsFailed = false; // a later Update may try again (e.g. after a shader fix + hot reload)
}

bool GlassBackdrop::EnsurePrograms() {
    if (m_Down && m_Up && m_Resolve) return true;
    if (m_ProgramsFailed) return false;
    try {
        m_Down = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("GlassDownsample.comp.glsl"), "GlassDownsample");
        m_Up = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("GlassUpsample.comp.glsl"), "GlassUpsample");
        m_Resolve = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("GlassResolve.comp.glsl"), "GlassResolve");
    } catch (const std::exception& e) {
        // Logged once; the HUDs draw a plain translucent tint instead of frosted glass.
        Log::Error(std::string("Glass backdrop: compute shaders unavailable, frosted HUDs fall back to a flat tint. ") + e.what());
        m_Down.reset();
        m_Up.reset();
        m_Resolve.reset();
        m_ProgramsFailed = true;
        return false;
    }
    return true;
}

void GlassBackdrop::EnsureTargets(int w, int h, int levels) {
    if (w == m_W && h == m_H && levels == m_Levels && m_Out) return;
    ReleaseTargets();
    auto makeChain = [&](unsigned int& tex) {
        glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        glTextureStorage2D(tex, levels, GL_RGBA16F, w, h);
        // Sampled with textureLod at an explicit level: bilinear within it, never across levels.
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    makeChain(m_DownTex);
    makeChain(m_UpTex);
    glCreateTextures(GL_TEXTURE_2D, 1, &m_Out);
    glTextureStorage2D(m_Out, 1, GL_RGBA8, w, h);
    glTextureParameteri(m_Out, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_Out, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_Out, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_Out, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_W = w;
    m_H = h;
    m_Levels = levels;
}

unsigned int GlassBackdrop::Update(unsigned int srcTex, int width, int height, int levels, float saturation) {
    if (srcTex == 0 || width < 8 || height < 8) return 0;
    if (!EnsurePrograms()) return 0;
    PROFILE_GPU_SCOPE("Glass Backdrop");

    const int hw = std::max(1, width / 2), hh = std::max(1, height / 2);
    // Stop before the smallest level gets under 4 px on its short side: below that the tent filter
    // just smears the edge texels and the blur starts to "breathe" as the viewport resizes.
    int maxLevels = 1;
    while (maxLevels < 8 && std::min(hw, hh) >> maxLevels >= 4) ++maxLevels;
    levels = std::clamp(levels, 2, maxLevels);
    EnsureTargets(hw, hh, levels);

    const GLbitfield kBarrier = GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;

    // 1. Downsample: src (full res, encoded) -> D0 (half, linear) -> ... -> D(n-1).
    m_Down->Bind();
    for (int lv = 0; lv < levels; ++lv) {
        const bool first = lv == 0;
        const unsigned int src = first ? srcTex : m_DownTex;
        const int srcW = first ? width : LevelSize(hw, lv - 1);
        const int srcH = first ? height : LevelSize(hh, lv - 1);
        const int dstW = LevelSize(hw, lv), dstH = LevelSize(hh, lv);
        glBindTextureUnit(0, src);
        glBindImageTexture(0, m_DownTex, lv, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        m_Down->SetFloat("uSrcLod", first ? 0.0f : (float)(lv - 1));
        m_Down->SetVec2("uSrcTexel", glm::vec2(1.0f / (float)srcW, 1.0f / (float)srcH));
        m_Down->SetInt("uDecodeSrgb", first ? 1 : 0);
        glUniform2i(m_Down->Loc("uDstSize"), dstW, dstH);
        m_Down->DispatchCompute(Groups(dstW), Groups(dstH), 1);
        glMemoryBarrier(kBarrier);
    }

    // 2. Upsample: D(n-1) -> U(n-2) -> ... -> U0, tent-filtered. Reading one level of a texture
    // while writing a different level of it is well defined; no level is read and written at once.
    m_Up->Bind();
    m_Up->SetFloat("uRadius", 1.0f);
    for (int lv = levels - 2; lv >= 0; --lv) {
        const bool fromDown = lv == levels - 2;
        const unsigned int src = fromDown ? m_DownTex : m_UpTex;
        const int srcLv = lv + 1;
        glBindTextureUnit(0, src);
        glBindImageTexture(0, m_UpTex, lv, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        m_Up->SetFloat("uSrcLod", (float)srcLv);
        m_Up->SetVec2("uSrcTexel", glm::vec2(1.0f / (float)LevelSize(hw, srcLv), 1.0f / (float)LevelSize(hh, srcLv)));
        glUniform2i(m_Up->Loc("uDstSize"), LevelSize(hw, lv), LevelSize(hh, lv));
        m_Up->DispatchCompute(Groups(LevelSize(hw, lv)), Groups(LevelSize(hh, lv)), 1);
        glMemoryBarrier(kBarrier);
    }

    // 3. Resolve U0 -> Out (encoded RGBA8 for ImGui).
    m_Resolve->Bind();
    glBindTextureUnit(0, m_UpTex);
    glBindImageTexture(0, m_Out, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    m_Resolve->SetFloat("uSrcLod", 0.0f);
    m_Resolve->SetFloat("uSaturation", saturation);
    glUniform2i(m_Resolve->Loc("uDstSize"), hw, hh);
    m_Resolve->DispatchCompute(Groups(hw), Groups(hh), 1);
    // ImGui samples Out in a fragment shader later this frame.
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

    // Leave no bindings behind for the UI pass, and tell the binding cache it can't trust its view.
    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindTextureUnit(0, 0);
    glUseProgram(0);
    GLStateCache::Invalidate();
    return m_Out;
}
