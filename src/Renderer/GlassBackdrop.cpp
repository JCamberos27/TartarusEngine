#include "GlassBackdrop.h"

#include "GLStateCache.h"
#include "Log.h"
#include "Profiler.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "gl.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
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

void GlassBackdrop::EnsureTargets(int w, int h, int levels, bool grow) {
    if (grow) {
        // UpdateRect: at least the rect, in 256-texel steps, and never smaller than what is there.
        auto step = [](int n) { return (n + 255) / 256 * 256; };
        w = std::max(step(w), m_W);
        h = std::max(step(h), m_H);
        levels = std::max(levels, m_Levels);
    }
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

unsigned int GlassBackdrop::Update(unsigned int srcTex, int width, int height, int levels, float saturation, float ceiling) {
    return Run(srcTex, width, height, 0, 0, width, height, levels, saturation, ceiling, false);
}

unsigned int GlassBackdrop::Reserve(int texW, int texH, int levels) {
    if (texW < 8 || texH < 8 || !EnsurePrograms()) return 0;
    EnsureTargets(std::max(1, texW / 2), std::max(1, texH / 2), std::clamp(levels, 2, 8), true);
    return m_Out;
}

int GlassBackdrop::BlurReach(int levels) {
    // In full-resolution px. Down: 2.5 source texels per level (the 13-tap filter's +-2 plus the bilinear
    // half), the sources 1, 2, 4 ... px. Up: 1.5 texels (the tent's 1 plus the bilinear half) of each source
    // level, 2^(levels) px down to 4 px. Resolve: half a 2 px texel.
    const int n = std::clamp(levels, 1, 8);
    const float down = 2.5f * (float)((1 << n) - 1);
    const float up = n >= 2 ? 1.5f * (float)((2 << n) - 4) : 0.0f;
    return (int)std::ceil(down + up + 1.0f);
}

unsigned int GlassBackdrop::UpdateRect(unsigned int srcTex, int texW, int texH, int x, int y, int w, int h, int levels,
                                       float saturation, float ceiling) {
    x = std::clamp(x, 0, std::max(texW - 1, 0));
    y = std::clamp(y, 0, std::max(texH - 1, 0));
    w = std::min(w, texW - x);
    h = std::min(h, texH - y);
    return Run(srcTex, texW, texH, x, y, w, h, levels, saturation, ceiling, true);
}

// Every pass maps its destination pixel to the same spot of its source in texels (origin + the pixel
// centre scaled by source / destination size) and divides by the source texture's own size, so a
// target larger than what is used (UpdateRect's grown chain) samples exactly as one cut to fit. Each
// tap is clamped to the texel centres of the part that holds data - the same as CLAMP_TO_EDGE when
// that part is the whole texture, which is what Update always has.
unsigned int GlassBackdrop::Run(unsigned int srcTex, int texW, int texH, int x, int y, int width, int height, int levels,
                                float saturation, float ceiling, bool grow) {
    if (srcTex == 0 || width < 8 || height < 8) return 0;
    if (!EnsurePrograms()) return 0;
    PROFILE_GPU_SCOPE("Glass Backdrop");

    const int hw = std::max(1, width / 2), hh = std::max(1, height / 2);
    // Stop before the smallest level gets under 4 px on its short side: below that the tent filter
    // just smears the edge texels and the blur starts to "breathe" as the viewport resizes.
    int maxLevels = 1;
    while (maxLevels < 8 && std::min(hw, hh) >> maxLevels >= 4) ++maxLevels;
    levels = std::clamp(levels, 2, maxLevels);
    EnsureTargets(hw, hh, levels, grow);
    m_RectX = x;
    m_RectY = y;

    const GLbitfield kBarrier = GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
    // A chain level's data covers its first LevelSize texels of a level LevelSize(m_W | m_H) wide.
    auto chainClamp = [&](int lv, int usedW, int usedH) {
        const float tw = (float)LevelSize(m_W, lv), th = (float)LevelSize(m_H, lv);
        return glm::vec4(0.5f / tw, 0.5f / th, ((float)usedW - 0.5f) / tw, ((float)usedH - 0.5f) / th);
    };
    auto chainTexel = [&](int lv) { return glm::vec2(1.0f / (float)LevelSize(m_W, lv), 1.0f / (float)LevelSize(m_H, lv)); };

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
        // The source is read inside the rect only (the whole texture for Update), a chain level inside its data.
        m_Down->SetVec2("uSrcTexel", first ? glm::vec2(1.0f / (float)texW, 1.0f / (float)texH) : chainTexel(lv - 1));
        m_Down->SetVec4("uSrcClamp", first ? glm::vec4(((float)x + 0.5f) / (float)texW, ((float)y + 0.5f) / (float)texH,
                                                       ((float)(x + width) - 0.5f) / (float)texW, ((float)(y + height) - 0.5f) / (float)texH)
                                           : chainClamp(lv - 1, srcW, srcH));
        m_Down->SetVec2("uSrcOrigin", first ? glm::vec2((float)x, (float)y) : glm::vec2(0.0f));
        m_Down->SetVec2("uSrcSize", glm::vec2((float)srcW, (float)srcH));
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
        const int srcW = LevelSize(hw, srcLv), srcH = LevelSize(hh, srcLv);
        glBindTextureUnit(0, src);
        glBindImageTexture(0, m_UpTex, lv, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        m_Up->SetFloat("uSrcLod", (float)srcLv);
        m_Up->SetVec2("uSrcTexel", chainTexel(srcLv));
        m_Up->SetVec4("uSrcClamp", chainClamp(srcLv, srcW, srcH));
        m_Up->SetVec2("uSrcSize", glm::vec2((float)srcW, (float)srcH));
        glUniform2i(m_Up->Loc("uDstSize"), LevelSize(hw, lv), LevelSize(hh, lv));
        m_Up->DispatchCompute(Groups(LevelSize(hw, lv)), Groups(LevelSize(hh, lv)), 1);
        glMemoryBarrier(kBarrier);
    }

    // 3. Resolve U0 -> Out (encoded RGBA8 for ImGui).
    m_Resolve->Bind();
    glBindTextureUnit(0, m_UpTex);
    glBindImageTexture(0, m_Out, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    m_Resolve->SetFloat("uSrcLod", 0.0f);
    m_Resolve->SetVec2("uSrcTexel", chainTexel(0));
    m_Resolve->SetVec4("uSrcClamp", chainClamp(0, hw, hh));
    m_Resolve->SetFloat("uSaturation", saturation);
    m_Resolve->SetFloat("uCeiling", std::clamp(ceiling, 0.05f, 1.0f));
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
