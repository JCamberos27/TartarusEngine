#pragma once

#include "Core/FrameLoads.h"
#include "Core/Profiler.h"
#include "Tests/MachineLoad.h"
#include "Tests/PerfG0.h"
#include "Renderer/CascadedShadowMap.h"
#include "Renderer/InstanceBatch.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// --perf-bench's measuring: every frame's time and the profiler's CPU / GPU scopes over a phase, the worst frames (with
// what each spent its time on), and the two reports (a scene's edit / play / play-max phase, and the --npc-test combat
// bench). Driven from SmokeHarness; the control flow around it (Play toggles, phase boundaries) stays with the caller.
// Each worst frame lists its top CPU scopes, its top GPU scopes (attached once they're read back, a few frames late)
// and what loaded during it (Core/FrameLoads.h). TARTARUS_PERF_CSV also appends the hitch report to a CSV file.
class PerfBench {
public:
    // --perf-bench measures each scene twice: kPhaseFrames in edit mode, then as many in Play.
    static constexpr int kPhaseFrames = 600;

    // TARTARUS_PERF_PACED (A-3): measure the frames as a player gets them, paced by VSync ("vsync") or a software FPS
    // cap (a number), instead of --perf-bench's default of no VSync and no cap. Only a visible run (TARTARUS_SHOW=1)
    // presents; a hidden one keeps its fence pacing and ignores this. The phase report then adds a "paced" line: the
    // target interval, and the frames that came later than 1.5x it.
    struct Pacing {
        bool VSync = false;
        int Fps = 0;
        bool On() const { return VSync || Fps > 0; }
    };
    static const Pacing& Paced();

    // Warmup is the frames skipped at the start of each phase. TARTARUS_PERF_WARMUP=0 keeps them, to measure first-use hitches.
    PerfBench();

    int Warmup() const { return m_Warmup; }
    // Milliseconds since the last call (or construction), and hands the frame time to the sampling profiler. Also takes
    // the frame's load log, so loads in unmeasured (warm-up) frames don't pile onto the first measured one.
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

    // The frame's halves: named per-frame values ("cpu.sim", "gpu.frame", "p48.Scene Draw"...) reported per phase as
    // avg / p50 / p95 / p99 / max (the HALF lines, and "half" rows in TARTARUS_PERF_CSV). Each key keeps its own
    // samples, one per call, so a value that lands frames late (a GPU timer query) still counts once; a key's N can
    // trail the frame count by a few. Sample() adds the cpu.* keys itself, every frame.
    void AddHalf(const std::string& key, double ms);
    // The key's latest sample, or -1 when it has none this phase.
    double LastHalf(const std::string& key) const;
    // The cpu.* bucket a TOP-LEVEL CPU scope's time goes to ("sim", "anim", "physics", "ui", "audio", "assets", "render",
    // "wait", "editor", "input"), or "other" for a scope nobody mapped yet (the HALF report names those).
    static const char* CpuBucket(const std::string& scope);

private:
    struct Accum { double Sum = 0.0; double Max = 0.0; int N = 0; };
    using AccumList = std::vector<std::pair<std::string, Accum>>;
    // Stutters: the worst frames with what that frame spent its time on (the per-scope maxima are independent, so they
    // can't say which scope made a hitch).
    struct Worst {
        double Ms = -1.0;
        int Frame = 0;
        unsigned Serial = 0; // the profiler's frame, to find its GPU scopes once they're read back
        std::string Cpu, Gpu, Loads;
        bool HaveGpu = false;
        bool GpuStarved = false; // GpuStarvedFrame: its GPU scopes are mostly CPU time, not GPU work
        // The profiler's frame (BeginFrame to BeginFrame) and the part of it no top-level scope covered: a hitch with
        // no big scope shows here whether the time went outside every scope. -1 = no scopes for the frame.
        float WallMs = -1.0f, UnscopedMs = -1.0f;
    };
    // One line of the hitch report: frame-time percentiles and hitch counts over a set of frames.
    struct Stats { double P50 = 0, P95 = 0, P99 = 0, P999 = 0, Max = 0; int Over2x = 0, Over33 = 0; size_t N = 0; };

    static void Add(AccumList& v, const std::string& name, double ms);
    static std::string TopScopes(const std::vector<Profiler::Entry>& entries);
    static void FillGpu(Worst& w);
    static Stats StatsOf(std::vector<double> t);
    void Settle(bool haveScopes);
    void RecordFrame(double frameMs, int frame);
    void ReportHitches(const std::string& sceneFile, const char* label);
    void AppendCsv(const std::string& sceneFile, const char* label, const Stats& all, const Stats& after2s) const;
    void PrintScopeTable();
    void AddCpuHalves();
    void ReportHalves(const std::string& sceneFile, const char* label);
    void PrintMachine() const;

    int m_Warmup = 0;
    AccumList m_Cpu, m_Gpu;
    Accum m_Frame;
    std::chrono::steady_clock::time_point m_LastFrameEnd;
    std::vector<double> m_FrameTimes;
    std::vector<Worst> m_Worst;
    Worst m_Pending;
    std::vector<FrameLoads::Event> m_Loads; // the frame's loads, from TakeFrameMs
    // G5-0: the skinning counters (Model::SkinFrameStats) of the frame TakeFrameMs closed, and their phase sums.
    struct SkinSums {
        double Frames = 0, Listed = 0, Dispatched = 0, Idle = 0, Fallback = 0, Unseen = 0, Verts = 0, Offscreen = 0, OffscreenVerts = 0,
               PosedDraws = 0, PosedMs = 0, PosedMsMax = 0;
    };
    SkinSums m_SkinFrame, m_Skin;
    void ReportSkin(const char* mode);
    MachineLoad::Snapshot m_LoadStart;      // at the phase's first measured frame
    std::uint64_t m_QuietLive0 = 0, m_Quiet0 = 0; // ShadowFrame::LocalQuietTotals there (G4-c share per phase)
    CascadedShadowMap::FarTotals m_Far0;          // CascadedShadowMap::FarCascadeTotals there (G4-d, per phase)
    DepthCasterCache::Totals m_SunCache0;         // DepthCasterCache::AllTotals there (sun caster cache, per phase)
    std::uint64_t m_SunDelta0 = 0, m_SunFull0 = 0; // ShadowFrame::SunListTotalsSoFar there (sun caster list, per phase)
    std::uint64_t m_SunReused0 = 0, m_SunGathered0 = 0, m_SunPatched0 = 0; // ShadowFrame::SunGatherTotalsSoFar there (sun gather reuse, per phase)
    std::vector<std::pair<std::string, std::vector<double>>> m_Halves; // AddHalf's samples, per key, in first-seen order
    std::vector<std::string> m_UnmappedCpu;
    PerfG0 m_G0; // G-0: the GPU half, the P-48 render rows and which half bound each frame (into m_Halves)  // top-level CPU scopes CpuBucket put in "other" this phase
};
