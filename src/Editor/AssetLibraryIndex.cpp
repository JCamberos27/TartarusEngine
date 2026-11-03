#include "AssetLibraryIndex.h"

#include "Enhancers/EnhancerCore.h" // GlobMatch
#include "Log.h"
#include "ProjectPaths.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

std::vector<std::string>& AssetLibraryView::Folders() {
    static std::vector<std::string> folders;
    return folders;
}

std::vector<std::string>& AssetLibraryView::Packages() {
    static std::vector<std::string> packages;
    return packages;
}

std::string AssetLibraryView::PackageOf(const std::string& path) {
    if (path.empty()) return {};
    std::string rel = AssetLibraryIndex::ProjectRelative(path);
    for (char& c : rel) c = (char)std::tolower((unsigned char)c);
    for (const std::string& p : Packages()) {
        std::string lp = p;
        for (char& c : lp) c = (char)std::tolower((unsigned char)c);
        if (rel == lp || (rel.size() > lp.size() && rel.compare(0, lp.size(), lp) == 0 && rel[lp.size()] == '/')) return p;
    }
    return {};
}

bool AssetLibraryView::InPackage(const std::string& path) { return !PackageOf(path).empty(); }

void AssetLibraryIndex::LoadPackages(const std::string& text) {
    m_Packages.clear();
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    for (const auto& p : j.value("packages", nlohmann::json::array()))
        if (p.is_object() && p.contains("folder") && p["folder"].is_string()) m_Packages.push_back(p["folder"].get<std::string>());
}

std::string AssetLibraryIndex::ProjectRelative(const std::string& path) {
    std::string p = ProjectPaths::Relativize(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

std::string AssetLibraryIndex::LabelOf(const std::string& categoryId) const {
    if (categoryId.empty() || categoryId == "other") return {};
    for (const auto& c : m_Categories) {
        if (c.Id != categoryId) continue;
        std::string label;
        for (char ch : c.Name) if (std::isalnum((unsigned char)ch)) label += ch;
        return label;
    }
    return {};
}

std::set<std::string> AssetLibraryIndex::LabelsOf(const std::string& categoryId) const {
    std::set<std::string> out;
    if (std::string l = LabelOf(categoryId); !l.empty()) out.insert(l);
    for (const auto& c : m_Categories)
        if (c.Id == categoryId && !c.Parent.empty())
            if (std::string l = LabelOf(c.Parent); !l.empty()) out.insert(l);
    return out;
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
    if (std::ifstream pk(ProjectPaths::Resolve("packages.json")); pk) {
        std::stringstream ps;
        ps << pk.rdbuf();
        LoadPackages(ps.str());
    }
    AssetLibraryView::Packages() = m_Packages;
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
