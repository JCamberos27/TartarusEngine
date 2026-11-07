#include "ConvolutionReverb.h"

#include "DenormalGuard.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int kChunk = 64;             // control rate: gliding values and filter coefficients are refreshed every 64 frames
constexpr float kMaxPreDelayMs = 250.0f; // (the pre-delay ring is sized from this and the sample rate)
constexpr double kShelfHz = 4000.0;
constexpr float kDelaySlew = 0.02f;    // frames of delay change per frame of audio: a moving pre-delay never pitch-shifts by more than 2 %

// Biquad state that went NaN / Inf (a bad sample got through) is cleared instead of ringing forever.
template <class B> inline void ScrubBiquad(B& b) {
    if (!std::isfinite(b.Z1) || !std::isfinite(b.Z2)) b.Z1 = b.Z2 = 0.0;
}
} // namespace

ConvolutionReverb::ConvolutionReverb(int sampleRate, bool threaded) : m_Rate(sampleRate < 8000 ? 8000 : sampleRate) {
    // the ring holds the longest pre-delay plus a chunk (and the interpolation tap): the next power of two above that
    const unsigned need = (unsigned)std::ceil(kMaxPreDelayMs * 0.001 * (double)m_Rate) + (unsigned)kChunk + 2u;
    m_RingSize = 1024u;
    while (m_RingSize < need) m_RingSize <<= 1;
    for (auto& p : m_Inst) p.store(nullptr);
    if (threaded) m_Worker = std::make_unique<TailWorker>();
}

ConvolutionReverb::~ConvolutionReverb() {
    FreeRetired(true);
    for (auto& p : m_Inst) delete p.load(); // (each Convolver waits, bounded, for its queued tail blocks)
    // the worker (declared before the instances' use) joins last
}

// Frees the replaced instances the audio thread can no longer be using: one retired while it was outside Process() never is; one
// retired while inside is safe once the epoch has moved on.
void ConvolutionReverb::FreeRetired(bool all) {
    const unsigned now = m_Epoch.load(std::memory_order_seq_cst);
    for (size_t i = 0; i < m_Retired.size();) {
        const unsigned e = m_Retired[i].second;
        if (all || (e & 1u) == 0 || now != e) {
            delete m_Retired[i].first;
            m_Retired.erase(m_Retired.begin() + (std::ptrdiff_t)i);
        } else {
            ++i;
        }
    }
}

int ConvolutionReverb::AddIr(std::shared_ptr<const IrData> ir, unsigned keepMask) {
    if (!ir || ir->Length <= 0) return -1;
    FreeRetired(false);
    int idx = m_Count.load(std::memory_order_relaxed);
    Inst* old = nullptr;
    if (idx >= kMaxInstances) {
        // Every slot is taken: reuse the least recently targeted one that is idle (faded out and drained), is not named by the
        // target, and whose latest target the audio thread has already picked up (so it is not about to start it).
        idx = -1;
        std::lock_guard<std::mutex> lock(m_Mail);
        if (m_Seen.load(std::memory_order_acquire) != m_Version.load(std::memory_order_acquire)) return -1;
        for (int k = 0; k < kMaxInstances; ++k) {
            Inst* s = m_Inst[k].load(std::memory_order_acquire);
            if (!s || (keepMask >> k & 1u) || s->OnReport.load(std::memory_order_acquire) || !s->Conv->Idle()) continue;
            bool named = false;
            for (int l = 0; l < m_Pending.Count && l < 2; ++l) named |= m_Pending.Layers[l].Instance == k;
            if (named) continue;
            if (idx < 0 || (int)(s->LastUse - old->LastUse) < 0) {
                idx = k;
                old = s;
            }
        }
        if (idx < 0) return -1;
    }
    auto* s = new Inst();
    s->Conv = std::make_unique<Convolver>(std::move(ir), m_Worker.get());
    s->Ring.assign(m_RingSize, 0.0f);
    s->LastUse = ++m_UseClock;
    if (old) {
        // (seq_cst both: the epoch read must not move ahead of the swap, or a Process() entered meanwhile could still hold the old one)
        m_Inst[idx].store(s, std::memory_order_seq_cst);
        m_Retired.push_back({old, m_Epoch.load(std::memory_order_seq_cst)});
    } else {
        m_Inst[idx].store(s, std::memory_order_release);
        m_Count.store(idx + 1, std::memory_order_release);
    }
    return idx;
}

void ConvolutionReverb::Silence() {
    const int count = m_Count.load(std::memory_order_acquire);
    for (int k = 0; k < count; ++k) {
        Inst* s = m_Inst[k].load(std::memory_order_acquire);
        if (!s) continue;
        s->On = false; // (the next start waits for the convolver to drain, then resets it, the history and the filters)
        s->W = 0.0f;
        s->WReport.store(0.0f, std::memory_order_relaxed);
        s->OnReport.store(false, std::memory_order_relaxed);
    }
}

