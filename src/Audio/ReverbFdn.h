#pragma once

#include <cstdint>
#include <vector>

// The engine's runtime reverb (the send bus every voice except the gun tails feeds): an early-reflection tap stage and an
// 8-line feedback delay network (Householder mix, per-line high-frequency damping, lightly modulated delays), stereo out,
// fed by the mono sum of its stereo input after a pre-delay. Pure DSP: no miniaudio, no allocation in Process() (every buffer
// is sized in the constructor), so the audio callback and the offline renderer (the video mixer) run the very same code.
//
// The six parameters are targets: Process() glides every derived quantity (delay lengths, feedback gains, damping,
// pre-delay, wet level) toward them in sub-blocks with a per-sample ramp inside each, so a change - a listener walking
// through a reverb zone - is smooth, with no zipper noise. Processing is wet only; the dry signal never passes through here.

struct ReverbParams {
    float RoomSize = 0.5f;      // 0 = a closet .. 1 = a canyon (scales every delay)
    float DecayTime = 1.0f;     // seconds, RT60 at low frequencies
    float HfDamping = 0.4f;     // 0 = bright .. 1 = dark
    float PreDelayMs = 10.0f;
    float WetLevel = 0.3f;      // 0..1
    float EarlyLateMix = 0.5f;  // 0 = all late tail .. 1 = all early reflections
};

class ReverbFdn {
public:
    static constexpr int kLines = 8;
    static constexpr int kTaps = 8;       // early reflections per channel

    explicit ReverbFdn(int sampleRate = 48000);
    // Where the parameters are heading (thread-compatible with Process if the caller serialises them, as the engine node does).
    void SetTarget(const ReverbParams& p);
    // Jumps straight to `p` (start-up, tests): no glide.
    void Reset(const ReverbParams& p);
    void SetSmoothingTime(float seconds) { m_SmoothTime = seconds < 0.001f ? 0.001f : seconds; }
    const ReverbParams& Current() const { return m_Cur; }
    const ReverbParams& Target() const { return m_Tgt; }
    // Interleaved stereo float, `frames` frames, in -> wet out (may alias).
    void Process(const float* in, float* out, int frames);
    // The wet signal of `frames` frames of silence (the tail), written to out (interleaved stereo): for tests.
    void Clear();
    int SampleRate() const { return m_Rate; }

private:
    struct Derived {
        float LateScale, EarlyScale, Pre, Wet, Early, Lp;
        float Delay[kLines], Gain[kLines];
    };
    Derived Derive(const ReverbParams& p) const;

    int m_Rate;
    float m_SmoothTime = 0.35f;
    ReverbParams m_Cur, m_Tgt;
    Derived m_D;                          // where the per-sample values are now
    std::vector<float> m_In;              // mono input history (pre-delay and early taps read it)
    std::vector<float> m_Line[kLines];
    int m_InPos = 0, m_LinePos = 0;
    int m_InMask = 0, m_LineMask = 0;
    float m_Lp[kLines] = {};              // per-line damping state
    float m_ErLpL = 0.0f, m_ErLpR = 0.0f; // early reflections' damping state
    float m_Phase[kLines] = {};
    float m_PhaseInc[kLines] = {};
    std::uint32_t m_Tick = 0;
};
