#include "Tonemapper.h"
#include "Framebuffer.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

#include <algorithm>
#include <cmath>

#ifndef GL_R32F
#define GL_R32F 0x822E
#endif
#ifndef GL_R16F
#define GL_R16F 0x822D
#endif

Tonemapper::~Tonemapper() {
    delete m_Shader;
    delete m_Fxaa;
    delete m_Ldr;
    delete m_LumShader;
    delete m_AdaptShader;
    if (m_Vao) glDeleteVertexArrays(1, &m_Vao);
    if (m_LumTex) glDeleteTextures(1, &m_LumTex);
    if (m_LumFbo) glDeleteFramebuffers(1, &m_LumFbo);
    for (int s = 0; s < kExposureSlots; ++s) {
        if (m_EvTex[s][0]) glDeleteTextures(2, m_EvTex[s]);
        if (m_EvFbo[s][0]) glDeleteFramebuffers(2, m_EvFbo[s]);
    }
}

void Tonemapper::EnsureCreated() {
    if (m_Shader) return;
    m_Shader = new Shader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"),
                          ShaderLibrary::ReadFile("Tonemapper.frag.glsl"));
    m_Fxaa = new Shader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"),
                        ShaderLibrary::ReadFile("Fxaa.frag.glsl"));
    m_Ldr = new Framebuffer();
    glGenVertexArrays(1, &m_Vao); // attribute-less; positions come from gl_VertexID
}

namespace {

// #162 - Unity's white balance (ColorUtils.ComputeColorBalance): Temperature/Tint shift the
// white point along/across the Planckian locus; the result is a per-channel LMS gain.
void WhiteBalanceGains(float temperature, float tint, float out[3]) {
    const float t1 = temperature / 60.0f;
    const float t2 = tint / 60.0f;
    const float x = 0.31271f - t1 * (t1 < 0.0f ? 0.1f : 0.05f);
    const float standardIlluminantY = 2.87f * x - 3.0f * x * x - 0.27509507f;
    const float y = standardIlluminantY + t2 * 0.05f;

    auto xyToLms = [](float cx, float cy, float lms[3]) {
        const float Y = 1.0f;
        const float X = Y * cx / cy;
        const float Z = Y * (1.0f - cx - cy) / cy;
        lms[0] = 0.7328f * X + 0.4296f * Y - 0.1624f * Z;
        lms[1] = -0.7036f * X + 1.6975f * Y + 0.0061f * Z;
        lms[2] = 0.0030f * X + 0.0136f * Y + 0.9834f * Z;
    };
    const float w1[3] = {0.949237f, 1.03542f, 1.08728f}; // D65 white in LMS
    float w2[3];
    xyToLms(x, y, w2);
    for (int i = 0; i < 3; ++i) out[i] = w1[i] / w2[i];
}

constexpr int kLumSize = 64;   // metering resolution; its mip chain ends at 1x1
constexpr int kLumTopMip = 6;  // log2(kLumSize)

unsigned int MakeColorTarget(unsigned int& fbo, int size, GLenum internalFormat, bool mips) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, size, size, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mips ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (mips) glGenerateMipmap(GL_TEXTURE_2D); // allocate the chain once
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return tex;
}

} // namespace

unsigned int Tonemapper::UpdateAutoExposure(unsigned int srcHdrTexture, const PostSettings& post) {
    const int slot = std::clamp(post.ExposureSlot, 0, kExposureSlots - 1);
    if (!m_LumShader) {
        m_LumShader = new Shader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"),
                                 ShaderLibrary::ReadFile("AutoExposureLum.frag.glsl"));
        m_AdaptShader = new Shader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"),
                                   ShaderLibrary::ReadFile("AutoExposureAdapt.frag.glsl"));
        m_LumTex = MakeColorTarget(m_LumFbo, kLumSize, GL_R16F, true);
        for (int s = 0; s < kExposureSlots; ++s)
            for (int i = 0; i < 2; ++i) m_EvTex[s][i] = MakeColorTarget(m_EvFbo[s][i], 1, GL_R32F, false);
        GLStateCache::Invalidate(); // MakeColorTarget bound textures behind the cache's back
    }
    glBindVertexArray(m_Vao);

    // 1) log2 luminance into 64x64, then let the mip chain average it down to 1x1.
    glBindFramebuffer(GL_FRAMEBUFFER, m_LumFbo);
    glViewport(0, 0, kLumSize, kLumSize);
    m_LumShader->Bind();
    GLStateCache::BindTexture2D(0, srcHdrTexture);
    m_LumShader->SetInt("uHdr", 0);
    m_LumShader->SetVec2("uTexel", glm::vec2(1.0f / kLumSize));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glGenerateTextureMipmap(m_LumTex);

    // 2) Ease this view's EV toward the metered target (ping-pong between two 1x1 texels).
    const int prev = m_EvCur[slot], next = 1 - prev;
    glBindFramebuffer(GL_FRAMEBUFFER, m_EvFbo[slot][next]);
    glViewport(0, 0, 1, 1);
    m_AdaptShader->Bind();
    GLStateCache::BindTexture2D(0, m_LumTex);
    GLStateCache::BindTexture2D(1, m_EvTex[slot][prev]);
    m_AdaptShader->SetInt("uLum", 0);
    m_AdaptShader->SetFloat("uTopMip", (float)kLumTopMip);
    m_AdaptShader->SetInt("uPrev", 1);
    m_AdaptShader->SetInt("uReset", (!m_EvValid[slot] || post.DeltaTime <= 0.0f) ? 1 : 0);
    const float lo = std::min(post.AutoExposureMinEV, post.AutoExposureMaxEV);
    const float hi = std::max(post.AutoExposureMinEV, post.AutoExposureMaxEV);
    m_AdaptShader->SetFloat("uMinEv", lo);
    m_AdaptShader->SetFloat("uMaxEv", hi);
    m_AdaptShader->SetFloat("uSpeedUp", std::max(post.AutoExposureSpeedUp, 0.0f));
    m_AdaptShader->SetFloat("uSpeedDown", std::max(post.AutoExposureSpeedDown, 0.0f));
    m_AdaptShader->SetFloat("uDt", std::min(post.DeltaTime, 0.25f)); // a hitch shouldn't pop exposure
    glDrawArrays(GL_TRIANGLES, 0, 3);
    m_EvCur[slot] = next;
    m_EvValid[slot] = true;
    return m_EvTex[slot][next];
}

