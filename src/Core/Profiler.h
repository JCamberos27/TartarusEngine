#pragma once
#include <string>
#include <vector>
#include <chrono>

// Frame profiler: named scoped timers collected once per frame, read by the editor's Stats
// overlay (EditorLayer::DrawStatsPanel). CPU timing (ScopeTimer / PROFILE_SCOPE) answers "where
// does a frame's CPU time go"; GPU timing (GpuScopeTimer / PROFILE_GPU_SCOPE, #197) answers the
// same question for GPU time via GL_TIMESTAMP query pairs, which the CPU timers structurally
// cannot see. GPU results land several frames late due to pipelining — a small ring of per-frame
// query pools means reading them back never stalls waiting on the GPU.
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

    // GPU timings of the most recent frame the GPU has finished - the GPU equivalent of
    // GetLastFrame(), just further behind by however many frames the driver's pipeline is deep
    // (typically 1-3). A scope that ran more than once that frame (e.g. "Scene Draw" for both
    // the Scene and Game views) is reported once, as the sum (#147).
    static const std::vector<Entry>& GetLastFrameGpu();

    // Constructed at the top of a scope, brackets it with a pair of GL_TIMESTAMP queries under
    // `name` (a string literal: it's stored, not copied). Scopes may nest (#147). Use via PROFILE_GPU_SCOPE(name). Must be constructed on the GL
    // thread with a current context, after GLLoader_Init() - same requirement as any other GL
    // call in this codebase.
    class GpuScopeTimer {
    public:
        explicit GpuScopeTimer(const char* name);
        ~GpuScopeTimer();
        GpuScopeTimer(const GpuScopeTimer&) = delete;
        GpuScopeTimer& operator=(const GpuScopeTimer&) = delete;
    private:
        int m_Record = -1; // index of this scope in the current frame's records; -1 = not timed
    };
};

// __LINE__-based naming means at most one PROFILE_SCOPE per source line, which every real call
// site already satisfies - two on the same line would need PROFILE_SCOPE_NAMED below instead.
#define PROFILE_SCOPE_CONCAT_INNER(a, b) a##b
#define PROFILE_SCOPE_CONCAT(a, b) PROFILE_SCOPE_CONCAT_INNER(a, b)
#define PROFILE_SCOPE(name) Profiler::ScopeTimer PROFILE_SCOPE_CONCAT(profileScope_, __LINE__)(name)
#define PROFILE_GPU_SCOPE(name) Profiler::GpuScopeTimer PROFILE_SCOPE_CONCAT(profileGpuScope_, __LINE__)(name)
