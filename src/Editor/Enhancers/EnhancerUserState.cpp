#include "EnhancerUserState.h"

#include "AtomicFile.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "UserPaths.h"

#include <algorithm>
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
    "favoritePages", "favoritesMigrated",
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
    FavoritePages.clear();
    FavoritesMigrated = false;
    ++m_Revision;
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
    json pages = json::array();
    for (const auto& p : FavoritePages) pages.push_back(json{{"name", p.Name}, {"items", RefsToJson(p.Items)}});
    root["favoritePages"] = pages;
    root["favoritesMigrated"] = FavoritesMigrated;
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
    if (auto it = root.find("favoritePages"); it != root.end() && it->is_array())
        for (const auto& p : *it) {
            if (!p.is_object()) continue;
            FavoritePage page;
            page.Name = p.contains("name") && p.at("name").is_string() ? p.at("name").get<std::string>() : std::string("Page");
            if (auto items = p.find("items"); items != p.end()) page.Items = RefsFromJson(*items);
            FavoritePages.push_back(std::move(page));
        }
    if (auto it = root.find("favoritesMigrated"); it != root.end() && it->is_boolean()) FavoritesMigrated = it->get<bool>();
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

namespace Enhancers {

void EnsureFavoritePage(std::vector<FavoritePage>& pages) {
    if (pages.empty()) pages.push_back(FavoritePage{"Favorites", {}});
}

bool AddFavorite(std::vector<FavoritePage>& pages, int page, const EditorRef& r) {
    EnsureFavoritePage(pages);
    page = std::clamp(page, 0, (int)pages.size() - 1);
    return AddUnique(pages[(std::size_t)page].Items, r, 256) && FindRef(pages[(std::size_t)page].Items, r) >= 0;
}

bool RemoveFavorite(std::vector<FavoritePage>& pages, const EditorRef& r) {
    bool any = false;
    for (auto& p : pages) any = RemoveRef(p.Items, r) || any;
    return any;
}

int FindFavorite(const std::vector<FavoritePage>& pages, const EditorRef& r) {
    for (std::size_t i = 0; i < pages.size(); ++i)
        if (FindRef(pages[i].Items, r) >= 0) return (int)i;
    return -1;
}

void MoveFavoritePage(std::vector<FavoritePage>& pages, int from, int to) {
    if (from < 0 || from >= (int)pages.size()) return;
    to = std::clamp(to, 0, (int)pages.size() - 1);
    if (from == to) return;
    FavoritePage p = std::move(pages[(std::size_t)from]);
    pages.erase(pages.begin() + from);
    pages.insert(pages.begin() + to, std::move(p));
}

} // namespace Enhancers
