#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

// BS.1770 loudness and a peak meter, for the editor's Audio panel and the --audio-test report. Pure DSP.
//
// K-weighting (the high-shelf pre-filter and the RLB high-pass, derived for any sample rate), channel weights 1 / 1, 100 ms
// blocks: momentary = the last 4 blocks (400 ms), short-term = the last 30 (3 s), both un-gated. The peak is the loudest sample of
// the last half second. The readouts are atomics, safe to read from another thread than the one calling Process().

class LoudnessMeter {
public:
    explicit LoudnessMeter(int sampleRate = 48000);
    void Reset();
    // Interleaved stereo.
    void Process(const float* in, int frames);
    float PeakDb() const { return m_PeakDb.load(std::memory_order_relaxed); }
    float MomentaryLufs() const { return m_Momentary.load(std::memory_order_relaxed); }
    float ShortTermLufs() const { return m_ShortTerm.load(std::memory_order_relaxed); }

    // Offline: the loudest 400 ms window (hop 100 ms, un-gated) of frames [from, to) of an interleaved stereo buffer, LUFS-M max;
    // -120 when the range is shorter than a window or silent. `energy` (optional) receives the mean-square K-weighted energy of the
    // whole range.
    static float MomentaryMax(const std::vector<float>& stereo, int sampleRate, size_t from, size_t to);
    static double KWeightedEnergy(const std::vector<float>& stereo, int sampleRate); // sum of squares after K-weighting, both channels
    // BS.1770-4 integrated loudness (400 ms blocks, 75 % overlap, absolute gate -70 LUFS, relative gate -10 LU); -120 when silent.
    static float Integrated(const std::vector<float>& stereo, int sampleRate);
    // EBU Tech 3342 loudness range: the 10th to 95th percentile of the 3 s short-term loudness (1 s hop), gated at -70 and -20 LU.
    static float ShortTermRange(const std::vector<float>& stereo, int sampleRate);
    static float RmsDb(const std::vector<float>& stereo, size_t from, size_t to); // plain RMS of both channels, dBFS
    static float PeakDb(const std::vector<float>& stereo, size_t from, size_t to);

private:
    struct Biquad {
        double B0 = 1, B1 = 0, B2 = 0, A1 = 0, A2 = 0, Z1 = 0, Z2 = 0;
        double Run(double x) {
            const double y = B0 * x + Z1;
            Z1 = B1 * x - A1 * y + Z2;
            Z2 = B2 * x - A2 * y;
            return y;
        }
    };
    static void Design(int rate, Biquad& shelf, Biquad& hp);
    static std::vector<double> BlockPowers(const std::vector<float>& stereo, int sampleRate); // K-weighted mean square per 100 ms
    int m_Rate, m_Block;
    Biquad m_Shelf[2], m_Hp[2];
    double m_Sum = 0.0;
    int m_InBlock = 0;
    float m_BlockPeak = 0.0f;
    double m_Blocks[30] = {};
    float m_Peaks[5] = {};
    int m_NumBlocks = 0, m_Head = 0;
    std::atomic<float> m_PeakDb{-120.0f}, m_Momentary{-120.0f}, m_ShortTerm{-120.0f};
};
