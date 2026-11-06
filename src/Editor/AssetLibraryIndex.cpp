#include "AssetLibraryIndex.h"

#include "Enhancers/EnhancerCore.h" // GlobMatch
#include "Log.h"
#include "Core/Paths.h"

#include <json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

std::vector<std::string>& AssetLibraryView::Folders() {
    static std::vector<std::string> folders;
    return folders;
}

std::string AssetLibraryIndex::ProjectRelative(const std::string& path) {
    std::string p = ProjectPaths::Relativize(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

void AssetLibraryIndex::Load() {
    std::ifstream in(ProjectPaths::Resolve("asset_library.json"));
    if (!in) {
        *this = AssetLibraryIndex();
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    LoadFromText(ss.str());
}

void AssetLibraryIndex::LoadFromText(const std::string& text) {
    *this = AssetLibraryIndex();
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        Log::Warn("asset_library.json: not valid JSON; the Library view is empty");
        return;
    }
    for (const auto& c : j.value("categories", nlohmann::json::array()))
        if (c.is_object())
            m_Categories.push_back({c.value("id", ""), c.value("name", ""), c.value("parent", "")});
    for (const auto& r : j.value("rules", nlohmann::json::array()))
        if (r.is_object()) m_Rules.emplace_back(r.value("glob", ""), r.value("category", "other"));
    const nlohmann::json kits = j.value("kits", nlohmann::json::object()); // items() must not outlive it
    for (const auto& [prefix, module] : kits.items())
        if (module.is_number()) m_Kits.emplace_back(prefix, module.get<float>());
}

const std::string& AssetLibraryIndex::CategoryOf(const std::string& path) const {
    static const std::string kOther = "other";
    for (const auto& [glob, id] : m_Rules)
        if (Enhancers::GlobMatch(glob.c_str(), path.c_str())) return id;
    return kOther;
}

float AssetLibraryIndex::KitModuleOf(const std::string& path) const {
    for (const auto& [prefix, module] : m_Kits)
        if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '/')
            return module;
    return 0.0f;
}
