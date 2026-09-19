// #176 - Prefab Mode: edit a .prefab in isolation (Unity's "Open Prefab").
//
// Entering snapshots the open scene and its undo history (the same way Play does), clears the
// world and instantiates the prefab on its own. Edits get their own undo history; Save writes the
// root and its children back to the .prefab. Leaving saves any unsaved prefab edits (Unity's
// Auto Save), restores the scene snapshot and history - and because a scene stores prefab
// instances as links, every instance of the edited prefab comes back with the changes.
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "Components.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "SceneSerializer.h"
#include "World.h"

#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <climits>
#include <filesystem>

using namespace EditorInternal;

namespace {

// The prefab's root: the parentless entity with the lowest Order (normally the only one).
entt::entity FindPrefabRoot(const World& world, int* rootCount) {
    entt::entity best = entt::null;
    int bestOrder = INT_MAX, count = 0;
    for (entt::entity e : world.Registry.view<const TransformComponent>()) {
        const auto* h = world.Registry.try_get<HierarchyComponent>(e);
        if (h && h->Parent != entt::null) continue;
        ++count;
        const auto* o = world.Registry.try_get<OrderComponent>(e);
        const int order = o ? o->Value : 0;
        if (order < bestOrder) { bestOrder = order; best = e; }
    }
    if (rootCount) *rootCount = count;
    return best;
}

} // namespace

void EditorLayer::EnterPrefabMode(World& world, AssetLibrary& assets, const std::string& path) {
    if (m_InPlayMode) {
        Log::Warn("Stop Play mode before opening a prefab.");
        return;
    }
    if (InPrefabMode()) {
        if (m_PrefabModePath == path) return;
        ExitPrefabMode(world, assets);
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), ec)) {
        Log::Error("Prefab '" + path + "' doesn't exist.");
        return;
    }
    CancelEyedropper();
    m_PrefabModeSceneSnapshot = SceneSerializer::SaveToString(world);
    m_PrePrefabHistory = { m_UndoStack, m_RedoStack, m_UndoBaseJson, m_RedoBaseJson,
                           m_ContentDepth, m_SavedUndoDepth, true };
    m_PrefabModeSceneDirty = m_Dirty;

    world = World();
    ClearSelection();
    m_HierarchyVisibleOrder.clear();
    m_HierarchyVisibleBuild.clear();
    ClearUndoHistory();
    entt::entity root = SceneSerializer::InstantiatePrefab(world, assets, path);
    if (root == entt::null) {
        // Put the scene back exactly as it was.
        SceneSerializer::LoadFromString(world, assets, m_PrefabModeSceneSnapshot);
        RestorePrePrefabHistory();
        m_PrefabModeSceneSnapshot.clear();
        Log::Error("Couldn't open prefab '" + path + "'.");
        return;
    }
    // Edited directly, not as an instance of itself.
    world.Registry.remove<PrefabInstanceComponent>(root);
    m_PrefabModePath = path;
    m_Dirty = false;
    m_SavedUndoDepth = m_ContentDepth;
    SelectItem(root, false);
    if (m_EditorCameraPtr) FrameSceneBounds(world, *m_EditorCameraPtr);
    Log::Info("Opened prefab " + ProjectPaths::Relativize(path) + " - Back to Scene saves it.");
}

bool EditorLayer::SavePrefabMode(World& world) {
    if (!InPrefabMode()) return false;
    int roots = 0;
    const entt::entity root = FindPrefabRoot(world, &roots);
    if (root == entt::null) {
        Log::Error("Prefab Mode: nothing to save - the prefab needs a root object.");
        return false;
    }
    if (roots > 1)
        Log::Warn("Prefab Mode: only the first root object and its children are saved; move the other " +
                  std::to_string(roots - 1) + " top-level object(s) under it.");
    if (!SceneSerializer::SavePrefab(world, root, m_PrefabModePath)) return false;
    SceneSerializer::ClearPrefabPristineCache(); // instances diff their overrides against the new file
    m_Dirty = false;
    m_SavedUndoDepth = m_ContentDepth;
    return true;
}

void EditorLayer::RestorePrePrefabHistory() {
    if (!m_PrePrefabHistory.Valid) return;
    m_UndoStack = std::move(m_PrePrefabHistory.Undo);
    m_RedoStack = std::move(m_PrePrefabHistory.Redo);
    m_UndoBaseJson = std::move(m_PrePrefabHistory.UndoBase);
    m_RedoBaseJson = std::move(m_PrePrefabHistory.RedoBase);
    m_ContentDepth = m_PrePrefabHistory.ContentDepth;
    m_SavedUndoDepth = m_PrePrefabHistory.SavedDepth;
    m_PrePrefabHistory = {};
    m_HasStagedUndo = false;
    m_StagedUndoJson.clear();
    RefreshDirtyFromHistory();
    m_Dirty = m_PrefabModeSceneDirty;
}

void EditorLayer::ExitPrefabMode(World& world, AssetLibrary& assets, bool save) {
    if (!InPrefabMode()) return;
    CancelEyedropper();
    if (save && m_Dirty && !SavePrefabMode(world)) {
        Log::Error("Prefab Mode: the prefab couldn't be saved; your prefab edits are lost, the scene is unchanged.");
    }
    SceneSerializer::ClearPrefabPristineCache();
    ClearSelection();
    m_HierarchyVisibleOrder.clear();
    m_HierarchyVisibleBuild.clear();
    SceneSerializer::LoadFromString(world, assets, m_PrefabModeSceneSnapshot);
    m_PrefabModeSceneSnapshot.clear();
    RestorePrePrefabHistory();
    const std::string was = m_PrefabModePath;
    m_PrefabModePath.clear();
    Log::Info("Back to the scene from prefab " + ProjectPaths::Relativize(was) + ".");
}

void EditorLayer::DrawPrefabModeBar(World& world, AssetLibrary& assets) {
    if (!InPrefabMode()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // A blue frame around the whole editor, like Play mode's orange one.
    const ImU32 col = IM_COL32(90, 150, 255, 230);
    const float t = 3.0f;
    ImGui::GetForegroundDrawList()->AddRect(ImVec2(vp->Pos.x + t * 0.5f, vp->Pos.y + t * 0.5f),
                                            ImVec2(vp->Pos.x + vp->Size.x - t * 0.5f, vp->Pos.y + vp->Size.y - t * 0.5f),
                                            col, 0.0f, 0, t);

    const bool haveViewport = m_ViewportSize.x > 50.0f;
    const ImVec2 anchor = haveViewport ? ImVec2(m_ViewportPos.x + m_ViewportSize.x * 0.5f, m_ViewportPos.y + 8.0f * m_UIScale)
                                       : ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 40.0f * m_UIScale);
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##PrefabModeBar", nullptr, flags)) {
        const std::string name = std::filesystem::u8path(m_PrefabModePath).stem().u8string();
        if (ActionButton(ICON_FA_ARROW_LEFT " Scene", "Back to the scene (saves the prefab)")) {
            ExitPrefabMode(world, assets);
            ImGui::End();
            return;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.55f, 0.72f, 1.0f, 1.0f), ICON_FA_BOX_ARCHIVE "  %s%s", name.c_str(), m_Dirty ? "*" : "");
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", ProjectPaths::Relativize(m_PrefabModePath).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_Dirty);
        if (ActionButton(ICON_FA_FLOPPY_DISK " Save", "Save the prefab (Ctrl+S)")) SavePrefabMode(world);
        ImGui::EndDisabled();
    }
    ImGui::End();
}
