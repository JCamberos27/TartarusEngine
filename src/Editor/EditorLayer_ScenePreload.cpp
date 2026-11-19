// Opening a scene in two steps. Loading it straight away imported every model it places on the
// main thread, one after another, inside a single frame: with the Drive assets that froze the
// editor for ~19 s on AnimTestbed and minutes on Forest. Instead the scene file (and the prefabs
// it places, a few levels deep) is read for the models and materials it uses, those load on the
// asset workers (AssetLibrary::Request*Async) while the editor keeps drawing a progress line, and
// the scene itself loads once they're in, from warm caches.
#include "EditorLayer.h"

#include "AssetDatabase.h"
#include "AssetLibrary.h"
#include "Core/Paths.h"
#include "Log.h"
#include "World.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

using json = nlohmann::json;
namespace fs = std::filesystem;

struct EditorLayer::ScenePreloadState {
    std::string Path;
    bool Frame = false; // frame the camera once it's open (a user open, not a revert)
    std::vector<AssetLibrary::AsyncHandle> Tickets;
    std::chrono::steady_clock::time_point Started;
};

namespace {

std::string LowerExt(const std::string& path) {
    std::string ext = fs::u8path(path).extension().u8string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

bool IsModelExt(const std::string& ext) {
    return ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".dae";
}

// A stored reference as the scene loader reads it: project-relative -> absolute, a GUID sibling
// ("<key>Guid") winning when it resolves; no "#clip" suffix, nothing synthetic ("primitive://").
std::string ResolveRef(const json& obj, const std::string& key, const std::string& value) {
    if (value.empty() || value.find("://") != std::string::npos) return {};
    std::string path = value.substr(0, value.find('#'));
    if (fs::u8path(path).is_relative()) path = fs::u8path(ProjectPaths::Resolve(path)).lexically_normal().u8string();
    const auto guid = obj.find(key + "Guid");
    if (guid != obj.end() && guid->is_string())
        path = AssetDatabase::Resolve(AssetGuid::FromString(guid->get<std::string>()), path);
    return path;
}

struct PreloadRefs {
    std::set<std::string> Models, Materials, Prefabs;
};

void CollectRefs(const json& node, PreloadRefs& out) {
    if (node.is_array()) {
        for (const json& v : node) CollectRefs(v, out);
        return;
    }
    if (!node.is_object()) return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (it->is_string()) {
            const std::string& value = it->get_ref<const std::string&>();
            const std::string ext = LowerExt(value.substr(0, value.find('#')));
            if (!IsModelExt(ext) && ext != ".mat" && ext != ".prefab") continue;
            const std::string path = ResolveRef(node, it.key(), value);
            if (path.empty()) continue;
            if (IsModelExt(ext)) out.Models.insert(path);
            else if (ext == ".mat") out.Materials.insert(path);
            else out.Prefabs.insert(path);
        } else if (it->is_structured()) {
            CollectRefs(*it, out);
        }
    }
}

bool ReadJsonFile(const std::string& path, json& out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f.is_open()) return false;
    out = json::parse(f, nullptr, /*allow_exceptions=*/false);
    return !out.is_discarded();
}

} // namespace

void EditorLayer::BeginOpenScene(World& world, AssetLibrary& assets, const std::string& path, bool frame) {
    m_ScenePreload.reset();
    json scene;
    PreloadRefs refs;
    if (ReadJsonFile(path, scene)) {
        CollectRefs(scene, refs);
        // Prefabs place models too: follow them a few levels (a prefab can nest others).
        std::set<std::string> seen;
        for (int depth = 0; depth < 4 && !refs.Prefabs.empty(); ++depth) {
            std::set<std::string> next;
            next.swap(refs.Prefabs);
            for (const std::string& prefab : next) {
                if (!seen.insert(prefab).second) continue;
                json pj;
                if (ReadJsonFile(prefab, pj)) CollectRefs(pj, refs);
            }
        }
    }
    auto state = std::make_shared<ScenePreloadState>();
    state->Path = path;
    state->Frame = frame;
    state->Started = std::chrono::steady_clock::now();
    std::error_code ec;
    for (const std::string& m : refs.Models)
        if (fs::exists(fs::u8path(m), ec)) state->Tickets.push_back(assets.RequestModelAsync(m));
    for (const std::string& m : refs.Materials)
        if (fs::exists(fs::u8path(m), ec)) state->Tickets.push_back(assets.RequestMaterialAsync(m));
    m_ScenePreload = std::move(state);
    PumpScenePreload(world, assets); // already loaded (or nothing to load): opens now
}

