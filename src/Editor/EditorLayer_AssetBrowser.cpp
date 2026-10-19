// Asset Browser: the folder tree, the asset grid and its search/filter parser, model
// thumbnails, and asset delete/rename/duplicate. Split out of EditorLayer.cpp (#179).

#include "EditorLayer.h"
#include "EditorTheme.h"
#include "Glass/GlassDragMotion.h" // the drag ghost
#include "Glass/GlassMotion.h"     // Motion v2: folder zoom, grid ripples, drag-fling
#include "EditorUIPrimitives.h"
#include "EditorSound.h"
#include "EditorLayerInternal.h"
#include "FileDialog.h"
#include "AssetLibrary.h"
#include "MaterialAsset.h"
#include "World.h"
#include "Camera.h"
#include "Model.h"
#include "FirstPersonWeaponWizard.h"
#include "FirstPersonBodyContract.h"
#include "Texture.h"
#include "Material.h"
#include "AudioEngine.h"
#include "OsShell.h"
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
#include "Enhancers/EnhancerUserState.h" // vFavorites pages back the favourite stars
#include "AtomicFile.h"
#include "ThumbnailCache.h"
#include "ComponentReflection.h" // ReflectAssetKind - AssetRefChoices
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
#include <unordered_set>
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
                                         // "sound","scene","prefab","folder"
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
// Since vFavorites, a favourite asset is one on any favorites page (EnhancerUserState, journaled
// so starring is undoable). The old per-user asset_favorites.json is imported into page 1 once
// (MigrateAssetFavorites) and no longer written.
namespace {
std::string AssetLeafName(const std::string& key) {
    const size_t slash = key.find_last_of("/\\");
    return slash == std::string::npos ? key : key.substr(slash + 1);
}
} // namespace

void EditorLayer::RefreshFavoriteCache() const {
    const auto& us = Enhancers::EnhancerUserState::Get();
    if (m_FavCacheRevision == us.Revision()) return;
    m_FavCacheRevision = us.Revision();
    m_AssetFavorites.clear();
    for (const auto& page : us.FavoritePages)
        for (const auto& r : page.Items)
            if (r.Kind == Enhancers::RefKind::Asset) m_AssetFavorites.insert(r.Path);
}

void EditorLayer::MigrateAssetFavorites() {
    auto& us = Enhancers::EnhancerUserState::Get();
    if (us.FavoritesMigrated) return;
    std::set<std::string> old;
    std::ifstream in(UserPaths::Resolve("asset_favorites.json"));
    if (!in.is_open()) in.open(ProjectPaths::Resolve("asset_favorites.json"));
    if (in.is_open()) {
        try {
            nlohmann::json root; in >> root;
            if (root.is_array())
                for (const auto& v : root) if (v.is_string()) old.insert(v.get<std::string>());
        } catch (const std::exception& e) {
            Log::Warn(std::string("Asset favorites: failed to parse: ") + e.what()); // #19
        }
    }
    Enhancers::EnsureFavoritePage(us.FavoritePages);
    for (const auto& k : old)
        if (!k.empty()) Enhancers::AddFavorite(us.FavoritePages, 0, Enhancers::EditorRef::MakeAsset(k, AssetLeafName(k)));
    us.FavoritesMigrated = true;
    us.MarkDirty();
    if (!old.empty()) Log::Info("Favorites: moved " + std::to_string(old.size()) + " starred assets to the first favorites page (hold Alt over the Asset Browser).");
    // Written now, at startup, so the import isn't recorded as a first undo step.
    if (!m_Headless) us.Flush();
}

void EditorLayer::PlayAssetFolderSound(bool history) const {
    if (ImGui::GetCurrentContext() && ImGui::GetFrameCount() < 30) return; // start-up restores (folder tabs) aren't the user's
    EditorSound::Play(history ? EditorSound::Event::FolderBack : EditorSound::Event::FolderEnter);
}

void EditorLayer::ToggleAssetFavorite(const std::string& key) {
    if (key.empty()) return;
    SetAssetFavorites({key}, !IsAssetFavorite(key));
}

