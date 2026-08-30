#pragma once
#include "ResolutionManager.h"
#include "../Renderer/Framebuffer.h"
#include <imgui.h>
#include <vector>

// What the stats overlay shows — filled in by whoever actually rendered the frame (main.cpp),
// kept as plain data here so GameViewPanel doesn't need to know about EditorLayer::RenderStats
// or the Profiler.
struct GameViewStats {
    int FPS = 0;
    float FrameMs = 0.0f;
    int DrawCalls = 0;
    int Triangles = 0;
    int Vertices = 0;
};

// Unity-style Game View: a dockable panel showing the scene rendered at a locked aspect
// ratio/resolution (or freely filling the panel), with a toolbar for picking that resolution,
// "Maximize on Play", a fullscreen toggle, and a stats overlay. Deliberately knows nothing about
// World/Camera/Player/Window/GLFW — main.cpp owns rendering the scene into this panel's
// Framebuffer (see GetFramebuffer()/ComputeTargetSize()) and owns real OS-window fullscreen;
// this class only owns the resolution/UI state, the letterbox math, and emits a fullscreen
// *request* the caller can act on (see ConsumeFullscreenRequest), rather than owning that state
// itself and risking it drifting out of sync with the real window.
class GameViewPanel {
public:
    // Draws the "Game" ImGui window: toolbar + the letterboxed framebuffer image. Call once per
    // frame (editor mode only — there's no ImGui content in Play Mode), after the framebuffer
    // has already been re-rendered for this frame. `stats` is optional (nullptr hides the
    // overlay regardless of the toggle) — pass it only when the framebuffer was actually
    // refreshed this frame, so the numbers shown are never stale. `isOsFullscreen` is the real,
    // current OS window fullscreen state (e.g. Window::IsFullscreen()) so the toolbar's
    // Fullscreen/Windowed button always reflects reality regardless of what last changed it —
    // this button, F11, or anything else.
    // `playing` / `inputEngaged` drive the in-panel play affordances: while the game is
    // running but the player hasn't clicked in yet, a "Click to control / Esc to release"
    // hint is drawn over the view and a left-click inside it is latched (see
    // ConsumeEngageClick) so main.cpp can lock the cursor to the game.
    // `noSceneCamera` (editing only): the scene has no Camera entity, so the view below is the
    // editor camera's — a hint says so, distinguishing it from a render failure (#36 B10).
    void RenderUI(const GameViewStats* stats, bool isOsFullscreen, bool playing, bool inputEngaged,
                  bool noSceneCamera = false);

    // True exactly once, the frame the user left-clicked inside the running Game view while
    // it wasn't yet controlling input — main.cpp turns that into "capture mouse/keyboard".
    bool ConsumeEngageClick() { bool v = m_EngageClickPending; m_EngageClickPending = false; return v; }
    // Whether the pointer was over the rendered image on the last RenderUI call.
    bool IsViewHovered() const { return m_ViewHovered; }

    // Call whenever Play Mode is entered/exited. When "Maximize on Play" is enabled, entering
    // Play requests OS fullscreen and exiting it requests leaving fullscreen again (consume the
    // request the same frame via ConsumeFullscreenRequest) — this engine already hides every
    // editor panel in Play Mode, so "maximize the Game tab over the other editor panels" (Unity's
    // literal behavior) has nothing left to do here; requesting real fullscreen is the closest
    // equivalent "give Play Mode the whole screen" gesture.
    void OnPlayStateChanged(bool isPlaying);

    // True exactly once, the frame a fullscreen state change was requested (by the toolbar
    // button, or automatically per OnPlayStateChanged above) — outWantFullscreen receives the
    // requested state. Call once per frame regardless of whether RenderUI ran this frame (a
    // Play Mode transition can request a change on a frame with no Game window drawn at all).
    bool ConsumeFullscreenRequest(bool& outWantFullscreen);

    void SetPreset(const ResolutionPreset& preset);
    const ResolutionPreset& GetCurrentPreset() const { return m_CurrentPreset; }

    ResolutionManager::LetterboxRect CalculateLetterboxSize(ImVec2 containerSize, float targetAspect) const {
        return ResolutionManager::CalculateLetterboxRect(containerSize, targetAspect);
    }

    // Resolves the current preset into concrete framebuffer pixel dimensions, given the space
    // actually available this frame (the Game panel's own content region while editing — see
    // GetLastAvailableRegion() — or the window's client size while a locked-aspect Play Mode is
    // blitting straight to the backbuffer). Always returns at least 1x1.
    void ComputeTargetSize(ImVec2 available, int& outWidth, int& outHeight) const;

    // The Game panel's own content-region size as of the LAST frame it was drawn (zero before
    // the first draw). One-frame-stale by construction: this frame's framebuffer render has to
    // happen before RenderUI (so the texture it displays is ready), but RenderUI is the only
    // place that knows the panel's actual size (only known once ImGui::Begin has run) — using
    // last frame's size instead of blocking on this frame's is the standard, imperceptible
    // resolution for that ordering problem.
    ImVec2 GetLastAvailableRegion() const { return m_LastAvailableRegion; }

    Framebuffer& GetFramebuffer() { return m_Framebuffer; }
    bool IsWindowOpen() const { return m_WindowOpen; }

    // Mirrors the rest of the editor's prefs (see EditorSettings) — reads/writes
    // EditorSettings::Get() directly so the chosen preset/toggles survive a relaunch the same
    // way every other editor preference does.
    void LoadSettings();
    void SaveSettings() const;

private:
    Framebuffer m_Framebuffer;
    ResolutionPreset m_CurrentPreset = ResolutionManager::BuiltInPresets()[0];
    std::vector<ResolutionPreset> m_CustomPresets;
    ImVec2 m_LastAvailableRegion{0.0f, 0.0f};

    bool m_WindowOpen = true;
    bool m_EngageClickPending = false;
    bool m_ViewHovered = false;
    bool m_MaximizeOnPlay = false;
    bool m_ShowStatsOverlay = true;

    bool m_LastKnownOsFullscreen = false; // mirrored from RenderUI's isOsFullscreen param, for the button's label
    bool m_FullscreenRequestPending = false;
    bool m_FullscreenRequestValue = false;
    bool m_FullscreenFromMaximizeOnPlay = false; // so leaving Play only auto-un-fullscreens if THIS feature caused it

    bool m_ShowCustomModal = false;
    int m_CustomWidth = 1920;
    int m_CustomHeight = 1080;

    void DrawToolbar();
    void DrawCustomResolutionModal();
};
