#include "Log.h"
#include <iostream>

namespace {

// Ring-buffer cap: old entries drop off the front once exceeded. High enough that a normal
// session never loses anything, low enough that a runaway per-frame log can't grow unbounded.
constexpr size_t kMaxEntries = 1000;

std::vector<LogEntry>& Storage() {
    static std::vector<LogEntry> entries;
    return entries;
}

unsigned int g_Revision = 0;

void Push(LogLevel level, const std::string& message) {
    auto& entries = Storage();

    if (!entries.empty() && entries.back().Level == level && entries.back().Message == message) {
        entries.back().Count++;
    } else {
        entries.push_back({level, message, 1});
        if (entries.size() > kMaxEntries) {
            entries.erase(entries.begin(), entries.begin() + (entries.size() - kMaxEntries));
        }
    }
    g_Revision++;

    // Mirrored so a terminal-attached run (or a crash before the editor draws) still shows it.
    std::ostream& out = (level == LogLevel::Error) ? std::cerr : std::cout;
    const char* prefix = level == LogLevel::Error ? "[error] " : (level == LogLevel::Warning ? "[warn]  " : "[info]  ");
    out << prefix << message << std::endl;
}

} // namespace

void Log::Info(const std::string& message) { Push(LogLevel::Info, message); }
void Log::Warn(const std::string& message) { Push(LogLevel::Warning, message); }
void Log::Error(const std::string& message) { Push(LogLevel::Error, message); }

const std::vector<LogEntry>& Log::Entries() { return Storage(); }

void Log::Clear() {
    Storage().clear();
    g_Revision++;
}

int Log::CountOf(LogLevel level) {
    int count = 0;
    for (const auto& e : Storage()) {
        if (e.Level == level) count++;
    }
    return count;
}

unsigned int Log::Revision() { return g_Revision; }
