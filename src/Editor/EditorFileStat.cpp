#include "EditorFileStat.h"

#include <string>
#include <unordered_map>

namespace EditorFileStat {
namespace {

struct Entry {
    Info Value;
    Clock::time_point ReadAt;
};

// Keyed by the native path string. Bounded: the Inspector shows a handful of paths at a time, so a
// map that grows past this has collected stale ones (selection changes) and is simply dropped.
constexpr size_t kMaxEntries = 512;
std::unordered_map<std::filesystem::path::string_type, Entry> s_Entries;
std::uint64_t s_DiskReads = 0;

Info ReadDisk(const std::filesystem::path& path) {
    ++s_DiskReads;
    Info info;
    std::error_code ec; // non-throwing: an odd path must not throw out of the frame
    const auto t = std::filesystem::last_write_time(path, ec);
    if (!ec) {
        info.Exists = true;
        info.WriteTime = t;
    }
    return info;
}

} // namespace

Info Get(const std::filesystem::path& path, Clock::time_point now) {
    if (path.empty()) return {};
    const auto it = s_Entries.find(path.native());
    if (it != s_Entries.end() && now >= it->second.ReadAt && now - it->second.ReadAt < kMaxAge)
        return it->second.Value;
    return Refresh(path, now);
}

Info Refresh(const std::filesystem::path& path, Clock::time_point now) {
    if (path.empty()) return {};
    if (s_Entries.size() >= kMaxEntries && !s_Entries.count(path.native())) s_Entries.clear();
    Entry& e = s_Entries[path.native()];
    e.Value = ReadDisk(path);
    e.ReadAt = now;
    return e.Value;
}

void Clear() { s_Entries.clear(); }
std::uint64_t DiskReads() { return s_DiskReads; }

} // namespace EditorFileStat
