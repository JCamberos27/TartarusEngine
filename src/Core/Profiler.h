#pragma once
#include <string>
#include <vector>
#include <chrono>

// Lightweight CPU frame profiler: named scoped timers collected once per frame, read by the
// editor's Stats overlay (EditorLayer::DrawStatsOverlay). CPU-only, deliberately — a GPU timer
// (glQueryCounter/GL_TIME_ELAPSED) reports results several frames late due to pipelining, which
// is real extra complexity (a ring buffer of in-flight queries, not just a start/stop pair), and
// nothing about this engine's current scene scale (see the architecture audit) points to the GPU
// being the actual bottleneck yet — CPU timing answers "where does a frame's time actually go"
// for the problems this codebase has today. Worth adding if/when that changes.
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
};

// __LINE__-based naming means at most one PROFILE_SCOPE per source line, which every real call
// site already satisfies - two on the same line would need PROFILE_SCOPE_NAMED below instead.
#define PROFILE_SCOPE_CONCAT_INNER(a, b) a##b
#define PROFILE_SCOPE_CONCAT(a, b) PROFILE_SCOPE_CONCAT_INNER(a, b)
#define PROFILE_SCOPE(name) Profiler::ScopeTimer PROFILE_SCOPE_CONCAT(profileScope_, __LINE__)(name)
