#include "ConvolutionReverb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
#include <xmmintrin.h>
#define CR_SSE 1
#endif

namespace {
constexpr int kChunk = 64;             // control rate: gliding values and filter coefficients are refreshed every 64 frames
constexpr unsigned kRingSize = 16384;  // 341 ms at 48 kHz (pre-delay is capped at 250 ms)
constexpr float kMaxPreDelayMs = 250.0f;
constexpr double kShelfHz = 4000.0;
constexpr float kDelaySlew = 0.02f;    // frames of delay change per frame of audio: a moving pre-delay never pitch-shifts by more than 2 %

// Enables flush-to-zero / denormals-are-zero for the scope of one callback (the FFT products of a decaying tail go subnormal).
struct DenormalGuard {
#ifdef CR_SSE
    unsigned Saved;
    DenormalGuard() : Saved(_mm_getcsr()) { _mm_setcsr(Saved | 0x8040u); }
    ~DenormalGuard() { _mm_setcsr(Saved); }
#endif
};
} // namespace

ConvolutionReverb::ConvolutionReverb(int sampleRate, bool threaded) : m_Rate(sampleRate < 8000 ? 8000 : sampleRate) {
    for (auto& p : m_Inst) p.store(nullptr);
    if (threaded) m_Worker = std::make_unique<TailWorker>();
}

ConvolutionReverb::~ConvolutionReverb() {
    for (auto& p : m_Inst) delete p.load(); // (each Convolver waits for its queued tail blocks)
    // the worker (declared before the instances' use) joins last
}

int ConvolutionReverb::AddIr(std::shared_ptr<const IrData> ir) {
    if (!ir || ir->Length <= 0) return -1;
    const int idx = m_Count.load(std::memory_order_relaxed);
    if (idx >= kMaxInstances) return -1;
    auto* s = new Inst();
    s->Conv = std::make_unique<Convolver>(std::move(ir), m_Worker.get());
    s->Ring.assign(kRingSize, 0.0f);
    m_Inst[idx].store(s, std::memory_order_release);
    m_Count.store(idx + 1, std::memory_order_release);
    return idx;
}

void ConvolutionReverb::SetTarget(const ReverbRtTarget& t) {
    std::lock_guard<std::mutex> lock(m_Mail);
    m_Pending = t;
    m_Version.fetch_add(1, std::memory_order_release);
}

ReverbRtTarget ConvolutionReverb::Target() const {
    std::lock_guard<std::mutex> lock(m_Mail);
    return m_Pending;
}

ConvolutionReverb::Info ConvolutionReverb::GetInfo() const {
    Info info;
    info.Instances = m_Count.load(std::memory_order_acquire);
    for (int k = 0; k < info.Instances; ++k) {
        const Inst* s = m_Inst[k].load(std::memory_order_acquire);
        if (!s) continue;
        info.On[k] = s->OnReport.load(std::memory_order_relaxed);
        info.Weight[k] = s->WReport.load(std::memory_order_relaxed);
        if (info.On[k]) ++info.Active;
    }
    return info;
}

std::uint64_t ConvolutionReverb::LateBlocks() const {
    std::uint64_t n = 0;
    for (int k = 0; k < m_Count.load(std::memory_order_acquire); ++k)
        if (const Inst* s = m_Inst[k].load(std::memory_order_acquire)) n += s->Conv->LateBlocks();
    return n;
}

// RBJ cookbook: a high shelf at 4 kHz (gain -hfDb) and a 2nd-order high-pass (Q 0.707) at the low cut.
void ConvolutionReverb::Design(Inst& s, float hfDb, float lowCutHz) const {
    const double sr = (double)m_Rate;
    const double pi = 3.14159265358979323846;
    {
        const double A = std::pow(10.0, -std::max((double)hfDb, 0.0) / 40.0);
        const double w0 = 2.0 * pi * std::min(kShelfHz, 0.45 * sr) / sr, c = std::cos(w0), sn = std::sin(w0);
        const double alpha = sn / 2.0 * std::sqrt(2.0); // S = 1
        const double sq = 2.0 * std::sqrt(A) * alpha;
        const double a0 = (A + 1) - (A - 1) * c + sq;
        s.Shelf.B0 = A * ((A + 1) + (A - 1) * c + sq) / a0;
        s.Shelf.B1 = -2.0 * A * ((A - 1) + (A + 1) * c) / a0;
        s.Shelf.B2 = A * ((A + 1) + (A - 1) * c - sq) / a0;
        s.Shelf.A1 = 2.0 * ((A - 1) - (A + 1) * c) / a0;
        s.Shelf.A2 = ((A + 1) - (A - 1) * c - sq) / a0;
    }
    {
        const double f = std::clamp((double)lowCutHz, 5.0, 0.45 * sr);
        const double w0 = 2.0 * pi * f / sr, c = std::cos(w0), sn = std::sin(w0);
        const double alpha = sn / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        s.HighPass.B0 = (1.0 + c) / 2.0 / a0;
        s.HighPass.B1 = -(1.0 + c) / a0;
        s.HighPass.B2 = (1.0 + c) / 2.0 / a0;
        s.HighPass.A1 = -2.0 * c / a0;
        s.HighPass.A2 = (1.0 - alpha) / a0;
    }
    s.AppliedHf = hfDb;
    s.AppliedLc = lowCutHz;
}

