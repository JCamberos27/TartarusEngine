#pragma once

#include <glm/glm.hpp>

class Shader;

// Temporal anti-aliasing (World::AntiAliasingMode::Taa), one instance per view (each keeps its own history).
//
// Per frame:
//   1. BeginFrame(view, proj, w, h) before the view draws: returns the projection with this frame's sub-pixel
//      jitter (Halton 2,3, 8 samples) for the world pass, and remembers last frame's unjittered view-projection.
//      The caller hands JitterNdc() / PrevViewProj() to the model shaders (RenderFrameContext::Taa*), which write
//      per-pixel motion vectors into the HDR target's velocity attachment (HdrTarget::SetVelocityEnabled).
//      The view-model sub-pass builds its own unjittered projection and writes zero motion (it moves with the eye).
//   2. Resolve(...) after HdrTarget::ResolveTo(): reprojects the history by the motion vectors (camera motion from
//      depth where no vector was written: sky, transparents), clips it to the current 3x3 neighbourhood's YCoCg
//      variance box, rejects it where depth says the surface was hidden last frame, blends ~1/10 of the new
//      frame in, then a light sharpen. Returns the RGBA16F image bloom and the tone mapper should read.
//
// A camera cut (a jump of more than a couple of metres or ~20 degrees, a resize, a projection change) drops the
// history, so the next frame starts clean instead of smearing the old view over the new one.
class TemporalAA {
public:
    TemporalAA() = default;
    ~TemporalAA();
    TemporalAA(const TemporalAA&) = delete;
    TemporalAA& operator=(const TemporalAA&) = delete;

    static constexpr int kJitterSamples = 8;
    static constexpr float kVelocityUnknown = 4.0f; // HdrTarget::ClearVelocity value; ModelFragment.glsl's too

    // Halton (2, 3) point `index` (1-based) centred on the pixel: each component in [-0.5, 0.5).
    static glm::vec2 HaltonJitter(int index);
    // `proj` with clip-space xy shifted by `ndc` * w (works for perspective and orthographic alike).
    static glm::mat4 JitterProjection(const glm::mat4& proj, glm::vec2 ndc);

    glm::mat4 BeginFrame(const glm::mat4& view, const glm::mat4& proj, int width, int height);
    glm::vec2 JitterNdc() const { return m_JitterNdc; }
    const glm::mat4& PrevViewProj() const { return m_PrevVP; }

    // color: the resolved HDR colour (linear); depth: resolved depth (same size); velocityMs: the HDR target's
    // velocity attachment (GL_TEXTURE_2D_MULTISAMPLE, sample 0 read). sharpness 0..1.
    unsigned int Resolve(unsigned int color, unsigned int depth, unsigned int velocityMs, float sharpness);

    void Reset() { m_HistoryValid = false; m_HaveCamera = false; }
    void WarmUp() { EnsureShaders(); }

private:
    void EnsureShaders();
    void EnsureTargets(int w, int h);
    void ReleaseTargets();

    Shader* m_Resolve = nullptr;
    Shader* m_Sharpen = nullptr;
    unsigned int m_Vao = 0;
    unsigned int m_History[2] = {}, m_HistoryFbo[2] = {};
    unsigned int m_Out = 0, m_OutFbo = 0;
    int m_W = 0, m_H = 0;       // the history targets' size
    int m_ReqW = 0, m_ReqH = 0; // this frame's (BeginFrame)
    int m_Cur = 0;            // which history the next Resolve writes
    bool m_HistoryValid = false;

    unsigned m_Frame = 0;
    bool m_HaveCamera = false;
    glm::mat4 m_CurVP{1.0f}, m_PrevVP{1.0f}, m_JitteredVP{1.0f}, m_LastProj{1.0f};
    glm::vec3 m_LastEye{0.0f}, m_LastFwd{0.0f, 0.0f, -1.0f};
    glm::vec2 m_JitterNdc{0.0f};
    bool m_Ortho = false;
};
