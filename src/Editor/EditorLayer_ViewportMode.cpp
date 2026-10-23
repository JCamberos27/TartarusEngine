// The unified viewport: the Scene panel shows either the scene or the game, in the same rect, so
// switching between them (and Play / Stop) is a dissolve rather than a jump between two differently
// sized tabs. On Play the editor camera glides from where you were to the player's view while the
// game image dissolves in over it; on Stop the game dissolves out while the camera glides back to
// where you left it. Window > Game still pops the Game view out into its own window (a second
// monitor, side by side); then the old tab behaviour applies.
//
// The editor camera is borrowed for the glide and always handed back: at the end of a move it is
// put back exactly where it was when the view left the scene (m_ScenePose).

#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorSettings.h"
#include "EditorTheme.h"
#include "EditorUIPrimitives.h"
#include "GameViewPanel.h"
#include "Camera.h"

#include <imgui.h>
#include <imgui_internal.h> // ClearActiveID
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cmath>

using namespace EditorInternal;

namespace {
float EaseInOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}
float Smooth(float a, float b, float t) {
    const float x = std::clamp((t - a) / (b - a), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
// Yaw the short way round (359 -> 1 turns 2 degrees, not 358).
float LerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return a + d * t;
}
bool MotionAllowed() {
    const EditorSettings& s = EditorSettings::Get();
    return !s.ReduceMotion && s.MotionViewport;
}
} // namespace

bool EditorLayer::GameViewPoppedOut() const {
    return m_GameView && m_GameView->IsWindowOpen();
}

bool EditorLayer::SceneImageWanted() const {
    return m_SceneViewportVisible && (m_ViewportMode == ViewportMode::Scene || m_ViewFade.Active || GameViewPoppedOut());
}

bool EditorLayer::GameImageWanted() const {
    return m_SceneViewportVisible && !GameViewPoppedOut() && (m_ViewportMode == ViewportMode::Game || m_ViewFade.Active);
}

void EditorLayer::RequestGameTabFocus() {
    if (GameViewPoppedOut()) { m_FocusGameTabRequested = true; return; }
    m_FocusSceneTabRequested = true; // the viewport itself in front (not the Script IDE)
    SetViewportMode(ViewportMode::Game, /*glide=*/true);
}

void EditorLayer::RequestSceneTabFocus() {
    m_FocusSceneTabRequested = true;
    if (!GameViewPoppedOut()) SetViewportMode(ViewportMode::Scene, /*glide=*/true);
}

void EditorLayer::SetViewportMode(ViewportMode mode, bool glide) {
    if (m_ViewFade.Active) FinishViewFade(m_EditorCameraPtr); // a second switch mid-move (F1 spam) cuts
    if (m_ViewportMode == mode) return;
    const bool toGame = mode == ViewportMode::Game;
    if (toGame && m_EditorCameraPtr) {
        const Camera& c = *m_EditorCameraPtr;
        m_ScenePose = {c.Position, c.Yaw, c.Pitch, c.Fov, true};
    }
    // No move to watch with the panel hidden, the editor chrome away (maximized play) or motion off.
    if (!MotionAllowed() || !m_SceneViewportVisible || GameViewPoppedOut()) {
        m_ViewportMode = mode;
        return;
    }
    m_ViewFade = {};
    m_ViewFade.Active = true;
    m_ViewFade.ToGame = toGame;
    m_ViewFade.Glide = glide && EditorSettings::Get().PlayTransitionGlide && m_ScenePose.Valid &&
                       (toGame || m_LastGamePose.Valid);
    m_ViewFade.Start = ImGui::GetTime();
    m_ViewFade.Duration = toGame ? (m_ViewFade.Glide ? 0.55f : 0.25f) : (m_ViewFade.Glide ? 0.45f : 0.25f);
    m_ViewFade.Alpha = toGame ? 0.0f : 1.0f;
}

