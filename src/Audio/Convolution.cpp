#include "Convolution.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
#include <emmintrin.h>
#define CONV_SSE 1
#endif

namespace {

using namespace Conv;

// interleaved (re, im) bins -> planar [re | im] with the given stride
void ToPlanar(const float* inter, float* planar, int bins, int stride) {
    float* re = planar;
    float* im = planar + stride;
    for (int k = 0; k < bins; ++k) {
        re[k] = inter[2 * k];
        im[k] = inter[2 * k + 1];
    }
    for (int k = bins; k < stride; ++k) re[k] = im[k] = 0.0f;
}
void ToInterleaved(const float* planar, float* inter, int bins, int stride) {
    const float* re = planar;
    const float* im = planar + stride;
    for (int k = 0; k < bins; ++k) {
        inter[2 * k] = re[k];
        inter[2 * k + 1] = im[k];
    }
}

// acc += x * h over `stride` planar bins (complex multiply-accumulate)
inline void Mac(float* acc, const float* x, const float* h, int stride) {
    float* accR = acc;
    float* accI = acc + stride;
    const float* xr = x;
    const float* xi = x + stride;
    const float* hr = h;
    const float* hi = h + stride;
#ifdef CONV_SSE
    for (int k = 0; k < stride; k += 4) {
        const __m128 a = _mm_loadu_ps(xr + k), b = _mm_loadu_ps(xi + k), c = _mm_loadu_ps(hr + k), d = _mm_loadu_ps(hi + k);
        __m128 r = _mm_loadu_ps(accR + k), i = _mm_loadu_ps(accI + k);
        r = _mm_add_ps(r, _mm_sub_ps(_mm_mul_ps(a, c), _mm_mul_ps(b, d)));
        i = _mm_add_ps(i, _mm_add_ps(_mm_mul_ps(a, d), _mm_mul_ps(b, c)));
        _mm_storeu_ps(accR + k, r);
        _mm_storeu_ps(accI + k, i);
    }
#else
    for (int k = 0; k < stride; ++k) {
        accR[k] += xr[k] * hr[k] - xi[k] * hi[k];
        accI[k] += xr[k] * hi[k] + xi[k] * hr[k];
    }
#endif
}

// dot of x[0 .. n) with two tap sets (n a multiple of 8)
inline void Dot2(const float* x, const float* hl, const float* hr, int n, float& outL, float& outR) {
#ifdef CONV_SSE
    __m128 l0 = _mm_setzero_ps(), r0 = _mm_setzero_ps(), l1 = _mm_setzero_ps(), r1 = _mm_setzero_ps();
    for (int k = 0; k < n; k += 8) {
        const __m128 x0 = _mm_loadu_ps(x + k), x1 = _mm_loadu_ps(x + k + 4);
        l0 = _mm_add_ps(l0, _mm_mul_ps(x0, _mm_loadu_ps(hl + k)));
        r0 = _mm_add_ps(r0, _mm_mul_ps(x0, _mm_loadu_ps(hr + k)));
        l1 = _mm_add_ps(l1, _mm_mul_ps(x1, _mm_loadu_ps(hl + k + 4)));
        r1 = _mm_add_ps(r1, _mm_mul_ps(x1, _mm_loadu_ps(hr + k + 4)));
    }
    l0 = _mm_add_ps(l0, l1);
    r0 = _mm_add_ps(r0, r1);
    float tl[4], tr[4];
    _mm_storeu_ps(tl, l0);
    _mm_storeu_ps(tr, r0);
    outL = (tl[0] + tl[1]) + (tl[2] + tl[3]);
    outR = (tr[0] + tr[1]) + (tr[2] + tr[3]);
#else
    float l = 0.0f, r = 0.0f;
    for (int k = 0; k < n; ++k) {
        l += x[k] * hl[k];
        r += x[k] * hr[k];
    }
    outL = l;
    outR = r;
#endif
}

} // namespace

// --- IrData ------------------------------------------------------------------------------------------------------------

