#pragma once

// Cached file status for checks the editor UI makes every frame (does this referenced asset exist,
// did this material file change on disk). Each answer is reused for up to kMaxAge, so a field drawn
// every frame costs one stat call twice a second instead of one per frame (each is a kernel call,
// slower still with antivirus hooks). A file deleted or edited outside the editor shows within kMaxAge.
// Editor UI thread only.

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace EditorFileStat {

using Clock = std::chrono::steady_clock;
inline constexpr Clock::duration kMaxAge = std::chrono::milliseconds(500);

struct Info {
    bool Exists = false;                          // false also when the path can't be read
    std::filesystem::file_time_type WriteTime{};  // valid only when Exists
};

// The path's status, read from disk when there is no entry for it or the entry is older than kMaxAge.
Info Get(const std::filesystem::path& path, Clock::time_point now = Clock::now());
inline bool Exists(const std::filesystem::path& path) { return Get(path).Exists; }

// Re-reads now (after the editor itself wrote the file) and returns the fresh status.
Info Refresh(const std::filesystem::path& path, Clock::time_point now = Clock::now());

void Clear();
std::uint64_t DiskReads(); // stat calls made so far (tests, profiling)

} // namespace EditorFileStat
