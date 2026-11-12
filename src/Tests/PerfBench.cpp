#include "Tests/PerfBench.h"

#include "Core/Env.h"
#include "Profiler.h"
#include "SamplingProfiler.h"
#include "Texture.h"
#include "TimeService.h"

#include <algorithm>
#include <cstdio>

PerfBench::PerfBench()
    : m_Warmup(std::max(0, Env::Int(Env::Var::PerfWarmup, 100))), m_LastFrameEnd(std::chrono::steady_clock::now()) {}

double PerfBench::TakeFrameMs() {
    const auto now = std::chrono::steady_clock::now();
    const double frameMs = std::chrono::duration<double, std::milli>(now - m_LastFrameEnd).count();
    m_LastFrameEnd = now;
    SamplingProfiler::EndFrame(frameMs);
    return frameMs;
}

void PerfBench::Sample(double frameMs, int frameNo) {
    m_Frame.Sum += frameMs; m_Frame.Max = std::max(m_Frame.Max, frameMs); ++m_Frame.N;
    for (const Profiler::Entry& e : Profiler::GetLastFrame()) Add(m_Cpu, e.Name, e.Milliseconds);
    for (const Profiler::Entry& e : Profiler::GetLastFrameGpu()) Add(m_Gpu, e.Name, e.Milliseconds);
    RecordFrame(frameMs, frameNo);
}

void PerfBench::Add(AccumList& v, const std::string& name, double ms) {
    auto it = std::find_if(v.begin(), v.end(), [&](const auto& p) { return p.first == name; });
    if (it == v.end()) { v.emplace_back(name, Accum{}); it = v.end() - 1; }
    it->second.Sum += ms; it->second.Max = std::max(it->second.Max, ms); ++it->second.N;
}

// The profiler hands back the frame before (Profiler::GetLastFrame), so a frame's scopes are attached on the next call.
std::string PerfBench::ScopesNow() {
    std::vector<std::pair<float, std::string>> scopes;
    for (const Profiler::Entry& e : Profiler::GetLastFrame())
        if (e.Name != "Swap Buffers") scopes.emplace_back(e.Milliseconds, e.Name);
    std::sort(scopes.begin(), scopes.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string s;
    char buf[96];
    for (size_t i = 0; i < scopes.size() && i < 10 && scopes[i].first >= 0.25f; ++i) {
        std::snprintf(buf, sizeof(buf), "%s%s %.2f", s.empty() ? "" : ", ", scopes[i].second.c_str(), scopes[i].first);
        s += buf;
    }
    return s;
}

void PerfBench::Settle(bool haveScopes) {
    constexpr size_t kKeep = 12;
    if (m_Pending.Ms < 0.0) return;
    if (m_Worst.size() < kKeep || m_Pending.Ms > m_Worst.back().Ms) {
        if (haveScopes) m_Pending.Scopes = ScopesNow();
        m_Worst.push_back(std::move(m_Pending));
        std::sort(m_Worst.begin(), m_Worst.end(), [](const Worst& a, const Worst& b) { return a.Ms > b.Ms; });
        if (m_Worst.size() > kKeep) m_Worst.pop_back();
    }
    m_Pending = {-1.0, 0, {}};
}

void PerfBench::RecordFrame(double frameMs, int frame) {
    Settle(true);
    m_FrameTimes.push_back(frameMs);
    m_Pending = {frameMs, frame, {}};
}

void PerfBench::ReportHitches(const char* label) {
    Settle(false);
    if (m_FrameTimes.empty()) return;
    auto report = [&](const char* what, std::vector<double> t) {
        if (t.empty()) return;
        std::sort(t.begin(), t.end());
        auto pct = [&](double p) { return t[std::min(t.size() - 1, (size_t)(p * (double)(t.size() - 1) + 0.5))]; };
        const double median = pct(0.5);
        int over2x = 0, over33 = 0;
        for (double ms : t) { over2x += ms > 2.0 * median; over33 += ms > 33.3; }
        std::printf("[PerfBench] %s%s frame ms: p50=%.2f p95=%.2f p99=%.2f p99.9=%.2f max=%.2f  hitches(>2x p50)=%d (>33ms)=%d of %zu\n",
                    label, what, median, pct(0.95), pct(0.99), pct(0.999), t.back(), over2x, over33, t.size());
    };
    report("", m_FrameTimes);
    // Without the first 2 s (Play's start: the squad spawning, the first clips and shaders).
    if (m_FrameTimes.size() > 240) report(" after 2 s", std::vector<double>(m_FrameTimes.begin() + 120, m_FrameTimes.end()));
    for (const Worst& w : m_Worst)
        std::printf("[PerfBench]   worst frame %d %.2f ms: %s\n", w.Frame, w.Ms, w.Scopes.c_str());
    m_FrameTimes.clear();
    m_Worst.clear();
}

void PerfBench::PrintScopeTable() {
    for (auto* v : {&m_Cpu, &m_Gpu}) {
        std::sort(v->begin(), v->end(), [](const auto& a, const auto& b) { return a.second.Sum > b.second.Sum; });
        for (const auto& [name, acc] : *v)
            std::printf("[PerfBench]   %s %-34s avg=%8.3f ms  max=%8.3f ms\n", v == &m_Cpu ? "CPU" : "GPU",
                        name.c_str(), acc.Sum / std::max(m_Frame.N, 1), acc.Max);
    }
}

void PerfBench::ReportCombat(const std::string& sceneFile, int draws, int tris, int width, int height) {
    std::printf("[PerfBench] %s (combat)  frames=%d  avg=%.3f ms (%.1f fps)  worst=%.3f ms  draws=%d tris=%d  window=%dx%d\n",
                sceneFile.c_str(), m_Frame.N,
                m_Frame.Sum / m_Frame.N, m_Frame.Sum > 0.0 ? 1000.0 * m_Frame.N / m_Frame.Sum : 0.0,
                m_Frame.Max, draws, tris, width, height);
    ReportHitches("combat");
    PrintScopeTable();
    std::fflush(stdout);
    m_Cpu.clear(); m_Gpu.clear(); m_Frame = {};
}

void PerfBench::ReportPhase(const std::string& sceneFile, const char* mode, int draws, int tris, int width, int height,
                            bool perfSample) {
    std::printf("[PerfBench] %s (%s)  frames=%d  avg=%.3f ms (%.1f fps)  worst=%.3f ms  draws=%d tris=%d  window=%dx%d\n",
                sceneFile.c_str(), mode, m_Frame.N, m_Frame.N ? m_Frame.Sum / m_Frame.N : 0.0,
                m_Frame.Sum > 0.0 ? 1000.0 * m_Frame.N / m_Frame.Sum : 0.0,
                m_Frame.Max, draws, tris, width, height);
    // The engine frame this phase ended on: TARTARUS_TRIS_REPORT takes one (a phase's last frame minus a hundred).
    std::printf("[PerfBench]   ended at engine frame %llu\n", (unsigned long long)Time::FrameCount());
    // Every texture this process has uploaded so far (the editor's own icons included), mips included.
    std::printf("[PerfBench]   textures=%d  %.1f MiB on the GPU\n", TextureLoadStats::Get().Count,
                TextureLoadStats::Get().GpuBytes / (1024.0 * 1024.0));
    PrintScopeTable();
    ReportHitches(mode);
    std::fflush(stdout);
    if (perfSample) SamplingProfiler::Report(mode, 60);
    m_Cpu.clear(); m_Gpu.clear(); m_Frame = {};
}
