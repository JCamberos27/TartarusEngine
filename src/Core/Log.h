#pragma once
#include <string>
#include <vector>

// Engine-wide message log, drained by the editor's Console panel. Everything that used to go
// to std::cerr/std::cout (import failures, shader errors, scene-load problems) routes through
// here instead so it's visible inside the editor rather than only in a terminal nobody has
// open — messages are still mirrored to the real stderr/stdout for headless/attached runs.
enum class LogLevel { Info, Warning, Error };

// #146 — what a message is about, carried beside the text so the Console can link to it without
// parsing the message ("entity #12", a quoted path). Both optional.
struct LogContext {
    int EntityOrder = -1;  // OrderComponent value of the entity concerned; -1 = none
    std::string AssetPath; // asset file concerned; empty = none

    static LogContext Entity(int order) { LogContext c; c.EntityOrder = order; return c; }
    static LogContext Asset(const std::string& path) { LogContext c; c.AssetPath = path; return c; }
    bool operator==(const LogContext& o) const { return EntityOrder == o.EntityOrder && AssetPath == o.AssetPath; }
};

struct LogEntry {
    LogLevel Level = LogLevel::Info;
    std::string Message;
    // Wall-clock "HH:MM:SS" of the first occurrence, formatted once at push time so the Console
    // can show it without re-deriving anything.
    std::string Time;
    // Consecutive identical messages collapse into one entry with a count instead of flooding
    // the panel — a per-frame error would otherwise push everything else out of the ring buffer
    // within a second.
    int Count = 1;
    // Monotonic id assigned when the entry is created (1, 2, 3...; never reused, survives the
    // ring buffer trimming older entries). Lets a consumer remember "the last entry I handled"
    // without relying on vector indices, which shift whenever old entries are dropped.
    unsigned long long Seq = 0;
    LogContext Context;
    // #178 - return addresses captured at the log call, for Warning and Error only (Info is far
    // too chatty to pay for, and a stack is rarely what you want from it). Raw addresses:
    // capturing is cheap and thread-safe, while turning them into names is slow and not, so
    // that happens lazily on the main thread via Log::ResolveStack when the row is expanded.
    std::vector<void*> Stack;
};

// Thread safety (#146): Info/Warn/Error may be called from any thread. Messages from other
// threads are echoed to the terminal and log file at once but reach Entries() only when the main
// thread next reads the log, so Entries(), CountOf(), Revision() and Clear() are main-thread only.
class Log {
public:
    static void Info(const std::string& message);
    static void Warn(const std::string& message);
    static void Error(const std::string& message);
    // With structured context (#146). Identical text with a different context is a separate
    // entry, so a repeated "missing collider" warning for two entities isn't collapsed into one.
    static void Info(const std::string& message, const LogContext& context);
    static void Warn(const std::string& message, const LogContext& context);
    static void Error(const std::string& message, const LogContext& context);

    // Starts mirroring every message into %LOCALAPPDATA%\TartarusEngine\Logs\<fileName> (#146),
    // moving the previous run's file to "<stem>-prev.log" first, and writes out whatever was
    // logged before this call. Written unbuffered so it survives a crash. Returns the full path,
    // or "" if the file couldn't be opened (logging carries on without it).
    static std::string OpenFile(const std::string& fileName);

    // #178 - frames as "module!symbol  file:line" lines, newline-separated, nearest frame
    // first; "" when `frames` is empty or symbols are unavailable. Main thread only: the
    // dbghelp Sym* APIs are single-threaded. The first call initialises the symbol handler,
    // which takes a moment - hence resolving only when the user asks for a stack.
    static std::string ResolveStack(const std::vector<void*>& frames);

    static const std::vector<LogEntry>& Entries();
    static void Clear();

    // Per-level totals for the Console's filter buttons ("3 Errors"), counting collapsed
    // duplicates once each — matching what the panel actually lists.
    static int CountOf(LogLevel level);

    // Bumped every time an entry is added, so the Console can auto-scroll only when something
    // new actually arrived instead of fighting the user's scrollback every frame.
    static unsigned int Revision();
};
