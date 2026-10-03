#pragma once

#include <atomic>
#include <vector>

// The master peak limiter (the last node before the output, after every bus and the reverb returns). Pure DSP.
//
// Look-ahead brick-wall: every input frame's required gain (ceiling / peak, the peak taken over both channels and an estimate
// of the inter-sample peak) goes through a sliding minimum over the look-ahead window, then a moving average over the same
// window (so the gain has fully arrived by the time the peak does, and ramps in without a click), then an exponential release.
// The output is the input delayed by the look-ahead (+ 2 frames of detection) times that gain; the gain is never above the
// requirement of the frame being output, so the output never exceeds the ceiling (a final clamp guarantees it for the sample
// peak whatever the settings do). Linked stereo: both channels get the same gain.

struct LimiterSettings {
    bool Enabled = true;
    float CeilingDb = -1.0f;     // dBFS
    float LookaheadMs = 1.5f;    // 0.1 .. 10
    float ReleaseMs = 80.0f;     // time constant of the gain's recovery
    // Before the limiter: the master trim, then the glue compressor (feed-forward, linked stereo, peak detector, soft knee, the
    // gain reduction smoothed with separate attack / release). Off = bypassed bit for bit.
    float TrimDb = 0.0f;
    bool GlueEnabled = false;
    float GlueThresholdDb = -12.0f;
    float GlueRatio = 2.0f;
    float GlueKneeDb = 6.0f;
    float GlueAttackMs = 15.0f;
    float GlueReleaseMs = 200.0f;
};

class MasterLimiter {
public:
    explicit MasterLimiter(int sampleRate);
    // Audio thread (or before processing starts). Changing the look-ahead does not allocate.
    void Configure(const LimiterSettings& s);
    const LimiterSettings& Settings() const { return m_S; }
    // Interleaved stereo, `frames` frames; may alias.
    void Process(const float* in, float* out, int frames);
    int LatencyFrames() const { return m_L + 2; }
    // The deepest gain reduction (dB, >= 0) since the last call; read from any thread.
    float TakeGainReductionDb() { return m_GrDb.exchange(0.0f, std::memory_order_relaxed); }
    float TakeGlueReductionDb() { return m_GlueGrDb.exchange(0.0f, std::memory_order_relaxed); } // the glue's, the same way
    float CurrentGain() const { return (float)m_Gs; }
    void Reset();

private:
    int m_Rate;
    LimiterSettings m_S;
    int m_L = 72;                   // look-ahead frames in use
    int m_Cap;                      // ring capacity (frames)
    std::vector<float> m_X[2];      // input history
    std::vector<long long> m_QIdx;  // monotonic deque of the required gains (frame numbers), as a ring
    std::vector<float> m_QVal;
    int m_QHead = 0, m_QCount = 0;
    std::vector<float> m_Mv;        // the sliding-min values, for the moving average
    double m_Sum = 0.0;
    long long m_N = 0;              // frames received
    double m_Gs = 1.0;              // the smoothed gain
    float m_PrevSeg = 0.0f;         // peak of the previous segment
    std::atomic<float> m_GrDb{0.0f}, m_GlueGrDb{0.0f};
    double m_GlueGr = 0.0;          // the glue's smoothed gain reduction (dB, >= 0)
    double m_GlueAtk = 0.0, m_GlueRel = 0.0;
    float m_Trim = 1.0f;
    double m_Alpha = 0.0;
    float m_CeilLin = 0.89f;
};
