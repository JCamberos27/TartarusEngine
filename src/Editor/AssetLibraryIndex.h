#pragma once
// project/asset_library.json (docs/PROJECT_ASSETS.md "Asset Library"): the Asset Browser's
// "Library" root sorts placeable assets into categories by ordered glob rules, without moving a
// file, and the "kits" table gives each modular pack its grid size for ModularSnap.

#include <string>
#include <utility>
#include <vector>

class AssetLibraryIndex {
public:
    struct Category {
        std::string Id, Name, Parent; // Parent empty for a top-level category
    };

    // Reads <project>/asset_library.json; an absent or broken file leaves the index empty.
    void Load();
    bool Empty() const { return m_Categories.empty(); }

    const std::vector<Category>& Categories() const { return m_Categories; }
    // The category id of a project-relative path ("assets/..."): first matching rule, else "other".
    const std::string& CategoryOf(const std::string& projectRelPath) const;
    // The module size (metres) of the kit `projectRelPath` belongs to, or 0 when it isn't a kit piece.
    float KitModuleOf(const std::string& projectRelPath) const;

    // Same as above from parsed text, for tests.
    void LoadFromText(const std::string& text);

    // The form the rules and kits are written in: project-relative, forward slashes ("assets/...").
    static std::string ProjectRelative(const std::string& path);

private:
    std::vector<Category> m_Categories;
    std::vector<std::pair<std::string, std::string>> m_Rules; // glob, category id
    std::vector<std::pair<std::string, float>> m_Kits;        // folder prefix, module
};

// The browser's "Asset Library" root: category folders that exist only in the browser (never in
// AssetLibrary's persisted folder list), so nothing can be created in, moved into or renamed there.
namespace AssetLibraryView {
inline const std::string kRoot = "Asset Library";
inline bool IsLibraryFolder(const std::string& folder) {
    return folder == kRoot || folder.rfind(kRoot + "/", 0) == 0;
}
std::vector<std::string>& Folders(); // filled by EditorLayer::RefreshLibraryView, read by the folder tree
} // namespace AssetLibraryView
