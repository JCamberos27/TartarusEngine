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
#include "ShaderLibrary.h" // built-in shaders in the Shaders folder (#104)
#include "SceneSerializer.h"
#include "AABB.h"
#include "Log.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "AssetImporterInspector.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "AssetDatabase.h"
#include "UserPaths.h"
#include "AtomicFile.h"
#include "ThumbnailCache.h"
#include "EditorModuleAPI.h" // kAssetDetails*ColW, shared with EditorModuleAssetBrowser.cpp's header row
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
#include <cstdio>
#include <functional>
#include <cfloat>
#include <chrono>
#include <ctime>

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
    std::vector<std::string> typeTerms;  // lowercased kind names: "model","texture","material","shader",
                                         // "sound","scene","prefab","screenshot","folder"
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


// --- Asset favourites (#236 G) --------------------------------------------------------------
// #129 - per-user, so they live in UserPaths (%LOCALAPPDATA%), not the version-controlled
// project folder; an older project/asset_favorites.json is still read until the first save.
void EditorLayer::LoadAssetFavorites() {
    m_AssetFavorites.clear();
    std::ifstream in(UserPaths::Resolve("asset_favorites.json"));
    if (!in.is_open()) in.open(ProjectPaths::Resolve("asset_favorites.json"));
    if (!in.is_open()) return;
    try {
        nlohmann::json root; in >> root;
        if (root.is_array())
            for (const auto& v : root) if (v.is_string()) m_AssetFavorites.insert(v.get<std::string>());
    } catch (const std::exception& e) {
        Log::Warn(std::string("Asset favorites: failed to parse: ") + e.what()); // #19
    }
}

void EditorLayer::SaveAssetFavorites() const {
    nlohmann::json root = nlohmann::json::array();
    for (const auto& k : m_AssetFavorites) root.push_back(k);
    // Atomic: a crash mid-write must not truncate the favourites list (audit CPP-206).
    AtomicFile::WriteJson(UserPaths::Resolve("asset_favorites.json"), root);
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
    unsigned int dst = 0;

    // Phase 5 item 9 — a persistent cache hit skips the render pass (and the model load it
    // implies) entirely: just upload the decoded PNG. Still spends this frame's thumbnail
    // budget above, since a decode-and-upload at scale (1,000+ objects) is exactly the frame-time
    // cost item 9 was written to bound, not only the render.
    // #133 — the cache key covers the model file, its .meta import settings and every texture
    // its materials sample, so re-importing or editing any of those re-renders the thumbnail.
    std::vector<std::string> deps;
    for (int i = 0; i < model.MeshCount(); ++i) {
        const Material& m = model.MeshMaterial(i);
        for (const auto* tex : { &m.AlbedoMap, &m.NormalMap, &m.MetallicRoughnessMap, &m.MetallicMap,
                                 &m.RoughnessMap, &m.AOMap, &m.EmissiveMap, &m.ClearCoatMap, &m.ThicknessMap })
            if (*tex && std::find(deps.begin(), deps.end(), (*tex)->Path()) == deps.end())
                deps.push_back((*tex)->Path());
    }
    const std::uint64_t cacheKey = ThumbnailCache::DependencyKey(path, deps);
    std::vector<unsigned char> cachedPixels;
    if (ThumbnailCache::Load(path, kSize, cacheKey, cachedPixels)) {
        glGenTextures(1, &dst);
        glBindTexture(GL_TEXTURE_2D, dst);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, cachedPixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLStateCache::Invalidate(); // raw glBindTexture above — see the matching note below
    } else {
        const float dist = ModelPreviewRenderer::ComputeFramingDistance(model);
        const unsigned int src = m_ThumbnailPreview.Render(model, 0.7f, 0.5f, dist, kSize, kSize);

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

        // Phase 5 item 9 — persist what was just rendered, so the NEXT launch is a cache hit.
        // glGetTexImage isn't in this codebase's trimmed GL loader (extern/glloader/gl.h); read
        // via glReadPixels from `src` instead, while it's still the bound READ_FRAMEBUFFER
        // attachment above — the exact same pixels glCopyTexSubImage2D just copied into `dst`.
        std::vector<unsigned char> freshPixels((size_t)kSize * kSize * 4);
        glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_UNSIGNED_BYTE, freshPixels.data());
        ThumbnailCache::Save(path, kSize, cacheKey, freshPixels.data());

        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevRead);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned int)prevDraw);

        // The preview render plus the raw glBindTexture calls above never went through
        // GLStateCache; resync it so a later Texture::Bind() on the same unit isn't skipped as
        // falsely-redundant (audit GL-202: the "call Invalidate() after every raw-GL pass" rule
        // this path was missing).
        GLStateCache::Invalidate();
    }

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

void EditorLayer::CopyPreviewTexture(unsigned int src, unsigned int& dst, int size, std::vector<unsigned char>* pixelsOut) {
    if (!dst) {
        glGenTextures(1, &dst);
        glBindTexture(GL_TEXTURE_2D, dst);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    GLint prevRead = 0, prevDraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevDraw);
    if (!m_ThumbnailBlitFbo) glGenFramebuffers(1, &m_ThumbnailBlitFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ThumbnailBlitFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    glBindTexture(GL_TEXTURE_2D, dst);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, size, size);
    if (pixelsOut) {
        pixelsOut->resize((size_t)size * size * 4);
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixelsOut->data());
    }
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (unsigned int)prevRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (unsigned int)prevDraw);
    GLStateCache::Invalidate(); // raw texture binds above
}

