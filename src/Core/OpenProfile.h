#pragma once

// Scene-open profile: where a scene open's time goes, by named stage. The frame profiler
// (Profiler.h) only sees time inside frames, and a scene open is one long stretch with no frame.
// OPEN_SCOPE("stage") adds its wall time to that stage's total while a profile is running
// (Begin() .. End()); outside one it costs one relaxed atomic load. Stages may nest: each total
// is the stage's own inclusive time, so a parent row includes its children's rows. Time on
// threads other than the one that called Begin() is kept apart ("other threads"), since it
// overlaps the open's wall time instead of adding to it.
namespace OpenProfile {
    // Clears the totals and starts collecting. The calling thread is the "main" one.
    void Begin();
    void End();
    bool Active();
    // Adds `ms` to `stage` (a string literal: kept as a pointer). No-op while not active.
    void Add(const char* stage, double ms);
    // Prints "[OpenProfile] <label> ..." lines to stdout: every stage, slowest first, with its call
    // count, main-thread and other-thread milliseconds, and the share of `wallMs` it covers.
    void Report(const char* label, double wallMs);

    // Times its own scope into `stage`. A null stage times nothing.
    class Scope {
    public:
        explicit Scope(const char* stage);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        const char* m_Stage;
        long long m_StartTicks = 0;
    };
}

#define OPEN_SCOPE_CONCAT_INNER(a, b) a##b
#define OPEN_SCOPE_CONCAT(a, b) OPEN_SCOPE_CONCAT_INNER(a, b)
#define OPEN_SCOPE(stage) OpenProfile::Scope OPEN_SCOPE_CONCAT(openScope_, __LINE__)(stage)