void EditorLayer::PumpScenePreload(World& world, AssetLibrary& assets) {
    if (!m_ScenePreload) return;
    ScenePreloadState& st = *m_ScenePreload;
    size_t ready = 0;
    for (const auto& t : st.Tickets) ready += AssetLibrary::IsReady(t) ? 1u : 0u;
    // A load that never reports back must not leave the scene unopened forever.
    constexpr auto kGiveUp = std::chrono::minutes(10);
    const bool timedOut = std::chrono::steady_clock::now() - st.Started > kGiveUp;
    if (ready < st.Tickets.size() && !timedOut) {
        m_ScenePreloadReady = ready;
        m_ScenePreloadTotal = st.Tickets.size();
        return;
    }
    if (timedOut) Log::Warn("Opening '" + st.Path + "': some assets were still loading after 10 minutes; opening anyway.");
    const std::string path = st.Path;
    const bool frame = st.Frame;
    m_ScenePreload.reset();
    m_ScenePreloadReady = m_ScenePreloadTotal = 0;
    if (m_InPlayMode) {
        Log::Warn("Stop Play mode before opening another scene.");
        return;
    }
    if (m_Dirty) { // edited while it loaded: ask again (the open is instant now, the caches are warm)
        m_PendingSceneSwitch = PendingSceneSwitch::Open;
        m_PendingScenePath = path;
        m_ScenePromptPending = true;
        return;
    }
    OpenScene(world, assets, path);
    if (frame) FrameOpenedScene(world, path);
}

bool EditorLayer::ScenePreloadProgress(std::string& sceneName, size_t& ready, size_t& total) const {
    if (!m_ScenePreload) return false;
    sceneName = fs::u8path(m_ScenePreload->Path).stem().u8string();
    ready = m_ScenePreloadReady;
    total = m_ScenePreloadTotal;
    return true;
}

// A line over the main viewport while a scene's assets load.
void EditorLayer::DrawScenePreloadOverlay() {
    std::string name;
    size_t ready = 0, total = 0;
    if (!ScenePreloadProgress(name, ready, total)) return;
    char text[256];
    std::snprintf(text, sizeof(text), "Opening %s: loading assets %zu / %zu", name.c_str(), ready, total);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(vp));
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pad(14.0f * m_UIScale, 8.0f * m_UIScale);
    const ImVec2 min(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f - pad.x, vp->WorkPos.y + vp->WorkSize.y * 0.5f - size.y * 0.5f - pad.y);
    const ImVec2 max(min.x + size.x + pad.x * 2.0f, min.y + size.y + pad.y * 2.0f);
    dl->AddRectFilled(min, max, IM_COL32(20, 22, 26, 225), 6.0f * m_UIScale);
    dl->AddText(ImVec2(min.x + pad.x, min.y + pad.y), IM_COL32(235, 235, 240, 255), text);
    if (total > 0) {
        const float f = (float)ready / (float)total;
        dl->AddRectFilled(ImVec2(min.x, max.y - 3.0f * m_UIScale), ImVec2(min.x + (max.x - min.x) * f, max.y),
                          IM_COL32(90, 160, 255, 255), 6.0f * m_UIScale);
    }
}