std::shared_ptr<IrData> IrData::FromSamples(const float* left, const float* right, int count, int rate, const IrBuildOptions& opt) {
    auto ir = std::make_shared<IrData>();
    ir->Rate = rate;
    if (!left || count <= 0) return ir;
    const int maxFrames = opt.MaxSeconds > 0.0f ? (int)(opt.MaxSeconds * (float)rate) : count;
    const int n = std::min(count, std::max(maxFrames, 1));
    ir->Length = n;
    std::vector<float> h[2];
    h[0].assign(left, left + n);
    h[1].assign(right ? right : left, (right ? right : left) + n);
    const int fade = std::min((int)(opt.FadeSeconds * (float)rate), n / 2);
    for (int i = 0; i < fade; ++i) {
        const float t = (float)(i + 1) / (float)(fade + 1); // 0 -> 1 over the fade
        const float g = 0.5f * (1.0f + std::cos(3.14159265f * t));
        h[0][(size_t)(n - fade + i)] *= g;
        h[1][(size_t)(n - fade + i)] *= g;
    }
    float gain = 1.0f;
    if (opt.Normalize) {
        double e = 0.0;
        for (int c = 0; c < 2; ++c)
            for (float v : h[c]) e += (double)v * v;
        e *= 0.5;
        if (e > 1e-20) gain = (float)(1.0 / std::sqrt(e));
    }
    ir->Gain = gain;
    for (int c = 0; c < 2; ++c)
        for (float& v : h[c]) v *= gain;

    ir->HeadParts = n > kHead ? std::min(kHeadParts, (n - kHead + kHead - 1) / kHead) : 0;
    ir->TailParts = n > kTailStart ? (n - kTailStart + kTail - 1) / kTail : 0;
    RealFft headFft(2 * kHead), tailFft(2 * kTail);
    std::vector<float> win(2 * (size_t)kTail), spec(2 * (size_t)(kTail + 1));
    for (int c = 0; c < 2; ++c) {
        ir->Direct[c].assign((size_t)kHead, 0.0f);
        for (int k = 0; k < std::min(n, kHead); ++k) ir->Direct[c][(size_t)(kHead - 1 - k)] = h[c][(size_t)k];
        ir->HeadSpec[c].assign((size_t)ir->HeadParts * 2 * kHeadStride, 0.0f);
        for (int m = 0; m < ir->HeadParts; ++m) {
            std::fill(win.begin(), win.begin() + 2 * kHead, 0.0f);
            const int t0 = kHead + m * kHead;
            for (int t = 0; t < kHead && t0 + t < n; ++t) win[(size_t)t] = h[c][(size_t)(t0 + t)];
            headFft.Forward(win.data(), spec.data());
            ToPlanar(spec.data(), ir->HeadSpec[c].data() + (size_t)m * 2 * kHeadStride, kHead + 1, kHeadStride);
        }
        ir->TailSpec[c].assign((size_t)ir->TailParts * 2 * kTailStride, 0.0f);
        for (int m = 0; m < ir->TailParts; ++m) {
            std::fill(win.begin(), win.end(), 0.0f);
            const int t0 = kTailStart + m * kTail;
            for (int t = 0; t < kTail && t0 + t < n; ++t) win[(size_t)t] = h[c][(size_t)(t0 + t)];
            tailFft.Forward(win.data(), spec.data());
            ToPlanar(spec.data(), ir->TailSpec[c].data() + (size_t)m * 2 * kTailStride, kTail + 1, kTailStride);
        }
    }
    return ir;
}

// --- TailWorker --------------------------------------------------------------------------------------------------------

TailWorker::TailWorker() : m_Thread([this] { Loop(); }) {}

TailWorker::~TailWorker() {
    m_Run.store(false);
    m_Cv.notify_all();
    if (m_Thread.joinable()) m_Thread.join();
}

bool TailWorker::Post(Convolver* c, std::int64_t block) {
    const unsigned h = m_Head.load(std::memory_order_relaxed);
    const unsigned t = m_TailIdx.load(std::memory_order_acquire);
    if (h - t >= (unsigned)kCap) return false;
    m_Ring[h % kCap] = {c, block};
    m_Head.store(h + 1, std::memory_order_release);
    m_Cv.notify_one(); // (no lock taken: the worker also wakes on its own every few ms)
    return true;
}