void Tonemapper::Apply(unsigned int srcHdrTexture, unsigned int dstFbo, int dstW, int dstH, const PostSettings& post) {
    EnsureCreated();

    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // #162 - with FXAA the tone-mapped image goes to an intermediate first, then FXAA resolves
    // it into the real destination.
    unsigned int adaptedEv = 0;
    if (post.AutoExposure) adaptedEv = UpdateAutoExposure(srcHdrTexture, post);
    else m_EvValid[std::clamp(post.ExposureSlot, 0, kExposureSlots - 1)] = false; // re-enabling snaps

    const bool fxaa = post.Fxaa && dstW > 0 && dstH > 0;
    if (fxaa) m_Ldr->Resize(dstW, dstH);
    glBindFramebuffer(GL_FRAMEBUFFER, fxaa ? m_Ldr->Handle() : dstFbo);
    glViewport(0, 0, dstW, dstH);

    m_Shader->Bind();
    GLStateCache::BindTexture2D(0, srcHdrTexture);
    m_Shader->SetInt("uHdr", 0);
    m_Shader->SetFloat("uExposure", std::pow(2.0f, post.ExposureEV));
    m_Shader->SetInt("uOperator", std::clamp(post.Operator, 0, 2));
    m_Shader->SetInt("uAutoExposure", adaptedEv ? 1 : 0);
    if (adaptedEv) GLStateCache::BindTexture2D(2, adaptedEv);
    m_Shader->SetInt("uAdaptedEv", 2);

    // PR16 — bloom: bind glow texture on unit 1; shader adds it before the tone curve.
    const bool bloomOn = post.BloomTexture != 0 && post.BloomIntensity > 0.0f;
    if (bloomOn) {
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, post.BloomTexture);
        glActiveTexture(GL_TEXTURE0);
        m_Shader->SetInt("uBloom", 1);
        m_Shader->SetFloat("uBloomIntensity", post.BloomIntensity);
    }
    m_Shader->SetInt("uBloomEnabled", bloomOn ? 1 : 0);

    // #162 - grading, vignette, dither.
    float wb[3];
    WhiteBalanceGains(std::clamp(post.Temperature, -100.0f, 100.0f), std::clamp(post.Tint, -100.0f, 100.0f), wb);
    m_Shader->SetVec3("uWhiteBalance", glm::vec3(wb[0], wb[1], wb[2]));
    m_Shader->SetVec3("uColorFilter", glm::vec3(post.ColorFilter[0], post.ColorFilter[1], post.ColorFilter[2]));
    m_Shader->SetFloat("uContrast", 1.0f + std::clamp(post.Contrast, -100.0f, 100.0f) / 100.0f);
    m_Shader->SetFloat("uSaturation", 1.0f + std::clamp(post.Saturation, -100.0f, 100.0f) / 100.0f);
    m_Shader->SetFloat("uVignette", std::clamp(post.VignetteIntensity, 0.0f, 1.0f));
    m_Shader->SetFloat("uVignetteSmoothness", std::clamp(post.VignetteSmoothness, 0.01f, 1.0f));
    m_Shader->SetFloat("uAspect", dstH > 0 ? (float)dstW / (float)dstH : 1.0f);
    m_Shader->SetInt("uDither", post.Dither ? 1 : 0);

    glBindVertexArray(m_Vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (fxaa) {
        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
        glViewport(0, 0, dstW, dstH);
        m_Fxaa->Bind();
        GLStateCache::BindTexture2D(0, m_Ldr->ColorTexture());
        m_Fxaa->SetInt("uLdr", 0);
        m_Fxaa->SetVec2("uTexel", glm::vec2(1.0f / dstW, 1.0f / dstH));
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glBindVertexArray(0);

    if (prevDepth) glEnable(GL_DEPTH_TEST);
    if (prevBlend) glEnable(GL_BLEND);

    // Shader::Bind goes through GLStateCache, but the raw VAO bind above and ImGui's later raw
    // GL calls mean the next cached bind shouldn't trust stale state.
    GLStateCache::Invalidate();
}
