#include "GpuMemTrack.h"
#include "Ssao.h"
#include "GLStateCache.h"
#include "GLFramebufferCheck.h"
#include "Shader.h"
#include "gl.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <random>
#include <string>

// RG texture format constants — not exposed by the engine's minimal gl.h loader.
#ifndef GL_RG32F
#define GL_RG32F 0x8230
#endif

Ssao::~Ssao() { Release(); }

void Ssao::Release() {
    GpuMemTrack::Set(GpuMemTrack::Key("Ssao depth copy", this), 0);
    if (m_DepthFbo) { glDeleteFramebuffers(1, &m_DepthFbo); m_DepthFbo = 0; }
    if (m_DepthTex) { glDeleteTextures(1, &m_DepthTex);     m_DepthTex = 0; }
    m_Raw.Release();
    m_Blur.Release();
    if (m_NoiseTex) { glDeleteTextures(1, &m_NoiseTex);     m_NoiseTex = 0; }
    m_Width = m_Height = 0;
    m_Valid = false;
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
    GpuMemTrack::Set(GpuMemTrack::Key("Ssao depth copy", this), (size_t)width * height * 4);
    glTextureParameteri(m_DepthTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(m_DepthTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(m_DepthTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_DepthTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_DepthTex, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    glCreateFramebuffers(1, &m_DepthFbo);
    glNamedFramebufferTexture(m_DepthFbo, GL_DEPTH_ATTACHMENT, m_DepthTex, 0);
    const GLenum kNone = GL_NONE;
    glNamedFramebufferDrawBuffers(m_DepthFbo, 1, &kNone); // depth-only: no colour writes

    // audit #358 — any incomplete target makes IsValid() false: callers then skip the SSAO passes
    // entirely and the scene renders without ambient occlusion, instead of the passes running
    // against a bad FBO. Raw occlusion at half resolution (nearest), the blurred map at full (linear,
    // for the model shader's bilinear fetch).
    bool ok = GLFramebufferCheck::Complete("Ssao depth pre-pass", m_DepthFbo, width, height);
    ok = m_Raw.Ensure((width + 1) / 2, (height + 1) / 2, GL_RG16F, "Ssao raw occlusion", false) && ok;
    ok = m_Blur.Ensure(width, height, GL_R8, "Ssao blur") && ok;
    m_Valid = ok;
}

void Ssao::Resize(int width, int height) {
    if (m_DepthFbo != 0 && width == m_Width && height == m_Height) return;
    GLuint savedNoise = m_NoiseTex; m_NoiseTex = 0;
    Release();
    m_NoiseTex = savedNoise;
    Create(width, height);
}

void Ssao::WarmUp(Shader& ssaoShader, Shader& blurShader) {
    Ssao tmp;
    tmp.Resize(16, 16);
    if (!tmp.IsValid()) return;
    tmp.Compute(ssaoShader, glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f));
    tmp.Blur(blurShader);
    // tmp's textures die with this scope; leaving the blur program current would sample them
    // (now texture 0) on the first draw of the next frame (KHR 131204).
    glUseProgram(0);
    GLStateCache::Invalidate();
}

void Ssao::Compute(Shader& ssaoShader, const glm::mat4& proj, float radius, float bias) {
    FullscreenPass pass; // #160 - depth/blend/cull back as the caller had them afterwards
    m_Raw.Bind();
    m_Proj = proj;

    ssaoShader.Bind();
    glBindTextureUnit(0, m_DepthTex);
    ssaoShader.SetInt("uDepth", 0);
    glBindTextureUnit(1, m_NoiseTex);
    ssaoShader.SetInt("uNoise", 1);

    ssaoShader.SetMat4("uProjection",    proj);
    ssaoShader.SetMat4("uInvProjection", glm::inverse(proj));
    glUniform2i(ssaoShader.Loc("uDepthSize"), m_Width, m_Height);
    ssaoShader.SetFloat("uRadius",       radius);
    ssaoShader.SetFloat("uBias",         bias);
    // #160 - the kernel never changes, so upload it once per program (again only if the program
    // is recreated) instead of 32 string-built uniform names every frame.
    if (m_KernelUploadedTo != ssaoShader.Program()) {
        // The 32-sample kernel's even entries (short and long alike): the 16 the shader takes.
        for (int i = 0; i < (int)m_Kernel.size() / 2; ++i)
            ssaoShader.SetVec3("uKernel[" + std::to_string(i) + "]", m_Kernel[2 * i]);
        m_KernelUploadedTo = ssaoShader.Program();
    }

    FullscreenPass::Draw();
}

void Ssao::Blur(Shader& blurShader) {
    FullscreenPass pass; // #160
    m_Blur.Bind();

    blurShader.Bind();
    glBindTextureUnit(0, m_Raw.Texture());
    blurShader.SetInt("uSsao", 0);
    glBindTextureUnit(1, m_DepthTex);
    blurShader.SetInt("uDepth", 1);
    blurShader.SetMat4("uProjection", m_Proj);
    FullscreenPass::Draw();
}
