#include "Tests/PerfBench.h"

#include "Core/Env.h"
#include "AssetCost.h"
#include "ShadowFrame.h"
#include "SamplingProfiler.h"
#include "Texture.h"
#include "TimeService.h"
#include "gl.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
constexpr size_t kKeepWorst = 10;
// TARTARUS_PERF_CSV's columns. "half" rows (ReportHalves) put a key in rank, its avg / p50 / p95 / p99 / max in
// ms / p50 / p95 / p99 / max and its sample count in frames.
constexpr const char* kCsvHeader =
    "scene,phase,row,rank,frame,ms,p50,p95,p99,p999,max,over2x,over33,frames,cpu,gpu,loads,gpu_starved\n";

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

// CpuBucket's map: a scope name (or, ending in '*', a name prefix) to its cpu.* bucket. Only top-level scopes are
// bucketed, so a nested name here is harmless: it just never matches a bucketed entry.
struct BucketRule { const char* Scope; const char* Bucket; };
constexpr BucketRule kCpuBuckets[] = {
    {"Physics Step", "physics"},
    {"Animators", "anim"}, {"Animation Update", "anim"}, {"First Person IK", "anim"},
    {"Player + FP Update", "sim"}, {"Dummies", "sim"}, {"Dummies Late", "sim"}, {"Player Shots + Impacts", "sim"},
    {"Combat FX + Foley", "sim"}, {"Blood", "sim"}, {"Game Module Tick", "sim"}, {"Particles", "sim"},
    {"Outfits + Transform Cache", "sim"}, {"AI *", "sim"},
    {"Audio Update", "audio"}, {"Audio Listener", "audio"},
    {"Editor UI Build", "ui"}, {"Game View UI", "ui"}, {"ImGui Render", "ui"}, {"UI *", "ui"},
    {"Asset Pump + Outfit Hiding", "assets"}, {"Asset Pump", "assets"},
    {"Game View Render", "render"}, {"Scene View Render", "render"}, {"Pre-Skin", "render"}, {"LOD Select", "render"},
    {"Probe *", "render"}, {"GI Bake", "render"}, {"HDRI Load", "render"}, {"IBL Bake*", "render"},
    {"Upload Fences", "render"}, {"Sky *", "render"}, {"Atmosphere *", "render"}, {"Cloud *", "render"},
    {"Volumetric *", "render"}, {"Decal *", "render"}, {"Grass *", "render"}, {"Collider Overlay", "render"},
    {"Selection Outline", "render"}, {"Scene*", "render"}, {"Sun*", "render"}, {"Spot Shadow*", "render"},
    {"Point Shadow*", "render"}, {"Local Shadow*", "render"}, {"View Model Draw", "render"}, {"Gather Visible", "render"},
    {"Swap Buffers", "wait"}, {"Pace Wait", "wait"}, {"Pace Flush", "wait"}, {"Frame Cap", "wait"},
    {"Project Sync", "editor"}, {"Focus Reimport", "editor"}, {"Shader Reload Poll", "editor"},
    {"Harness Begin", "editor"}, {"Harness End", "editor"}, {"Undo *", "editor"}, {"PushUndo", "editor"},
    {"Events + Input", "input"},
};
// Every cpu.* bucket, in report order; each gets a sample every frame (0 when nothing ran) so they share one N.
constexpr const char* kCpuBucketKeys[] = {"sim", "anim", "physics", "ui", "audio", "assets", "render", "wait",
                                          "editor", "input", "other"};

bool ScopeMatches(const char* rule, const std::string& scope) {
    const size_t n = std::strlen(rule);
    if (n > 0 && rule[n - 1] == '*') return scope.compare(0, n - 1, rule, n - 1) == 0;
    return scope == rule;
}

// The value at fraction p of sorted samples (nearest rank, as StatsOf).
double Pct(const std::vector<double>& sorted, double p) {
    return sorted[std::min(sorted.size() - 1, (size_t)(p * (double)(sorted.size() - 1) + 0.5))];
}
} // namespace

