#include "MasterLimiter.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kMaxLookaheadMs = 10.0f;

inline float Catmull(float p0, float p1, float p2, float p3, float t) {
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
}
} // namespace

MasterLimiter::MasterLimiter(int sampleRate) : m_Rate(sampleRate < 8000 ? 8000 : sampleRate) {
    m_Cap = (int)(kMaxLookaheadMs * 0.001f * (float)m_Rate) + 16;
    for (auto& x : m_X) x.assign((size_t)m_Cap, 0.0f);
    m_QIdx.assign((size_t)m_Cap, 0);
    m_QVal.assign((size_t)m_Cap, 1.0f);
    m_Mv.assign((size_t)m_Cap, 1.0f);
    Configure(LimiterSettings{});
}

void MasterLimiter::Reset() {
    for (auto& x : m_X) std::fill(x.begin(), x.end(), 0.0f);
    std::fill(m_Mv.begin(), m_Mv.end(), 1.0f);
    m_QHead = m_QCount = 0;
    m_Sum = (double)(m_L + 1);
    m_N = 0;
    m_Gs = 1.0;
    m_PrevSeg = 0.0f;
    m_GlueGr = 0.0;
}

void MasterLimiter::Configure(const LimiterSettings& s) {
    const int maxL = m_Cap - 16;
    const int l = std::clamp((int)std::lround(std::clamp(s.LookaheadMs, 0.1f, kMaxLookaheadMs) * 0.001f * (float)m_Rate), 1, maxL);
    const bool restart = l != m_L || m_N == 0;
    m_S = s;
    m_S.LookaheadMs = std::clamp(s.LookaheadMs, 0.1f, kMaxLookaheadMs);
    m_S.ReleaseMs = std::max(s.ReleaseMs, 1.0f);
    m_S.CeilingDb = std::min(s.CeilingDb, 0.0f);
    m_L = l;
    m_Alpha = 1.0 - std::exp(-1.0 / ((double)m_S.ReleaseMs * 0.001 * (double)m_Rate));
    m_CeilLin = std::pow(10.0f, m_S.CeilingDb / 20.0f);
    m_S.GlueRatio = std::max(s.GlueRatio, 1.0f);
    m_S.GlueKneeDb = std::max(s.GlueKneeDb, 0.0f);
    m_GlueAtk = 1.0 - std::exp(-1.0 / (std::max((double)s.GlueAttackMs, 0.1) * 0.001 * (double)m_Rate));
    m_GlueRel = 1.0 - std::exp(-1.0 / (std::max((double)s.GlueReleaseMs, 1.0) * 0.001 * (double)m_Rate));
    m_Trim = std::pow(10.0f, s.TrimDb / 20.0f);
    if (restart) Reset();
}

void MasterLimiter::Process(const float* in, float* out, int frames) {
    const int cap = m_Cap;
    const int L = m_L;
    const float ceil = m_CeilLin;
    float worstGs = 1.0f;
    double worstGlue = 0.0;
    const bool glue = m_S.GlueEnabled;
    const double gT = m_S.GlueThresholdDb, gW = m_S.GlueKneeDb, gS = 1.0 - 1.0 / (double)m_S.GlueRatio;
    for (int f = 0; f < frames; ++f) {
        float inL = in[2 * (size_t)f] * m_Trim, inR = in[2 * (size_t)f + 1] * m_Trim;
        if (glue) { // the glue: a soft-knee gain computer on the linked peak, its reduction smoothed (attack / release) in dB
            const double x = 20.0 * std::log10(std::max((double)std::max(std::fabs(inL), std::fabs(inR)), 1e-9));
            const double over = x - gT;
            double want = 0.0;
            if (2.0 * over >= gW) want = gS * over;
            else if (gW > 0.0 && 2.0 * over > -gW) want = gS * (over + gW * 0.5) * (over + gW * 0.5) / (2.0 * gW);
            m_GlueGr += (want > m_GlueGr ? m_GlueAtk : m_GlueRel) * (want - m_GlueGr);
            const float gg = (float)std::pow(10.0, -m_GlueGr / 20.0);
            inL *= gg;
            inR *= gg;
            worstGlue = std::max(worstGlue, m_GlueGr);
        }
        const long long n = m_N++;
        m_X[0][(size_t)(n % cap)] = inL;
        m_X[1][(size_t)(n % cap)] = inR;
        out[2 * (size_t)f] = 0.0f;
        out[2 * (size_t)f + 1] = 0.0f;
        if (n < 2) continue;
        // required gain of frame j = n - 2: its own peak and the two segments (j - 1, j), (j, j + 1) of the cubic through the samples around
        const long long j = n - 2;
        auto at = [&](int c, long long k) { return m_X[c][(size_t)(((k % cap) + cap) % cap)]; };
        float seg = 0.0f;
        float peak = 0.0f;
        for (int c = 0; c < 2; ++c) {
            const float p0 = at(c, j - 1), p1 = at(c, j), p2 = at(c, j + 1), p3 = at(c, j + 2);
            peak = std::max(peak, std::fabs(p1));
            for (int q = 1; q <= 3; ++q) seg = std::max(seg, std::fabs(Catmull(p0, p1, p2, p3, 0.25f * (float)q)));
        }
        const float need = std::max({peak, seg, m_PrevSeg});
        m_PrevSeg = seg;
        const float t = (m_S.Enabled && need > ceil) ? ceil / need : 1.0f;
        // sliding minimum of t over the last L + 1 frames (monotonic deque), then a moving average of that over L + 1 frames
        while (m_QCount > 0 && m_QVal[(size_t)((m_QHead + m_QCount - 1) % cap)] >= t) --m_QCount;
        const int slot = (m_QHead + m_QCount) % cap;
        m_QIdx[(size_t)slot] = j;
        m_QVal[(size_t)slot] = t;
        ++m_QCount;
        while (m_QCount > 0 && m_QIdx[(size_t)m_QHead] < j - L) {
            m_QHead = (m_QHead + 1) % cap;
            --m_QCount;
        }
        const float m = m_QVal[(size_t)m_QHead];
        const long long old = j - L - 1;
        m_Sum += (double)m - (double)m_Mv[(size_t)(((old % cap) + cap) % cap)];
        m_Mv[(size_t)(j % cap)] = m;
        const double g = std::min(m_Sum / (double)(L + 1), 1.0);
        if (g < m_Gs) m_Gs = g;
        else m_Gs += m_Alpha * (g - m_Gs);
        worstGs = std::min(worstGs, (float)m_Gs);
        const long long o = j - L;
        if (o >= 0) {
            const float gs = (float)m_Gs;
            float yl = at(0, o) * gs, yr = at(1, o) * gs;
            if (m_S.Enabled) { // (the gain already keeps it under; this makes the sample peak a guarantee)
                yl = std::clamp(yl, -ceil, ceil);
                yr = std::clamp(yr, -ceil, ceil);
            }
            out[2 * (size_t)f] = yl;
            out[2 * (size_t)f + 1] = yr;
        }
    }
    if (worstGlue > 0.0) {
        const float gr = (float)worstGlue;
        float cur = m_GlueGrDb.load(std::memory_order_relaxed);
        while (gr > cur && !m_GlueGrDb.compare_exchange_weak(cur, gr, std::memory_order_relaxed)) {}
    }
    if (worstGs < 1.0f) {
        const float gr = -20.0f * std::log10(std::max(worstGs, 1e-6f));
        float cur = m_GrDb.load(std::memory_order_relaxed);
        while (gr > cur && !m_GrDb.compare_exchange_weak(cur, gr, std::memory_order_relaxed)) {}
    }
}
