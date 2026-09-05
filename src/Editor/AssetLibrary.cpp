#include "AssetLibrary.h"
#include "Model.h"
#include "Texture.h"
#include <algorithm>

namespace {
const std::string kPrimitivePrefix = "primitive://";
}

std::shared_ptr<Model> AssetLibrary::LoadModel(const std::string& path) {
    auto it = m_ModelCache.find(path);
    if (it != m_ModelCache.end()) return it->second;

    std::shared_ptr<Model> model;
    bool isPrimitive = path.rfind(kPrimitivePrefix, 0) == 0;
    if (isPrimitive) {
        // "primitive://<kind>#<id>" — reconstruct on load (e.g. after relaunch) the same way
        // CreatePrimitive builds it fresh; any saved material override is reapplied separately
        // by SceneSerializer right after this returns.
        std::string rest = path.substr(kPrimitivePrefix.size());
        std::string kind = rest.substr(0, rest.find('#'));
        model = Model::CreatePrimitive(kind, path);
    } else {
        // Consult any settings saved for this path (e.g. from a scene's assetMeta, applied
        // before this is called) so an asset with custom import settings is imported once,
        // correctly, instead of once with defaults and once more via Reimport.
        model = std::make_shared<Model>(path, GetModelSettings(path));
    }

    m_ModelCache[path] = model;
    // A primitive:// entry is a per-instance procedural mesh (one per placed Add > Cube / Duplicate),
    // not an importable asset — it's cached above so scene entities resolve their path, but it must
    // NOT appear in the Asset Browser listing or get written into libraryModels. Scene boxes[]/
    // models[] recreate their own primitives from their path on load, independent of that list.
    if (!isPrimitive) m_ModelList.push_back(model);
    return model;
}

std::shared_ptr<Model> AssetLibrary::CreatePrimitive(const std::string& kind) {
    static int counter = 0;
    std::string path = kPrimitivePrefix + kind + "#" + std::to_string(counter++);
    return LoadModel(path); // fresh path -> guaranteed cache miss -> takes the primitive branch above
}

std::shared_ptr<Model> AssetLibrary::CloneModel(const std::shared_ptr<Model>& original) {
    const std::string& path = original->Path();
    if (path.rfind(kPrimitivePrefix, 0) == 0) {
        std::string rest = path.substr(kPrimitivePrefix.size());
        std::string kind = rest.substr(0, rest.find('#'));
        return CreatePrimitive(kind); // already fresh + independent + uniquely pathed
    }
    // Re-import a standalone instance rather than returning the cached one LoadModel would —
    // deliberately NOT added to m_ModelCache/m_ModelList, so it doesn't show up as a second
    // "same file" entry in the Asset Browser and doesn't get reused by a later LoadModel(path).
    // Uses the asset's own import settings so the copy matches the library entry's geometry.
    return std::make_shared<Model>(path, GetModelSettings(path));
}

std::shared_ptr<Model> AssetLibrary::InstantiateModel(const std::string& path) {
    return CloneModel(LoadModel(path));
}

std::shared_ptr<Texture> AssetLibrary::LoadTexture(const std::string& path) {
    auto it = m_TextureCache.find(path);
    if (it != m_TextureCache.end()) return it->second;

    // Consult any settings saved for this path (e.g. from a scene's assetMeta, applied before
    // this is called) so an asset with custom import settings is imported once, correctly,
    // instead of once with defaults and once more via Reimport.
    auto tex = std::make_shared<Texture>(path, GetTextureSettings(path));
    m_TextureCache[path] = tex;
    m_TextureList.push_back(tex);
    return tex;
}

void AssetLibrary::RegisterSound(const std::string& path) {
    if (std::find(m_Sounds.begin(), m_Sounds.end(), path) == m_Sounds.end()) {
        m_Sounds.push_back(path);
    }
}

void AssetLibrary::RemoveModel(const std::shared_ptr<Model>& model) {
    if (!model) return;
    m_ModelCache.erase(model->Path());
    m_ModelList.erase(std::remove(m_ModelList.begin(), m_ModelList.end(), model), m_ModelList.end());
    m_AssetFolder.erase(model->Path());
    m_DisplayNames.erase(model->Path());
    m_ModelSettings.erase(model->Path());
    m_Labels.erase(model->Path());
    m_AllLabelsDirty = true;
}