void EditorLayer::SetAssetFavorites(const std::vector<std::string>& keys, bool on) {
    auto& us = Enhancers::EnhancerUserState::Get();
    bool changed = false;
    for (const std::string& k : keys) {
        if (k.empty()) continue;
        const Enhancers::EditorRef r = Enhancers::EditorRef::MakeAsset(k, AssetLeafName(k));
        if (on) { if (Enhancers::FindFavorite(us.FavoritePages, r) < 0) changed |= Enhancers::AddFavorite(us.FavoritePages, m_FavPage, r); }
        else changed |= Enhancers::RemoveFavorite(us.FavoritePages, r);
    }
    if (!changed) return;
    us.MarkDirty();
    EditorSound::Play(on ? EditorSound::Event::FavoriteOn : EditorSound::Event::FavoriteOff);
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
    // The model's material remap (its .mat per imported material) is what placing it uses, so the
    // thumbnail is drawn with it too - and keyed on those .mat files.
    std::map<std::string, std::string> remap;
    if (m_AssetsPtr) remap = m_AssetsPtr->MaterialRemap(path);
    for (const auto& [slotName, matPath] : remap) { (void)slotName; deps.push_back(ProjectPaths::Resolve(matPath)); }
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
        std::vector<std::shared_ptr<MaterialAsset>> slots;
        if (m_AssetsPtr && !remap.empty()) m_AssetsPtr->ApplyMaterialRemap(model, slots);
        const unsigned int src = m_ThumbnailPreview.Render(model, 0.7f, 0.5f, dist, kSize, kSize, slots);

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
            if (evictIt->second.first) m_RetiredThumbnailTextures.push_back(evictIt->second.first);
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
        for (auto& [p, entry] : m_ModelThumbnails) { (void)p; if (entry.first) m_RetiredThumbnailTextures.push_back(entry.first); }
        m_ModelThumbnails.clear();
        m_ThumbnailLRU.clear();
        return;
    }
    auto it = m_ModelThumbnails.find(path);
    if (it == m_ModelThumbnails.end()) return;
    if (it->second.first) m_RetiredThumbnailTextures.push_back(it->second.first);
    m_ThumbnailLRU.erase(it->second.second);
    m_ModelThumbnails.erase(it);
}
void EditorLayer::FreeRetiredThumbnailTextures() {
    if (m_RetiredThumbnailTextures.empty()) return;
    glDeleteTextures((GLsizei)m_RetiredThumbnailTextures.size(), m_RetiredThumbnailTextures.data());
    m_RetiredThumbnailTextures.clear();
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
        if (ext != ".json" && ext != ".prefab" && ext != ".mat" && ext != ".controller" && ext != ".fpsanim" && ext != ".physicmaterial") continue;
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
            ImGui::TextColored(EditorTheme::Warning,
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
        bool deleteClicked;
        { EditorSoundEvents::ScopedClickSilence quiet; deleteClicked = PrimaryButton("Delete", ImVec2(buttonWidth, 0.0f)); } // PushUndo plays Delete
        if (deleteClicked || keyConfirm) {
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
        bool diskDeleteClicked;
        { EditorSoundEvents::ScopedClickSilence quiet; diskDeleteClicked = PrimaryButton("Delete", ImVec2(bw, 0.0f)); }
        if (diskDeleteClicked) {
            m_DeleteError.clear();
            EditorSound::Play(EditorSound::Event::Delete); // no undo step here to play it
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
        AtomicFile::NotifyWillChange(candidate);
        std::filesystem::copy_file(srcPath, candidate, copyErr);
        if (copyErr) {
            Log::Error("Failed to duplicate '" + key + "': " + copyErr.message());
            return {};
        }

        // (#175) A duplicated Scene (not an AssetLibrary entry — see the "registered" checks
        // below) still just copied a real file into scenes/,
        // so the corresponding cached directory listing needs invalidating same as any other
        // create. Harmless if the extension happens to belong to something else entirely.
        std::string dupExt = candidate.extension().string();
        for (char& c : dupExt) c = (char)tolower((unsigned char)c);
        if (dupExt == ".json") InvalidateScenesListing();
        else if (dupExt == ".controller" || dupExt == ".fpsanim" || dupExt == ".recoil" || dupExt==".camerashake") InvalidateAnimationListing();

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
        EditorSound::Play(EditorSound::Event::Blocked); // same frame as the Enter, so no FieldCommit
        m_RenamingJustStarted = true; // Enter took focus off the field; give it back next frame
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
                    EditorSound::Play(EditorSound::Event::Blocked);
                    m_RenamingJustStarted = true; // Enter took focus off the field; give it back next frame
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
    ImGui::TextColored(EditorTheme::Danger,
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
        case Cell::Kind::Shader:     return "Shader";
        case Cell::Kind::Animator:   return "Animator Controller";
        case Cell::Kind::Weapon:     return "Weapon Definition";
        case Cell::Kind::Recoil:     return "Recoil Profile";
        case Cell::Kind::CameraShake:return "Camera Shake";
        case Cell::Kind::Hdri:       return "HDRI";
        case Cell::Kind::Script:     return "Script";
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
    InvalidateShadersListing();
    InvalidateAnimationListing();
    InvalidateProjectAssetIndex();
    InvalidateModelThumbnail();          // clears every cached model thumbnail
    m_AssetThumbs.Clear();
    m_AssetListingRefreshTimer = 0.0f;
    m_AssetRefreshFlash = 1.6f; // drives the module's brief "Assets refreshed" confirmation
    Log::Info("Asset Browser refreshed - re-scanned the project folder, dropped thumbnail caches.");
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

// Animator Controllers and weapon definitions, wherever they live under the project.
void EditorLayer::RefreshAnimationListingIfNeeded() {
    if (m_AnimationListingCache.valid) return;
    m_AnimationListingCache.paths.clear();
    std::error_code ec;
    const std::filesystem::path root(ProjectPaths::Root());
    for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() == 0 && it->is_directory() && it->path().filename() == "Library") {
            it.disable_recursion_pending();
            continue;
        }
        const auto ext = it->path().extension();
        if (it->is_regular_file() && (ext == ".controller" || ext == ".fpsanim" || ext == ".recoil" || ext==".camerashake"))
            m_AnimationListingCache.paths.push_back(it->path().generic_string());
    }
    std::sort(m_AnimationListingCache.paths.begin(), m_AnimationListingCache.paths.end());
    m_AnimationListingCache.valid = true;
}

// Worker thread (m_ProjectAssetIndex): every model / texture / material / sound / prefab file
// under `root`. Touches no editor or library state - the .meta's folder is read straight from the
// file, and the main thread files each entry into the folder tree.
std::vector<EditorLayer::ProjectAssetFile> EditorLayer::ScanProjectAssetFiles(const std::string& root) {
    namespace fs = std::filesystem;
    std::vector<EditorLayer::ProjectAssetFile> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code entryEc;
        if (it->is_directory(entryEc)) {
            // Library/ is caches; screenshots/ holds old editor captures; dot-folders are tooling.
            const std::string dir = it->path().filename().string();
            if (dir=="bin" || dir=="obj" || (it.depth() == 0 && (dir == "Library" || dir == "screenshots")) || (!dir.empty() && dir[0] == '.'))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(entryEc)) continue;
        EditorLayer::ProjectAssetFile f;
        f.Path = it->path().lexically_normal().string();
        const std::string type = AssetDatabase::AssetType(f.Path);
        if (type == "model") f.Kind = Cell::Kind::Model;
        else if (type == "texture") f.Kind = Cell::Kind::Texture;
        else if (type == "material") f.Kind = Cell::Kind::Material;
        else if (type == "audio") f.Kind = Cell::Kind::Sound;
        else if (type == "prefab") f.Kind = Cell::Kind::Prefab;
        else if (type == "animatorcontroller") f.Kind = Cell::Kind::Animator;
        else if (type == "firstpersonanimationset") f.Kind = Cell::Kind::Weapon;
        else if (type == "recoil") f.Kind = Cell::Kind::Recoil;
        else if (type == "camerashake") f.Kind = Cell::Kind::CameraShake;
        else if (type == "hdri") f.Kind = Cell::Kind::Hdri;
        else if (type == "script") f.Kind = Cell::Kind::Script;
        else continue;
        f.Key = AssetDatabase::PathKey(f.Path);
        f.Name = it->path().filename().string();
        std::ifstream meta(f.Path + ".meta", std::ios::binary);
        if (meta.is_open()) {
            const nlohmann::json j = nlohmann::json::parse(meta, nullptr, /*allow_exceptions=*/false);
            if (j.is_object() && j.contains("folder") && j["folder"].is_string()) {
                f.Folder = j["folder"].get<std::string>();
                f.MetaFolder = true;
            }
        }
        // The main thread files it with DiskFolderFor when the scan lands, which canonicalizes the path on disk:
        // done here on the scan's thread, it lands as a memoized lookup (ProjectPaths::Relativize) instead of a
        // disk call per file in one frame.
        if (!f.MetaFolder) (void)ProjectPaths::Relativize(f.Path);
        files.push_back(std::move(f));
    }
    return files;
}

void EditorLayer::RefreshProjectAssetIndexIfNeeded(AssetLibrary& assets) {
    auto& index = m_ProjectAssetIndex;
    bool replaced = false;
    if (index.scan.valid() && index.scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        index.files = index.scan.get();
        for (ProjectAssetFile& f : index.files)
            f.Folder = f.MetaFolder ? assets.AddFolderPath(f.Folder) : assets.DiskFolderFor(f.Path);
        replaced = true;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - index.scanStarted).count();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Asset Browser: indexed %zu asset file(s) in the project in %.0f ms.", index.files.size(), ms);
        Log::Info(buf);
    }
    if (!index.valid && !index.scan.valid()) {
        index.valid = true; // an invalidation while this runs starts another scan after it
        index.scanStarted = std::chrono::steady_clock::now();
        index.scan = std::async(std::launch::async, ScanProjectAssetFiles, ProjectPaths::Root());
    }

    // What the library lists, by PathKey - the index shows only the rest. Rebuilt when a list
    // grows or shrinks, or a new scan lands (a rename keeps the sizes).
    const size_t signature = assets.Models().size() + (assets.Textures().size() << 12) +
                             (assets.Materials().size() << 24) + ((size_t)assets.Sounds().size() << 36) +
                             ((size_t)assets.Prefabs().size() << 48);
    if (signature != m_ListedAssetSignature || replaced) {
        m_ListedAssetSignature = signature;
        m_ListedAssetKeys.clear();
        for (const auto& m : assets.Models()) m_ListedAssetKeys.insert(AssetDatabase::PathKey(m->Path()));
        for (const auto& t : assets.Textures()) m_ListedAssetKeys.insert(AssetDatabase::PathKey(t->Path()));
        for (const auto& m : assets.Materials()) m_ListedAssetKeys.insert(AssetDatabase::PathKey(m->Path));
        for (const auto& s : assets.Sounds()) m_ListedAssetKeys.insert(AssetDatabase::PathKey(s));
        for (const auto& p : assets.Prefabs()) m_ListedAssetKeys.insert(AssetDatabase::PathKey(p));
    }
}

std::string EditorLayer::NewAssetFolder(const std::string& fallback) const {
    const std::string& f = m_CurrentAssetFolder;
    // The virtual folders listing special directories are not places to create assets in.
    if (f.empty() || f == "Scenes" || f == "Shaders" || f == "Animation" ||
        f.find("..") != std::string::npos || f.find(':') != std::string::npos)
        return fallback;
    std::string lower = f;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower == "library" || lower.rfind("library/", 0) == 0) return fallback;
    std::error_code ec;
    return std::filesystem::is_directory(ProjectPaths::Resolve(f), ec) ? f : fallback;
}

std::vector<std::string> EditorLayer::AssetRefChoices(AssetLibrary& assets, ReflectAssetKind kind) {
    std::vector<std::string> out;
    Cell::Kind cellKind;
    switch (kind) {
        case ReflectAssetKind::Sound:    out = assets.Sounds(); cellKind = Cell::Kind::Sound; break;
        case ReflectAssetKind::Texture:  out = assets.TexturePaths(); cellKind = Cell::Kind::Texture; break;
        case ReflectAssetKind::Material: out = assets.MaterialPaths(); cellKind = Cell::Kind::Material; break;
        case ReflectAssetKind::Model:
            for (const auto& m : assets.Models()) out.push_back(m->Path());
            cellKind = Cell::Kind::Model;
            break;
        case ReflectAssetKind::Script:   cellKind = Cell::Kind::Script; break;
        default: return out;
    }
    std::unordered_set<std::string> have;
    for (const std::string& p : out) have.insert(AssetDatabase::PathKey(p));
    RefreshProjectAssetIndexIfNeeded(assets);
    for (const auto& f : m_ProjectAssetIndex.files)
        if (f.Kind == cellKind && have.insert(f.Key).second) out.push_back(f.Path);
    auto lowerName = [](const std::string& p) {
        std::string n = std::filesystem::path(p).filename().string();
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        return n;
    };
    std::sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) { return lowerName(a) < lowerName(b); });
    return out;
}

