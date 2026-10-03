#include "ReverbFdn.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kBlock = 16;            // frames between parameter updates (the per-sample ramp runs inside)

// Delay-line lengths in samples at 48 kHz (mutually non-multiple, so the lines' echoes do not stack), at RoomSize scale 1.
constexpr float kLineBase[ReverbFdn::kLines] = {1031.0f, 1237.0f, 1429.0f, 1621.0f, 1823.0f, 2027.0f, 2251.0f, 2467.0f};
// Where the mono input enters each line and how the lines mix down to left / right (orthogonal sign patterns, so the two
// channels are decorrelated yet fold to mono without a hole).
constexpr float kInject[ReverbFdn::kLines] = {1, -1, 1, 1, -1, 1, -1, -1};
constexpr float kOutL[ReverbFdn::kLines] = {1, -1, 1, -1, 1, -1, 1, -1};
constexpr float kOutR[ReverbFdn::kLines] = {1, 1, -1, -1, 1, 1, -1, -1};
// Modulation: a light wobble of each line's length (samples) at its own slow rate (Hz).
constexpr float kModDepth = 1.4f;
constexpr float kModRate[ReverbFdn::kLines] = {0.13f, 0.17f, 0.21f, 0.25f, 0.29f, 0.33f, 0.37f, 0.41f};
// Early reflections (ms at room size 1.0 before the 0.5 .. 2.0 scale) for the left and right channel, and their gains.
constexpr float kTapL[ReverbFdn::kTaps] = {4.1f, 7.7f, 11.3f, 16.9f, 23.3f, 31.7f, 41.9f, 53.3f};
constexpr float kTapR[ReverbFdn::kTaps] = {5.3f, 9.1f, 13.7f, 19.9f, 27.1f, 36.1f, 47.3f, 58.9f};
constexpr float kTapGainL[ReverbFdn::kTaps] = {0.50f, -0.42f, 0.36f, -0.30f, 0.26f, -0.22f, 0.18f, -0.15f};
constexpr float kTapGainR[ReverbFdn::kTaps] = {-0.47f, 0.40f, -0.34f, 0.29f, -0.24f, 0.20f, -0.17f, 0.14f};
constexpr float kInjectGain = 0.30f;
constexpr float kLateOut = 0.5f;

int Pow2AtLeast(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

inline float ReadFrac(const std::vector<float>& buf, int mask, int pos, float delay) {
    const int i0 = (int)delay;
    const float f = delay - (float)i0;
    const float a = buf[(size_t)((pos - i0) & mask)], b = buf[(size_t)((pos - i0 - 1) & mask)];
    return a + (b - a) * f;
}

} // namespace

ReverbFdn::ReverbFdn(int sampleRate) : m_Rate(sampleRate < 8000 ? 8000 : sampleRate) {
    const float k = (float)m_Rate / 48000.0f;
    m_InMask = Pow2AtLeast((int)(0.55f * (float)m_Rate)) - 1;
    m_LineMask = Pow2AtLeast((int)(kLineBase[kLines - 1] * 3.0f * k + 64.0f)) - 1;
    m_In.assign((size_t)m_InMask + 1, 0.0f);
    for (auto& l : m_Line) l.assign((size_t)m_LineMask + 1, 0.0f);
    for (int i = 0; i < kLines; ++i) {
        m_PhaseInc[i] = 2.0f * kPi * kModRate[i] / (float)m_Rate;
        m_Phase[i] = 2.0f * kPi * (float)i / (float)kLines; // not in step
    }
    Reset(ReverbParams{});
}

void ReverbFdn::Clear() {
    std::fill(m_In.begin(), m_In.end(), 0.0f);
    for (auto& l : m_Line) std::fill(l.begin(), l.end(), 0.0f);
    std::fill(std::begin(m_Lp), std::end(m_Lp), 0.0f);
    m_ErLpL = m_ErLpR = 0.0f;
}

ReverbFdn::Derived ReverbFdn::Derive(const ReverbParams& p) const {
    Derived d;
    const float rate = (float)m_Rate, k = rate / 48000.0f;
    const float room = std::clamp(p.RoomSize, 0.0f, 1.0f);
    const float damp = std::clamp(p.HfDamping, 0.0f, 1.0f);
    const float decay = std::max(p.DecayTime, 0.05f);
    d.LateScale = 0.6f + 2.4f * room;
    d.EarlyScale = 0.5f + 1.5f * room;
    d.Pre = std::clamp(p.PreDelayMs, 0.0f, 250.0f) * rate / 1000.0f + 1.0f;
    d.Wet = std::clamp(p.WetLevel, 0.0f, 1.0f);
    d.Early = std::clamp(p.EarlyLateMix, 0.0f, 1.0f);
    const float fc = 16000.0f - damp * (16000.0f - 1500.0f);
    d.Lp = std::exp(-2.0f * kPi * std::min(fc, 0.45f * rate) / rate);
    for (int i = 0; i < kLines; ++i) {
        d.Delay[i] = kLineBase[i] * k * d.LateScale;
        d.Gain[i] = std::pow(10.0f, -3.0f * d.Delay[i] / (rate * decay));
    }
    return d;
}

void ReverbFdn::SetTarget(const ReverbParams& p) { m_Tgt = p; }