void AssetLibrary::RemoveTexture(const std::shared_ptr<Texture>& texture) {
    if (!texture) return;
    m_TextureCache.erase(texture->Path());
    m_TextureList.erase(std::remove(m_TextureList.begin(), m_TextureList.end(), texture), m_TextureList.end());
    m_AssetFolder.erase(texture->Path());
    m_DisplayNames.erase(texture->Path());
    m_TextureSettings.erase(texture->Path());
    m_Labels.erase(texture->Path());
    m_AllLabelsDirty = true;
}

void AssetLibrary::RemoveSound(const std::string& path) {
    m_Sounds.erase(std::remove(m_Sounds.begin(), m_Sounds.end(), path), m_Sounds.end());
    m_AssetFolder.erase(path);
    m_DisplayNames.erase(path);
    m_Labels.erase(path);
    m_AllLabelsDirty = true;
}

void AssetLibrary::RegisterPrefab(const std::string& path) {
    if (std::find(m_Prefabs.begin(), m_Prefabs.end(), path) == m_Prefabs.end()) {
        m_Prefabs.push_back(path);
    }
}

void AssetLibrary::RemovePrefab(const std::string& path) {
    m_Prefabs.erase(std::remove(m_Prefabs.begin(), m_Prefabs.end(), path), m_Prefabs.end());
    m_AssetFolder.erase(path);
    m_DisplayNames.erase(path);
    m_Labels.erase(path);
    m_AllLabelsDirty = true;
}

void AssetLibrary::SetAssetFolder(const std::string& assetKey, const std::string& folder) {
    if (folder.empty()) m_AssetFolder.erase(assetKey);
    else m_AssetFolder[assetKey] = folder;
}

std::string AssetLibrary::AssetFolder(const std::string& assetKey) const {
    auto it = m_AssetFolder.find(assetKey);
    return it != m_AssetFolder.end() ? it->second : std::string();
}

void AssetLibrary::SetDisplayName(const std::string& assetKey, const std::string& name) {
    if (name.empty()) m_DisplayNames.erase(assetKey);
    else m_DisplayNames[assetKey] = name;
}

std::string AssetLibrary::DisplayName(const std::string& assetKey) const {
    auto it = m_DisplayNames.find(assetKey);
    if (it != m_DisplayNames.end()) return it->second;
    size_t slash = assetKey.find_last_of("/\\");
    return slash == std::string::npos ? assetKey : assetKey.substr(slash + 1);
}

void AssetLibrary::SetLabels(const std::string& assetKey, const std::set<std::string>& labels) {
    if (labels.empty()) m_Labels.erase(assetKey);
    else m_Labels[assetKey] = labels;
    m_AllLabelsDirty = true;
}

const std::set<std::string>& AssetLibrary::Labels(const std::string& assetKey) const {
    static const std::set<std::string> kEmpty;
    auto it = m_Labels.find(assetKey);
    return it != m_Labels.end() ? it->second : kEmpty;
}

const std::set<std::string>& AssetLibrary::AllKnownLabels() const {
    if (m_AllLabelsDirty) {
        m_AllLabelsCache.clear();
        for (const auto& [key, labels] : m_Labels) m_AllLabelsCache.insert(labels.begin(), labels.end());
        m_AllLabelsDirty = false;
    }
    return m_AllLabelsCache;
}

void AssetLibrary::PruneToKeepSet(const std::set<std::string>& modelPaths, const std::set<std::string>& texturePaths,
    const std::set<std::string>& soundPaths, const std::set<std::string>& prefabPaths,
    const std::set<std::string>& folderPaths) {
    // Copy each list before iterating it - RemoveModel/etc. erase from the very vectors
    // Models()/Textures()/... return, so iterating those directly while removing from them
    // would be iterating a container out from under itself.
    for (const auto& m : std::vector<std::shared_ptr<Model>>(m_ModelList)) {
        if (!modelPaths.count(m->Path())) RemoveModel(m);
    }
    for (const auto& t : std::vector<std::shared_ptr<Texture>>(m_TextureList)) {
        if (!texturePaths.count(t->Path())) RemoveTexture(t);
    }
    for (const auto& s : std::vector<std::string>(m_Sounds)) {
        if (!soundPaths.count(s)) RemoveSound(s);
    }
    for (const auto& p : std::vector<std::string>(m_Prefabs)) {
        if (!prefabPaths.count(p)) RemovePrefab(p);
    }
    for (const auto& f : std::vector<std::string>(m_Folders)) {
        // A folder that still has something under it isn't in the keep-set by mistake - it
        // just hasn't had its contents pruned yet on this pass (RemoveModel/etc. above already
        // ran, so by this point anything real that shouldn't survive is already gone). Silently
        // skipping instead of forcing it is the safe fallback if that assumption is ever wrong.
        if (!folderPaths.count(f) && CanDeleteFolder(f)) DeleteFolder(f);
    }
}

