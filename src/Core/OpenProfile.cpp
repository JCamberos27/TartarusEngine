#include "Core/OpenProfile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
struct Totals {
    long long Calls = 0;
    double MainMs = 0.0;
    double OtherMs = 0.0;
};

std::atomic<bool> g_Active{false};
std::mutex g_Mutex;
std::thread::id g_MainThread;
std::unordered_map<const char*, Totals> g_Totals; // keyed by the literal's address: one row per call site's name

long long NowTicks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
double TicksToMs(long long ticks) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::duration(ticks)).count();
}
} // namespace

void OpenProfile::Begin() {
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Totals.clear();
    g_MainThread = std::this_thread::get_id();
    g_Active.store(true, std::memory_order_release);
}

void OpenProfile::End() { g_Active.store(false, std::memory_order_release); }

bool OpenProfile::Active() { return g_Active.load(std::memory_order_relaxed); }

void OpenProfile::Add(const char* stage, double ms) {
    if (!stage || !Active()) return;
    const bool main = std::this_thread::get_id() == g_MainThread;
    std::lock_guard<std::mutex> lock(g_Mutex);
    Totals& t = g_Totals[stage];
    ++t.Calls;
    (main ? t.MainMs : t.OtherMs) += ms;
}

void OpenProfile::Report(const char* label, double wallMs) {
    struct Row { const char* Stage; Totals T; };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        rows.reserve(g_Totals.size());
        for (const auto& [stage, t] : g_Totals) rows.push_back({stage, t});
    }
    // The same name from two call sites is two keys (two literals): merge them by text.
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return std::strcmp(a.Stage, b.Stage) < 0; });
    std::vector<Row> merged;
    for (const Row& r : rows) {
        if (!merged.empty() && std::strcmp(merged.back().Stage, r.Stage) == 0) {
            merged.back().T.Calls += r.T.Calls;
            merged.back().T.MainMs += r.T.MainMs;
            merged.back().T.OtherMs += r.T.OtherMs;
        } else {
            merged.push_back(r);
        }
    }
    std::sort(merged.begin(), merged.end(), [](const Row& a, const Row& b) {
        return a.T.MainMs + a.T.OtherMs > b.T.MainMs + b.T.OtherMs;
    });
    std::printf("[OpenProfile] %s: open %.0f ms wall; %zu stage(s), slowest first (stages nest: a parent includes its children)\n",
                label, wallMs, merged.size());
    std::printf("[OpenProfile] %8s %9s %9s %6s  %s\n", "calls", "main ms", "other ms", "%wall", "stage");
    for (const Row& r : merged)
        std::printf("[OpenProfile] %8lld %9.1f %9.1f %5.1f%%  %s\n", r.T.Calls, r.T.MainMs, r.T.OtherMs,
                    wallMs > 0.0 ? 100.0 * r.T.MainMs / wallMs : 0.0, r.Stage);
    std::fflush(stdout);
}

OpenProfile::Scope::Scope(const char* stage) : m_Stage(stage && Active() ? stage : nullptr) {
    if (m_Stage) m_StartTicks = NowTicks();
}

OpenProfile::Scope::~Scope() {
    if (m_Stage) Add(m_Stage, TicksToMs(NowTicks() - m_StartTicks));
}
