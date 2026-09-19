// Scene lifecycle: new/open/save, the recovery + unsaved-changes prompts, undo/redo, play
// mode enter/exit, dropped-file import, and screenshot capture. Split out of
// EditorLayer.cpp for build time (#179).

#include <iterator>
#include "MaterialAsset.h"
#include "AtomicFile.h"
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "FileDialog.h"
#include "AssetLibrary.h"
#include "World.h"
#include "Camera.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
#include "AudioEngine.h"
#include "Screenshot.h"
#include "SceneSerializer.h"
#include "AABB.h"
#include "PhysicsWorld.h"
#include "Log.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "AssetImporterInspector.h"
#include "Profiler.h"
#include "ProjectPaths.h"
#include "GLStateCache.h"
#include "Framebuffer.h"
#include "gl.h"

#include "UndoDeltaChain.h" // undo history stores JSON-Patch deltas, not full snapshots (#174)

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
#include <cmath>
#include <cctype>
#include <cstring>
#include <functional>
#include <cfloat>

using namespace EditorInternal;

namespace {
// Cheap non-cryptographic hash of a serialized scene snapshot, used only to dedupe
// consecutive undo pushes (#174 stage 1). Comparing full multi-KB/MB JSON strings byte-for-byte
// on every edit was the actual cost being avoided; this hash is only ever used to answer "did
// anything change since last time", never to reconstruct anything (stage 2 stores deltas).
// Same FNV-1a constants/style as TextureCache::HashSettings, extended to arbitrary byte spans.
uint64_t HashSceneJson(const std::string& s) {
    uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull; // FNV-1a prime
    }
    return h;
}
} // namespace

// Thin wrappers over the delta chain (UndoDeltaChain.h) - the mechanism lives there, isolated
// from EditorLayer so it can be exercised on its own; these just bind it to UndoEntry and route
// the one failure mode to the Console.
void EditorLayer::PushHistoryEntry(std::vector<UndoEntry>& stack, std::string& baseJson,
                                   UndoEntry&& entry, const std::string& newFullJson) {
    UndoDelta::Push(stack, baseJson, std::move(entry), newFullJson);
}

bool EditorLayer::PopHistoryEntry(std::vector<UndoEntry>& stack, std::string& baseJson,
                                  UndoEntry& outEntry, std::string& outFullJson) {
    const UndoDelta::PopResult r = UndoDelta::Pop(stack, baseJson, outEntry, outFullJson);
    if (r == UndoDelta::PopResult::TailDropped)
        Log::Error("Undo history: a step couldn't be reconstructed - earlier history was dropped.");
    return r != UndoDelta::PopResult::Empty;
}

void EditorLayer::ClearRedoHistory() {
    m_RedoStack.clear();
    m_RedoBaseJson.clear();
    m_RedoBaseJson.shrink_to_fit();
}

void EditorLayer::ClearUndoHistory() {
    m_UndoStack.clear();
    m_UndoBaseJson.clear();
    m_UndoBaseJson.shrink_to_fit();
    m_ContentDepth = 0; // Q6 — the live stack is empty, so is its count of real (non-selection) edits
    ClearRedoHistory();
}

std::string EditorLayer::RecoveryPathFor(const std::string& scenePath) {
    std::filesystem::path p(scenePath);
    p.replace_extension(); // "…/scene.json" -> "…/scene"
    return p.string() + ".recovery.json";
}

void EditorLayer::WriteRecoverySnapshot(const World& world, const AssetLibrary& assets) {
    if (m_CurrentScenePath.empty()) return; // untitled scene (New Scene) has no sidecar location
    const std::string path = RecoveryPathFor(m_CurrentScenePath);
    if (InPrefabMode()) { // #176 - the world holds the prefab; the scene is the snapshot
        if (m_PrefabModeSceneDirty) SceneSerializer::SaveSnapshotToFile(m_PrefabModeSceneSnapshot, assets, path);
        return;
    }
    if (SceneSerializer::Save(world, assets, path)) {
        Log::Info("Auto-save: wrote recovery snapshot (unsaved changes are safe if the editor closes unexpectedly).");
    }
}

void EditorLayer::ClearRecoverySnapshot() {
    if (m_CurrentScenePath.empty()) return;
    std::error_code ec;
    std::filesystem::remove(RecoveryPathFor(m_CurrentScenePath), ec); // absent file is not an error
}

bool EditorLayer::DoSaveAs(World& world, AssetLibrary& assets) {
    // #180 — the one choke point every save path goes through (menu, Ctrl+Shift+S, module API).
    if (InPrefabMode()) { // #176
        Log::Warn("Save As isn't available in Prefab Mode - Save writes the prefab; go Back to Scene to save the scene.");
        return false;
    }
    if (m_InPlayMode) {
        Log::Warn("Save is disabled while Playing - changes made in Play mode revert on Stop.");
        return false;
    }
    std::string path = FileDialog::SaveFile("Scene Files\0*.json\0All Files\0*.*\0", "json", m_Window);
    if (path.empty()) return false; // user cancelled
    if (!SceneSerializer::Save(world, assets, path)) return false;
    ClearRecoverySnapshot();       // clears the snapshot for the PREVIOUS path (still current here)
    m_CurrentScenePath = path;
    m_LoadFailedScenePath.clear(); // #84
    EditorSettings::Get().LastScenePath = path; // reopen this one next launch (#95)
    EditorSettings::Save();
    m_Dirty = false;
    m_SavedUndoDepth = m_ContentDepth; // this history position now matches disk
    m_AutoSaveTimer = 0.0f;
    InvalidateScenesListing(); // (#175) may have written a new file under scenes/
    return true;
}

bool EditorLayer::DoSave(World& world, AssetLibrary& assets) {
    // #180 — the action bar's Save icon and the module's document strip reached this with no
    // Play-mode check (only the File menu and Ctrl+S had one), writing the live play state
    // over the scene file. Guarding here covers every entry point.
    if (m_InPlayMode) {
        Log::Warn("Save is disabled while Playing - changes made in Play mode revert on Stop.");
        return false;
    }
    if (InPrefabMode()) return SavePrefabMode(world); // #176 - Ctrl+S / Save icon save the open prefab
    if (m_CurrentScenePath.empty()) {   // untitled -> must choose a location
        return DoSaveAs(world, assets);
    }
    if (!m_LoadFailedScenePath.empty() && m_CurrentScenePath == m_LoadFailedScenePath) {
        // #84 — never silently replace a scene that failed to load with whatever is in the
        // world now (usually nothing). Ask for a new location instead.
        Log::Warn("'" + m_CurrentScenePath + "' failed to load at startup, so it won't be overwritten - "
                  "choose where to save instead (fix or restore the original file separately).");
        return DoSaveAs(world, assets);
    }
    // #86 — a failed save (read-only file, disk full, file locked by sync/AV) must leave the
    // scene dirty and keep the recovery snapshot; the error is logged, which also raises a toast.
    if (!SceneSerializer::Save(world, assets, m_CurrentScenePath)) {
        Log::Error("Save failed - '" + m_CurrentScenePath + "' was NOT updated. Your changes are still "
                   "unsaved; try File > Save As.");
        return false;
    }
    m_Dirty = false;
    m_SavedUndoDepth = m_ContentDepth; // this history position now matches disk
    m_AutoSaveTimer = 0.0f;
    ClearRecoverySnapshot();            // the real file is now current — the snapshot is stale
    return true;
}

void EditorLayer::OnStartupSceneLoadFailed(const std::string& path) {
    m_LoadFailedScenePath = path;
    Log::Error("Scene: '" + path + "' exists but could not be loaded (see the error above). The editor "
               "started with an empty scene; the file on disk has been left untouched and Save will "
               "ask for a new location instead of overwriting it.");
}

void EditorLayer::EmergencyRecoverySave(World& world, AssetLibrary& assets) noexcept {
    try {
        if (m_InPlayMode) OnExitPlayMode(world, assets);
        if (InPrefabMode()) ExitPrefabMode(world, assets, /*save=*/false); // #176 - back to the real scene
        if (m_Dirty && !m_CurrentScenePath.empty()) {
            WriteRecoverySnapshot(world, assets);
            Log::Error("Unexpected error - wrote a recovery snapshot of the unsaved scene.");
        }
    } catch (...) {
        // The world may be what's broken; nothing more can be done safely here.
    }
}

bool EditorLayer::CrashRecoverySave(const World& world, const AssetLibrary& assets) noexcept {
    try {
        if (InPrefabMode()) { // #176 - the scene is the snapshot taken on entering Prefab Mode
            return m_PrefabModeSceneDirty && !m_CurrentScenePath.empty() &&
                   SceneSerializer::SaveSnapshotToFile(m_PrefabModeSceneSnapshot, assets, RecoveryPathFor(m_CurrentScenePath));
        }
        if (!m_Dirty || m_CurrentScenePath.empty()) return false;
        const std::string path = RecoveryPathFor(m_CurrentScenePath);
        if (m_InPlayMode)
            return !m_PlayModeSnapshot.empty() && SceneSerializer::SaveSnapshotToFile(m_PlayModeSnapshot, assets, path);
        return SceneSerializer::Save(world, assets, path);
    } catch (...) {
        return false;
    }
}