void AssetLibrary::ClearMetadataOnly() {
    m_AssetFolder.clear();
    m_DisplayNames.clear();
    m_TextureSettings.clear();
    m_ModelSettings.clear();
    m_Labels.clear();
    m_AllLabelsDirty = true;
}

void AssetLibrary::CreateFolder(const std::string& folderPath) {
    if (std::find(m_Folders.begin(), m_Folders.end(), folderPath) == m_Folders.end()) {
        m_Folders.push_back(folderPath);
    }
}

void AssetLibrary::RenameFolder(const std::string& oldPath, const std::string& newPath) {
    if (oldPath == newPath) return;
    auto remap = [&](std::string& path) {
        if (path == oldPath) path = newPath;
        else if (path.rfind(oldPath + "/", 0) == 0) path = newPath + path.substr(oldPath.size());
    };
    for (auto& f : m_Folders) remap(f);
    for (auto& [assetKey, folder] : m_AssetFolder) remap(folder);
}

bool AssetLibrary::CanDeleteFolder(const std::string& folderPath) const {
    for (const auto& [assetKey, folder] : m_AssetFolder) {
        if (folder == folderPath) return false;
    }
    for (const auto& f : m_Folders) {
        if (f != folderPath && f.rfind(folderPath + "/", 0) == 0) return false;
    }
    return true;
}

void AssetLibrary::DeleteFolder(const std::string& folderPath) {
    if (!CanDeleteFolder(folderPath)) return;
    m_Folders.erase(std::remove(m_Folders.begin(), m_Folders.end(), folderPath), m_Folders.end());
}

void AssetLibrary::DeleteFolderRecursive(const std::string& folderPath) {
    auto isUnderFolder = [&](const std::string& assetPath) {
        std::string f = AssetFolder(assetPath);
        return f == folderPath || f.rfind(folderPath + "/", 0) == 0;
    };

    // Copy each list before removing from it - RemoveModel/etc. erase from the very vectors
    // Models()/Textures()/... return.
    for (const auto& m : std::vector<std::shared_ptr<Model>>(m_ModelList)) {
        if (isUnderFolder(m->Path())) RemoveModel(m);
    }
    for (const auto& t : std::vector<std::shared_ptr<Texture>>(m_TextureList)) {
        if (isUnderFolder(t->Path())) RemoveTexture(t);
    }
    for (const auto& s : std::vector<std::string>(m_Sounds)) {
        if (isUnderFolder(s)) RemoveSound(s);
    }
    for (const auto& p : std::vector<std::string>(m_Prefabs)) {
        if (isUnderFolder(p)) RemovePrefab(p);
    }

    // Every asset that belonged under this folder is gone now, so every subfolder (and the
    // folder itself) is empty - just drop them from the folder list directly rather than going
    // through CanDeleteFolder/DeleteFolder's "must be empty" guard one at a time.
    m_Folders.erase(std::remove_if(m_Folders.begin(), m_Folders.end(), [&](const std::string& f) {
        return f == folderPath || f.rfind(folderPath + "/", 0) == 0;
    }), m_Folders.end());
}

TextureImportSettings AssetLibrary::GetTextureSettings(const std::string& path) const {
    auto it = m_TextureSettings.find(path);
    return it != m_TextureSettings.end() ? it->second : TextureImportSettings{};
}

void AssetLibrary::SetTextureSettings(const std::string& path, const TextureImportSettings& settings) {
    m_TextureSettings[path] = settings;
}

ModelImportSettings AssetLibrary::GetModelSettings(const std::string& path) const {
    auto it = m_ModelSettings.find(path);
    return it != m_ModelSettings.end() ? it->second : ModelImportSettings{};
}

void AssetLibrary::SetModelSettings(const std::string& path, const ModelImportSettings& settings) {
    m_ModelSettings[path] = settings;
}

bool AssetLibrary::ReimportTexture(const std::string& path) {
    auto it = m_TextureCache.find(path);
    if (it == m_TextureCache.end()) return false;
    return it->second->Reimport(GetTextureSettings(path));
}

bool AssetLibrary::ReimportModel(const std::string& path) {
    auto it = m_ModelCache.find(path);
    if (it == m_ModelCache.end()) return false;
    return it->second->Reimport(GetModelSettings(path));
}