void ConvolutionReverb::SetTarget(const ReverbRtTarget& t) {
    std::lock_guard<std::mutex> lock(m_Mail);
    m_Pending = t;
    ++m_UseClock;
    for (int l = 0; l < t.Count && l < 2; ++l)
        if (t.Layers[l].Instance >= 0 && t.Layers[l].Instance < kMaxInstances)
            if (Inst* s = m_Inst[t.Layers[l].Instance].load(std::memory_order_acquire)) s->LastUse = m_UseClock;
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
    const unsigned mask = m_RingSize - 1;
    for (int k = 0; k < count; ++k) {
        Inst* s = m_Inst[k].load(std::memory_order_acquire);
        if (!s) continue;
        // what the target wants of this IR; one the target no longer names keeps its level and filters while its weight fades out
        // (only the weight carries the crossfade: gliding the level to 0 as well would dip the sum)
        float tw = 0.0f, tWet = s->Wet, tPd = s->Delay * 1000.0f / sr, tHf = s->Hf, tLc = s->Lc;
        float aWet = 0.0f, aPd = 0.0f, aHf = 0.0f, aLc = 0.0f; // the layers naming this IR, weight-weighted (two layers may share one)
        bool named = false;
        for (int l = 0; l < m_Cur.Count && l < 2; ++l) {
            const ReverbRtLayer& L = m_Cur.Layers[l];
            if (L.Instance != k) continue;
            const float lw = std::max(L.Weight, 0.0f);
            tw += lw;
            aWet += lw * L.WetLin;
            aPd += lw * std::clamp(L.PreDelayMs, 0.0f, kMaxPreDelayMs);
            aHf += lw * std::max(L.HfDampDb, 0.0f);
            aLc += lw * std::max(L.LowCutHz, 0.0f);
            tWet = L.WetLin; // (all-zero weights: the last layer's values stand)
            tPd = std::clamp(L.PreDelayMs, 0.0f, kMaxPreDelayMs);
            tHf = std::max(L.HfDampDb, 0.0f);
            tLc = std::max(L.LowCutHz, 0.0f);
            named = true;
        }
        if (named && tw > 1e-6f) {
            tWet = aWet / tw;
            tPd = aPd / tw;
            tHf = aHf / tw;
            tLc = aLc / tw;
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
            s->Ring[s->Pos & mask] = mono[i];
            const float step = std::clamp(tDelay - s->Delay, -kDelaySlew, kDelaySlew);
            s->Delay += step;
            const unsigned di = (unsigned)s->Delay;     // whole frames of delay, then the fraction (interpolated)
            const float fr = s->Delay - (float)di;
            const float a = s->Ring[(s->Pos - di) & mask], b = s->Ring[(s->Pos - di - 1) & mask];
            const double v = (double)(a + (b - a) * fr);
            x[i] = (float)s->HighPass.Run(s->Shelf.Run(v));
            ++s->Pos;
        }
        ScrubBiquad(s->Shelf); // a non-finite filter state would otherwise feed the convolver (and its tail) forever
        ScrubBiquad(s->HighPass);
        for (int i = 0; i < n; ++i)
            if (!std::isfinite(x[i])) x[i] = 0.0f;
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
    struct EpochScope { // (AddIr frees a replaced instance only once the audio thread is outside Process)
        std::atomic<unsigned>& E;
        explicit EpochScope(std::atomic<unsigned>& e) : E(e) { E.fetch_add(1, std::memory_order_seq_cst); }
        ~EpochScope() { E.fetch_add(1, std::memory_order_seq_cst); }
    } epoch(m_Epoch);
    {
        std::unique_lock<std::mutex> lock(m_Mail, std::try_to_lock); // never wait for the game thread
        if (lock.owns_lock() && m_Version.load(std::memory_order_acquire) != m_Seen.load(std::memory_order_relaxed)) {
            m_Cur = m_Pending;
            m_Seen.store(m_Version.load(std::memory_order_acquire), std::memory_order_release);
        }
    }
    float mono[kChunk], l[kChunk], r[kChunk];
    for (int off = 0; off < frames; off += kChunk) {
        const int n = std::min(kChunk, frames - off);
        for (int i = 0; i < n; ++i) {
            const float m = 0.5f * (in[2 * (size_t)(off + i)] + in[2 * (size_t)(off + i) + 1]);
            mono[i] = std::isfinite(m) ? m : 0.0f; // (a NaN / Inf sample would sit in the pre-delay ring and the convolver history)
            l[i] = r[i] = 0.0f;
        }
        ProcessChunk(mono, l, r, n);
        for (int i = 0; i < n; ++i) {
            out[2 * (size_t)(off + i)] = l[i];
            out[2 * (size_t)(off + i) + 1] = r[i];
        }
    }
}
