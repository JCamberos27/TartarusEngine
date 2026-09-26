#include "AssetLibrary.h"
#include "MaterialAsset.h"
#include "ShaderAsset.h"
#include "Model.h"
#include "Texture.h"
#include "AssetDatabase.h"
#include "AssetImport.h"
#include "Log.h"
#include "ProjectSettings.h" // #121 - virtual folders are project data, not scene data
#include "ProjectPaths.h"
#include <algorithm>
#include <map>
#include <set>
#include <json.hpp>

using json = nlohmann::json;

namespace {
const std::string kPrimitivePrefix = "primitive://";

// Reads import settings from a .meta JSON object into `s`. Returns true if the "importer"
// key was present (even if individual sub-keys were absent and defaulted).
bool ParseTextureImporter(const json& j, TextureImportSettings& s) {
    if (!j.contains("importer") || !j["importer"].is_object()) return false;
    const auto& imp = j["importer"];
    s.TextureType    = (TextureImportSettings::Type)imp.value("textureType",   (int)TextureImportSettings::Type::Default);
    s.GenerateMipmaps= imp.value("generateMipmaps", true);
    s.IsSRGB         = imp.value("isSRGB",           true);
    s.FilterMode     = (TextureImportSettings::Filter)imp.value("filterMode",  (int)TextureImportSettings::Filter::Bilinear);
    s.WrapMode       = (TextureImportSettings::Wrap)imp.value("wrapMode",      (int)TextureImportSettings::Wrap::Repeat);
    s.MaxTextureSize = imp.value("maxTextureSize",   2048);
    s.AnisoLevel     = imp.value("anisoLevel",       8);
    s.CompressionMode= (TextureImportSettings::Compression)std::clamp(imp.value("compression", 0), 0, 2); // #156
    return true;
}

bool ParseModelImporter(const json& j, ModelImportSettings& s) {
    if (!j.contains("importer") || !j["importer"].is_object()) return false;
    const auto& imp = j["importer"];
    s.GlobalScale        = imp.value("globalScale",       1.0f);
    s.ImportNormals      = imp.value("importNormals",     true);
    s.ImportAnimations   = imp.value("importAnimations",  true);
    s.ImportSkeleton     = imp.value("importSkeleton",    true);
    s.OptimizeGraph      = imp.value("optimizeGraph",     true);
    s.MaterialImportMode = (ModelImportSettings::MaterialMode)imp.value("materialImportMode",
                               (int)ModelImportSettings::MaterialMode::ImportEmbedded);
    return true;
}

// Reads folder/displayName/labels from a .meta JSON object into the AssetLibrary maps (direct
// map access — callers must not invoke Set* here to avoid recursive write-through).
void ApplyMetaBrowserData(const json& j, const std::string& path,
    std::map<std::string, std::string>& folders,
    std::map<std::string, std::string>& displayNames,
    std::map<std::string, std::set<std::string>>& labels,
    bool& labelsDirty)
{
    if (j.contains("folder") && j["folder"].is_string()) {
        std::string f = j["folder"].get<std::string>();
        if (!f.empty() && folders.find(path) == folders.end()) folders[path] = f;
    }
    if (j.contains("displayName") && j["displayName"].is_string()) {
        std::string n = j["displayName"].get<std::string>();
        if (!n.empty() && displayNames.find(path) == displayNames.end()) displayNames[path] = n;
    }
    if (j.contains("labels") && j["labels"].is_array() && labels.find(path) == labels.end()) {
        std::set<std::string> ls;
        for (auto& el : j["labels"]) if (el.is_string()) ls.insert(el.get<std::string>());
        if (!ls.empty()) { labels[path] = std::move(ls); labelsDirty = true; }
    }
}

} // namespace

// --- #132: path matching shared by the loaders and the project-sync entry points ----------------
namespace {
bool SamePath(const std::string& a, const std::string& b) {
    return !a.empty() && !b.empty() && AssetDatabase::PathKey(a) == AssetDatabase::PathKey(b);
}
bool UnderPath(const std::string& path, const std::string& dir) {
    const std::string p = AssetDatabase::PathKey(path), d = AssetDatabase::PathKey(dir);
    return p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/';
}
template <typename Map>
void RekeyMap(Map& m, const std::string& oldPath, const std::string& newPath) {
    for (auto it = m.begin(); it != m.end(); ++it) {
        if (!SamePath(it->first, oldPath)) continue;
        auto value = std::move(it->second);
        m.erase(it);
        m[newPath] = std::move(value);
        return;
    }
}
void RenameInList(std::vector<std::string>& v, const std::string& oldPath, const std::string& newPath) {
    for (std::string& s : v) if (SamePath(s, oldPath)) s = newPath;
}
} // namespace

