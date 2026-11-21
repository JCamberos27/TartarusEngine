#include "Tests/PerfBench.h"

#include "Core/Env.h"
#include "SamplingProfiler.h"
#include "Texture.h"
#include "TimeService.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
constexpr size_t kKeepWorst = 10;

// A CSV field: quoted, inner quotes doubled (the scope / load lists hold commas).
std::string CsvField(const std::string& s) {
    std::string out = "\"";
    for (char c : s) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}
// A frame whose CPU work ran past a 60 Hz frame while the CPU never waited for the GPU ("Swap Buffers" is the
// hidden run's fence wait): the GPU sat idle for the CPU. A GPU scope's end stamp then only runs once the CPU has
// sent the commands after it, so the scope reads the CPU time in between, not GPU work (Pre-Skin over Local Shadow
// Gather, Sky Atmosphere over Gather Visible). Its GPU column is flagged so nobody chases those numbers.
bool GpuStarvedFrame(double frameMs, const std::vector<Profiler::Entry>& cpu) {
    double waitMs = 0.0;
    for (const Profiler::Entry& e : cpu)
        if (e.Name == "Swap Buffers") waitMs = e.Milliseconds;
    return waitMs < 1.0 && frameMs - waitMs > 16.7;
}
} // namespace

PerfBench::PerfBench()
    : m_Warmup(std::max(0, Env::Int(Env::Var::PerfWarmup, 100))), m_LastFrameEnd(std::chrono::steady_clock::now()) {}

double PerfBench::TakeFrameMs() {
    const auto now = std::chrono::steady_clock::now();
    const double frameMs = std::chrono::duration<double, std::milli>(now - m_LastFrameEnd).count();
    m_LastFrameEnd = now;
    SamplingProfiler::EndFrame(frameMs);
    if (!FrameLoads::Enabled()) FrameLoads::Enable(true); // only --perf-bench calls this: the log is off otherwise
    FrameLoads::Take(m_Loads);
    return frameMs;
}

void PerfBench::Sample(double frameMs, int frameNo) {
    if (m_Frame.N == 0) m_LoadStart = MachineLoad::Take();
    m_Frame.Sum += frameMs; m_Frame.Max = std::max(m_Frame.Max, frameMs); ++m_Frame.N;
    for (const Profiler::Entry& e : Profiler::GetLastFrame()) Add(m_Cpu, e.Name, e.Milliseconds);
    for (const Profiler::Entry& e : Profiler::GetLastFrameGpu()) Add(m_Gpu, e.Name, e.Milliseconds);
    RecordFrame(frameMs, frameNo);
    for (Worst& w : m_Worst) FillGpu(w);
}

void PerfBench::Add(AccumList& v, const std::string& name, double ms) {
    auto it = std::find_if(v.begin(), v.end(), [&](const auto& p) { return p.first == name; });
    if (it == v.end()) { v.emplace_back(name, Accum{}); it = v.end() - 1; }
    it->second.Sum += ms; it->second.Max = std::max(it->second.Max, ms); ++it->second.N;
}

