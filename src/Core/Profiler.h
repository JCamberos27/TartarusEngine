#pragma once
#include <string>
#include <vector>
#include <chrono>

// Frame profiler: named scoped timers collected once per frame, read by the editor's Stats
// overlay (EditorLayer::DrawStatsPanel). CPU timing (ScopeTimer / PROFILE_SCOPE) answers "where
// does a frame's CPU time go"; GPU timing (GpuScopeTimer / PROFILE_GPU_SCOPE, #197) answers the
// same question for GPU time via GL_TIME_ELAPSED queries, which the CPU timers structurally
// cannot see. GPU results land several frames late due to pipelining — each named scope keeps
// its own small ring of query objects so reading them back never stalls waiting on the GPU.
class Profiler {
public:
    struct Entry {
        std::string Name;
        float Milliseconds;
    };

    // Call once at the top of the frame, before any PROFILE_SCOPE for that frame starts -
    // rotates last frame's collected entries out for GetLastFrame() and starts a fresh list.
    static void BeginFrame();

    static void PushSample(const std::string& name, float milliseconds);

    // Entries from the most recently COMPLETED frame - i.e. if you're reading this while
    // building this frame's UI (as the Stats overlay does), you're seeing last frame's numbers,
    // the same one-frame-behind convention the existing smoothed FPS counter already uses.
    static const std::vector<Entry>& GetLastFrame();

    // Constructed at the top of a scope, records elapsed wall-clock time under `name` when it
    // goes out of scope. Use via the PROFILE_SCOPE(name) macro below rather than directly, so
    // the variable name (which must be unique per scope) isn't something every call site has to
    // pick by hand.
    class ScopeTimer {
    public:
        explicit ScopeTimer(std::string name);
        ~ScopeTimer();
        ScopeTimer(const ScopeTimer&) = delete;
        ScopeTimer& operator=(const ScopeTimer&) = delete;
    private:
        std::string m_Name;
        std::chrono::steady_clock::time_point m_Start;
    };

    // GPU timings, from whichever named ring's query most recently completed - the GPU
    // equivalent of GetLastFrame(), just further behind by however many frames the driver's
    // pipeline is deep (typically 1-3), not exactly one.
    static const std::vector<Entry>& GetLastFrameGpu();

    // Constructed at the top of a scope, issues a GL_TIME_ELAPSED query under `name` that ends
    // when the scope exits. Use via PROFILE_GPU_SCOPE(name). Must be constructed on the GL
    // thread with a current context, after GLLoader_Init() - same requirement as any other GL
    // call in this codebase.
    class GpuScopeTimer {
    public:
        explicit GpuScopeTimer(std::string name);
        ~GpuScopeTimer();
        GpuScopeTimer(const GpuScopeTimer&) = delete;
        GpuScopeTimer& operator=(const GpuScopeTimer&) = delete;
    private:
        std::string m_Name;
        int m_Slot = -1; // which ring slot this instance's query landed in; -1 = skipped (ring busy)
    };
};

// __LINE__-based naming means at most one PROFILE_SCOPE per source line, which every real call
// site already satisfies - two on the same line would need PROFILE_SCOPE_NAMED below instead.
#define PROFILE_SCOPE_CONCAT_INNER(a, b) a##b
#define PROFILE_SCOPE_CONCAT(a, b) PROFILE_SCOPE_CONCAT_INNER(a, b)
#define PROFILE_SCOPE(name) Profiler::ScopeTimer PROFILE_SCOPE_CONCAT(profileScope_, __LINE__)(name)
#define PROFILE_GPU_SCOPE(name) Profiler::GpuScopeTimer PROFILE_SCOPE_CONCAT(profileGpuScope_, __LINE__)(name)