// The entry of `cache` for `path` under any spelling of it (slashes, case), or end().
template <typename Map>
typename Map::iterator FindAnySpelling(Map& cache, const std::string& path) {
    auto it = cache.find(path);
    if (it != cache.end()) return it;
    for (it = cache.begin(); it != cache.end(); ++it) if (SamePath(it->first, path)) return it;
    return cache.end();
}

std::shared_ptr<Model> AssetLibrary::LoadModel(const std::string& path) {
    if (path.rfind(kPrimitivePrefix, 0) == 0) {
        // "primitive://<kind>#<id>" — a per-instance procedural mesh, reconstructed from its
        // path; any saved material override is reapplied separately by SceneSerializer right
        // after this returns. #97 — NOT cached: every scene load / undo / redo / Play-Stop used
        // to add a fresh primitive Model (VAO/VBO) to m_ModelCache that nothing ever removed,
        // so GPU + CPU memory grew for the whole editing session. The entity that owns the
        // returned shared_ptr is its only owner now; destroying the entity frees the mesh.
        std::string rest = path.substr(kPrimitivePrefix.size());
        std::string kind = rest.substr(0, rest.find('#'));
        return Model::CreatePrimitive(kind, path);
    }

    // #132 - any spelling of the same file ("a/b.fbx", "a\b.fbx", different case) is one entry;
    // a second spelling used to import the file a second time.
    auto it = FindAnySpelling(m_ModelCache, path);
    if (it != m_ModelCache.end()) return it->second;

    std::shared_ptr<Model> model;
    {
        // Populate settings and browser metadata from .meta before constructing the model, so
        // that the first import uses the persisted settings rather than requiring a Reimport.
        if (m_ModelSettings.find(path) == m_ModelSettings.end()) {
            std::string metaStr = AssetDatabase::ReadMetaFields(path);
            if (metaStr != "{}") {
                try {
                    json j = json::parse(metaStr);
                    ModelImportSettings s;
                    if (ParseModelImporter(j, s)) m_ModelSettings[path] = s;
                    ApplyMetaBrowserData(j, path, m_AssetFolder, m_DisplayNames, m_Labels, m_AllLabelsDirty);
                } catch (...) {}
            }
        }
        AdoptDiskFolder(path);
        model = std::make_shared<Model>(path, GetModelSettings(path));
    }

    m_ModelCache[path] = model;
    m_ModelList.push_back(model);
    AssetDatabase::EnsureGuid(path);
    return model;
}

std::shared_ptr<Model> AssetLibrary::CreatePrimitive(const std::string& kind) {
    // The "#N" suffix only makes each instance's path distinct (the path is also what the scene
    // file stores); primitives aren't cached (#97), so a restart reusing an old N is harmless.
    static int counter = 0;
    std::string path = kPrimitivePrefix + kind + "#" + std::to_string(counter++);
    return Model::CreatePrimitive(kind, path);
}

std::shared_ptr<Model> AssetLibrary::CloneModel(const std::shared_ptr<Model>& original) {
    const std::string& path = original->Path();
    if (path.rfind(kPrimitivePrefix, 0) == 0) {
        std::string rest = path.substr(kPrimitivePrefix.size());
        std::string kind = rest.substr(0, rest.find('#'));
        return CreatePrimitive(kind); // already fresh + independent + uniquely pathed
    }
    // #96 — a lightweight instance sharing the library entry's imported data (meshes, GPU
    // buffers, textures, skeleton, clips) with its own animation state. Used to be a full Assimp
    // re-import per placed object, on every load / undo / Play-Stop / duplicate. Not added to
    // m_ModelCache/m_ModelList, so the Asset Browser still lists the file once; a Reimport of
    // the library entry reaches every instance because they share its data.
    return original->CreateInstance();
}

