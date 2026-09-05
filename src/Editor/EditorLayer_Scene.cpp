// Scene lifecycle: new/open/save, the recovery + unsaved-changes prompts, undo/redo, play
// mode enter/exit, dropped-file import, and screenshot capture. Split out of
// EditorLayer.cpp for build time (#179).

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
#include "Log.h"
#include "EditorSettings.h"
#include "EditorUIHelpers.h"
#include "AssetImporterInspector.h"
#include "Profiler.h"
#include "ProjectPaths.h"
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
#include <cmath>
#include <cctype>
#include <cstring>
#include <functional>
#include <cfloat>

using namespace EditorInternal;


std::string EditorLayer::RecoveryPathFor(const std::string& scenePath) {
    std::filesystem::path p(scenePath);
    p.replace_extension(); // "…/scene.json" -> "…/scene"
    return p.string() + ".recovery.json";
}

void EditorLayer::WriteRecoverySnapshot(const World& world, const AssetLibrary& assets) {
    if (m_CurrentScenePath.empty()) return; // untitled scene (New Scene) has no sidecar location
    const std::string path = RecoveryPathFor(m_CurrentScenePath);
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
    std::string path = FileDialog::SaveFile("Scene Files\0*.json\0All Files\0*.*\0", "json", m_Window);
    if (path.empty()) return false; // user cancelled
    if (!SceneSerializer::Save(world, assets, path)) return false;
    ClearRecoverySnapshot();       // clears the snapshot for the PREVIOUS path (still current here)
    m_CurrentScenePath = path;
    EditorSettings::Get().LastScenePath = path; // reopen this one next launch (#95)
    EditorSettings::Save();
    m_Dirty = false;
    m_SavedUndoDepth = (int)m_UndoStack.size(); // this history position now matches disk
    m_AutoSaveTimer = 0.0f;
    InvalidateScenesListing(); // (#175) may have written a new file under scenes/
    return true;
}

void EditorLayer::DoSave(World& world, AssetLibrary& assets) {
    if (m_CurrentScenePath.empty()) {   // untitled -> must choose a location
        DoSaveAs(world, assets);
        return;
    }
    SceneSerializer::Save(world, assets, m_CurrentScenePath);
    m_Dirty = false;
    m_SavedUndoDepth = (int)m_UndoStack.size(); // this history position now matches disk
    m_AutoSaveTimer = 0.0f;
    ClearRecoverySnapshot();            // the real file is now current — the snapshot is stale
}

