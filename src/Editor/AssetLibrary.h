#pragma once
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include "Texture.h"       // TextureImportSettings - stored by value below, needs the full definition
#include "Model.h"         // ModelImportSettings - same
#include "MaterialAsset.h" // MaterialAsset - owned by the library

// Tracks every asset imported through the editor so it can be reused (by path)
// and listed in the Asset Browser panel.
class AssetLibrary {
public:
    std::shared_ptr<Model> LoadModel(const std::string& path);
    std::shared_ptr<Texture> LoadTexture(const std::string& path);
    void RegisterSound(const std::string& path); // sounds are played by path via AudioEngine
    // Prefabs are tracked by path only (like sounds) rather than loaded into memory — a prefab
    // file is read fresh on each instantiate, so editing one on disk affects the next instance.
    void RegisterPrefab(const std::string& path);

    // Creates a fresh, independent primitive Model (kind: "cube"/"sphere"/"cylinder"/"cone"/
    // "plane") with its own unique synthetic path, so multiple placed primitives of the same
    // kind never end up sharing one Model instance's material/animation state the way two
    // instances of the same imported file would. Round-trips through scene save/load via
    // LoadModel recognizing the same "primitive://" path scheme.
    std::shared_ptr<Model> CreatePrimitive(const std::string& kind);

    // A fresh, independent Model instance with the same geometry as `original` (re-imports
    // the file, or regenerates the primitive) — NOT the cached shared_ptr LoadModel would
    // return, and not registered in the Asset Browser list. Used for "Duplicate", so the copy
    // gets its own material-override/animation-playback state instead of sharing the
    // original's (which is what happens if two PlacedModels just point at the same Model).
    std::shared_ptr<Model> CloneModel(const std::shared_ptr<Model>& original);

    // The one call every *scene entity* should use to get its Model: registers the asset in
    // the library (so it lists in the Asset Browser and its per-path import settings exist),
    // then returns an independent instance of it — never the cached shared_ptr. Two entities
    // of the same imported file therefore each own their animation time and material override
    // instead of fighting over one shared Model (#106). Equivalent to CloneModel(LoadModel(path)).
    std::shared_ptr<Model> InstantiateModel(const std::string& path);

    const std::vector<std::shared_ptr<Model>>& Models() const { return m_ModelList; }
    const std::vector<std::shared_ptr<Texture>>& Textures() const { return m_TextureList; }
    // Flat path list parallel to Textures() — returned as const ref so AssetRefPathList can
    // return it without copying. Kept in sync by LoadTexture / RemoveTexture.
    const std::vector<std::string>& TexturePaths() const { return m_TexturePaths; }
    const std::vector<std::string>& Sounds() const { return m_Sounds; }
    const std::vector<std::string>& Prefabs() const { return m_Prefabs; }

    // Loads a .mat file and registers it in the material library (no-op if already loaded).
    std::shared_ptr<MaterialAsset> LoadMaterial(const std::string& path);
    // Removes a material from the library without touching the .mat file on disk.
    void RemoveMaterial(const std::shared_ptr<MaterialAsset>& mat);
    // All currently registered materials. Stable order (registration order).
    const std::vector<std::shared_ptr<MaterialAsset>>& Materials() const { return m_MaterialList; }
    // Flat path list parallel to Materials(), for AssetRefPathList.
    const std::vector<std::string>& MaterialPaths() const { return m_MaterialPaths; }

    // Removes an asset from the browser/cache only — safe to call even while placed objects
    // still reference it, since they hold their own shared_ptr (Model/Texture) or plain path
    // string (Sound) that keeps working independently. It just means a future LoadModel/
    // LoadTexture for that same path re-imports fresh rather than reusing this instance, and
    // the entry stops appearing in the Asset Browser.
    void RemoveModel(const std::shared_ptr<Model>& model);
    void RemoveTexture(const std::shared_ptr<Texture>& texture);
    void RemoveSound(const std::string& path);
    void RemovePrefab(const std::string& path);

    // Virtual folders for browsing only (Unity Project-window style) — these never touch
    // real files on disk, they just group asset *references* by an editor-side path like
    // "Props/Guns". An asset with no assignment lives at the root (folder == "").
    void SetAssetFolder(const std::string& assetKey, const std::string& folder);
    std::string AssetFolder(const std::string& assetKey) const;

    // Display name shown in the browser instead of the bare filename — renaming here never
    // touches the real file, it's a per-asset label override (empty clears it, falling back
    // to the filename again).
    void SetDisplayName(const std::string& assetKey, const std::string& name);
    std::string DisplayName(const std::string& assetKey) const; // filename fallback already applied

    // Free-text labels (Unity's Inspector "Labels" field) — independent of folder/display name,
    // searchable via "l:label" in the Asset Browser search box. An asset can carry any number.
    void SetLabels(const std::string& assetKey, const std::set<std::string>& labels);
    const std::set<std::string>& Labels(const std::string& assetKey) const; // empty set if none
    // Every label currently in use by anything, for the search bar's Label filter dropdown.
    // Cached and rebuilt only when a label is added/removed/renamed anywhere in the library —
    // called every frame by the Asset Browser (to decide whether the Filters button's "active"
    // dot should show), so it must not allocate/rebuild a fresh set each call (#177).
    const std::set<std::string>& AllKnownLabels() const;