std::shared_ptr<Model> AssetLibrary::InstantiateModel(const std::string& path) {
    if (path.rfind(kPrimitivePrefix, 0) == 0) return LoadModel(path); // already a fresh instance (#97)
    return CloneModel(LoadModel(path));
}

AssetLibrary::TextureUse AssetLibrary::GuessTextureUse(const std::string& path) {
    switch (AssetImport::GuessTextureKind(path)) {
        case AssetImport::TextureKind::Normal: return TextureUse::Normal;
        case AssetImport::TextureKind::Data:   return TextureUse::Data;
        default:                               return TextureUse::Color;
    }
}

TextureImportSettings AssetLibrary::DefaultTextureSettings(const std::string& path) {
    TextureImportSettings s;
    const TextureUse use = GuessTextureUse(path);
    if (use != TextureUse::Color) s.IsSRGB = false;
    if (use == TextureUse::Normal) s.TextureType = TextureImportSettings::Type::NormalMap;
    return s;
}

void AssetLibrary::ResolveTextureSettings(const std::string& path, TextureUse use) {
    // Populate settings and browser metadata from .meta before constructing the texture, so
    // that the first import uses the persisted settings rather than requiring a Reimport.
    // If settings were already set in-memory (e.g. from a scene's assetMeta block applied
    // before this call), skip the .meta read — in-memory wins for the current session.
    if (m_TextureSettings.find(path) == m_TextureSettings.end()) {
        std::string metaStr = AssetDatabase::ReadMetaFields(path);
        if (metaStr != "{}") {
            try {
                json j = json::parse(metaStr);
                TextureImportSettings s;
                if (ParseTextureImporter(j, s)) m_TextureSettings[path] = s;
                ApplyMetaBrowserData(j, path, m_AssetFolder, m_DisplayNames, m_Labels, m_AllLabelsDirty);
            } catch (...) {}
        }
    }

    // #369 - nothing chose settings for a texture a material uses as a data map, so default it to
    // linear (and to the NormalMap type for a normal map) instead of the sRGB colour default.
    // Gamma-decoding a normal map bends every normal - lighting on a rolling ball shifted with
    // its orientation. Kept in memory only: loading a material must not write .meta files. The
    // Inspector does the same for a map assigned by hand, but a .mat only names its textures.
    // Nothing chose settings and nothing says what it's used for: go by the file name, so
    // "Crate_Normal.png" dragged in on its own isn't gamma-decoded as colour either.
    if (use == TextureUse::Color && m_TextureSettings.find(path) == m_TextureSettings.end())
        use = GuessTextureUse(path);
    if (use != TextureUse::Color && m_TextureSettings.find(path) == m_TextureSettings.end()) {
        TextureImportSettings s;
        s.IsSRGB = false;
        if (use == TextureUse::Normal) s.TextureType = TextureImportSettings::Type::NormalMap;
        m_TextureSettings[path] = s;
    }
}

std::shared_ptr<Texture> AssetLibrary::LoadTexture(const std::string& path, TextureUse use) {
    auto it = FindAnySpelling(m_TextureCache, path); // #132 - see LoadModel
    if (it != m_TextureCache.end()) return it->second;

    ResolveTextureSettings(path, use);
    AdoptDiskFolder(path);

    // Register the GUID BEFORE constructing the Texture, not after (audit #75). TextureCache's
    // on-disk entry is keyed by AssetDatabase::GuidForPath when one is registered, falling back
    // to a hash of the absolute path otherwise (TextureCache.cpp's EntryPath) — the Texture
    // constructor below is what actually reads/writes that entry. Calling EnsureGuid after
    // construction meant the very first decode of any not-yet-scanned texture wrote its cache
    // entry under the path-hash key (no GUID existed yet), while every later load of that same
    // path found a GUID already registered and looked the entry up under a DIFFERENT key — a
    // guaranteed cache miss on every load after the first. In normal project use this was masked
    // by AssetDatabase::ScanProject() registering every known asset's GUID at startup, before any
    // texture is ever loaded — but anything loaded from outside that scan (freshly imported
    // files, or --asset-load-bench's synthetic textures) hit it on every single load.
    AssetDatabase::EnsureGuid(path);
    auto tex = std::make_shared<Texture>(path, GetTextureSettings(path));
    m_TextureCache[path] = tex;
    m_TextureList.push_back(tex);
    m_TexturePaths.push_back(path);
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    if (!ec) m_TextureWriteTime[path] = written;
    return tex;
}

