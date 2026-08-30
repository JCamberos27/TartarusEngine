#pragma once
#include <string>
#include <vector>

// Engine-wide message log, drained by the editor's Console panel. Everything that used to go
// to std::cerr/std::cout (import failures, shader errors, scene-load problems) routes through
// here instead so it's visible inside the editor rather than only in a terminal nobody has
// open — messages are still mirrored to the real stderr/stdout for headless/attached runs.
enum class LogLevel { Info, Warning, Error };

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
};

class Log {
public:
    static void Info(const std::string& message);
    static void Warn(const std::string& message);
    static void Error(const std::string& message);

    static const std::vector<LogEntry>& Entries();
    static void Clear();

    // Per-level totals for the Console's filter buttons ("3 Errors"), counting collapsed
    // duplicates once each — matching what the panel actually lists.
    static int CountOf(LogLevel level);

    // Bumped every time an entry is added, so the Console can auto-scroll only when something
    // new actually arrived instead of fighting the user's scrollback every frame.
    static unsigned int Revision();
};
