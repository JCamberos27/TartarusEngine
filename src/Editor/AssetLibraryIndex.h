#pragma once
// project/asset_library.json (docs/PROJECT_ASSETS.md "Asset Library"): the Asset Browser's
// "Library" root sorts placeable assets into categories by ordered glob rules, without moving a
// file, and the "kits" table gives each modular pack its grid size for ModularSnap.

#include <set>
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
    // project/packages.json: vendor pack folders, read-only in the editor (AssetLibraryView::InPackage).
    void LoadPackages(const std::string& text);
    const std::vector<std::string>& Packages() const { return m_Packages; }

    // The form the rules and kits are written in: project-relative, forward slashes ("assets/...").
    static std::string ProjectRelative(const std::string& path);
    // The asset label a category gives its assets (searched as l:<label>): its name with only letters
    // and digits ("Bushes & Plants" -> "BushesPlants"); "" for "other".
    std::string LabelOf(const std::string& categoryId) const;
    // Every label an asset of `categoryId` carries: its own and its parent's (a wall is l:Walls and
    // l:Architecture, so the parent's saved search finds its children's assets too).
    std::set<std::string> LabelsOf(const std::string& categoryId) const;

private:
    std::vector<Category> m_Categories;
    std::vector<std::pair<std::string, std::string>> m_Rules; // glob, category id
    std::vector<std::pair<std::string, float>> m_Kits;        // folder prefix, module
    std::vector<std::string> m_Packages;                      // read-only vendor pack folders
};

// The browser's Favorites root (Unity's saved searches): one search per asset_library.json category
// (l:<label>) plus the user's own starred searches. Its folders exist only in the browser (never in
// AssetLibrary's persisted folder list or on disk), so nothing can be created in, moved into or renamed there.
namespace AssetLibraryView {
inline const std::string kRoot = "Favorites";
inline bool IsLibraryFolder(const std::string& folder) {
    return folder == kRoot || folder.rfind(kRoot + "/", 0) == 0;
}
std::vector<std::string>& Folders(); // filled by EditorLayer::RefreshLibraryView, read by the folder tree
// Vendor packs (project/packages.json) are read-only in the editor, like Unity's Packages.
std::vector<std::string>& Packages();
// True when `path` (absolute or project-relative, file or folder) is inside a package (or is one).
bool InPackage(const std::string& path);
// The package folder `path` is in, "" when none.
std::string PackageOf(const std::string& path);
} // namespace AssetLibraryView