// The ten biggest scopes of a frame (0.25 ms and up), biggest first.
std::string PerfBench::TopScopes(const std::vector<Profiler::Entry>& entries) {
    std::vector<std::pair<float, std::string>> scopes;
    for (const Profiler::Entry& e : entries)
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

// GPU timings land a few frames late: a worst frame picks its own up once the profiler has read them back.
void PerfBench::FillGpu(Worst& w) {
    if (w.HaveGpu) return;
    if (const std::vector<Profiler::Entry>* gpu = Profiler::GpuFrameScopes(w.Serial)) {
        w.Gpu = TopScopes(*gpu);
        w.HaveGpu = true;
    }
}

// The profiler hands back the frame before (Profiler::GetLastFrame), so a frame's CPU scopes are attached on the next call.
void PerfBench::Settle(bool haveScopes) {
    if (m_Pending.Ms < 0.0) return;
    if (m_Worst.size() < kKeepWorst || m_Pending.Ms > m_Worst.back().Ms) {
        if (haveScopes) {
            m_Pending.Cpu = TopScopes(Profiler::GetLastFrame());
            m_Pending.GpuStarved = GpuStarvedFrame(m_Pending.Ms, Profiler::GetLastFrame());
            m_Pending.WallMs = Profiler::LastFrameWallMs();
            m_Pending.UnscopedMs = Profiler::LastFrameUnscopedMs();
        }
        m_Worst.push_back(std::move(m_Pending));
        std::sort(m_Worst.begin(), m_Worst.end(), [](const Worst& a, const Worst& b) { return a.Ms > b.Ms; });
        if (m_Worst.size() > kKeepWorst) m_Worst.pop_back();
    }
    m_Pending = Worst{};
}

void PerfBench::RecordFrame(double frameMs, int frame) {
    Settle(true);
    m_FrameTimes.push_back(frameMs);
    m_Pending = Worst{};
    m_Pending.Ms = frameMs;
    m_Pending.Frame = frame;
    m_Pending.Serial = Profiler::FrameSerial();
    if (!m_Loads.empty()) m_Pending.Loads = FrameLoads::Summarize(m_Loads);
}

PerfBench::Stats PerfBench::StatsOf(std::vector<double> t) {
    Stats s;
    if (t.empty()) return s;
    std::sort(t.begin(), t.end());
    auto pct = [&](double p) { return t[std::min(t.size() - 1, (size_t)(p * (double)(t.size() - 1) + 0.5))]; };
    s.P50 = pct(0.5); s.P95 = pct(0.95); s.P99 = pct(0.99); s.P999 = pct(0.999); s.Max = t.back(); s.N = t.size();
    for (double ms : t) { s.Over2x += ms > 2.0 * s.P50; s.Over33 += ms > 33.3; }
    return s;
}

void PerfBench::ReportHitches(const std::string& sceneFile, const char* label) {
    Settle(false);
    if (m_FrameTimes.empty()) return;
    for (Worst& w : m_Worst) FillGpu(w);
    auto print = [&](const char* what, const Stats& s) {
        if (s.N == 0) return;
        std::printf("[PerfBench] %s%s frame ms: p50=%.2f p95=%.2f p99=%.2f p99.9=%.2f max=%.2f  hitches(>2x p50)=%d (>33ms)=%d of %zu\n",
                    label, what, s.P50, s.P95, s.P99, s.P999, s.Max, s.Over2x, s.Over33, s.N);
    };
    const Stats all = StatsOf(m_FrameTimes);
    print("", all);
    // Without the first 2 s (Play's start: the squad spawning, the first clips and shaders).
    Stats after2s;
    if (m_FrameTimes.size() > 240) after2s = StatsOf(std::vector<double>(m_FrameTimes.begin() + 120, m_FrameTimes.end()));
    print(" after 2 s", after2s);
    // What else ran during the phase: hitches in a CONTENDED phase are likely other processes, not this one.
    const std::string machine =
        MachineLoad::Describe(MachineLoad::Between(m_LoadStart, MachineLoad::Take()), MachineLoad::Cores());
    if (!machine.empty()) std::printf("[PerfBench] %s machine: %s\n", label, machine.c_str());
    for (const Worst& w : m_Worst) {
        char unscoped[64] = "-";
        if (w.UnscopedMs >= 0.0f) std::snprintf(unscoped, sizeof(unscoped), "%.2f of %.2f", w.UnscopedMs, w.WallMs);
        // "unscoped" goes last so scripts that split the line on "| GPU" / "| loaded" read it as before.
        std::printf("[PerfBench]   worst frame %d %.2f ms: CPU %s | GPU%s %s | loaded %s | unscoped %s\n", w.Frame, w.Ms,
                    w.Cpu.empty() ? "-" : w.Cpu.c_str(),
                    w.GpuStarved ? " (STARVED: CPU-bound frame, its GPU scopes include CPU time)" : "",
                    !w.HaveGpu ? "(not read back)" : w.Gpu.empty() ? "-" : w.Gpu.c_str(), w.Loads.empty() ? "-" : w.Loads.c_str(),
                    unscoped);
    }
    AppendCsv(sceneFile, label, all, after2s);
    m_FrameTimes.clear();
    m_Worst.clear();
}

// TARTARUS_PERF_CSV: one row per summary ("all", "after2s") and per worst frame, appended (header on a new file), so
// several runs and scenes land in one sheet.
void PerfBench::AppendCsv(const std::string& sceneFile, const char* label, const Stats& all, const Stats& after2s) const {
    const std::string path = Env::Str(Env::Var::PerfCsv);
    if (path.empty()) return;
    std::error_code ec;
    const bool fresh = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0;
    std::ofstream out(path, std::ios::app);
    if (!out) { std::printf("[PerfBench] cannot write the CSV '%s'\n", path.c_str()); return; }
    if (fresh) out << "scene,phase,row,rank,frame,ms,p50,p95,p99,p999,max,over2x,over33,frames,cpu,gpu,loads,gpu_starved\n";
    char buf[192];
    auto summary = [&](const char* row, const Stats& s) {
        if (s.N == 0) return;
        std::snprintf(buf, sizeof(buf), ",,,,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%zu,,,,\n", s.P50, s.P95, s.P99, s.P999, s.Max,
                      s.Over2x, s.Over33, s.N);
        out << CsvField(sceneFile) << ',' << label << ',' << row << buf;
    };
    summary("all", all);
    summary("after2s", after2s);
    int rank = 0;
    for (const Worst& w : m_Worst) {
        std::snprintf(buf, sizeof(buf), ",worst,%d,%d,%.3f,,,,,,,,,", ++rank, w.Frame, w.Ms);
        out << CsvField(sceneFile) << ',' << label << buf << CsvField(w.Cpu) << ',' << CsvField(w.Gpu) << ','
            << CsvField(w.Loads) << ',' << (w.GpuStarved ? 1 : 0) << '\n';
    }
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
    ReportHitches(sceneFile, "combat");
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
    ReportHitches(sceneFile, mode);
    std::fflush(stdout);
    if (perfSample) SamplingProfiler::Report(mode, 60);
    m_Cpu.clear(); m_Gpu.clear(); m_Frame = {};
}
