#pragma once
#include <string>
#include <vector>
#include <memory>
#include <map>

class Model;
class Texture;

// Tracks every asset imported through the editor so it can be reused (by path)
// and listed in the Asset Browser panel.
class AssetLibrary {
public:
    std::shared_ptr<Model> LoadModel(const std::string& path);
    std::shared_ptr<Texture> LoadTexture(const std::string& path);
    void RegisterSound(const std::string& path); // sounds are played by path via AudioEngine

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

    const std::vector<std::shared_ptr<Model>>& Models() const { return m_ModelList; }
    const std::vector<std::shared_ptr<Texture>>& Textures() const { return m_TextureList; }
    const std::vector<std::string>& Sounds() const { return m_Sounds; }

    // Removes an asset from the browser/cache only — safe to call even while placed objects
    // still reference it, since they hold their own shared_ptr (Model/Texture) or plain path
    // string (Sound) that keeps working independently. It just means a future LoadModel/
    // LoadTexture for that same path re-imports fresh rather than reusing this instance, and
    // the entry stops appearing in the Asset Browser.
    void RemoveModel(const std::shared_ptr<Model>& model);
    void RemoveTexture(const std::shared_ptr<Texture>& texture);
    void RemoveSound(const std::string& path);

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

    void CreateFolder(const std::string& folderPath);
    // Renames (or, since a folder's "path" IS its nesting, re-parents) a folder, cascading the
    // prefix change to every subfolder and asset nested under it.
    void RenameFolder(const std::string& oldPath, const std::string& newPath);
    // A folder can only be deleted once empty (no direct assets, no subfolders) — deliberately
    // refuses otherwise rather than guessing whether to recursively delete real assets or
    // silently un-nest an unknown amount of content.
    bool CanDeleteFolder(const std::string& folderPath) const;
    void DeleteFolder(const std::string& folderPath);
    const std::vector<std::string>& Folders() const { return m_Folders; }

    // For SceneSerializer: every asset key currently known to have a folder and/or display
    // name override, so the file format only needs to store what's actually customized.
    const std::map<std::string, std::string>& AssetFolders() const { return m_AssetFolder; }
    const std::map<std::string, std::string>& DisplayNames() const { return m_DisplayNames; }

private:
    std::map<std::string, std::shared_ptr<Model>> m_ModelCache;
    std::map<std::string, std::shared_ptr<Texture>> m_TextureCache;
    std::vector<std::shared_ptr<Model>> m_ModelList;
    std::vector<std::shared_ptr<Texture>> m_TextureList;
    std::vector<std::string> m_Sounds;

    std::map<std::string, std::string> m_AssetFolder;  // asset key -> folder path ("" = root)
    std::map<std::string, std::string> m_DisplayNames; // asset key -> user-given display name
    std::vector<std::string> m_Folders;                // every known folder path, so empty ones persist too
};