bool EditorLayer::LoadUnloadedAsset(AssetLibrary& assets, AssetGridCell& cell) {
    if (!cell.unloaded) return true;
    cell.unloaded = false;
    bool ok = true;
    switch (cell.kind) {
        case Cell::Kind::Model:
            cell.model = assets.LoadModel(cell.key);
            ok = cell.model && (cell.model->MeshCount() > 0 || cell.model->HasAnimations());
            break;
        case Cell::Kind::Texture:  cell.texture = assets.LoadTexture(cell.key); ok = cell.texture != nullptr; break;
        case Cell::Kind::Material: cell.material = assets.LoadMaterial(cell.key); ok = cell.material != nullptr; break;
        case Cell::Kind::Sound:    assets.RegisterSound(cell.key); break;
        case Cell::Kind::Prefab:   assets.RegisterPrefab(cell.key); break;
        default: break;
    }
    if (!ok) Log::Error("Couldn't load '" + ProjectPaths::Relativize(cell.key) + "' - see the lines above.");
    return ok;
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
    (void)world; (void)assets;
    EnqueueImports(FileDialog::OpenFiles(filter, m_Window), intoFolder); // #139 — multi-select
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
// raw filesystem-backed virtual folders (Scenes/Shaders) that AssetFolder() doesn't know about.
bool IsUnder(const std::string& path, const std::string& dir) {
    std::error_code ec;
    auto rel = std::filesystem::relative(path, dir, ec);
    return !ec && rel.string().rfind("..", 0) != 0;
}

// --- Glass Motion v2: folder zoom, grid ripple, import ripple, drag-fling ---------------------------
// All of it is draw-only: tiles are submitted at their rest layout (hit-testing, drag-and-drop and the
// clipper see the real grid) and their vertices are moved afterwards (Glass layers). The timers are
// Glass pulses, so an idle browser holds no animation state; what's kept here is a few strings and
// numbers, plus - only while an import batch is in flight - the keys the folder showed when it began.
struct AssetGridMotionState {
    bool        Primed = false;
    std::string Folder;           // the folder the grid showed last frame
    bool        Filtering = false;
    // Last frame's grid geometry (screen px): where tile 0 sits, the stride between tiles, one tile,
    // the columns, and the visible list rect.
    ImVec2 Origin, Stride, CellSize, ViewMin, ViewMax;
    int    Cols = 1;
    int    GeomFrame = -1;        // the frame the geometry was taken (its first drawn cell)
    // The folder tile under the pointer (else the selected one), so a dive knows where it came from.
    std::string TileKey;
    ImVec2 TileCentre;
    int    TileIndex = -1, TileFrame = -100;
    bool   TileHovered = false;
    // The folder zoom in flight, and its layer (begun on the first drawn cell, ended after the last).
    bool        ZoomOut = false;
    ImVec2      ZoomPivot;
    ImDrawList* LayerDl = nullptr;
    int         LayerVtx = 0;
    int         RippleCol = 0, RippleRow = 0; // the folder ripple's origin tile
    // The import ripple.
    int    ImportSerial = 0;
    bool   ImportArmed = false;
    std::string ImportSig;
    std::unordered_set<std::string> ImportKnown;
    int    ImportCol = 0, ImportRow = 0;
    double ImportStart = 0.0, ImportIdleSince = -1.0;
};
AssetGridMotionState& GridMotion() { static AssetGridMotionState s; return s; }

constexpr ImGuiID kGridZoomId     = 0xA55E7201u;
constexpr ImGuiID kGridRippleId   = 0xA55E7202u;
constexpr ImGuiID kImportTileSalt = 0x1A7E0C11u;
constexpr ImGuiID kGridFlingSalt  = 0x5F11A6D3u;
constexpr float   kZoomOutFrom    = 1.12f; // going up: the parent grid shrinks from this
constexpr float   kTileFromScale  = 0.94f; // a rippling tile grows from this
constexpr double  kImportSettleSeconds = 1.5; // after a batch, how long freshly listed tiles still ripple

ImGuiID ImportTileId(const std::string& key) { return ImHashStr(key.c_str()) ^ kImportTileSalt; }
float RippleSeconds() {
    return (float)Glass::RippleCap * Glass::RippleStepMs * 0.001f + Glass::GridWaveFadeMs * 0.001f;
}
// The module draws the Asset Browser's grid in a child named "##AssetList"; the Glass Library also
// rebuilds the cell list (AssetGridFrameBegin) from its own window, and must not drive this.
bool InAssetListWindow() {
    const ImGuiWindow* w = ImGui::GetCurrentWindow();
    return w && std::strstr(w->Name, "##AssetList") != nullptr;
}

// Per frame, from AssetGridFrameBegin inside ##AssetList: notice a folder change (zoom + ripple), a
// search starting or ending (ripple from the top-left) and new tiles while an import runs.
void GridMotionFrameBegin(const std::vector<AssetGridCell>& cells, const std::string& folder,
                          const std::string& filter, bool favoritesOnly, ImportQueueManager& queue) {
    AssetGridMotionState& gm = GridMotion();
    Glass::ImAnimContext& motion = Glass::Motion();
    const int frame = ImGui::GetFrameCount();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    gm.LayerDl = nullptr;
    gm.ViewMin = win->InnerRect.Min;
    gm.ViewMax = win->InnerRect.Max;
    // Details rows sit in the table's own window; their first cell sets Origin instead.
    if (!EditorSettings::Get().AssetDetailsMode) gm.Origin = ImGui::GetCursorScreenPos();
    queue.NoteGridWatching(frame);

    const int cols = std::max(1, gm.Cols);
    const int rows = cells.empty() ? 0 : ((int)cells.size() + cols - 1) / cols;
    auto centreOf = [&](int idx) {
        return ImVec2(gm.Origin.x + (float)(idx % cols) * gm.Stride.x + gm.CellSize.x * 0.5f,
                      gm.Origin.y + (float)(idx / cols) * gm.Stride.y + gm.CellSize.y * 0.5f);
    };
    auto inView = [&](ImVec2 p) {
        return p.x >= gm.ViewMin.x && p.x <= gm.ViewMax.x && p.y >= gm.ViewMin.y && p.y <= gm.ViewMax.y;
    };
    const ImVec2 viewCentre((gm.ViewMin.x + gm.ViewMax.x) * 0.5f, (gm.ViewMin.y + gm.ViewMax.y) * 0.5f);
    const bool filtering = !filter.empty() || favoritesOnly;

    if (!gm.Primed) {
        // First open: the tiles ripple in from the top-left.
        gm.Primed = true;
        gm.Folder = folder;
        gm.RippleCol = gm.RippleRow = 0;
        motion.Pulse(kGridRippleId, RippleSeconds());
    } else if (folder != gm.Folder) {
        const std::string old = std::move(gm.Folder);
        gm.Folder = folder;
        // Up (Up, Back, a breadcrumb or tree click on an ancestor): zoom back out about the folder tile
        // we came from. Anything else is a dive: zoom in about the tile that was opened, if we saw it.
        const bool out = !old.empty() && (folder.empty() || old.rfind(folder + "/", 0) == 0);
        int idx = -1;
        if (out) {
            std::string child = old;
            while (!child.empty() && ParentFolderOf(child) != folder) child = ParentFolderOf(child);
            for (size_t i = 0; i < cells.size() && idx < 0; ++i)
                if (cells[i].kind == AssetGridCell::Kind::Folder && cells[i].key == child) idx = (int)i;
            gm.ZoomPivot = (idx >= 0 && inView(centreOf(idx))) ? centreOf(idx) : viewCentre;
        } else if (gm.TileFrame >= frame - 3 && gm.TileKey == folder && gm.TileIndex >= 0) {
            idx = gm.TileIndex;
            gm.ZoomPivot = gm.TileCentre;
        } else {
            gm.ZoomPivot = viewCentre;
        }
        gm.RippleCol = idx >= 0 ? idx % cols : 0;
        gm.RippleRow = idx >= 0 ? idx / cols : 0;
        gm.ZoomOut = out;
        motion.Pulse(kGridZoomId, Glass::FolderZoom.durationMs * 0.001f);
        motion.Pulse(kGridRippleId, RippleSeconds());
        // Reduce Motion: no pulse was made, nothing moves, and there's nothing to hear.
        if (frame >= 30 && motion.PulseActive(kGridZoomId))
            EditorSound::Play(out ? EditorSound::Event::FolderZoomOut : EditorSound::Event::FolderZoomIn,
                              gm.ZoomPivot.x / std::max(1.0f, ImGui::GetIO().DisplaySize.x));
    } else if (filtering != gm.Filtering) {
        // A search (or the favourites view) started or ended: no origin, so from the top-left.
        gm.RippleCol = gm.RippleRow = 0;
        motion.Pulse(kGridRippleId, RippleSeconds());
    }
    gm.Filtering = filtering;
    motion.PulseProgress(kGridRippleId); // retires it the frame it ends

    // Import ripple: a new batch snapshots what the folder holds; tiles that appear after that ripple
    // in from where the files were dropped (the nearest tile to the drop point), or the top-left.
    const int serial = queue.BatchSerial();
    if (serial != gm.ImportSerial) {
        gm.ImportSerial = serial;
        if (!gm.ImportArmed) {
            gm.ImportArmed = true;
            gm.ImportKnown.clear();
            for (const auto& c : cells) gm.ImportKnown.insert(c.key);
            gm.ImportSig = folder + '\x1f' + filter + (favoritesOnly ? "\x1f*" : "");
        }
        gm.ImportCol = gm.ImportRow = 0;
        const ImVec2 drop = queue.DropPos();
        if (queue.HasDropPos() && inView(drop) && gm.Stride.y > 0.0f) {
            gm.ImportCol = gm.Stride.x > 0.0f ? std::clamp((int)std::floor((drop.x - gm.Origin.x) / gm.Stride.x), 0, cols - 1) : 0;
            gm.ImportRow = std::max(0, (int)std::floor((drop.y - gm.Origin.y) / gm.Stride.y));
        }
        gm.ImportStart = motion.Now();
        gm.ImportIdleSince = -1.0;
    }
    if (gm.ImportArmed) {
        // A different folder or search while armed: what it shows isn't new, just re-snapshot.
        const std::string sig = folder + '\x1f' + filter + (favoritesOnly ? "\x1f*" : "");
        const bool resnap = sig != gm.ImportSig;
        if (resnap) gm.ImportSig = sig;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (!gm.ImportKnown.insert(cells[i].key).second || resnap) continue;
            // The front spreads from the drop at the ripple's speed; a tile that lands after the front
            // has passed simply starts now.
            const int c = (int)i % cols, r = (int)i / cols;
            const float at = Glass::ImAnimContext::GridRippleDelay(c, r, cols, rows, gm.ImportCol, gm.ImportRow,
                                                                   Glass::RippleStepMs, Glass::RippleCap);
            const float wait = std::max(0.0f, (float)(gm.ImportStart + at - motion.Now()));
            motion.Pulse(ImportTileId(cells[i].key), Glass::GridWaveFadeMs * 0.001f, wait);
        }
        if (queue.IsActive()) {
            gm.ImportIdleSince = -1.0;
        } else if (gm.ImportIdleSince < 0.0) {
            gm.ImportIdleSince = motion.Now();
        } else if (motion.Now() - gm.ImportIdleSince > kImportSettleSeconds) {
            gm.ImportArmed = false;
            std::unordered_set<std::string>().swap(gm.ImportKnown);
            std::string().swap(gm.ImportSig);
        }
    }
}
} // namespace

