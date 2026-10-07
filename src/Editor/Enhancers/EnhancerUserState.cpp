#include "EnhancerUserState.h"

#include "AtomicFile.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "UserPaths.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>

using json = nlohmann::json;

namespace Enhancers {

namespace {
// Keys this build reads/writes; anything else in the file is carried through untouched.
const char* const kOwnedKeys[] = {
    "version", "sceneBookmarks", "entityBookmarks", "folderBookmarks", "inspectorBookmarks", "defaultParents",
};
bool IsOwnedKey(const std::string& k) {
    for (const char* o : kOwnedKeys) if (k == o) return true;
    return false;
}
} // namespace

EnhancerUserState& EnhancerUserState::Get() {
    static EnhancerUserState s;
    return s;
}

std::string EnhancerUserState::ProjectKey(const std::string& projectRoot) {
    std::string norm;
    norm.reserve(projectRoot.size());
    for (char c : projectRoot) norm.push_back(c == '\\' ? '/' : (char)std::tolower((unsigned char)c));
    while (norm.size() > 1 && norm.back() == '/') norm.pop_back();
    std::uint64_t h = 1469598103934665603ull; // FNV-1a 64
    for (unsigned char c : norm) { h ^= c; h *= 1099511628211ull; }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

std::string EnhancerUserState::Path() {
    return UserPaths::Resolve("enhancers_" + ProjectKey(ProjectPaths::Root()) + ".json");
}

void EnhancerUserState::Reset() {
    SceneBookmarks.clear();
    EntityBookmarks.clear();
    FolderBookmarks.clear();
    InspectorBookmarks.clear();
    DefaultParents.clear();
    m_Unknown = json::object();
    m_Dirty = false;
}

json EnhancerUserState::ToJson() const {
    json root = m_Unknown.is_object() ? m_Unknown : json::object();
    root["version"] = kVersion;
    root["sceneBookmarks"] = RefsToJson(SceneBookmarks);
    root["entityBookmarks"] = RefsToJson(EntityBookmarks);
    root["folderBookmarks"] = RefsToJson(FolderBookmarks);
    root["inspectorBookmarks"] = RefsToJson(InspectorBookmarks);
    json dp = json::object();
    for (const auto& kv : DefaultParents) dp[kv.first] = kv.second;
    root["defaultParents"] = dp;
    return root;
}

void EnhancerUserState::FromJson(const json& root) {
    Reset();
    if (!root.is_object()) return;
    auto list = [&](const char* key) {
        auto it = root.find(key);
        return it != root.end() ? RefsFromJson(*it) : std::vector<EditorRef>{};
    };
    SceneBookmarks = list("sceneBookmarks");
    EntityBookmarks = list("entityBookmarks");
    FolderBookmarks = list("folderBookmarks");
    InspectorBookmarks = list("inspectorBookmarks");
    if (auto it = root.find("defaultParents"); it != root.end() && it->is_object())
        for (auto kv = it->begin(); kv != it->end(); ++kv)
            if (kv.value().is_number_integer()) DefaultParents[kv.key()] = kv.value().get<int>();
    for (auto kv = root.begin(); kv != root.end(); ++kv)
        if (!IsOwnedKey(kv.key())) m_Unknown[kv.key()] = kv.value();
}

void EnhancerUserState::Load() {
    const std::string path = Path();
    std::ifstream in(path);
    if (!in.is_open()) { Reset(); return; } // first run for this project - not an error
    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        Log::Warn(std::string("Editor Enhancers: failed to parse '") + path + "': " + e.what() + " - starting empty.");
        Reset();
        return;
    }
    FromJson(root);
}

void EnhancerUserState::Flush() {
    if (!m_Dirty) return;
    m_Dirty = false;
    const std::string path = Path();
    if (!AtomicFile::WriteJson(path, ToJson()))
        Log::Warn(std::string("Editor Enhancers: could not write '") + path + "'.");
}

} // namespace Enhancers