void EditorLayer::FinishViewFade(Camera* editorCamera) {
    if (!m_ViewFade.Active) return;
    m_ViewFade.Active = false;
    m_ViewportMode = m_ViewFade.ToGame ? ViewportMode::Game : ViewportMode::Scene;
    m_ViewFade.Alpha = m_ViewFade.ToGame ? 1.0f : 0.0f;
    if (m_ViewFade.Glide && editorCamera && m_ScenePose.Valid) {
        // Handed back exactly as it was left, whichever way the move went.
        editorCamera->Position = m_ScenePose.Position;
        editorCamera->Yaw = m_ScenePose.Yaw;
        editorCamera->Pitch = m_ScenePose.Pitch;
        editorCamera->Fov = m_ScenePose.Fov;
    }
}

void EditorLayer::UpdateViewFade(Camera& editorCamera, const Camera& gameCam, bool playing) {
    if (playing && m_ViewportMode == ViewportMode::Game && !m_ViewFade.Active)
        m_LastGamePose = {gameCam.Position, gameCam.Yaw, gameCam.Pitch, gameCam.Fov, true};
    if (!playing && !m_ViewFade.Active) m_LastGamePose.Valid = false; // a stale pose never starts a glide
    if (!m_ViewFade.Active) return;

    const float t = std::clamp((float)((ImGui::GetTime() - m_ViewFade.Start) / m_ViewFade.Duration), 0.0f, 1.0f);
    if (m_ViewFade.ToGame) {
        // The camera travels over the first 80%; the game dissolves in over the last half.
        if (m_ViewFade.Glide) {
            const float g = EaseInOutCubic(t / 0.8f);
            editorCamera.Position = glm::mix(m_ScenePose.Position, gameCam.Position, g);
            editorCamera.Yaw = LerpAngle(m_ScenePose.Yaw, gameCam.Yaw, g);
            editorCamera.Pitch = m_ScenePose.Pitch + (gameCam.Pitch - m_ScenePose.Pitch) * g;
            editorCamera.Fov = m_ScenePose.Fov + (gameCam.Fov - m_ScenePose.Fov) * g;
        }
        m_ViewFade.Alpha = m_ViewFade.Glide ? Smooth(0.5f, 1.0f, t) : Smooth(0.0f, 1.0f, t);
    } else {
        // The game dissolves out over the first half, uncovering the camera already on its way back.
        if (m_ViewFade.Glide) {
            const float g = EaseInOutCubic(t);
            editorCamera.Position = glm::mix(m_LastGamePose.Position, m_ScenePose.Position, g);
            editorCamera.Yaw = LerpAngle(m_LastGamePose.Yaw, m_ScenePose.Yaw, g);
            editorCamera.Pitch = m_LastGamePose.Pitch + (m_ScenePose.Pitch - m_LastGamePose.Pitch) * g;
            editorCamera.Fov = m_LastGamePose.Fov + (m_ScenePose.Fov - m_LastGamePose.Fov) * g;
        }
        m_ViewFade.Alpha = 1.0f - (m_ViewFade.Glide ? Smooth(0.0f, 0.5f, t) : Smooth(0.0f, 1.0f, t));
    }
    if (t >= 1.0f) FinishViewFade(&editorCamera);
}

// The header's Scene | Game segmented switch (Ctrl+` toggles it).
void EditorLayer::DrawViewportModeSwitch(float btnH) {
    const bool game = m_ViewportMode == ViewportMode::Game;
    const bool poppedOut = GameViewPoppedOut();
    if (ActionButton(ICON_FA_CUBE "  Scene", "The scene, with the editor camera and tools (Ctrl+`)", !game || poppedOut, ImVec2(0.0f, btnH)))
        SetViewportMode(ViewportMode::Scene);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginDisabled(poppedOut);
    if (ActionButton(ICON_FA_GAMEPAD "  Game", poppedOut ? "The Game view is in its own window (close it to bring it back)"
                                                         : "What the player sees, through the game camera (Ctrl+`)\n"
                                                           "Drag it out to dock the Game view anywhere in the layout",
                     game && !poppedOut, ImVec2(0.0f, btnH)))
        SetViewportMode(ViewportMode::Game);
    // Dragged off the button: tear the Game view out into its own tab, carried by the mouse.
    if (!poppedOut && m_GameView && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, EditorTheme::Px(8.0f))) {
        ImGui::ClearActiveID();
        SetViewportMode(ViewportMode::Scene);
        m_GameView->SetWindowOpen(true);
        m_GameTearOff = true;
    }
    ImGui::EndDisabled();
}
