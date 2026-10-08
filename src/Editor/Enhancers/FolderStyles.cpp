#include "FolderStyles.h"
#include "EnhancerCore.h"
#include "Palette.h"

#include "AtomicFile.h"
#include "Log.h"
#include "ProjectPaths.h"

#include <algorithm>
#include <fstream>

using json = nlohmann::json;

namespace Enhancers {

FolderKind DominantKind(const FolderSummary& s, float share, int minCount) {
    if (s.Total == 0) return FolderKind::Count;
    int best = -1;
    for (int k = 0; k < kFolderKindCount; ++k)
        if (best < 0 || s.Counts[k] > s.Counts[best]) best = k;
    if (best < 0 || (FolderKind)best == FolderKind::Other) return FolderKind::Count;
    const int n = s.Counts[best];
    if (n < minCount || (float)n < share * (float)s.Total) return FolderKind::Count;
    return (FolderKind)best;
}

int TopKinds(const FolderSummary& s, FolderKind* out, int max) {
    int idx[kFolderKindCount];
    int n = 0;
    for (int k = 0; k < kFolderKindCount; ++k)
        if (s.Counts[k] > 0) idx[n++] = k;
    std::stable_sort(idx, idx + n, [&](int a, int b) { return s.Counts[a] > s.Counts[b]; });
    const int m = std::min(n, max);
    for (int i = 0; i < m; ++i) out[i] = (FolderKind)idx[i];
    return m;
}

const char* FolderKindIconName(FolderKind k) {
    switch (k) {
        case FolderKind::Model:     return "cube";
        case FolderKind::Texture:   return "image";
        case FolderKind::Material:  return "droplet";
        case FolderKind::Sound:     return "music";
        case FolderKind::Prefab:    return "box-archive";
        case FolderKind::Scene:     return "map";
        case FolderKind::Script:    return "scroll";
        case FolderKind::Animation: return "diagram-project";
        case FolderKind::Shader:    return "file-code";
        default:                    return nullptr;
    }
}

const char* FolderKindLabel(FolderKind k) {
    switch (k) {
        case FolderKind::Model:     return "Models";
        case FolderKind::Texture:   return "Textures";
        case FolderKind::Material:  return "Materials";
        case FolderKind::Sound:     return "Sounds";
        case FolderKind::Prefab:    return "Prefabs";
        case FolderKind::Scene:     return "Scenes";
        case FolderKind::Script:    return "Scripts";
        case FolderKind::Animation: return "Animation";
        case FolderKind::Shader:    return "Shaders";
        default:                    return "Other";
    }
}

FolderStyles& FolderStyles::Get() {
    static FolderStyles s;
    return s;
}

const FolderRule* FolderStyles::MatchRule(const std::string& path) const {
    if (Rules.empty()) return nullptr;
    const size_t slash = path.find_last_of('/');
    const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    for (const auto& r : Rules)
        if (!r.Pattern.empty() && (GlobMatch(r.Pattern.c_str(), path.c_str()) || GlobMatch(r.Pattern.c_str(), leaf.c_str())))
            return &r;
    return nullptr;
}

ResolvedFolderStyle FolderStyles::Resolve(const std::string& path, const FolderSummary* summary) const {
    ResolvedFolderStyle out;
    // Built-in: the Asset Browser's filesystem-backed virtual folders.
    if (path == "Scenes")           { out.Icon = "map";      out.IconSource = FolderStyleSource::Builtin; }
    else if (path == "Screenshots") { out.Icon = "camera";   out.IconSource = FolderStyleSource::Builtin; }
    else if (path == "Shaders")     { out.Icon = "file-code"; out.IconSource = FolderStyleSource::Builtin; }
    else if (AutoIcons && summary) {
        const FolderKind k = DominantKind(*summary);
        if (const char* n = k != FolderKind::Count ? FolderKindIconName(k) : nullptr) {
            out.Icon = n;
            out.IconSource = FolderStyleSource::Auto;
        }
    }
    if (const FolderRule* r = MatchRule(path)) {
        if (!r->Style.Icon.empty()) { out.Icon = r->Style.Icon; out.IconSource = FolderStyleSource::Rule; }
        if (r->Style.Color) out.Color = r->Style.Color;
    }
    const auto it = Folders.find(path);
    if (it != Folders.end()) {
        if (!it->second.Icon.empty()) { out.Icon = it->second.Icon; out.IconSource = FolderStyleSource::Explicit; }
        if (it->second.Color) out.Color = it->second.Color;
    }
    // A name that no longer resolves (an FA rename, a hand edit) falls back to the folder glyph.
    if (!out.Icon.empty() && !FAIconGlyph(out.Icon.c_str())) { out.Icon.clear(); out.IconSource = FolderStyleSource::Default; }
    return out;
}

int FolderStyles::OnFolderPathChanged(const std::string& oldPath, const std::string& newPath) {
    const int n = RemapFolderKeys(Folders, oldPath, newPath);
    if (n) m_Dirty = true;
    return n;
}

namespace {
json StyleToJson(const FolderStyle& s) {
    json j = json::object();
    if (!s.Icon.empty()) j["icon"] = s.Icon;
    if (s.Color) j["color"] = ToHexColor(s.Color);
    return j;
}
FolderStyle StyleFromJson(const json& j) {
    FolderStyle s;
    if (!j.is_object()) return s;
    if (auto it = j.find("icon"); it != j.end() && it->is_string()) s.Icon = it->get<std::string>();
    if (auto it = j.find("color"); it != j.end() && it->is_string()) {
        std::uint32_t c = 0;
        if (ParseHexColor(it->get<std::string>(), c)) s.Color = c;
    }
    return s;
}
} // namespace

json FolderStyles::ToJson() const {
    json j;
    j["version"] = kVersion;
    j["autoIcons"] = AutoIcons;
    json f = json::object();
    for (const auto& kv : Folders)
        if (!kv.second.IsEmpty()) f[kv.first] = StyleToJson(kv.second);
    j["folders"] = f;
    json r = json::array();
    for (const auto& rule : Rules) {
        json e = StyleToJson(rule.Style);
        e["pattern"] = rule.Pattern;
        r.push_back(e);
    }
    j["rules"] = r;
    return j;
}

void FolderStyles::FromJson(const json& j) {
    Reset();
    if (!j.is_object()) return;
    if (auto it = j.find("autoIcons"); it != j.end() && it->is_boolean()) AutoIcons = it->get<bool>();
    if (auto it = j.find("folders"); it != j.end() && it->is_object())
        for (auto kv = it->begin(); kv != it->end(); ++kv) {
            FolderStyle s = StyleFromJson(kv.value());
            if (!s.IsEmpty()) Folders[kv.key()] = s;
        }
    if (auto it = j.find("rules"); it != j.end() && it->is_array())
        for (const auto& e : *it) {
            if (!e.is_object()) continue;
            FolderRule r;
            if (auto p = e.find("pattern"); p != e.end() && p->is_string()) r.Pattern = p->get<std::string>();
            r.Style = StyleFromJson(e);
            if (!r.Pattern.empty()) Rules.push_back(std::move(r));
        }
}

void FolderStyles::Reset() {
    Folders.clear();
    Rules.clear();
    AutoIcons = true;
    m_Dirty = false;
}

std::string FolderStyles::Path() { return ProjectPaths::Resolve("editor_folders.json"); }

void FolderStyles::Load() {
    std::ifstream in(Path());
    if (!in.is_open()) { Reset(); return; }
    try {
        json j;
        in >> j;
        FromJson(j);
    } catch (const std::exception& e) {
        Log::Warn(std::string("Editor Enhancers: failed to parse '") + Path() + "': " + e.what() + " - folder styles off until fixed.");
        Reset();
    }
}

void FolderStyles::Flush() {
    if (!m_Dirty) return;
    m_Dirty = false;
    if (!AtomicFile::WriteJson(Path(), ToJson()))
        Log::Warn(std::string("Editor Enhancers: could not write '") + Path() + "'.");
}

} // namespace Enhancers
