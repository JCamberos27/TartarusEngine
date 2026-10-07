#pragma once
#include <json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Editor Enhancers / vFolders (docs/EDITOR_ENHANCERS.md): Asset Browser folder icons and colours.
//
// Project data, shared by the team: saved to project/editor_folders.json. Asset Browser folders
// are virtual project data (project/settings.json "assetFolders") with no .meta or GUID of their
// own, so styles are keyed by folder path and re-keyed on rename/delete through
// AssetLibrary::OnFolderPathChanged. The file is written through AtomicFile, so the global file
// undo journals it - styling a folder is undoable like any other project-file edit.
//
// A folder's look resolves in layers, each overriding only what it sets:
//   built-in (Scenes / Screenshots / Shaders) -> automatic icon from its content -> the first
//   matching rule -> the folder's own style.
namespace Enhancers {

struct FolderStyle {
    std::string   Icon;      // Font Awesome name, "" = not set
    std::uint32_t Color = 0; // IM_COL32-packed, 0 = not set
    bool IsEmpty() const { return Icon.empty() && Color == 0; }
};

// Pattern is a case-insensitive glob (GlobMatch) tried against both the full folder path and its
// leaf name: "Materials" styles every folder named Materials, "Art/*/Textures" only those.
struct FolderRule {
    std::string Pattern;
    FolderStyle Style;
};

// What a folder directly holds, by kind - feeds the automatic icon and the content minimap.
enum class FolderKind : std::uint8_t { Model, Texture, Material, Sound, Prefab, Scene, Script, Animation, Shader, Other, Count };
constexpr int kFolderKindCount = (int)FolderKind::Count;
struct FolderSummary {
    std::uint16_t Counts[kFolderKindCount] = {};
    std::uint16_t Total = 0;
    void Add(FolderKind k) { if (Counts[(int)k] < 0xFFFF) ++Counts[(int)k]; if (Total < 0xFFFF) ++Total; }
};
// The kind making up at least `share` of the folder's direct assets (with at least `minCount`
// of them), else FolderKind::Count.
FolderKind DominantKind(const FolderSummary& s, float share = 0.6f, int minCount = 2);
// Up to `max` kinds present, most numerous first (ties in enum order). Returns how many.
int TopKinds(const FolderSummary& s, FolderKind* out, int max);
const char* FolderKindIconName(FolderKind k); // FA name; nullptr for Other
const char* FolderKindLabel(FolderKind k);    // "Models", "Textures", ...

enum class FolderStyleSource : std::uint8_t { Default, Builtin, Auto, Rule, Explicit };
struct ResolvedFolderStyle {
    std::string       Icon;  // FA name; "" = the plain folder glyph
    std::uint32_t     Color = 0;
    FolderStyleSource IconSource = FolderStyleSource::Default;
};

class FolderStyles {
public:
    static FolderStyles& Get();

    std::map<std::string, FolderStyle> Folders; // folder path -> its own style
    std::vector<FolderRule> Rules;              // first match wins
    bool AutoIcons = true;                      // icon from content when nothing else sets one

    ResolvedFolderStyle Resolve(const std::string& path, const FolderSummary* summary) const;
    const FolderRule* MatchRule(const std::string& path) const;

    // Folder renamed / moved (`newPath` set) or deleted (`newPath` empty): re-key the styles of it
    // and everything under it. Returns the number of entries changed.
    int OnFolderPathChanged(const std::string& oldPath, const std::string& newPath);

    nlohmann::json ToJson() const;
    void FromJson(const nlohmann::json& j);
    void Reset();

    void Load();   // project/editor_folders.json; a missing file is an empty store
    void MarkDirty() { m_Dirty = true; }
    void Flush();  // atomic write if dirty; once per frame
    static std::string Path();

    static constexpr int kVersion = 1;

private:
    FolderStyles() = default;
    bool m_Dirty = false;
};

} // namespace Enhancers