const char* PerfBench::CpuBucket(const std::string& scope) {
    for (const BucketRule& r : kCpuBuckets)
        if (ScopeMatches(r.Scope, scope)) return r.Bucket;
    return "other";
}

void PerfBench::AddHalf(const std::string& key, double ms) {
    auto it = std::find_if(m_Halves.begin(), m_Halves.end(), [&](const auto& h) { return h.first == key; });
    if (it == m_Halves.end()) { m_Halves.emplace_back(key, std::vector<double>{}); it = m_Halves.end() - 1; }
    it->second.push_back(ms);
}

double PerfBench::LastHalf(const std::string& key) const {
    for (const auto& [k, samples] : m_Halves)
        if (k == key) return samples.empty() ? -1.0 : samples.back();
    return -1.0;
}

// The profiler's last frame, top-level scopes summed per bucket, plus the frame's wall time and its unscoped part.
void PerfBench::AddCpuHalves() {
    const float wallMs = Profiler::LastFrameWallMs();
    if (wallMs <= 0.0f) return; // no completed profiler frame yet
    double sums[std::size(kCpuBucketKeys)] = {};
    for (const Profiler::Entry& e : Profiler::GetLastFrame()) {
        if (e.Nested) continue; // its time is already in the scope around it
        const char* bucket = CpuBucket(e.Name);
        for (size_t i = 0; i < std::size(kCpuBucketKeys); ++i)
            if (std::strcmp(bucket, kCpuBucketKeys[i]) == 0) sums[i] += e.Milliseconds;
        if (std::strcmp(bucket, "other") == 0 &&
            std::find(m_UnmappedCpu.begin(), m_UnmappedCpu.end(), e.Name) == m_UnmappedCpu.end())
            m_UnmappedCpu.push_back(e.Name);
    }
    std::string key;
    for (size_t i = 0; i < std::size(kCpuBucketKeys); ++i) {
        key = "cpu.";
        key += kCpuBucketKeys[i];
        AddHalf(key, sums[i]);
    }
    AddHalf("cpu.unscoped", Profiler::LastFrameUnscopedMs());
    AddHalf("cpu.frame", wallMs);
}

// TARTARUS_PERF_MACHINE labels every phase with the machine it ran on (G-0: numbers name their machine). Unset, the
// line says so and gives what the process can see (the GL renderer, the logical cores).
void PerfBench::PrintMachine() const {
    const std::string label = Env::Str(Env::Var::PerfMachine);
    if (!label.empty()) { std::printf("[PerfBench]   MACHINE %s\n", label.c_str()); return; }
    const char* gpu = reinterpret_cast<const char*>(glGetString(0x1F01 /* GL_RENDERER */));
    std::printf("[PerfBench]   MACHINE (unlabelled: set TARTARUS_PERF_MACHINE) GPU %s, %d logical cores\n",
                gpu ? gpu : "?", MachineLoad::Cores());
}

// The HALF lines (and the CSV's "half" rows): per key avg / p50 / p95 / p99 / max over its own samples, then clears them.
void PerfBench::ReportHalves(const std::string& sceneFile, const char* label) {
    PrintMachine();
    std::ofstream csv;
    const std::string path = Env::Str(Env::Var::PerfCsv);
    if (!path.empty()) {
        std::error_code ec;
        const bool fresh = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0;
        csv.open(path, std::ios::app);
        if (csv && fresh) csv << kCsvHeader;
    }
    for (auto& [key, samples] : m_Halves) {
        if (samples.empty()) continue;
        double sum = 0.0;
        for (double v : samples) sum += v;
        std::sort(samples.begin(), samples.end());
        const double avg = sum / (double)samples.size(), p50 = Pct(samples, 0.5), p95 = Pct(samples, 0.95),
                     p99 = Pct(samples, 0.99), mx = samples.back();
        std::printf("[PerfBench]   HALF %-24s avg=%8.3f p50=%8.3f p95=%8.3f p99=%8.3f max=%8.3f ms  N=%zu\n", key.c_str(),
                    avg, p50, p95, p99, mx, samples.size());
        if (csv) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), ",,%.3f,%.3f,%.3f,%.3f,,%.3f,,,%zu,,,,\n", avg, p50, p95, p99, mx,
                          samples.size());
            csv << CsvField(sceneFile) << ',' << label << ",half," << CsvField(key) << buf;
        }
    }
    if (!m_UnmappedCpu.empty()) {
        std::string names;
        for (const std::string& n : m_UnmappedCpu) names += (names.empty() ? "" : ", ") + n;
        std::printf("[PerfBench]   HALF cpu.other holds: %s\n", names.c_str());
    }
    m_Halves.clear();
    m_UnmappedCpu.clear();
}

