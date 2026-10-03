#pragma once

#include "Fft.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Partitioned FFT convolution of a mono signal with a stereo impulse response (the engine's reverb: Audio/ConvolutionReverb).
// Pure DSP, no miniaudio, no allocation while processing.
//
// Two stages, zero latency overall:
//  * the HEAD, on the caller's (audio) thread. Taps 0 .. 255 are convolved directly in the time domain, taps 256 .. 8191 as
//    uniform 256-frame partitions (FFT 512, a frequency-domain delay line); a block's FFT work is done as the block completes and
//    is used by the NEXT block, which is why the first partition starts at tap 256.
//  * the TAIL (taps 8192 and up): 4096-frame partitions (FFT 8192). One block of input is handed to a worker thread as it
//    completes (TailWorker); the result is needed two blocks later (the tail starts 8192 = 2 x 4096 taps in), so the worker has a
//    whole block (85 ms at 48 kHz) to finish and the audio callback never does the big transforms. A block the worker has not
//    finished in time is skipped (silence for that tail block, counted in LateBlocks()) rather than waited for.
// Without a worker (Convolver(ir, nullptr) - tests, offline rendering) the tail block runs inline when it completes, so the
// output is exact and deterministic.

namespace Conv {
constexpr int kHead = 256;                 // head block (frames)
constexpr int kTail = 4096;                // tail partition (frames)
constexpr int kTailStart = 2 * kTail;      // first tap of the tail stage; a multiple of kHead
constexpr int kHeadParts = kTailStart / kHead - 1; // FFT partitions of the head stage (taps kHead .. kTailStart - 1)
// A spectrum is stored planar (all real parts, then all imaginary parts), each padded to a multiple of 4 bins for the SIMD loops.
constexpr int kHeadStride = ((kHead + 1) + 3) & ~3;
constexpr int kTailStride = ((kTail + 1) + 3) & ~3;
} // namespace Conv

struct IrBuildOptions {
    bool Normalize = true;        // scale so the mean energy of the two channels is 1 (wet / dry then follows the send x wet gain)
    float MaxSeconds = 8.0f;      // longer files are cut here (a fade hides the cut)
    float FadeSeconds = 0.02f;    // cosine fade-out over the last part of the response (0 = none)
};

// An impulse response prepared for convolution: immutable, shared by every Convolver that uses it.
struct IrData {
    int Rate = 48000;
    int Length = 0;                              // frames kept
    float Gain = 1.0f;                           // the normalisation that was applied
    std::vector<float> Direct[2];                // taps 0 .. kHead - 1, REVERSED (for a dot product against the input history)
    int HeadParts = 0;
    std::vector<float> HeadSpec[2];              // HeadParts x (2 x kHeadStride), planar
    int TailParts = 0;
    std::vector<float> TailSpec[2];              // TailParts x (2 x kTailStride), planar
    // `right` may be null (mono: both channels get the left). `count` frames at `rate`.
    static std::shared_ptr<IrData> FromSamples(const float* left, const float* right, int count, int rate, const IrBuildOptions& opt = IrBuildOptions());
    float Seconds() const { return Rate > 0 ? (float)Length / (float)Rate : 0.0f; }
};

class Convolver;

// The thread the tails run on: one per reverb bus, shared by its convolvers.
class TailWorker {
public:
    TailWorker();
    ~TailWorker();
    bool Post(Convolver* c, std::int64_t block); // audio thread: never blocks, never allocates; false when the queue is full
    std::uint64_t Jobs() const { return m_Jobs.load(std::memory_order_relaxed); }
    double TotalMicros() const { return m_Micros.load(std::memory_order_relaxed); }

private:
    struct Job { Convolver* C; std::int64_t Block; };
    static constexpr int kCap = 64;
    Job m_Ring[kCap];
    std::atomic<unsigned> m_Head{0}, m_TailIdx{0};
    std::atomic<bool> m_Run{true};
    std::atomic<std::uint64_t> m_Jobs{0};
    std::atomic<double> m_Micros{0.0};
    std::mutex m_M;
    std::condition_variable m_Cv;
    std::thread m_Thread;
    void Loop();
};

class Convolver {
public:
    // `worker` null: the tail runs inline (exact, deterministic).
    Convolver(std::shared_ptr<const IrData> ir, TailWorker* worker);
    ~Convolver();
    Convolver(const Convolver&) = delete;
    Convolver& operator=(const Convolver&) = delete;

    // Forgets the input history and everything in flight. Only when Idle().
    void Reset();
    bool Idle() const { return m_InFlight.load(std::memory_order_acquire) == 0; }
    // `frames` (any count) of mono input -> the left / right convolution (overwritten).
    void Process(const float* in, float* outL, float* outR, int frames);
    const IrData& Ir() const { return *m_Ir; }
    std::uint64_t LateBlocks() const { return m_Late.load(std::memory_order_relaxed); }
    // The worker side (TailWorker only).
    void RunTail(std::int64_t block);

private:
    std::shared_ptr<const IrData> m_Ir;
    TailWorker* m_Worker;
    RealFft m_HeadFft, m_TailFft;
    // head
    std::vector<float> m_Win;                    // [previous block | current block], 2 x kHead
    int m_Pos = 0;                               // frames of the current block written
    std::vector<float> m_HeadFdl;                // HeadParts spectra of past blocks
    std::int64_t m_HeadBlock = 0;
    std::vector<float> m_Rest[2];                // the FFT part of the head for the block being played (kHead each)
    std::vector<float> m_HeadAcc, m_HeadTmp, m_HeadInter;
    // tail
    std::vector<float> m_TailIn;                 // 4 blocks of input
    std::vector<float> m_TailOut[2];             // 3 blocks of output per channel
    std::vector<float> m_TailFdl, m_TailWin, m_TailAcc, m_TailTmp, m_TailInter;
    std::int64_t m_Total = 0;                    // input frames so far
    std::atomic<std::int64_t> m_Done{-1};        // highest tail block the worker finished
    std::atomic<int> m_InFlight{0};
    std::atomic<std::uint64_t> m_Late{0};
    bool m_TailLate = false;                     // the current output block's tail is missing
    void HeadBlockDone();
};
