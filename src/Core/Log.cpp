#include "Log.h"
#include "UserPaths.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h> // #178 - Console stack traces (Dbghelp is already linked for MiniDumpWriteDump)
#endif
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
struct PendingEntry {
    LogLevel Level;
    std::string Message;
    LogContext Context;
    std::vector<void*> Stack; // #178
};
std::vector<PendingEntry> g_Pending;
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

void AddEntry(LogLevel level, std::string message, LogContext context, std::vector<void*> stack = {}) {
    auto& entries = Storage();
    if (!entries.empty() && entries.back().Level == level && entries.back().Message == message &&
        entries.back().Context == context) {
        // Keep the first occurrence stack: it is the one with the original context, and
        // re-capturing per repeat would pay for a stack on a per-frame error forever.
        entries.back().Count++;
    } else {
        entries.push_back({level, std::move(message), NowHMS(), 1, g_NextSeq++, std::move(context), std::move(stack)});
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
    std::vector<PendingEntry> batch;
    {
        std::lock_guard<std::mutex> lock(g_PendingMutex);
        batch.swap(g_Pending);
        g_HasPending.store(false, std::memory_order_release);
    }
    for (auto& p : batch) AddEntry(p.Level, std::move(p.Message), std::move(p.Context), std::move(p.Stack));
}

// #178 - return addresses for a Warning/Error, nearest frame first. Thread-safe and cheap (a
// walk of the stack, no symbol work); Info is excluded because it is far too chatty to pay
// anything for. Log own frames are dropped at RESOLVE time by symbol name rather than with a
// FramesToSkip count here: how many frames Push/CaptureStack/Log::Error actually occupy depends
// on what the optimiser inlined, so a fixed count is right in one build configuration and wrong
// in the other.
std::vector<void*> CaptureStack(LogLevel level) {
    if (level == LogLevel::Info) return {};
#ifdef _WIN32
    void* frames[32];
    const USHORT n = CaptureStackBackTrace(/*FramesToSkip=*/1, (DWORD)std::size(frames), frames, nullptr);
    return std::vector<void*>(frames, frames + n);
#else
    return {};
#endif
}

void Push(LogLevel level, const std::string& rawMessage, LogContext context = {}) {
    std::string message = NormalizeSeparators(rawMessage);
    Sink(level, message);
    std::vector<void*> stack = CaptureStack(level);
    if (OnMainThread()) {
        DrainPending();
        AddEntry(level, std::move(message), std::move(context), std::move(stack));
    } else {
        std::lock_guard<std::mutex> lock(g_PendingMutex);
        g_Pending.push_back({level, std::move(message), std::move(context), std::move(stack)});
        g_HasPending.store(true, std::memory_order_release);
    }
}

} // namespace

void Log::Info(const std::string& message) { Push(LogLevel::Info, message); }
void Log::Warn(const std::string& message) { Push(LogLevel::Warning, message); }
void Log::Error(const std::string& message) { Push(LogLevel::Error, message); }
void Log::Info(const std::string& message, const LogContext& context) { Push(LogLevel::Info, message, context); }
void Log::Warn(const std::string& message, const LogContext& context) { Push(LogLevel::Warning, message, context); }
void Log::Error(const std::string& message, const LogContext& context) { Push(LogLevel::Error, message, context); }

// #178 - main thread only, and only when the user expands a row: SymInitialize walks the loaded
// modules and is slow enough that doing it per logged error would be worse than the bug being
// diagnosed. dbghelp Sym* is single-threaded, hence the main-thread rule in the header.
std::string Log::ResolveStack(const std::vector<void*>& frames) {
    if (frames.empty()) return {};
#ifdef _WIN32
    static bool s_Init = false, s_Ok = false;
    if (!s_Init) {
        s_Init = true;
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        s_Ok = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }
    if (!s_Ok) return {};

    const HANDLE proc = GetCurrentProcess();
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    // Drop the leading frames belonging to the logging machinery itself, so the first line is
    // the code that actually logged. Keyed on the SOURCE FILE rather than the symbol name: the
    // file-local helpers here resolve as "`anonymous namespace'::Push", and how many frames they
    // occupy depends on what the optimiser inlined, so neither a name match nor a fixed
    // FramesToSkip count is reliable across build configurations.
    bool pastInternals = false;

    std::string out;
    for (void* addr : frames) {
        const DWORD64 a = reinterpret_cast<DWORD64>(addr);
        std::string name = "(unknown)";
        DWORD64 disp = 0;
        if (SymFromAddr(proc, a, &disp, sym)) name = sym->Name;
        std::string where, file;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(proc, a, &lineDisp, &line) && line.FileName) {
            // Just the file name: the full build path is noise in a Console row.
            file = std::filesystem::path(line.FileName).filename().string();
            where = "  " + file + ":" + std::to_string(line.LineNumber);
        }
        if (!pastInternals) {
            // No line info (a release frame, a foreign module) ends the skip too, rather than
            // silently eating the caller.
            if (file == "Log.cpp") continue;
            pastInternals = true;
        }
        if (!out.empty()) out += "\n";
        out += name + where;
    }
    return out;
#else
    return {};
#endif
}

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
