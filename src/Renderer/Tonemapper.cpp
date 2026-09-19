#include "Tonemapper.h"
#include "Framebuffer.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

#include <algorithm>
#include <cmath>

Tonemapper::~Tonemapper() {
    delete m_Shader;
    delete m_Fxaa;
    delete m_Ldr;
    if (m_Vao) glDeleteVertexArrays(1, &m_Vao);
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

} // namespace

void Tonemapper::Apply(unsigned int srcHdrTexture, unsigned int dstFbo, int dstW, int dstH, const PostSettings& post) {
    EnsureCreated();

    GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // #162 - with FXAA the tone-mapped image goes to an intermediate first, then FXAA resolves
    // it into the real destination.
    const bool fxaa = post.Fxaa && dstW > 0 && dstH > 0;
    if (fxaa) m_Ldr->Resize(dstW, dstH);
    glBindFramebuffer(GL_FRAMEBUFFER, fxaa ? m_Ldr->Handle() : dstFbo);
    glViewport(0, 0, dstW, dstH);

    m_Shader->Bind();
    GLStateCache::BindTexture2D(0, srcHdrTexture);
    m_Shader->SetInt("uHdr", 0);
    m_Shader->SetFloat("uExposure", std::pow(2.0f, post.ExposureEV));
    m_Shader->SetInt("uOperator", std::clamp(post.Operator, 0, 2));

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
