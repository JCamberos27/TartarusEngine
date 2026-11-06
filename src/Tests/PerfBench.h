#pragma once

#include <chrono>
#include <string>
#include <utility>
#include <vector>

// --perf-bench's measuring: every frame's time and the profiler's CPU / GPU scopes over a phase, the worst frames (with
// what each spent its time on), and the two reports (a scene's edit / play / play-max phase, and the --npc-test combat
// bench). Driven from SmokeHarness; the control flow around it (Play toggles, phase boundaries) stays with the caller.
class PerfBench {
public:
    // --perf-bench measures each scene twice: kPhaseFrames in edit mode, then as many in Play.
    static constexpr int kPhaseFrames = 600;

    // Warmup is the frames skipped at the start of each phase. TARTARUS_PERF_WARMUP=0 keeps them, to measure first-use hitches.
    PerfBench();

    int Warmup() const { return m_Warmup; }
    // Milliseconds since the last call (or construction), and hands the frame time to the sampling profiler.
    double TakeFrameMs();
    // Adds one measured frame: its time, the profiler's last-frame CPU and GPU scopes, and the stutter record.
    void Sample(double frameMs, int frameNo);
    // Frames sampled since the last report.
    int Samples() const { return m_Frame.N; }

    // The combat bench's one report (frames, hitches, then the CPU / GPU table), then clears the phase.
    void ReportCombat(const std::string& sceneFile, int draws, int tris, int width, int height);
    // A phase's report: the summary line, textures, the CPU / GPU table, hitches, then (perfSample) the sampling report,
    // then clears the phase. `mode` is "edit", "play" or "play-max".
    void ReportPhase(const std::string& sceneFile, const char* mode, int draws, int tris, int width, int height, bool perfSample);

private:
    struct Accum { double Sum = 0.0; double Max = 0.0; int N = 0; };
    using AccumList = std::vector<std::pair<std::string, Accum>>;
    // Stutters: the worst frames with what that frame spent its time on (the per-scope maxima are independent, so they
    // can't say which scope made a hitch).
    struct Worst { double Ms; int Frame; std::string Scopes; };

    static void Add(AccumList& v, const std::string& name, double ms);
    static std::string ScopesNow();
    void Settle(bool haveScopes);
    void RecordFrame(double frameMs, int frame);
    void ReportHitches(const char* label);
    void PrintScopeTable();

    int m_Warmup = 0;
    AccumList m_Cpu, m_Gpu;
    Accum m_Frame;
    std::chrono::steady_clock::time_point m_LastFrameEnd;
    std::vector<double> m_FrameTimes;
    std::vector<Worst> m_Worst;
    Worst m_Pending{-1.0, 0, {}};
};
