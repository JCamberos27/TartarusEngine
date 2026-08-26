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
    if (path.rfind(kPrimitivePrefix, 0) == 0) {
        // "primitive://<kind>#<id>" — reconstruct on load (e.g. after relaunch) the same way
        // CreatePrimitive builds it fresh; any saved material override is reapplied separately
        // by SceneSerializer right after this returns.
        std::string rest = path.substr(kPrimitivePrefix.size());
        std::string kind = rest.substr(0, rest.find('#'));
        model = Model::CreatePrimitive(kind, path);
    } else {
        model = std::make_shared<Model>(path);
    }

    m_ModelCache[path] = model;
    m_ModelList.push_back(model);
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
    return std::make_shared<Model>(path);
}

std::shared_ptr<Texture> AssetLibrary::LoadTexture(const std::string& path) {
    auto it = m_TextureCache.find(path);
    if (it != m_TextureCache.end()) return it->second;

    auto tex = std::make_shared<Texture>(path);
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
}

void AssetLibrary::RemoveTexture(const std::shared_ptr<Texture>& texture) {
    if (!texture) return;
    m_TextureCache.erase(texture->Path());
    m_TextureList.erase(std::remove(m_TextureList.begin(), m_TextureList.end(), texture), m_TextureList.end());
    m_AssetFolder.erase(texture->Path());
    m_DisplayNames.erase(texture->Path());
}

void AssetLibrary::RemoveSound(const std::string& path) {
    m_Sounds.erase(std::remove(m_Sounds.begin(), m_Sounds.end(), path), m_Sounds.end());
    m_AssetFolder.erase(path);
    m_DisplayNames.erase(path);
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