void EditorLayer::DrawRecoveryPrompt(World& world, AssetLibrary& assets) {
    if (!m_RecoveryPromptPending) return;

    if (BeginCenteredModal("Recover Unsaved Changes?")) {
        ImGui::TextUnformatted(
            "A recovery snapshot newer than the saved scene was found - the editor\n"
            "likely closed before these changes were saved.\n\n"
            "Restore the unsaved changes, or discard them and keep the saved scene?");
        ImGui::Separator();

        if (PrimaryButton("Restore", ImVec2(120.0f * m_UIScale, 0.0f))) {
            const std::string recoveryPath = RecoveryPathFor(m_CurrentScenePath);
            if (SceneSerializer::Load(world, assets, recoveryPath)) {
                CheckSceneVersionWarning();
                ClearSelection();
                ClearUndoHistory();
                m_Dirty = true; // recovered content isn't in the real scene file yet
                m_LoadFailedScenePath.clear(); // #84 — the user chose this content for the path
                m_SavedUndoDepth = -1;
                Log::Info("Restored unsaved changes from the recovery snapshot.");
            } else {
                Log::Error("Recovery snapshot could not be read - kept the saved scene instead.");
            }
            ClearRecoverySnapshot();
            m_RecoveryPromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Discard", ImVec2(120.0f * m_UIScale, 0.0f))) {
            ClearRecoverySnapshot();
            m_RecoveryPromptPending = false;
            Log::Info("Discarded the recovery snapshot; opened the saved scene.");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::CheckSceneVersionWarning() {
    std::string warning = SceneSerializer::TakeLoadWarning();
    if (!warning.empty()) m_SceneVersionWarning = std::move(warning);
}

void EditorLayer::DrawSceneVersionWarningPopup() {
    if (m_SceneVersionWarning.empty()) return;

    const char* kPopupId = "Newer Scene Format";
    ImGui::SetNextWindowSize(ImVec2(420.0f * m_UIScale, 0.0f));
    if (BeginCenteredModal(kPopupId)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f * m_UIScale);
        ImGui::TextUnformatted(m_SceneVersionWarning.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        const bool dismiss = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                             ImGui::IsKeyPressed(ImGuiKey_Escape);
        if (PrimaryButton("OK", ImVec2(-FLT_MIN, 0.0f)) || dismiss) {
            m_SceneVersionWarning.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::DrawExitPrompt() {
    if (!m_ExitPromptPending) return;

    if (BeginCenteredModal("Save changes?")) {
        std::string sceneName = std::filesystem::path(m_CurrentScenePath).filename().string();
        if (sceneName.empty()) sceneName = "Untitled";
        ImGui::Text("\"%s\" has unsaved changes.", sceneName.c_str());
        ImGui::TextUnformatted("Save them before closing?");
        ImGui::Separator();

        if (PrimaryButton("Save", ImVec2(110.0f * m_UIScale, 0.0f))) {
            m_ExitDecision = ExitDecision::SaveAndExit;
            m_ExitPromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Don't Save", ImVec2(110.0f * m_UIScale, 0.0f))) {
            m_ExitDecision = ExitDecision::DiscardAndExit;
            m_ExitDiscardChosen = true; // Shutdown() may drop the recovery snapshot (#86)
            m_ExitPromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Cancel", ImVec2(110.0f * m_UIScale, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_ExitDecision = ExitDecision::None;
            m_ExitPromptPending = false; // main sees ExitPromptActive() == false -> stays open
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::RequestNewScene(World& world, AssetLibrary& assets) {
    if (InPrefabMode()) ExitPrefabMode(world, assets); // #176
    // #85 — no scene switching while Playing: Stop would restore the pre-Play snapshot of the
    // OLD scene under the NEW scene's path (and PhysicsWorld still holds actors keyed by the old
    // scene's recycled entity ids), so the next save wrote scene A's content into B's file.
    if (m_InPlayMode) {
        Log::Warn("Stop Play mode before creating a new scene.");
        return;
    }
    if (m_Dirty) {
        m_PendingSceneSwitch = PendingSceneSwitch::New;
        m_PendingScenePath.clear();
        m_ScenePromptPending = true;
        return;
    }
    NewScene(world, assets);
}

void EditorLayer::RequestOpenScene(World& world, AssetLibrary& assets, const std::string& path) {
    if (InPrefabMode() && !path.empty()) ExitPrefabMode(world, assets); // #176
    // #85 — no scene switching while Playing: Stop would restore the pre-Play snapshot of the
    // OLD scene under the NEW scene's path (and PhysicsWorld still holds actors keyed by the old
    // scene's recycled entity ids), so the next save wrote scene A's content into B's file.
    if (m_InPlayMode) {
        Log::Warn("Stop Play mode before opening another scene.");
        return;
    }
    if (path.empty()) return; // dialog cancelled, or an empty drag payload
    if (m_Dirty) {
        m_PendingSceneSwitch = PendingSceneSwitch::Open;
        m_PendingScenePath = path;
        m_ScenePromptPending = true;
        return;
    }
    OpenScene(world, assets, path);
}

void EditorLayer::RequestRevertScene(World& world, AssetLibrary& assets) {
    if (InPrefabMode()) ExitPrefabMode(world, assets); // #176
    // #85 — no scene switching while Playing: Stop would restore the pre-Play snapshot of the
    // OLD scene under the NEW scene's path (and PhysicsWorld still holds actors keyed by the old
    // scene's recycled entity ids), so the next save wrote scene A's content into B's file.
    if (m_InPlayMode) {
        Log::Warn("Stop Play mode before reverting the scene.");
        return;
    }
    if (m_CurrentScenePath.empty()) return; // Untitled — nothing on disk to revert to
    std::error_code ec;
    if (!std::filesystem::exists(m_CurrentScenePath, ec) || ec) {
        Log::Warn("Revert Scene: '" + m_CurrentScenePath + "' no longer exists on disk.");
        return;
    }
    if (m_Dirty) { m_RevertPromptPending = true; return; }
    OpenScene(world, assets, m_CurrentScenePath);
}

void EditorLayer::DrawRevertScenePrompt(World& world, AssetLibrary& assets) {
    if (!m_RevertPromptPending) return;
    if (BeginCenteredModal("Revert Scene?##Revert")) {
        std::string name = std::filesystem::path(m_CurrentScenePath).filename().string();
        ImGui::Text("Discard unsaved changes to \"%s\"", name.c_str());
        ImGui::TextUnformatted("and reload it from disk?");
        ImGui::Separator();
        if (PrimaryButton("Revert", ImVec2(110.0f * m_UIScale, 0.0f))) {
            m_RevertPromptPending = false;
            ImGui::CloseCurrentPopup();
            OpenScene(world, assets, m_CurrentScenePath);
        }
        ImGui::SameLine();
        if (PrimaryButton("Cancel", ImVec2(110.0f * m_UIScale, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_RevertPromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// "Save changes?" before New/Open discard the current scene (#216) — same Save / Don't Save /
// Cancel shape as DrawExitPrompt, but for switching scenes rather than closing the window, so it
// runs the stashed New/Open instead of exiting. A distinct popup ID ("##SceneSwitch") keeps it
// independent of the exit prompt even though the visible title text matches.
void EditorLayer::DrawSceneSwitchPrompt(World& world, AssetLibrary& assets) {
    if (!m_ScenePromptPending) return;

    if (BeginCenteredModal("Save changes?##SceneSwitch")) {
        std::string sceneName = std::filesystem::path(m_CurrentScenePath).filename().string();
        if (sceneName.empty()) sceneName = "Untitled";
        ImGui::Text("\"%s\" has unsaved changes.", sceneName.c_str());
        ImGui::TextUnformatted("Save them before continuing?");
        ImGui::Separator();

        auto runPendingSwitch = [&]() {
            if (m_PendingSceneSwitch == PendingSceneSwitch::New) NewScene(world, assets);
            else if (m_PendingSceneSwitch == PendingSceneSwitch::Open) OpenScene(world, assets, m_PendingScenePath);
            m_PendingSceneSwitch = PendingSceneSwitch::None;
            m_PendingScenePath.clear();
        };

        if (PrimaryButton("Save", ImVec2(110.0f * m_UIScale, 0.0f))) {
            // DoSaveAs returns false if the user cancels the file dialog — in that case the
            // switch stays pending and the prompt stays open, same as a fresh Cancel would.
            // #86 — DoSave now also reports a failed write, so a scene that couldn't be saved
            // isn't silently switched away from (it would lose the unsaved changes).
            const bool saved = DoSave(world, assets);
            if (saved) {
                runPendingSwitch();
                m_ScenePromptPending = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (PrimaryButton("Don't Save", ImVec2(110.0f * m_UIScale, 0.0f))) {
            runPendingSwitch();
            m_ScenePromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Cancel", ImVec2(110.0f * m_UIScale, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_PendingSceneSwitch = PendingSceneSwitch::None;
            m_PendingScenePath.clear();
            m_ScenePromptPending = false; // abort entirely — nothing happens
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
std::vector<int> EditorLayer::CaptureSelectedOrders(const World& world) const {
    return CaptureSelectedOrders(world, GetSelectedItems());
}

std::vector<int> EditorLayer::CaptureSelectedOrders(const World& world,
                                                     const std::vector<entt::entity>& entities) const {
    std::vector<int> orders;
    for (entt::entity e : entities) {
        if (e != entt::null && world.Registry.valid(e) && world.Registry.all_of<OrderComponent>(e)) {
            orders.push_back(world.Registry.get<OrderComponent>(e).Value);
        }
    }
    return orders;
}

void EditorLayer::RestoreSelectionByOrder(World& world, const std::vector<int>& orders) {
    ClearSelection();
    // This selection change is Undo/Redo/JumpTo* restoring what an UndoEntry recorded, not a new
    // user action — swallow the next RecordSelectionHistory() poll (Q6) so it doesn't also push a
    // redundant "Select" entry (or m_SelHistory row) for a change we just drove ourselves.
    m_SelHistoryNavigating = true;
    if (orders.empty()) return;

    // OrderComponent values are assigned once per entity and unique (unlike NameComponent,
    // which two "Cube"s created via Add > Cube can share) and survive the scene JSON
    // round-trip Undo/Redo does, so - unlike a raw entt::entity, invalidated by the reload -
    // they reliably re-identify the same object. Same approach OnEnterPlayMode /
    // OnExitPlayMode already use to re-find the selection across a registry rebuild (#110).
    // Restoring by name instead let duplicate names collapse every match onto one entity, so
    // a multi-selected batch edit silently applied to the same object several times (#217).
    std::unordered_map<int, entt::entity> byOrder;
    for (auto e : world.Registry.view<OrderComponent>()) {
        byOrder[world.Registry.get<OrderComponent>(e).Value] = e;
    }

    bool first = true;
    for (int wantedOrder : orders) {
        auto it = byOrder.find(wantedOrder);
        if (it == byOrder.end()) continue; // that object doesn't exist at this point in history
        if (first) { m_Selected = it->second; first = false; }
        else m_ExtraSelection.push_back(it->second);
    }
}

void EditorLayer::PushUndo(const World& world, const std::string& label, bool selectionOnly,
                           const std::vector<int>* selectedOrdersOverride) {
    PROFILE_SCOPE("PushUndo");
    CancelEyedropper(); // #93
    // #138 - a selection-only entry never has its scene restored (Undo/Redo skip the reload for
    // it: popping the entries above it already brought the scene back to what it was when the
    // selection changed), so it doesn't need the current scene serialised - every Hierarchy /
    // viewport click used to cost a full save + diff. It records the top entry's state instead,
    // which keeps the delta chain intact (an empty patch) at the cost of a string copy.
    const bool reuseTop = selectionOnly && !m_UndoStack.empty() && !m_UndoBaseJson.empty();
    const std::string sceneJson = reuseTop ? m_UndoBaseJson
                                : m_AssetsPtr ? SceneSerializer::SaveToString(world, *m_AssetsPtr)
                                              : SceneSerializer::SaveToString(world);
    UndoEntry entry;
    entry.Hash = reuseTop ? m_UndoStack.back().Hash : HashSceneJson(sceneJson);
    entry.SelectedOrders = selectedOrdersOverride ? *selectedOrdersOverride : CaptureSelectedOrders(world);
    // Plenty of call sites fire on "field focused" / "gizmo grabbed" before anything actually
    // changes — a double-click-to-type on a Transform field lands here twice with no edit
    // between. Don't stack a byte-identical snapshot on the last one: it produced phantom
    // History entries and left extra Ctrl+Z presses that did nothing (#19 P8, #23 P23).
    // Compared by hash rather than the full JSON string (#174 stage 1) - scenes can be
    // megabytes, and this compare runs on every single edit. Also requires SelectedOrders to
    // match (Q6/Phase 6 item 6): a scene-identical push whose ONLY difference is a new selection
    // is exactly what RecordSelectionHistory's "Select" entries look like, and those must NOT be
    // deduped away — that's the whole point of unifying selection changes into this same stack.
    if (!m_UndoStack.empty() && m_UndoStack.back().Hash == entry.Hash &&
        m_UndoStack.back().SelectedOrders == entry.SelectedOrders) {
        ClearRedoHistory(); // still a fresh edit intent — a stale redo branch shouldn't survive it
        RefreshDirtyFromHistory();
        return;
    }
    entry.Label = label;
    entry.SelectionOnly = selectionOnly;
    // Branching off a mid-history position discards the redo entries — the saved state may be
    // among them, in which case there's no longer a clean point to return to (#22 P22).
    if (m_SavedUndoDepth >= 0 && !m_RedoStack.empty()) m_SavedUndoDepth = -1;
    PushHistoryEntry(m_UndoStack, m_UndoBaseJson, std::move(entry), sceneJson);
    if (!selectionOnly) m_ContentDepth++; // Q6 — only real edits count toward the dirty flag
    if (m_UndoStack.size() > kMaxHistory) {
        // Safe with the delta chain as-is: entry 0's patch only ever rebuilt entry 0 from
        // entry 1, so dropping it leaves every remaining link intact (#174 stage 2).
        const bool evictedWasContent = m_UndoStack.front().CountsAsSceneEdit();
        m_UndoStack.erase(m_UndoStack.begin());
        if (evictedWasContent) {
            if (m_SavedUndoDepth > 0) m_SavedUndoDepth--; // the whole stack shifted down by one
            m_ContentDepth--;
        }
    }
    ClearRedoHistory(); // a fresh edit invalidates whatever redo history existed
    RefreshDirtyFromHistory();
    if (!selectionOnly) m_EditPushedThisFrame = true; // tells RecordSelectionHistory not to ALSO push a Select entry
}

void EditorLayer::PushAssetUndo(const World& world, const std::string& matPath, std::string before,
                                const std::string& label) {
    CancelEyedropper(); // #93
    const std::string sceneJson = m_AssetsPtr ? SceneSerializer::SaveToString(world, *m_AssetsPtr)
                                              : SceneSerializer::SaveToString(world);
    UndoEntry entry;
    entry.Hash = HashSceneJson(sceneJson);
    entry.SelectedOrders = CaptureSelectedOrders(world);
    entry.Label = label;
    entry.AssetPath = matPath;
    entry.AssetJson = std::move(before);
    PushHistoryEntry(m_UndoStack, m_UndoBaseJson, std::move(entry), sceneJson);
    if (m_UndoStack.size() > kMaxHistory) {
        const bool evictedWasContent = m_UndoStack.front().CountsAsSceneEdit();
        m_UndoStack.erase(m_UndoStack.begin());
        if (evictedWasContent) {
            if (m_SavedUndoDepth > 0) m_SavedUndoDepth--;
            m_ContentDepth--;
        }
    }
    ClearRedoHistory();
    RefreshDirtyFromHistory();
    m_EditPushedThisFrame = true; // not also a "Select" entry
}

std::string EditorLayer::ReadTextFile(const std::string& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void EditorLayer::RestoreMaterialFile(AssetLibrary& assets, const std::string& path, const std::string& json) {
    if (!AtomicFile::WriteBytes(path, json, /*binary=*/true)) {
        Log::Error("Undo: couldn't write '" + path + "'.");
        return;
    }
    // Reload in place: renderers and the Inspector hold this same shared_ptr.
    if (auto fresh = MaterialAsset::Load(path, &assets)) {
        std::shared_ptr<MaterialAsset> live = assets.LoadMaterial(path);
        if (live && live != fresh) *live = *fresh;
    }
}

// Phase 6 item 6 / Q6 helper — see the header's comment on SelectionUndoLabel.
std::string EditorLayer::SelectionUndoLabel(const World& world) const {
    const size_t count = (m_Selected != entt::null ? 1u : 0u) + m_ExtraSelection.size();
    if (count == 0) return "Deselect";
    if (count > 1) return "Select " + std::to_string(count) + " objects";
    const auto* name = world.Registry.try_get<NameComponent>(m_Selected);
    return "Select " + (name && !name->Name.empty() ? name->Name : std::string("Object"));
}

void EditorLayer::StageUndo(const World& world) {
    CancelEyedropper(); // #93
    if (m_HasStagedUndo) return; // keep the FIRST (true pre-edit) snapshot of this interaction
    m_StagedUndoJson = m_AssetsPtr ? SceneSerializer::SaveToString(world, *m_AssetsPtr)
                                   : SceneSerializer::SaveToString(world);
    m_StagedUndoHash = HashSceneJson(m_StagedUndoJson);
    m_StagedUndoSelectedOrders = CaptureSelectedOrders(world);
    m_HasStagedUndo = true;
}

void EditorLayer::CommitStagedUndo(const World& world, const std::string& label) {
    if (!m_HasStagedUndo) return;
    m_HasStagedUndo = false;

    // If the interaction ended on the same state it began — a no-op drag, a value typed back to
    // what it was, or an input the commit path rejected (non-finite) — record nothing, so it
    // neither adds a phantom History entry nor dirties the scene (#22 P22, #23 P23, #34 D4).
    const std::string currentJson = m_AssetsPtr ? SceneSerializer::SaveToString(world, *m_AssetsPtr)
                                                : SceneSerializer::SaveToString(world);
    // Compared by hash rather than the full JSON string (#174 stage 1) - same reasoning as
    // the PushUndo dedupe above.
    if (HashSceneJson(currentJson) == m_StagedUndoHash) {
        m_StagedUndoJson.clear();
        m_StagedUndoHash = 0;
        m_StagedUndoSelectedOrders.clear();
        return;
    }

    // The staged snapshot is the PRE-edit state, so that's the full state this entry stands for.
    const std::string stagedJson = std::move(m_StagedUndoJson);
    m_StagedUndoJson.clear();
    UndoEntry entry;
    entry.Hash = m_StagedUndoHash;
    entry.SelectedOrders = std::move(m_StagedUndoSelectedOrders);
    entry.Label = label;
    if (m_SavedUndoDepth >= 0 && !m_RedoStack.empty()) m_SavedUndoDepth = -1;
    PushHistoryEntry(m_UndoStack, m_UndoBaseJson, std::move(entry), stagedJson);
    m_ContentDepth++; // Q6 — a staged edit is always a real edit, never SelectionOnly
    if (m_UndoStack.size() > kMaxHistory) {
        const bool evictedWasContent = m_UndoStack.front().CountsAsSceneEdit();
        m_UndoStack.erase(m_UndoStack.begin());
        if (evictedWasContent) {
            if (m_SavedUndoDepth > 0) m_SavedUndoDepth--;
            m_ContentDepth--;
        }
    }
    ClearRedoHistory();
    RefreshDirtyFromHistory();
    m_HasStagedUndo = false;
    m_EditPushedThisFrame = true; // Q6 — see PushUndo's matching line
}

void EditorLayer::Undo(World& world, AssetLibrary& assets) {
    CancelEyedropper(); // #93 — the registry is about to be rebuilt
    if (m_UndoStack.empty()) return;
    // #91 — Undo reloads the whole registry from a snapshot, which under a live PhysicsWorld
    // (actors keyed by entity id) scrambles the simulation; and anything done in Play reverts on
    // Stop anyway.
    if (m_InPlayMode) { Log::Info("Undo is disabled while Playing - Stop reverts Play-mode changes."); return; }

    const std::string currentJson = SceneSerializer::SaveToString(world, assets);
    UndoEntry redoEntry;
    redoEntry.Hash = HashSceneJson(currentJson);
    redoEntry.SelectedOrders = CaptureSelectedOrders(world);
    redoEntry.Label = m_UndoStack.back().Label; // the action Redo would re-apply from here
    redoEntry.SelectionOnly = m_UndoStack.back().SelectionOnly; // Q6 — carry the flag across stacks
    // #107 — an asset entry's redo side holds the file as it is NOW, before we roll it back.
    redoEntry.AssetPath = m_UndoStack.back().AssetPath;
    if (!redoEntry.AssetPath.empty()) redoEntry.AssetJson = ReadTextFile(redoEntry.AssetPath);
    PushHistoryEntry(m_RedoStack, m_RedoBaseJson, std::move(redoEntry), currentJson);

    UndoEntry entry;
    std::string targetJson;
    if (!PopHistoryEntry(m_UndoStack, m_UndoBaseJson, entry, targetJson)) return;
    if (entry.CountsAsSceneEdit()) m_ContentDepth--; // Q6 — a real-edit entry just left the live stack
    if (!entry.AssetPath.empty()) RestoreMaterialFile(assets, entry.AssetPath, entry.AssetJson); // #107
    // An asset-only step leaves the scene as it is — skip the full registry rebuild. So does a
    // selection-only step (#138): its stored state is only a placeholder (see PushUndo).
    if (!entry.SelectionOnly && (entry.AssetPath.empty() || HashSceneJson(currentJson) != entry.Hash)) {
        if (!SceneSerializer::LoadFromString(world, assets, targetJson)) {
            // #83 — a snapshot that can't be applied must not leave a half-loaded world.
            Log::Error("Undo failed - the scene was left as it was.");
            SceneSerializer::LoadFromString(world, assets, currentJson);
        }
    }
    RestoreSelectionByOrder(world, entry.SelectedOrders);
    InvalidateModelThumbnail(); // LoadFromString may rebuild the asset library
    RefreshDirtyFromHistory(); // "*" clears when history returns to the last-saved point (#22 P22)
}

void EditorLayer::Redo(World& world, AssetLibrary& assets) {
    CancelEyedropper(); // #93
    if (m_RedoStack.empty()) return;
    if (m_InPlayMode) { Log::Info("Redo is disabled while Playing - Stop reverts Play-mode changes."); return; } // #91

    const std::string currentJson = SceneSerializer::SaveToString(world, assets);
    UndoEntry undoEntry;
    undoEntry.Hash = HashSceneJson(currentJson);
    undoEntry.SelectedOrders = CaptureSelectedOrders(world);
    undoEntry.Label = m_RedoStack.back().Label;
    undoEntry.SelectionOnly = m_RedoStack.back().SelectionOnly; // Q6 — carry the flag across stacks
    undoEntry.AssetPath = m_RedoStack.back().AssetPath; // #107 — see Undo()
    if (!undoEntry.AssetPath.empty()) undoEntry.AssetJson = ReadTextFile(undoEntry.AssetPath);
    if (undoEntry.CountsAsSceneEdit()) m_ContentDepth++; // Q6 — a real-edit entry is returning to the live stack
    PushHistoryEntry(m_UndoStack, m_UndoBaseJson, std::move(undoEntry), currentJson);

    UndoEntry entry;
    std::string targetJson;
    if (!PopHistoryEntry(m_RedoStack, m_RedoBaseJson, entry, targetJson)) return;
    if (!entry.AssetPath.empty()) RestoreMaterialFile(assets, entry.AssetPath, entry.AssetJson); // #107
    if (!entry.SelectionOnly && (entry.AssetPath.empty() || HashSceneJson(currentJson) != entry.Hash)) { // #138
        if (!SceneSerializer::LoadFromString(world, assets, targetJson)) {
            Log::Error("Redo failed - the scene was left as it was."); // #83
            SceneSerializer::LoadFromString(world, assets, currentJson);
        }
    }
    RestoreSelectionByOrder(world, entry.SelectedOrders);
    InvalidateModelThumbnail(); // LoadFromString may rebuild the asset library
    RefreshDirtyFromHistory(); // "*" clears when history returns to the last-saved point (#22 P22)
}

void EditorLayer::JumpToUndoEntry(World& world, AssetLibrary& assets, size_t undoStackIndex) {
    if (undoStackIndex >= m_UndoStack.size()) return;
    while (m_UndoStack.size() > undoStackIndex) {
        Undo(world, assets);
    }
}

void EditorLayer::JumpToRedoEntry(World& world, AssetLibrary& assets, size_t redoStackIndex) {
    if (redoStackIndex >= m_RedoStack.size()) return;
    size_t steps = m_RedoStack.size() - redoStackIndex;
    for (size_t i = 0; i < steps; ++i) Redo(world, assets);
}

void EditorLayer::OnEnterPlayMode(const World& world) {
    CancelEyedropper(); // #93
    m_InPlayMode = true;
    m_PlayModeSnapshot = SceneSerializer::SaveToString(world);
    m_PrePlayHistory = { m_UndoStack, m_RedoStack, m_UndoBaseJson, m_RedoBaseJson,
                         m_ContentDepth, m_SavedUndoDepth, true }; // #91
    // Remember what's selected by OrderComponent value, not entt id: Stop rebuilds the whole
    // registry and entt recycles ids, so a retained handle can pass valid() yet denote a
    // different object afterward (#110).
    m_PlaySelectionOrders.clear();
    for (entt::entity e : GetSelectedItems()) {
        if (const auto* o = world.Registry.try_get<OrderComponent>(e))
            m_PlaySelectionOrders.push_back(o->Value);
    }

    // #199: AudioSourceComponent used to do nothing at runtime - start every "Play On Start"
    // source now. Skips InactiveTag entities the same way other Play-mode iteration does (a
    // disabled source shouldn't be heard). Positioned via the cached world transform (#173/#224)
    // so a source under a moved/rotated parent plays from its actual world position, not its
    // parent-local one.
    m_PlayModeAudioHandles.clear();
    auto audioView = world.Registry.view<const AudioSourceComponent>();
    for (entt::entity e : audioView) {
        if (world.Registry.all_of<InactiveTag>(e)) continue;
        const auto& audio = audioView.get<const AudioSourceComponent>(e);
        if (!audio.PlayOnStart || audio.SoundPath.empty()) continue;
        AudioEngine::SoundHandle handle = AudioEngine::Play(audio.SoundPath, audio.Volume, audio.Loop,
                                                            (AudioEngine::Bus)std::clamp(audio.Output, 0, AudioEngine::kBusCount - 1));
        if (handle == AudioEngine::InvalidHandle) continue;
        glm::vec3 worldPos = glm::vec3(world.GetCachedWorldTransform(e)[3]);
        AudioEngine::SetPosition(handle, worldPos);
        m_PlayModeAudioHandles[e] = handle;
    }

    // Stand up the PhysX world for this Play session (#185): a static actor per collider plus
    // a controller manager for the Player's capsule (created lazily on the first Player::Update).
    // Torn down in OnExitPlayMode.
    PhysicsWorld::Create(world);

    Log::Info("Entered play mode - scene state saved, changes will be reverted on exit.");
}

void EditorLayer::OnExitPlayMode(World& world, AssetLibrary& assets) {
    CancelEyedropper(); // #93
    m_InPlayMode = false;

    // Always tear the PhysX world down, even on the snapshot-empty early-out below — Create()
    // may have run regardless (#185). Destroy() is idempotent when nothing is active.
    PhysicsWorld::Destroy();

    if (m_PlayModeSnapshot.empty()) return;

    // Stop exactly the voices Play On Start began (not AudioEngine::StopAll(), which would also
    // cut off an unrelated editor preview sound started while Play was running).
    for (auto& [entity, handle] : m_PlayModeAudioHandles) AudioEngine::Stop(handle);
    m_PlayModeAudioHandles.clear();

    SceneSerializer::LoadFromString(world, assets, m_PlayModeSnapshot);
    m_PlayModeSnapshot.clear();

    // #91 — drop every history entry recorded during Play (their snapshots are play state; an
    // Undo after Stop used to load one straight into the edit scene) by restoring the pre-Play
    // history wholesale.
    if (m_PrePlayHistory.Valid) {
        m_UndoStack = std::move(m_PrePlayHistory.Undo);
        m_RedoStack = std::move(m_PrePlayHistory.Redo);
        m_UndoBaseJson = std::move(m_PrePlayHistory.UndoBase);
        m_RedoBaseJson = std::move(m_PrePlayHistory.RedoBase);
        m_ContentDepth = m_PrePlayHistory.ContentDepth;
        m_SavedUndoDepth = m_PrePlayHistory.SavedDepth;
        m_PrePlayHistory = {};
        m_HasStagedUndo = false;
        m_StagedUndoJson.clear();
        RefreshDirtyFromHistory();
    }

    // Drop every retained handle before it can rebind to a recycled id (#110).
    ClearSelection();
    m_HierarchyVisibleOrder.clear();
    m_HierarchyVisibleBuild.clear();

    // Re-resolve the pre-Play selection against the freshly rebuilt entities by OrderComponent.
    if (!m_PlaySelectionOrders.empty()) {
        std::unordered_map<int, entt::entity> byOrder;
        for (entt::entity e : world.Registry.view<OrderComponent>())
            byOrder[world.Registry.get<OrderComponent>(e).Value] = e;
        bool first = true;
        for (int ord : m_PlaySelectionOrders) {
            auto it = byOrder.find(ord);
            if (it == byOrder.end() || !world.Registry.valid(it->second)) continue;
            SelectItem(it->second, /*addToSelection=*/!first);
            first = false;
        }
        m_PlaySelectionOrders.clear();
    }
    // Deliberately does NOT set m_Dirty: the scene is back exactly as it was before Play, so
    // there's nothing new to save — the same reason Unity doesn't dirty a scene on play/stop.
    Log::Info("Exited play mode - scene state restored.");
}
void EditorLayer::NewScene(World& world, AssetLibrary& assets) {
    CancelEyedropper(); // #93
    // #85 — no scene switching while Playing: Stop would restore the pre-Play snapshot of the
    // OLD scene under the NEW scene's path (and PhysicsWorld still holds actors keyed by the old
    // scene's recycled entity ids), so the next save wrote scene A's content into B's file.
    if (m_InPlayMode) {
        Log::Warn("Stop Play mode before creating a new scene.");
        return;
    }
    world = World();
    InvalidateModelThumbnail();
    ClearSelection();
    m_SoloLights.clear();
    m_MutedLights.clear();
    m_LookThroughActive = false;
    m_LookThroughLight = entt::null;
    m_LookThroughMoved = false;
    m_PendingLookThrough = entt::null;
    ClearUndoHistory();
    m_AutoSaveTimer = 0.0f;

    // Write the fresh scene to disk right away — the first free "Untitled N.json" under
    // project/scenes/ — so it shows in the Asset Browser's Scenes folder immediately and can be
    // renamed / deleted / reopened like any other scene.
    std::error_code ec;
    const std::filesystem::path dir = ProjectPaths::Resolve("scenes");
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path scenePath = dir / "Untitled.json";
    for (int n = 2; std::filesystem::exists(scenePath, ec); ++n)
        scenePath = dir / ("Untitled " + std::to_string(n) + ".json");

    const std::string pathStr = scenePath.generic_string();
    if (SceneSerializer::Save(world, assets, pathStr)) {
        // Only now that the fresh scene is safely on disk do we drop the OUTGOING scene's
        // recovery snapshot (#216) — while m_CurrentScenePath still names the outgoing scene,
        // so a failed write below leaves that snapshot in place as the way back.
        ClearRecoverySnapshot();
        m_CurrentScenePath = pathStr;
        m_LoadFailedScenePath.clear(); // #84
        m_Dirty = false;                       // matches disk
        m_SavedUndoDepth = m_ContentDepth;
        EditorSettings::Get().LastScenePath = pathStr;
        EditorSettings::Save();
        InvalidateScenesListing(); // (#175) wrote a new file under scenes/
    } else {
        // Couldn't write (read-only project dir, …) — fall back to untitled-in-memory: Save
        // prompts for a location, main.cpp skips save-on-exit while the path is empty. The
        // outgoing scene's recovery snapshot is deliberately left alone: the fresh scene
        // couldn't be persisted, so it's still the only way back if this session is lost too.
        Log::Error("New Scene: couldn't create '" + pathStr + "' — scene is untitled/in-memory.");
        m_CurrentScenePath.clear();
        m_Dirty = true;
        m_SavedUndoDepth = -1;
    }
}

void EditorLayer::OpenScene(World& world, AssetLibrary& assets, const std::string& path) {
    CancelEyedropper(); // #93
    // #85 — no scene switching while Playing: Stop would restore the pre-Play snapshot of the
    // OLD scene under the NEW scene's path (and PhysicsWorld still holds actors keyed by the old
    // scene's recycled entity ids), so the next save wrote scene A's content into B's file.
    if (m_InPlayMode) {
        Log::Warn("Stop Play mode before opening another scene.");
        return;
    }
    if (path.empty() || !SceneSerializer::Load(world, assets, path)) return;
    m_LoadFailedScenePath.clear(); // #84 — a real scene is loaded now
    InvalidateModelThumbnail();
    ClearRecoverySnapshot(); // drop the outgoing scene's snapshot before switching away from it
    m_CurrentScenePath = path;
    EditorSettings::Get().LastScenePath = path; // reopen this one next launch (#95)
    EditorSettings::Save();
    ClearSelection();
    m_SoloLights.clear();
    m_MutedLights.clear();
    m_LookThroughActive = false;
    m_LookThroughLight = entt::null;
    m_LookThroughMoved = false;
    m_PendingLookThrough = entt::null;
    ClearUndoHistory();
    m_Dirty = false;
    m_SavedUndoDepth = 0; // freshly loaded — empty history == on disk
    m_AutoSaveTimer = 0.0f;
    Log::Info("Opened scene '" + path + "'.");
}

// Imports one file (never a directory) into `targetFolder` - the virtual Asset Browser folder
// it should be filed under, '/'-joined the same way m_CurrentAssetFolder is. Factored out of
// HandleDroppedFiles so a dropped folder's contents can each land in their own mirrored
// subfolder instead of everything collapsing into whichever folder happened to be open.
std::string EditorLayer::CopyAssetIntoProject(const std::string& sourcePath, const std::string& subfolder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path abs = fs::absolute(sourcePath, ec);
    if (ec) return sourcePath;

    // Already under the project root (re-importing something the library already owns, or
    // dragging a file from inside project/ itself) — reference it in place. Re-organizing an
    // existing asset must never silently duplicate its file.
    const std::string root = fs::path(ProjectPaths::Root()).lexically_normal().generic_string();
    const std::string absStr = abs.lexically_normal().generic_string();
    if (absStr == root || absStr.rfind(root + "/", 0) == 0) return sourcePath;

    const std::string destDir = ProjectPaths::Resolve("assets/" + subfolder);
    fs::create_directories(destDir, ec);
    if (ec) {
        Log::Error("Couldn't create '" + destDir + "' (" + ec.message() + ") - importing '" +
                   sourcePath + "' from its original location instead.");
        return sourcePath;
    }

    // Name collision — append " (2)", " (3)"... rather than overwrite a different file that
    // happens to share a name, matching the naming convention CreateFolder/Duplicate already use
    // elsewhere in the Asset Browser.
    const std::string stem = abs.stem().string();
    const std::string ext = abs.extension().string();

    // #124 — a model that needs companion files (glTF .bin / images, OBJ .mtl, external
    // textures) gets its own folder, assets/models/<name>/, with those files copied alongside in
    // their original relative layout. Copying the model file alone left glTF unimportable and
    // OBJ / FBX untextured.
    std::vector<std::pair<std::string, std::string>> deps;
    if (subfolder == "models") deps = Model::SourceDependencies(abs.string());
    if (!deps.empty()) {
        fs::path folder = fs::path(destDir) / stem;
        for (int n = 2; fs::exists(folder, ec); ++n) folder = fs::path(destDir) / (stem + " (" + std::to_string(n) + ")");
        fs::create_directories(folder, ec);
        const fs::path dest = folder / abs.filename();
        if (!ec) fs::copy_file(abs, dest, ec);
        if (ec) {
            Log::Error("Couldn't copy '" + sourcePath + "' into the project (" + ec.message() +
                       ") - importing from its original location instead.");
            return sourcePath;
        }
        int copied = 0;
        for (const auto& [src, rel] : deps) {
            const fs::path to = (folder / fs::path(rel)).lexically_normal();
            std::error_code dec;
            fs::create_directories(to.parent_path(), dec);
            if (!dec && !fs::exists(to, dec)) fs::copy_file(src, to, dec);
            if (dec) Log::Warn("Import: couldn't copy '" + src + "' (" + dec.message() + ").");
            else ++copied;
        }
        Log::Info("Copied '" + abs.filename().string() + "' and " + std::to_string(copied) +
                  " companion file(s) into '" + folder.generic_string() + "'.");
        return dest.generic_string();
    }

    fs::path dest = fs::path(destDir) / abs.filename();
    for (int n = 2; fs::exists(dest, ec); ++n) {
        dest = fs::path(destDir) / (stem + " (" + std::to_string(n) + ")" + ext);
    }

    fs::copy_file(abs, dest, ec);
    if (ec) {
        Log::Error("Couldn't copy '" + sourcePath + "' into the project (" + ec.message() +
                   ") - importing from its original location instead.");
        return sourcePath;
    }
    return dest.generic_string();
}

void EditorLayer::ImportFileIntoProject(World& world, AssetLibrary& assets, const std::string& path) {
    Camera unusedCamera; // ImportDroppedFile keeps a Camera& only for signature symmetry
    ImportDroppedFile(world, assets, m_EditorCameraPtr ? *m_EditorCameraPtr : unusedCamera, path, m_CurrentAssetFolder);
}

void EditorLayer::ImportDroppedFile(World& world, AssetLibrary& assets, Camera& editorCamera,
    const std::string& path, const std::string& targetFolder) {
    (void)editorCamera; // kept for signature symmetry with HandleDroppedFiles; not needed here
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::string name = std::filesystem::path(path).stem().string();

    Log::Info("Importing '" + path + "'...");

    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
        // Phase 5 item 11 — copied into project/assets/models/ first (unless already inside the
        // project), rather than left referencing wherever the source file happened to sit.
        std::string projectPath = CopyAssetIntoProject(path, "models");
        // Imports into the library only - it shows up in the Asset Browser, nothing more.
        // Deliberately NOT placed into the scene: that used to happen automatically here, but
        // it meant every dropped/imported model needed an undo (or a manual delete) if you only
        // wanted it available to drag in later. Explicit placement is now always the Asset
        // Browser -> Viewport drag (DrawViewportDropTarget's live ghost preview).
        assets.LoadModel(projectPath);
        assets.SetAssetFolder(projectPath, targetFolder);
        Log::Info("Imported model '" + name + "'.");
    } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
        std::string projectPath = CopyAssetIntoProject(path, "textures");
        auto tex = assets.LoadTexture(projectPath);
        if (tex) {
            assets.SetAssetFolder(projectPath, targetFolder);
            Log::Info("Imported texture '" + name + "'.");
        } else {
            Log::Error("Failed to load texture '" + path + "'.");
        }
    } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") {
        std::string projectPath = CopyAssetIntoProject(path, "audio");
        if (AudioEngine::Load(projectPath)) {
            assets.RegisterSound(projectPath);
            assets.SetAssetFolder(projectPath, targetFolder);
            Log::Info("Imported sound '" + name + "'.");
        } else {
            Log::Error("Failed to load sound '" + path + "'.");
        }
    } else if (ext == ".json") {
        // A dropped scene file offers to open it rather than silently doing nothing —
        // "import" has no other meaning for a whole scene.
        RequestOpenScene(world, assets, path);
    } else if (ext == ".prefab") {
        entt::entity e = SceneSerializer::InstantiatePrefab(world, assets, path);
        if (e != entt::null) {
            UniquifyName(world, e);
            assets.RegisterPrefab(path);
            assets.SetAssetFolder(path, targetFolder);
            SelectItem(e, false);
            Log::Info("Instantiated prefab '" + name + "'.");
        } else {
            Log::Error("Failed to load prefab '" + path + "'.");
        }
    } else if (ext == ".tif" || ext == ".tiff") {
        Log::Warn("Skipped '" + path + "' - TIFF isn't supported directly. Convert it to PNG first (see the TifSplitter tool) and drop that instead.");
    } else {
        Log::Warn("Don't know how to import '" + path + "' (unrecognized extension \"" + ext + "\").");
    }
}

void EditorLayer::HandleDroppedFiles(World& world, AssetLibrary& assets, Camera& editorCamera,
    bool editorUIVisible, const std::vector<std::string>& paths) {
    (void)world; (void)editorCamera; // only queued here; the actual import (ImportDroppedFile) uses them later
    if (!editorUIVisible) {
        Log::Warn("Ignored " + std::to_string(paths.size()) + " dropped file(s) - restore the editor panels to import assets.");
        return;
    }

    // Expanding a dropped folder's structure and resolving every file's target virtual folder
    // is pure filesystem traversal (no GL calls) - cheap and safe to do synchronously, right
    // here. Only the actual per-file import (which DOES touch GL, via Texture/Model loading)
    // gets deferred to the queue, drained a few at a time from EditorLayer::Draw.
    std::vector<std::string> toEnqueue;
    auto queueFile = [&](const std::string& filePath, const std::string& targetFolder) {
        m_ImportTargetFolder[filePath] = targetFolder;
        toEnqueue.push_back(filePath);
    };

    for (const std::string& path : paths) {
        std::error_code isDirErr;
        if (!std::filesystem::is_directory(path, isDirErr) || isDirErr) {
            queueFile(path, m_CurrentAssetFolder);
            continue;
        }

        // A dropped folder mirrors its own structure into the Asset Browser rather than
        // dumping every file it contains flat into whichever folder is currently open - e.g.
        // dropping "BuildingKit" (containing Meshes/ and Textures/Walls/) creates matching
        // "BuildingKit", "BuildingKit/Meshes", "BuildingKit/Textures/Walls" virtual folders
        // under the current one, and files land in the folder that mirrors where they sat on
        // disk. GLFW hands directory drops through the exact same path list as files - nothing
        // upstream of this treats them differently, so without this branch a dropped folder
        // just fell through to the "unrecognized extension" case below (a folder path has no
        // extension) and silently did nothing.
        std::filesystem::path root(path);
        std::string rootFolder = m_CurrentAssetFolder.empty()
            ? root.filename().string()
            : m_CurrentAssetFolder + "/" + root.filename().string();
        assets.CreateFolder(rootFolder);

        std::error_code walkErr;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, walkErr)) {
            std::error_code relErr;
            std::filesystem::path rel = std::filesystem::relative(entry.path(), root, relErr);
            if (relErr) continue;

            // Virtual folder path mirroring this entry's position under rootFolder - '/'
            // regardless of the OS path separator, since that's what the Asset Browser expects.
            std::string relFolder;
            for (const auto& part : rel.parent_path()) {
                if (!relFolder.empty()) relFolder += "/";
                relFolder += part.string();
            }
            std::string virtualFolder = relFolder.empty() ? rootFolder : rootFolder + "/" + relFolder;

            if (entry.is_directory()) {
                assets.CreateFolder(virtualFolder + "/" + entry.path().filename().string());
            } else if (entry.is_regular_file()) {
                assets.CreateFolder(virtualFolder);
                queueFile(entry.path().string(), virtualFolder);
            }
        }
        if (walkErr) Log::Warn("Folder scan of '" + path + "' reported: " + walkErr.message());
    }

    m_ImportQueue.Enqueue(toEnqueue);
}
void EditorLayer::OnCaptureTaken() {
    const auto& s = EditorSettings::Get();
    if (s.CaptureFlash) m_CaptureFlashT = 1.0f;
    if (s.CaptureSound) {
        std::string clip = Screenshot::ShutterClipPath();
        if (!clip.empty()) AudioEngine::Play(clip, 0.6f);
    }
}

void EditorLayer::OnCaptureDone(const std::string& path, int w, int h) {
    m_LastCapturePath = path;
    if (path.empty()) {
        Log::Error("Screenshot failed — see Console"); // was a fading toast; capture failures are rare enough to just log
        return;
    }
    InvalidateShotsListing(); // (#175) a new file just landed under screenshots/
    PushCaptureNotification(path, w, h);
}

// Phase 3 item 9 (audit #5 Appendix A #8) — one dismissible card per capture, replacing the old
// fading/click-through toast. Capped so a capture spree doesn't grow the stack forever; the
// thumbnail load is a small (128px-capped) synchronous decode, the same cost the Asset Browser's
// own Screenshots-folder grid already pays per shot.
void EditorLayer::PushCaptureNotification(const std::string& path, int w, int h) {
    EditorNotification n;
    n.Level = NotificationLevel::Success;
    n.Title = std::filesystem::path(path).filename().string();
    n.Subtitle = std::to_string(w) + "x" + std::to_string(h);
    n.FilePath = path;
    n.Thumbnail = LoadScreenshotTexture(path, 128);

    m_Notifications.insert(m_Notifications.begin(), std::move(n));
    constexpr size_t kMaxNotifications = 4;
    if (m_Notifications.size() > kMaxNotifications) m_Notifications.resize(kMaxNotifications);
}

// Phase 6 item 14 — the generic entry point: no thumbnail, no FilePath (nothing to open/show in
// folder for a log-sourced warning). Only Warning/Error bump the bell's unread badge — Info never
// reaches here (PollLogNotifications only forwards Warning/Error) and Success is captures, which
// already have their own always-visible card.
void EditorLayer::PushNotification(NotificationLevel level, const std::string& title, const std::string& subtitle) {
    EditorNotification n;
    n.Level = level;
    n.Title = title;
    n.Subtitle = subtitle;
    m_Notifications.insert(m_Notifications.begin(), std::move(n));
    constexpr size_t kMaxNotifications = 8; // a little deeper than captures alone needed
    if (m_Notifications.size() > kMaxNotifications) m_Notifications.resize(kMaxNotifications);
    if (level == NotificationLevel::Warning || level == NotificationLevel::Error) ++m_NotificationUnreadCount;
}

// Diffs Log::Revision() once a frame instead of re-scanning the whole ring buffer. A message
// that only repeats (Log::Push's dedup) bumps the revision without adding a new entry, so this
// tracks the last entry Seq handled, not the revision counter itself; a pure repeat is silently
// skipped (the first occurrence already notified). Seq, not a vector index: the log trims old
// entries, and an index-based cursor used to reset to 0 then and re-toast every warning and
// error still in the buffer (#182).
void EditorLayer::PollLogNotifications() {
    const unsigned int rev = Log::Revision();
    if (rev == m_LastLogRevisionSeen) return;
    m_LastLogRevisionSeen = rev;

    const auto& entries = Log::Entries();
    // New entries are always at the back; walk back to the first one not yet handled.
    size_t first = entries.size();
    while (first > 0 && entries[first - 1].Seq > m_LastLogSeqSeen) --first;
    if (!entries.empty()) m_LastLogSeqSeen = std::max(m_LastLogSeqSeen, entries.back().Seq);
    for (size_t i = first; i < entries.size(); ++i) {
        const LogEntry& e = entries[i];
        if (e.Level != LogLevel::Warning && e.Level != LogLevel::Error) continue;
        std::string title = e.Message;
        constexpr size_t kMaxTitleLen = 80;
        if (title.size() > kMaxTitleLen) { title.resize(kMaxTitleLen - 1); title += "\xE2\x80\xA6"; }
        PushNotification(e.Level == LogLevel::Error ? NotificationLevel::Error : NotificationLevel::Warning,
                          title, e.Time);
    }
}

// Called once per frame from Draw() — just the fading white flash over the Scene viewport now;
// the toast it used to also draw is DrawNotifications' job below.
void EditorLayer::DrawCaptureFeedback(float dt) {
    if (m_CaptureFlashT <= 0.0f) return;
    if (m_ViewportSize.x < 1.0f || m_ViewportSize.y < 1.0f) return;

    ImGuiWindow* sceneWin = ImGui::FindWindowByName("Scene");
    ImDrawList* dl = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    const ImVec2 mn(m_ViewportPos.x, m_ViewportPos.y);
    const ImVec2 mx(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y);
    dl->PushClipRect(mn, mx, true);

    m_CaptureFlashT -= dt / 0.35f;
    float a = m_CaptureFlashT;
    a = a < 0.0f ? 0.0f : a * a; // ease out
    dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, (int)(a * 220.0f)));

    dl->PopClipRect();
}

// The notification stack itself — a real ImGui window (unlike the old toast, which was bare
// draw-list rect+text with nothing underneath to click), anchored to the Scene viewport's
// bottom-right corner. History used to pin there too (Phase 3 item 8 made it a dockable panel),
// so the corner is free now. Persistent: cards sit until dismissed, not timed out.
void EditorLayer::DrawNotifications() {
    if (m_Notifications.empty()) return;
    if (!m_SceneViewportVisible || m_ViewportSize.x < 1.0f || m_ViewportSize.y < 1.0f) return;
    if (m_HideOverlaysThisFrame) return;

    const float margin = 14.0f * m_UIScale;
    const float statusBarH = ImGui::GetTextLineHeight() + 8.0f * m_UIScale;
    const float thumbSize = 48.0f * m_UIScale;
    const float cardWidth = 260.0f * m_UIScale;

    ImGui::SetNextWindowPos(ImVec2(m_ViewportPos.x + m_ViewportSize.x - margin,
                                   m_ViewportPos.y + m_ViewportSize.y - margin - statusBarH),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.0f); // cards paint their own plates; the stack window itself stays invisible
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar;
    ImGui::Begin("##Notifications", nullptr, flags);

    // Content is 3 stacked rows beside the thumbnail (title, subtitle, button row) — the card
    // has to be at least that tall, not just as tall as the thumbnail (#0 attempt clipped the
    // button row entirely: thumbSize alone undershoots 3 text/button lines at any normal font
    // size).
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float btnH = ImGui::GetFrameHeight();
    const ImGuiStyle& cardStyle = ImGui::GetStyle();
    const float contentH = lineH * 2.0f + btnH;
    const float cardH = std::max(thumbSize, contentH) + cardStyle.WindowPadding.y * 2.0f;
    const float dismissSize = 18.0f * m_UIScale;

    int dismissIndex = -1;
    for (int i = 0; i < (int)m_Notifications.size(); ++i) {
        EditorNotification& n = m_Notifications[i];
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorUIPrimitives::kHudPlateColor);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f * m_UIScale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * m_UIScale, 8.0f * m_UIScale));
        ImGui::BeginChild("##card", ImVec2(cardWidth, cardH),
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);

        if (n.Thumbnail && n.Thumbnail->GLHandle()) {
            const float aspect = (float)n.Thumbnail->Width() / std::max(1, n.Thumbnail->Height());
            const ImVec2 imgSize = aspect >= 1.0f ? ImVec2(thumbSize, thumbSize / aspect)
                                                   : ImVec2(thumbSize * aspect, thumbSize);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (contentH - imgSize.y) * 0.5f);
            ImGui::Image((ImTextureID)(intptr_t)n.Thumbnail->GLHandle(), imgSize);
        } else {
            // Log-sourced entries (item 14): a level-coloured glyph stands in for the missing
            // thumbnail instead of blank space. Reserve the same box the thumbnail would've used
            // (via Dummy) so the title/subtitle column lines up identically either way, then paint
            // the glyph centred on top of it.
            const ImVec2 boxMin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(thumbSize, thumbSize));
            const char* glyph = n.Level == NotificationLevel::Error ? ICON_FA_CIRCLE_XMARK
                               : n.Level == NotificationLevel::Warning ? ICON_FA_TRIANGLE_EXCLAMATION
                               : ICON_FA_CIRCLE_CHECK;
            const ImVec4 col = n.Level == NotificationLevel::Error ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f)
                              : n.Level == NotificationLevel::Warning ? ImVec4(0.95f, 0.75f, 0.25f, 1.0f)
                              : ImVec4(0.4f, 0.8f, 0.5f, 1.0f);
            const ImVec2 glyphSize = ImGui::CalcTextSize(glyph);
            const ImVec2 glyphPos(boxMin.x + (thumbSize - glyphSize.x) * 0.5f,
                                  boxMin.y + (thumbSize - glyphSize.y) * 0.5f);
            ImGui::GetWindowDrawList()->AddText(glyphPos, ImGui::GetColorU32(col), glyph);
        }
        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::kHudTextColor);
        ImGui::TextUnformatted(n.Title.c_str());
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::kHudTextDisabledColor);
        ImGui::TextUnformatted(n.Subtitle.c_str());
        ImGui::PopStyleColor();

        if (!n.FilePath.empty()) {
            if (ActionButton(ICON_FA_UP_RIGHT_FROM_SQUARE, "Open", false, ImVec2(0, 0)))
                Screenshot::OpenFile(n.FilePath);
            ImGui::SameLine(0.0f, 4.0f);
            if (ActionButton(ICON_FA_FOLDER_OPEN, "Show in folder", false, ImVec2(0, 0)))
                Screenshot::ShowInFolder(n.FilePath);
            ImGui::SameLine(0.0f, 4.0f);
            if (ActionButton(ICON_FA_COPY, "Copy path", false, ImVec2(0, 0)))
                ImGui::SetClipboardText(n.FilePath.c_str());
        }
        ImGui::EndGroup();

        // Dismiss — pinned to the card's own top-right corner via an absolute cursor position
        // (the child's own screen rect, known now that BeginChild has run) rather than chained
        // off the content above via SameLine, so it lands consistently regardless of how tall
        // the title/subtitle/button rows above actually came out.
        {
            const ImVec2 childPos = ImGui::GetWindowPos();
            const ImVec2 childSize = ImGui::GetWindowSize();
            ImGui::SetCursorScreenPos(ImVec2(childPos.x + childSize.x - dismissSize - 4.0f * m_UIScale,
                                             childPos.y + 4.0f * m_UIScale));
            if (ActionButton(ICON_FA_XMARK, "Dismiss", false, ImVec2(dismissSize, dismissSize)))
                dismissIndex = i;
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::End();

    if (dismissIndex >= 0) m_Notifications.erase(m_Notifications.begin() + dismissIndex);
}

// The bell's dropdown (Phase 6 item 14) — a compact list, one row per notification, newest
// first, same underlying m_Notifications the floating cards read from. No thumbnails here (the
// cards already show those); this is meant as a scan-and-jump list, not a second copy of the card
// UI. Opening it doesn't mark anything read by itself — EditorModuleToolbar calls
// MarkNotificationsRead() the moment the popup opens, matching how a bell icon behaves everywhere
// else (badge clears once you've looked, not once you've closed the list).
void EditorLayer::DrawNotificationsPopupBody() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * m_UIScale, 4.0f * m_UIScale));
    if (m_Notifications.empty()) {
        ImGui::TextDisabled("No notifications");
    }
    int dismissIndex = -1;
    for (int i = 0; i < (int)m_Notifications.size(); ++i) {
        EditorNotification& n = m_Notifications[i];
        ImGui::PushID(i);
        const char* glyph = n.Level == NotificationLevel::Error ? ICON_FA_CIRCLE_XMARK
                           : n.Level == NotificationLevel::Warning ? ICON_FA_TRIANGLE_EXCLAMATION
                           : ICON_FA_CIRCLE_CHECK;
        const ImVec4 col = n.Level == NotificationLevel::Error ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f)
                          : n.Level == NotificationLevel::Warning ? ImVec4(0.95f, 0.75f, 0.25f, 1.0f)
                          : ImVec4(0.4f, 0.8f, 0.5f, 1.0f);
        ImGui::TextColored(col, "%s", glyph);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(n.Title.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextUnformatted(n.Subtitle.c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 18.0f * m_UIScale);
        if (ActionButton(ICON_FA_XMARK, "Dismiss", false, ImVec2(16.0f * m_UIScale, 16.0f * m_UIScale)))
            dismissIndex = i;
        if (i + 1 < (int)m_Notifications.size()) ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    if (dismissIndex >= 0) m_Notifications.erase(m_Notifications.begin() + dismissIndex);
}

// Full-quality load for the lightbox — uncapped, since the user can zoom in.
static std::shared_ptr<Texture> LoadScreenshotLightboxTexture(const std::string& path) {
    return LoadScreenshotTexture(path, 0);
}
void EditorLayer::OpenScreenshotPreview(const std::string& path) {
    m_ShotPreviewPath = path;
    m_ShotPreviewTex.reset();
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        auto tex = LoadScreenshotLightboxTexture(path);
        if (tex->IsValid()) m_ShotPreviewTex = std::move(tex);
    }
    if (m_ShotPreviewTex) {
        m_ShotPreviewOpen = true;
        m_ShotPreviewAnim = 0.0f;
        m_ShotPreviewZoom = 1.0f;
        m_ShotPreviewPan = ImVec2(0.0f, 0.0f);
        m_ShotPreviewPanning = false;
        m_ShotPreviewPressOutside = false; // stale from a previous close would insta-dismiss
    } else {
        // Couldn't decode it here — fall back to the OS viewer rather than an empty window.
        m_ShotPreviewPath.clear();
        Screenshot::ShowInFolder(path);
    }
}

// Phase 6 item 7 — every image in the screenshots folder, sorted (Screenshot::Save's filenames
// are date/time-suffixed, so alphabetical reads chronological within a scene).
std::vector<std::string> EditorLayer::ListScreenshotsSorted() const {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(Screenshot::Dir(), ec)) {
        if (ec || !entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg")
            files.push_back(entry.path().generic_string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// A centred, chrome-light lightbox for a saved screenshot. The backdrop gets the same frosted
// blur as the delete-confirmation modal (EndFrame() blurs whatever's behind the top-most modal),
// so no extra dim is applied here. The window is freely movable and resizable; over the image,
// the scroll wheel zooms about the cursor and left-drag pans. Esc / X / a backdrop click closes,
// Left/Right arrows step to the previous/next screenshot in the folder (#7), and the header's
// action bar covers Open externally / Copy path / Copy image / Show in folder / Delete / Close.
void EditorLayer::DrawScreenshotPreview(World& world, AssetLibrary& assets) {
    if (!m_ShotPreviewOpen) return;

    if (!ImGui::IsPopupOpen("##ShotPreview")) ImGui::OpenPopup("##ShotPreview");

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    Texture* tex = m_ShotPreviewTex.get();
    const float iw = (tex && tex->Width()  > 0) ? (float)tex->Width()  : 1.0f;
    const float ih = (tex && tex->Height() > 0) ? (float)tex->Height() : 1.0f;

    // First-show size: fit the native image into ~3/4 of the work area (never upscaled past 1:1),
    // plus room for padding and the header row. After that the user owns the window size.
    float initFit = std::min((vp->WorkSize.x * 0.74f) / iw, (vp->WorkSize.y * 0.80f) / ih);
    initFit = std::clamp(initFit, 0.05f, m_UIScale);
    const ImVec2 initSize(iw * initFit + 28.0f * m_UIScale, ih * initFit + 66.0f * m_UIScale);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(initSize, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f * m_UIScale, 220.0f * m_UIScale),
                                        ImVec2(FLT_MAX, FLT_MAX));

    m_ShotPreviewAnim += (1.0f - m_ShotPreviewAnim) * 0.30f;
    if (m_ShotPreviewAnim > 0.999f) m_ShotPreviewAnim = 1.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * m_UIScale, 12.0f * m_UIScale)); // #37
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * m_ShotPreviewAnim);

    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::BeginPopupModal("##ShotPreview", &open, flags)) {
        // #6 — don't rely on ImGui's implicit escape-closes-modal behaviour: the canvas
        // InvisibleButton below can hold an active ID (mid-drag) that suppresses it, exactly
        // like the backdrop-dismissal check further down already has to work around. Every other
        // modal dialog in this file checks Escape explicitly for the same reason — match that.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { open = false; ImGui::CloseCurrentPopup(); }

        // #7 — Left/Right steps to the previous/next screenshot in the folder. Re-opening via
        // OpenScreenshotPreview resets zoom/pan for the new image, same as clicking a different
        // thumbnail in the Asset Browser would.
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
            const std::vector<std::string> shots = ListScreenshotsSorted();
            const auto it = std::find(shots.begin(), shots.end(), m_ShotPreviewPath);
            if (it != shots.end()) {
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && it != shots.begin()) {
                    const std::string prev = *(it - 1);
                    ImGui::EndPopup();
                    ImGui::PopStyleVar(4);
                    OpenScreenshotPreview(prev);
                    return;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) && (it + 1) != shots.end()) {
                    const std::string next = *(it + 1);
                    ImGui::EndPopup();
                    ImGui::PopStyleVar(4);
                    OpenScreenshotPreview(next);
                    return;
                }
            }
        }

        // Slim header: filename + native size + current zoom, then icon actions pinned right.
        const std::string name = std::filesystem::path(m_ShotPreviewPath).filename().string();
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%d x %d", (int)iw, (int)ih);
        ImGui::SameLine();
        // #7 — this used to just print m_ShotPreviewZoom*100, so it claimed "100%" at the
        // DEFAULT (fit-to-canvas) zoom instead of the image's true native-pixel percentage.
        // `fit` isn't known yet this early (it depends on the canvas rect, established further
        // down after the header), so estimate it the same way with the header's own height
        // subtracted - close enough for a rounded display percentage, and self-corrects every
        // frame regardless.
        {
            const float estHeaderH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float estCanvasW = std::max(avail.x, 32.0f);
            const float estCanvasH = std::max(avail.y - estHeaderH, 32.0f);
            const float estFit = std::min(estCanvasW / iw, estCanvasH / ih);
            if (estFit * m_ShotPreviewZoom >= 0.995f && estFit * m_ShotPreviewZoom < 1.005f)
                ImGui::TextDisabled("\xE2\x80\xA2  100%%");
            else
                ImGui::TextDisabled("\xE2\x80\xA2  %.0f%%", estFit * m_ShotPreviewZoom * 100.0f);
        }

        const float btnW = 26.0f * m_UIScale;
        const int   btnCount = 6; // open externally, copy path, copy image, show in folder, delete, close
        float rightX = ImGui::GetContentRegionMax().x - btnW * (float)btnCount - 6.0f * (float)(btnCount - 1);
        if (rightX > ImGui::GetCursorPosX()) ImGui::SameLine(rightX);
        else ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f); // flat icon row, no hairline box
        if (ImGui::Button(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE, ImVec2(btnW, 0.0f)))
            Screenshot::OpenFile(m_ShotPreviewPath);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Open externally");
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Button(ICON_FA_COPY, ImVec2(btnW, 0.0f))) {
            ImGui::SetClipboardText(m_ShotPreviewPath.c_str());
            Log::Info("Copied path: " + m_ShotPreviewPath);
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Copy path");
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Button(ICON_FA_IMAGE, ImVec2(btnW, 0.0f))) {
            if (Screenshot::CopyImageToClipboard(m_ShotPreviewPath)) Log::Info("Copied image to clipboard.");
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Copy image");
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Button(ICON_FA_FOLDER_OPEN, ImVec2(btnW, 0.0f))) Screenshot::ShowInFolder(m_ShotPreviewPath);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Show in folder");
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, EditorUIPrimitives::DangerColor());
        if (ImGui::Button(ICON_FA_TRASH, ImVec2(btnW, 0.0f))) {
            RequestDeleteAssets(world, assets, { AssetKeyRef{ m_ShotPreviewPath, false } }, /*skipDialog=*/false);
            open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Delete");
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Button(ICON_FA_XMARK, ImVec2(btnW, 0.0f))) { open = false; ImGui::CloseCurrentPopup(); }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Close (Esc)");
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::Spacing();

        // The image canvas fills the rest of the window. An InvisibleButton over it captures
        // hover / drag without turning the image into a "widget".
        ImVec2 canvas = ImGui::GetContentRegionAvail();
        canvas.x = std::max(canvas.x, 32.0f);
        canvas.y = std::max(canvas.y, 32.0f);
        const ImVec2 cpos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##shotcanvas", canvas,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool canvasHovered = ImGui::IsItemHovered();
        const bool canvasActive  = ImGui::IsItemActive();

        const float fit = std::min(canvas.x / iw, canvas.y / ih); // image fits canvas at zoom 1

        // Scroll wheel -> zoom about the cursor (keep the point under the pointer anchored).
        if (canvasHovered) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                const float prev = m_ShotPreviewZoom;
                m_ShotPreviewZoom = std::clamp(m_ShotPreviewZoom * std::pow(1.15f, wheel), 0.1f, 16.0f);
                const float k = m_ShotPreviewZoom / prev;
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const ImVec2 imgCtr(cpos.x + canvas.x * 0.5f + m_ShotPreviewPan.x,
                                    cpos.y + canvas.y * 0.5f + m_ShotPreviewPan.y);
                m_ShotPreviewPan.x += (imgCtr.x - mouse.x) * (k - 1.0f);
                m_ShotPreviewPan.y += (imgCtr.y - mouse.y) * (k - 1.0f);
            }
        }
        // Left / middle drag pans.
        if (canvasActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            m_ShotPreviewPan.x += d.x;
            m_ShotPreviewPan.y += d.y;
            m_ShotPreviewPanning = true;
        } else {
            m_ShotPreviewPanning = false;
        }
        // Double-click resets to fit.
        if (canvasHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_ShotPreviewZoom = 1.0f;
            m_ShotPreviewPan = ImVec2(0.0f, 0.0f);
        }
        if (canvasHovered && !m_ShotPreviewPanning)
            EditorUI::SetTooltip("Scroll to zoom  \xE2\x80\xA2  drag to pan  \xE2\x80\xA2  double-click to reset\n"
                                 "\xE2\x86\x90 / \xE2\x86\x92 for the previous / next screenshot"); // #7

        const float dispW = iw * fit * m_ShotPreviewZoom;
        const float dispH = ih * fit * m_ShotPreviewZoom;
        const ImVec2 ic(cpos.x + canvas.x * 0.5f + m_ShotPreviewPan.x,
                        cpos.y + canvas.y * 0.5f + m_ShotPreviewPan.y);
        const ImVec2 a(ic.x - dispW * 0.5f, ic.y - dispH * 0.5f);
        const ImVec2 b(a.x + dispW, a.y + dispH);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(cpos, ImVec2(cpos.x + canvas.x, cpos.y + canvas.y), true);
        if (tex && tex->IsValid())
            dl->AddImage((ImTextureID)(intptr_t)tex->GLHandle(), a, b);
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 26));
        dl->PopClipRect();

        // Backdrop dismissal: close only on a press-and-release that BOTH land on the dimmed
        // area outside the window. Tracking the press origin keeps a resize-grip drag (which
        // starts on the window edge and can wander outside) or a pan from closing the lightbox.
        const bool overWindow = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            m_ShotPreviewPressOutside = !overWindow;
        if (m_ShotPreviewAnim > 0.5f && m_ShotPreviewPressOutside && !canvasActive &&
            !ImGui::IsAnyItemActive() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !overWindow) {
            open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(4);

    if (!open) {
        m_ShotPreviewOpen = false;
        m_ShotPreviewTex.reset();
        m_ShotPreviewPath.clear();
        m_ShotPreviewAnim = 0.0f;
    }
}