    // Removes any currently-loaded model/texture/sound/prefab/folder whose path isn't in the
    // corresponding keep-set — used by undo/redo's snapshot restore to make loading a prior
    // state a true replace (an asset created after that point disappears again) without
    // reimporting anything that's staying. Cheap: dropping a cache entry costs nothing, and
    // nothing in the keep-set is touched, so a texture/model that's already loaded and still
    // wanted is never re-decoded/re-imported — only genuinely-missing entries get a real
    // Load*() call afterward, by whoever calls this. Deliberately NOT used by File > Open: that
    // path stays additive (opening a different scene isn't "forget every known asset").
    void PruneToKeepSet(const std::set<std::string>& modelPaths, const std::set<std::string>& texturePaths,
        const std::set<std::string>& soundPaths, const std::set<std::string>& prefabPaths,
        const std::set<std::string>& folderPaths,
        const std::set<std::string>& materialPaths = {});

    // Clears only folder assignments, display names, and import settings — NOT the loaded
    // assets themselves. Paired with PruneToKeepSet and a plain re-application of a snapshot's
    // metadata, this makes metadata restore a true replace too, without touching decoded
    // pixel/mesh data. Cheap regardless of library size (these are just small maps of strings).
    void ClearMetadataOnly();

    void CreateFolder(const std::string& folderPath);
    // Renames (or, since a folder's "path" IS its nesting, re-parents) a folder, cascading the
    // prefix change to every subfolder and asset nested under it.
    void RenameFolder(const std::string& oldPath, const std::string& newPath);
    // A folder can only be deleted once empty (no direct assets, no subfolders) — deliberately
    // refuses otherwise rather than guessing whether to recursively delete real assets or
    // silently un-nest an unknown amount of content.
    bool CanDeleteFolder(const std::string& folderPath) const;
    void DeleteFolder(const std::string& folderPath);
    // Like DeleteFolder, but doesn't require the folder to be empty first — removes every
    // model/texture/sound/prefab filed under it (directly or in a subfolder) and every
    // subfolder itself. Real, irreversible-except-via-undo data loss if misused, which is why
    // the Asset Browser only offers this behind a confirmation dialog.
    void DeleteFolderRecursive(const std::string& folderPath);
    const std::vector<std::string>& Folders() const { return m_Folders; }

    // For SceneSerializer: every asset key currently known to have a folder and/or display
    // name override, so the file format only needs to store what's actually customized.
    const std::map<std::string, std::string>& AssetFolders() const { return m_AssetFolder; }
    const std::map<std::string, std::string>& DisplayNames() const { return m_DisplayNames; }

    // Import settings, keyed by asset path. Reading returns a default-constructed settings
    // struct for a path that's never been customized, so callers never need an existence check.
    TextureImportSettings GetTextureSettings(const std::string& path) const;
    void SetTextureSettings(const std::string& path, const TextureImportSettings& settings);
    ModelImportSettings GetModelSettings(const std::string& path) const;
    void SetModelSettings(const std::string& path, const ModelImportSettings& settings);

    // Re-imports the cached Texture/Model at `path` (if it's actually loaded) using whatever
    // settings are currently stored for it — call SetTexture/ModelSettings first, then this.
    // Returns false if the asset isn't loaded yet or the reimport itself fails.
    bool ReimportTexture(const std::string& path);
    bool ReimportModel(const std::string& path);

    // For SceneSerializer, same pattern as AssetFolders()/DisplayNames() above.
    const std::map<std::string, TextureImportSettings>& TextureSettingsMap() const { return m_TextureSettings; }
    const std::map<std::string, ModelImportSettings>& ModelSettingsMap() const { return m_ModelSettings; }
    const std::map<std::string, std::set<std::string>>& LabelsMap() const { return m_Labels; }

private:
    std::map<std::string, std::shared_ptr<Model>> m_ModelCache;
    std::map<std::string, std::shared_ptr<Texture>> m_TextureCache;
    std::map<std::string, std::shared_ptr<MaterialAsset>> m_MaterialCache;
    std::vector<std::shared_ptr<Model>> m_ModelList;
    std::vector<std::shared_ptr<Texture>> m_TextureList;
    std::vector<std::string> m_TexturePaths;    // parallel to m_TextureList, kept in sync
    std::vector<std::shared_ptr<MaterialAsset>> m_MaterialList;
    std::vector<std::string> m_MaterialPaths;   // parallel to m_MaterialList, kept in sync
    std::vector<std::string> m_Sounds;
    std::vector<std::string> m_Prefabs;

    std::map<std::string, std::string> m_AssetFolder;  // asset key -> folder path ("" = root)
    std::map<std::string, std::string> m_DisplayNames; // asset key -> user-given display name
    std::vector<std::string> m_Folders;                // every known folder path, so empty ones persist too

    std::map<std::string, TextureImportSettings> m_TextureSettings;
    std::map<std::string, ModelImportSettings> m_ModelSettings;
    std::map<std::string, std::set<std::string>> m_Labels;

    // Cache for AllKnownLabels() (#177) — rebuilt lazily on next read after any mutation that
    // could change the set of distinct labels in use (SetLabels, and every Remove*/Clear* that
    // erases from m_Labels). Mutable because the rebuild happens inside a const getter.
    mutable std::set<std::string> m_AllLabelsCache;
    mutable bool m_AllLabelsDirty = true;
};
