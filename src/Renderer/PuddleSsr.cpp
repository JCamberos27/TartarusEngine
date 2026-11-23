#include "PuddleSsr.h"
#include "HdrTarget.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "Core/Profiler.h"
#include "gl.h"
#include "World.h"

#include <algorithm>

PuddleSsr::PuddleSsr() = default;
PuddleSsr::~PuddleSsr() { Release(); }

void PuddleSsr::Release() {
    for (auto& [t, v] : m_Views)
        if (v.Tex) glDeleteTextures(1, &v.Tex);
    m_Views.clear();
    m_Program.reset();
}

void PuddleSsr::WarmUp() {
    if (!m_Program) m_Program = std::make_unique<Shader>(ShaderLibrary::ReadFileRequired("PuddleSsr.comp.glsl"), "PuddleSsr");
}

unsigned int PuddleSsr::Prepare(const HdrTarget* target) {
    if (!target || !target->IsValid()) return 0;
    ++m_Tick;
    for (auto it = m_Views.begin(); it != m_Views.end();) { // views gone (a closed tab, a destroyed target)
        if (it->first != target && m_Tick - it->second.LastUsed > 600) {
            if (it->second.Tex) glDeleteTextures(1, &it->second.Tex);
            it = m_Views.erase(it);
        } else {
            ++it;
        }
    }
    View& v = m_Views[target];
    v.LastUsed = m_Tick;
    if (v.W != target->Width() || v.H != target->Height() || !v.Tex) {
        if (v.Tex) glDeleteTextures(1, &v.Tex);
        v.W = target->Width();
        v.H = target->Height();
        glCreateTextures(GL_TEXTURE_2D, 1, &v.Tex);
        glTextureStorage2D(v.Tex, 1, GL_RGBA16F, std::max(1, (v.W + 1) / 2), std::max(1, (v.H + 1) / 2));
        glTextureParameteri(v.Tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(v.Tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(v.Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(v.Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        v.Frames = 0;
    }
    return v.Frames > 0 ? v.Tex : 0u;
}

void PuddleSsr::Trace(const HdrTarget* target, unsigned int depthTex, const glm::mat4& view, const glm::mat4& proj,
                      const World& world) {
    auto it = m_Views.find(target);
    if (it == m_Views.end() || !depthTex) return;
    View& v = it->second;
    const glm::mat4 viewProj = proj * view;
    if (v.Frames > 0) {
        PROFILE_GPU_SCOPE("Puddle SSR");
        WarmUp();
        const unsigned int prevColor = target->PreviousColorTexture(); // before Bind: its first call unbinds programs
        Shader& p = *m_Program;
        p.Bind();
        p.SetMat4("uProj", proj);
        p.SetMat4("uInvProj", glm::inverse(proj));
        p.SetMat4("uInvView", glm::inverse(view));
        p.SetMat4("uPrevViewProj", v.PrevViewProj);
        p.SetFloat("uPuddleAmount", world.Render.PuddleAmount);
        p.SetFloat("uPuddleScale", world.Render.PuddleScale);
        p.SetInt("uSteps", kSteps);
        p.SetFloat("uMaxDistance", kMaxDistance);
        p.SetInt("uFrame", ++m_Frame);
        glBindTextureUnit(32, depthTex); // TextureUnits::SsrDepth / SsrPrevColor: no draw uses them
        glBindTextureUnit(33, prevColor); // last frame's colour (uPrevColor)
        glBindImageTexture(0, v.Tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        const int w = std::max(1, (v.W + 1) / 2), h = std::max(1, (v.H + 1) / 2);
        glDispatchCompute((unsigned)((w + 7) / 8), (unsigned)((h + 7) / 8), 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glBindTextureUnit(32, 0);
        glBindTextureUnit(33, 0);
        GLStateCache::Invalidate();
    }
    v.PrevViewProj = viewProj;
    ++v.Frames;
}