void EditorLayer::DrawRecoveryPrompt(World& world, AssetLibrary& assets) {
    if (!m_RecoveryPromptPending) return;

    if (!ImGui::IsPopupOpen("Recover Unsaved Changes?")) {
        ImGui::OpenPopup("Recover Unsaved Changes?");
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Recover Unsaved Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "A recovery snapshot newer than the saved scene was found - the editor\n"
            "likely closed before these changes were saved.\n\n"
            "Restore the unsaved changes, or discard them and keep the saved scene?");
        ImGui::Separator();

        if (PrimaryButton("Restore", ImVec2(120.0f, 0.0f))) {
            const std::string recoveryPath = RecoveryPathFor(m_CurrentScenePath);
            if (SceneSerializer::Load(world, assets, recoveryPath)) {
                CheckSceneVersionWarning();
                ClearSelection();
                m_UndoStack.clear();
                m_RedoStack.clear();
                m_Dirty = true; // recovered content isn't in the real scene file yet
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
        if (PrimaryButton("Discard", ImVec2(120.0f, 0.0f))) {
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
    if (!ImGui::IsPopupOpen(kPopupId)) ImGui::OpenPopup(kPopupId);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420.0f * m_UIScale, 0.0f));

    if (ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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

    if (!ImGui::IsPopupOpen("Save changes?")) ImGui::OpenPopup("Save changes?");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Save changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string sceneName = std::filesystem::path(m_CurrentScenePath).filename().string();
        if (sceneName.empty()) sceneName = "Untitled";
        ImGui::Text("\"%s\" has unsaved changes.", sceneName.c_str());
        ImGui::TextUnformatted("Save them before closing?");
        ImGui::Separator();

        if (PrimaryButton("Save", ImVec2(110.0f, 0.0f))) {
            m_ExitDecision = ExitDecision::SaveAndExit;
            m_ExitPromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Don't Save", ImVec2(110.0f, 0.0f))) {
            m_ExitDecision = ExitDecision::DiscardAndExit;
            m_ExitPromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_ExitDecision = ExitDecision::None;
            m_ExitPromptPending = false; // main sees ExitPromptActive() == false -> stays open
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorLayer::RequestNewScene(World& world, AssetLibrary& assets) {
    if (m_Dirty) {
        m_PendingSceneSwitch = PendingSceneSwitch::New;
        m_PendingScenePath.clear();
        m_ScenePromptPending = true;
        return;
    }
    NewScene(world, assets);
}

void EditorLayer::RequestOpenScene(World& world, AssetLibrary& assets, const std::string& path) {
    if (path.empty()) return; // dialog cancelled, or an empty drag payload
    if (m_Dirty) {
        m_PendingSceneSwitch = PendingSceneSwitch::Open;
        m_PendingScenePath = path;
        m_ScenePromptPending = true;
        return;
    }
    OpenScene(world, assets, path);
}

// "Save changes?" before New/Open discard the current scene (#216) — same Save / Don't Save /
// Cancel shape as DrawExitPrompt, but for switching scenes rather than closing the window, so it
// runs the stashed New/Open instead of exiting. A distinct popup ID ("##SceneSwitch") keeps it
// independent of the exit prompt even though the visible title text matches.
void EditorLayer::DrawSceneSwitchPrompt(World& world, AssetLibrary& assets) {
    if (!m_ScenePromptPending) return;

    if (!ImGui::IsPopupOpen("Save changes?##SceneSwitch")) ImGui::OpenPopup("Save changes?##SceneSwitch");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Save changes?##SceneSwitch", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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

        if (PrimaryButton("Save", ImVec2(110.0f, 0.0f))) {
            // DoSaveAs returns false if the user cancels the file dialog — in that case the
            // switch stays pending and the prompt stays open, same as a fresh Cancel would.
            bool saved = true;
            if (m_CurrentScenePath.empty()) saved = DoSaveAs(world, assets);
            else DoSave(world, assets);
            if (saved) {
                runPendingSwitch();
                m_ScenePromptPending = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (PrimaryButton("Don't Save", ImVec2(110.0f, 0.0f))) {
            runPendingSwitch();
            m_ScenePromptPending = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (PrimaryButton("Cancel", ImVec2(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_PendingSceneSwitch = PendingSceneSwitch::None;
            m_PendingScenePath.clear();
            m_ScenePromptPending = false; // abort entirely — nothing happens
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
std::vector<int> EditorLayer::CaptureSelectedOrders(const World& world) const {
    std::vector<int> orders;
    auto addIfValid = [&](entt::entity e) {
        if (e != entt::null && world.Registry.valid(e) && world.Registry.all_of<OrderComponent>(e)) {
            orders.push_back(world.Registry.get<OrderComponent>(e).Value);
        }
    };
    addIfValid(m_Selected);
    for (entt::entity e : m_ExtraSelection) addIfValid(e);
    return orders;
}

void EditorLayer::RestoreSelectionByOrder(World& world, const std::vector<int>& orders) {
    ClearSelection();
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

void EditorLayer::PushUndo(const World& world, const std::string& label) {
    UndoEntry entry;
    entry.SceneJson = m_AssetsPtr ? SceneSerializer::SaveToString(world, *m_AssetsPtr)
                                   : SceneSerializer::SaveToString(world);
    // Plenty of call sites fire on "field focused" / "gizmo grabbed" before anything actually
    // changes — a double-click-to-type on a Transform field lands here twice with no edit
    // between. Don't stack a byte-identical snapshot on the last one: it produced phantom
    // History entries and left extra Ctrl+Z presses that did nothing (#19 P8, #23 P23).
    if (!m_UndoStack.empty() && m_UndoStack.back().SceneJson == entry.SceneJson) {
        m_RedoStack.clear(); // still a fresh edit intent — a stale redo branch shouldn't survive it
        RefreshDirtyFromHistory();
        return;
    }
    entry.SelectedOrders = CaptureSelectedOrders(world);
    entry.Label = label;
    // Branching off a mid-history position discards the redo entries — the saved state may be
    // among them, in which case there's no longer a clean point to return to (#22 P22).
    if (m_SavedUndoDepth >= 0 && !m_RedoStack.empty()) m_SavedUndoDepth = -1;
    m_UndoStack.push_back(std::move(entry));
    if (m_UndoStack.size() > kMaxHistory) {
        m_UndoStack.erase(m_UndoStack.begin());
        if (m_SavedUndoDepth > 0) m_SavedUndoDepth--; // the whole stack shifted down by one
    }
    m_RedoStack.clear(); // a fresh edit invalidates whatever redo history existed
    RefreshDirtyFromHistory();
}

void EditorLayer::StageUndo(const World& world) {
    if (m_HasStagedUndo) return; // keep the FIRST (true pre-edit) snapshot of this interaction
    m_StagedUndoJson = m_AssetsPtr ? SceneSerializer::SaveToString(world, *m_AssetsPtr)
                                   : SceneSerializer::SaveToString(world);
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
    if (currentJson == m_StagedUndoJson) {
        m_StagedUndoJson.clear();
        m_StagedUndoSelectedOrders.clear();
        return;
    }

    UndoEntry entry;
    entry.SceneJson = std::move(m_StagedUndoJson);
    entry.SelectedOrders = std::move(m_StagedUndoSelectedOrders);
    entry.Label = label;
    if (m_SavedUndoDepth >= 0 && !m_RedoStack.empty()) m_SavedUndoDepth = -1;
    m_UndoStack.push_back(std::move(entry));
    if (m_UndoStack.size() > kMaxHistory) {
        m_UndoStack.erase(m_UndoStack.begin());
        if (m_SavedUndoDepth > 0) m_SavedUndoDepth--;
    }
    m_RedoStack.clear();
    RefreshDirtyFromHistory();
    m_HasStagedUndo = false;
}

void EditorLayer::Undo(World& world, AssetLibrary& assets) {
    if (m_UndoStack.empty()) return;

    UndoEntry redoEntry;
    redoEntry.SceneJson = SceneSerializer::SaveToString(world, assets);
    redoEntry.SelectedOrders = CaptureSelectedOrders(world);
    redoEntry.Label = m_UndoStack.back().Label; // the action Redo would re-apply from here
    m_RedoStack.push_back(std::move(redoEntry));

    UndoEntry entry = std::move(m_UndoStack.back());
    m_UndoStack.pop_back();
    SceneSerializer::LoadFromString(world, assets, entry.SceneJson);
    RestoreSelectionByOrder(world, entry.SelectedOrders);
    InvalidateModelThumbnail(); // LoadFromString may rebuild the asset library
    RefreshDirtyFromHistory(); // "*" clears when history returns to the last-saved point (#22 P22)
}

void EditorLayer::Redo(World& world, AssetLibrary& assets) {
    if (m_RedoStack.empty()) return;

    UndoEntry undoEntry;
    undoEntry.SceneJson = SceneSerializer::SaveToString(world, assets);
    undoEntry.SelectedOrders = CaptureSelectedOrders(world);
    undoEntry.Label = m_RedoStack.back().Label;
    m_UndoStack.push_back(std::move(undoEntry));

    UndoEntry entry = std::move(m_RedoStack.back());
    m_RedoStack.pop_back();
    SceneSerializer::LoadFromString(world, assets, entry.SceneJson);
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
    m_PlayModeSnapshot = SceneSerializer::SaveToString(world);
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
        AudioEngine::SoundHandle handle = AudioEngine::Play(audio.SoundPath, audio.Volume, audio.Loop);
        if (handle == AudioEngine::InvalidHandle) continue;
        glm::vec3 worldPos = glm::vec3(world.GetCachedWorldTransform(e)[3]);
        AudioEngine::SetPosition(handle, worldPos);
        m_PlayModeAudioHandles[e] = handle;
    }

    Log::Info("Entered play mode - scene state saved, changes will be reverted on exit.");
}

void EditorLayer::OnExitPlayMode(World& world, AssetLibrary& assets) {
    if (m_PlayModeSnapshot.empty()) return;

    // Stop exactly the voices Play On Start began (not AudioEngine::StopAll(), which would also
    // cut off an unrelated editor preview sound started while Play was running).
    for (auto& [entity, handle] : m_PlayModeAudioHandles) AudioEngine::Stop(handle);
    m_PlayModeAudioHandles.clear();

    SceneSerializer::LoadFromString(world, assets, m_PlayModeSnapshot);
    m_PlayModeSnapshot.clear();

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
    world = World();
    InvalidateModelThumbnail();
    ClearSelection();
    m_SoloLights.clear();
    m_MutedLights.clear();
    m_LookThroughActive = false;
    m_LookThroughLight = entt::null;
    m_LookThroughMoved = false;
    m_PendingLookThrough = entt::null;
    m_UndoStack.clear();
    m_RedoStack.clear();
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
        m_Dirty = false;                       // matches disk
        m_SavedUndoDepth = (int)m_UndoStack.size();
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
    if (path.empty() || !SceneSerializer::Load(world, assets, path)) return;
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
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_Dirty = false;
    m_SavedUndoDepth = 0; // freshly loaded — empty history == on disk
    m_AutoSaveTimer = 0.0f;
    Log::Info("Opened scene '" + path + "'.");
}

// Imports one file (never a directory) into `targetFolder` - the virtual Asset Browser folder
// it should be filed under, '/'-joined the same way m_CurrentAssetFolder is. Factored out of
// HandleDroppedFiles so a dropped folder's contents can each land in their own mirrored
// subfolder instead of everything collapsing into whichever folder happened to be open.
void EditorLayer::ImportDroppedFile(World& world, AssetLibrary& assets, Camera& editorCamera,
    const std::string& path, const std::string& targetFolder) {
    (void)editorCamera; // kept for signature symmetry with HandleDroppedFiles; not needed here
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::string name = std::filesystem::path(path).stem().string();

    Log::Info("Importing '" + path + "'...");

    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
        // Imports into the library only - it shows up in the Asset Browser, nothing more.
        // Deliberately NOT placed into the scene: that used to happen automatically here, but
        // it meant every dropped/imported model needed an undo (or a manual delete) if you only
        // wanted it available to drag in later. Explicit placement is now always the Asset
        // Browser -> Viewport drag (DrawViewportDropTarget's live ghost preview).
        assets.LoadModel(path);
        assets.SetAssetFolder(path, targetFolder);
        Log::Info("Imported model '" + name + "'.");
    } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
        auto tex = assets.LoadTexture(path);
        if (tex) {
            assets.SetAssetFolder(path, targetFolder);
            Log::Info("Imported texture '" + name + "'.");
        } else {
            Log::Error("Failed to load texture '" + path + "'.");
        }
    } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") {
        if (AudioEngine::Load(path)) {
            assets.RegisterSound(path);
            assets.SetAssetFolder(path, targetFolder);
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
void EditorLayer::OnCaptureDone(const std::string& path, int w, int h) {
    m_LastCapturePath = path;
    if (path.empty()) { m_CaptureToast = "Screenshot failed — see Console"; m_CaptureToastT = 3.0f; return; }
    InvalidateShotsListing(); // (#175) a new file just landed under screenshots/
    const auto& s = EditorSettings::Get();
    if (s.CaptureFlash) m_CaptureFlashT = 1.0f;
    if (s.CaptureSound) {
        std::string clip = Screenshot::ShutterClipPath();
        if (!clip.empty()) AudioEngine::Play(clip, 0.6f);
    }
    m_CaptureToast = std::filesystem::path(path).filename().string() + "   " +
                     std::to_string(w) + "x" + std::to_string(h);
    m_CaptureToastT = 3.5f;
}

// Called once per frame from Draw() — the fading white flash over the Scene viewport and the
// little "saved" toast in its bottom-right.
void EditorLayer::DrawCaptureFeedback(float dt) {
    if (m_CaptureFlashT <= 0.0f && m_CaptureToastT <= 0.0f) return;
    if (m_ViewportSize.x < 1.0f || m_ViewportSize.y < 1.0f) return;

    ImGuiWindow* sceneWin = ImGui::FindWindowByName("Scene");
    ImDrawList* dl = sceneWin ? sceneWin->DrawList : ImGui::GetForegroundDrawList();
    const ImVec2 mn(m_ViewportPos.x, m_ViewportPos.y);
    const ImVec2 mx(m_ViewportPos.x + m_ViewportSize.x, m_ViewportPos.y + m_ViewportSize.y);
    dl->PushClipRect(mn, mx, true);

    if (m_CaptureFlashT > 0.0f) {
        m_CaptureFlashT -= dt / 0.35f;
        float a = m_CaptureFlashT;
        a = a < 0.0f ? 0.0f : a * a; // ease out
        dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, (int)(a * 220.0f)));
    }
    if (m_CaptureToastT > 0.0f) {
        m_CaptureToastT -= dt;
        float a = m_CaptureToastT > 0.4f ? 1.0f : m_CaptureToastT / 0.4f;
        const ImVec2 ts = ImGui::CalcTextSize(m_CaptureToast.c_str());
        const float pad = 8.0f * m_UIScale;
        ImVec2 p1(mx.x - ts.x - pad * 2.0f - 16.0f * m_UIScale, mx.y - ts.y - pad * 2.0f - 16.0f * m_UIScale);
        ImVec2 p2(mx.x - 16.0f * m_UIScale, mx.y - 16.0f * m_UIScale);
        dl->AddRectFilled(p1, p2, IM_COL32(20, 20, 24, (int)(a * 220.0f)), 4.0f);
        dl->AddText(ImVec2(p1.x + pad, p1.y + pad), IM_COL32(235, 238, 245, (int)(a * 255.0f)), m_CaptureToast.c_str());
    }
    dl->PopClipRect();
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

// A centred, chrome-light lightbox for a saved screenshot. The backdrop gets the same frosted
// blur as the delete-confirmation modal (EndFrame() blurs whatever's behind the top-most modal),
// so no extra dim is applied here. The window is freely movable and resizable; over the image,
// the scroll wheel zooms about the cursor and left-drag pans. Esc / X / a backdrop click closes.
void EditorLayer::DrawScreenshotPreview() {
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * m_ShotPreviewAnim);

    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::BeginPopupModal("##ShotPreview", &open, flags)) {
        // Slim header: filename + native size + current zoom, then icon actions pinned right.
        const std::string name = std::filesystem::path(m_ShotPreviewPath).filename().string();
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%d x %d", (int)iw, (int)ih);
        ImGui::SameLine();
        ImGui::TextDisabled("\xE2\x80\xA2  %.0f%%", m_ShotPreviewZoom * 100.0f);

        const float btnW = 26.0f * m_UIScale;
        float rightX = ImGui::GetContentRegionMax().x - btnW * 2.0f - 6.0f;
        if (rightX > ImGui::GetCursorPosX()) ImGui::SameLine(rightX);
        else ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        if (ImGui::Button(ICON_FA_FOLDER_OPEN, ImVec2(btnW, 0.0f))) Screenshot::ShowInFolder(m_ShotPreviewPath);
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Show in folder");
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::Button(ICON_FA_XMARK, ImVec2(btnW, 0.0f))) { open = false; ImGui::CloseCurrentPopup(); }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Close (Esc)");
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
            EditorUI::SetTooltip("Scroll to zoom  \xE2\x80\xA2  drag to pan  \xE2\x80\xA2  double-click to reset");

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
