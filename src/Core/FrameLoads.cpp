#include "FrameLoads.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {
std::atomic<bool> g_On{false};
std::mutex g_Mutex;
std::vector<FrameLoads::Event> g_Events;
} // namespace

namespace FrameLoads {

void Enable(bool on) { g_On.store(on, std::memory_order_relaxed); }

bool Enabled() { return g_On.load(std::memory_order_relaxed); }

void Note(const char* kind, std::string_view name, double ms) {
    if (!Enabled()) return;
    std::lock_guard<std::mutex> lk(g_Mutex);
    g_Events.push_back({kind, std::string(name), ms});
}

void Take(std::vector<Event>& out) {
    out.clear();
    if (!Enabled()) return;
    std::lock_guard<std::mutex> lk(g_Mutex);
    std::swap(out, g_Events);
}

std::string Summarize(const std::vector<Event>& events) {
    std::string s;
    std::vector<const char*> kinds;
    for (const Event& e : events) {
        bool seen = false;
        for (const char* k : kinds) seen = seen || std::strcmp(k, e.Kind) == 0;
        if (!seen) kinds.push_back(e.Kind);
    }
    char buf[128];
    for (const char* k : kinds) {
        int n = 0;
        double ms = 0.0;
        std::string names;
        for (const Event& e : events) {
            if (std::strcmp(k, e.Kind) != 0) continue;
            ++n;
            ms += e.Ms;
            if (n <= 3) names += (names.empty() ? "" : " ") + e.Name.substr(e.Name.find_last_of("/\\") + 1); // npos + 1 = 0: the whole name
        }
        if (n > 3) names += " ...";
        std::snprintf(buf, sizeof(buf), "%s%s x%d %.2f ms (", s.empty() ? "" : "; ", k, n, ms);
        s += buf;
        s += names;
        s += ")";
    }
    return s;
}

} // namespace FrameLoads
