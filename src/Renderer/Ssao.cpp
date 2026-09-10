#include "Ssao.h"
#include "GLFramebufferCheck.h"
#include "Shader.h"
#include "gl.h"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <string>

// RG texture format constants — not exposed by the engine's minimal gl.h loader.
#ifndef GL_RG
#define GL_RG   0x8227
#endif
#ifndef GL_RG32F
#define GL_RG32F 0x8230
#endif

Ssao::~Ssao() { Release(); }

void Ssao::Release() {
    if (m_Vao)       { glDeleteVertexArrays(1, &m_Vao);        m_Vao       = 0; }
    if (m_DepthFbo)  { glDeleteFramebuffers(1, &m_DepthFbo);   m_DepthFbo  = 0; }
    if (m_DepthTex)  { glDeleteTextures(1, &m_DepthTex);       m_DepthTex  = 0; }
    if (m_SsaoFbo)   { glDeleteFramebuffers(1, &m_SsaoFbo);    m_SsaoFbo   = 0; }
    if (m_SsaoColor) { glDeleteTextures(1, &m_SsaoColor);      m_SsaoColor = 0; }
    if (m_BlurFbo)   { glDeleteFramebuffers(1, &m_BlurFbo);    m_BlurFbo   = 0; }
    if (m_BlurColor) { glDeleteTextures(1, &m_BlurColor);      m_BlurColor = 0; }
    if (m_NoiseTex)  { glDeleteTextures(1, &m_NoiseTex);       m_NoiseTex  = 0; }
    m_Width = m_Height = 0;
}

void Ssao::InitKernelAndNoise() {
    std::mt19937 rng(42u);
    std::uniform_real_distribution<float> rand01(0.0f, 1.0f);

    m_Kernel.clear();
    m_Kernel.reserve(32);
    for (int i = 0; i < 32; ++i) {
        glm::vec3 s(rand01(rng) * 2.0f - 1.0f,
                    rand01(rng) * 2.0f - 1.0f,
                    rand01(rng));
        s = glm::normalize(s) * rand01(rng);
        float scale = float(i) / 32.0f;
        scale = 0.1f + scale * scale * 0.9f;
        m_Kernel.push_back(s * scale);
    }

    float noiseData[32]; // 4x4 RG
    for (int i = 0; i < 16; ++i) {
        noiseData[i * 2]     = rand01(rng) * 2.0f - 1.0f;
        noiseData[i * 2 + 1] = rand01(rng) * 2.0f - 1.0f;
    }
    glCreateTextures(GL_TEXTURE_2D, 1, &m_NoiseTex);
    glTextureStorage2D(m_NoiseTex, 1, GL_RG32F, 4, 4);
    glTextureParameteri(m_NoiseTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_NoiseTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_NoiseTex, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(m_NoiseTex, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureSubImage2D(m_NoiseTex, 0, 0, 0, 4, 4, GL_RG, GL_FLOAT, noiseData);
}

void Ssao::Create(int width, int height) {
    m_Width  = width;
    m_Height = height;

    if (m_Kernel.empty()) InitKernelAndNoise();

    // Depth pre-pass: depth-only FBO, no color attachment
    glCreateTextures(GL_TEXTURE_2D, 1, &m_DepthTex);
    glTextureStorage2D(m_DepthTex, 1, GL_DEPTH_COMPONENT32F, width, height);
    glTextureParameteri(m_DepthTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_DepthTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_DepthTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_DepthTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_DepthTex, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    glCreateFramebuffers(1, &m_DepthFbo);
    glNamedFramebufferTexture(m_DepthFbo, GL_DEPTH_ATTACHMENT, m_DepthTex, 0);
    const GLenum kNone = GL_NONE;
    glNamedFramebufferDrawBuffers(m_DepthFbo, 1, &kNone); // depth-only: no colour writes

    const GLenum kColor0 = GL_COLOR_ATTACHMENT0;

    // Raw SSAO pass: R8 occlusion
    glCreateTextures(GL_TEXTURE_2D, 1, &m_SsaoColor);
    glTextureStorage2D(m_SsaoColor, 1, GL_R8, width, height);
    glTextureParameteri(m_SsaoColor, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_SsaoColor, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_SsaoColor, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_SsaoColor, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCreateFramebuffers(1, &m_SsaoFbo);
    glNamedFramebufferTexture(m_SsaoFbo, GL_COLOR_ATTACHMENT0, m_SsaoColor, 0);
    glNamedFramebufferDrawBuffers(m_SsaoFbo, 1, &kColor0);

    // Blurred pass: R8 with linear filter (for bilinear fetch from modelShader)
    glCreateTextures(GL_TEXTURE_2D, 1, &m_BlurColor);
    glTextureStorage2D(m_BlurColor, 1, GL_R8, width, height);
    glTextureParameteri(m_BlurColor, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_BlurColor, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_BlurColor, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_BlurColor, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCreateFramebuffers(1, &m_BlurFbo);
    glNamedFramebufferTexture(m_BlurFbo, GL_COLOR_ATTACHMENT0, m_BlurColor, 0);
    glNamedFramebufferDrawBuffers(m_BlurFbo, 1, &kColor0);

    // audit #358 — name the owner/dims/status if any of the three targets is incomplete.
    GLFramebufferCheck::Complete("Ssao depth pre-pass", m_DepthFbo, width, height);
    GLFramebufferCheck::Complete("Ssao raw occlusion", m_SsaoFbo, width, height);
    GLFramebufferCheck::Complete("Ssao blur", m_BlurFbo, width, height);

    glGenVertexArrays(1, &m_Vao);
}

void Ssao::Resize(int width, int height) {
    if (m_DepthFbo != 0 && width == m_Width && height == m_Height) return;
    GLuint savedNoise = m_NoiseTex; m_NoiseTex = 0;
    Release();
    m_NoiseTex = savedNoise;
    Create(width, height);
}

void Ssao::Compute(Shader& ssaoShader, const glm::mat4& proj) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_SsaoFbo);
    glViewport(0, 0, m_Width, m_Height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    ssaoShader.Bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_DepthTex);
    ssaoShader.SetInt("uDepth", 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, m_NoiseTex);
    ssaoShader.SetInt("uNoise", 1);

    ssaoShader.SetMat4("uProjection",    proj);
    ssaoShader.SetMat4("uInvProjection", glm::inverse(proj));
    ssaoShader.SetVec2("uScreenSize",    glm::vec2((float)m_Width, (float)m_Height));
    ssaoShader.SetFloat("uRadius",       0.5f);
    ssaoShader.SetFloat("uBias",         0.025f);
    for (int i = 0; i < (int)m_Kernel.size(); ++i)
        ssaoShader.SetVec3("uKernel[" + std::to_string(i) + "]", m_Kernel[i]);

    glBindVertexArray(m_Vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

void Ssao::Blur(Shader& blurShader) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_BlurFbo);
    glViewport(0, 0, m_Width, m_Height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    blurShader.Bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_SsaoColor);
    blurShader.SetInt("uSsao", 0);

    glBindVertexArray(m_Vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glActiveTexture(GL_TEXTURE0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}
