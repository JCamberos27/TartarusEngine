// Asset Browser: the folder tree, the asset grid and its search/filter parser, model
// thumbnails, and asset delete/rename/duplicate. Split out of EditorLayer.cpp (#179).

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "FileDialog.h"
#include "AssetLibrary.h"
#include "MaterialAsset.h"
#include "World.h"
#include "Camera.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
#include "AudioEngine.h"
#include "Screenshot.h"
#include "SceneSerializer.h"
#include "AABB.h"
#include "Log.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "AssetImporterInspector.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "AtomicFile.h"
#include "GLStateCache.h"
#include "Framebuffer.h"
#include "gl.h"

#include <imgui.h>
#include <imgui_internal.h> // ImMax/ImFloor, ImGuiWindow, and the item-flag helpers the panels use
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <IconsFontAwesome6.h>

#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp> // extractEulerAngleYXZ - must match ComposeTransform's order (#108)

#include <filesystem>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <sstream>
#include <fstream>
#include <json.hpp>
#include <cmath>
#include <cctype>
#include <cstring>
#include <functional>
#include <cfloat>

using namespace EditorInternal;

// The grid's per-frame tile list (EditorLayer::m_AssetGridCells). `Cell` kept as a short local
// alias so the ported per-cell code below reads unchanged.
using Cell = AssetGridCell;


namespace {

// Reverse lookup for the Asset Browser: which placed objects reference a given asset.
// Compared by raw pointer (Model/Texture) since two placed objects can share one instance.
// Level-geometry entities excluded — their cube Model is private/unshared, never an "asset".
std::vector<std::string> FindModelUsages(const World& world, const Model* model) {
    std::vector<std::string> names;
    auto view = world.Registry.view<const NameComponent, const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    for (auto entity : view) {
        const auto& [name, renderable] = view.get<const NameComponent, const RenderableComponent>(entity);
        if (renderable.ModelRef.get() == model) names.push_back(name.Name.empty() ? "(unnamed)" : name.Name);
    }
    return names;
}

bool MaterialUsesTexture(const Material& mat, const Texture* tex) {
    return mat.AlbedoMap.get() == tex || mat.NormalMap.get() == tex || mat.MetallicRoughnessMap.get() == tex ||
           mat.MetallicMap.get() == tex || mat.RoughnessMap.get() == tex || mat.AOMap.get() == tex ||
           mat.EmissiveMap.get() == tex;
}

std::vector<std::string> FindTextureUsages(const World& world, const Texture* tex) {
    std::vector<std::string> names;
    auto view = world.Registry.view<const NameComponent, const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    for (auto entity : view) {
        const auto& [name, renderable] = view.get<const NameComponent, const RenderableComponent>(entity);
        bool used = false;
        if (!renderable.Materials.empty() && renderable.Materials[0]) {
            used = MaterialUsesTexture(renderable.Materials[0]->Mat, tex);
        } else {
            for (int i = 0; i < renderable.ModelRef->MeshCount(); ++i) {
                if (MaterialUsesTexture(renderable.ModelRef->MeshMaterial(i), tex)) { used = true; break; }
            }
        }
        if (used) names.push_back(name.Name.empty() ? "(unnamed)" : name.Name);
    }
    return names;
}

std::vector<std::string> FindSoundUsages(const World& world, const std::string& path) {
    std::vector<std::string> names;
    auto view = world.Registry.view<const NameComponent, const AudioSourceComponent>();
    for (auto entity : view) {
        const auto& [name, audio] = view.get<const NameComponent, const AudioSourceComponent>(entity);
        if (audio.SoundPath == path) names.push_back(name.Name.empty() ? "(unnamed)" : name.Name);
    }
    return names;
}

std::string UsageTooltip(const std::vector<std::string>& users) {
    if (users.empty()) return "Not currently used by anything in the scene.";
    std::string s = "Used by:\n";
    for (size_t i = 0; i < users.size() && i < 10; ++i) s += "  - " + users[i] + "\n";
    if (users.size() > 10) s += "  ...and " + std::to_string(users.size() - 10) + " more\n";
    s.pop_back(); // drop the trailing newline
    return s;
}

// Unity's Project-window search syntax: plain words match the asset name and are ANDed
// together ("coastal scene" needs both words); "t:Model" restricts by asset type, and several
// t: terms are ORed ("t:Model t:Texture" means either kind); "l:label" restricts by label, and
// several l: terms are ANDed (must carry every listed label). Typed directly or built by the
// Type/Label filter dropdown buttons - both just edit this same text.
struct ParsedAssetSearch {
    std::vector<std::string> nameTerms;
    std::vector<std::string> typeTerms;  // lowercased kind names: "model","texture","sound","scene","prefab","folder"
    std::vector<std::string> labelTerms; // lowercased
};

ParsedAssetSearch ParseAssetSearch(const std::string& filter) {
    ParsedAssetSearch result;
    std::istringstream iss(filter);
    std::string token;
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    while (iss >> token) {
        if (token.size() > 2 && (token[0] == 't' || token[0] == 'T') && token[1] == ':') {
            result.typeTerms.push_back(toLower(token.substr(2)));
        } else if (token.size() > 2 && (token[0] == 'l' || token[0] == 'L') && token[1] == ':') {
            result.labelTerms.push_back(toLower(token.substr(2)));
        } else {
            result.nameTerms.push_back(token);
        }
    }
    return result;
}

// `kind` is the asset's type name (already lowercased: "model", "texture", ...), `labels` its
// current label set. Name terms AND, type terms OR (any one is enough), label terms AND (every
// one must be present) - matching the semantics Unity documents for t:/l:.
bool MatchesAssetSearch(const ParsedAssetSearch& parsed, const std::string& name, const std::string& kind,
    const std::set<std::string>& labels) {
    for (const auto& term : parsed.nameTerms) {
        if (!MatchesFilter(term, name)) return false;
    }
    if (!parsed.typeTerms.empty()) {
        bool anyTypeMatches = false;
        for (const auto& t : parsed.typeTerms) {
            if (t == kind) { anyTypeMatches = true; break; }
        }
        if (!anyTypeMatches) return false;
    }
    for (const auto& term : parsed.labelTerms) {
        bool found = false;
        for (const auto& lbl : labels) {
            if (MatchesFilter(term, lbl) && term.size() == lbl.size()) { found = true; break; } // exact, case-insensitive
        }
        if (!found) return false;
    }
    return true;
}

// Adds `token` (e.g. "t:Model") to `filter`'s text if it's not already there, or removes it if
// it is - how the Type/Label filter dropdown checkboxes edit the plain search text, since
// that's the one source of truth Unity's own filters work the same way against.
void ToggleSearchToken(std::string& filter, const std::string& token) {
    std::istringstream iss(filter);
    std::vector<std::string> tokens;
    std::string t;
    bool removed = false;
    while (iss >> t) {
        if (t == token) { removed = true; continue; }
        tokens.push_back(t);
    }
    if (!removed) tokens.push_back(token);
    filter.clear();
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) filter += " ";
        filter += tokens[i];
    }
}

bool SearchHasToken(const std::string& filter, const std::string& token) {
    std::istringstream iss(filter);
    std::string t;
    while (iss >> t) {
        if (t == token) return true;
    }
    return false;
}

} // namespace


// --- Asset favourites (#236 G) — project/asset_favorites.json --------------------------------
void EditorLayer::LoadAssetFavorites() {
    m_AssetFavorites.clear();
    std::ifstream in(ProjectPaths::Resolve("asset_favorites.json"));
    if (!in.is_open()) return;
    try {
        nlohmann::json root; in >> root;
        if (root.is_array())
            for (const auto& v : root) if (v.is_string()) m_AssetFavorites.insert(v.get<std::string>());
    } catch (const std::exception& e) {
        Log::Warn(std::string("Asset favourites: failed to parse: ") + e.what());
    }
}

void EditorLayer::SaveAssetFavorites() const {
    nlohmann::json root = nlohmann::json::array();
    for (const auto& k : m_AssetFavorites) root.push_back(k);
    // Atomic: a crash mid-write must not truncate the favourites list (audit CPP-206).
    AtomicFile::WriteJson(ProjectPaths::Resolve("asset_favorites.json"), root);
}

void EditorLayer::ToggleAssetFavorite(const std::string& key) {
    if (key.empty()) return;
    if (!m_AssetFavorites.insert(key).second) m_AssetFavorites.erase(key);
    SaveAssetFavorites();
}

void EditorLayer::SetAssetFavorites(const std::vector<std::string>& keys, bool on) {
    bool changed = false;
    for (const std::string& k : keys) {
        if (k.empty()) continue;
        changed |= on ? m_AssetFavorites.insert(k).second : (m_AssetFavorites.erase(k) > 0);
    }
    if (changed) SaveAssetFavorites();
}

// Decoded once per sound path (#236 G). An empty vector means "not decodable / not audio" and
// is cached too, so a bad file isn't re-probed every frame.
const std::vector<float>& EditorLayer::SoundWaveform(const std::string& path) {
    auto it = m_SoundWaveforms.find(path);
    if (it == m_SoundWaveforms.end()) {
        std::vector<float> peaks;
        AudioEngine::WaveformPeaks(path, 48, peaks);
        it = m_SoundWaveforms.emplace(path, std::move(peaks)).first;
    }
    return it->second;
}

