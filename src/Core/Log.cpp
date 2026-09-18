#include "Log.h"
#include "UserPaths.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

namespace {

// Ring-buffer caps, per level (#182): the oldest entries of a level drop off once that level
// exceeds its cap. Separate budgets so a flood of Info (physics trigger spam, import chatter)
// can never evict the warnings and errors the user actually needs to see - they only compete
// with their own kind. High enough that a normal session never loses anything, low enough that
// a runaway per-frame log can't grow unbounded.
constexpr size_t kMaxPerLevel[3] = {1000, 500, 500}; // Info, Warning, Error
// #146: trimmed in batches of this many past the cap, not one erase (an O(n) shift) per message.
constexpr size_t kTrimSlack = 200;

// Live per-level entry counts (collapsed duplicates count once), kept in step with Storage() so
// CountOf is O(1) and the trim check doesn't rescan the buffer.
size_t g_LevelCounts[3] = {0, 0, 0};
unsigned long long g_NextSeq = 1;

// Only ever touched on the main thread: the Console iterates the vector Entries() returns
// without a lock, so other threads must never push into it directly (#146). They queue into
// g_Pending instead, which the main thread drains before every read or push.
std::vector<LogEntry>& Storage() {
    static std::vector<LogEntry> entries;
    return entries;
}

unsigned int g_Revision = 0;

// Static initialisation runs on the thread that runs main(), so this is the main thread.
const std::thread::id g_MainThread = std::this_thread::get_id();

std::mutex g_PendingMutex;
std::vector<std::pair<LogLevel, std::string>> g_Pending;
std::atomic<bool> g_HasPending{false};

// Guards the terminal mirror and the log file, which any thread writes immediately (so a
// terminal or Editor.log shows lines in real order even before the main thread drains them).
std::mutex g_SinkMutex;
std::FILE* g_File = nullptr;

std::string NowHMS() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

// Normalizes any embedded Windows path separators to '/'. Call sites pass a mix — some route
// paths through ProjectPaths::Relativize() first (generic_string(), already '/'), others log a
// raw OS path straight from a file dialog or ProjectPaths::Resolve() (native, '\' on Windows) —
// so the same session's Console otherwise shows both in adjacent lines (Appendix A #18).
std::string NormalizeSeparators(std::string message) {
    for (char& c : message) if (c == '\\') c = '/';
    return message;
}

const char* Prefix(LogLevel level) {
    return level == LogLevel::Error ? "[error] " : (level == LogLevel::Warning ? "[warn]  " : "[info]  ");
}

void WriteFileLine(const std::string& time, LogLevel level, const std::string& message) {
    if (!g_File) return;
    const std::string line = time + " " + Prefix(level) + message + "\n";
    std::fwrite(line.data(), 1, line.size(), g_File); // unbuffered: one OS write per line
}

// Mirrored so a terminal-attached run (or a crash before the editor draws) still shows it.
// Info isn't flushed per line any more (#146: std::endl on every message was slow under log
// spam); warnings and errors still are, so they're never lost behind a buffer.
void Sink(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_SinkMutex);
    std::ostream& out = (level == LogLevel::Error) ? std::cerr : std::cout;
    out << Prefix(level) << message << '\n';
    if (level != LogLevel::Info) out.flush();
    WriteFileLine(NowHMS(), level, message);
}

void AddEntry(LogLevel level, std::string message) {
    auto& entries = Storage();
    if (!entries.empty() && entries.back().Level == level && entries.back().Message == message) {
        entries.back().Count++;
    } else {
        entries.push_back({level, std::move(message), NowHMS(), 1, g_NextSeq++});
        const size_t li = (size_t)level;
        if (++g_LevelCounts[li] > kMaxPerLevel[li] + kTrimSlack) {
            // Drop this level's oldest entries back down to its cap; other levels are untouched
            // and relative order is preserved.
            size_t toDrop = g_LevelCounts[li] - kMaxPerLevel[li];
            g_LevelCounts[li] -= toDrop;
            auto newEnd = std::remove_if(entries.begin(), entries.end(), [&](const LogEntry& e) {
                if (toDrop == 0 || e.Level != level) return false;
                --toDrop;
                return true;
            });
            entries.erase(newEnd, entries.end());
        }
    }
    g_Revision++;
}

bool OnMainThread() { return std::this_thread::get_id() == g_MainThread; }

// Main thread only: moves messages other threads logged into the Console's storage.
void DrainPending() {
    if (!g_HasPending.load(std::memory_order_acquire)) return;
    std::vector<std::pair<LogLevel, std::string>> batch;
    {
        std::lock_guard<std::mutex> lock(g_PendingMutex);
        batch.swap(g_Pending);
        g_HasPending.store(false, std::memory_order_release);
    }
    for (auto& [level, message] : batch) AddEntry(level, std::move(message));
}

void Push(LogLevel level, const std::string& rawMessage) {
    std::string message = NormalizeSeparators(rawMessage);
    Sink(level, message);
    if (OnMainThread()) {
        DrainPending();
        AddEntry(level, std::move(message));
    } else {
        std::lock_guard<std::mutex> lock(g_PendingMutex);
        g_Pending.emplace_back(level, std::move(message));
        g_HasPending.store(true, std::memory_order_release);
    }
}

} // namespace

void Log::Info(const std::string& message) { Push(LogLevel::Info, message); }
void Log::Warn(const std::string& message) { Push(LogLevel::Warning, message); }
void Log::Error(const std::string& message) { Push(LogLevel::Error, message); }

const std::vector<LogEntry>& Log::Entries() {
    if (OnMainThread()) DrainPending();
    return Storage();
}

void Log::Clear() {
    if (OnMainThread()) DrainPending();
    Storage().clear();
    for (size_t& c : g_LevelCounts) c = 0;
    g_Revision++;
}

int Log::CountOf(LogLevel level) {
    if (OnMainThread()) DrainPending();
    const size_t li = (size_t)level;
    return li < 3 ? (int)g_LevelCounts[li] : 0;
}

unsigned int Log::Revision() {
    if (OnMainThread()) DrainPending();
    return g_Revision;
}

std::string Log::OpenFile(const std::string& fileName) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(UserPaths::Resolve("Logs"));
    fs::create_directories(dir, ec);
    const fs::path path = dir / fileName;
    // Keep exactly one previous run, like Unity's Editor-prev.log.
    fs::path prev = path;
    prev.replace_filename(path.stem().string() + "-prev" + path.extension().string());
    if (fs::exists(path, ec)) {
        fs::remove(prev, ec);
        fs::rename(path, prev, ec);
    }

    std::lock_guard<std::mutex> lock(g_SinkMutex);
    if (g_File) std::fclose(g_File);
#if defined(_MSC_VER)
    g_File = _wfsopen(path.wstring().c_str(), L"wb", _SH_DENYWR); // readable while we write
#else
    g_File = std::fopen(path.string().c_str(), "wb");
#endif
    if (!g_File) return {};
    // Unbuffered: every line reaches the OS as it's logged, so the file survives a crash with
    // no flush from the crash handler (which must not take our locks).
    std::setvbuf(g_File, nullptr, _IONBF, 0);

    std::time_t t = std::time(nullptr);
    char stamp[64];
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    const std::string header = std::string("Tartarus Engine log, started ") + stamp + "\n";
    std::fwrite(header.data(), 1, header.size(), g_File);
    // Anything logged before the file existed (main thread only reaches here at startup).
    for (const LogEntry& e : Storage())
        for (int i = 0; i < e.Count; ++i) WriteFileLine(e.Time, e.Level, e.Message);
    return path.string();
}
