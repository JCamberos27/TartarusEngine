#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include "Texture.h"       // TextureImportSettings - stored by value below, needs the full definition
#include "Model.h"         // ModelImportSettings - same
#include "MaterialAsset.h" // MaterialAsset - owned by the library

class ShaderAsset;

// Tracks every asset imported through the editor so it can be reused (by path)
// and listed in the Asset Browser panel.
class AssetLibrary {
public:
    // #121 - seeds the virtual-folder list from project/settings.json. Construct after
    // ProjectSettings::Load().
    AssetLibrary();

    std::shared_ptr<Model> LoadModel(const std::string& path);
    // #369 - what a texture is used for, which decides its default import colour space. Colour
    // (albedo, emissive) is sRGB; Data (metallic, roughness, AO, height, ...) and Normal are raw
    // linear values, and Normal also gets the NormalMap import type. Only a *default*: an
    // explicit "importer" block in the texture's .meta always wins.
    enum class TextureUse { Color, Data, Normal };
    std::shared_ptr<Texture> LoadTexture(const std::string& path, TextureUse use = TextureUse::Color);
    // What a texture's file name says it holds ("_Normal", "_Roughness", ...; see
    // AssetImport::GuessTextureKind), and the import settings that follow from it.
    static TextureUse GuessTextureUse(const std::string& path);
    static TextureImportSettings DefaultTextureSettings(const std::string& path);
    // The settings half of LoadTexture, split out so it can be tested without a GL context:
    // reads the .meta importer block if there is one, else applies the default for `use`.
    // Does nothing if settings for `path` are already known (in-memory settings win).
    void ResolveTextureSettings(const std::string& path, TextureUse use);
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

    // Extract Materials (Unity's): writes one editable .mat per material the model imported,
    // into the asset's Materials/ folder (beside a Models/ folder, else beside the model), with
    // the maps the import found. An existing .mat of that name is kept, never overwritten, so
    // re-extracting (or a second clip of the same pack) reuses the one already edited. The
    // name -> .mat mapping is saved in the model's .meta ("materialRemap") and placed instances
    // of the model start with those materials in their slots. `onlyTextured` skips materials
    // with no texture map (plain colours), for the automatic extraction on import.
    struct MaterialExtraction { int Created = 0, Reused = 0; std::string Folder; };
    MaterialExtraction ExtractModelMaterials(const std::string& modelPath, bool onlyTextured);
    // The model's saved material name -> .mat path (project-relative) mapping; empty if none.
    std::map<std::string, std::string> MaterialRemap(const std::string& modelPath) const;
    // Fills every empty slot of `slots` whose submesh's imported material has a remapped .mat.
    // Returns how many slots it filled.
    int ApplyMaterialRemap(const Model& model, std::vector<std::shared_ptr<MaterialAsset>>& slots);

    // Loads a .shader asset and caches it by path (no-op if already loaded). Returns nullptr
    // on parse failure. Cached indefinitely; hot-reload via ShaderAsset::Variant(key).
    std::shared_ptr<ShaderAsset> LoadShader(const std::string& path);
    // #158 — hot reload: reloads every cached ShaderAsset that depends on one of `changedKeys`
    // (ShaderLibrary::DependencyKey spellings). Returns how many were reloaded.
    int HotReloadShaders(const std::vector<std::string>& changedKeys);
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

    // Clears only folder assignments, display names, labels and import settings - NOT the loaded
    // assets themselves. Paired with PruneToKeepSet and the undo snapshot re-applying its own
    // copy, this makes an undo of an Asset Browser edit a true replace. Only the in-memory undo
    // snapshot carries that copy; scene files do not (#121).
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

    // Writes m_Folders back to project/settings.json (#121).
    void PersistFolders();

