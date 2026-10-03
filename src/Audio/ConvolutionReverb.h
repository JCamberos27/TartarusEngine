#pragma once

#include "Convolution.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// The engine's runtime reverb: a send bus every voice with a reverb send feeds, convolved with a real recorded impulse response
// (stereo, from a mono sum of the send) and summed into the output. Pure DSP (no miniaudio): the audio callback and the unit tests
// run the very same code. Wet only; the dry signal never passes through here.
//
// A "space" is up to two weighted layers (the two heaviest impulse responses of the listener's zones / probe). Each layer is a
// registered IR (AddIr) with its own return level, pre-delay, high-frequency damping (a high shelf on the input) and low cut.
// SetTarget() only states where the reverb is heading: the callback glides there. Changing the space crossfades the layers
// (equal power: the gain of a layer is sqrt(weight) x its level) and a convolver only runs while its layer is audible, so the
// second one costs nothing outside a crossfade. A convolver that comes back after being silent starts from an empty history.

struct ReverbRtLayer {
    int Instance = -1;       // the IR (AddIr's index)
    float Weight = 1.0f;     // crossfade weight; the layers' weights sum to 1
    float WetLin = 1.0f;     // linear return level of this layer (sqrt(weight) is applied on top)
    float PreDelayMs = 0.0f;
    float HfDampDb = 0.0f;   // attenuation (dB, >= 0) of a high shelf at 4 kHz on the reverb's input
    float LowCutHz = 0.0f;   // 2nd-order high-pass on the input (0 / below 20 = effectively off)
};
struct ReverbRtTarget {
    int Count = 0;           // layers used (0 = silent reverb: everything fades out)
    ReverbRtLayer Layers[2];
    float GlideSeconds = 0.35f; // how long a change of space takes (the crossfade time)
};

class ConvolutionReverb {
public:
    static constexpr int kMaxInstances = 12;
    // threaded: tail blocks run on a worker thread (the game); not threaded: inline, exact and deterministic (tests, offline).
    ConvolutionReverb(int sampleRate, bool threaded);
    ~ConvolutionReverb();
    ConvolutionReverb(const ConvolutionReverb&) = delete;
    ConvolutionReverb& operator=(const ConvolutionReverb&) = delete;

    // Game thread. Registers an IR (allocates its convolver and buffers); -1 when all slots are taken. Instances live as long as
    // the reverb.
    int AddIr(std::shared_ptr<const IrData> ir);
    int InstanceCount() const { return m_Count.load(std::memory_order_acquire); }
    // Game thread: where the reverb is heading (picked up by the callback's next block).
    void SetTarget(const ReverbRtTarget& t);
    ReverbRtTarget Target() const;
    // Audio thread: `frames` of interleaved stereo in -> the wet signal out (may alias).
    void Process(const float* in, float* out, int frames);

    struct Info {
        int Active = 0;                         // convolvers running
        int Instances = 0;
        bool On[kMaxInstances] = {};
        float Weight[kMaxInstances] = {};       // the layer's current (gliding) weight
    };
    Info GetInfo() const;
    std::uint64_t LateBlocks() const;           // tail blocks the worker had not finished in time
    std::uint64_t WorkerJobs() const { return m_Worker ? m_Worker->Jobs() : 0; }
    double WorkerMicros() const { return m_Worker ? m_Worker->TotalMicros() : 0.0; }
    int SampleRate() const { return m_Rate; }

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
    struct Inst {
        std::unique_ptr<Convolver> Conv;
        std::vector<float> Ring;               // input history for the pre-delay
        unsigned Pos = 0;
        bool On = false;
        float W = 0.0f, Wet = 0.0f, Delay = 0.0f, Hf = 0.0f, Lc = 0.0f; // gliding values (Delay in frames)
        float AppliedHf = -1.0f, AppliedLc = -1.0f;
        Biquad Shelf, HighPass;
        std::atomic<float> WReport{0.0f};
        std::atomic<bool> OnReport{false};
    };
    int m_Rate;
    std::unique_ptr<TailWorker> m_Worker;
    std::atomic<Inst*> m_Inst[kMaxInstances]; // published by AddIr (game thread), owned here
    std::atomic<int> m_Count{0};
    mutable std::mutex m_Mail;
    ReverbRtTarget m_Pending;
    std::atomic<unsigned> m_Version{0};
    unsigned m_Seen = 0;
    ReverbRtTarget m_Cur;
    void Design(Inst& s, float hfDb, float lowCutHz) const;
    void ProcessChunk(const float* mono, float* outL, float* outR, int n);
};