void ConvolutionReverb::ProcessChunk(const float* mono, float* outL, float* outR, int n) {
    const float glide = std::max(m_Cur.GlideSeconds, 0.01f);
    const float sr = (float)m_Rate;
    const float ctl = 1.0f - std::exp(-(float)n / (sr * glide / 3.0f)); // one-pole step of the smoothed values over this chunk
    const int count = m_Count.load(std::memory_order_acquire);
    for (int k = 0; k < count; ++k) {
        Inst* s = m_Inst[k].load(std::memory_order_acquire);
        if (!s) continue;
        // what the target wants of this IR
        float tw = 0.0f, tWet = 0.0f, tPd = 0.0f, tHf = 0.0f, tLc = 0.0f;
        for (int l = 0; l < m_Cur.Count && l < 2; ++l) {
            const ReverbRtLayer& L = m_Cur.Layers[l];
            if (L.Instance != k) continue;
            tw += std::max(L.Weight, 0.0f);
            tWet = L.WetLin;
            tPd = std::clamp(L.PreDelayMs, 0.0f, kMaxPreDelayMs);
            tHf = std::max(L.HfDampDb, 0.0f);
            tLc = std::max(L.LowCutHz, 0.0f);
        }
        if (!s->On) {
            if (tw <= 1e-4f || !s->Conv->Idle()) continue; // silent, or still finishing its last tail blocks
            s->Conv->Reset();
            std::fill(s->Ring.begin(), s->Ring.end(), 0.0f);
            s->Shelf.Z1 = s->Shelf.Z2 = s->HighPass.Z1 = s->HighPass.Z2 = 0.0;
            s->Pos = 0;
            s->W = 0.0f;
            s->Wet = tWet;
            s->Delay = tPd * sr / 1000.0f;
            s->Hf = tHf;
            s->Lc = tLc;
            s->AppliedHf = s->AppliedLc = -1.0f;
            s->On = true;
        }
        // the glide: the weight moves linearly (so two layers cross at equal power), the rest follows one-pole
        const float dw = (float)n / (sr * glide);
        const float w0 = s->W, w1 = std::clamp(tw, w0 - dw, w0 + dw);
        s->Hf += (tHf - s->Hf) * ctl;
        s->Lc += (tLc - s->Lc) * ctl;
        if (std::fabs(s->Hf - s->AppliedHf) > 0.01f || std::fabs(s->Lc - s->AppliedLc) > 0.05f) Design(*s, s->Hf, s->Lc);
        const float tDelay = tPd * sr / 1000.0f;
        float x[kChunk], cl[kChunk], cr[kChunk];
        for (int i = 0; i < n; ++i) {
            s->Ring[s->Pos & (kRingSize - 1)] = mono[i];
            const float step = std::clamp(tDelay - s->Delay, -kDelaySlew, kDelaySlew);
            s->Delay += step;
            const unsigned di = (unsigned)s->Delay;     // whole frames of delay, then the fraction (interpolated)
            const float fr = s->Delay - (float)di;
            const float a = s->Ring[(s->Pos - di) & (kRingSize - 1)], b = s->Ring[(s->Pos - di - 1) & (kRingSize - 1)];
            const double v = (double)(a + (b - a) * fr);
            x[i] = (float)s->HighPass.Run(s->Shelf.Run(v));
            ++s->Pos;
        }
        s->Conv->Process(x, cl, cr, n);
        float wet = s->Wet;
        const float wetStep = (tWet - wet) * ctl / (float)n;
        for (int i = 0; i < n; ++i) {
            const float w = w0 + (w1 - w0) * (float)(i + 1) / (float)n;
            wet += wetStep;
            const float g = std::sqrt(std::max(w, 0.0f)) * wet;
            outL[i] += g * cl[i];
            outR[i] += g * cr[i];
        }
        s->Wet = wet;
        s->W = w1;
        s->WReport.store(w1, std::memory_order_relaxed);
        if (tw <= 1e-4f && w1 <= 1e-5f) { // faded out: stop running it
            s->On = false;
            s->W = 0.0f;
            s->WReport.store(0.0f, std::memory_order_relaxed);
        }
        s->OnReport.store(s->On, std::memory_order_relaxed);
    }
}

void ConvolutionReverb::Process(const float* in, float* out, int frames) {
    DenormalGuard guard;
    {
        std::unique_lock<std::mutex> lock(m_Mail, std::try_to_lock); // never wait for the game thread
        if (lock.owns_lock() && m_Version.load(std::memory_order_acquire) != m_Seen) {
            m_Cur = m_Pending;
            m_Seen = m_Version.load(std::memory_order_acquire);
        }
    }
    float mono[kChunk], l[kChunk], r[kChunk];
    for (int off = 0; off < frames; off += kChunk) {
        const int n = std::min(kChunk, frames - off);
        for (int i = 0; i < n; ++i) {
            mono[i] = 0.5f * (in[2 * (size_t)(off + i)] + in[2 * (size_t)(off + i) + 1]);
            l[i] = r[i] = 0.0f;
        }
        ProcessChunk(mono, l, r, n);
        for (int i = 0; i < n; ++i) {
            out[2 * (size_t)(off + i)] = l[i];
            out[2 * (size_t)(off + i) + 1] = r[i];
        }
    }
}