// Rendered once per Model into its own small texture, then cached. Returns 0 while this frame's
// render budget is spent — the caller falls back to the type glyph and picks it up next frame.
unsigned int EditorLayer::ModelThumbnail(Model& model) {
    const std::string& path = model.Path();
    auto it = m_ModelThumbnails.find(path);
    if (it != m_ModelThumbnails.end()) {
        // Touch: move to the front (most-recently-used end) of the LRU list.
        m_ThumbnailLRU.erase(it->second.second);
        m_ThumbnailLRU.push_front(path);
        it->second.second = m_ThumbnailLRU.begin();
        return it->second.first;
    }
    if (m_ThumbnailBudgetThisFrame <= 0) return 0;
    m_ThumbnailBudgetThisFrame--;

    const int kSize = 128;
    const float dist = ModelPreviewRenderer::ComputeFramingDistance(model);
    const unsigned int src = m_ThumbnailPreview.Render(model, 0.7f, 0.5f, dist, kSize, kSize);

    unsigned int dst = 0;
    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Copy the shared preview render into `dst` via a scratch read-FBO. Predates the 4.6
    // upgrade (4.3+ has glCopyImageSubData) but glCopyTexSubImage2D from a bound READ
    // framebuffer works fine and is already loaded, so it's kept as-is.
    GLint prevRead = 0, prevDraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevDraw);
    if (!m_ThumbnailBlitFbo) glGenFramebuffers(1, &m_ThumbnailBlitFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ThumbnailBlitFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    glBindTexture(GL_TEXTURE_2D, dst);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, kSize, kSize);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned int)prevDraw);

    // The preview render plus the raw glBindTexture calls above never went through GLStateCache;
    // resync it so a later Texture::Bind() on the same unit isn't skipped as falsely-redundant
    // (audit GL-202: the "call Invalidate() after every raw-GL pass" rule this path was missing).
    GLStateCache::Invalidate();

    m_ThumbnailLRU.push_front(path);
    m_ModelThumbnails[path] = { dst, m_ThumbnailLRU.begin() };

    // LRU eviction: bounded above kMaxModelThumbnails so browsing a large asset library over a
    // session doesn't accumulate GL textures forever. The least-recently-used entry is the back
    // of the list.
    if (m_ModelThumbnails.size() > kMaxModelThumbnails) {
        const std::string evictPath = m_ThumbnailLRU.back();
        m_ThumbnailLRU.pop_back();
        auto evictIt = m_ModelThumbnails.find(evictPath);
        if (evictIt != m_ModelThumbnails.end()) {
            if (evictIt->second.first) glDeleteTextures(1, &evictIt->second.first);
            m_ModelThumbnails.erase(evictIt);
        }
    }
    return dst;
}

void EditorLayer::InvalidateModelThumbnail(const std::string& path) {
    if (path.empty()) { // clear all — a reimport can rebuild any model in place
        for (auto& [p, entry] : m_ModelThumbnails) { (void)p; if (entry.first) glDeleteTextures(1, &entry.first); }
        m_ModelThumbnails.clear();
        m_ThumbnailLRU.clear();
        return;
    }
    auto it = m_ModelThumbnails.find(path);
    if (it == m_ModelThumbnails.end()) return;
    if (it->second.first) glDeleteTextures(1, &it->second.first);
    m_ThumbnailLRU.erase(it->second.second);
    m_ModelThumbnails.erase(it);
}
// Small, cheap load for the Asset Browser grid thumbnail (~96px on screen).
static std::shared_ptr<Texture> LoadScreenshotThumbTexture(const std::string& path) {
    return LoadScreenshotTexture(path, 384);
}
void EditorLayer::BeginRenameAsset(const std::string& key, bool isFolder, const std::string& currentName) {
    m_RenamingAssetKey = key;
    m_RenamingIsFolder = isFolder;
    m_RenamingJustStarted = true;
    snprintf(m_RenameBuffer, sizeof(m_RenameBuffer), "%s", currentName.c_str());
    m_SelectedAssetKey = key;
    m_SelectedAssetIsFolder = isFolder;
}

bool EditorLayer::IsAssetSelected(const std::string& key, bool isFolder) const {
    if (m_SelectedAssetKey == key && m_SelectedAssetIsFolder == isFolder) return true;
    for (const auto& e : m_ExtraAssetSelection) {
        if (e.Key == key && e.IsFolder == isFolder) return true;
    }
    return false;
}

void EditorLayer::ClearAssetSelection() {
    m_SelectedAssetKey.clear();
    m_ExtraAssetSelection.clear();
}

void EditorLayer::ToggleAssetSelection(const std::string& key, bool isFolder) {
    if (m_SelectedAssetKey.empty()) {
        m_SelectedAssetKey = key;
        m_SelectedAssetIsFolder = isFolder;
        return;
    }
    if (m_SelectedAssetKey == key && m_SelectedAssetIsFolder == isFolder) {
        // Toggling off the primary — promote an extra selection to take its place, if any.
        if (!m_ExtraAssetSelection.empty()) {
            m_SelectedAssetKey = m_ExtraAssetSelection.back().Key;
            m_SelectedAssetIsFolder = m_ExtraAssetSelection.back().IsFolder;
            m_ExtraAssetSelection.pop_back();
        } else {
            m_SelectedAssetKey.clear();
        }
        return;
    }
    for (auto it = m_ExtraAssetSelection.begin(); it != m_ExtraAssetSelection.end(); ++it) {
        if (it->Key == key && it->IsFolder == isFolder) {
            m_ExtraAssetSelection.erase(it); // already co-selected — toggle it back off
            return;
        }
    }
    m_ExtraAssetSelection.push_back({key, isFolder});
}

void EditorLayer::RequestDeleteAsset(World& world, AssetLibrary& assets, const std::string& key, bool isFolder, bool skipDialog) {
    if (key.empty()) return;
    RequestDeleteAssets(world, assets, { AssetKeyRef{key, isFolder} }, skipDialog);
}

void EditorLayer::RequestDeleteAssets(World& world, AssetLibrary& assets, const std::vector<AssetKeyRef>& items, bool skipDialog) {
    if (items.empty()) return;
    if (skipDialog) {
        m_DeleteError.clear();
        PushUndo(world, items.size() == 1
            ? (items[0].IsFolder ? "Delete Folder" : "Delete Asset")
            : "Delete Assets");
        for (const auto& item : items) PerformAssetDelete(world, assets, item.Key, item.IsFolder);
        ClearAssetSelection();
        if (!m_DeleteError.empty()) m_OpenDeleteErrorRequested = true;
        return;
    }
    m_PendingDelete = items;
    m_OpenDeleteConfirmRequested = true;
}

bool EditorLayer::PerformAssetDelete(World& world, AssetLibrary& assets, const std::string& key, bool isFolder) {
    (void)world; // undo is now pushed once by the caller, covering the whole batch (#212)
    InvalidateModelThumbnail(); // a freed Model could be reallocated at the same address
    const std::string leaf = std::filesystem::path(key).filename().string();
    if (isFolder) {
        assets.DeleteFolderRecursive(key);
        // Don't leave the browser pointed at a folder that no longer exists.
        if (m_CurrentAssetFolder == key || m_CurrentAssetFolder.rfind(key + "/", 0) == 0) {
            m_CurrentAssetFolder = ParentFolderOf(key);
        }
    } else {
        for (const auto& model : assets.Models()) {
            if (model->Path() == key) { assets.RemoveModel(model); break; }
        }
        for (const auto& tex : assets.Textures()) {
            if (tex->Path() == key) { assets.RemoveTexture(tex); break; }
        }
        for (const auto& sound : assets.Sounds()) {
            if (sound == key) { assets.RemoveSound(sound); break; }
        }
        for (const auto& prefab : assets.Prefabs()) {
            if (prefab == key) { assets.RemovePrefab(prefab); break; }
        }
        for (const auto& mat : assets.Materials()) {
            if (mat->Path == key) { assets.RemoveMaterial(mat); break; }
        }
        // A Scene entry is a real .json file, not an AssetLibrary asset — remove it from disk
        // directly, and report why if that fails.
        std::error_code ec;
        const bool isScene = std::filesystem::path(key).extension() == ".json" &&
                             std::filesystem::exists(key, ec);
        if (isScene) {
            // Accumulate reasons so a batch delete reports every item that couldn't be removed.
            auto fail = [&](const std::string& msg) {
                if (!m_DeleteError.empty()) m_DeleteError += "\n";
                m_DeleteError += "\xE2\x80\xA2 " + msg; // "• "
            };
            if (key == m_CurrentScenePath) {
                fail("\"" + leaf + "\" is the scene you have open.");
                return false;
            }
            std::string why;
            if (FileDialog::RecycleFile(key, why)) {
                Log::Info("Sent scene file to Recycle Bin: " + leaf);
                InvalidateScenesListing(); // (#175) scenes/ just lost a file
                if (EditorSettings::Get().LastScenePath == key) {
                    EditorSettings::Get().LastScenePath.clear();
                    EditorSettings::Save();
                }
            } else {
                if (why.empty()) why = "the file may be open in another program or write-protected";
                fail("\"" + leaf + "\" — " + why + ".");
                Log::Error("Couldn't recycle scene file '" + key + "': " + why);
                return false;
            }
        }

        // A screenshot is also a plain file on disk (project/screenshots/).
        std::string ext = std::filesystem::path(key).extension().string();
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if ((ext == ".png" || ext == ".jpg" || ext == ".jpeg") && std::filesystem::exists(key, ec)) {
            m_ShotThumbs.erase(key);
            std::string why;
            if (FileDialog::RecycleFile(key, why)) {
                Log::Info("Sent screenshot to Recycle Bin: " + leaf);
                InvalidateShotsListing(); // (#175) screenshots/ just lost a file
            } else {
                if (why.empty()) why = "the file may be open in another program or write-protected";
                if (!m_DeleteError.empty()) m_DeleteError += "\n";
                m_DeleteError += "\xE2\x80\xA2 \"" + leaf + "\" — " + why + ".";
                return false;
            }
        }
    }
    if (m_SelectedAssetKey == key) m_SelectedAssetKey.clear();
    return true;
}

