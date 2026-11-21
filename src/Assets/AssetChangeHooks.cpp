#include "AssetChangeHooks.h"
#include "AssetDatabase.h"
#include "Core/Paths.h"
#include "ProjectWatcher.h"

#include <filesystem>
#include <vector>

namespace AssetChangeHooks {
namespace {

struct Entry {
    int Id = 0;
    std::string Type;    // asset type, or empty for a file hook
    std::string File;    // the file as registered (a relative one resolves per Dispatch: the project may change), or empty
    Hook Fn;
};

std::vector<Entry>& Entries() {
    static std::vector<Entry> s_Entries;
    return s_Entries;
}
int s_NextId = 1;

std::string FileKeyOf(const std::string& path) {
    if (path.empty()) return {};
    const std::string abs = std::filesystem::path(path).is_absolute() ? path : ProjectPaths::Resolve(path);
    return AssetDatabase::PathKey(abs);
}

} // namespace

int Register(const std::string& assetType, Hook hook) {
    if (assetType.empty() || !hook) return 0;
    Entries().push_back({s_NextId, assetType, {}, std::move(hook)});
    return s_NextId++;
}

int RegisterFile(const std::string& path, Hook hook) {
    if (path.empty() || !hook) return 0;
    Entries().push_back({s_NextId, {}, path, std::move(hook)});
    return s_NextId++;
}

void Unregister(int id) {
    auto& v = Entries();
    for (auto it = v.begin(); it != v.end(); ++it)
        if (it->Id == id) { v.erase(it); return; }
}

int Dispatch(const Event& e) {
    // A copy: a hook may register or unregister (a reload that rebuilds its owner).
    const std::vector<Entry> entries = Entries();
    if (entries.empty()) return 0;
    const bool rescan = e.Type == Event::Kind::Rescan;
    const std::string key = rescan ? std::string() : FileKeyOf(e.Path);
    const std::string oldKey = e.Type == Event::Kind::Moved ? FileKeyOf(e.OldPath) : std::string();
    const std::string oldType = e.Type == Event::Kind::Moved ? AssetDatabase::AssetType(e.OldPath) : std::string();
    int applied = 0;
    for (const Entry& h : entries) {
        bool match = rescan;
        if (!match && !h.Type.empty()) match = h.Type == e.AssetType || (!oldType.empty() && h.Type == oldType);
        if (!match && !h.File.empty()) {
            const std::string fileKey = FileKeyOf(h.File);
            match = fileKey == key || (!oldKey.empty() && fileKey == oldKey);
        }
        if (match && h.Fn(e)) ++applied;
    }
    return applied;
}

bool Watching() { return ProjectWatcher::IsRunning(); }

} // namespace AssetChangeHooks