void TailWorker::Loop() {
    while (m_Run.load()) {
        const unsigned t = m_TailIdx.load(std::memory_order_relaxed);
        if (t == m_Head.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(m_M);
            m_Cv.wait_for(lock, std::chrono::milliseconds(2));
            continue;
        }
        const Job job = m_Ring[t % kCap];
        const auto t0 = std::chrono::steady_clock::now();
        job.C->RunTail(job.Block);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        m_Micros.store(m_Micros.load(std::memory_order_relaxed) + us, std::memory_order_relaxed);
        m_Jobs.fetch_add(1, std::memory_order_relaxed);
        m_TailIdx.store(t + 1, std::memory_order_release);
    }
}

// --- Convolver ---------------------------------------------------------------------------------------------------------

Convolver::Convolver(std::shared_ptr<const IrData> ir, TailWorker* worker)
    : m_Ir(std::move(ir)), m_Worker(worker), m_HeadFft(2 * kHead), m_TailFft(2 * kTail) {
    m_Win.assign(2 * (size_t)kHead, 0.0f);
    m_HeadFdl.assign((size_t)std::max(m_Ir->HeadParts, 1) * 2 * kHeadStride, 0.0f);
    for (auto& r : m_Rest) r.assign((size_t)kHead, 0.0f);
    m_HeadAcc.assign(2 * (size_t)kHeadStride, 0.0f);
    m_HeadTmp.assign(2 * (size_t)kHead, 0.0f);
    m_HeadInter.assign(2 * (size_t)(kHead + 1), 0.0f);
    m_TailIn.assign(4 * (size_t)kTail, 0.0f);
    for (auto& o : m_TailOut) o.assign(3 * (size_t)kTail, 0.0f);
    m_TailFdl.assign((size_t)std::max(m_Ir->TailParts, 1) * 2 * kTailStride, 0.0f);
    m_TailWin.assign(2 * (size_t)kTail, 0.0f);
    m_TailAcc.assign(2 * (size_t)kTailStride, 0.0f);
    m_TailTmp.assign(2 * (size_t)kTail, 0.0f);
    m_TailInter.assign(2 * (size_t)(kTail + 1), 0.0f);
}

Convolver::~Convolver() {
    while (!Idle()) std::this_thread::yield(); // a queued tail block still points here
}

void Convolver::Reset() {
    std::fill(m_Win.begin(), m_Win.end(), 0.0f);
    std::fill(m_HeadFdl.begin(), m_HeadFdl.end(), 0.0f);
    for (auto& r : m_Rest) std::fill(r.begin(), r.end(), 0.0f);
    std::fill(m_TailIn.begin(), m_TailIn.end(), 0.0f);
    for (auto& o : m_TailOut) std::fill(o.begin(), o.end(), 0.0f);
    std::fill(m_TailFdl.begin(), m_TailFdl.end(), 0.0f);
    m_Pos = 0;
    m_HeadBlock = 0;
    m_Total = 0;
    m_Done.store(-1, std::memory_order_release);
    m_TailLate = false;
}

void Convolver::HeadBlockDone() {
    const int parts = m_Ir->HeadParts;
    if (parts > 0) {
        float* slot = m_HeadFdl.data() + (size_t)(m_HeadBlock % parts) * 2 * kHeadStride;
        m_HeadFft.Forward(m_Win.data(), m_HeadInter.data());
        ToPlanar(m_HeadInter.data(), slot, kHead + 1, kHeadStride);
        for (int c = 0; c < 2; ++c) {
            std::fill(m_HeadAcc.begin(), m_HeadAcc.end(), 0.0f);
            for (int m = 0; m < parts; ++m) {
                const int s = (int)(((m_HeadBlock - m) % parts + parts) % parts);
                Mac(m_HeadAcc.data(), m_HeadFdl.data() + (size_t)s * 2 * kHeadStride, m_Ir->HeadSpec[c].data() + (size_t)m * 2 * kHeadStride, kHeadStride);
            }
            ToInterleaved(m_HeadAcc.data(), m_HeadInter.data(), kHead + 1, kHeadStride);
            m_HeadFft.Inverse(m_HeadInter.data(), m_HeadTmp.data());
            std::memcpy(m_Rest[c].data(), m_HeadTmp.data() + kHead, sizeof(float) * (size_t)kHead); // the valid half
        }
    }
    std::memcpy(m_Win.data(), m_Win.data() + kHead, sizeof(float) * (size_t)kHead); // current becomes previous
    m_Pos = 0;
    ++m_HeadBlock;
}

