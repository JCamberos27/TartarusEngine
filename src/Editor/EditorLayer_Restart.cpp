// Rebuild & Restart (File menu, Ctrl+Alt+R): the editor's side of EditorRestart. The old editor
// writes the hand-over (unsaved edits as the recovery snapshot, the selection, Play) once the build
// is done; the new one restores it after its startup scene load, with no recovery prompt.

#include "EditorLayer.h"
#include "EditorRestart.h"
#include "EditorSound.h"
#include "Log.h"
#include "RestartState.h"
#include "World.h"

#include <system_error>

std::string EditorLayer::RestartBlockedReason() const {
    if (!EditorRestart::Available()) return EditorRestart::UnavailableReason();
    if (EditorRestart::Building()) return "A restart build is already running.";
    if (m_Dirty && m_CurrentScenePath.empty())
        return "Save the untitled scene first: its unsaved edits have nowhere to be kept across the restart.";
    if (HasUnsavedScripts()) return "Save or close the Script IDE's unsaved files first.";
    if (InPrefabMode() && m_Dirty) return "Save or leave Prefab Mode first.";
    return {};
}

bool EditorLayer::LeavePrefabModeForRestart(World& world, AssetLibrary& assets) {
    if (!InPrefabMode()) return true;
    if (m_Dirty) { // edited while the build ran: never write the prefab behind the user's back
        Log::Warn("Rebuild & Restart: cancelled, the open prefab has unsaved edits. Save or leave Prefab Mode, then restart.");
        EditorSound::Play(EditorSound::Event::Blocked);
        return false;
    }
    ExitPrefabMode(world, assets, /*save=*/false);
    return !InPrefabMode();
}

void EditorLayer::RequestRebuildRestart() {
    const std::string why = RestartBlockedReason();
    if (!why.empty()) {
        Log::Warn("Rebuild & Restart: " + why);
        EditorSound::Play(EditorSound::Event::Blocked);
        return;
    }
    EditorRestart::Request();
}

bool EditorLayer::WriteRestartState(const World& world, const AssetLibrary& assets, const std::filesystem::path& file,
                                    bool wasPlaying) {
    RestartState::State state;
    state.ScenePath = m_CurrentScenePath;
    state.WasPlaying = wasPlaying;
    // Synchronous, not the timer's background write: the process exits right after.
    if (m_Dirty) {
        state.RestoreRecovery = WriteRecoverySnapshot(world, assets);
        WaitRecoveryWrite();
        if (!state.RestoreRecovery) {
            Log::Error("Rebuild & Restart: couldn't write the recovery snapshot; not restarting (your edits stay here).");
            return false;
        }
    }
    state.Selection = RestartState::RefsFor(world, GetSelectedItems());
    if (!RestartState::Save(file, state)) {
        Log::Error("Rebuild & Restart: couldn't write " + file.u8string() + "; not restarting.");
        return false;
    }
    return true;
}

bool EditorLayer::ApplyRestartState(World& world, AssetLibrary& assets, const std::filesystem::path& file) {
    std::string why;
    const std::optional<RestartState::State> state = RestartState::Load(file, RestartState::NowUnix(), &why);
    std::error_code ec;
    std::filesystem::remove(file, ec); // read once, whatever it held
    if (!state) {
        Log::Warn("Rebuild & Restart: no state restored (" + why + ").");
        return false;
    }
    if (state->ScenePath != m_CurrentScenePath) {
        // The startup scene isn't the one that was open (moved, deleted): keep what loaded, and let
        // the normal recovery prompt handle any snapshot.
        Log::Warn("Rebuild & Restart: the scene that was open (" + state->ScenePath + ") didn't reopen; selection and Play not restored.");
        return false;
    }
    if (state->RestoreRecovery && m_RecoveryPromptPending) RestoreRecoverySnapshot(world, assets);
    const std::vector<entt::entity> selection = RestartState::Resolve(world, state->Selection);
    if (!selection.empty()) SetSelectionToDistinct(selection);
    Log::Info("Rebuild & Restart: back on the same scene" + std::string(state->RestoreRecovery ? " with its unsaved edits." : "."));
    return state->WasPlaying;
}