// Phase 6 item 4 — Console click-to-navigate's asset half (see the header comment). Unlike the
// Inspector's ping button, `path` here comes from parsed log-message text, so it might not be a
// real path at all — the existence check is load-bearing, not a formality.
bool EditorLayer::PingAssetPath(AssetLibrary& assets, const std::string& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) return false;

    // Scenes and Shaders are raw filesystem-backed virtual folders (RefreshScenesListingIfNeeded
    // above) rather than AssetLibrary entries, so AssetFolder() would return "" (root) for them —
    // check those directories first, same as the grid does.
    std::string folder;
    if (IsUnder(path, ProjectPaths::Resolve("scenes"))) folder = "Scenes";
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
        InvalidateAnimationListing();
    }
    assets.CreateFolder("Scenes");
    RefreshProjectAssetIndexIfNeeded(assets); // registers the folders of files nothing has loaded yet
    InstallFolderPathHook(assets); // vFolders: styles + bookmarks follow a renamed / deleted folder

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

    // Animation assets use their real project folders, like prefabs and scripts.
    // Retire the old all-project shortcut when it has no authored folder behind it.
    // DeleteFolder only changes the browser folder list; it does not delete files.
    // The folder list first: the disk is only asked while there is a shortcut to retire.
    std::error_code animationFolderEc;
    const auto& browserFolders = assets.Folders();
    if (std::find(browserFolders.begin(), browserFolders.end(), "Animation") != browserFolders.end() &&
        !std::filesystem::is_directory(ProjectPaths::Resolve("Animation"), animationFolderEc)) {
        if (assets.CanDeleteFolder("Animation")) {
            assets.DeleteFolder("Animation");
            if (m_CurrentAssetFolder == "Animation") NavigateAssetFolder("assets/Weapons", /*sound=*/false);
        }
    }

    for (const auto& folder : assets.Folders()) {
        bool show = filtering ? (MatchesAssetSearch(parsedSearch, LeafNameOf(folder), "folder", {})
                                 && inSearchScope(ParentFolderOf(folder)))
            : (ParentFolderOf(folder) == m_CurrentAssetFolder);
        if (show) cells.push_back({Cell::Kind::Folder, folder, LeafNameOf(folder), nullptr, nullptr});
    }
    // Where it's filed first (a lookup, no copies): browsing shows one folder of thousands of assets, so
    // the name, labels and search match are only worked out for the ones that can show.
    auto inView = [&](const std::string& path) {
        const std::string& folder = assets.AssetFolderRef(path);
        return filtering ? inSearchScope(folder) : folder == m_CurrentAssetFolder;
    };
    for (const auto& model : assets.Models()) {
        const std::string& path = model->Path();
        if (!inView(path)) continue;
        std::string name = assets.DisplayName(path);
        if (!MatchesAssetSearch(parsedSearch, name, "model", assets.Labels(path))) continue;
        cells.push_back({Cell::Kind::Model, path, std::move(name), model, nullptr});
    }
    for (const auto& tex : assets.Textures()) {
        const std::string& path = tex->Path();
        if (!inView(path)) continue;
        std::string name = assets.DisplayName(path);
        if (!MatchesAssetSearch(parsedSearch, name, "texture", assets.Labels(path))) continue;
        cells.push_back({Cell::Kind::Texture, path, std::move(name), nullptr, tex});
    }
    for (const auto& mat : assets.Materials()) {
        const std::string& path = mat->Path;
        if (!inView(path)) continue;
        std::string name = assets.DisplayName(path);
        if (!MatchesAssetSearch(parsedSearch, name, "material", assets.Labels(path))) continue;
        cells.push_back({Cell::Kind::Material, path, std::move(name), nullptr, nullptr, mat});
    }
    for (const auto& sound : assets.Sounds()) {
        if (!inView(sound)) continue;
        std::string name = assets.DisplayName(sound);
        if (!MatchesAssetSearch(parsedSearch, name, "sound", assets.Labels(sound))) continue;
        cells.push_back({Cell::Kind::Sound, sound, std::move(name), nullptr, nullptr});
    }
    for (const auto& prefab : assets.Prefabs()) {
        if (!inView(prefab)) continue;
        std::string name = assets.DisplayName(prefab);
        if (!MatchesAssetSearch(parsedSearch, name, "prefab", assets.Labels(prefab))) continue;
        cells.push_back({Cell::Kind::Prefab, prefab, std::move(name), nullptr, nullptr});
    }
    // Every other asset file in the project, unloaded until it's used (see m_ProjectAssetIndex),
    // and the kinds that are never library entries.
    RefreshProjectAssetIndexIfNeeded(assets);
    for (const auto& f : m_ProjectAssetIndex.files) {
        const std::string& moved = assets.AssetFolderRef(f.Path); // filed elsewhere in the browser since indexing
        const std::string& folder = moved.empty() ? f.Folder : moved;
        if (filtering ? !inSearchScope(folder) : folder != m_CurrentAssetFolder) continue;
        if (m_ListedAssetKeys.count(f.Key)) continue;
        const bool fileOnly = f.Kind == Cell::Kind::Animator || f.Kind == Cell::Kind::Weapon ||
                              f.Kind == Cell::Kind::Recoil || f.Kind == Cell::Kind::CameraShake ||
                              f.Kind == Cell::Kind::Hdri || f.Kind == Cell::Kind::Script;
        const char* kindName = f.Kind == Cell::Kind::Model ? "model" : f.Kind == Cell::Kind::Texture ? "texture"
                             : f.Kind == Cell::Kind::Material ? "material" : f.Kind == Cell::Kind::Sound ? "sound"
                             : f.Kind == Cell::Kind::Animator ? "animator" : f.Kind == Cell::Kind::Weapon ? "weapon"
                             : f.Kind == Cell::Kind::Recoil ? "recoil"
                             : f.Kind == Cell::Kind::CameraShake ? "camerashake"
                             : f.Kind == Cell::Kind::Script ? "script" : f.Kind == Cell::Kind::Hdri ? "hdri" : "prefab";
        // An HDRI is an image: both t:Hdri and t:Texture find it (like a screenshot).
        if (!MatchesAssetSearch(parsedSearch, f.Name, kindName, {}) &&
            !(f.Kind == Cell::Kind::Hdri && MatchesAssetSearch(parsedSearch, f.Name, "texture", {}))) continue;
        Cell c{f.Kind, f.Path, f.Name, nullptr, nullptr};
        c.unloaded = !fileOnly;
        cells.push_back(std::move(c));
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
                case Cell::Kind::Shader:     return 8;
                case Cell::Kind::Animator:   return 9;
                case Cell::Kind::Weapon:     return 10;
                case Cell::Kind::Recoil:     return 10;
                case Cell::Kind::CameraShake:return 10;
                case Cell::Kind::Hdri:       return 11;
                case Cell::Kind::Script:     return 12;
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

    // SearchEmpty: once, when typing turns a search that found something into one that finds nothing.
    if (searching && cells.empty() && m_SearchSoundPrevHadResults && !m_SearchSoundPrevFilter.empty() &&
        m_AssetSearchFilter != m_SearchSoundPrevFilter)
        EditorSound::Play(EditorSound::Event::SearchEmpty);
    m_SearchSoundPrevFilter = m_AssetSearchFilter;
    m_SearchSoundPrevHadResults = !cells.empty();

    // Ctrl+A - select every currently-visible item (respecting the active search/filter, same
    // as Unity's own "select all visible items in list"). Not while a text field (the search box,
    // the labels popup) has the keyboard: there Ctrl+A selects its text.
    if (m_AssetBrowserFocused && m_RenamingAssetKey.empty() && !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl
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

    // Motion v2: folder zoom / ripples (the Asset Browser's grid only, not the Glass Library's rebuild).
    if (InAssetListWindow()) GridMotionFrameBegin(cells, m_CurrentAssetFolder, m_AssetSearchFilter, m_AssetFavoritesOnly, m_ImportQueue);
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
    if (cells[cellIndex].unloaded) {
        // A file nothing has loaded yet: load it the moment it's clicked or right-clicked, before
        // the tile code below runs, so selection / drag / the context menu see a normal asset.
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 size = gridMode ? ImVec2(cellW, cellH)
                                     : ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeightWithSpacing());
        if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, ImVec2(min.x + size.x, min.y + size.y)) &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
            LoadUnloadedAsset(assets, cells[cellIndex]);
    }
    // --- Motion v2 -------------------------------------------------------------------------------
    // The first drawn cell of the frame takes the grid's geometry (the clipper starts every step at a
    // row start, so the content width here is the module's) and opens the folder-zoom layer.
    AssetGridMotionState& gm = GridMotion();
    Glass::ImAnimContext& motion = Glass::Motion();
    {
        const int frame = ImGui::GetFrameCount();
        if (gm.GeomFrame != frame) {
            gm.GeomFrame = frame;
            const ImGuiStyle& st = ImGui::GetStyle();
            const float availW = ImGui::GetContentRegionAvail().x;
            if (gridMode) {
                gm.Cols = std::max(1, (int)((availW + st.ItemSpacing.x) / (cellW + st.ItemSpacing.x)));
                gm.Stride = ImVec2(cellW + st.ItemSpacing.x, cellH + st.ItemSpacing.y);
                gm.CellSize = ImVec2(cellW, cellH);
            } else {
                gm.Cols = 1;
                gm.Stride = ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing());
                gm.CellSize = ImVec2(availW, ImGui::GetTextLineHeight());
            }
            const ImVec2 here = ImGui::GetCursorScreenPos();
            gm.Origin = ImVec2(here.x, here.y - (float)(index / gm.Cols) * gm.Stride.y);
            if (motion.PulseActive(kGridZoomId) && !gm.LayerDl) {
                gm.LayerDl = ImGui::GetWindowDrawList();
                gm.LayerVtx = Glass::ImAnimContext::BeginLayer(gm.LayerDl);
            }
        }
    }
    // How far this tile has rippled in: the folder ripple spreads from its origin tile, an imported
    // tile has its own pulse. The look is the old diagonal wave's (fade, rise, OutCubic over 260 ms).
    float reveal = 1.0f;
    {
        const int col = index % gm.Cols, row = index / gm.Cols;
        const float el = motion.PulseElapsed(kGridRippleId, FLT_MAX);
        if (el != FLT_MAX) {
            const int rows = ((int)cells.size() + gm.Cols - 1) / gm.Cols;
            const float delay = Glass::ImAnimContext::GridRippleDelay(col, row, gm.Cols, rows, gm.RippleCol, gm.RippleRow,
                                                                      Glass::RippleStepMs, Glass::RippleCap);
            reveal = Glass::EaseProgress(Glass::HoverIn, (el - delay) / (Glass::GridWaveFadeMs * 0.001f));
        }
        if (gm.ImportArmed) {
            const ImGuiID tid = ImportTileId(cells[cellIndex].key);
            if (motion.PulseActive(tid)) reveal = std::min(reveal, Glass::EaseProgress(Glass::HoverIn, motion.PulseProgress(tid)));
        }
    }
    ImDrawList* tileDl = ImGui::GetWindowDrawList();
    const ImVec2 restMin = ImGui::GetCursorScreenPos();
    const ImVec2 restSize = gridMode ? ImVec2(cellW, cellH) : ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight());
    const int tileVtx = reveal < 1.0f ? Glass::ImAnimContext::BeginLayer(tileDl) : -1;
    {
        const auto& cell = cells[cellIndex];
        ImGui::PushID(cell.key.c_str());
        // Tile image for a file that isn't loaded: a small decode of an image, the last cached
        // render of a model / material.
        AssetThumbnailLoader::Thumb unloadedThumb;
        if ((cell.unloaded && (cell.kind == Cell::Kind::Texture || cell.kind == Cell::Kind::Model || cell.kind == Cell::Kind::Material)) ||
            cell.kind == Cell::Kind::Hdri) // never loaded here: a small tone-mapped decode (stb; .exr keeps the glyph)
            unloadedThumb = m_AssetThumbs.Get(cell.key, cell.kind == Cell::Kind::Texture || cell.kind == Cell::Kind::Hdri
                ? AssetThumbnailLoader::Source::Image : AssetThumbnailLoader::Source::Cached);
        const ImVec2 thumbUv0(0.0f, unloadedThumb.FlipV ? 1.0f : 0.0f), thumbUv1(1.0f, unloadedThumb.FlipV ? 0.0f : 1.0f);

        bool isFolder = cell.kind == Cell::Kind::Folder;
        bool isSelected = IsAssetSelected(cell.key, isFolder);
        bool isRenaming = m_RenamingAssetKey == cell.key && m_RenamingIsFolder == isFolder;
        bool playing = cell.kind == Cell::Kind::Sound && AudioEngine::IsPreviewPlaying(cell.key);
        const char* icon = isFolder ? ICON_FA_FOLDER
            : cell.kind == Cell::Kind::Model ? (cell.model && cell.model->HasAnimations() ? ICON_FA_FILM : ICON_FA_CUBE)
            : cell.kind == Cell::Kind::Scene ? ICON_FA_MAP
            : cell.kind == Cell::Kind::Prefab ? ICON_FA_BOX_ARCHIVE
            : cell.kind == Cell::Kind::Material ? ICON_FA_DROPLET
            : cell.kind == Cell::Kind::Shader ? ICON_FA_FILE_CODE
            : cell.kind == Cell::Kind::Animator ? ICON_FA_DIAGRAM_PROJECT
            : cell.kind == Cell::Kind::Weapon ? ICON_FA_CROSSHAIRS
            : cell.kind == Cell::Kind::Recoil ? ICON_FA_BURST
            : cell.kind == Cell::Kind::CameraShake ? ICON_FA_VIDEO
            : cell.kind == Cell::Kind::Hdri ? ICON_FA_SUN
            : cell.kind == Cell::Kind::Script ? ICON_FA_SCROLL
            : (playing ? ICON_FA_STOP : ICON_FA_MUSIC);

        // Editor Enhancers / vFolders: a folder tile shows its resolved icon, colour and content.
        EditorFolderVisual folderVis;
        if (isFolder) {
            GetFolderVisual(assets, cell.key, folderVis);
            if (folderVis.Icon[0]) icon = folderVis.Icon;
        }
        const ImU32 folderTint = (isFolder && folderVis.Color) ? (EditorTheme::UserPacked(folderVis.Color) | 0xFF000000u) : 0u;

        // The asset-type colour: the tile's bottom stripe in Grid view.
        auto kindColor = [&]() -> ImVec4 {
            switch (cell.kind) {
                case Cell::Kind::Folder:     return EditorTheme::Secondary;
                case Cell::Kind::Model:      return EditorTheme::KindMesh;
                case Cell::Kind::Texture:    return EditorTheme::KindTexture;
                case Cell::Kind::Hdri:       return EditorTheme::KindLight;
                case Cell::Kind::Material:   return EditorTheme::KindMaterial;
                case Cell::Kind::Sound:      return EditorTheme::KindAudio;
                case Cell::Kind::Scene:      return EditorTheme::KindScene;
                case Cell::Kind::Prefab:     return EditorTheme::KindPrefab;
                case Cell::Kind::Script:
                case Cell::Kind::Shader:     return EditorTheme::KindScript;
                case Cell::Kind::Animator:   return EditorTheme::KindAnim;
                case Cell::Kind::Weapon:     return EditorTheme::Warning;
                case Cell::Kind::Recoil:     return EditorTheme::Warning;
                case Cell::Kind::CameraShake:return EditorTheme::Warning;
            }
            return EditorTheme::Secondary;
        };

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
            {
                // 2px asset-type stripe along the tile's bottom edge; a selected tile also gets a
                // 1.5px accent outline.
                const float stripeH = EditorTheme::Px(2.0f);
                const ImVec2 tileMax(tileMin.x + tileSize.x, tileMin.y + tileSize.y);
                dl->AddRectFilled(ImVec2(tileMin.x, tileMax.y - stripeH), tileMax,
                                  EditorTheme::U32(kindColor()));
                if (isSelected)
                    dl->AddRect(tileMin, tileMax, EditorTheme::U32(EditorTheme::Accent),
                                ImGui::GetStyle().FrameRounding, ImDrawFlags_None, EditorTheme::Px(1.5f));
            }
            unsigned int modelThumb = (cell.kind == Cell::Kind::Model && cell.model) ? ModelThumbnail(*cell.model)
                                    : (cell.kind == Cell::Kind::Material) ? MaterialThumbnail(cell.material) : 0u; // #107
            if (unloadedThumb.Tex) {
                ImVec2 imgPos(tileMin.x + (cellWidth - m_AssetIconSize) * 0.5f, tileMin.y + cellPadding * 0.5f);
                dl->AddImage((ImTextureID)(intptr_t)unloadedThumb.Tex, imgPos,
                             ImVec2(imgPos.x + m_AssetIconSize, imgPos.y + m_AssetIconSize), thumbUv0, thumbUv1);
            } else if (cell.texture && cell.kind == Cell::Kind::Texture) {
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
                dl->AddText(font, m_AssetIconSize, glyphPos, folderTint ? folderTint : textColor, icon);
                // vFolders content minimap: what a plain folder holds, stamped small along its glyph's foot.
                if (isFolder && !folderVis.Icon[0] && folderVis.MiniCount > 0 && EditorSettings::Get().FolderMinimap) {
                    const float ms = std::max(9.0f, m_AssetIconSize * 0.22f);
                    float mx = tileMin.x + (cellWidth - folderVis.MiniCount * ms * 1.25f) * 0.5f;
                    const float my = tileMin.y + cellPadding * 0.5f + m_AssetIconSize - ms * 0.9f;
                    for (int k = 0; k < folderVis.MiniCount; ++k, mx += ms * 1.25f)
                        dl->AddText(font, ms, ImVec2(mx, my), EditorTheme::U32(EditorTheme::Base), folderVis.Mini[k]);
                }
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
            // Details mode runs inside the module's ImGui table (EditorModuleAssetBrowser.cpp,
            // DrawDetailsTable): the module has already started this row, so the Type / Size /
            // Modified cells are filled first (columns 1-3), then the cursor returns to column 0 for
            // the icon + the row's Selectable - which stays the last submitted item, so hover,
            // drag source, double-click and the context menu below all attach to it.
            const bool detailsMode = !isRenaming && EditorSettings::Get().AssetDetailsMode;
            if (detailsMode) {
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
                ImGui::TableSetColumnIndex(1); ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Dim);
                ImGui::TextUnformatted(AssetKindLabel(cell.kind));
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(sizeStr.c_str());
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(modStr.c_str());
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(0);
            }

            // List mode: little icon (real thumbnail for textures, a Font Awesome glyph
            // otherwise) followed by the name, mirroring how the Scene Hierarchy lists rows.
            float rowIconSize = ImGui::GetTextLineHeight();
            unsigned int rowModelThumb = (cell.kind == Cell::Kind::Model && cell.model) ? ModelThumbnail(*cell.model)
                                       : (cell.kind == Cell::Kind::Material) ? MaterialThumbnail(cell.material) : 0u; // #107
            if (unloadedThumb.Tex) {
                ImGui::Image((ImTextureID)(intptr_t)unloadedThumb.Tex, ImVec2(rowIconSize, rowIconSize), thumbUv0, thumbUv1);
            } else if (cell.texture && cell.kind == Cell::Kind::Texture) {
                ImGui::Image((ImTextureID)(intptr_t)cell.texture->GLHandle(), ImVec2(rowIconSize, rowIconSize));
            } else if (rowModelThumb) {
                ImGui::Image((ImTextureID)(intptr_t)rowModelThumb, ImVec2(rowIconSize, rowIconSize), ImVec2(0, 1), ImVec2(1, 0));
            } else if (folderTint) {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(folderTint), "%s", icon);
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
                clicked = ImGui::Selectable(cell.display.c_str(), isSelected,
                    detailsMode ? (ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)
                                : ImGuiSelectableFlags_None,
                    ImVec2(0.0f, 0.0f));
                ImGui::PopItemFlag();
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
        // Motion v2: remember the folder tile under the pointer (else the selected one), so opening
        // it (double-click, Enter) zooms in about it and the new grid ripples out from there.
        if (isFolder) {
            const int frame = ImGui::GetFrameCount();
            const bool hov = ImGui::IsItemHovered();
            if (hov || (isSelected && !(gm.TileFrame == frame && gm.TileHovered))) {
                if (gm.TileKey != cell.key) gm.TileKey = cell.key;
                const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                gm.TileCentre = ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
                gm.TileIndex = index;
                gm.TileFrame = frame;
                gm.TileHovered = hov;
            }
        }
        if (isFolder && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            NavigateAssetFolder(cell.key);
        }
        if (cell.kind == Cell::Kind::Scene && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            RequestOpenScene(world, assets, cell.key);
        }
        if (cell.kind == Cell::Kind::Animator && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            OpenAnimatorWindow(ProjectPaths::Relativize(cell.key));
            EditorSound::Play(EditorSound::Event::AssetOpen, EditorSound::LastItemScreenX());
        }
        if ((cell.kind == Cell::Kind::Shader || cell.kind == Cell::Kind::Script) &&
            ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            OpenScriptIDE(cell.key);
            EditorSound::Play(EditorSound::Event::AssetOpen, EditorSound::LastItemScreenX());
        }
        // #176 - double-click opens the prefab in Prefab Mode, like Unity (drag it into the scene,
        // or right-click > Place Instance, to place one).
        if (cell.kind == Cell::Kind::Prefab && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            EnterPrefabMode(world, assets, cell.key);
        }

        if (isFolder) {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                ImGui::SetDragDropPayload("ASSET_FOLDER_PATH", cell.key.c_str(), cell.key.size() + 1);
                Glass::DragGhostLabel(ICON_FA_FOLDER, cell.display.c_str());
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
                // Controllers, weapon definitions and scripts: files, filed the same way.
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_FILE_PATH")) {
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
        } else if (cell.kind != Cell::Kind::Scene &&
                   cell.kind != Cell::Kind::Shader) { // not placeable — nothing to drag into the viewport
            const char* payloadType = cell.kind == Cell::Kind::Model ? "ASSET_MODEL_PATH"
                : cell.kind == Cell::Kind::Texture || cell.kind == Cell::Kind::Hdri ? "ASSET_TEXTURE_PATH" // the sky's HDRI field takes .hdr
                : cell.kind == Cell::Kind::Material ? "ASSET_MATERIAL_PATH"
                : cell.kind == Cell::Kind::Prefab ? "ASSET_PREFAB_PATH"
                : cell.kind == Cell::Kind::Sound ? "ASSET_SOUND_PATH" : "ASSET_FILE_PATH";
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                ImGui::SetDragDropPayload(payloadType, cell.key.c_str(), cell.key.size() + 1);
                Glass::DragGhostLabel(ICON_FA_FILE, cell.display.c_str());
                ImGui::EndDragDropSource();
            }
        }

        if (isFolder && ImGui::IsItemHovered()) SetHoveredAssetFolder(cell.key); // vFolders hover keys
        if (!isRenaming && ImGui::IsItemHovered()) {
            if (cell.kind == Cell::Kind::Model) {
                EditorUI::SetTooltip("%s\n\nDrag into the viewport to place\n\n%s",
                    cell.display.c_str(), UsageTooltip(cell.model ? FindModelUsages(world, cell.model.get())
                                                                  : std::vector<std::string>{}).c_str());
            } else if (cell.kind == Cell::Kind::Texture) {
                EditorUI::SetTooltip(
                    "%s\n\nDrag onto a model to set its Albedo map,\nor onto a map row in the Inspector's PBR Material.\n\n%s",
                    cell.display.c_str(), UsageTooltip(cell.texture ? FindTextureUsages(world, cell.texture.get())
                                                                    : std::vector<std::string>{}).c_str());
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
            } else if (cell.kind == Cell::Kind::Shader) {
                EditorUI::SetTooltip("%s\n\nDouble-click to open externally.\nSelect to preview + compile status in the Inspector.",
                    cell.display.c_str());
            } else if (cell.kind == Cell::Kind::Animator) {
                EditorUI::SetTooltip("%s\n\nDouble-click to open in the Animator.", cell.display.c_str());
            } else if (cell.kind == Cell::Kind::Weapon || cell.kind == Cell::Kind::Recoil || cell.kind==Cell::Kind::CameraShake) {
                EditorUI::SetTooltip("%s\n\nSelect to edit in the Inspector.", cell.display.c_str());
            } else if (cell.kind == Cell::Kind::Hdri) {
                EditorUI::SetTooltip("%s\n\nDrag onto the sky's HDRI field (Environment) to light the scene with it.",
                    cell.display.c_str());
            } else if (cell.kind == Cell::Kind::Script) {
                EditorUI::SetTooltip("%s\n\nDouble-click to open externally.", cell.display.c_str());
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
                if (ImGui::MenuItem(ICON_FA_UP_RIGHT_FROM_SQUARE "  Open Externally")) OsShell::OpenFile(cell.key);
                if((cell.kind==Cell::Kind::Script || cell.kind==Cell::Kind::Shader) && ImGui::MenuItem("Open in Script IDE"))OpenScriptIDE(cell.key);
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show in folder")) OsShell::ShowInFolder(cell.key);
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
            if (isFolder && ImGui::BeginMenu(ICON_FA_PALETTE "  Folder Style & Bookmark")) { // vFolders
                DrawFolderContextMenuBody(world, assets, cell.key);
                ImGui::EndMenu();
            }
            EditorUIPrimitives::SectionHeader(ICON_FA_PEN "  Edit");
            if (ImGui::MenuItem(ICON_FA_PEN "  Rename (F2)", nullptr, false, m_ExtraAssetSelection.empty())) {
                BeginRenameAsset(cell.key, isFolder, cell.display);
            }
            // The labels popup opens nested in this context menu, so the menu has to stay open
            // for it (a MenuItem normally closes its popup, and the nested popup with it); Apply
            // closes both.
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
            const bool editLabels = ImGui::MenuItem(ICON_FA_TAG "  Edit Labels...");
            ImGui::PopItemFlag();
            if (editLabels) {
                std::string joined;
                for (const auto& lbl : assets.Labels(cell.key)) {
                    if (!joined.empty()) joined += ", ";
                    joined += lbl;
                }
                snprintf(m_LabelsEditBuffer, sizeof(m_LabelsEditBuffer), "%s", joined.c_str());
                ImGui::OpenPopup("##EditLabels");
            }
            bool labelsApplied = false;
            if (ImGui::BeginPopup("##EditLabels")) {
                ImGui::TextDisabled("Comma-separated labels - searchable as l:label");
                ImGui::SetNextItemWidth(240.0f * m_UIScale); // #37
                bool enter = ImGui::InputText("##LabelsBuf", m_LabelsEditBuffer, sizeof(m_LabelsEditBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                bool apply = enter || EditorUIPrimitives::PrimaryButton("Apply");
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
                    labelsApplied = true;
                }
                ImGui::EndPopup();
            }
            if (labelsApplied) ImGui::CloseCurrentPopup(); // the context menu too
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
                EditorUIPrimitives::SectionHeader(ICON_FA_ROTATE "  Actions");
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
                if (cell.kind == Cell::Kind::Model && ImGui::MenuItem(ICON_FA_DROPLET "  Extract Materials"))
                    ExtractMaterialsFor(world, assets, cell.key);
                if (cell.kind == Cell::Kind::Model && ImGui::IsItemHovered())
                    EditorUI::SetTooltip("Write one editable .mat per material this model uses (into the\n"
                                         "asset's Materials folder) and use them on its placed objects.");
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
                // Files that are never library entries have nothing to remove from it.
                const bool fileOnly = cell.kind == Cell::Kind::Animator || cell.kind == Cell::Kind::Weapon ||
                                      cell.kind == Cell::Kind::Recoil || cell.kind==Cell::Kind::CameraShake ||
                                      cell.kind == Cell::Kind::Hdri || cell.kind == Cell::Kind::Script;
                ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
                if (!fileOnly && ImGui::MenuItem(removeLabel)) {
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

    // Motion v2: the ripple's fade, rise and scale, applied to everything the tile just drew.
    if (tileVtx >= 0) {
        Glass::LayerTransform t;
        t.pivot = ImVec2(restMin.x + restSize.x * 0.5f, restMin.y + restSize.y * 0.5f);
        const float s = gridMode ? kTileFromScale + (1.0f - kTileFromScale) * reveal : 1.0f;
        t.scale = ImVec2(s, s);
        t.translate = ImVec2(0.0f, EditorTheme::Px(Glass::GridWaveRisePx) * (1.0f - reveal));
        t.alpha = reveal;
        Glass::ImAnimContext::EndLayer(tileDl, tileVtx, t);
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

    // ChildWindows: Details view's table scrolls in its own inner window inside ##AssetList.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemHovered()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ClearAssetSelection();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##BrowserBgContext");
    }

    // --- Motion v2 ---------------------------------------------------------------------------------
    AssetGridMotionState& gm = GridMotion();
    Glass::ImAnimContext& motion = Glass::Motion();
    // Folder zoom: diving in, the new grid grows from FolderZoomFrom about the opened tile while it
    // fades in; going up, the parent shrinks from 1.12 about the folder we came from.
    if (gm.LayerDl) {
        const float e = Glass::EaseProgress(Glass::FolderZoom, motion.PulseProgress(kGridZoomId));
        const float from = gm.ZoomOut ? kZoomOutFrom : Glass::FolderZoomFrom;
        const float s = from + (1.0f - from) * e;
        Glass::LayerTransform t;
        t.pivot = gm.ZoomPivot;
        t.scale = ImVec2(s, s);
        t.alpha = std::min(1.0f, e / 0.6f);
        Glass::ImAnimContext::EndLayer(gm.LayerDl, gm.LayerVtx, t);
        gm.LayerDl = nullptr;
    } else {
        motion.PulseProgress(kGridZoomId); // nothing drawn (an empty folder): let it run out
    }

    // Drag the grid to scroll it; let go to fling (Inertia). Left-drag on empty grid space (the grid
    // has no box-select, so empty space is free) or middle-drag anywhere over it. A tile's own
    // left-drag stays drag-and-drop: a press on a tile never starts this. Claiming the active id also
    // keeps a floating Asset Browser from being dragged around by its background.
    {
        ImGuiWindow* list = ImGui::GetCurrentWindow();
        ImGuiWindow* target = list; // Details view scrolls its table's inner window
        if (EditorSettings::Get().AssetDetailsMode)
            if (ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##AssetDetails")))
                if (table->InnerWindow) target = table->InnerWindow;
        const ImGuiID dragId = ImGui::GetID("##gridFling");
        const ImGuiID flingKey = target->ID ^ kGridFlingSalt;
        static int s_DragButton = -1;
        if (ImGui::GetActiveID() == 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            int button = -1;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) button = ImGuiMouseButton_Left;
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) button = ImGuiMouseButton_Middle;
            if (button >= 0) {
                ImGui::SetActiveID(dragId, list);
                ImGui::FocusWindow(list); // what the click would have done (the active id stops ImGui doing it)
                s_DragButton = button;
                motion.InertiaStop(flingKey);
            }
        }
        auto scrollBy = [&](float dy) {
            const float y = std::clamp(target->Scroll.y + dy, 0.0f, target->ScrollMax.y);
            ImGui::SetScrollY(target, y);
            return y != target->Scroll.y + dy; // hit an end
        };
        if (ImGui::GetActiveID() == dragId) {
            if (s_DragButton >= 0 && ImGui::IsMouseDown(s_DragButton)) {
                const float dy = -ImGui::GetIO().MouseDelta.y;
                if (dy != 0.0f) scrollBy(dy);
                motion.InertiaTrack(flingKey, ImVec2(0.0f, dy));
                if (ImGui::IsMouseDragging(s_DragButton)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            } else {
                motion.InertiaRelease(flingKey);
                ImGui::ClearActiveID();
                s_DragButton = -1;
            }
        }
        const ImVec2 coast = motion.InertiaDelta(flingKey);
        if (coast.y != 0.0f && scrollBy(coast.y)) motion.InertiaStop(flingKey);
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
            // In the folder being viewed, like Unity (assets/Materials when it isn't a real folder).
            const std::filesystem::path dir = ProjectPaths::Resolve(NewAssetFolder("assets/Materials"));
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
        if (ImGui::MenuItem(ICON_FA_GUN "  Create First-Person Weapon...")) m_OpenWeaponWizard = true;
        if (ImGui::MenuItem(ICON_FA_VIDEO "  Create Camera Shake")) {
            const auto base=std::filesystem::u8path(ProjectPaths::Resolve(NewAssetFolder("assets/CameraShake")+"/New Camera Shake.camerashake"));
            auto target=base;
            std::error_code ec;
            for(int n=1;std::filesystem::exists(target,ec);++n)
                target=base.parent_path()/(base.stem().u8string()+" "+std::to_string(n)+".camerashake");
            if(CameraShakeAsset{}.SaveFile(target.u8string())) {
                AssetDatabase::EnsureGuid(target.u8string());
                m_SelectedAssetKey=target.u8string(); m_SelectedAssetIsFolder=false;
                EditorSound::Play(EditorSound::Event::Create);
                InvalidateAnimationListing();
                InvalidateProjectAssetIndex(); // listed in the folder without a manual refresh
            } else Log::Error("Could not create camera shake asset.");
        }
        if (ImGui::MenuItem(ICON_FA_BURST "  Create Recoil Profile")) {
            const std::string rel = NewAssetFolder("assets/Recoil") + "/New Recoil.recoil";
            auto recoilPath = std::filesystem::u8path(ProjectPaths::Resolve(rel));
            recoilPath.replace_extension(".recoil");
            const auto base = recoilPath;
            std::error_code recoilEc;
            std::filesystem::create_directories(recoilPath.parent_path(), recoilEc);
            for (int n = 1; std::filesystem::exists(recoilPath, recoilEc); ++n)
                recoilPath = base.parent_path() / (base.stem().u8string() + " " + std::to_string(n) + ".recoil");
            if (RecoilAsset{}.SaveFile(recoilPath.u8string())) {
                AssetDatabase::EnsureGuid(recoilPath.u8string());
                m_SelectedAssetKey = recoilPath.u8string();
                m_SelectedAssetIsFolder = false;
                EditorSound::Play(EditorSound::Event::Create);
                InvalidateAnimationListing();
                InvalidateProjectAssetIndex(); // listed in the folder without a manual refresh
            } else Log::Error("Could not create recoil profile.");
        }
        if (ImGui::MenuItem(ICON_FA_PERSON_WALKING "  Create Body Locomotion Controller")) {
            // The standard first-person body graph (14 states, 63 tuned transitions), its clips found by name
            // among the project's animation files (the MC Core Motion pack's names: Loco_Walk_Fwd, ...).
            // Roles with no file are left empty for the Animator; its Lint tab lists them.
            std::error_code ec;
            const std::string dirRel = NewAssetFolder("assets/Animations/Controllers");
            std::filesystem::create_directories(std::filesystem::u8path(ProjectPaths::Resolve(dirRel)), ec);
            std::string rel = dirRel + "/Body Locomotion.controller";
            for (int n = 1; std::filesystem::exists(std::filesystem::u8path(ProjectPaths::Resolve(rel)), ec); ++n)
                rel = dirRel + "/Body Locomotion (" + std::to_string(n) + ").controller";
            const std::vector<std::string>& files = ProjectModelFiles();
            int found = 0, total = 0;
            const AnimatorController c = FPBody::BuildLocomotionController([&](const std::string& role) {
                ++total;
                const std::string f = FPBody::PickLocomotionClip(role, files);
                found += !f.empty();
                return f;
            });
            if (c.SaveFile(ProjectPaths::Resolve(rel))) {
                InvalidateAnimationListing();
                InvalidateProjectAssetIndex(); // listed in the folder without a manual refresh
                OpenAnimatorWindow(rel);
                EditorSound::Play(EditorSound::Event::Create);
                Log::Info("Created " + rel + ": " + std::to_string(found) + " of " + std::to_string(total) +
                          " clip slots filled by name. Fill the rest in the Animator (Lint lists them).");
            } else {
                Log::Error("Create Body Locomotion Controller: couldn't write '" + rel + "'.");
            }
        }
        if (ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT "  Create Animator Controller")) {
            // In the folder being viewed, like Create Material (assets/Animations/Controllers otherwise);
            // listed in that project folder and opened straight into the Animator window.
            std::error_code ec;
            const std::string dirRel = NewAssetFolder("assets/Animations/Controllers");
            std::filesystem::create_directories(std::filesystem::u8path(ProjectPaths::Resolve(dirRel)), ec);
            std::string rel = dirRel + "/New Animator Controller.controller";
            for (int n = 1; std::filesystem::exists(std::filesystem::u8path(ProjectPaths::Resolve(rel)), ec); ++n)
                rel = dirRel + "/New Animator Controller (" + std::to_string(n) + ").controller";
            AnimatorController c;
            AnimatorController::State idle;
            idle.Name = "Idle";
            idle.Motions.resize(c.Tracks.size());
            c.Layers[0].States.push_back(std::move(idle));
            c.Layers[0].DefaultState = "Idle";
            if (c.SaveFile(ProjectPaths::Resolve(rel))) {
                InvalidateAnimationListing();
                InvalidateProjectAssetIndex(); // listed in the folder without a manual refresh
                OpenAnimatorWindow(rel);
                EditorSound::Play(EditorSound::Event::Create);
                Log::Info("Created " + rel + ".");
            } else {
                Log::Error("Create Animator Controller: couldn't write '" + rel + "'.");
            }
        }
        ImGui::EndPopup();
    }

    // --- New First-Person Weapon wizard ----------------------------------------------------------
    // Arms + weapon model, the folders holding their animation files, one clip per state (guessed from the
    // file names, editable), then: <weapons/Name/Name.fpsanim> and the standard first-person controller.
    {
        struct Wizard {
            bool Open = false;
            char Name[64] = "New Weapon";
            std::string ArmsModel, WeaponModel, ArmsDir, WeaponDir;
            std::vector<FPWizard::Pick> Picks;
            std::string Error;
        };
        static Wizard w;
        auto parentOf = [](const std::string& p) {
            const size_t s = p.find_last_of('/');
            return s == std::string::npos ? std::string() : p.substr(0, s);
        };
        auto filesIn = [&](const std::string& dir) {
            std::vector<std::string> out;
            for (const std::string& f : ProjectModelFiles())
                if (parentOf(f) == dir) out.push_back(f);
            return out;
        };
        auto autoAssign = [&]() {
            const std::vector<std::string> armsFiles = filesIn(w.ArmsDir), weaponFiles = filesIn(w.WeaponDir);
            w.Picks.clear();
            for (const auto& s : FPWizard::States())
                w.Picks.push_back({s.State, FPWizard::PickClip(s.State, armsFiles), FPWizard::PickClip(s.State, weaponFiles)});
        };
        // A model file combo.
        auto modelCombo = [&](const char* id, std::string& path) {
            bool changed = false;
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 20.0f, 0.0f), ImVec2(FLT_MAX, ImGui::GetFontSize() * 24.0f));
            if (ImGui::BeginCombo(id, path.empty() ? "(none)" : path.c_str())) {
                static char filter[64] = "";
                if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##f", ICON_FA_MAGNIFYING_GLASS " search", filter, sizeof filter);
                std::string picked = path;
                if (ProjectClipFileList(filter, picked, 200) && picked != path) { path = picked; changed = true; ImGui::CloseCurrentPopup(); }
                ImGui::EndCombo();
            }
            return changed;
        };
        // A folder combo: every project folder that holds model files.
        auto dirCombo = [&](const char* id, std::string& dir) {
            bool changed = false;
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 20.0f, 0.0f), ImVec2(FLT_MAX, ImGui::GetFontSize() * 24.0f));
            if (ImGui::BeginCombo(id, dir.empty() ? "(pick a folder)" : dir.c_str())) {
                static char filter[64] = "";
                if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##f", ICON_FA_MAGNIFYING_GLASS " search", filter, sizeof filter);
                std::set<std::string> dirs;
                for (const std::string& f : ProjectModelFiles()) dirs.insert(parentOf(f));
                for (const std::string& d : dirs) {
                    if (!ClipFilterMatch(filter, d)) continue;
                    const std::string label = (d.empty() ? std::string("(project root)") : d) + "##dir" + d;
                    if (ImGui::Selectable(label.c_str(), d == dir)) { dir = d; changed = true; ImGui::CloseCurrentPopup(); }
                }
                ImGui::EndCombo();
            }
            return changed;
        };
        auto clipCombo = [&](const char* id, std::string& clip, const std::string& dir) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            const std::string shown = clip.empty() ? std::string("(none)") : std::filesystem::u8path(clip).stem().u8string();
            if (ImGui::BeginCombo(id, shown.c_str())) {
                if (ImGui::Selectable("(none)", clip.empty())) clip.clear();
                for (const std::string& f : filesIn(dir))
                    if (ImGui::Selectable(std::filesystem::u8path(f).stem().u8string().c_str(), f == clip)) clip = f;
                ImGui::EndCombo();
            }
        };

        if (m_OpenWeaponWizard) {
            m_OpenWeaponWizard = false;
            w = Wizard{};
            autoAssign();
            ImGui::OpenPopup("New First-Person Weapon");
        }
        ImGui::SetNextWindowSize(ImVec2(620.0f * m_UIScale, 640.0f * m_UIScale), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("New First-Person Weapon", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("Pick the arms and weapon models and the folders holding their animation files. Clips are matched to the "
                               "standard first-person states by file name; fix any that are wrong, then Create.");
            ImGui::Spacing();
            ImGui::InputText("Name", w.Name, sizeof w.Name);
            ImGui::TextUnformatted("Arms model");
            modelCombo("##wa", w.ArmsModel);
            ImGui::TextUnformatted("Weapon model");
            modelCombo("##ww", w.WeaponModel);
            ImGui::TextUnformatted("Arms animations folder");
            if (dirCombo("##wad", w.ArmsDir)) autoAssign();
            ImGui::TextUnformatted("Weapon animations folder");
            if (dirCombo("##wwd", w.WeaponDir)) autoAssign();
            if (EditorUIPrimitives::SecondaryButton("Re-match by name")) autoAssign();
            ImGui::Separator();
            if (ImGui::BeginChild("##wclips", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.5f), ImGuiChildFlags_Borders)) {
                if (ImGui::BeginTable("##wt", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn("Arms clip", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                    ImGui::TableSetupColumn("Weapon clip", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < w.Picks.size(); ++i) {
                        ImGui::PushID((int)i);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(w.Picks[i].State.c_str());
                        ImGui::TableNextColumn();
                        clipCombo("##a", w.Picks[i].ArmsClip, w.ArmsDir);
                        ImGui::TableNextColumn();
                        clipCombo("##w", w.Picks[i].WeaponClip, w.WeaponDir);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();
            int filled = 0;
            for (const auto& p : w.Picks) filled += !p.ArmsClip.empty() || !p.WeaponClip.empty();
            const bool ready = w.Name[0] && !w.ArmsModel.empty() && !w.WeaponModel.empty() && filled > 0;
            ImGui::TextDisabled("%d of %d states have a clip. Missing ones play the bind pose / nothing (Idle and Fire matter most).", filled, (int)w.Picks.size());
            if (!w.Error.empty()) ImGui::TextColored(EditorUIPrimitives::DangerColor(), "%s", w.Error.c_str());
            ImGui::BeginDisabled(!ready);
            bool createClicked;
            { EditorSoundEvents::ScopedClickSilence quiet; createClicked = EditorUIPrimitives::PrimaryButton("Create"); } // Create / Blocked below
            if (createClicked) {
                std::string safe = w.Name;
                for (char& c : safe) if (!std::isalnum((unsigned char)c) && c != '_' && c != '-' && c != ' ') c = '_';
                const std::string dirRel = "assets/Weapons/" + safe;
                const std::string setRel = dirRel + "/" + safe + ".fpsanim", ctrlRel = dirRel + "/" + safe + ".controller";
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::u8path(ProjectPaths::Resolve(dirRel)), ec);
                FirstPersonAnimationSet set = FPWizard::Build(w.ArmsModel, w.WeaponModel, w.Picks);
                if (std::filesystem::exists(std::filesystem::u8path(ProjectPaths::Resolve(setRel)), ec)) {
                    w.Error = "'" + setRel + "' already exists: pick another name.";
                    EditorSound::Play(EditorSound::Event::Blocked);
                } else if (!BuildFirstPersonController(set).SaveFile(ProjectPaths::Resolve(ctrlRel))) {
                    w.Error = "Couldn't write '" + ctrlRel + "'.";
                    EditorSound::Play(EditorSound::Event::Blocked);
                } else {
                    set.Controller = ctrlRel;
                    set.Clips.clear();
                    RecoilAsset recoil;
                    set.Procedural.Recoil = recoil.Data;
                    set.Procedural.Recoil.HipProcedural = true;
                    set.RecoilProfile = dirRel + "/" + safe + ".recoil";
                    const bool profileSaved = recoil.SaveFile(ProjectPaths::Resolve(set.RecoilProfile));
                    if (profileSaved) {
                        AssetDatabase::EnsureGuid(ProjectPaths::Resolve(set.RecoilProfile));
                        set.RecoilProfileGuid = AssetDatabase::RefGuid(set.RecoilProfile);
                    }
                    if (profileSaved && set.SaveFile(ProjectPaths::Resolve(setRel))) {
                        InvalidateAnimationListing();
                        Log::Info("Created weapon " + setRel + " (controller " + ctrlRel + "). Select it in the Asset Browser to tune it.");
                        EditorSound::Play(EditorSound::Event::Create);
                        ImGui::CloseCurrentPopup();
                    } else {
                        w.Error = "Couldn't write '" + setRel + "'.";
                        EditorSound::Play(EditorSound::Event::Blocked);
                    }
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (EditorUIPrimitives::SecondaryButton("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
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