void Convolver::RunTail(std::int64_t block) {
    const int parts = m_Ir->TailParts;
    if (parts > 0) {
        const float* cur = m_TailIn.data() + (size_t)(block % 4) * kTail;
        if (block > 0) std::memcpy(m_TailWin.data(), m_TailIn.data() + (size_t)((block - 1) % 4) * kTail, sizeof(float) * (size_t)kTail);
        else std::fill(m_TailWin.begin(), m_TailWin.begin() + kTail, 0.0f);
        std::memcpy(m_TailWin.data() + kTail, cur, sizeof(float) * (size_t)kTail);
        float* slot = m_TailFdl.data() + (size_t)(block % parts) * 2 * kTailStride;
        m_TailFft.Forward(m_TailWin.data(), m_TailInter.data());
        ToPlanar(m_TailInter.data(), slot, kTail + 1, kTailStride);
        for (int c = 0; c < 2; ++c) {
            std::fill(m_TailAcc.begin(), m_TailAcc.end(), 0.0f);
            for (int m = 0; m < parts; ++m) {
                const int s = (int)(((block - m) % parts + parts) % parts);
                Mac(m_TailAcc.data(), m_TailFdl.data() + (size_t)s * 2 * kTailStride, m_Ir->TailSpec[c].data() + (size_t)m * 2 * kTailStride, kTailStride);
            }
            ToInterleaved(m_TailAcc.data(), m_TailInter.data(), kTail + 1, kTailStride);
            m_TailFft.Inverse(m_TailInter.data(), m_TailTmp.data());
            std::memcpy(m_TailOut[c].data() + (size_t)(block % 3) * kTail, m_TailTmp.data() + kTail, sizeof(float) * (size_t)kTail);
        }
    }
    m_Done.store(block, std::memory_order_release);
    m_InFlight.fetch_sub(1, std::memory_order_release);
}

void Convolver::Process(const float* in, float* outL, float* outR, int frames) {
    const IrData& ir = *m_Ir;
    const bool tail = ir.TailParts > 0;
    int done = 0;
    while (done < frames) {
        const int c = std::min(frames - done, kHead - m_Pos);
        const std::int64_t tailBlock = m_Total / kTail;
        const int tpos0 = (int)(m_Total % kTail);
        if (tpos0 == 0 && tail && tailBlock >= 2) { // a new output block: has the worker finished the block that is due?
            m_TailLate = m_Done.load(std::memory_order_acquire) < tailBlock - 2;
            if (m_TailLate) m_Late.fetch_add(1, std::memory_order_relaxed);
        }
        const bool addTail = tail && tailBlock >= 2 && !m_TailLate;
        const float* tl = addTail ? m_TailOut[0].data() + (size_t)((tailBlock - 2) % 3) * kTail + tpos0 : nullptr;
        const float* tr = addTail ? m_TailOut[1].data() + (size_t)((tailBlock - 2) % 3) * kTail + tpos0 : nullptr;
        float* tin = m_TailIn.data() + (size_t)(tailBlock % 4) * kTail + tpos0;
        for (int i = 0; i < c; ++i) {
            const int pos = m_Pos + i;
            const float x = in[done + i];
            m_Win[(size_t)(kHead + pos)] = x;
            tin[i] = x;
            float yl, yr;
            Dot2(m_Win.data() + pos + 1, ir.Direct[0].data(), ir.Direct[1].data(), kHead, yl, yr);
            yl += m_Rest[0][(size_t)pos];
            yr += m_Rest[1][(size_t)pos];
            if (addTail) {
                yl += tl[i];
                yr += tr[i];
            }
            outL[done + i] = yl;
            outR[done + i] = yr;
        }
        done += c;
        m_Total += c;
        m_Pos += c;
        if (m_Pos == kHead) HeadBlockDone();
        if (m_Total % kTail == 0 && tail) { // a tail block is complete: hand it over
            const std::int64_t b = m_Total / kTail - 1;
            m_InFlight.fetch_add(1, std::memory_order_acq_rel);
            if (m_Worker) {
                if (!m_Worker->Post(this, b)) m_InFlight.fetch_sub(1, std::memory_order_acq_rel); // queue full: this block is lost
            } else {
                RunTail(b);
            }
        }
    }
}
