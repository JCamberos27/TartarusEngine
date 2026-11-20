#include "Tonemapper.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

#include <algorithm>
#include <cmath>

Tonemapper::Tonemapper() = default;
Tonemapper::~Tonemapper() = default;

namespace {

std::unique_ptr<Shader> MakeFullscreenShader(const char* frag) {
    return std::make_unique<Shader>(ShaderLibrary::ReadFile("Fullscreen.vert.glsl"), ShaderLibrary::ReadFile(frag));
}

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

} // namespace

void Tonemapper::EnsureCreated() {
    if (m_Shader) return;
    m_Shader = MakeFullscreenShader("Tonemapper.frag.glsl");
    m_Fxaa = MakeFullscreenShader("Fxaa.frag.glsl");
}

unsigned int Tonemapper::UpdateAutoExposure(unsigned int srcHdrTexture, const PostSettings& post) {
    const int slot = std::clamp(post.ExposureSlot, 0, kExposureSlots - 1);
    if (!m_LumShader) {
        m_LumShader = MakeFullscreenShader("AutoExposureLum.frag.glsl");
        m_AdaptShader = MakeFullscreenShader("AutoExposureAdapt.frag.glsl");
        m_Lum.Ensure(kLumSize, kLumSize, GL_R16F, "Tonemapper luminance target", true, 0);
        for (int s = 0; s < kExposureSlots; ++s)
            for (int i = 0; i < 2; ++i) m_Ev[s][i].Ensure(1, 1, GL_R32F, "Tonemapper exposure target", false);
    }

    // 1) log2 luminance into 64x64, then let the mip chain average it down to 1x1.
    m_Lum.Bind();
    m_LumShader->Bind();
    GLStateCache::BindTexture2D(0, srcHdrTexture);
    m_LumShader->SetInt("uHdr", 0);
    m_LumShader->SetVec2("uTexel", glm::vec2(1.0f / kLumSize));
    FullscreenPass::Draw();
    glGenerateTextureMipmap(m_Lum.Texture());

    // 2) Ease this view's EV toward the metered target (ping-pong between two 1x1 texels).
    const int prev = m_EvCur[slot], next = 1 - prev;
    m_Ev[slot][next].Bind();
    m_AdaptShader->Bind();
    GLStateCache::BindTexture2D(0, m_Lum.Texture());
    GLStateCache::BindTexture2D(1, m_Ev[slot][prev].Texture());
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
    FullscreenPass::Draw();
    m_EvCur[slot] = next;
    m_EvValid[slot] = true;
    return m_Ev[slot][next].Texture();
}

unsigned int Tonemapper::ApplyDepthOfField(unsigned int srcHdrTexture, int w, int h, const PostSettings& post) {
    if (!m_DofShader) m_DofShader = MakeFullscreenShader("DepthOfField.frag.glsl");
    m_Dof.Ensure(w, h, GL_RGBA16F, "Tonemapper depth of field");
    m_Dof.Bind();
    m_DofShader->Bind();
    GLStateCache::BindTexture2D(0, srcHdrTexture);
    GLStateCache::BindTexture2D(1, post.DepthTexture);
    m_DofShader->SetInt("uHdr", 0);
    m_DofShader->SetInt("uDepth", 1);
    m_DofShader->SetFloat("uProjA", post.ProjA);
    m_DofShader->SetFloat("uProjB", post.ProjB);
    m_DofShader->SetInt("uOrtho", post.Ortho ? 1 : 0);
    m_DofShader->SetFloat("uFocus", std::max(post.FocusDistance, 0.0f));
    m_DofShader->SetFloat("uRange", std::max(post.FocusRange, 0.01f));
    m_DofShader->SetFloat("uMaxRadius", std::clamp(post.MaxBlur, 0.0f, 32.0f) * (float)h / 1080.0f);
    m_DofShader->SetVec2("uTexel", glm::vec2(1.0f / (float)w, 1.0f / (float)h));
    FullscreenPass::Draw();
    return m_Dof.Texture();
}

void Tonemapper::Apply(unsigned int srcHdrTexture, unsigned int dstFbo, int dstW, int dstH, const PostSettings& post) {
    EnsureCreated();
    FullscreenPass pass;

    // #162 - with FXAA the tone-mapped image goes to an intermediate first, then FXAA resolves
    // it into the real destination.
    if (post.DepthOfField && post.DepthTexture && post.MaxBlur > 0.0f && dstW > 0 && dstH > 0)
        srcHdrTexture = ApplyDepthOfField(srcHdrTexture, dstW, dstH, post);

    unsigned int adaptedEv = 0;
    if (post.AutoExposure) adaptedEv = UpdateAutoExposure(srcHdrTexture, post);
    else m_EvValid[std::clamp(post.ExposureSlot, 0, kExposureSlots - 1)] = false; // re-enabling snaps

    const bool fxaa = post.Fxaa && dstW > 0 && dstH > 0;
    if (fxaa) m_Ldr.Ensure(dstW, dstH, GL_RGB8, "Tonemapper FXAA input");
    glBindFramebuffer(GL_FRAMEBUFFER, fxaa ? m_Ldr.Fbo() : dstFbo);
    glViewport(0, 0, dstW, dstH);

    m_Shader->Bind();
    GLStateCache::BindTexture2D(0, srcHdrTexture);
    m_Shader->SetInt("uHdr", 0);
    m_Shader->SetFloat("uExposure", std::pow(2.0f, post.ExposureEV));
    m_Shader->SetInt("uOperator", std::clamp(post.Operator, 0, 2));
    m_Shader->SetInt("uAutoExposure", adaptedEv ? 1 : 0);
    if (adaptedEv) GLStateCache::BindTexture2D(2, adaptedEv);
    m_Shader->SetInt("uAdaptedEv", 2);

    // PR16 - bloom: glow texture on unit 1; the shader adds it before the tone curve.
    const bool bloomOn = post.BloomTexture != 0 && post.BloomIntensity > 0.0f;
    if (bloomOn) {
        GLStateCache::BindTexture2D(1, post.BloomTexture);
        m_Shader->SetInt("uBloom", 1);
        m_Shader->SetFloat("uBloomIntensity", post.BloomIntensity);
    }
    m_Shader->SetInt("uBloomEnabled", bloomOn ? 1 : 0);

    // #162 - grading, dither.
    float wb[3];
    WhiteBalanceGains(std::clamp(post.Temperature, -100.0f, 100.0f), std::clamp(post.Tint, -100.0f, 100.0f), wb);
    m_Shader->SetVec3("uWhiteBalance", glm::vec3(wb[0], wb[1], wb[2]));
    m_Shader->SetVec3("uColorFilter", glm::vec3(post.ColorFilter[0], post.ColorFilter[1], post.ColorFilter[2]));
    m_Shader->SetFloat("uContrast", 1.0f + std::clamp(post.Contrast, -100.0f, 100.0f) / 100.0f);
    m_Shader->SetFloat("uSaturation", 1.0f + std::clamp(post.Saturation, -100.0f, 100.0f) / 100.0f);
    m_Shader->SetInt("uDither", post.Dither ? 1 : 0);
    FullscreenPass::Draw();

    if (fxaa) {
        glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
        glViewport(0, 0, dstW, dstH);
        m_Fxaa->Bind();
        GLStateCache::BindTexture2D(0, m_Ldr.Texture());
        m_Fxaa->SetInt("uLdr", 0);
        m_Fxaa->SetVec2("uTexel", glm::vec2(1.0f / dstW, 1.0f / dstH));
        FullscreenPass::Draw();
    }
}