// #107 - see MaterialThumb in EditorLayer.h.
unsigned int EditorLayer::MaterialThumbnail(const std::shared_ptr<MaterialAsset>& ma) {
    if (!ma || ma->Missing || ma->Path.empty()) return 0;
    const std::string& path = ma->Path;
    const int kSize = 128;
    const std::uint64_t contentKey = MaterialPreviewRenderer::ContentKey(*ma);

    auto it = m_MaterialThumbs.find(path);
    if (it != m_MaterialThumbs.end()) {
        m_MaterialThumbLRU.erase(it->second.Lru);
        m_MaterialThumbLRU.push_front(path);
        it->second.Lru = m_MaterialThumbLRU.begin();
        if (it->second.ContentKey == contentKey || m_ThumbnailBudgetThisFrame <= 0)
            return it->second.Tex; // current, or stale for a frame until there's budget
    } else if (m_ThumbnailBudgetThisFrame <= 0) {
        return 0;
    }
    m_ThumbnailBudgetThisFrame--;

    // The persistent cache is keyed by the .mat file and every texture file it uses.
    std::vector<std::string> deps;
    const Material& m = ma->Mat;
    for (const auto* tex : { &m.AlbedoMap, &m.NormalMap, &m.MetallicRoughnessMap, &m.MetallicMap, &m.RoughnessMap,
                             &m.AOMap, &m.EmissiveMap, &m.ClearCoatMap, &m.ThicknessMap, &m.HeightMap,
                             &m.DetailAlbedoMap, &m.DetailNormalMap })
        if (*tex && std::find(deps.begin(), deps.end(), (*tex)->Path()) == deps.end()) deps.push_back((*tex)->Path());
    for (const auto& [name, prop] : m.ExtraProps)
        if (prop.Tex && std::find(deps.begin(), deps.end(), prop.Tex->Path()) == deps.end()) deps.push_back(prop.Tex->Path());
    if (ma->Shader && !ma->ShaderPath.empty()) deps.push_back(ma->ShaderPath);
    const std::uint64_t diskKey = ThumbnailCache::DependencyKey(path, deps);

    const bool fresh = it == m_MaterialThumbs.end();
    MaterialThumb entry = fresh ? MaterialThumb{} : it->second;
    std::vector<unsigned char> pixels;
    // First sight this session: a cached PNG whose dependencies are unchanged is the same image.
    if (fresh && ThumbnailCache::Load(path, kSize, diskKey, pixels)) {
        glGenTextures(1, &entry.Tex);
        glBindTexture(GL_TEXTURE_2D, entry.Tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLStateCache::Invalidate();
        entry.DiskKey = diskKey;
    } else {
        const unsigned int src = m_MaterialThumbPreview.Render(ma, MaterialPreviewRenderer::Shape::Sphere,
                                                               0.5f, 0.3f, kSize, kSize);
        if (!src) return entry.Tex;
        // Persist only when the files changed; a live slider drag re-renders in memory without
        // rewriting the PNG every frame.
        const bool persist = diskKey != entry.DiskKey;
        CopyPreviewTexture(src, entry.Tex, kSize, persist ? &pixels : nullptr);
        if (persist) {
            ThumbnailCache::Save(path, kSize, diskKey, pixels.data());
            entry.DiskKey = diskKey;
        }
    }
    entry.ContentKey = contentKey;

    if (fresh) {
        m_MaterialThumbLRU.push_front(path);
        entry.Lru = m_MaterialThumbLRU.begin();
    }
    m_MaterialThumbs[path] = entry;
    if (m_MaterialThumbs.size() > kMaxModelThumbnails) {
        const std::string evict = m_MaterialThumbLRU.back();
        m_MaterialThumbLRU.pop_back();
        auto e = m_MaterialThumbs.find(evict);
        if (e != m_MaterialThumbs.end()) {
            if (e->second.Tex) glDeleteTextures(1, &e->second.Tex);
            m_MaterialThumbs.erase(e);
        }
    }
    return entry.Tex;
}

void EditorLayer::ClearMaterialThumbnails() {
    for (auto& [p, t] : m_MaterialThumbs) { (void)p; if (t.Tex) glDeleteTextures(1, &t.Tex); }
    m_MaterialThumbs.clear();
    m_MaterialThumbLRU.clear();
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

namespace {
// #104 — the engine's own shaders are listed in the Shaders folder (read-only, for preview and
// as material targets) but are part of the install, never deletable from a project.
bool IsBuiltinShaderPath(const std::string& key) {
    const std::string dir = std::filesystem::path(ShaderLibrary::Dir()).generic_string();
    return !dir.empty() && std::filesystem::path(key).generic_string().rfind(dir, 0) == 0;
}
// #129 - true for an existing file inside the project folder (the only kind Delete from Disk may
// send to the Recycle Bin: never an engine file or something imported from elsewhere).
bool IsProjectFile(const std::string& key) {
    std::error_code ec;
    const std::filesystem::path p(key);
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    const std::string file = std::filesystem::weakly_canonical(p, ec).generic_string();
    std::string root = std::filesystem::weakly_canonical(std::filesystem::path(ProjectPaths::Root()), ec).generic_string();
    if (root.empty()) return false;
    if (root.back() != '/') root += '/';
    auto lower = [](std::string x) { std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return (char)std::tolower(c); }); return x; };
    return lower(file).rfind(lower(root), 0) == 0;
}

// #178 - Unity's project-wide Find References: every scene, prefab, material, Animator Controller
// and Physic Material file that mentions the asset, by its GUID (how scenes reference most
// assets, #132) or by its project-relative path. Plain text search; files are small.
std::vector<std::string> FindProjectReferences(const std::string& assetKey) {
    std::vector<std::string> needles;
    if (const AssetGuid g = AssetDatabase::GuidForPath(assetKey); g.IsValid()) needles.push_back(g.ToString());
    std::string rel = std::filesystem::path(assetKey).is_absolute() ? ProjectPaths::Relativize(assetKey) : assetKey;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    if (!rel.empty()) needles.push_back(rel);
    std::vector<std::string> out;
    if (needles.empty()) return out;
    std::error_code ec;
    const std::filesystem::path root(ProjectPaths::Root());
    const std::filesystem::path self = std::filesystem::weakly_canonical(std::filesystem::path(ProjectPaths::Resolve(rel)), ec);
    for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() == 0 && it->is_directory() && it->path().filename() == "Library") {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".json" && ext != ".prefab" && ext != ".mat" && ext != ".controller" && ext != ".physicmaterial") continue;
        std::error_code ec2;
        if (std::filesystem::weakly_canonical(it->path(), ec2) == self) continue;
        std::ifstream in(it->path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (const std::string& needle : needles)
            if (text.find(needle) != std::string::npos) {
                out.push_back(std::filesystem::relative(it->path(), root, ec2).generic_string());
                break;
            }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

void EditorLayer::RequestDeleteAssets(World& world, AssetLibrary& assets, const std::vector<AssetKeyRef>& requested, bool skipDialog) {
    std::vector<AssetKeyRef> items;
    for (const auto& item : requested) {
        if (!item.IsFolder && IsBuiltinShaderPath(item.Key)) {
            Log::Warn("Built-in shaders are part of the engine and can't be deleted: " + item.Key);
            continue;
        }
        items.push_back(item);
    }
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
        // Don't leave the browser pointed at a folder that no longer exists. A raw assignment,
        // not NavigateAssetFolder (Phase 5 item 3) - this is a forced correction after the
        // current folder vanished out from under the user, not a real navigation they'd expect
        // Back to retrace.
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

    // #129 - Delete from Disk confirmation: lists the project files that still reference the
    // asset(s), since those references break once the file is gone.
    const char* kDiskPopupId = "Delete from Disk?";
    if (m_OpenDiskDeleteRequested) {
        ImGui::OpenPopup(kDiskPopupId);
        m_OpenDiskDeleteRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(380.0f * m_UIScale, 0.0f));
    if (ImGui::BeginPopupModal(kDiskPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 340.0f * m_UIScale);
        if (m_PendingDiskDelete.size() == 1)
            ImGui::Text("Move \"%s\" and its .meta to the Recycle Bin?", LeafNameOf(m_PendingDiskDelete[0]).c_str());
        else
            ImGui::Text("Move %d files and their .meta files to the Recycle Bin?", (int)m_PendingDiskDelete.size());
        ImGui::TextDisabled("This can't be undone here - restore from the Recycle Bin if needed.");
        if (!m_PendingDiskDeleteRefs.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(EditorUIPrimitives::WarningColor(), ICON_FA_TRIANGLE_EXCLAMATION "  Still referenced by:");
            const size_t shown = std::min<size_t>(m_PendingDiskDeleteRefs.size(), 8);
            for (size_t i = 0; i < shown; ++i) ImGui::BulletText("%s", m_PendingDiskDeleteRefs[i].c_str());
            if (m_PendingDiskDeleteRefs.size() > shown)
                ImGui::TextDisabled("...and %d more.", (int)(m_PendingDiskDeleteRefs.size() - shown));
            ImGui::TextDisabled("Those references will be missing the next time they load.");
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (PrimaryButton("Cancel", ImVec2(bw, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_PendingDiskDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Delete", ImVec2(bw, 0.0f))) {
            m_DeleteError.clear();
            for (const std::string& key : m_PendingDiskDelete) {
                const std::string leaf = LeafNameOf(key);
                if (!PerformAssetDelete(world, assets, key, false)) continue; // e.g. the open scene
                std::error_code ec;
                for (const std::string& path : {key, key + ".meta"}) {
                    if (!std::filesystem::exists(std::filesystem::path(path), ec)) continue; // scenes recycle themselves
                    std::string why;
                    if (!FileDialog::RecycleFile(path, why)) {
                        if (why.empty()) why = "the file may be open in another program or write-protected";
                        if (!m_DeleteError.empty()) m_DeleteError += "\n";
                        m_DeleteError += "\xE2\x80\xA2 \"" + LeafNameOf(path) + "\" - " + why + ".";
                    }
                }
                AssetDatabase::ForgetPath(key);
                Log::Info("Sent to Recycle Bin: " + leaf);
            }
            m_PendingDiskDelete.clear();
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

        // #129 - carry the import settings / labels over in a fresh .meta (new GUID: it's a new
        // asset), before registering, so the copy imports exactly like the original.
        if (!AssetDatabase::IsSynthetic(key)) {
            nlohmann::json fields = nlohmann::json::parse(AssetDatabase::ReadMetaFields(key), nullptr, false);
            AssetDatabase::EnsureGuid(newPath);
            if (fields.is_object()) {
                for (const char* k : {"guid", "type", "metaVersion", "displayName"}) fields.erase(k);
                if (!fields.empty()) AssetDatabase::MergeMetaFields(newPath, fields.dump());
            }
        }

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
        if (!registered) for (const auto& mat : assets.Materials()) { // #129 - .mat copies were never registered
            if (mat->Path == key) { registered = assets.LoadMaterial(newPath) != nullptr; break; }
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
    // Strip control/illegal-filename characters and reserved device names, trim, and cap length
    // (#38 B12) — these names get '/'-joined into virtual folder paths, so an un-sanitized '/'
    // or '\' would silently rewrite the hierarchy instead of just looking odd in a label.
    std::string newName = SanitizeAssetName(m_RenameBuffer);
    if (newName.empty()) {
        // Nothing usable survived sanitization (blank, all-illegal, or a reserved name like
        // "CON") - reject instead of silently accepting garbage or silently discarding the
        // edit. Leave the field open with the raw text still in it so the user can fix it, and
        // flash an inline error instead.
        m_RenameRejectedFlash = 1.6f;
        return;
    }
    if (m_RenamingIsFolder) {
        PushUndo(world, "Rename Folder");
        std::string parent = ParentFolderOf(m_RenamingAssetKey);
        std::string newPath = parent.empty() ? newName : (parent + "/" + newName);
        assets.RenameFolder(m_RenamingAssetKey, newPath);
        // Path-patching, not navigation (Phase 5 item 3's NavigateAssetFolder) - the user hasn't
        // gone anywhere, the folder they were already in just changed name out from under them.
        // Patch every history entry that pointed into the renamed subtree too, or Back could walk
        // to a path that no longer exists.
        auto patchRenamedPath = [&](std::string& path) {
            if (path == m_RenamingAssetKey) path = newPath;
            else if (path.rfind(m_RenamingAssetKey + "/", 0) == 0)
                path = newPath + path.substr(m_RenamingAssetKey.size());
        };
        patchRenamedPath(m_CurrentAssetFolder);
        for (std::string& histPath : m_AssetFolderHistory) patchRenamedPath(histPath);
        if (m_SelectedAssetKey == m_RenamingAssetKey) m_SelectedAssetKey = newPath;
    } else {
        // #129 - a real file is renamed on disk (keeping its extension, .meta and GUID); only
        // things with no file of their own (built-in primitives) fall back to a display name.
        const std::filesystem::path oldP(m_RenamingAssetKey);
        std::error_code ec;
        const std::string oldAbs = oldP.is_absolute() ? m_RenamingAssetKey : ProjectPaths::Resolve(m_RenamingAssetKey);
        if (!AssetDatabase::IsSynthetic(m_RenamingAssetKey) && std::filesystem::is_regular_file(oldAbs, ec)) {
            const std::string newKey = (oldP.parent_path() / (newName + oldP.extension().string())).generic_string();
            if (newKey != oldP.generic_string()) {
                std::string why;
                if (!RenameAssetFile(world, assets, m_RenamingAssetKey, newKey, why)) {
                    Log::Error("Couldn't rename '" + oldP.filename().string() + "': " + why + ".");
                    m_RenameRejectedFlash = 1.6f;
                    return;
                }
                if (m_SelectedAssetKey == m_RenamingAssetKey) m_SelectedAssetKey = newKey;
                for (auto& e : m_ExtraAssetSelection) if (e.Key == m_RenamingAssetKey) e.Key = newKey;
                m_RenamingAssetKey = newKey;
            }
            assets.SetDisplayName(m_RenamingAssetKey, ""); // the file name is the name now
        } else {
            PushUndo(world, "Rename Asset");
            assets.SetDisplayName(m_RenamingAssetKey, newName);
        }
    }
    m_RenamingAssetKey.clear();
    m_RenameRejectedFlash = 0.0f;
}

// Inline error for a rejected rename (#38 B12) - a tooltip pinned near the still-open rename
// field, decaying on its own, rather than a modal the user has to dismiss. Both the grid and
// list rename layouts call this right after their InputText.
void EditorLayer::DrawRenameRejectedTooltip(ImVec2 fieldMin, ImVec2 fieldMax) {
    if (m_RenameRejectedFlash <= 0.0f) return;
    m_RenameRejectedFlash -= ImGui::GetIO().DeltaTime;
    // Pinned just below the rename field itself, matching the comment above — a bare
    // BeginTooltip() with no explicit position follows the mouse cursor instead, which drifts
    // away from the field the moment the user isn't actively hovering it (e.g. right after the
    // Enter key submitted the rejected name).
    ImGui::SetNextWindowPos(ImVec2(fieldMin.x, fieldMax.y + 4.0f));
    ImGui::BeginTooltip();
    ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
        "Name can't be blank, only illegal characters (< > : \" / \\ | ? *), or a reserved name like CON/NUL.");
    ImGui::EndTooltip();
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

// Phase 5 item 4 — the three text columns Details view appends after a list row's name. Mirrors
// the Explorer/Finder convention of leaving Size blank for folders (a recursive folder size isn't
// worth a stat-per-subfile here) rather than showing 0.
const char* AssetKindLabel(Cell::Kind k) {
    switch (k) {
        case Cell::Kind::Folder:     return "Folder";
        case Cell::Kind::Scene:      return "Scene";
        case Cell::Kind::Prefab:     return "Prefab";
        case Cell::Kind::Model:      return "Model";
        case Cell::Kind::Texture:    return "Texture";
        case Cell::Kind::Material:   return "Material";
        case Cell::Kind::Sound:      return "Sound";
        case Cell::Kind::Screenshot: return "Screenshot";
        case Cell::Kind::Shader:     return "Shader";
    }
    return "";
}

std::string FormatFileSize(unsigned long long bytes) {
    static const char* kUnits[] = { "B", "KB", "MB", "GB" };
    double v = (double)bytes;
    int unit = 0;
    while (v >= 1024.0 && unit < 3) { v /= 1024.0; ++unit; }
    char buf[32];
    snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", v, kUnits[unit]);
    return buf;
}

std::string FormatModifiedTime(const std::filesystem::file_time_type& t) {
    // file_clock -> system_clock (C++20's clock_cast isn't available pre-20 here) via the
    // duration-since-epoch difference between the two clocks' "now", same trick used wherever
    // this codebase already bridges the two (kept local since it's only needed for this column).
    const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
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
    InvalidateShadersListing();
    InvalidateModelThumbnail();          // clears every cached model thumbnail
    m_ShotThumbs.clear();                // shared_ptr<Texture> entries free their GL textures here
    m_AssetListingRefreshTimer = 0.0f;
    m_AssetRefreshFlash = 1.6f; // drives the module's brief "Assets refreshed" confirmation
    Log::Info("Asset Browser refreshed - re-scanned scenes/, screenshots/ and shaders/, dropped thumbnail caches.");
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

// Same idea for project/shaders/ (Phase 6 item 15's browsable Shaders folder) — see
// RefreshScenesListingIfNeeded above.
void EditorLayer::RefreshShadersListingIfNeeded() {
    if (m_ShadersListingCache.valid) return;
    m_ShadersListingCache.paths.clear();
    std::error_code ec;
    const std::string shadersDir = ProjectPaths::Resolve("shaders");
    std::filesystem::create_directory(shadersDir, ec);
    for (const auto& entry : std::filesystem::directory_iterator(shadersDir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".shader") continue;
        m_ShadersListingCache.paths.push_back(entry.path().generic_string());
    }
    // #104 — plus the engine's built-in descriptors (Standard.shader), which materials reference
    // as engine://Name.shader; listed after the project's own, read-only.
    std::error_code engineEc;
    for (const auto& entry : std::filesystem::directory_iterator(ShaderLibrary::Dir(), engineEc)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".shader") continue;
        m_ShadersListingCache.paths.push_back(entry.path().generic_string());
    }
    m_ShadersListingCache.valid = true;
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

// kind 0/1/2 = model / texture / sound; 3 = any (Phase 5 item 5's unified "Import Asset..." —
// ImportDroppedFile already infers the real type from the extension regardless of which kind
// picked the dialog's filter, so kind only ever chooses what the OS file picker shows/defaults
// to). Opens the OS file dialog, then imports into the current folder — the host half of the
// module's +Create/Import menu.
void EditorLayer::AssetBrowserImportViaDialog(World& world, AssetLibrary& assets, int kind, const std::string& intoFolder) {
    const char* filter =
        kind == 0 ? "3D Models\0*.fbx;*.obj;*.gltf;*.glb\0All Files\0*.*\0" :
        kind == 1 ? "Images\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All Files\0*.*\0" :
        kind == 2 ? "Audio\0*.wav;*.mp3;*.ogg;*.flac\0All Files\0*.*\0" :
                    "All Assets\0*.fbx;*.obj;*.gltf;*.glb;*.png;*.jpg;*.jpeg;*.tga;*.bmp;"
                    "*.wav;*.mp3;*.ogg;*.flac\0All Files\0*.*\0";
    if (!m_EditorCameraPtr) return;
    for (const std::string& p : FileDialog::OpenFiles(filter, m_Window)) // #139 — multi-select
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

namespace {
// True if `path` names an existing entry inside directory `dir`. Used below to recognise the two
// raw filesystem-backed virtual folders (Screenshots/Scenes) that AssetFolder() doesn't know about.
bool IsUnder(const std::string& path, const std::string& dir) {
    std::error_code ec;
    auto rel = std::filesystem::relative(path, dir, ec);
    return !ec && rel.string().rfind("..", 0) != 0;
}
} // namespace

// Phase 6 item 4 — Console click-to-navigate's asset half (see the header comment). Unlike the
// Inspector's ping button, `path` here comes from parsed log-message text, so it might not be a
// real path at all — the existence check is load-bearing, not a formality.
bool EditorLayer::PingAssetPath(AssetLibrary& assets, const std::string& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) return false;

    // Screenshots and Scenes are raw filesystem-backed virtual folders (RefreshShotsListingIfNeeded
    // / RefreshScenesListingIfNeeded above) rather than AssetLibrary entries, so AssetFolder() would
    // return "" (root) for them — check those two directories first, same as the grid does.
    std::string folder;
    if (IsUnder(path, Screenshot::Dir())) folder = "Screenshots";
    else if (IsUnder(path, ProjectPaths::Resolve("scenes"))) folder = "Scenes";
    else if (IsUnder(path, ProjectPaths::Resolve("shaders"))) folder = "Shaders";
    else folder = assets.AssetFolder(path);

    m_SelectedAssetKey = path;
    m_SelectedAssetIsFolder = false;
    NavigateAssetFolder(folder);
    return true;
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

    // Phase 6 item 15 — a "Shaders" folder listing every *.shader under project/shaders/, so
    // shaders are browsable the same way scenes are, instead of invisible to the Asset Browser.
    static const std::string kShadersFolder = "Shaders";
    assets.CreateFolder(kShadersFolder);
    if ((filtering && inSearchScope(kShadersFolder)) || m_CurrentAssetFolder == kShadersFolder) {
        RefreshShadersListingIfNeeded();
        for (const auto& path : m_ShadersListingCache.paths) {
            std::string name = std::filesystem::path(path).stem().string();
            if (!MatchesAssetSearch(parsedSearch, name, "shader", {})) continue;
            if (IsBuiltinShaderPath(path)) name += " (built-in)";
            cells.push_back({Cell::Kind::Shader, path, name, nullptr, nullptr});
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
            // A screenshot is an image: both t:Screenshot and t:Texture find it (#184).
            if (!MatchesAssetSearch(parsedSearch, name, "screenshot", {}) &&
                !MatchesAssetSearch(parsedSearch, name, "texture", {})) continue;
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
                case Cell::Kind::Shader:     return 8;
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
            : cell.kind == Cell::Kind::Shader ? ICON_FA_FILE_CODE
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
                // NoNav (Defect #43): the host's Enter/Backspace/Left/Right folder-traversal
                // block drives selection off m_CurrentAssetFolder directly — ImGui's own
                // keyboard nav must not also move focus between tiles on the same keys.
                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                clicked = ImGui::Selectable("##tile", isSelected, ImGuiSelectableFlags_None, tileSize);
                ImGui::PopItemFlag();
            } else {
                ImGui::Dummy(tileSize);
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
            unsigned int modelThumb = (cell.kind == Cell::Kind::Model) ? ModelThumbnail(*cell.model)
                                    : (cell.kind == Cell::Kind::Material) ? MaterialThumbnail(cell.material) : 0u; // #107
            if (cell.texture && (cell.kind == Cell::Kind::Texture || cell.kind == Cell::Kind::Screenshot)) {
                float aspect = cell.texture->Height() > 0 ? (float)cell.texture->Width() / (float)cell.texture->Height() : 1.0f;
                ImVec2 imgSize = aspect >= 1.0f ? ImVec2(m_AssetIconSize, m_AssetIconSize / aspect) : ImVec2(m_AssetIconSize * aspect, m_AssetIconSize);
                ImVec2 imgPos(tileMin.x + (cellWidth - imgSize.x) * 0.5f, tileMin.y + cellPadding * 0.5f + (m_AssetIconSize - imgSize.y) * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)cell.texture->GLHandle(), imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y));
            } else if (modelThumb) {
                ImVec2 imgSize(m_AssetIconSize, m_AssetIconSize);
                ImVec2 imgPos(tileMin.x + (cellWidth - imgSize.x) * 0.5f, tileMin.y + cellPadding * 0.5f);
                // GL render targets are bottom-up: flip V so the preview is upright.
                dl->AddImage((ImTextureID)(intptr_t)modelThumb, imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y),
                             ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
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
                // Phase 5 item 4 — two lines instead of one, so a longer name (a common case once
                // the browser is re-rooted on the real project tree) doesn't truncate as eagerly.
                ImVec2 labelPos(tileMin.x + 3.0f, tileMin.y + m_AssetIconSize + cellPadding);
                bool truncated = DrawClampedGridLabel(dl, labelPos, cellWidth - 6.0f,
                    ImGui::GetTextLineHeightWithSpacing(), textColor, cell.display.c_str(), /*maxLines=*/2);
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
                const ImVec2 renameFieldMin = ImGui::GetItemRectMin();
                const ImVec2 renameFieldMax = ImGui::GetItemRectMax();
                bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
                bool lostFocus = ImGui::IsItemDeactivated() && !done;
                if (done) CommitRename(world, assets);
                else if (cancel || lostFocus) { m_RenamingAssetKey.clear(); m_RenameRejectedFlash = 0.0f; }
                DrawRenameRejectedTooltip(renameFieldMin, renameFieldMax);
                ImGui::SetCursorScreenPos(ImVec2(tileMin.x, tileMin.y + tileSize.y));
            }
        } else {
            // List mode: little icon (real thumbnail for textures, a Font Awesome glyph
            // otherwise) followed by the name, mirroring how the Scene Hierarchy lists rows.
            float rowIconSize = ImGui::GetTextLineHeight();
            unsigned int rowModelThumb = (cell.kind == Cell::Kind::Model) ? ModelThumbnail(*cell.model)
                                       : (cell.kind == Cell::Kind::Material) ? MaterialThumbnail(cell.material) : 0u; // #107
            if (cell.texture && (cell.kind == Cell::Kind::Texture || cell.kind == Cell::Kind::Screenshot)) {
                ImGui::Image((ImTextureID)(intptr_t)cell.texture->GLHandle(), ImVec2(rowIconSize, rowIconSize));
            } else if (rowModelThumb) {
                ImGui::Image((ImTextureID)(intptr_t)rowModelThumb, ImVec2(rowIconSize, rowIconSize), ImVec2(0, 1), ImVec2(1, 0));
            } else {
                ImGui::TextUnformatted(icon);
            }
            ImGui::SameLine();

            // Phase 5 item 4 — Details view appends Type/Size/Modified after the name, all within
            // this same row/Selectable so click/drag/rename/context-menu below stay untouched.
            const bool detailsMode = !isRenaming && EditorSettings::Get().AssetDetailsMode;
            const float rightEdge = ImGui::GetWindowContentRegionMax().x;
            const float reservedW = (kAssetDetailsTypeColW + kAssetDetailsSizeColW + kAssetDetailsModifiedColW) * m_UIScale;
            const float nameW = detailsMode
                ? std::max(40.0f * m_UIScale, (rightEdge - ImGui::GetCursorPosX()) - reservedW) : 0.0f;

            if (isRenaming) {
                ImGui::SetNextItemWidth(-1);
                if (m_RenamingJustStarted) {
                    ImGui::SetKeyboardFocusHere();
                    m_RenamingJustStarted = false;
                }
                bool done = ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                const ImVec2 renameFieldMin = ImGui::GetItemRectMin();
                const ImVec2 renameFieldMax = ImGui::GetItemRectMax();
                bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
                bool lostFocus = ImGui::IsItemDeactivated() && !done;
                if (done) CommitRename(world, assets);
                else if (cancel || lostFocus) { m_RenamingAssetKey.clear(); m_RenameRejectedFlash = 0.0f; }
                DrawRenameRejectedTooltip(renameFieldMin, renameFieldMax);
            } else {
                // NoNav (Defect #43): same reasoning as the grid tile above.
                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                clicked = ImGui::Selectable(cell.display.c_str(), isSelected, ImGuiSelectableFlags_None,
                    detailsMode ? ImVec2(nameW, 0.0f) : ImVec2(0.0f, 0.0f));
                ImGui::PopItemFlag();

                if (detailsMode) {
                    const float typeX = rightEdge - (kAssetDetailsTypeColW + kAssetDetailsSizeColW + kAssetDetailsModifiedColW) * m_UIScale;
                    const float sizeX = rightEdge - (kAssetDetailsSizeColW + kAssetDetailsModifiedColW) * m_UIScale;
                    const float modX  = rightEdge - kAssetDetailsModifiedColW * m_UIScale;

                    std::error_code ec;
                    std::string sizeStr = "-", modStr = "-";
                    if (!isFolder) {
                        const std::filesystem::path p(cell.key);
                        if (std::filesystem::exists(p, ec)) {
                            const auto sz = std::filesystem::file_size(p, ec);
                            if (!ec) sizeStr = FormatFileSize(sz);
                            const auto mt = std::filesystem::last_write_time(p, ec);
                            if (!ec) modStr = FormatModifiedTime(mt);
                        }
                    }

                    ImGui::SameLine(typeX); ImGui::TextDisabled("%s", AssetKindLabel(cell.kind));
                    ImGui::SameLine(sizeX); ImGui::TextDisabled("%s", sizeStr.c_str());
                    ImGui::SameLine(modX);  ImGui::TextDisabled("%s", modStr.c_str());
                }
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
            NavigateAssetFolder(cell.key);
        }
        if (cell.kind == Cell::Kind::Scene && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            RequestOpenScene(world, assets, cell.key);
        }
        if (cell.kind == Cell::Kind::Screenshot && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            OpenScreenshotPreview(cell.key); // centred in-editor lightbox (Show in folder is on the context menu)
        }
        if (cell.kind == Cell::Kind::Shader && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            Screenshot::OpenFile(cell.key); // item 15: browsable + externally-openable, no in-editor text editor
        }
        // #176 - double-click opens the prefab in Prefab Mode, like Unity (drag it into the scene,
        // or right-click > Place Instance, to place one).
        if (cell.kind == Cell::Kind::Prefab && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            EnterPrefabMode(world, assets, cell.key);
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
                // #184 — materials are foldered by AssetFolder like the kinds above but weren't accepted.
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_MATERIAL_PATH")) {
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
        } else if (cell.kind != Cell::Kind::Scene && cell.kind != Cell::Kind::Screenshot &&
                   cell.kind != Cell::Kind::Shader) { // not placeable — nothing to drag into the viewport
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
            } else if (cell.kind == Cell::Kind::Screenshot) {
                // Defect #17 — this used to fall into the folder-oriented default below ("Drag
                // assets onto it to file them here"), which only makes sense for an actual folder.
                EditorUI::SetTooltip("%s\n\nDouble-click to preview.", cell.display.c_str());
            } else if (cell.kind == Cell::Kind::Shader) {
                EditorUI::SetTooltip("%s\n\nDouble-click to open externally.\nSelect to preview + compile status in the Inspector.",
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
                        (allFav ? "Remove from Favorites" : "Add to Favorites"); // #19 — en-US, matches IsAssetFavorite/SetAssetFavorites
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
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
                if (ImGui::MenuItem(multi ? ICON_FA_TRASH "  Delete Selected" : ICON_FA_TRASH "  Delete",
                                    nullptr, false, !(!multi && anyOpen)))
                    RequestDeleteAssets(world, assets, scenesForAction, /*skipDialog=*/false);
                ImGui::PopStyleColor();
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
                        (allFav ? "Remove from Favorites" : "Add to Favorites"); // #19 — en-US, matches IsAssetFavorite/SetAssetFavorites
                    if (favKeys.size() > 1) lbl += " (" + std::to_string(favKeys.size()) + ")";
                    if (ImGui::MenuItem(lbl.c_str())) SetAssetFavorites(favKeys, !allFav);
                }
                std::vector<AssetKeyRef> shotsForAction;
                shotsForAction.push_back({m_SelectedAssetKey, false});
                for (const auto& e : m_ExtraAssetSelection) shotsForAction.push_back(e);
                const bool multi = shotsForAction.size() > 1;
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
                if (ImGui::MenuItem(multi ? ICON_FA_TRASH "  Delete Selected" : ICON_FA_TRASH "  Delete"))
                    RequestDeleteAssets(world, assets, shotsForAction, /*skipDialog=*/false);
                ImGui::PopStyleColor();
                ImGui::EndPopup();
            }
        } else if (cell.kind == Cell::Kind::Shader) {
            // Also a real file on disk, not a library asset (item 15): Open externally, Show in
            // folder, Copy Path, Delete — no favorites/rename, this isn't a placeable/reference
            // asset the rest of the browser's machinery (usages, drag-onto-model, etc.) applies to.
            if (ImGui::BeginPopupContextItem()) {
                if (!IsAssetSelected(cell.key, false)) {
                    ClearAssetSelection();
                    m_SelectedAssetKey = cell.key;
                    m_SelectedAssetIsFolder = false;
                }
                if (ImGui::MenuItem(ICON_FA_UP_RIGHT_FROM_SQUARE "  Open Externally")) Screenshot::OpenFile(cell.key);
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show in folder")) Screenshot::ShowInFolder(cell.key);
                if (m_ExtraAssetSelection.empty() && ImGui::MenuItem(ICON_FA_COPY "  Copy Path")) {
                    ImGui::SetClipboardText(cell.key.c_str());
                    Log::Info("Copied path: " + cell.key);
                }
                std::vector<AssetKeyRef> shadersForAction;
                shadersForAction.push_back({m_SelectedAssetKey, false});
                for (const auto& e : m_ExtraAssetSelection) shadersForAction.push_back(e);
                const bool multi = shadersForAction.size() > 1;
                if (!IsBuiltinShaderPath(cell.key) || multi) {
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
                    if (ImGui::MenuItem(multi ? ICON_FA_TRASH "  Delete Selected" : ICON_FA_TRASH "  Delete"))
                        RequestDeleteAssets(world, assets, shadersForAction, /*skipDialog=*/false);
                    ImGui::PopStyleColor();
                }
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
            ImGui::SeparatorText(ICON_FA_PEN "  Edit");
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
                ImGui::SetNextItemWidth(240.0f * m_UIScale); // #37
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
                    (allFav ? "Remove from Favorites" : "Add to Favorites"); // #19 — en-US, matches IsAssetFavorite/SetAssetFavorites
                if (favKeys.size() > 1) lbl += " (" + std::to_string(favKeys.size()) + ")";
                if (ImGui::MenuItem(lbl.c_str())) SetAssetFavorites(favKeys, !allFav);
            }

            // #178 - Unity's Find References In Scene: selects every object in the open scene that
            // uses this asset (as its mesh, a material slot, a texture in any of its materials,
            // its sound, or its prefab source).
            if (!isFolder && m_ExtraAssetSelection.empty() && ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Find References in Scene")) {
                auto same = [](const std::string& a, const std::string& b) {
                    if (a.empty() || b.empty()) return false;
                    std::error_code ec;
                    const auto na = std::filesystem::weakly_canonical(std::filesystem::path(a), ec);
                    const auto nb = std::filesystem::weakly_canonical(std::filesystem::path(b), ec);
                    std::string sa = na.generic_string(), sb = nb.generic_string();
                    std::transform(sa.begin(), sa.end(), sa.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    std::transform(sb.begin(), sb.end(), sb.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    return sa == sb;
                };
                auto usesTexture = [&](const Material& m) {
                    for (const auto* t : {&m.AlbedoMap, &m.NormalMap, &m.MetallicRoughnessMap, &m.MetallicMap,
                                          &m.RoughnessMap, &m.AOMap, &m.EmissiveMap, &m.ClearCoatMap,
                                          &m.ThicknessMap, &m.HeightMap, &m.DetailAlbedoMap, &m.DetailNormalMap})
                        if (*t && same((*t)->Path(), cell.key)) return true;
                    for (const auto& [name, prop] : m.ExtraProps)
                        if ((prop.Tex && same(prop.Tex->Path(), cell.key)) || same(prop.TexPath, cell.key)) return true;
                    return false;
                };
                std::vector<entt::entity> hits;
                for (auto [e, name] : world.Registry.view<const NameComponent>().each()) {
                    (void)name;
                    bool uses = false;
                    if (const auto* rc = world.Registry.try_get<RenderableComponent>(e); rc && rc->ModelRef) {
                        uses = same(rc->ModelRef->Path(), cell.key);
                        for (int i = 0; !uses && i < rc->ModelRef->MeshCount(); ++i) {
                            const bool hasSlot = i < (int)rc->Materials.size() && rc->Materials[i];
                            if (hasSlot && same(rc->Materials[i]->Path, cell.key)) uses = true;
                            else uses = usesTexture(hasSlot ? rc->Materials[i]->Mat : rc->ModelRef->MeshMaterial(i));
                        }
                    }
                    if (!uses) if (const auto* a = world.Registry.try_get<AudioSourceComponent>(e)) uses = same(a->SoundPath, cell.key);
                    if (!uses) if (const auto* p = world.Registry.try_get<PrefabInstanceComponent>(e)) uses = same(p->SourcePath, cell.key);
                    if (uses) hits.push_back(e);
                }
                ClearSelection();
                for (entt::entity e : hits) AddToSelectionIfAbsent(e);
                if (hits.empty()) Log::Info("No objects in this scene use '" + cell.display + "'.");
                else Log::Info("Selected " + std::to_string(hits.size()) + " object(s) using '" + cell.display + "'.");
            }

            if (!isFolder && m_ExtraAssetSelection.empty() && ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Find References in Project")) {
                const std::vector<std::string> refs = FindProjectReferences(cell.key);
                if (refs.empty()) {
                    Log::Info("No scene, prefab, material or controller in the project references '" + cell.display + "'.");
                } else {
                    Log::Info(std::to_string(refs.size()) + " file(s) reference '" + cell.display + "' (double-click one to find it):");
                    for (const std::string& r : refs)
                        Log::Info("    " + r, LogContext::Asset(ProjectPaths::Resolve(r)));
                }
            }
            // Reimport straight from the context menu instead of only via Import Settings >
            // Apply (#28 P17). Single selection, real imported assets only.
            if (!isFolder && m_ExtraAssetSelection.empty() &&
                (cell.kind == Cell::Kind::Model || cell.kind == Cell::Kind::Texture)) {
                ImGui::SeparatorText(ICON_FA_ROTATE "  Actions");
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
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
                if (ImGui::MenuItem(deleteLabel)) {
                    RequestDeleteAssets(world, assets, selectionForAction, /*skipDialog=*/false);
                }
                ImGui::PopStyleColor();
            } else {
                if (cell.kind == Cell::Kind::Prefab && selectionForAction.size() == 1 && ImGui::MenuItem(ICON_FA_PEN_TO_SQUARE "  Open Prefab")) {
                    EnterPrefabMode(world, assets, cell.key); // #176
                }
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
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
                if (ImGui::MenuItem(removeLabel)) {
                    RequestDeleteAssets(world, assets, selectionForAction, /*skipDialog=*/false);
                }
                ImGui::PopStyleColor();
                if (cell.kind == Cell::Kind::Prefab) ImGui::TextDisabled("The .prefab file stays on disk.");
                // #129 - Unity's Delete: the file and its .meta go to the Recycle Bin (project files only).
                std::vector<std::string> onDisk;
                for (const auto& item : selectionForAction)
                    if (!item.IsFolder && !IsBuiltinShaderPath(item.Key) && IsProjectFile(item.Key)) onDisk.push_back(item.Key);
                ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
                if (!onDisk.empty() &&
                    ImGui::MenuItem(onDisk.size() > 1 ? ICON_FA_TRASH "  Delete Selected from Disk..." : ICON_FA_TRASH "  Delete from Disk...")) {
                    m_PendingDiskDeleteRefs.clear();
                    for (const std::string& k : onDisk)
                        for (std::string& r : FindProjectReferences(k))
                            if (std::find(m_PendingDiskDeleteRefs.begin(), m_PendingDiskDeleteRefs.end(), r) == m_PendingDiskDeleteRefs.end())
                                m_PendingDiskDeleteRefs.push_back(std::move(r));
                    m_PendingDiskDelete = onDisk;
                    m_OpenDiskDeleteRequested = true;
                }
                ImGui::PopStyleColor();
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
            // #87 — the file used to be built from the VIRTUAL folder name with no root, so it
            // landed relative to the process CWD (usually build/Release/, wiped by a clean
            // build). Write it under the project's materials/ folder, then file it into the
            // Asset Browser folder the user is looking at.
            const std::string folder = m_CurrentAssetFolder;
            std::error_code ec;
            const std::filesystem::path dir = ProjectPaths::Resolve("materials");
            std::filesystem::create_directories(dir, ec);
            std::filesystem::path candidatePath = dir / "New Material.mat";
            for (int n = 1; std::filesystem::exists(candidatePath, ec); ++n)
                candidatePath = dir / ("New Material (" + std::to_string(n) + ").mat");
            const std::string candidate = candidatePath.generic_string();
            PushUndo(world, "Create Material");
            auto mat = MaterialAsset::CreateDefault(candidate);
            if (mat) {
                assets.LoadMaterial(candidate);
                if (!folder.empty()) assets.SetAssetFolder(candidate, folder);
                BeginRenameAsset(candidate, false, std::filesystem::path(candidate).stem().string());
            } else {
                Log::Error("Create Material: couldn't write '" + candidate + "'.");
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

// Phase 5 item 4 — Grid/List (0/1) is still purely icon-size-derived (unchanged from item 3's
// remainder); Details (2) is a separate persisted bool layered on top, so the icon-size slider
// keeps meaning exactly what it always did within Grid/List and Details doesn't disturb it.
int EditorLayer::GetAssetViewMode() const {
    if (EditorSettings::Get().AssetDetailsMode) return 2;
    return (m_AssetIconSize > kListViewIconSize * m_UIScale) ? 0 : 1;
}

void EditorLayer::ToggleAssetViewMode() {
    if (EditorSettings::Get().AssetDetailsMode) {
        // Details -> Grid, restoring whatever zoom Grid was left at.
        EditorSettings::Get().AssetDetailsMode = false;
        EditorSettings::Save();
        const float restore = m_AssetGridIconSizeMemory > kListViewIconSize * m_UIScale
            ? m_AssetGridIconSizeMemory : 64.0f * m_UIScale;
        SetAssetIconSize(restore, /*commit=*/true);
        return;
    }
    const bool gridMode = m_AssetIconSize > kListViewIconSize * m_UIScale;
    if (gridMode) {
        // Grid -> List.
        m_AssetGridIconSizeMemory = m_AssetIconSize;
        SetAssetIconSize(kListViewIconSize * m_UIScale, /*commit=*/true);
    } else {
        // List -> Details.
        EditorSettings::Get().AssetDetailsMode = true;
        EditorSettings::Save();
    }
}
