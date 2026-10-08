#include "UiStyles.h"
#include "Palette.h"

#include "AtomicFile.h"
#include "Log.h"
#include "ProjectPaths.h"

#include <algorithm>
#include <fstream>

using json = nlohmann::json;

namespace Enhancers {

namespace {
json StyleToJson(const UiStyle& s) {
    json j = json::object();
    if (!s.Icon.empty()) j["icon"] = s.Icon;
    if (s.Color) j["color"] = ToHexColor(s.Color);
    return j;
}
UiStyle StyleFromJson(const json& j) {
    UiStyle s;
    if (!j.is_object()) return s;
    if (auto it = j.find("icon"); it != j.end() && it->is_string()) s.Icon = it->get<std::string>();
    if (auto it = j.find("color"); it != j.end() && it->is_string()) {
        std::uint32_t c = 0;
        if (ParseHexColor(it->get<std::string>(), c)) s.Color = c;
    }
    return s;
}
void MapFromJson(const json& j, const char* key, std::map<std::string, UiStyle>& out) {
    if (auto it = j.find(key); it != j.end() && it->is_object())
        for (auto kv = it->begin(); kv != it->end(); ++kv) {
            UiStyle s = StyleFromJson(kv.value());
            if (!s.IsEmpty()) out[kv.key()] = s;
        }
}
json MapToJson(const std::map<std::string, UiStyle>& m) {
    json o = json::object();
    for (const auto& kv : m)
        if (!kv.second.IsEmpty()) o[kv.first] = StyleToJson(kv.second);
    return o;
}
} // namespace

UiStyles& UiStyles::Get() {
    static UiStyles s;
    return s;
}

const UiStyle* UiStyles::Panel(const std::string& id) const {
    auto it = Panels.find(id);
    return it != Panels.end() && !it->second.IsEmpty() ? &it->second : nullptr;
}
const UiStyle* UiStyles::Component(const std::string& name) const {
    auto it = Components.find(name);
    return it != Components.end() && !it->second.IsEmpty() ? &it->second : nullptr;
}

void UiStyles::SetPanel(const std::string& id, const UiStyle& s) {
    if (s.IsEmpty()) Panels.erase(id);
    else Panels[id] = s;
    MarkDirty();
}
void UiStyles::SetComponent(const std::string& name, const UiStyle& s) {
    if (s.IsEmpty()) Components.erase(name);
    else Components[name] = s;
    MarkDirty();
}

UiStyles UiStyles::Spectrum(const std::vector<std::string>& extraComponents) {
    // The first ten palette entries are the hue wheel (Palette::Defaults); the neutrals after
    // them aren't part of the spectrum.
    const Palette defaults = Palette::Defaults();
    const std::size_t hues = std::min<std::size_t>(10, defaults.Colors.size());
    auto hue = [&](std::size_t i) { return defaults.Colors[i % hues]; };

    // Panel ids (EditorPanels.h, after "###"; the Script IDE has no "###" and is keyed by its
    // whole title), in the order the default layout reads left to right, top to bottom.
    static const char* const kPanels[] = {
        "Hierarchy", "Scene", "Game", "Script IDE", "Inspector", "Statistics", "Assets",
        "Console", "History", "Lighting", "Audio", "Settings", "PhysicsDebug", "Animator", "AssetLibrary",
    };
    // The components an entity most often shows, top of the Inspector down.
    static const char* const kComponents[] = {
        "Transform", "Mesh Renderer", "Material", "Animator Controller", "Outfit Piece",
    };

    UiStyles s;
    std::size_t i = 0;
    for (const char* id : kPanels) s.Panels[id].Color = hue(i++);
    i = 0;
    std::vector<std::string> order(std::begin(kComponents), std::end(kComponents));
    for (const auto& name : extraComponents)
        if (std::find(order.begin(), order.end(), name) == order.end()) order.push_back(name);
    for (const auto& name : order) s.Components[name].Color = hue(i++);
    return s;
}

json UiStyles::ToJson() const {
    json j;
    j["version"] = kVersion;
    j["panels"] = MapToJson(Panels);
    j["components"] = MapToJson(Components);
    return j;
}

void UiStyles::FromJson(const json& j) {
    Reset();
    if (!j.is_object()) return;
    MapFromJson(j, "panels", Panels);
    MapFromJson(j, "components", Components);
}

void UiStyles::Reset() {
    Panels.clear();
    Components.clear();
    m_Dirty = false;
}

std::string UiStyles::Path() { return ProjectPaths::Resolve("editor_ui_styles.json"); }

void UiStyles::Load() {
    std::ifstream in(Path());
    if (!in.is_open()) { Reset(); return; }
    try {
        json j;
        in >> j;
        FromJson(j);
    } catch (const std::exception& e) {
        Log::Warn(std::string("Editor Enhancers: failed to parse '") + Path() + "': " + e.what() + " - tab and header styles off until fixed.");
        Reset();
    }
}

void UiStyles::Flush() {
    if (!m_Dirty) return;
    m_Dirty = false;
    if (!AtomicFile::WriteJson(Path(), ToJson()))
        Log::Warn(std::string("Editor Enhancers: could not write '") + Path() + "'.");
}

bool UiStyles::ExportTo(const std::string& path) const {
    return !path.empty() && AtomicFile::WriteJson(path, ToJson());
}

bool UiStyles::ImportFrom(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    try {
        json j;
        in >> j;
        // Neither map present: not a styles file - refuse rather than wipe the user's styles.
        if (!j.is_object() || (!j.contains("panels") && !j.contains("components"))) return false;
        FromJson(j);
        MarkDirty();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace Enhancers
