#include "LoudnessMeter.h"

#include <algorithm>
#include <cmath>

namespace {
float ToLufs(double meanSquare) { return meanSquare > 1e-12 ? (float)(-0.691 + 10.0 * std::log10(meanSquare)) : -120.0f; }
float ToDb(double v) { return v > 1e-9 ? (float)(20.0 * std::log10(v)) : -180.0f; }
} // namespace

void LoudnessMeter::Design(int rate, Biquad& shelf, Biquad& hp) {
    const double pi = 3.14159265358979323846, sr = (double)rate;
    {   // pre-filter: high shelf, +4 dB at 1681.97 Hz, Q 0.7072
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(pi * f0 / sr), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.B0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf.B1 = 2.0 * (K * K - Vh) / a0;
        shelf.B2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.A1 = 2.0 * (K * K - 1.0) / a0;
        shelf.A2 = (1.0 - K / Q + K * K) / a0;
    }
    {   // RLB: high-pass at 38.13 Hz, Q 0.5003
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(pi * f0 / sr);
        const double a0 = 1.0 + K / Q + K * K;
        hp.B0 = 1.0 / a0;
        hp.B1 = -2.0 / a0;
        hp.B2 = 1.0 / a0;
        hp.A1 = 2.0 * (K * K - 1.0) / a0;
        hp.A2 = (1.0 - K / Q + K * K) / a0;
    }
}

LoudnessMeter::LoudnessMeter(int sampleRate) : m_Rate(sampleRate < 8000 ? 8000 : sampleRate), m_Block(m_Rate / 10) {
    for (int c = 0; c < 2; ++c) Design(m_Rate, m_Shelf[c], m_Hp[c]);
}

void LoudnessMeter::Reset() {
    for (int c = 0; c < 2; ++c) {
        m_Shelf[c].Z1 = m_Shelf[c].Z2 = m_Hp[c].Z1 = m_Hp[c].Z2 = 0.0;
    }
    m_Sum = 0.0;
    m_InBlock = 0;
    m_BlockPeak = 0.0f;
    std::fill(std::begin(m_Blocks), std::end(m_Blocks), 0.0);
    std::fill(std::begin(m_Peaks), std::end(m_Peaks), 0.0f);
    m_NumBlocks = m_Head = 0;
    m_PeakDb.store(-120.0f);
    m_Momentary.store(-120.0f);
    m_ShortTerm.store(-120.0f);
}

void LoudnessMeter::Process(const float* in, int frames) {
    for (int f = 0; f < frames; ++f) {
        for (int c = 0; c < 2; ++c) {
            const float x = in[2 * (size_t)f + (size_t)c];
            m_BlockPeak = std::max(m_BlockPeak, std::fabs(x));
            const double y = m_Hp[c].Run(m_Shelf[c].Run((double)x));
            m_Sum += y * y;
        }
        if (++m_InBlock < m_Block) continue;
        // a 100 ms block is complete: its mean-square energy (both channels summed, weights 1)
        m_Blocks[m_Head] = m_Sum / (double)m_Block;
        m_Head = (m_Head + 1) % 30;
        m_NumBlocks = std::min(m_NumBlocks + 1, 30);
        for (int i = 4; i > 0; --i) m_Peaks[i] = m_Peaks[i - 1];
        m_Peaks[0] = m_BlockPeak;
        m_Sum = 0.0;
        m_InBlock = 0;
        m_BlockPeak = 0.0f;
        double e4 = 0.0, e30 = 0.0;
        for (int i = 0; i < m_NumBlocks; ++i) {
            const double e = m_Blocks[(m_Head - 1 - i + 60) % 30];
            if (i < 4) e4 += e;
            e30 += e;
        }
        m_Momentary.store(ToLufs(e4 / 4.0), std::memory_order_relaxed);
        m_ShortTerm.store(ToLufs(e30 / (double)std::max(m_NumBlocks, 1)), std::memory_order_relaxed);
        float pk = 0.0f;
        for (float p : m_Peaks) pk = std::max(pk, p);
        m_PeakDb.store(pk > 1e-6f ? 20.0f * std::log10(pk) : -120.0f, std::memory_order_relaxed);
    }
}

float LoudnessMeter::MomentaryMax(const std::vector<float>& stereo, int sampleRate, size_t from, size_t to) {
    to = std::min(to, stereo.size() / 2);
    const size_t block = (size_t)(sampleRate / 10), window = 4 * block;
    if (from >= to || to - from < window) return -120.0f;
    Biquad shelf[2], hp[2];
    for (int c = 0; c < 2; ++c) Design(sampleRate, shelf[c], hp[c]);
    // run the filters from a little before the range so they have settled (1 s)
    const size_t warm = from > (size_t)sampleRate ? from - (size_t)sampleRate : 0;
    std::vector<double> blocks;
    size_t pos = warm;
    // (the blocks are aligned to `from`, the filter warm-up is discarded)
    for (; pos < from; ++pos)
        for (int c = 0; c < 2; ++c) hp[c].Run(shelf[c].Run((double)stereo[2 * pos + (size_t)c]));
    for (; pos + block <= to; pos += block) {
        double s = 0.0;
        for (size_t i = pos; i < pos + block; ++i)
            for (int c = 0; c < 2; ++c) {
                const double y = hp[c].Run(shelf[c].Run((double)stereo[2 * i + (size_t)c]));
                s += y * y;
            }
        blocks.push_back(s / (double)block);
    }
    float best = -120.0f;
    for (size_t b = 0; b + 4 <= blocks.size(); ++b) best = std::max(best, ToLufs((blocks[b] + blocks[b + 1] + blocks[b + 2] + blocks[b + 3]) / 4.0));
    return best;
}

float LoudnessMeter::RmsDb(const std::vector<float>& stereo, size_t from, size_t to) {
    to = std::min(to, stereo.size() / 2);
    if (from >= to) return -180.0f;
    double s = 0.0;
    for (size_t i = from; i < to; ++i) s += (double)stereo[2 * i] * stereo[2 * i] + (double)stereo[2 * i + 1] * stereo[2 * i + 1];
    return ToDb(std::sqrt(s / (2.0 * (double)(to - from))));
}

float LoudnessMeter::PeakDb(const std::vector<float>& stereo, size_t from, size_t to) {
    to = std::min(to, stereo.size() / 2);
    float p = 0.0f;
    for (size_t i = from; i < to; ++i) p = std::max({p, std::fabs(stereo[2 * i]), std::fabs(stereo[2 * i + 1])});
    return ToDb(p);
}
