#pragma once

#include <functional>
#include <string>
#include <vector>

// --perf-bench's G-0 rows: the GPU half of each frame (Profiler::EndGpuFrame's lagged whole-frame time and every GPU
// scope, as "gpu.<name>"), the render CPU breakdown the P-48 ranking reads ("p48.<scope>", nested scopes summed by
// name, so they overlap the cpu.* buckets on purpose), and which half bound each frame. PerfBench owns the per-key
// sample lists and their percentiles; this only decides what goes into them.
class PerfG0 {
public:
    using AddFn = std::function<void(const std::string& key, double ms)>;

    // At a phase's first measured frame: GPU frames recorded before it are not this phase's.
    void BeginPhase();
    // One measured frame: its CPU breakdown and bind (from Profiler::GetLastFrame), and every GPU frame read back
    // since the last call (lagged a few frames, so a phase's last few frames never land: their keys have a smaller N).
    void Sample(const AddFn& add);
    // The phase's bind line ("BIND cpu-bound=.. gpu-bound=.. of .."), and its rows in TARTARUS_PERF_CSV; then clears.
    void Report(const std::string& sceneFile, const char* mode);

    // A frame the CPU waited at least this long at the swap / pacing fence was waiting on the GPU: the GPU half bound it.
    static constexpr double kGpuBoundWaitMs = 1.0;
    // A scope name as a key: "Scene: Classify Visible" -> "scene_classify_visible".
    static std::string KeyOf(const std::string& scopeName);

private:
    void AddSeen(const AddFn& add, const std::string& key, double ms, std::vector<std::string>& seen, bool present);

    bool m_Started = false;
    unsigned m_LastGpuSerial = 0;
    int m_CpuBound = 0, m_GpuBound = 0;
    std::vector<std::string> m_SeenCpu, m_SeenGpu; // keys met this phase: from then on a frame without one adds 0
};
