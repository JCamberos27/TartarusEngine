#include "DecalAssetCache.h"

#include "AssetDatabase.h"
#include "DecalAsset.h"
#include "Core/Paths.h"

#include <filesystem>
#include <unordered_map>

namespace fs = std::filesystem;

namespace DecalAssetCache {
namespace {

struct Entry {
    fs::file_time_type Stamp{};
    Clock::time_point Checked{};
    std::shared_ptr<DecalAsset> Asset;
    std::string Abs;       // the .decal's absolute path
    std::string AlbedoAbs; // resolved once per load
};

std::string AbsoluteAssetPath(const std::string& p) {
    if (p.empty()) return {};
    return fs::u8path(p).is_absolute() ? p : ProjectPaths::Resolve(p);
}

// One entry per file (by PathKey); every spelling a caller used points at it. Element pointers of
// an unordered_map stay valid across rehashing.
std::unordered_map<std::string, Entry> s_Files;
std::unordered_map<std::string, Entry*> s_Spellings;
std::uint64_t s_Frame = ~0ull;
int s_StatsThisFrame = 0;
std::uint64_t s_DiskStats = 0, s_Loads = 0;

void Load(Entry& e, fs::file_time_type stamp) {
    ++s_Loads;
    e.Stamp = stamp;
    e.Asset = DecalAsset::Load(e.Abs);
    e.AlbedoAbs = e.Asset && !e.Asset->Missing ? AbsoluteAssetPath(e.Asset->AlbedoPath) : std::string();
}

} // namespace

Hit Get(const std::string& path, std::uint64_t frame, Clock::time_point now, bool force) {
    if (path.empty()) return {};
    if (frame != s_Frame) {
        s_Frame = frame;
        s_StatsThisFrame = 0;
    }
    Entry* e = nullptr;
    if (const auto it = s_Spellings.find(path); it != s_Spellings.end()) {
        e = it->second;
    } else {
        std::string abs = AbsoluteAssetPath(path);
        e = &s_Files[AssetDatabase::PathKey(abs)];
        if (e->Abs.empty()) e->Abs = std::move(abs);
        s_Spellings.emplace(path, e);
    }
    const bool stale = now - e->Checked >= kMaxAge || now < e->Checked;
    if (e->Asset && !force && (!stale || s_StatsThisFrame >= kStatsPerFrame)) return {e->Asset, &e->AlbedoAbs};
    if (e->Asset && !force) ++s_StatsThisFrame; // a first read is never held back: the tile needs it
    e->Checked = now;
    ++s_DiskStats;
    std::error_code ec;
    const auto stamp = fs::last_write_time(fs::u8path(e->Abs), ec);
    if (!e->Asset || stamp != e->Stamp) Load(*e, stamp);
    return {e->Asset, &e->AlbedoAbs};
}

void Clear() {
    s_Spellings.clear();
    s_Files.clear();
    s_Frame = ~0ull;
    s_StatsThisFrame = 0;
}

std::uint64_t DiskStats() { return s_DiskStats; }
std::uint64_t Loads() { return s_Loads; }

} // namespace DecalAssetCache