PerfBench::PerfBench()
    : m_Warmup(std::max(0, Env::Int(Env::Var::PerfWarmup, 100))), m_LastFrameEnd(std::chrono::steady_clock::now()) {
    // The asset cost table wants every load, the first scene's own included (TakeFrameMs only turns the log on at
    // the first frame, after that load).
    if (AssetCost::Enabled()) FrameLoads::Enable(true);
}

double PerfBench::TakeFrameMs() {
    const auto now = std::chrono::steady_clock::now();
    const double frameMs = std::chrono::duration<double, std::milli>(now - m_LastFrameEnd).count();
    m_LastFrameEnd = now;
    SamplingProfiler::EndFrame(frameMs);
    if (!FrameLoads::Enabled()) FrameLoads::Enable(true); // only --perf-bench calls this: the log is off otherwise
    FrameLoads::Take(m_Loads);
    if (AssetCost::Enabled()) AssetCost::NoteLoads(m_Loads);
    return frameMs;
}

void PerfBench::Sample(double frameMs, int frameNo) {
    if (m_Frame.N == 0) {
        m_LoadStart = MachineLoad::Take(); m_G0.BeginPhase();
        m_QuietLive0 = ShadowFrame::LocalQuietTotals().Live; m_Quiet0 = ShadowFrame::LocalQuietTotals().Quiet;
    }
    m_Frame.Sum += frameMs; m_Frame.Max = std::max(m_Frame.Max, frameMs); ++m_Frame.N;
    for (const Profiler::Entry& e : Profiler::GetLastFrame()) Add(m_Cpu, e.Name, e.Milliseconds);
    for (const Profiler::Entry& e : Profiler::GetLastFrameGpu()) Add(m_Gpu, e.Name, e.Milliseconds);
    AddCpuHalves();
    m_G0.Sample([this](const std::string& key, double ms) { AddHalf(key, ms); }); // gpu.*, half.cpu, p48.*, bind
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
    const int cores = MachineLoad::Cores();
    const std::string machine = MachineLoad::Describe(MachineLoad::Between(m_LoadStart, MachineLoad::Take(), cores), cores);
    if (!machine.empty()) std::printf("[PerfBench] %s machine: %s\n", label, machine.c_str());
    // G4-c: of the live local lights each frame, how many kept their map without a look (0 with verify on or G4-c off).
    {
        const ShadowFrame::QuietTotals& q = ShadowFrame::LocalQuietTotals();
        const std::uint64_t live = q.Live - m_QuietLive0, quiet = q.Quiet - m_Quiet0;
        std::printf("[PerfBench] %s local shadows: %llu of %llu live light looks kept quiet%s\n", label,
                    (unsigned long long)quiet, (unsigned long long)live,
                    ShadowFrame::CacheVerify() ? " (verify on: quiet lights looked at anyway)"
                    : !ShadowFrame::SlotDirtyEnabled() ? " (G4-c off)" : "");
    }
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
    if (fresh) out << kCsvHeader;
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
    ReportHalves(sceneFile, "combat");
    m_G0.Report(sceneFile, "combat");
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
    ReportHalves(sceneFile, mode);
    m_G0.Report(sceneFile, mode);
    ReportHitches(sceneFile, mode);
    std::fflush(stdout);
    if (perfSample) SamplingProfiler::Report(mode, 60);
    if (AssetCost::Enabled()) AssetCost::WritePhase(sceneFile, mode);
    m_Cpu.clear(); m_Gpu.clear(); m_Frame = {};
}