    // An asset whose .meta never chose a folder (loaded by a scene or a weapon file rather than
    // imported through the Asset Browser) is filed under its folder on disk, so it doesn't land
    // loose in the root. Reuses a registered folder of the same name in any case, and registers
    // the rest. In memory only; a folder chosen in the browser (even the root) always wins.
    void AdoptDiskFolder(const std::string& path);

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
    // #132 - Unity's "reimport on focus": re-imports every loaded texture whose file changed on
    // disk since it was loaded (edited in Photoshop, replaced in Explorer, pulled from git).
    // main.cpp calls it when the editor window regains focus. Returns how many were reimported.
    int ReimportChangedOnDisk();
    bool ReimportModel(const std::string& path);

    // --- #132: files changed outside the editor (ProjectWatcher -> EditorLayer::SyncProjectChanges)
    // Paths compare by AssetDatabase::PathKey, so any spelling of the same file matches.
    // The library's own spelling of `path` if it lists that file (any asset kind), else "".
    std::string FindListed(const std::string& path) const;
    // Re-keys every list, cache, setting, label and folder entry from `oldPath` to `newPath`
    // and points the loaded Model / Texture / MaterialAsset objects at the new file, so nothing
    // has to reload. Materials whose textures moved get their stored map paths refreshed.
    // Returns whether the library knew the file.
    bool RenamePath(const std::string& oldPath, const std::string& newPath);
    // Drops every entry for `path`, or for anything under it when it was a folder. Loaded
    // objects stay alive in whatever still uses them. Returns how many entries went.
    int ForgetRemoved(const std::string& path);
    // Re-reads a .mat edited outside the editor into the SAME MaterialAsset object, so every
    // renderer using it picks the change up. False if it isn't loaded or can't be read.
    bool ReloadMaterial(const std::string& path);

    // For SceneSerializer, same pattern as AssetFolders()/DisplayNames() above.
    const std::map<std::string, TextureImportSettings>& TextureSettingsMap() const { return m_TextureSettings; }
    const std::map<std::string, ModelImportSettings>& ModelSettingsMap() const { return m_ModelSettings; }
    const std::map<std::string, std::set<std::string>>& LabelsMap() const { return m_Labels; }

private:
    std::map<std::string, std::shared_ptr<Model>> m_ModelCache;
    std::map<std::string, std::shared_ptr<Texture>> m_TextureCache;
    std::map<std::string, std::shared_ptr<MaterialAsset>> m_MaterialCache;
    std::map<std::string, std::shared_ptr<ShaderAsset>> m_ShaderCache;
    std::vector<std::shared_ptr<Model>> m_ModelList;
    std::vector<std::shared_ptr<Texture>> m_TextureList;
    std::vector<std::string> m_TexturePaths;    // parallel to m_TextureList, kept in sync
    std::map<std::string, std::filesystem::file_time_type> m_TextureWriteTime; // #132 - at load/reimport
    std::vector<std::shared_ptr<MaterialAsset>> m_MaterialList;
    std::vector<std::string> m_MaterialPaths;   // parallel to m_MaterialList, kept in sync
    std::vector<std::string> m_Sounds;
    std::vector<std::string> m_Prefabs;

    std::map<std::string, std::string> m_AssetFolder;  // asset key -> folder path ("" = root)
    std::map<std::string, std::string> m_DisplayNames; // asset key -> user-given display name
    std::vector<std::string> m_Folders;                // every known folder path, so empty ones persist too

    std::map<std::string, TextureImportSettings> m_TextureSettings;
    std::map<std::string, ModelImportSettings> m_ModelSettings;
    // Material remaps read from model .meta files, keyed by AssetDatabase::PathKey.
    mutable std::map<std::string, std::map<std::string, std::string>> m_MaterialRemap;
    std::map<std::string, std::set<std::string>> m_Labels;

    // Cache for AllKnownLabels() (#177) — rebuilt lazily on next read after any mutation that
    // could change the set of distinct labels in use (SetLabels, and every Remove*/Clear* that
    // erases from m_Labels). Mutable because the rebuild happens inside a const getter.
    mutable std::set<std::string> m_AllLabelsCache;
    mutable bool m_AllLabelsDirty = true;
};