void ReverbFdn::Reset(const ReverbParams& p) {
    m_Cur = m_Tgt = p;
    m_D = Derive(p);
    Clear();
}

void ReverbFdn::Process(const float* in, float* out, int frames) {
    const float rate = (float)m_Rate;
    const float msToSamples = rate / 1000.0f;
    int done = 0;
    while (done < frames) {
        const int n = std::min(kBlock, frames - done);
        // Where the parameters are by the end of this block.
        const float k = 1.0f - std::exp(-(float)n / (rate * m_SmoothTime));
        m_Cur.RoomSize += (m_Tgt.RoomSize - m_Cur.RoomSize) * k;
        m_Cur.DecayTime += (m_Tgt.DecayTime - m_Cur.DecayTime) * k;
        m_Cur.HfDamping += (m_Tgt.HfDamping - m_Cur.HfDamping) * k;
        m_Cur.PreDelayMs += (m_Tgt.PreDelayMs - m_Cur.PreDelayMs) * k;
        m_Cur.WetLevel += (m_Tgt.WetLevel - m_Cur.WetLevel) * k;
        m_Cur.EarlyLateMix += (m_Tgt.EarlyLateMix - m_Cur.EarlyLateMix) * k;
        const Derived d1 = Derive(m_Cur);
        const Derived d0 = m_D;
        // Modulation at the block's two ends (a linear ramp between: the phases move a few thousandths of a cycle).
        float mod0[kLines], mod1[kLines];
        for (int i = 0; i < kLines; ++i) {
            mod0[i] = kModDepth * std::sin(m_Phase[i]);
            m_Phase[i] += m_PhaseInc[i] * (float)n;
            if (m_Phase[i] > 2.0f * kPi) m_Phase[i] -= 2.0f * kPi;
            mod1[i] = kModDepth * std::sin(m_Phase[i]);
        }
        const float inv = 1.0f / (float)n;
        float dly[kLines], dlyStep[kLines], g[kLines], gStep[kLines];
        for (int i = 0; i < kLines; ++i) {
            dly[i] = d0.Delay[i] + mod0[i];
            dlyStep[i] = ((d1.Delay[i] + mod1[i]) - dly[i]) * inv;
            g[i] = d0.Gain[i];
            gStep[i] = (d1.Gain[i] - g[i]) * inv;
        }
        float pre = d0.Pre, preStep = (d1.Pre - d0.Pre) * inv;
        float wet = d0.Wet, wetStep = (d1.Wet - d0.Wet) * inv;
        float early = d0.Early, earlyStep = (d1.Early - d0.Early) * inv;
        float lp = d0.Lp, lpStep = (d1.Lp - d0.Lp) * inv;
        float escale = d0.EarlyScale * msToSamples, escaleStep = (d1.EarlyScale * msToSamples - escale) * inv;

        for (int s = 0; s < n; ++s) {
            const float* fi = in + 2 * (size_t)(done + s);
            const float anti = (m_Tick++ & 1u) ? 1e-20f : -1e-20f; // keeps every state a normal float through silence
            const float mono = 0.5f * (fi[0] + fi[1]) + anti;
            m_In[(size_t)m_InPos] = mono;
            // Early reflections: taps of the pre-delayed input, a little damped.
            float erL = 0.0f, erR = 0.0f;
            for (int t = 0; t < kTaps; ++t) {
                erL += kTapGainL[t] * ReadFrac(m_In, m_InMask, m_InPos, pre + kTapL[t] * escale);
                erR += kTapGainR[t] * ReadFrac(m_In, m_InMask, m_InPos, pre + kTapR[t] * escale);
            }
            m_ErLpL = erL + (m_ErLpL - erL) * lp;
            m_ErLpR = erR + (m_ErLpR - erR) * lp;
            // Late field: the pre-delayed input into the feedback network.
            const float x = ReadFrac(m_In, m_InMask, m_InPos, pre) * kInjectGain;
            float y[kLines], sum = 0.0f;
            for (int i = 0; i < kLines; ++i) {
                y[i] = ReadFrac(m_Line[i], m_LineMask, m_LinePos, dly[i]);
                sum += y[i];
            }
            float lateL = 0.0f, lateR = 0.0f;
            const float h = sum * 0.25f; // Householder: y - (2/N) * sum
            for (int i = 0; i < kLines; ++i) {
                const float fb = y[i] - h;
                m_Lp[i] = fb + (m_Lp[i] - fb) * lp; // damping: one pole per line
                m_Line[i][(size_t)m_LinePos] = kInject[i] * x + g[i] * m_Lp[i];
                lateL += kOutL[i] * y[i];
                lateR += kOutR[i] * y[i];
                dly[i] += dlyStep[i];
                g[i] += gStep[i];
            }
            const float oL = early * m_ErLpL + (1.0f - early) * lateL * kLateOut;
            const float oR = early * m_ErLpR + (1.0f - early) * lateR * kLateOut;
            float* fo = out + 2 * (size_t)(done + s);
            fo[0] = oL * wet;
            fo[1] = oR * wet;
            m_InPos = (m_InPos + 1) & m_InMask;
            m_LinePos = (m_LinePos + 1) & m_LineMask;
            pre += preStep;
            wet += wetStep;
            early += earlyStep;
            lp += lpStep;
            escale += escaleStep;
        }
        m_D = d1;
        done += n;
    }
}
