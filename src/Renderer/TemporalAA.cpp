#include "TemporalAA.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "GLStateCache.h"
#include "gl.h"

#include <algorithm>
#include <cmath>

namespace {
float Halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}
} // namespace

TemporalAA::~TemporalAA() {
    delete m_Resolve;
    delete m_Sharpen;
    if (m_Vao) glDeleteVertexArrays(1, &m_Vao);
    ReleaseTargets();
}

glm::vec2 TemporalAA::HaltonJitter(int index) {
    return glm::vec2(Halton(index, 2) - 0.5f, Halton(index, 3) - 0.5f);
}

glm::mat4 TemporalAA::JitterProjection(const glm::mat4& proj, glm::vec2 ndc) {
    // clip' = T * clip with T adding ndc * w to x and y: row 0 += ndc.x * row 3, row 1 += ndc.y * row 3.
    glm::mat4 p = proj;
    for (int c = 0; c < 4; ++c) {
        p[c][0] += ndc.x * proj[c][3];
        p[c][1] += ndc.y * proj[c][3];
    }
    return p;
}

glm::mat4 TemporalAA::BeginFrame(const glm::mat4& view, const glm::mat4& proj, int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    const glm::mat4 vp = proj * view;
    const glm::mat4 inv = glm::inverse(view);
    const glm::vec3 eye(inv[3]);
    const glm::vec3 fwd = -glm::normalize(glm::vec3(inv[2]));

    bool cut = !m_HaveCamera || width != m_ReqW || height != m_ReqH;
    if (!cut) {
        const glm::vec3 d = eye - m_LastEye;
        cut = glm::dot(d, d) > 2.0f * 2.0f || glm::dot(fwd, m_LastFwd) < 0.94f; // ~20 degrees in one frame
        for (int c = 0; c < 4 && !cut; ++c)
            for (int r = 0; r < 4 && !cut; ++r)
                cut = std::abs(proj[c][r] - m_LastProj[c][r]) > 1e-3f * std::max(1.0f, std::abs(proj[c][r]));
    }
    if (cut) m_HistoryValid = false;
    m_PrevVP = cut ? vp : m_CurVP;
    m_CurVP = vp;
    m_LastEye = eye;
    m_LastFwd = fwd;
    m_LastProj = proj;
    m_HaveCamera = true;
    m_ReqW = width;
    m_ReqH = height;
    m_Ortho = std::abs(proj[3][3]) > 0.5f;

    const glm::vec2 j = HaltonJitter((int)(m_Frame++ % kJitterSamples) + 1);
    m_JitterNdc = glm::vec2(2.0f * j.x / (float)width, 2.0f * j.y / (float)height);
    const glm::mat4 jittered = JitterProjection(proj, m_JitterNdc);
    m_JitteredVP = jittered * view;
    return jittered;
}

void TemporalAA::EnsureShaders() {
    if (m_Resolve) return;
    m_Resolve = new Shader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"), ShaderLibrary::ReadFile("TaaResolve.frag.glsl"),
                           "taaResolve");
    m_Sharpen = new Shader(ShaderLibrary::ReadFile("Tonemapper.vert.glsl"), ShaderLibrary::ReadFile("TaaSharpen.frag.glsl"),
                           "taaSharpen");
    glGenVertexArrays(1, &m_Vao);
}

void TemporalAA::ReleaseTargets() {
    for (int i = 0; i < 2; ++i) {
        if (m_History[i]) glDeleteTextures(1, &m_History[i]);
        if (m_HistoryFbo[i]) glDeleteFramebuffers(1, &m_HistoryFbo[i]);
        m_History[i] = m_HistoryFbo[i] = 0;
    }
    if (m_Out) glDeleteTextures(1, &m_Out);
    if (m_OutFbo) glDeleteFramebuffers(1, &m_OutFbo);
    m_Out = m_OutFbo = 0;
}

void TemporalAA::EnsureTargets(int w, int h) {
    if (m_Out && w == m_W && h == m_H) return;
    ReleaseTargets();
    m_W = w;
    m_H = h;
    auto make = [w, h](unsigned int& tex, unsigned int& fbo) {
        glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        glTextureStorage2D(tex, 1, GL_RGBA16F, w, h);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCreateFramebuffers(1, &fbo);
        glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, tex, 0);
    };
    make(m_History[0], m_HistoryFbo[0]);
    make(m_History[1], m_HistoryFbo[1]);
    make(m_Out, m_OutFbo);
    m_HistoryValid = false;
}

unsigned int TemporalAA::Resolve(unsigned int color, unsigned int depth, unsigned int velocityMs, float sharpness) {
    if (!color || !depth || !velocityMs || m_ReqW <= 0 || m_ReqH <= 0) return color;
    EnsureShaders();
    EnsureTargets(m_ReqW, m_ReqH);

    const int src = 1 - m_Cur, dst = m_Cur;
    glBindVertexArray(m_Vao);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, m_W, m_H);

    // Resolve: current + reprojected history -> history[dst] (kept unsharpened, in the tone-mapped working space).
    glBindFramebuffer(GL_FRAMEBUFFER, m_HistoryFbo[dst]);
    m_Resolve->Bind();
    glBindTextureUnit(0, color);
    glBindTextureUnit(1, depth);
    glBindTextureUnit(2, velocityMs);
    glBindTextureUnit(3, m_History[src]);
    m_Resolve->SetInt("uColor", 0);
    m_Resolve->SetInt("uDepth", 1);
    m_Resolve->SetInt("uVelocity", 2);
    m_Resolve->SetInt("uHistory", 3);
    m_Resolve->SetInt("uHistoryValid", m_HistoryValid ? 1 : 0);
    m_Resolve->SetInt("uOrtho", m_Ortho ? 1 : 0);
    m_Resolve->SetMat4("uInvViewProj", glm::inverse(m_JitteredVP));
    m_Resolve->SetMat4("uCurViewProj", m_CurVP);
    m_Resolve->SetMat4("uPrevViewProj", m_PrevVP);
    m_Resolve->SetVec2("uJitterUv", m_JitterNdc * 0.5f);
    m_Resolve->SetVec2("uTexel", glm::vec2(1.0f / (float)m_W, 1.0f / (float)m_H));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Sharpen: history[dst] -> out (linear HDR again).
    glBindFramebuffer(GL_FRAMEBUFFER, m_OutFbo);
    m_Sharpen->Bind();
    glBindTextureUnit(0, m_History[dst]);
    m_Sharpen->SetInt("uSrc", 0);
    m_Sharpen->SetFloat("uSharpness", std::clamp(sharpness, 0.0f, 1.0f));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int u = 0; u < 4; ++u) glBindTextureUnit(u, 0);
    GLStateCache::Invalidate(); // raw program / texture binds above
    glEnable(GL_DEPTH_TEST);

    m_HistoryValid = true;
    m_Cur = src;
    return m_Out;
}