void EditorLayer::DrawDeleteConfirmPopup(World& world, AssetLibrary& assets) {
    const char* kPopupId = "Delete Asset?";
    if (m_OpenDeleteConfirmRequested) {
        ImGui::OpenPopup(kPopupId);
        m_OpenDeleteConfirmRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(340.0f * m_UIScale, 0.0f));
    if (ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f * m_UIScale);
        if (m_PendingDelete.size() == 1) {
            ImGui::Text("Delete \"%s\"?", LeafNameOf(m_PendingDelete[0].Key).c_str());
        } else {
            ImGui::Text("Delete %d selected items?", (int)m_PendingDelete.size());
        }
        bool anyNonEmptyFolder = false;
        for (const auto& item : m_PendingDelete) {
            if (item.IsFolder && !assets.CanDeleteFolder(item.Key)) { anyNonEmptyFolder = true; break; }
        }
        if (anyNonEmptyFolder) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                "One or more of these folders isn't empty - everything inside, including subfolders, will be removed too.");
        }
        // Scenes and screenshots are real files on disk, not AssetLibrary entries — deleting them
        // erases the file (sent to the Recycle Bin, not the "Undoable" history the line below
        // promises for library assets). Say so explicitly rather than a blanket "Undoable" (#211).
        size_t realFileCount = 0;
        for (const auto& item : m_PendingDelete) {
            if (item.IsFolder) continue;
            std::string ext = std::filesystem::path(item.Key).extension().string();
            for (char& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".json" || ext == ".png" || ext == ".jpg" || ext == ".jpeg") ++realFileCount;
        }
        if (realFileCount == m_PendingDelete.size()) {
            ImGui::TextDisabled(realFileCount == 1
                ? "This is a real file on disk, not a library asset - it will be sent to the Recycle Bin."
                : "These are real files on disk, not library assets - they will be sent to the Recycle Bin.");
        } else {
            ImGui::TextDisabled("Objects already placed in the scene keep working - this only removes it from the Asset Browser. Undoable.");
            if (realFileCount == 1) {
                ImGui::TextDisabled("1 of these is a real file on disk and will be sent to the Recycle Bin instead.");
            } else if (realFileCount > 1) {
                ImGui::TextDisabled("%zu of these are real files on disk and will be sent to the Recycle Bin instead.", realFileCount);
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        // Enter or Delete confirms, Esc cancels — the prompt can be cleared without the mouse.
        const bool keyConfirm = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                                ImGui::IsKeyPressed(ImGuiKey_Delete);
        const bool keyCancel  = ImGui::IsKeyPressed(ImGuiKey_Escape);

        float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (PrimaryButton("Cancel", ImVec2(buttonWidth, 0.0f)) || keyCancel) {
            m_PendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Delete", ImVec2(buttonWidth, 0.0f)) || keyConfirm) {
            m_DeleteError.clear();
            PushUndo(world, m_PendingDelete.size() == 1
                ? (m_PendingDelete[0].IsFolder ? "Delete Folder" : "Delete Asset")
                : "Delete Assets");
            for (const auto& item : m_PendingDelete) PerformAssetDelete(world, assets, item.Key, item.IsFolder);
            m_PendingDelete.clear();
            ClearAssetSelection();
            ImGui::CloseCurrentPopup();
            if (!m_DeleteError.empty()) m_OpenDeleteErrorRequested = true;
        }
        ImGui::EndPopup();
    }

    // Follow-up modal: something couldn't be deleted — explain why, OK to dismiss.
    const char* kErrPopupId = "Can't Delete";
    if (m_OpenDeleteErrorRequested) {
        ImGui::OpenPopup(kErrPopupId);
        m_OpenDeleteErrorRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(340.0f * m_UIScale, 0.0f));
    if (ImGui::BeginPopupModal(kErrPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f * m_UIScale);
        const bool several = m_DeleteError.find('\n') != std::string::npos;
        ImGui::TextUnformatted(several ? "Some items couldn't be deleted:" : "That item couldn't be deleted:");
        ImGui::Spacing();
        ImGui::TextUnformatted(m_DeleteError.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const bool dismiss = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                             ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (PrimaryButton("OK", ImVec2(-FLT_MIN, 0.0f)) || dismiss) {
            m_DeleteError.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::DuplicateSelectedAsset(World& world, AssetLibrary& assets) {
    std::vector<AssetKeyRef> targets;
    if (!m_SelectedAssetKey.empty() && !m_SelectedAssetIsFolder) targets.push_back({m_SelectedAssetKey, false});
    for (const auto& e : m_ExtraAssetSelection) {
        if (!e.IsFolder) targets.push_back(e); // folders aren't duplicable — silently skipped
    }
    if (targets.empty()) return;

    // Duplicates one asset's file on disk to a numbered sibling and registers it the same way
    // importing it fresh would. Returns the new path, or empty if the file's gone, the copy
    // failed, or it's not an AssetLibrary-tracked kind (e.g. a Scene — file copied, nothing
    // further to register, so not worth reselecting).
    auto duplicateOne = [&](const std::string& key) -> std::string {
        std::filesystem::path srcPath(key);
        std::error_code existsErr;
        if (!std::filesystem::exists(srcPath, existsErr)) {
            Log::Error("Can't duplicate '" + key + "' - the file no longer exists on disk.");
            return {};
        }

        std::string stem = srcPath.stem().string();
        std::string ext = srcPath.extension().string();
        std::filesystem::path dir = srcPath.parent_path();
        std::filesystem::path candidate;
        int n = 1;
        do {
            candidate = dir / (stem + " (" + std::to_string(n) + ")" + ext);
            n++;
        } while (std::filesystem::exists(candidate));

        std::error_code copyErr;
        std::filesystem::copy_file(srcPath, candidate, copyErr);
        if (copyErr) {
            Log::Error("Failed to duplicate '" + key + "': " + copyErr.message());
            return {};
        }

        // (#175) A duplicated Scene or Screenshot (neither is an AssetLibrary entry — see the
        // "registered" checks below) still just copied a real file into scenes/ or screenshots/,
        // so the corresponding cached directory listing needs invalidating same as any other
        // create. Harmless if the extension happens to belong to something else entirely.
        std::string dupExt = candidate.extension().string();
        for (char& c : dupExt) c = (char)tolower((unsigned char)c);
        if (dupExt == ".json") InvalidateScenesListing();
        else if (dupExt == ".png" || dupExt == ".jpg" || dupExt == ".jpeg") InvalidateShotsListing();

        std::string newPath = candidate.generic_string();
        std::string folder = assets.AssetFolder(key);

        bool registered = false;
        for (const auto& model : assets.Models()) {
            if (model->Path() == key) { assets.LoadModel(newPath); registered = true; break; }
        }
        if (!registered) for (const auto& tex : assets.Textures()) {
            if (tex->Path() == key) { assets.LoadTexture(newPath); registered = true; break; }
        }
        if (!registered) for (const auto& sound : assets.Sounds()) {
            if (sound == key) { if (AudioEngine::Load(newPath)) { assets.RegisterSound(newPath); registered = true; } break; }
        }
        if (!registered) for (const auto& prefab : assets.Prefabs()) {
            if (prefab == key) { assets.RegisterPrefab(newPath); registered = true; break; }
        }

        Log::Info("Duplicated '" + srcPath.filename().string() + "' -> '" + candidate.filename().string() + "'.");
        if (!registered) return {};
        assets.SetAssetFolder(newPath, folder);
        return newPath;
    };

    PushUndo(world, targets.size() > 1 ? "Duplicate Assets" : "Duplicate Asset");
    std::vector<std::string> newKeys;
    for (const auto& t : targets) {
        std::string newKey = duplicateOne(t.Key);
        if (!newKey.empty()) newKeys.push_back(newKey);
    }

    // Select the duplicates afterward, same as Unity's own Ctrl+D.
    if (!newKeys.empty()) {
        ClearAssetSelection();
        m_SelectedAssetKey = newKeys[0];
        m_SelectedAssetIsFolder = false;
        for (size_t i = 1; i < newKeys.size(); ++i) m_ExtraAssetSelection.push_back({newKeys[i], false});
    }
}

void EditorLayer::CommitRename(World& world, AssetLibrary& assets) {
    std::string newName = m_RenameBuffer;
    if (!newName.empty()) {
        if (m_RenamingIsFolder) {
            PushUndo(world, "Rename Folder");
            std::string parent = ParentFolderOf(m_RenamingAssetKey);
            std::string newPath = parent.empty() ? newName : (parent + "/" + newName);
            assets.RenameFolder(m_RenamingAssetKey, newPath);
            if (m_CurrentAssetFolder == m_RenamingAssetKey) {
                m_CurrentAssetFolder = newPath;
            } else if (m_CurrentAssetFolder.rfind(m_RenamingAssetKey + "/", 0) == 0) {
                m_CurrentAssetFolder = newPath + m_CurrentAssetFolder.substr(m_RenamingAssetKey.size());
            }
            if (m_SelectedAssetKey == m_RenamingAssetKey) m_SelectedAssetKey = newPath;
        } else {
            PushUndo(world, "Rename Asset");
            assets.SetDisplayName(m_RenamingAssetKey, newName);
        }
    }
    m_RenamingAssetKey.clear();
}

void EditorLayer::SetFolderExpandedRecursive(AssetLibrary& assets, const std::string& folderPath, bool expand, bool recursive) {
    if (expand) m_ExpandedAssetFolders.insert(folderPath);
    else m_ExpandedAssetFolders.erase(folderPath);
    if (!recursive) return;
    for (const auto& f : assets.Folders()) {
        if (ParentFolderOf(f) == folderPath) SetFolderExpandedRecursive(assets, f, expand, true);
    }
}

namespace {
// Draws `text` as at most `maxLines` lines that fit `wrapWidth`, each line horizontally
// centred within [pos.x, pos.x + wrapWidth] under the tile's icon. Breaks on word boundaries
// where possible; if the whole string still doesn't fit, the last line is trimmed at the
// character level and gets a trailing "…". Replaces ImDrawList::AddText's own wrap_width mode,
// which splits a single long word across lines ("Chesterfi / eld Sofa"). Returns true when
// anything was clipped, so the caller can add a hover tooltip with the full name.
bool DrawClampedGridLabel(ImDrawList* dl, ImVec2 pos, float wrapWidth, float lineHeight,
                          ImU32 color, const char* text, int maxLines = 2) {
    ImFont* font = ImGui::GetFont();
    const float sz = ImGui::GetFontSize();
    const char* const textEnd = text + strlen(text);
    const char* s = text;
    const float left = pos.x;

    // Draw one already-delimited run centred on this line, then advance to the next line.
    auto drawCentered = [&](const char* a, const char* b) {
        float w = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, a, b).x;
        dl->AddText(font, sz, ImVec2(left + (wrapWidth - w) * 0.5f, pos.y), color, a, b);
        pos.y += lineHeight;
    };

    for (int line = 0; line < maxLines; ++line) {
        if (s >= textEnd) return false;
        const bool lastLine = (line == maxLines - 1);

        // Whole remainder fits on this line?
        if (font->CalcTextSizeA(sz, FLT_MAX, 0.0f, s, textEnd).x <= wrapWidth) {
            drawCentered(s, textEnd);
            return false;
        }

        if (!lastLine) {
            // Word-wrap break for this line, then continue with the rest below.
            const char* brk = font->CalcWordWrapPosition(sz, s, textEnd, wrapWidth);
            if (brk <= s) brk = s + 1; // guarantee forward progress on an unbreakable word
            drawCentered(s, brk);
            s = brk;
            while (s < textEnd && (*s == ' ' || *s == '\n')) ++s; // skip the breaking blank
            continue;
        }

        // Last line and it overflows: character-trim to leave room for the ellipsis, then
        // centre the visible text + "…" together.
        static const char* kEllipsis = "\xE2\x80\xA6";
        const float ellW = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, kEllipsis).x;
        const char* fitEnd = s;
        font->CalcTextSizeA(sz, ImMax(wrapWidth - ellW, 1.0f), 0.0f, s, textEnd, &fitEnd);
        if (fitEnd <= s) fitEnd = s + 1; // always show at least one glyph
        const float textW = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, s, fitEnd).x;
        const float x0 = left + (wrapWidth - (textW + ellW)) * 0.5f;
        dl->AddText(font, sz, ImVec2(x0, pos.y), color, s, fitEnd);
        dl->AddText(font, sz, ImVec2(x0 + textW, pos.y), color, kEllipsis);
        return true;
    }
    return true;
}
} // namespace

// Unity Project window's left pane (a real folder hierarchy with expand/collapse arrows, Alt+click
// for recursive) moved into EditorModuleAssetBrowser.cpp (#229). Expansion state stays here in
// m_ExpandedAssetFolders (host-side) so the tree's Left/Right-arrow shortcuts keep working and it
// survives a reload; SetFolderExpandedRecursive below is still its writer, and the module reaches
// it (and the drop-onto-folder / rename-folder AssetLibrary ops) through EditorModuleHostAPI.

// Rescans scenes/ on disk into m_ScenesListingCache — only when the cache has been invalidated
// (timer, Asset Browser focus regained, or an explicit create/delete/duplicate elsewhere in this
// file). See the header for why this replaced a per-frame directory_iterator (#175).
void EditorLayer::RefreshAssetBrowser() {
    InvalidateScenesListing();
    InvalidateShotsListing();
    InvalidateModelThumbnail();          // clears every cached model thumbnail
    m_ShotThumbs.clear();                // shared_ptr<Texture> entries free their GL textures here
    m_AssetListingRefreshTimer = 0.0f;
    m_AssetRefreshFlash = 1.6f; // drives the module's brief "Assets refreshed" confirmation
    Log::Info("Asset Browser refreshed - re-scanned scenes/ and screenshots/, dropped thumbnail caches.");
}

void EditorLayer::RefreshScenesListingIfNeeded() {
    if (m_ScenesListingCache.valid) return;
    m_ScenesListingCache.paths.clear();
    std::error_code ec;
    // Under the project folder, alongside scene.json — not the working directory (see
    // ProjectPaths.h), so saved scenes are tracked content rather than build output.
    const std::string scenesDir = ProjectPaths::Resolve("scenes");
    std::filesystem::create_directory(scenesDir, ec);
    for (const auto& entry : std::filesystem::directory_iterator(scenesDir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        m_ScenesListingCache.paths.push_back(entry.path().generic_string());
    }
    m_ScenesListingCache.valid = true;
}

// Same idea for project/screenshots/ — see RefreshScenesListingIfNeeded above.
void EditorLayer::RefreshShotsListingIfNeeded() {
    if (m_ShotsListingCache.valid) return;
    m_ShotsListingCache.paths.clear();
    std::error_code ec;
    const std::string shotsDir = Screenshot::Dir();
    for (const auto& entry : std::filesystem::directory_iterator(shotsDir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg") continue;
        if (entry.path().filename().string().rfind('.', 0) == 0) continue; // .shutter.wav etc.
        m_ShotsListingCache.paths.push_back(entry.path().generic_string());
    }
    m_ShotsListingCache.valid = true;
}

// kind 0/1/2 = model / texture / sound. Opens the OS file dialog, then imports into the current
// folder — the host half of the module's +Create/Import menu.
void EditorLayer::AssetBrowserImportViaDialog(World& world, AssetLibrary& assets, int kind, const std::string& intoFolder) {
    const char* filter =
        kind == 0 ? "3D Models\0*.fbx;*.obj;*.gltf;*.glb\0All Files\0*.*\0" :
        kind == 1 ? "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0" :
                    "Audio\0*.wav;*.mp3;*.ogg;*.flac\0All Files\0*.*\0";
    std::string p = FileDialog::OpenFile(filter, m_Window);
    if (!p.empty() && m_EditorCameraPtr)
        ImportDroppedFile(world, assets, *m_EditorCameraPtr, p, intoFolder);
}

void EditorLayer::SetAssetTreeWidthPx(float px, bool commit) {
    m_AssetTreeWidth = std::clamp(px, 140.0f * m_UIScale, 460.0f * m_UIScale);
    if (commit) { EditorSettings::Get().AssetBrowserTreeWidth = m_AssetTreeWidth; EditorSettings::Save(); }
}

void EditorLayer::AssetBrowserCreateFolder(World& world, AssetLibrary& assets, const std::string& path) {
    PushUndo(world, "Create Folder");
    assets.CreateFolder(path);
}
void EditorLayer::AssetBrowserRenameFolder(World& world, AssetLibrary& assets,
                                           const std::string& oldPath, const std::string& newPath) {
    PushUndo(world, "Move Folder");
    assets.RenameFolder(oldPath, newPath);
}
void EditorLayer::AssetBrowserMoveAssetToFolder(World& world, AssetLibrary& assets,
                                                const std::string& assetKey, const std::string& folder) {
    PushUndo(world, "Move Asset to Folder");
    assets.SetAssetFolder(assetKey, folder);
}

// Per-frame prep the module calls before drawing the folder tree: keep the scenes/screenshots
// directory-listing caches fresh (short timer + focus-gained edge), make sure the two
// filesystem-backed virtual folders exist, and — whenever the current folder changed from
// elsewhere (a folder tile, a search hit, Backspace) — force its ancestors open and hand back the
// folder that should scroll itself into view this frame ("" = none). m_AssetBrowserFocused is set
// by the module through SetAssetBrowserFocused().
std::string EditorLayer::AssetBrowserTreeFrameSetup(AssetLibrary& assets) {
    constexpr float kAssetListingRefreshInterval = 1.5f; // seconds
    const bool focusGained = m_AssetBrowserFocused && !m_AssetBrowserFocusedLastFrame;
    m_AssetBrowserFocusedLastFrame = m_AssetBrowserFocused;
    m_AssetListingRefreshTimer += ImGui::GetIO().DeltaTime;
    if (focusGained || m_AssetListingRefreshTimer >= kAssetListingRefreshInterval) {
        m_AssetListingRefreshTimer = 0.0f;
        InvalidateScenesListing();
        InvalidateShotsListing();
    }
    assets.CreateFolder("Scenes");
    assets.CreateFolder("Screenshots");

    std::string reveal;
    if (m_CurrentAssetFolder != m_AssetFolderTreeRevealed) {
        m_AssetFolderTreeRevealed = m_CurrentAssetFolder;
        if (!m_CurrentAssetFolder.empty()) reveal = m_CurrentAssetFolder;
        for (size_t s = 0; s < m_CurrentAssetFolder.size();) {
            size_t slash = m_CurrentAssetFolder.find('/', s);
            m_ExpandedAssetFolders.insert(m_CurrentAssetFolder.substr(
                0, slash == std::string::npos ? m_CurrentAssetFolder.size() : slash));
            if (slash == std::string::npos) break;
            s = slash + 1;
        }
    }
    return reveal;
}

// The asset grid (right pane) + the footer + the delete-confirm popup — everything the thin-slice
// #229 migration left host-side. Drawn into the module's "Asset Browser" window, between its
// tree/splitter and its End(); `contentHeight` is the tree pane's height, computed module-side.
// Rebuild the per-frame tile list and run the Ctrl+A "select every visible item" shortcut. The
// module calls this right after ImGui::BeginChild("##AssetList") so IsWindowFocused is meaningful.
void EditorLayer::AssetGridFrameBegin(World& world, AssetLibrary& assets) {
    (void)world;
    auto& cells = m_AssetGridCells;
    cells.clear();

    bool searching = !m_AssetSearchFilter.empty();
    ParsedAssetSearch parsedSearch = ParseAssetSearch(m_AssetSearchFilter);
    // A type/label filter still narrows results even inside a specific folder (not just while
    // searching by name) - e.g. "t:Texture" alone, browsing normally, should hide non-textures
    // right where they are rather than forcing a switch to whole-library search first.
    // "filtering" collects candidate cells from the WHOLE library (not just the current
    // folder). A search/type/label filter does that, and so does the favourites-only view —
    // its whole point is to gather starred assets from anywhere.
    bool filtering = searching || !parsedSearch.typeTerms.empty() || !parsedSearch.labelTerms.empty()
                     || m_AssetFavoritesOnly;

    // Search scope (#236 G): unless "whole project" is on, a filtered result must also live in
    // the current folder or one of its descendants. Browsing (no filter) is always folder-local.
    const bool scopeGlobal = EditorSettings::Get().AssetSearchGlobal;
    auto inSearchScope = [&](const std::string& assetFolder) {
        if (scopeGlobal || m_AssetFavoritesOnly || m_CurrentAssetFolder.empty()) return true;
        return assetFolder == m_CurrentAssetFolder ||
               assetFolder.rfind(m_CurrentAssetFolder + "/", 0) == 0;
    };

    // A special, filesystem-backed folder (not one of AssetLibrary's virtual reference
    // folders) listing every *.json under scenes/ on disk, so scenes can be browsed and
    // opened the same way models/textures/sounds are, instead of only via File > Open.
    static const std::string kScenesFolder = "Scenes";
    assets.CreateFolder(kScenesFolder);
    if ((filtering && inSearchScope(kScenesFolder)) || m_CurrentAssetFolder == kScenesFolder) {
        RefreshScenesListingIfNeeded(); // (#175) cached — see m_ScenesListingCache
        for (const auto& path : m_ScenesListingCache.paths) {
            std::string name = std::filesystem::path(path).stem().string();
            if (!MatchesAssetSearch(parsedSearch, name, "scene", assets.Labels(path))) continue;
            cells.push_back({Cell::Kind::Scene, path, name, nullptr, nullptr});
        }
    }

    // Same idea for the Capture tool's output: a "Screenshots" folder listing every PNG/JPG
    // under project/screenshots/ (thumbnails loaded lazily, cached, and dropped when the file
    // disappears). Not AssetLibrary entries — real image files, browsable and deletable here.
    static const std::string kShotsFolder = "Screenshots";
    assets.CreateFolder(kShotsFolder);
    if ((filtering && inSearchScope(kShotsFolder)) || m_CurrentAssetFolder == kShotsFolder) {
        RefreshShotsListingIfNeeded(); // (#175) cached — see m_ShotsListingCache
        std::set<std::string> seen;
        for (const auto& path : m_ShotsListingCache.paths) {
            std::string name = std::filesystem::path(path).stem().string();
            if (!MatchesAssetSearch(parsedSearch, name, "texture", {})) continue;
            seen.insert(path);
            auto it = m_ShotThumbs.find(path);
            std::shared_ptr<Texture> thumb;
            if (it != m_ShotThumbs.end()) {
                thumb = it->second;
            } else if (m_ScreenshotThumbBudgetThisFrame > 0) {
                // Not loaded yet and budget's still open this frame: decode/upload it now and
                // cache it. Otherwise leave it out of the map so the cell falls back to the
                // folder glyph (see the Cell::Kind::Screenshot draw below) and gets retried on
                // a later frame once the budget resets (#176) — same pattern as ModelThumbnail.
                m_ScreenshotThumbBudgetThisFrame--;
                thumb = m_ShotThumbs.emplace(path, LoadScreenshotThumbTexture(path)).first->second;
            }
            cells.push_back({Cell::Kind::Screenshot, path, name, nullptr, thumb});
        }
        // Drop thumbnails for files that were deleted since last frame. O(n) single pass against
        // the `seen` set built above, rather than an std::find over a vector per map entry (#176).
        for (auto it = m_ShotThumbs.begin(); it != m_ShotThumbs.end();)
            it = (seen.find(it->first) == seen.end())
                     ? m_ShotThumbs.erase(it) : std::next(it);
    }

    for (const auto& folder : assets.Folders()) {
        bool show = filtering ? (MatchesAssetSearch(parsedSearch, LeafNameOf(folder), "folder", {})
                                 && inSearchScope(ParentFolderOf(folder)))
            : (ParentFolderOf(folder) == m_CurrentAssetFolder);
        if (show) cells.push_back({Cell::Kind::Folder, folder, LeafNameOf(folder), nullptr, nullptr});
    }
    for (const auto& model : assets.Models()) {
        std::string path = model->Path();
        std::string name = assets.DisplayName(path);
        if (!MatchesAssetSearch(parsedSearch, name, "model", assets.Labels(path))) continue;
        bool show = filtering ? inSearchScope(assets.AssetFolder(path)) : (assets.AssetFolder(path) == m_CurrentAssetFolder);
        if (show) cells.push_back({Cell::Kind::Model, path, name, model, nullptr});
    }
    for (const auto& tex : assets.Textures()) {
        std::string path = tex->Path();
        std::string name = assets.DisplayName(path);
        if (!MatchesAssetSearch(parsedSearch, name, "texture", assets.Labels(path))) continue;
        bool show = filtering ? inSearchScope(assets.AssetFolder(path)) : (assets.AssetFolder(path) == m_CurrentAssetFolder);
        if (show) cells.push_back({Cell::Kind::Texture, path, name, nullptr, tex});
    }
    for (const auto& mat : assets.Materials()) {
        std::string path = mat->Path;
        std::string name = assets.DisplayName(path);
        if (!MatchesAssetSearch(parsedSearch, name, "material", assets.Labels(path))) continue;
        bool show = filtering ? inSearchScope(assets.AssetFolder(path)) : (assets.AssetFolder(path) == m_CurrentAssetFolder);
        if (show) cells.push_back({Cell::Kind::Material, path, name, nullptr, nullptr, mat});
    }
    for (const auto& sound : assets.Sounds()) {
        std::string name = assets.DisplayName(sound);
        if (!MatchesAssetSearch(parsedSearch, name, "sound", assets.Labels(sound))) continue;
        bool show = filtering ? inSearchScope(assets.AssetFolder(sound)) : (assets.AssetFolder(sound) == m_CurrentAssetFolder);
        if (show) cells.push_back({Cell::Kind::Sound, sound, name, nullptr, nullptr});
    }
    for (const auto& prefab : assets.Prefabs()) {
        std::string name = assets.DisplayName(prefab);
        if (!MatchesAssetSearch(parsedSearch, name, "prefab", assets.Labels(prefab))) continue;
        bool show = filtering ? inSearchScope(assets.AssetFolder(prefab)) : (assets.AssetFolder(prefab) == m_CurrentAssetFolder);
        if (show) cells.push_back({Cell::Kind::Prefab, prefab, name, nullptr, nullptr});
    }

    // Favourites view (#236 G): a flat list of just the starred entries — assets, folders,
    // scenes and screenshots alike — from anywhere.
    if (m_AssetFavoritesOnly) {
        cells.erase(std::remove_if(cells.begin(), cells.end(),
            [&](const Cell& c) { return !IsAssetFavorite(c.key); }), cells.end());
    }

    // Sort control (#236 G) — folders always first; within each group, Name / Type / Date / Size,
    // ascending or descending. Date/Size stat the on-disk file once here (primitive:// and other
    // fileless keys fall back to 0, sorting to the "oldest / smallest" end).
    {
        const int mode = std::clamp(EditorSettings::Get().AssetSortMode, 0, 3);
        const bool desc = EditorSettings::Get().AssetSortDesc;
        auto typeRank = [](Cell::Kind k) {
            switch (k) {
                case Cell::Kind::Folder:     return 0;
                case Cell::Kind::Scene:      return 1;
                case Cell::Kind::Prefab:     return 2;
                case Cell::Kind::Model:      return 3;
                case Cell::Kind::Texture:    return 4;
                case Cell::Kind::Material:   return 5;
                case Cell::Kind::Sound:      return 6;
                case Cell::Kind::Screenshot: return 7;
            }
            return 7;
        };
        std::unordered_map<std::string, long long> mtime;
        std::unordered_map<std::string, unsigned long long> fsize;
        if (mode == 2 || mode == 3) {
            std::error_code ec;
            for (const auto& c : cells) {
                if (c.kind == Cell::Kind::Folder) continue;
                std::filesystem::path p(c.key);
                if (!std::filesystem::exists(p, ec)) continue;
                if (mode == 2) {
                    auto t = std::filesystem::last_write_time(p, ec);
                    if (!ec) mtime[c.key] = (long long)t.time_since_epoch().count();
                } else {
                    auto s = std::filesystem::file_size(p, ec);
                    if (!ec) fsize[c.key] = (unsigned long long)s;
                }
            }
        }
        auto lower = [](std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
        std::sort(cells.begin(), cells.end(), [&](const Cell& a, const Cell& b) {
            const bool aFolder = a.kind == Cell::Kind::Folder, bFolder = b.kind == Cell::Kind::Folder;
            if (aFolder != bFolder) return aFolder; // folders first, always
            int cmp = 0;
            switch (mode) {
                case 1: cmp = typeRank(a.kind) - typeRank(b.kind); break;
                case 2: {
                    long long ta = mtime.count(a.key) ? mtime[a.key] : 0;
                    long long tb = mtime.count(b.key) ? mtime[b.key] : 0;
                    cmp = (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
                    break;
                }
                case 3: {
                    unsigned long long sa = fsize.count(a.key) ? fsize[a.key] : 0;
                    unsigned long long sb = fsize.count(b.key) ? fsize[b.key] : 0;
                    cmp = (sa < sb) ? -1 : (sa > sb) ? 1 : 0;
                    break;
                }
                default: break;
            }
            if (cmp == 0) cmp = lower(a.display).compare(lower(b.display)); // stable tiebreak by name
            return desc ? cmp > 0 : cmp < 0;
        });
    }

    // Ctrl+A - select every currently-visible item (respecting the active search/filter, same
    // as Unity's own "select all visible items in list").
    if (m_AssetBrowserFocused && m_RenamingAssetKey.empty() && ImGui::GetIO().KeyCtrl
        && ImGui::IsKeyPressed(ImGuiKey_A) && !cells.empty()) {
        ClearAssetSelection();
        m_SelectedAssetKey = cells[0].key;
        m_SelectedAssetIsFolder = cells[0].kind == Cell::Kind::Folder;
        for (size_t i = 1; i < cells.size(); ++i) {
            m_ExtraAssetSelection.push_back({cells[i].key, cells[i].kind == Cell::Kind::Folder});
        }
    }

    // #219: the wrapping grid clips by ROW (see the module's clipper loop). The cell-size math and
    // cellsPerRow live module-side now, derived from GetAssetGridMetrics(); DrawAssetCell just
    // takes the rect it was told to fill. List mode is a grid with one cell per row.
}

// One tile at the module's current cursor. `gridMode` false = the compact icon+name list row.
// The module owns the per-row SameLine wrapping, so this never advances past its own cell.
void EditorLayer::DrawAssetCell(World& world, AssetLibrary& assets, int index, float cellW, float cellH, bool gridMode) {
    auto& cells = m_AssetGridCells;
    if (index < 0 || (size_t)index >= cells.size()) return;
    const size_t cellIndex = (size_t)index;
    const float cellWidth = cellW;
    const float cellHeight = cellH;
    const float cellPadding = 8.0f;
    {
        const auto& cell = cells[cellIndex];
        ImGui::PushID(cell.key.c_str());

        bool isFolder = cell.kind == Cell::Kind::Folder;
        bool isSelected = IsAssetSelected(cell.key, isFolder);
        bool isRenaming = m_RenamingAssetKey == cell.key && m_RenamingIsFolder == isFolder;
        bool playing = cell.kind == Cell::Kind::Sound && AudioEngine::IsPreviewPlaying(cell.key);
        const char* icon = isFolder ? ICON_FA_FOLDER
            : cell.kind == Cell::Kind::Model ? (cell.model->HasAnimations() ? ICON_FA_FILM : ICON_FA_CUBE)
            : cell.kind == Cell::Kind::Scene ? ICON_FA_MAP
            : cell.kind == Cell::Kind::Prefab ? ICON_FA_BOX_ARCHIVE
            : cell.kind == Cell::Kind::Screenshot ? ICON_FA_IMAGE
            : cell.kind == Cell::Kind::Material ? ICON_FA_DROPLET
            : (playing ? ICON_FA_STOP : ICON_FA_MUSIC);

        bool clicked = false;
        if (gridMode) {
            // The tile itself (Selectable for its background/hit-test, or a same-size Dummy
            // while renaming) stays "the last submitted item" for everything below (hover,
            // drag-drop, context menu) - icon and label are painted directly onto the draw
            // list afterward rather than as their own widgets, so they never steal that.
            ImVec2 tileMin = ImGui::GetCursorScreenPos();
            ImVec2 tileSize(cellWidth, cellHeight);
            if (!isRenaming) {
                clicked = ImGui::Selectable("##tile", isSelected, ImGuiSelectableFlags_None, tileSize);
            } else {
                ImGui::Dummy(tileSize);
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
            unsigned int modelThumb = (cell.kind == Cell::Kind::Model) ? ModelThumbnail(*cell.model) : 0u;
            if (cell.texture && (cell.kind == Cell::Kind::Texture || cell.kind == Cell::Kind::Screenshot)) {
                float aspect = cell.texture->Height() > 0 ? (float)cell.texture->Width() / (float)cell.texture->Height() : 1.0f;
                ImVec2 imgSize = aspect >= 1.0f ? ImVec2(m_AssetIconSize, m_AssetIconSize / aspect) : ImVec2(m_AssetIconSize * aspect, m_AssetIconSize);
                ImVec2 imgPos(tileMin.x + (cellWidth - imgSize.x) * 0.5f, tileMin.y + cellPadding * 0.5f + (m_AssetIconSize - imgSize.y) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)cell.texture->GLHandle(), imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y));
            } else if (modelThumb) {
                ImVec2 imgSize(m_AssetIconSize, m_AssetIconSize);
                ImVec2 imgPos(tileMin.x + (cellWidth - imgSize.x) * 0.5f, tileMin.y + cellPadding * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)modelThumb, imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y));
            } else if (cell.kind == Cell::Kind::Sound && !SoundWaveform(cell.key).empty()) {
                // Waveform envelope in the icon square (#236 G), with a small play/stop glyph
                // bottom-right so it still reads as a clickable sound.
                const std::vector<float>& peaks = SoundWaveform(cell.key);
                const float w = m_AssetIconSize, h = m_AssetIconSize;
                const ImVec2 o(tileMin.x + (cellWidth - w) * 0.5f, tileMin.y + cellPadding * 0.5f);
                const float midY = o.y + h * 0.5f;
                const ImU32 wc = ImGui::GetColorU32(ImGuiCol_SliderGrab);
                for (size_t i = 0; i < peaks.size(); ++i) {
                    const float x = o.x + (float)i / (float)(peaks.size() - 1) * w;
                    const float a = std::clamp(peaks[i], 0.0f, 1.0f) * (h * 0.46f);
                    dl->AddLine(ImVec2(x, midY - a), ImVec2(x, midY + a), wc, 1.2f);
                }
                ImFont* font = ImGui::GetFont();
                const float gs = m_AssetIconSize * 0.32f;
                dl->AddText(font, gs, ImVec2(o.x + w - gs, o.y + h - gs),
                            ImGui::GetColorU32(ImGuiCol_Text), playing ? ICON_FA_STOP : ICON_FA_PLAY);
            } else {
                ImFont* font = ImGui::GetFont();
                ImVec2 glyphSize = font->CalcTextSizeA(m_AssetIconSize, FLT_MAX, 0.0f, icon);
                ImVec2 glyphPos(tileMin.x + (cellWidth - glyphSize.x) * 0.5f, tileMin.y + cellPadding * 0.5f + (m_AssetIconSize - glyphSize.y) * 0.5f);
                dl->AddText(font, m_AssetIconSize, glyphPos, textColor, icon);
            }

            // Favourite star badge, top-right of the tile (#236 G).
            if (IsAssetFavorite(cell.key)) {
                const float ss = std::max(10.0f, m_AssetIconSize * 0.28f);
                dl->AddText(ImGui::GetFont(), ss, ImVec2(tileMin.x + cellWidth - ss - 3.0f, tileMin.y + 2.0f),
                            ImGui::GetColorU32(ImGuiCol_SliderGrab), ICON_FA_STAR);
            }

            if (!isRenaming) {
                // Label box spans the whole cell (like the icon above it, which is centred in
                // cellWidth), with a small inset so a full-width line doesn't touch the edges —
                // DrawClampedGridLabel centres each line within this box.
                ImVec2 labelPos(tileMin.x + 3.0f, tileMin.y + m_AssetIconSize + cellPadding);
                bool truncated = DrawClampedGridLabel(dl, labelPos, cellWidth - 6.0f,
                    ImGui::GetTextLineHeightWithSpacing(), textColor, cell.display.c_str(), /*maxLines=*/1);
                if (truncated && ImGui::IsItemHovered()) {
                    EditorUI::SetTooltip(cell.display.c_str());
                }
            } else {
                ImGui::SetCursorScreenPos(ImVec2(tileMin.x + 2.0f, tileMin.y + m_AssetIconSize + cellPadding));
                ImGui::SetNextItemWidth(cellWidth - 4.0f);
                if (m_RenamingJustStarted) {
                    ImGui::SetKeyboardFocusHere();
                    m_RenamingJustStarted = false;
                }
                bool done = ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
                bool lostFocus = ImGui::IsItemDeactivated() && !done;
                if (done) CommitRename(world, assets);
                else if (cancel || lostFocus) m_RenamingAssetKey.clear();
                ImGui::SetCursorScreenPos(ImVec2(tileMin.x, tileMin.y + tileSize.y));
            }
        } else {
            // List mode: little icon (real thumbnail for textures, a Font Awesome glyph
            // otherwise) followed by the name, mirroring how the Scene Hierarchy lists rows.
            float rowIconSize = ImGui::GetTextLineHeight();
            unsigned int rowModelThumb = (cell.kind == Cell::Kind::Model) ? ModelThumbnail(*cell.model) : 0u;
            if (cell.texture && (cell.kind == Cell::Kind::Texture || cell.kind == Cell::Kind::Screenshot)) {
                ImGui::Image((ImTextureID)(intptr_t)cell.texture->GLHandle(), ImVec2(rowIconSize, rowIconSize));
            } else if (rowModelThumb) {
                ImGui::Image((ImTextureID)(intptr_t)rowModelThumb, ImVec2(rowIconSize, rowIconSize));
            } else {
                ImGui::TextUnformatted(icon);
            }
            ImGui::SameLine();

            if (isRenaming) {
                ImGui::SetNextItemWidth(-1);
                if (m_RenamingJustStarted) {
                    ImGui::SetKeyboardFocusHere();
                    m_RenamingJustStarted = false;
                }
                bool done = ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
                bool lostFocus = ImGui::IsItemDeactivated() && !done;
                if (done) CommitRename(world, assets);
                else if (cancel || lostFocus) m_RenamingAssetKey.clear();
            } else {
                clicked = ImGui::Selectable(cell.display.c_str(), isSelected);
            }
        }

        if (clicked) {
            // One selection context at a time: clicking an asset drops the scene-entity
            // selection, so the Inspector shows this asset's Import Settings instead of staying
            // on whatever object was selected (it otherwise always wins, so an asset click
            // right after e.g. a Hierarchy rename appeared to do nothing).
            ClearSelection();

            ImGuiIO& assetIO = ImGui::GetIO();
            if (assetIO.KeyShift && !m_SelectedAssetKey.empty()) {
                // Range-select from the anchor to here, replacing the current selection —
                // Unity/Explorer-standard Shift-click behavior. The anchor index is only
                // meaningful against this same, currently-visible `cells` list.
                ClearAssetSelection();
                size_t lo = std::min(m_AssetSelectionAnchorIndex, cellIndex);
                size_t hi = std::min(std::max(m_AssetSelectionAnchorIndex, cellIndex), cells.size() - 1);
                for (size_t i = lo; i <= hi; ++i) {
                    bool f = cells[i].kind == Cell::Kind::Folder;
                    if (i == lo) { m_SelectedAssetKey = cells[i].key; m_SelectedAssetIsFolder = f; }
                    else m_ExtraAssetSelection.push_back({cells[i].key, f});
                }
                // Deliberately don't move the anchor, so repeated Shift-clicks keep extending
                // or shrinking the range from the same starting point.
            } else if (assetIO.KeyCtrl) {
                ToggleAssetSelection(cell.key, isFolder);
                m_AssetSelectionAnchorIndex = cellIndex;
            } else {
                ClearAssetSelection();
                m_SelectedAssetKey = cell.key;
                m_SelectedAssetIsFolder = isFolder;
                m_AssetSelectionAnchorIndex = cellIndex;
            }
            if (cell.kind == Cell::Kind::Sound && !assetIO.KeyShift && !assetIO.KeyCtrl) {
                if (playing) AudioEngine::StopPreview();
                else AudioEngine::PlayPreview(cell.key);
            }
        }
        if (isFolder && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_CurrentAssetFolder = cell.key;
        }
        if (cell.kind == Cell::Kind::Scene && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            RequestOpenScene(world, assets, cell.key);
        }
        if (cell.kind == Cell::Kind::Screenshot && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            OpenScreenshotPreview(cell.key); // centred in-editor lightbox (Show in folder is on the context menu)
        }
        if (cell.kind == Cell::Kind::Prefab && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            PushUndo(world, "Place Prefab Instance");
            entt::entity spawned = SceneSerializer::InstantiatePrefab(world, assets, cell.key);
            if (spawned != entt::null) {
                UniquifyName(world, spawned);
                SelectItem(spawned, false);
            }
        }

        if (isFolder) {
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_FOLDER_PATH", cell.key.c_str(), cell.key.size() + 1);
                ImGui::TextUnformatted(cell.display.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH")) {
                    PushUndo(world, "Move Asset to Folder");
                    assets.SetAssetFolder((const char*)p->Data, cell.key);
                }
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_TEXTURE_PATH")) {
                    PushUndo(world, "Move Asset to Folder");
                    assets.SetAssetFolder((const char*)p->Data, cell.key);
                }
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_SOUND_PATH")) {
                    PushUndo(world, "Move Asset to Folder");
                    assets.SetAssetFolder((const char*)p->Data, cell.key);
                }
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PREFAB_PATH")) {
                    PushUndo(world, "Move Asset to Folder");
                    assets.SetAssetFolder((const char*)p->Data, cell.key);
                }
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_FOLDER_PATH")) {
                    std::string src((const char*)p->Data);
                    if (src != cell.key && cell.key.rfind(src + "/", 0) != 0) {
                        PushUndo(world, "Move Folder");
                        assets.RenameFolder(src, cell.key + "/" + LeafNameOf(src));
                    }
                }
                ImGui::EndDragDropTarget();
            }
        } else if (cell.kind != Cell::Kind::Scene && cell.kind != Cell::Kind::Screenshot) { // not placeable — nothing to drag into the viewport
            const char* payloadType = cell.kind == Cell::Kind::Model ? "ASSET_MODEL_PATH"
                : cell.kind == Cell::Kind::Texture ? "ASSET_TEXTURE_PATH"
                : cell.kind == Cell::Kind::Material ? "ASSET_MATERIAL_PATH"
                : cell.kind == Cell::Kind::Prefab ? "ASSET_PREFAB_PATH" : "ASSET_SOUND_PATH";
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(payloadType, cell.key.c_str(), cell.key.size() + 1);
                ImGui::TextUnformatted(cell.display.c_str());
                ImGui::EndDragDropSource();
            }
        }

        if (!isRenaming && ImGui::IsItemHovered()) {
            if (cell.kind == Cell::Kind::Model) {
                EditorUI::SetTooltip("%s\n\nDrag into the viewport to place\n\n%s",
                    cell.display.c_str(), UsageTooltip(FindModelUsages(world, cell.model.get())).c_str());
            } else if (cell.kind == Cell::Kind::Texture) {
                EditorUI::SetTooltip(
                    "%s\n\nDrag onto a model to set its Albedo map,\nor onto a map row in the Inspector's PBR Material.\n\n%s",
                    cell.display.c_str(), UsageTooltip(FindTextureUsages(world, cell.texture.get())).c_str());
            } else if (cell.kind == Cell::Kind::Material) {
                EditorUI::SetTooltip("%s\n\nDrag onto a model to apply this material.", cell.display.c_str());
            } else if (cell.kind == Cell::Kind::Sound) {
                EditorUI::SetTooltip("%s\n\nClick to preview\n\n%s",
                    cell.display.c_str(), UsageTooltip(FindSoundUsages(world, cell.key)).c_str());
            } else if (cell.kind == Cell::Kind::Scene) {
                EditorUI::SetTooltip("%s\n\nDouble-click to open", cell.display.c_str());
            } else if (cell.kind == Cell::Kind::Prefab) {
                EditorUI::SetTooltip("%s\n\nDrag into the viewport to place an instance,\nor double-click to place one at the origin.",
                    cell.display.c_str());
            } else {
                EditorUI::SetTooltip("%s\n\nDouble-click to open. Drag assets onto it to file them here.", cell.display.c_str());
            }
        }

        if (cell.kind == Cell::Kind::Scene) {
            // Scenes aren't AssetLibrary entries (no rename / remove-from-library — they're real
            // .json files on disk), so they get their own short context menu: Open + Delete.
            if (ImGui::BeginPopupContextItem()) {
                if (!IsAssetSelected(cell.key, false)) {
                    ClearAssetSelection();
                    m_SelectedAssetKey = cell.key;
                    m_SelectedAssetIsFolder = false;
                }
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open")) RequestOpenScene(world, assets, cell.key);
                if (m_ExtraAssetSelection.empty() && ImGui::MenuItem(ICON_FA_COPY "  Copy Path")) {
                    ImGui::SetClipboardText(cell.key.c_str());
                    Log::Info("Copied path: " + cell.key);
                }
                {
                    std::vector<std::string> favKeys{ m_SelectedAssetKey };
                    for (const auto& e : m_ExtraAssetSelection) favKeys.push_back(e.Key);
                    const bool allFav = std::all_of(favKeys.begin(), favKeys.end(),
                        [&](const std::string& k) { return IsAssetFavorite(k); });
                    std::string lbl = std::string(ICON_FA_STAR "  ") +
                        (allFav ? "Remove from Favourites" : "Add to Favourites");
                    if (favKeys.size() > 1) lbl += " (" + std::to_string(favKeys.size()) + ")";
                    if (ImGui::MenuItem(lbl.c_str())) SetAssetFavorites(favKeys, !allFav);
                }

                // Act on the whole selection when the right-clicked scene is part of a
                // multi-selection, same as the generic asset menu.
                std::vector<AssetKeyRef> scenesForAction;
                scenesForAction.push_back({m_SelectedAssetKey, false});
                for (const auto& e : m_ExtraAssetSelection) scenesForAction.push_back(e);
                const bool multi = scenesForAction.size() > 1;
                const bool anyOpen = std::any_of(scenesForAction.begin(), scenesForAction.end(),
                    [&](const AssetKeyRef& r) { return r.Key == m_CurrentScenePath; });
                if (ImGui::MenuItem(multi ? ICON_FA_TRASH "  Delete Selected" : ICON_FA_TRASH "  Delete",
                                    nullptr, false, !(!multi && anyOpen)))
                    RequestDeleteAssets(world, assets, scenesForAction, /*skipDialog=*/false);
                if (!multi && anyOpen && ImGui::IsItemHovered())
                    EditorUI::SetTooltip("This scene is open — open a different scene first.");
                ImGui::EndPopup();
            }
        } else if (cell.kind == Cell::Kind::Screenshot) {
            // Also a real file on disk, not a library asset: Show in folder + Delete.
            if (ImGui::BeginPopupContextItem()) {
                if (!IsAssetSelected(cell.key, false)) {
                    ClearAssetSelection();
                    m_SelectedAssetKey = cell.key;
                    m_SelectedAssetIsFolder = false;
                }
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show in folder")) Screenshot::ShowInFolder(cell.key);
                if (m_ExtraAssetSelection.empty() && ImGui::MenuItem(ICON_FA_COPY "  Copy Path")) {
                    ImGui::SetClipboardText(cell.key.c_str());
                    Log::Info("Copied path: " + cell.key);
                }
                {
                    std::vector<std::string> favKeys{ m_SelectedAssetKey };
                    for (const auto& e : m_ExtraAssetSelection) favKeys.push_back(e.Key);
                    const bool allFav = std::all_of(favKeys.begin(), favKeys.end(),
                        [&](const std::string& k) { return IsAssetFavorite(k); });
                    std::string lbl = std::string(ICON_FA_STAR "  ") +
                        (allFav ? "Remove from Favourites" : "Add to Favourites");
                    if (favKeys.size() > 1) lbl += " (" + std::to_string(favKeys.size()) + ")";
                    if (ImGui::MenuItem(lbl.c_str())) SetAssetFavorites(favKeys, !allFav);
                }
                std::vector<AssetKeyRef> shotsForAction;
                shotsForAction.push_back({m_SelectedAssetKey, false});
                for (const auto& e : m_ExtraAssetSelection) shotsForAction.push_back(e);
                const bool multi = shotsForAction.size() > 1;
                if (ImGui::MenuItem(multi ? ICON_FA_TRASH "  Delete Selected" : ICON_FA_TRASH "  Delete"))
                    RequestDeleteAssets(world, assets, shotsForAction, /*skipDialog=*/false);
                ImGui::EndPopup();
            }
        } else if (ImGui::BeginPopupContextItem()) {
            // Right-clicking an item already part of the selection keeps the whole selection
            // (so Delete/Remove from Library below can act on all of it, Explorer-style);
            // right-clicking an unselected item replaces the selection with just this one.
            if (!IsAssetSelected(cell.key, isFolder)) {
                ClearAssetSelection();
                m_SelectedAssetKey = cell.key;
                m_SelectedAssetIsFolder = isFolder;
            }
            if (ImGui::MenuItem(ICON_FA_PEN "  Rename (F2)", nullptr, false, m_ExtraAssetSelection.empty())) {
                BeginRenameAsset(cell.key, isFolder, cell.display);
            }
            if (ImGui::MenuItem(ICON_FA_TAG "  Edit Labels...")) {
                std::string joined;
                for (const auto& lbl : assets.Labels(cell.key)) {
                    if (!joined.empty()) joined += ", ";
                    joined += lbl;
                }
                snprintf(m_LabelsEditBuffer, sizeof(m_LabelsEditBuffer), "%s", joined.c_str());
                ImGui::OpenPopup("##EditLabels");
            }
            if (ImGui::BeginPopup("##EditLabels")) {
                ImGui::TextDisabled("Comma-separated labels - searchable as l:label");
                ImGui::SetNextItemWidth(240.0f);
                bool enter = ImGui::InputText("##LabelsBuf", m_LabelsEditBuffer, sizeof(m_LabelsEditBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                bool apply = enter || ImGui::Button("Apply");
                if (apply) {
                    std::set<std::string> labels;
                    std::stringstream ss(m_LabelsEditBuffer);
                    std::string part;
                    while (std::getline(ss, part, ',')) {
                        size_t b = part.find_first_not_of(" \t");
                        size_t e = part.find_last_not_of(" \t");
                        if (b != std::string::npos) labels.insert(part.substr(b, e - b + 1));
                    }
                    PushUndo(world, "Edit Labels");
                    assets.SetLabels(cell.key, labels);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (m_ExtraAssetSelection.empty() && ImGui::MenuItem(ICON_FA_COPY "  Copy Path")) {
                ImGui::SetClipboardText(cell.key.c_str());
                Log::Info("Copied path: " + cell.key);
            }
            {
                // Favourites — acts on the whole selection when the right-clicked item is
                // part of a multi-selection (#236 G).
                std::vector<std::string> favKeys{ m_SelectedAssetKey };
                for (const auto& e : m_ExtraAssetSelection) favKeys.push_back(e.Key);
                const bool allFav = std::all_of(favKeys.begin(), favKeys.end(),
                    [&](const std::string& k) { return IsAssetFavorite(k); });
                std::string lbl = std::string(ICON_FA_STAR "  ") +
                    (allFav ? "Remove from Favourites" : "Add to Favourites");
                if (favKeys.size() > 1) lbl += " (" + std::to_string(favKeys.size()) + ")";
                if (ImGui::MenuItem(lbl.c_str())) SetAssetFavorites(favKeys, !allFav);
            }

            // Reimport straight from the context menu instead of only via Import Settings >
            // Apply (#28 P17). Single selection, real imported assets only.
            if (!isFolder && m_ExtraAssetSelection.empty() &&
                (cell.kind == Cell::Kind::Model || cell.kind == Cell::Kind::Texture)) {
                if (ImGui::MenuItem(ICON_FA_ROTATE "  Reimport")) {
                    if (cell.kind == Cell::Kind::Model) {
                        PushUndo(world, "Reimport Model");
                        if (assets.ReimportModel(cell.key)) Log::Info("Reimported model '" + cell.key + "'.");
                        else Log::Error("Reimport failed for '" + cell.key + "' - see Console.");
                        InvalidateModelThumbnail();
                    } else {
                        PushUndo(world, "Reimport Texture");
                        if (assets.ReimportTexture(cell.key)) Log::Info("Reimported texture '" + cell.key + "'.");
                        else Log::Error("Reimport failed for '" + cell.key + "' - see Console.");
                    }
                }
            }

            // Everything currently selected, whenever the right-clicked item is part of a
            // multi-selection - so Delete/Remove from Library act on the whole group rather
            // than just the one cell that happened to receive the right-click.
            std::vector<AssetKeyRef> selectionForAction;
            selectionForAction.push_back({m_SelectedAssetKey, m_SelectedAssetIsFolder});
            for (const auto& e : m_ExtraAssetSelection) selectionForAction.push_back(e);
            const char* deleteLabel = selectionForAction.size() > 1
                ? ICON_FA_TRASH "  Delete Selected" : ICON_FA_TRASH "  Delete Folder";

            if (isFolder) {
                if (ImGui::MenuItem(deleteLabel)) {
                    RequestDeleteAssets(world, assets, selectionForAction, /*skipDialog=*/false);
                }
            } else {
                if (cell.kind == Cell::Kind::Prefab && selectionForAction.size() == 1 && ImGui::MenuItem(ICON_FA_PLUS "  Place Instance")) {
                    PushUndo(world, "Place Prefab Instance");
                    entt::entity spawned = SceneSerializer::InstantiatePrefab(world, assets, cell.key);
                    if (spawned != entt::null) {
                        UniquifyName(world, spawned);
                        SelectItem(spawned, false);
                    }
                }
                const char* removeLabel = selectionForAction.size() > 1
                    ? ICON_FA_TRASH "  Remove Selected from Library" : ICON_FA_TRASH "  Remove from Library";
                if (ImGui::MenuItem(removeLabel)) {
                    RequestDeleteAssets(world, assets, selectionForAction, /*skipDialog=*/false);
                }
                if (cell.kind == Cell::Kind::Prefab) ImGui::TextDisabled("The .prefab file stays on disk.");
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }
}

// The empty-space click-to-clear + right-click "New Folder" popup, drawn by the module right
// after the last DrawAssetCell (still inside its ##AssetList child).
void EditorLayer::HandleAssetGridBackground(World& world, AssetLibrary& assets) {
    auto makeNewFolder = [&]() {
        std::string base = m_CurrentAssetFolder.empty() ? "New Folder" : (m_CurrentAssetFolder + "/New Folder");
        std::string candidate = base;
        int n = 1;
        auto exists = [&](const std::string& p) {
            for (const auto& f : assets.Folders()) if (f == p) return true;
            return false;
        };
        while (exists(candidate)) candidate = base + " (" + std::to_string(n++) + ")";
        PushUndo(world, "Create Folder");
        assets.CreateFolder(candidate);
        BeginRenameAsset(candidate, true, LeafNameOf(candidate));
    };

    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ClearAssetSelection();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##BrowserBgContext");
    }
    if (ImGui::BeginPopup("##BrowserBgContext")) {
        if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder")) makeNewFolder();
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_DROPLET "  Create Material")) {
            std::string folder = m_CurrentAssetFolder;
            std::string base = folder.empty() ? "New Material" : (folder + "/New Material");
            // Pick a path that doesn't already exist on disk.
            std::string candidate = base + ".mat";
            int n = 1;
            while (std::filesystem::exists(candidate)) candidate = base + " (" + std::to_string(n++) + ").mat";
            PushUndo(world, "Create Material");
            auto mat = MaterialAsset::CreateDefault(candidate);
            if (mat) {
                assets.LoadMaterial(candidate);
                BeginRenameAsset(candidate, false, std::filesystem::path(candidate).stem().string());
            }
        }
        ImGui::EndPopup();
    }
}

// Footer text: "N items selected" / the selected item's display name / "".
void EditorLayer::GetAssetSelectionSummary(AssetLibrary& assets, char* out, int n) const {
    if (!out || n <= 0) return;
    std::string s;
    if (!m_ExtraAssetSelection.empty())
        s = std::to_string(m_ExtraAssetSelection.size() + 1) + " items selected";
    else if (!m_SelectedAssetKey.empty())
        s = m_SelectedAssetIsFolder ? m_SelectedAssetKey : assets.DisplayName(m_SelectedAssetKey);
    const int m = (int)s.size() < n - 1 ? (int)s.size() : n - 1;
    std::memcpy(out, s.data(), (size_t)m);
    out[m] = '\0';
}

void EditorLayer::SetAssetIconSize(float px, bool commit) {
    m_AssetIconSize = std::clamp(px, kListViewIconSize * m_UIScale, 128.0f * m_UIScale);
    if (commit) { EditorSettings::Get().AssetBrowserIconSize = m_AssetIconSize; EditorSettings::Save(); }
}