void AssetLibrary::RegisterSound(const std::string& path) {
    if (std::none_of(m_Sounds.begin(), m_Sounds.end(), [&](const std::string& s) { return SamePath(s, path); })) { // #132
        m_Sounds.push_back(path);
        AssetDatabase::EnsureGuid(path);
        std::string metaStr = AssetDatabase::ReadMetaFields(path);
        if (metaStr != "{}") {
            try {
                json j = json::parse(metaStr);
                ApplyMetaBrowserData(j, path, m_AssetFolder, m_DisplayNames, m_Labels, m_AllLabelsDirty);
            } catch (...) {}
        }
        AdoptDiskFolder(path);
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
    m_TextureWriteTime.erase(texture->Path());
    m_TextureList.erase(std::remove(m_TextureList.begin(), m_TextureList.end(), texture), m_TextureList.end());
    m_TexturePaths.erase(std::remove(m_TexturePaths.begin(), m_TexturePaths.end(), texture->Path()), m_TexturePaths.end());
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

std::shared_ptr<MaterialAsset> AssetLibrary::LoadMaterial(const std::string& path) {
    auto it = FindAnySpelling(m_MaterialCache, path); // #132 - see LoadModel
    if (it != m_MaterialCache.end()) return it->second;

    auto ma = MaterialAsset::Load(path, this);
    if (!ma) return nullptr;

    m_MaterialCache[path] = ma;
    m_MaterialList.push_back(ma);
    m_MaterialPaths.push_back(path);
    AssetDatabase::EnsureGuid(path);
    std::string metaStr = AssetDatabase::ReadMetaFields(path);
    if (metaStr != "{}") {
        try {
            json j = json::parse(metaStr);
            ApplyMetaBrowserData(j, path, m_AssetFolder, m_DisplayNames, m_Labels, m_AllLabelsDirty);
        } catch (...) {}
    }
    AdoptDiskFolder(path);
    return ma;
}

std::shared_ptr<ShaderAsset> AssetLibrary::LoadShader(const std::string& path) {
    auto it = m_ShaderCache.find(path);
    if (it != m_ShaderCache.end()) return it->second;
    auto sa = ShaderAsset::ParseFile(path);
    if (!sa) return nullptr;
    m_ShaderCache[path] = sa;
    return sa;
}

int AssetLibrary::HotReloadShaders(const std::vector<std::string>& changedKeys) {
    int reloaded = 0;
    for (auto& [path, sa] : m_ShaderCache)
        if (sa && sa->DependsOnAny(changedKeys) && sa->ReloadFromDisk()) ++reloaded;
    return reloaded;
}

void AssetLibrary::RemoveMaterial(const std::shared_ptr<MaterialAsset>& mat) {
    if (!mat) return;
    m_MaterialCache.erase(mat->Path);
    m_MaterialList.erase(std::remove(m_MaterialList.begin(), m_MaterialList.end(), mat), m_MaterialList.end());
    m_MaterialPaths.erase(std::remove(m_MaterialPaths.begin(), m_MaterialPaths.end(), mat->Path), m_MaterialPaths.end());
    m_AssetFolder.erase(mat->Path);
    m_DisplayNames.erase(mat->Path);
    m_Labels.erase(mat->Path);
    m_AllLabelsDirty = true;
}

void AssetLibrary::RegisterPrefab(const std::string& path) {
    if (std::none_of(m_Prefabs.begin(), m_Prefabs.end(), [&](const std::string& s) { return SamePath(s, path); })) { // #132
        m_Prefabs.push_back(path);
        AssetDatabase::EnsureGuid(path);
        std::string metaStr = AssetDatabase::ReadMetaFields(path);
        if (metaStr != "{}") {
            try {
                json j = json::parse(metaStr);
                ApplyMetaBrowserData(j, path, m_AssetFolder, m_DisplayNames, m_Labels, m_AllLabelsDirty);
            } catch (...) {}
        }
        AdoptDiskFolder(path);
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
    AssetDatabase::MergeMetaFields(assetKey, json{{"folder", folder}}.dump());
}

std::string AssetLibrary::AssetFolder(const std::string& assetKey) const {
    auto it = m_AssetFolder.find(assetKey);
    return it != m_AssetFolder.end() ? it->second : std::string();
}

void AssetLibrary::SetDisplayName(const std::string& assetKey, const std::string& name) {
    if (name.empty()) m_DisplayNames.erase(assetKey);
    else m_DisplayNames[assetKey] = name;
    AssetDatabase::MergeMetaFields(assetKey, json{{"displayName", name}}.dump());
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
    json labelsArr = json::array();
    for (const auto& l : labels) labelsArr.push_back(l);
    AssetDatabase::MergeMetaFields(assetKey, json{{"labels", labelsArr}}.dump());
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
    const std::set<std::string>& folderPaths, const std::set<std::string>& materialPaths) {
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
    if (!materialPaths.empty()) {
        for (const auto& m : std::vector<std::shared_ptr<MaterialAsset>>(m_MaterialList)) {
            if (!materialPaths.count(m->Path)) RemoveMaterial(m);
        }
    }
    for (const auto& f : std::vector<std::string>(m_Folders)) {
        // A folder that still has something under it isn't in the keep-set by mistake - it
        // just hasn't had its contents pruned yet on this pass (RemoveModel/etc. above already
        // ran, so by this point anything real that shouldn't survive is already gone). Silently
        // skipping instead of forcing it is the safe fallback if that assumption is ever wrong.
        if (!folderPaths.count(f) && CanDeleteFolder(f)) DeleteFolder(f);
    }
}

// #121 - the folder list lives in project/settings.json. AssetLibrary keeps a working copy
// (the Asset Browser reads it every frame) and writes it back on every change.
AssetLibrary::AssetLibrary() : m_Folders(ProjectSettings::AssetFolders()) {}

void AssetLibrary::PersistFolders() {
    ProjectSettings::SetAssetFolders(m_Folders);
    ProjectSettings::Save();
}

void AssetLibrary::ClearMetadataOnly() {
    m_AssetFolder.clear();
    m_DisplayNames.clear();
    m_TextureSettings.clear();
    m_ModelSettings.clear();
    m_Labels.clear();
    m_AllLabelsDirty = true;
}

void AssetLibrary::AdoptDiskFolder(const std::string& path) {
    if (path.empty() || path.find("://") != std::string::npos || m_AssetFolder.count(path)) return;
    std::string dir = ProjectPaths::Relativize(path);
    std::replace(dir.begin(), dir.end(), '\\', '/');
    if (dir.empty() || dir.find(':') != std::string::npos || dir[0] == '/') return; // outside the project
    const size_t slash = dir.find_last_of('/');
    if (slash == std::string::npos) return;                          // already at the root
    dir.resize(slash);
    try {
        const json j = json::parse(AssetDatabase::ReadMetaFields(path));
        if (j.contains("folder")) return;                            // chosen in the browser
    } catch (...) {}

    auto sameName = [](const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
    };
    // Walk the directories top-down, reusing "Shaders" for "shaders" and so on.
    std::string folder;
    bool added = false;
    for (size_t start = 0; start <= dir.size();) {
        size_t end = dir.find('/', start);
        if (end == std::string::npos) end = dir.size();
        const std::string wanted = (folder.empty() ? "" : folder + "/") + dir.substr(start, end - start);
        auto it = std::find_if(m_Folders.begin(), m_Folders.end(),
                               [&](const std::string& f) { return sameName(f, wanted); });
        if (it != m_Folders.end()) folder = *it;
        else { folder = wanted; m_Folders.push_back(folder); added = true; }
        start = end + 1;
    }
    // Not persisted: these folders are derived from where the files sit and are found again on every load
    // (saving them rewrote the tracked project/settings.json each time an asset was first seen). Folders
    // the user makes or renames in the browser still persist (CreateFolder / RenameFolder).
    (void)added;
    m_AssetFolder[path] = folder;
}

void AssetLibrary::CreateFolder(const std::string& folderPath) {
    if (std::find(m_Folders.begin(), m_Folders.end(), folderPath) == m_Folders.end()) {
        m_Folders.push_back(folderPath);
        PersistFolders();
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
    PersistFolders();
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
    PersistFolders();
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
    for (const auto& m : std::vector<std::shared_ptr<MaterialAsset>>(m_MaterialList)) {
        if (isUnderFolder(m->Path)) RemoveMaterial(m);
    }

    // Every asset that belonged under this folder is gone now, so every subfolder (and the
    // folder itself) is empty - just drop them from the folder list directly rather than going
    // through CanDeleteFolder/DeleteFolder's "must be empty" guard one at a time.
    m_Folders.erase(std::remove_if(m_Folders.begin(), m_Folders.end(), [&](const std::string& f) {
        return f == folderPath || f.rfind(folderPath + "/", 0) == 0;
    }), m_Folders.end());
    PersistFolders();
}

TextureImportSettings AssetLibrary::GetTextureSettings(const std::string& path) const {
    auto it = m_TextureSettings.find(path);
    return it != m_TextureSettings.end() ? it->second : TextureImportSettings{};
}

void AssetLibrary::SetTextureSettings(const std::string& path, const TextureImportSettings& settings) {
    m_TextureSettings[path] = settings;
    json imp;
    imp["textureType"]    = (int)settings.TextureType;
    imp["generateMipmaps"]= settings.GenerateMipmaps;
    imp["isSRGB"]         = settings.IsSRGB;
    imp["filterMode"]     = (int)settings.FilterMode;
    imp["wrapMode"]       = (int)settings.WrapMode;
    imp["maxTextureSize"] = settings.MaxTextureSize;
    imp["anisoLevel"]     = settings.AnisoLevel;
    imp["compression"]    = (int)settings.CompressionMode;
    AssetDatabase::MergeMetaFields(path, json{{"importer", imp}}.dump());
}

ModelImportSettings AssetLibrary::GetModelSettings(const std::string& path) const {
    auto it = m_ModelSettings.find(path);
    return it != m_ModelSettings.end() ? it->second : ModelImportSettings{};
}

void AssetLibrary::SetModelSettings(const std::string& path, const ModelImportSettings& settings) {
    m_ModelSettings[path] = settings;
    json imp;
    imp["globalScale"]       = settings.GlobalScale;
    imp["importNormals"]     = settings.ImportNormals;
    imp["importAnimations"]  = settings.ImportAnimations;
    imp["importSkeleton"]    = settings.ImportSkeleton;
    imp["optimizeGraph"]     = settings.OptimizeGraph;
    imp["materialImportMode"]= (int)settings.MaterialImportMode;
    AssetDatabase::MergeMetaFields(path, json{{"importer", imp}}.dump());
}

bool AssetLibrary::ReimportTexture(const std::string& path) {
    auto it = m_TextureCache.find(path);
    if (it == m_TextureCache.end()) return false;
    // #132 - remember the file time this import saw, so reimport-on-focus doesn't redo it.
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    if (!ec) m_TextureWriteTime[path] = written;
    return it->second->Reimport(GetTextureSettings(path));
}

int AssetLibrary::ReimportChangedOnDisk() {
    int count = 0;
    for (auto& [path, tex] : m_TextureCache) {
        std::error_code ec;
        const auto written = std::filesystem::last_write_time(path, ec);
        if (ec) continue; // missing or embedded (#113 in-memory textures have no file)
        auto known = m_TextureWriteTime.find(path);
        if (known != m_TextureWriteTime.end() && known->second == written) continue;
        const bool first = known == m_TextureWriteTime.end();
        m_TextureWriteTime[path] = written;
        if (first) continue; // no baseline yet (loaded before the file existed); record only
        // TextureCache is keyed on the file's mtime too, so this decodes the new pixels.
        if (tex->Reimport(GetTextureSettings(path))) ++count;
    }
    return count;
}

// --- #132: keeping the library in step with the project folder ---------------------------------


std::string AssetLibrary::FindListed(const std::string& path) const {
    for (const auto& [k, v] : m_ModelCache) if (SamePath(k, path)) return k;
    for (const auto& [k, v] : m_TextureCache) if (SamePath(k, path)) return k;
    for (const auto& [k, v] : m_MaterialCache) if (SamePath(k, path)) return k;
    for (const std::string& s : m_Sounds) if (SamePath(s, path)) return s;
    for (const std::string& s : m_Prefabs) if (SamePath(s, path)) return s;
    return {};
}

bool AssetLibrary::RenamePath(const std::string& oldPath, const std::string& newPath) {
    const std::string listed = FindListed(oldPath);
    for (auto& [k, model] : m_ModelCache) if (SamePath(k, oldPath) && model) model->SetPath(newPath);
    bool textureMoved = false;
    for (auto& [k, tex] : m_TextureCache)
        if (SamePath(k, oldPath) && tex) { tex->SetPath(newPath); textureMoved = true; }
    for (auto& [k, mat] : m_MaterialCache) if (SamePath(k, oldPath) && mat) mat->Path = newPath;

    RekeyMap(m_ModelCache, oldPath, newPath);
    RekeyMap(m_TextureCache, oldPath, newPath);
    RekeyMap(m_TextureWriteTime, oldPath, newPath);
    RekeyMap(m_MaterialCache, oldPath, newPath);
    RekeyMap(m_ShaderCache, oldPath, newPath);
    RekeyMap(m_AssetFolder, oldPath, newPath);
    RekeyMap(m_DisplayNames, oldPath, newPath);
    RekeyMap(m_TextureSettings, oldPath, newPath);
    RekeyMap(m_ModelSettings, oldPath, newPath);
    RekeyMap(m_Labels, oldPath, newPath);
    RenameInList(m_TexturePaths, oldPath, newPath);
    RenameInList(m_MaterialPaths, oldPath, newPath);
    RenameInList(m_Sounds, oldPath, newPath);
    RenameInList(m_Prefabs, oldPath, newPath);
    // A material's stored map paths come from its Texture objects, which now carry the new path.
    if (textureMoved)
        for (auto& mat : m_MaterialList) if (mat) mat->SyncTexturePathsFromMat();
    m_AllLabelsDirty = true;
    return !listed.empty();
}

int AssetLibrary::ForgetRemoved(const std::string& path) {
    auto gone = [&](const std::string& p) { return SamePath(p, path) || UnderPath(p, path); };
    int count = 0;
    std::vector<std::shared_ptr<Model>> models;
    for (auto& [k, m] : m_ModelCache) if (gone(k)) models.push_back(m);
    for (auto& m : models) { RemoveModel(m); ++count; }
    std::vector<std::shared_ptr<Texture>> textures;
    for (auto& [k, t] : m_TextureCache) if (gone(k)) textures.push_back(t);
    for (auto& t : textures) { RemoveTexture(t); ++count; }
    std::vector<std::shared_ptr<MaterialAsset>> mats;
    for (auto& [k, m] : m_MaterialCache) if (gone(k)) mats.push_back(m);
    for (auto& m : mats) { RemoveMaterial(m); ++count; }
    for (const std::string& s : std::vector<std::string>(m_Sounds)) if (gone(s)) { RemoveSound(s); ++count; }
    for (const std::string& s : std::vector<std::string>(m_Prefabs)) if (gone(s)) { RemovePrefab(s); ++count; }
    for (auto it = m_ShaderCache.begin(); it != m_ShaderCache.end();) it = gone(it->first) ? m_ShaderCache.erase(it) : std::next(it);
    return count;
}

bool AssetLibrary::ReloadMaterial(const std::string& path) {
    for (auto& [k, mat] : m_MaterialCache) {
        if (!SamePath(k, path) || !mat) continue;
        auto fresh = MaterialAsset::Load(k, this);
        if (!fresh || fresh->Missing) return false;
        *mat = *fresh; // same object: every renderer slot holding it sees the new values
        return true;
    }
    return false;
}

bool AssetLibrary::ReimportModel(const std::string& path) {
    auto it = m_ModelCache.find(path);
    if (it == m_ModelCache.end()) return false;
    return it->second->Reimport(GetModelSettings(path));
}

namespace {
// "Door Mat:1/2" -> "Door_Mat_1_2": a material name made safe for a file name.
std::string MaterialFileName(const std::string& name, int index) {
    std::string out;
    for (char c : name) out += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "Material_" + std::to_string(index);
    return out;
}
} // namespace

AssetLibrary::MaterialExtraction AssetLibrary::ExtractModelMaterials(const std::string& modelPath, bool onlyTextured) {
    namespace fs = std::filesystem;
    MaterialExtraction result;
    auto model = LoadModel(modelPath);
    if (!model || model->MeshCount() == 0) return result;

    const fs::path abs = fs::path(ProjectPaths::Resolve(ProjectPaths::Relativize(modelPath))).lexically_normal();
    const fs::path dir = abs.parent_path();
    std::string dirName = dir.filename().string();
    for (char& c : dirName) c = (char)std::tolower((unsigned char)c);
    const bool modelsFolder = dirName == "models" || dirName == "model" || dirName == "meshes" ||
                              dirName == "mesh" || dirName == "fbx" || dirName == "obj";
    const fs::path folder = (modelsFolder ? dir.parent_path() : dir) / "Materials";
    result.Folder = folder.string();

    std::map<std::string, std::string> remap = MaterialRemap(modelPath);
    std::set<std::string> done;
    for (int i = 0; i < model->MeshCount(); ++i) {
        const Material& src = model->MeshMaterial(i);
        if (!done.insert(src.Name).second) continue;
        const bool textured = src.AlbedoMap || src.NormalMap || src.MetallicRoughnessMap || src.MetallicMap ||
                              src.RoughnessMap || src.AOMap || src.EmissiveMap;
        if (onlyTextured && !textured) continue;

        const fs::path matPath = folder / (MaterialFileName(src.Name, i) + ".mat");
        std::error_code ec;
        if (fs::exists(matPath, ec)) {
            ++result.Reused;
        } else {
            fs::create_directories(folder, ec);
            auto ma = MaterialAsset::CreateDefault(matPath.string());
            if (!ma) {
                Log::Error("Extract Materials: couldn't write '" + matPath.string() + "'.");
                continue;
            }
            ma->Name = src.Name.empty() ? matPath.stem().string() : src.Name;
            ma->Mat = src;
            if (src.AlphaClip) ma->RenderQueue = MaterialAsset::Queue::AlphaTest;
            ma->SyncTexturePathsFromMat();
            if (!ma->Save()) {
                Log::Error("Extract Materials: couldn't write '" + matPath.string() + "'.");
                continue;
            }
            ++result.Created;
        }
        LoadMaterial(matPath.string());
        remap[src.Name] = ProjectPaths::Relativize(matPath.string());
    }

    if (!remap.empty()) {
        AssetDatabase::EnsureGuid(modelPath);
        AssetDatabase::MergeMetaFields(modelPath, json{{"materialRemap", remap}}.dump());
        m_MaterialRemap[AssetDatabase::PathKey(modelPath)] = remap;
    }
    return result;
}

std::map<std::string, std::string> AssetLibrary::MaterialRemap(const std::string& modelPath) const {
    const std::string key = AssetDatabase::PathKey(modelPath);
    auto it = m_MaterialRemap.find(key);
    if (it != m_MaterialRemap.end()) return it->second;
    std::map<std::string, std::string> remap;
    try {
        const json j = json::parse(AssetDatabase::ReadMetaFields(modelPath));
        if (j.contains("materialRemap") && j["materialRemap"].is_object())
            for (const auto& [name, v] : j["materialRemap"].items())
                if (v.is_string() && !v.get<std::string>().empty()) remap[name] = v.get<std::string>();
    } catch (...) {}
    m_MaterialRemap[key] = remap;
    return remap;
}

int AssetLibrary::ApplyMaterialRemap(const Model& model, std::vector<std::shared_ptr<MaterialAsset>>& slots) {
    const auto remap = MaterialRemap(model.Path());
    if (remap.empty()) return 0;
    int filled = 0;
    for (int i = 0; i < model.MeshCount(); ++i) {
        if (i < (int)slots.size() && slots[i]) continue;
        auto it = remap.find(model.MeshMaterial(i).Name);
        if (it == remap.end()) continue;
        auto mat = LoadMaterial(ProjectPaths::Resolve(it->second));
        if (!mat || mat->Missing) continue;
        if (i >= (int)slots.size()) slots.resize(i + 1);
        slots[i] = mat;
        ++filled;
    }
    return filled;
}
