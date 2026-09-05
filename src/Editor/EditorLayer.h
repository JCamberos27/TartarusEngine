#pragma once
#include <imgui.h> // ImGuiID (GetSceneGameDockNodeId)
#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <list>
#include <memory>
#include "Texture.h" // TextureImportSettings - stored by value in the Import Settings panel state
#include "Model.h"   // ModelImportSettings - same
#include "ImportQueueManager.h"
#include "ChannelPreviewRenderer.h"
#include "ModelPreviewRenderer.h"
#include "AudioEngine.h" // AudioEngine::SoundHandle - m_PlayModeAudioHandles

struct GLFWwindow;
class World;
class Camera;
class AssetLibrary;
class ScreenBlur;
class Framebuffer;

// Rect = Unity's "Rect Tool" adapted to 3D: translate handles plus bounding-box corner/edge
// handles for non-uniform scaling, in one combined gizmo (ImGuizmo's TRANSLATE | BOUNDS).
enum class GizmoOp { Translate, Rotate, Scale, Rect };

// Async (PBO-backed) state for one adaptive-contrast sampling call site (#178). SampleTextureLuminance
// ping-pongs two pixel-pack buffer objects: each call kicks off a non-blocking glReadPixels into
// one PBO and, in the same call, maps+consumes whatever the OTHER PBO was loaded with by the
// previous kickoff (one throttled ~100ms tick earlier) — so the GPU is never stalled waiting for
// the readback. Each HUD element that samples independently (Stats HUD, History HUD, status bar,
// nav-gizmo cluster, Play/Stop button) needs its own instance so their reads don't clobber one
// another.
struct AsyncLuminanceReadback {
    unsigned int Pbo[2] = { 0, 0 }; // fixed-size (kMaxLumPatch^2 * 4 bytes) buffers, DSA-mapped
    int Pending[2] = { 0, 0 };      // pixel count kicked into that slot and not yet consumed (0 = none)
    int Next = 0;                  // slot to write into on the next call; the other slot is read
};

// In-game editor overlay (Dear ImGui + ImGuizmo): import assets, place/inspect
// entities, manipulate them with viewport gizmos. Toggle with F1; gameplay pauses
// while the editor is open.
class EditorLayer {
public:
    // Declared (rather than left implicit) and defined in the .cpp — a stylistic match for the
    // other Init/Shutdown-style lifecycle methods below, not a forward-declaration requirement
    // (Texture.h/Model.h are both fully included above now, for TextureImportSettings/
    // ModelImportSettings).
    EditorLayer();
    ~EditorLayer();

    void Init(GLFWwindow* window);
    void Shutdown();

    // Writes the colour palette for EditorSettings::EditorTheme into ImGui's live style. Called
    // once from Init() and again (colours only — no size/font rebuild) whenever the theme combo
    // in Preferences changes. For the Prism theme it also seeds the animated colours below.
    void ApplyEditorTheme();

    void BeginFrame();
    void Draw(World& world, AssetLibrary& assets, Camera& editorCamera, float dt);
    void EndFrame();

    // Pull the editor camera back to fit the whole scene's bounds in view, keeping its current
    // aim. No-op on an empty scene. Called once on startup so the editor doesn't open staring
    // at empty space next to the geometry (audit #87).
    void FrameSceneBounds(World& world, Camera& editorCamera);

    // The Play/Stop control cluster, drawn every frame in every state (unlike Draw(), which is
    // editor-UI-only). While editing it's a lone green Play in the toolbar strip; while playing
    // it's a red Stop plus a Fullscreen/Restore toggle (maximize the Game view over the editor
    // panels, or drop back). `maximized` floats it near the top of the window since the toolbar
    // is hidden then. Clicks only raise request flags — main.cpp owns the play/maximize/cursor
    // state itself.
    void DrawPlayStopButton(bool playing, bool maximized);

    // Minimize / maximize-restore / close buttons, right-aligned in the top toolbar — the OS
    // title bar is removed (Win32 custom frame in Window.cpp). Called from DrawTopToolbar.
    void DrawWindowControls();
    // True when the cursor is over the toolbar's empty (draggable) area this frame; main.cpp
    // forwards it to Window so its WM_NCHITTEST can treat that region as the window's caption.
    bool WantsWindowDrag() const { return m_TitleBarDragHovered; }
    bool m_TitleBarDragHovered = false;
    // Screen-space rect of the live Game view image + its framebuffer's colour texture/size, so
    // the Play-Mode Stop/Fullscreen overlay can anchor to the game viewport and adapt its tint
    // to what's rendered there. Pass a zero size to say "no game view this frame".
    void SetGameViewRect(ImVec2 imgPos, ImVec2 imgSize, unsigned int colorTex, int texW, int texH) {
        m_GameViewImgPos = imgPos; m_GameViewImgSize = imgSize;
        m_GameViewTex = colorTex; m_GameViewTexW = texW; m_GameViewTexH = texH;
    }
    // Average luminance (0..1) of this frame's rendered Scene viewport inside a screen-space box
    // of `boxPx` centered at `centerScreen`; -1 when no result is available yet (no scene texture,
    // box entirely outside the viewport, or the async readback hasn't landed yet — see
    // AsyncLuminanceReadback below). Throttle calls yourself — it still does a GPU->CPU sync, just
    // a frame or two later and without stalling the pipeline.
    float SampleSceneLuminance(AsyncLuminanceReadback& rb, ImVec2 centerScreen, float boxPx);
    // General form: sample `colorTex` (texW x texH) as if it were displayed in screen rect
    // imgPos..imgPos+imgSize, in a screen-space box of `boxPx` at `centerScreen`. -1 if unusable.
    float SampleTextureLuminance(AsyncLuminanceReadback& rb, unsigned int colorTex, int texW, int texH,
                                 ImVec2 imgPos, ImVec2 imgSize, ImVec2 centerScreen, float boxPx);
    bool ConsumePlayStopRequest() {
        bool requested = m_PlayStopRequested;
        m_PlayStopRequested = false;
        return requested;
    }
    // Raised by the Fullscreen/Restore button next to Stop — main.cpp flips playMaximized.
    bool ConsumeMaximizeToggleRequest() {
        bool requested = m_MaximizeToggleRequested;
        m_MaximizeToggleRequested = false;
        return requested;
    }

    // False while ImGui wants the mouse for some OTHER panel/popup — deliberately does NOT count
    // hovering the "Scene" viewport itself as capturing the mouse (see IsMouseOverSceneViewport's
    // comment), so camera navigation and viewport picking/box-select — both gated on this — keep
    // working inside the one place they need to.
    bool WantsCaptureMouse() const;
    bool WantsCaptureKeyboard() const;
    // A floating window (Preferences, an undocked panel) or an open menu has keyboard focus, so
    // the viewport's own shortcuts (tools, view snaps, frame, quick-add) should stand down.
    bool OtherWindowOwnsKeyboard() const;
    // Pure geometric point-in-rect test against ViewportPos()/ViewportSize() — used by
    // WantsCaptureMouse() above, and by main.cpp to decide whether the fly-camera should read
    // input this frame.
    bool IsMouseOverSceneViewport() const;

    // Force the "Scene" or "Game" tab to become the active one in the dock node they share
    // (they stay grouped/tabbed — this only changes which is selected). Scene defaults to
    // already-requested so the first frame starts there; main.cpp requests Scene on Stop and
    // Game on Play, so the viewport you're shown always matches what you just did.
    void RequestSceneTabFocus() { m_FocusSceneTabRequested = true; }
    void RequestGameTabFocus() { m_FocusGameTabRequested = true; }
    // Applies a pending Request*TabFocus() (no-op otherwise) — main.cpp calls this once per
    // editor-UI frame, AFTER both Scene's and Game's Begin() calls have run. The request stays
    // pending until it actually lands on a live dock node. Deliberately doesn't go through
    // ImGui::SetWindowFocus() — see the .cpp for why that doesn't select a tab in the vendored
    // ImGui build — so it needs imgui_internal.h, which is why this lives here not in main.cpp.
    void ApplyPendingViewportTabFocus();

    // Call once per frame during Play Mode (when Draw()'s own DockSpace() call doesn't run at
    // all). THE actual root cause of the Play/Stop docking corruption, found by reading ImGui's
    // BeginDocked(): a dock node tracks the last frame its DockSpace() was submitted
    // (LastFrameAlive); once that goes stale by even one frame, BeginDocked() undocks every
    // window in it outright the next time they're Begin()'d — not a focus/selection quirk, an
    // actual full undock. Both Scene and Game were getting silently undocked-then-rebound on
    // every single Play/Stop cycle because Draw() (and the real DockSpace() call inside it)
    // doesn't run during Play at all; rebinding order then made whichever window re-added itself
    // LAST (Game, always after Scene) win the tab bar's own "select newly added tab" behavior -
    // nothing to do with window focus, which is why every earlier attempt at this kept failing.
    // ImGuiDockNodeFlags_KeepAliveOnly is the documented fix for exactly this shape of problem
    // ("submit the non-visible dockspace with KeepAliveOnly" - ImGui's own docs, word for word):
    // it keeps LastFrameAlive current without displaying or hosting anything, so the dock tree
    // structure Draw() built stays completely untouched the whole time Play Mode is skipping it.
    //
    // CRUCIAL: it has to keep alive the SAME dockspace id Draw() uses. ImGui::GetID("EditorDockspace")
    // is relative to the current window's id stack — Draw() evaluates it inside "##DockHost", but
    // KeepDockspaceAlive() runs with no window pushed (the implicit Debug window is current), so
    // re-deriving it here hashes to a *different*, phantom id and keeps the wrong (empty) node
    // alive while the real one still goes stale. So Draw() stashes the resolved id in
    // m_EditorDockspaceId and this reuses that verbatim (DockSpace() with KeepAliveOnly needs no
    // window of its own — "may be called from any location", per imgui.cpp).
    void KeepDockspaceAlive();

    // Set by main.cpp before Draw() when the running game owns the mouse/keyboard (in-panel
    // play, clicked in; or maximized play). While set, the editor's own viewport picking,
    // box-select, gizmo shortcuts and W/E/R/T tool keys stand down so a "shoot" click or a
    // strafe key doesn't also poke the editor behind the game.
    void SetGameInputActive(bool active) { m_GameInputActive = active; }

    // The dock node "Scene" and "Game" share. Re-read every editor-mode frame from "Scene"'s
    // live dock assignment (seeded once by the one-time layout builder), so it follows the pair
    // wherever the user drags it instead of staying pinned to the first-run spot. main.cpp
    // passes this to ImGui::SetNextWindowDockID(id, ImGuiCond_Appearing) before gameView's own
    // Begin("Game") call — see that call site's comment for why: neither window is submitted at
    // all during Play Mode, and re-docking Game defensively onto Scene's node the moment it
    // "appears" again after Stop keeps the two tabbed together in their current home rather than
    // trusting ImGui to always remember, or (the old bug) yanking them back to the seed spot.
    ImGuiID GetSceneGameDockNodeId() const { return m_SceneGameDockNodeId; }

    // True while the pointer is hovering or actively dragging a gizmo handle. The editor
    // camera should ignore look input in that case so it doesn't fight the gizmo drag.
    bool GizmoEngaged() const { return m_GizmoEngaged; }

    // Grid settings, read by main.cpp to draw the actual 3D grid (an OpenGL draw call
    // outside ImGui's frame, so EditorLayer only owns the settings, not the rendering).
    bool ShowGrid() const { return m_ShowGrid; }

    // A model currently being dragged from the Asset Browser over the Viewport, and where it
    // would land if released right now — recomputed every frame in DrawViewportDropTarget()
    // while such a drag is in progress. Read by main.cpp (same reasoning as ShowGrid() above:
    // the actual OpenGL draw happens outside ImGui's frame) to render a translucent preview at
    // that spot via TintOverlayRenderer, so what you see while dragging matches exactly what
    // gets committed on release.
    struct DragPreview {
        bool Active = false;
        std::shared_ptr<Model> ModelRef;
        glm::vec3 Position{0.0f};
    };
    const DragPreview& GetDragPreview() const { return m_DragPreview; }

    // The viewport's actual on-screen rect this frame — the dockspace's central node, i.e.
    // whatever's left after the Hierarchy/Inspector/Asset Browser panels take their share.
    // Recomputed every frame in Draw() (editor mode only), so it tracks live panel resizes.
    // main.cpp reads this to size the 3D render (glViewport + camera aspect) to match instead
    // of always rendering to the full window underneath the docked panels.
    glm::vec2 ViewportPos() const { return m_ViewportPos; }
    glm::vec2 ViewportSize() const { return m_ViewportSize; }
    // False whenever the "Scene" tab isn't the active one (the "Game" tab is showing instead) —
    // ViewportPos()/ViewportSize() are zeroed in that case, so main.cpp skips rendering/picking
    // into a region the Scene view isn't actually occupying on screen this frame.
    bool IsSceneViewportVisible() const { return m_SceneViewportVisible; }

    // The GL color texture main.cpp should hand over each frame (its editor-camera render,
    // already containing the grid/outline/drag-preview overlays) — Draw() displays it inside
    // the "Scene" window via ImGui::Image instead of relying on raw GL drawn straight into the
    // backbuffer underneath a passthrough dock node. That old technique broke the moment Scene
    // became a real, named, tabbed ImGui window: a docked window's HOST NODE always paints its
    // own (near-opaque) background over whatever's already in the framebuffer, regardless of
    // that window's own NoBackground flag, which is what made the viewport look "super dark" —
    // correctly rendered, just painted over by that near-black background afterward. Routing the
    // render through a texture sidesteps the problem entirely: it's drawn as ordinary window
    // content, in the same draw-list ordering as anything else in that window, always on top of
    // its own host background rather than underneath it.
    void SetSceneTexture(unsigned int glColorTexture) { m_SceneColorTexture = glColorTexture; }

    // True while a modal dialog (Save changes?, Recover unsaved changes?, a confirm prompt) is
    // open. main.cpp reads it to frost the Scene viewport behind the dim scrim.
    bool AnyModalOpen() const;

    // --- Capture (screenshot) tool -----------------------------------------------------------
    // The toolbar camera button, the PrintScreen key and Preferences all call RequestCapture();
    // main.cpp does the grab from the right framebuffer for the chosen mode, then OnCaptureDone.
    // `primed` guards the one-frame ordering gap: the trigger (toolbar / key / sentinel) fires
    // after the Scene view has already rendered for the frame, so a viewport shot that needs a
    // resized render target waits until the NEXT frame — main.cpp sets primed once it has
    // rendered the scene at the requested size, and only then does the grab run.
    struct CaptureRequest { bool pending = false; bool primed = false; int mode = 0; int scale = 1; int format = 0; int resW = 0; int resH = 0; };
    void RequestCapture();                       // snapshots the current EditorSettings capture prefs
    CaptureRequest PeekCaptureRequest() const { return m_CaptureReq; }
    void PrimeCaptureRequest() { m_CaptureReq.primed = true; }
    CaptureRequest ConsumeCaptureRequest() { CaptureRequest r = m_CaptureReq; m_CaptureReq.pending = false; m_CaptureReq.primed = false; return r; }
    void OnCaptureDone(const std::string& path, int w, int h);
    void DrawCaptureFeedback(float dt);          // fading flash + "saved" toast; called from Draw()
    // Double-clicking a shot in the Asset Browser's Screenshots folder opens a centred, sleek
    // in-editor lightbox instead of shelling out to the OS viewer.
    void OpenScreenshotPreview(const std::string& path);
    void DrawScreenshotPreview();                // centred image lightbox; called from Draw()
    // Set by main.cpp for the single frame a "clean viewport" capture renders — every editor
    // overlay (grid, gizmos, entity icons, light gizmos, engine mark, HUDs, status bar) skips.
    void SetHideOverlaysThisFrame(bool v) { m_HideOverlaysThisFrame = v; }
    bool OverlaysHidden() const { return m_HideOverlaysThisFrame; }

    // The Scene window's own content-region size as of the LAST frame it was drawn (zero before
    // the first draw) — one-frame-stale for the same reason GameViewPanel's equivalent is: this
    // frame's texture has to already exist before Draw() can Image() it, but the window's actual
    // size isn't known until ImGui::Begin runs, which is inside Draw() itself.
    glm::vec2 GetLastSceneContentRegion() const { return m_LastSceneContentRegion; }

    // The scene file currently open — starts at "scene.json" to match the historical
    // auto-save/auto-load default; changes when the user uses Open/Save As. main.cpp reads
    // this for the auto-save-on-exit so it writes back to wherever the user actually has open.
    const std::string& CurrentScenePath() const { return m_CurrentScenePath; }
    // True once any edit has happened since the last Save/Save As/Open/New — main.cpp reads
    // this to show an unsaved-changes indicator in the window title.
    bool IsDirty() const { return m_Dirty; }

    // "Save changes?" on-exit prompt (audit #56). main.cpp intercepts the window-close request
    // when the scene is dirty, calls OpenExitPrompt(), and each frame polls TakeExitDecision():
    //   SaveAndExit  - main saves, then exits
    //   DiscardAndExit - main exits without saving
    //   None + ExitPromptActive() false again - the user picked Cancel; stay open
    enum class ExitDecision { None, SaveAndExit, DiscardAndExit };
    void OpenExitPrompt() { m_ExitPromptPending = true; m_ExitDecision = ExitDecision::None; }
    bool ExitPromptActive() const { return m_ExitPromptPending; }
    ExitDecision TakeExitDecision() { ExitDecision d = m_ExitDecision; m_ExitDecision = ExitDecision::None; return d; }

    // The full selection (primary + any Ctrl+Click extras) as entity handles, for main.cpp's
    // outline-render pass — empty when nothing is selected or outside editor mode.
    std::vector<entt::entity> GetSelectedItems() const {
        std::vector<entt::entity> result;
        if (m_Selected != entt::null) result.push_back(m_Selected);
        for (entt::entity e : m_ExtraSelection) result.push_back(e);
        return result;
    }

    // Lights panel solo/mute (#140 phase 4). main.cpp's per-frame light gather skips a light
    // this returns true for, but only while editing (Play mode always renders every light).
    // Muted lights are always suppressed; if ANY light is soloed, every non-soloed one is too.
    bool IsLightSuppressed(entt::entity e) const {
        if (m_MutedLights.count(e)) return true;
        return !m_SoloLights.empty() && !m_SoloLights.count(e);
    }

    // World-space center of the current selection's bounding box (single object or group) —
    // main.cpp reads this to know what to orbit the editor camera around while Alt+Left-drag
    // is held. False (outCenter untouched) when nothing is selected.
    bool GetSelectionCenter(World& world, glm::vec3& outCenter) const;

    // Unity's defining play-mode rule: entering Play snapshots the whole scene, and leaving it
    // restores that snapshot — so anything gameplay did (crates shot, objects moved) is undone
    // and Play is a safe place to experiment. main.cpp calls these on either side of its
    // editorMode flip, since it (not EditorLayer) owns that switch.
    void OnEnterPlayMode(const World& world);
    void OnExitPlayMode(World& world, AssetLibrary& assets);

    // Scene-view shading, chosen in the toolbar. main.cpp reads it to set the GL polygon mode
    // for the main draw pass (Wireframe) or skip the lighting/texture work entirely (Unlit).
    enum class ShadingMode { Shaded, Wireframe, Unlit };
    ShadingMode GetShadingMode() const { return m_ShadingMode; }

    // Per-frame render statistics, filled in by main.cpp's draw loop and displayed by the
    // editor's Stats overlay — EditorLayer can count entities itself, but only the renderer
    // knows how many draw calls actually issued.
    struct RenderStats {
        int DrawCalls = 0;
        int Triangles = 0;
        int Vertices = 0;
        int PointLights = 0;
        int Culled = 0; // entities skipped by frustum culling this frame - not drawn at all
        // #204: the scene has more active lights than the forward LightBuffer can hold
        // (LightBuffer::kMaxLights) - the excess were silently dropped before this existed.
        bool LightBufferOverflowed = false;
        // #204: the global per-frame cluster light-index list (ClusterGrid::GLOBAL_INDEX_CAPACITY)
        // ran out of room this frame, so at least one cluster's reserved block was truncated and
        // may be missing lights that should be shading it (#208: adapted from a per-cluster cap
        // to this shared-list capacity).
        bool ClusterSaturated = false;
    };
    void SetRenderStats(const RenderStats& stats) { m_RenderStats = stats; }
    // Last frame's stats, as set above — read by the --smoke-test harness (main.cpp) to check
    // a loaded scene actually issued draw calls rather than rendering silently empty.
    const RenderStats& GetRenderStats() const { return m_RenderStats; }

    // Files (or whole folders) dropped onto the window from the OS (e.g. dragged in from
    // Explorer) — routed by extension through the exact same AssetLibrary calls File > Import
    // already uses, then filed into whichever Asset Browser folder is currently open. A dropped
    // folder is walked recursively and its own subfolder structure is mirrored into the Asset
    // Browser rather than dumping every file flat into one place. Import-only, same as File >
    // Import — nothing is placed into the scene; drag it from the Asset Browser into the
    // Viewport when you want an instance. A dropped .prefab is the one exception (it IS a scene
    // object, not a raw asset) and instantiates immediately, same as always. Unsupported
    // extensions and load failures are logged, never silently dropped or crashing. No-op while
    // the editor panels are hidden (maximized play) — nothing sensible to import into then.
    void HandleDroppedFiles(World& world, AssetLibrary& assets, Camera& editorCamera,
        bool editorUIVisible, const std::vector<std::string>& paths);

private:
    // The single-file half of HandleDroppedFiles' logic - imports one file (never a directory)
    // into `targetFolder`, a '/'-joined virtual Asset Browser path. Split out so a dropped
    // folder can route each of its contents into its own mirrored subfolder.
    void ImportDroppedFile(World& world, AssetLibrary& assets, Camera& editorCamera,
        const std::string& path, const std::string& targetFolder);

    GLFWwindow* m_Window = nullptr;

    CaptureRequest m_CaptureReq;
    bool m_HideOverlaysThisFrame = false;
    float m_CaptureFlashT = 0.0f;              // eased 1 -> 0 over ~0.35s after a shot
    std::string m_LastCapturePath;
    std::string m_CaptureToast;               // shown in a corner for a few seconds after a shot
    float m_CaptureToastT = 0.0f;

    // Frosted backdrop behind modal dialogs — created lazily the first time a modal opens (see
    // EndFrame): the whole framebuffer is blurred and the dialog window redrawn crisp on top.
    std::unique_ptr<ScreenBlur> m_ModalBlur;
    std::unique_ptr<Framebuffer> m_FrostCapture;

    // Monitor content-scale factor (1.0 = 96 DPI, 2.0 = 200% Windows scaling, etc.), read once
    // at Init and baked into font sizes and the handful of raw-pixel layout constants below —
    // so the UI reads at a consistent physical size instead of shrinking to illegible on a
    // high-DPI/4K display.
    float m_UIScale = 1.0f;
    bool m_PlayStopRequested = false;
    bool m_MaximizeToggleRequested = false;
    bool m_GameInputActive = false; // see SetGameInputActive
    // Set by Settings > Reset Layout; consumed at the top of Draw()'s dockspace setup to
    // rebuild the default panel arrangement from scratch.
    bool m_ResetLayoutRequested = false;

    // Defaults to a plausible full-window size so nothing divides by zero if anything reads
    // these before the first Draw() has run; overwritten every editor-mode frame after that.
    bool m_FocusSceneTabRequested = true;
    bool m_FocusGameTabRequested = false;
    // Seed the bottom dock node showing the Asset Browser tab (not Console) whenever the default
    // layout is (re)built. Retried each frame until its dock node exists — same pattern as the
    // Scene/Game tab focus above.
    int  m_SelectAssetBrowserTabFrames = 90;  // force the Asset Browser tab active for this many
                                              // startup frames, outlasting ImGui's .ini dock restore
    ImGuiID m_SceneGameDockNodeId = 0;
    // The editor dockspace id as resolved inside "##DockHost" during Draw() — stashed so
    // KeepDockspaceAlive() (which runs with no window pushed) keeps the RIGHT node alive
    // through Play Mode instead of re-hashing "EditorDockspace" against the wrong id stack.
    ImGuiID m_EditorDockspaceId = 0;
    glm::vec2 m_ViewportPos{0.0f, 0.0f};
    glm::vec2 m_ViewportSize{1280.0f, 720.0f};
    bool m_SceneViewportVisible = true;
    unsigned int m_SceneColorTexture = 0;
    glm::vec2 m_LastSceneContentRegion{0.0f, 0.0f};

    // Primary selection. entt::entity handles stay valid across insertions/deletions elsewhere
    // in the registry (unlike the vector indices this replaced), so unlike before there's no
    // shifting-index bookkeeping needed anywhere below.
    entt::entity m_Selected = entt::null;

    // Additional objects co-selected with the primary via Ctrl+Click, for group operations
    // (move/rotate/scale several objects together with the gizmo, delete them all at once).
    // The multi-select Inspector edits properties shared by the whole selection (Transform,
    // Active/Tag/Static, and any component common to every selected entity — including the PBR
    // material and its texture maps), Unity-style, with a mixed-value dash for properties that
    // differ across the selection.
    std::vector<entt::entity> m_ExtraSelection;
    glm::mat4 m_GroupGizmoMatrix{1.0f}; // pivot frame for the multi-select gizmo, updated across a drag

    // The Hierarchy's flattened top-to-bottom order of currently-visible rows (respects group
    // headers, expand/collapse state, and the search filter). Ctrl+A selects all of it;
    // Shift+Click selects the contiguous run between the anchor and the clicked row along it.
    // Two buffers: rows accumulate into ...Build as DrawHierarchyNode draws them, then it is
    // published to ...Order at the end of the pass. A click handler fires PART-WAY through the
    // draw (only rows above the clicked one exist in ...Build yet), so it must read the
    // fully-populated ...Order from the previous frame instead — otherwise an upward range
    // (anchor below the clicked row) would see the anchor missing and fall back to a single
    // pick.
    std::vector<entt::entity> m_HierarchyVisibleOrder;
    std::vector<entt::entity> m_HierarchyVisibleBuild;
    // Range-select anchor: the last row picked with a plain or Ctrl+Click. Shift+Click extends
    // from here without moving it, so the range can be grown or shrunk by clicking again.
    entt::entity m_SelectionAnchor = entt::null;
    // Ctrl+A while the Hierarchy is focused: replace the selection with every visible row.
    void SelectAllVisibleInHierarchy();
    // Shift+Click on `target`: select every row between m_SelectionAnchor and `target`
    // inclusive along m_HierarchyVisibleOrder. `additive` (Ctrl+Shift) keeps the existing
    // selection and adds the range; otherwise the range replaces it. `target` becomes primary.
    void SelectHierarchyRange(World& world, entt::entity target, bool additive);

    bool HasGroupSelection() const { return !m_ExtraSelection.empty(); }
    bool HasAnySelection() const { return m_Selected != entt::null; }
    bool IsSelected(entt::entity entity) const {
        if (m_Selected == entity) return true;
        for (entt::entity e : m_ExtraSelection) {
            if (e == entity) return true;
        }
        return false;
    }
    // Replaces the primary selection (plain click) or toggles membership in the group
    // (Ctrl+Click) — the one place both the Hierarchy and viewport picking route through so
    // they stay consistent.
    void SelectItem(entt::entity entity, bool addToSelection);
    void ClearSelection();
    void DeleteSelection(World& world);
    void DuplicateSelection(World& world, AssetLibrary& assets);
    void DrawGroupGizmo(World& world, Camera& editorCamera);
    // Shared setup/teardown behind DrawGizmo() (single-object) and DrawGroupGizmo() (multi-select):
    // opens the fullscreen transparent overlay window ImGuizmo's hit-testing needs and configures
    // its per-frame global state (ortho, drawlist, rect, gizmo size). `overlayName` is the one
    // difference between the two call sites (distinct ImGui window IDs). Returns false — with no
    // overlay left open — if the current window size is degenerate and the caller should bail out;
    // EndGizmoOverlay() must only be called after a true return.
    bool BeginGizmoOverlay(Camera& editorCamera, const char* overlayName);
    void EndGizmoOverlay();
    // Shared bounds computation behind FocusOnSelection() and GetSelectionCenter() — world-space
    // AABB of the current selection (single object or group). False if nothing is selected.
    bool ComputeSelectionBounds(World& world, glm::vec3& outMin, glm::vec3& outMax) const;
    // World-space AABB over every renderable/placed entity in the scene. False if the scene has
    // nothing to frame. Used as the last resort for view snapping when there's no selection and
    // the camera isn't pointed at anything.
    bool ComputeSceneBounds(World& world, glm::vec3& outMin, glm::vec3& outMax) const;
    // Repositions the camera along its current view direction so the whole selection (single
    // or group) fits in frame, without changing where it's looking (matches most editors'
    // basic "frame selection" behavior — it centers distance, not aim).
    void FocusOnSelection(World& world, Camera& editorCamera);
    // Drops the primary selection straight down onto its own lowest point — a toolbar action
    // (not tied to any one component) since it acts on the whole entity's placement, not a
    // single field. No-op if nothing selected or the selection has no mesh to measure.
    bool CanSnapSelectionToGround(World& world) const;
    void SnapSelectionToGround(World& world);

    // Set in Init() to ProjectPaths::Resolve("scenes/Showcase.json") — the project folder, not the
    // working directory. Left as a bare filename here only as a harmless pre-Init default.
    std::string m_CurrentScenePath = "scenes/Showcase.json";
    bool m_Dirty = false;
    // Undo-stack depth at the last save. When history is walked back to exactly this point the
    // scene matches disk again, so the title should drop its "*" (#22 P22). -1 = no clean point
    // (untitled, or a branching edit discarded the saved state from the redo stack). Starts at 0:
    // the initial scene load in main.cpp leaves an empty history that matches the file on disk.
    int m_SavedUndoDepth = 0;
    void RefreshDirtyFromHistory() {
        m_Dirty = (m_SavedUndoDepth < 0) || ((int)m_UndoStack.size() != m_SavedUndoDepth);
    }
    // Seconds since the last save (manual or auto) — ticked in Draw(), reset by any of the
    // manual Save/Save As/Open/New Scene paths so auto-save never fires moments after one of
    // those. Session-local (not persisted) - EditorSettings::AutoSaveEnabled/IntervalMinutes are
    // the persisted preferences this timer is measured against.
    float m_AutoSaveTimer = 0.0f;

    // --- Crash-recovery auto-save --------------------------------------------------------
    // The auto-save timer writes a full-scene snapshot to a sidecar file (<stem>.recovery.json)
    // NEXT TO the scene file, rather than overwriting the scene file itself — so an unnoticed
    // bad edit that happens to auto-save can never become the only copy. The real scene file
    // changes only on an explicit Save/Save As. On launch, a recovery file newer than the scene
    // file raises a Restore/Discard modal (m_RecoveryPromptPending, drawn by DrawRecoveryPrompt).
    static std::string RecoveryPathFor(const std::string& scenePath);
    void WriteRecoverySnapshot(const World& world, const AssetLibrary& assets);
    void ClearRecoverySnapshot(); // deletes the recovery file for m_CurrentScenePath if present; silent
    void DrawRecoveryPrompt(World& world, AssetLibrary& assets);
    bool m_RecoveryPromptPending = false;

    void DrawExitPrompt();
    bool m_ExitPromptPending = false;
    ExitDecision m_ExitDecision = ExitDecision::None;

    // #195: a scene file whose formatVersion is newer than this build understands still loads
    // best-effort, but SceneSerializer::TakeLoadWarning() comes back non-empty in that case — the
    // Console already got a Log::Error line from SceneSerializer itself, and this modal is the
    // loud, hard-to-miss half of that warning. m_SceneVersionWarning non-empty is what drives the
    // popup open; set it right after any Load() the user can see the result of (OpenScene, the
    // recovery-restore path) via CheckSceneVersionWarning().
    void CheckSceneVersionWarning();
    void DrawSceneVersionWarningPopup();
    std::string m_SceneVersionWarning;

    // Preferences window (Ctrl+,) — replaces the old giant Settings menu-bar dropdown (#53).
    void DrawPreferencesWindow(World& world);
    bool m_ShowPreferences = false;
    int m_PrefsCategory = 0;
    std::string m_PrefsShortcutFilter;
public:
    // The launch-time system report (OS/CPU/RAM/GPU/GL/display/build), built by main.cpp.
    // Shown in Preferences > About.
    void SetSystemReport(const std::vector<std::string>& lines) { m_SystemReport = lines; }
private:
    std::vector<std::string> m_SystemReport;

    // Save routing. m_CurrentScenePath is EMPTY for an untitled scene (File > New Scene): it has
    // no file to overwrite, so main.cpp skips the save-on-exit and DoSave() must prompt for a
    // location. DoSaveAs() always prompts. Both reset dirty/timer and clear the stale recovery
    // snapshot on success.
    void DoSave(World& world, AssetLibrary& assets);
    bool DoSaveAs(World& world, AssetLibrary& assets);

    GizmoOp m_GizmoOp = GizmoOp::Translate;
    bool m_GizmoLocalSpace = false; // false = world-aligned handles, true = aligned to the object's own rotation
    float m_GizmoSize = 0.20f;      // ImGuizmo clip-space units; 0.1 is its stock default but reads too
                                    // small against typical geometry and the origin axes (#43 P26, audit #85)
    bool m_GizmoEngaged = false;
    bool m_GizmoWasUsing = false;
    bool m_PrevLeftMouseDown = false;

    // Box/marquee select: press-drag-release in empty viewport space. Whether it turns out to
    // be a plain click (single-object pick, existing behavior) or a real drag (select every
    // object whose screen-space bounds fall in the rectangle) is only decided on release.
    bool m_BoxSelectActive = false;
    glm::vec2 m_BoxSelectStart{0.0f, 0.0f};
    void AddToSelectionIfAbsent(entt::entity entity); // additive-only: never toggles an already-selected item off

    bool m_ShowGrid = true;
    bool m_ShowGizmos = true; // View menu toggle for the viewport transform gizmo (audit #60)
    bool m_FrameOnSelect = false;   // auto-frame the editor camera when the selection changes (#69)
    bool m_PendingFrameSelect = false; // set by SelectItem, consumed in Draw() where the camera is in scope
    bool m_GridSnapEnabled = true;    // hold Ctrl to invert momentarily, Blender-style
    float m_SnapTranslation = 1.0f;
    float m_SnapRotationDeg = 15.0f;
    float m_SnapScale = 0.1f;
    // How close (screen pixels) the cursor must be to a vertex to grab/preview/snap onto it —
    // one setting governs the hover circle, the initial grab, AND the drag-time snap search,
    // so "the circle is showing" and "this will snap" always mean the same thing.
    float m_VertexPickPixels = 35.0f;

    // Hold-V vertex drag: hold V near the selected model's vertex (shown as a yellow circle),
    // click it to grab, drag to move the whole object with that vertex snapping onto the
    // nearest vertex of any OTHER model, release the mouse button or V to drop it in place.
    bool m_VertexDragActive = false;
    glm::vec3 m_VertexDragLocal{0.0f};       // grabbed vertex, in the selected model's local space
    glm::vec3 m_VertexDragPlanePoint{0.0f};  // grabbed vertex's world position at drag start (fixes the drag depth)
    glm::vec3 m_VertexDragOffset{0.0f};      // object pivot position minus grabbed-vertex world position, held constant

    // Nearest vertex on the SELECTED model to the cursor, in local space. Used both for the
    // continuous hover preview (so you can see you're in range before clicking) and to start
    // a drag. Returns false if no model is selected or nothing is within range.
    bool FindVertexUnderCursor(World& world, Camera& editorCamera, glm::vec3& outLocalPos) const;
    void UpdateVertexDrag(World& world, Camera& editorCamera);

    // Undo/redo: whole-scene JSON snapshots (via SceneSerializer, entities AND AssetLibrary
    // state both), pushed at the start of a discrete edit (gizmo drag, field drag, import,
    // delete, rename, folder move...) rather than every frame. `Label` is what the History
    // panel (DrawHistoryPanel) shows for that step, and `SelectedOrders` is what was selected
    // right before the edit — restored by stable OrderComponent value (not raw entt::entity,
    // which a full scene reload invalidates, and not by name, which collides whenever two
    // entities share a name - #217) when undoing back to this point.
    struct UndoEntry {
        std::string SceneJson;
        std::vector<int> SelectedOrders;
        std::string Label;
    };
    std::vector<UndoEntry> m_UndoStack;
    std::vector<UndoEntry> m_RedoStack;
    static constexpr size_t kMaxHistory = 100;

    // Set at the top of every Draw() call - PushUndo needs AssetLibrary to snapshot its state
    // (folders, display names, import settings) alongside the entity graph, but threading an
    // extra parameter through every one of PushUndo's call sites (most of which have nothing to
    // do with assets) wasn't worth the churn. Valid for the object's whole lifetime in practice:
    // main.cpp owns exactly one AssetLibrary for the life of the program.
    AssetLibrary* m_AssetsPtr = nullptr;

    // Also set at the top of every Draw() - lets deep Inspector code (e.g. the Camera section's
    // "Align to View") reach the editor camera without threading it through every draw helper.
    Camera* m_EditorCameraPtr = nullptr;

    void PushUndo(const World& world, const std::string& label = "Edit");
    void Undo(World& world, AssetLibrary& assets);
    void Redo(World& world, AssetLibrary& assets);

    // Staged undo for widgets whose one logical edit spans many frames / a popup (colour
    // pickers, sliders). StageUndo snapshots once, on the first activation of the interaction;
    // CommitStagedUndo pushes that snapshot onto the stack only when the edit actually finishes
    // with a change. So a colour-pick session is ONE undo step, not one per popup sub-widget
    // re-activation, and opening a picker without changing anything adds nothing (issue #11).
    bool m_HasStagedUndo = false;
    std::string m_StagedUndoJson;
    std::vector<int> m_StagedUndoSelectedOrders;
    void StageUndo(const World& world);
    void CommitStagedUndo(const World& world, const std::string& label);
    // Repeatedly calls Undo()/Redo() until the entry at this position in the visible history
    // list (see DrawHistoryPanel) becomes current - each step is still a single full-snapshot
    // load, not incremental replay, so this stays cheap even jumping many steps at once.
    void JumpToUndoEntry(World& world, AssetLibrary& assets, size_t undoStackIndex);
    void JumpToRedoEntry(World& world, AssetLibrary& assets, size_t redoStackIndex);

    std::vector<int> CaptureSelectedOrders(const World& world) const;
    void RestoreSelectionByOrder(World& world, const std::vector<int>& orders);

    bool m_ShowHistory = false;
    void DrawHistoryPanel(World& world, AssetLibrary& assets);

    // Per-light solo / mute — editor-only, never serialized, cleared by NewScene/OpenScene.
    // See IsLightSuppressed(); consumed by main.cpp's per-frame light gather.
    std::unordered_set<entt::entity> m_SoloLights;
    std::unordered_set<entt::entity> m_MutedLights;

    // The three "core" docked panels. Always started visible; the Window menu (and each
    // window's own close button) can hide them, Reset Layout brings them all back.
    bool m_ShowHierarchy = true;
    bool m_ShowInspector = true;
    bool m_ShowAssetBrowser = true;

    // The Hierarchy row usage hint is helpful once and noise forever — show it only until the
    // user has actually clicked a row, then never again this session (audit #70).
    bool m_HierarchyRowHintDone = false;

    // Engine wordmark ("TARTARUS ENGINE" text, no icon), drawn small and translucent above the
    // Inspector panel — loaded once in Init() from assets/branding/ (a build-time copy of
    // extern/branding/, same treatment as the icon font). Null and silently skipped if the
    // file's missing.
    std::unique_ptr<Texture> m_LogoTexture;
    // Engine mark (the "TE" monogram, no text), spinning slowly in the viewport's bottom-left
    // corner — same load treatment as m_LogoTexture, just the other half of the full lockup.
    std::unique_ptr<Texture> m_MarkTexture;
    float m_MarkSpinAngle = 0.0f; // radians, advanced by dt * EngineMarkSpinSpeed each frame in DrawEngineMark()
    float m_MarkHue = 0.0f;        // 0..1, advanced each frame; drives the tint when EngineMarkRgb is on
    // Contrast-adaptive tint for the mark: each frame DrawEngineMark reads back the little patch
    // of the scene texture directly behind the mark, and eases this toward white over dark
    // content / black over light content. 1 = white, 0 = black; starts white (matches the old
    // fixed colour) so the first frame before any readback looks unchanged.
    unsigned int m_MarkSampleFbo = 0; // scratch read-FBO wrapping m_SceneColorTexture for the readback
    float m_MarkContrastLum = 1.0f;    // eased, per-frame
    float m_MarkContrastTarget = 1.0f; // refreshed by the throttled readback
    float m_MarkSampleAccum = 0.0f;    // seconds since last readback (sampled ~10 Hz, not per-frame,
                                       // so the GPU->CPU sync never touches the 60 fps frame budget)
    // Same contrast-adaptive trick for the viewport-top-center Play/Stop button: sample the scene
    // luminance behind it and steer the glyph white-on-dark / dark-on-light. Its own eased state
    // because it sits far from the corner mark and reads a different patch of the scene.
    float m_PlayBtnContrastLum = 1.0f;
    float m_PlayBtnContrastTarget = 1.0f;
    float m_PlayBtnSampleAccum = 0.0f;
    AsyncLuminanceReadback m_PlayBtnReadback; // ping-ponged PBOs backing the sample above (#178)
    // Same again for the top-right nav-gizmo cluster (dolly / pan tool buttons + the Persp/axis
    // label): its own sample because the corner it lives in can differ in brightness from the
    // top-center where the Play button sits.
    float m_NavGizmoContrastLum = 1.0f;
    float m_NavGizmoContrastTarget = 1.0f;
    float m_NavGizmoSampleAccum = 0.0f;
    AsyncLuminanceReadback m_NavGizmoReadback;
    // ...and for the bottom-of-viewport status readout, now a bare adaptive-tinted line of text
    // with no strip behind it.
    float m_StatusBarContrastLum = 1.0f;
    float m_StatusBarContrastTarget = 1.0f;
    float m_StatusBarSampleAccum = 0.0f;
    AsyncLuminanceReadback m_StatusBarReadback;
    // ...and for the two transparent viewport HUDs — Statistics (top-left) and History (bottom-
    // right). Each reads the patch of scene directly behind it, so they tint independently.
    float m_StatsHudContrastLum = 1.0f;
    float m_StatsHudContrastTarget = 1.0f;
    float m_StatsHudSampleAccum = 0.0f;
    AsyncLuminanceReadback m_StatsHudReadback;
    // Set each frame by DrawStatsPanel: true when the (capped) Statistics HUD reaches far enough
    // down the left edge to collide with the corner monogram — the mark is skipped while so.
    bool m_HideEngineMarkForStats = false;
    float m_HistoryHudContrastLum = 1.0f;
    float m_HistoryHudContrastTarget = 1.0f;
    float m_HistoryHudSampleAccum = 0.0f;
    AsyncLuminanceReadback m_HistoryHudReadback;
    // Live Game-view rect + texture, pushed in each frame by main.cpp (zero size = none). Used by
    // DrawPlayStopButton to place the Stop/Fullscreen control over the game viewport and tint it.
    ImVec2 m_GameViewImgPos{0.0f, 0.0f};
    ImVec2 m_GameViewImgSize{0.0f, 0.0f};
    unsigned int m_GameViewTex = 0;
    int m_GameViewTexW = 0;
    int m_GameViewTexH = 0;
    // DVD-screensaver idle bounce: after 30 s with no mouse/keyboard input the mark launches out
    // of its corner and ricochets around the viewport edges; any input eases it back home.
    float m_MarkIdleTime = 0.0f;
    glm::vec2 m_MarkPos{0.0f, 0.0f}; // drawn centre, screen px — eases to the corner, or bounces
    glm::vec2 m_MarkVel{0.0f, 0.0f}; // px/sec, non-zero only while bouncing
    bool m_MarkBouncing = false;
    bool m_MarkPosValid = false;     // false until first laid out, so it doesn't fly in from (0,0)
    void DrawEngineMark(float dt);

    // Prism theme: a hue phase [0,1) advanced every frame in Draw() while EditorTheme == 1, and
    // the routine that repaints all the hue-driven style colours (accent, buttons, text tint,
    // tab keyline, ...) from it. ApplyEditorTheme() sets the static near-black backgrounds; this
    // rides on top each frame so the palette drifts through the spectrum. No-op for Dark Slate.
    float m_ThemeHue = 0.0f;
    void ApplyPrismAnimation(float hue);

    // Full-width strip above the viewport: everything lives here now — one-click toggles in
    // the row below the menu bar, and File/Import/Add/Settings as dropdown menus (Settings
    // holds what used to be a separate always-open side panel: environment, grid/snap tuning,
    // vertex snap tuning, controls reference).
    void DrawTopToolbar(World& world, AssetLibrary& assets, Camera& editorCamera);
    // Add-menu contents, shared by the menu bar and the Shift+A quick-add popup.
    void DrawAddEntityItems(World& world, AssetLibrary& assets, Camera& editorCamera);
    bool m_OpenQuickAdd = false; // set by the Shift+A shortcut, consumed next frame in Draw()
    std::string m_LastSelectedName; // last valid scene selection, shown in the Inspector empty state

    // Inspector Transform > Copy / Paste Values (#77) — stored as raw vectors to keep
    // Components.h out of this header.
    glm::vec3 m_TransformClipPos{0.0f}, m_TransformClipRot{0.0f}, m_TransformClipScale{1.0f};
    bool m_HasTransformClipboard = false;

    // Inspector Tag field: dropdown of known tags with an inline "New tag..." entry mode.
    bool m_TagAdding = false;
    bool m_TagAddingJustOpened = false;
    char m_TagAddBuf[64] = "";
    void DrawHierarchy(World& world, AssetLibrary& assets);
    void DrawInspector(World& world, AssetLibrary& assets, float dt);
    void DrawAssetBrowser(World& world, AssetLibrary& assets);
    std::string m_AssetSearchFilter;
    std::string m_AssetLabelMenuFilter; // the Label filter popup's own mini search box
    char m_LabelsEditBuffer[256] = {};  // the per-asset "Edit Labels..." popup's comma-separated text

    // Loads `path` as the active scene, resetting selection/undo state the same way the
    // File > Open... dialog does — shared so the Asset Browser's Scenes folder can open a
    // scene with one double-click instead of going through the OS file dialog.
    void OpenScene(World& world, AssetLibrary& assets, const std::string& path);
    void NewScene(World& world, AssetLibrary& assets); // fresh scene, written to disk immediately; File > New Scene / Ctrl+N

    // Guards New Scene / Open Scene against silently discarding unsaved work (#216). Every call
    // site that wants to switch scenes — Ctrl+N/Ctrl+O, the File menu, a dropped .json, and both
    // Asset Browser open paths — goes through these instead of calling NewScene/OpenScene
    // directly. If the current scene is dirty, the switch is deferred behind DrawSceneSwitchPrompt
    // (Save / Don't Save / Cancel, same as the on-exit prompt); Cancel aborts the switch entirely.
    // If the scene isn't dirty, the switch happens immediately.
    enum class PendingSceneSwitch { None, New, Open };
    void RequestNewScene(World& world, AssetLibrary& assets);
    void RequestOpenScene(World& world, AssetLibrary& assets, const std::string& path);
    void DrawSceneSwitchPrompt(World& world, AssetLibrary& assets);
    bool m_ScenePromptPending = false;
    PendingSceneSwitch m_PendingSceneSwitch = PendingSceneSwitch::None;
    std::string m_PendingScenePath;

    // Unity Project-window style browsing: the current virtual folder ("" = root), the
    // browser-local selection (separate from the scene selection — this is for F2/right-click
    // actions on library assets, not placed objects), and inline-rename state shared by both
    // folders and assets (they use the same InputText-in-place mechanic).
    std::string m_CurrentAssetFolder;
    std::string m_SelectedAssetKey;
    bool m_SelectedAssetIsFolder = false;

    // Unity Project-window-style left folder tree, shown alongside the icon grid rather than
    // breadcrumb-only navigation. Which folders are expanded is tracked here (not left to
    // ImGui's own per-ID tree state) so Alt+click can recursively force every descendant open
    // or closed in one pass - something ImGui's own TreeNode open-state has no hook for.
    std::set<std::string> m_ExpandedAssetFolders;
    // #157 — the folder tree is the only navigator now (breadcrumb is plain text, no ".." row),
    // so when the current folder changes from anywhere its ancestors are force-opened in the
    // tree and it's scrolled into view. Tracks the folder we last did that for + a one-shot
    // scroll request the tree node consumes.
    std::string m_AssetFolderTreeRevealed;
    bool m_RevealAssetFolderInTree = false;
    float m_AssetTreeWidth = 180.0f; // drag-resizable via the splitter between tree and grid
    void DrawFolderTreeNode(World& world, AssetLibrary& assets, const std::string& folderPath, bool isRoot);
    void SetFolderExpandedRecursive(AssetLibrary& assets, const std::string& folderPath, bool expand, bool recursive);

    // Icon size for the grid on the right - dragging the slider at its bottom below
    // kListViewIconSize switches to a compact list (icon + name per row) instead of tiles,
    // matching Unity's "slide to the extreme left for list view" behavior.
    float m_AssetIconSize = 64.0f;
    static constexpr float kListViewIconSize = 24.0f;

    // Import Settings panel state (see DrawAssetImportInspector): a local, edited-but-not-yet-
    // applied copy of whichever texture/model's settings are currently shown, plus which asset
    // key it belongs to — so switching the Asset Browser selection reloads a fresh copy from
    // AssetLibrary instead of carrying over a stale edit onto the next asset.
    std::string m_ImportInspectorKey;
    TextureImportSettings m_PendingTextureSettings;
    ModelImportSettings m_PendingModelSettings;
    bool m_ImportSettingsDirty = false;
    void DrawAssetImportInspector(World& world, AssetLibrary& assets, const std::string& key);

    // R/G/B/A channel-isolation toggle on the texture preview above — -1 shows the texture
    // combined/normal. Rendered lazily: m_ChannelPreviewRenderedKey/Channel track what's
    // CURRENTLY in the offscreen texture, so switching tabs and back without changing anything
    // doesn't re-render every frame for no reason.
    int m_ChannelPreviewChannel = -1;
    std::string m_ChannelPreviewRenderedKey;
    int m_ChannelPreviewRenderedChannel = -2; // never equals a real channel value on frame 1
    ChannelPreviewRenderer m_ChannelPreview;

    // Orbit-camera state for the model preview - reset (angle to a pleasant default, distance
    // auto-framed to the model's own bounds) whenever the inspected model changes, then driven
    // by drag-to-orbit/scroll-to-zoom on the preview image itself.
    float m_ModelPreviewYaw = 0.6f;
    float m_ModelPreviewPitch = 0.35f;
    float m_ModelPreviewDistance = 3.0f;
    bool m_ModelPreviewDragging = false; // press started while hovering the preview image
    ModelPreviewRenderer m_ModelPreview;

    // Asset Browser model thumbnails (#18 P18): each Model rendered once into its own small GL
    // texture and cached, so a folder of FBXs shows real previews instead of a generic cube
    // glyph. A per-frame budget keeps opening a big folder from stalling; the blit FBO copies
    // the shared preview render into the per-model texture (via glCopyTexSubImage2D).
    //
    // Keyed by asset path (#181) rather than the Model* — stable across a Model being destroyed
    // and a new allocation reusing the same address, unlike the pointer. Bounded to
    // kMaxModelThumbnails with simple LRU eviction (a doubly-linked list for recency order plus
    // the map for O(1) lookup/touch) so browsing a large library over a session doesn't leak GL
    // textures forever; each 128x128 RGBA8 thumbnail is 64KB, so the cap keeps this well under a
    // megabyte of VRAM.
    ModelPreviewRenderer m_ThumbnailPreview;
    static constexpr size_t kMaxModelThumbnails = 128;
    std::list<std::string> m_ThumbnailLRU; // front = most recently used
    std::unordered_map<std::string, std::pair<unsigned int, std::list<std::string>::iterator>> m_ModelThumbnails;
    unsigned int m_ThumbnailBlitFbo = 0;
    int m_ThumbnailBudgetThisFrame = 0;
    unsigned int ModelThumbnail(Model& model); // cached GL texture, or 0 while over this frame's budget
    void InvalidateModelThumbnail(const std::string& path = ""); // empty = clear all (e.g. a freed Model could be reallocated at the same address)

    // Thumbnails for the Asset Browser's "Screenshots" folder — keyed by file path, kept in sync
    // with what's on disk each cache refresh (#175; entries drop when their file is gone).
    // Loading is capped by its own per-frame budget (#176) so opening a folder of many captures
    // decodes/uploads a few at a time instead of stalling on the frame the folder is opened;
    // unloaded entries just aren't inserted into the map yet, so they're retried next frame.
    std::unordered_map<std::string, std::shared_ptr<Texture>> m_ShotThumbs;
    int m_ScreenshotThumbBudgetThisFrame = 0;

    // Cached directory listings backing the filesystem-based "Scenes" and "Screenshots" folders
    // (#175) — std::filesystem::directory_iterator used to run every single frame while either
    // folder was open or a search/filter was active. Now refreshed only on a short timer, when
    // the Asset Browser regains focus, or right after an operation that actually creates/deletes/
    // duplicates a file in one of those folders (a rename never touches these two folders on
    // disk — see CommitRename — so it isn't a trigger).
    struct AssetDirListingCache {
        std::vector<std::string> paths; // generic_string() full paths of the matching files
        bool valid = false;
    };
    AssetDirListingCache m_ScenesListingCache;
    AssetDirListingCache m_ShotsListingCache;
    float m_AssetListingRefreshTimer = 0.0f;   // ticks up in DrawAssetBrowser; see kAssetListingRefreshInterval
    bool m_AssetBrowserFocusedLastFrame = false; // edge-detects m_AssetBrowserFocused for "just gained focus"
    void RefreshScenesListingIfNeeded();
    void RefreshShotsListingIfNeeded();
    void InvalidateScenesListing() { m_ScenesListingCache.valid = false; }
    void InvalidateShotsListing() { m_ShotsListingCache.valid = false; }

    // The screenshot lightbox (DrawScreenshotPreview). Its own full-res Texture, not a m_ShotThumbs
    // entry, so it survives that map being pruned and isn't size-capped to the thumbnail budget.
    std::string m_ShotPreviewPath;
    std::shared_ptr<Texture> m_ShotPreviewTex;
    bool m_ShotPreviewOpen = false;
    float m_ShotPreviewAnim = 0.0f;              // 0->1 pop-in ease
    float m_ShotPreviewZoom = 1.0f;              // 1 = fit-to-canvas; scroll wheel scales it
    ImVec2 m_ShotPreviewPan{0.0f, 0.0f};         // image offset within the canvas, in pixels
    bool m_ShotPreviewPanning = false;
    bool m_ShotPreviewPressOutside = false;       // the active L-press began on the dimmed backdrop

    // Drains dropped/imported files a few per frame instead of all at once in a single
    // synchronous stall — see ImportQueueManager.h for why this isn't a background thread.
    // ImportQueueManager only carries plain paths, so the virtual Asset Browser folder each
    // queued file resolved to (computed once, up front, when a dropped folder's structure is
    // walked) is looked up from here at actual-import time and erased once consumed.
    ImportQueueManager m_ImportQueue;
    std::map<std::string, std::string> m_ImportTargetFolder;
    std::string m_RenamingAssetKey; // empty = not renaming anything right now
    bool m_RenamingIsFolder = false;
    bool m_RenamingJustStarted = false; // one-shot: focus + select-all the rename field on the frame it opens
    char m_RenameBuffer[128] = {};
    void BeginRenameAsset(const std::string& key, bool isFolder, const std::string& currentName);

    // One entry in the Asset Browser's multi-selection — a key plus whether it's a folder,
    // since folder paths and asset paths are separate namespaces that could theoretically
    // collide as plain strings.
    struct AssetKeyRef {
        std::string Key;
        bool IsFolder = false;
        bool operator==(const AssetKeyRef& o) const { return Key == o.Key && IsFolder == o.IsFolder; }
    };
    // m_SelectedAssetKey/m_SelectedAssetIsFolder is the "primary"/anchor selection (same role as
    // m_Selected for scene entities); m_ExtraAssetSelection holds everything else co-selected via
    // Ctrl/Shift-click — mirroring the m_Selected + m_ExtraSelection split already used for scene
    // entities, just for library assets instead. Actions that only make sense for exactly one
    // asset (Rename) act on the primary alone; actions that generalize (Delete, Duplicate) act on
    // the whole selection.
    std::vector<AssetKeyRef> m_ExtraAssetSelection;
    // Anchor index (into DrawAssetBrowser's locally-built, currently-visible `cells` list) that a
    // Shift-click range-select extends from. Only meaningful within the same frame/folder view it
    // was set in; a stale value after navigating just produces a harmless one-off range on the
    // next Shift-click, never a crash (always bounds-checked against the live cell count).
    size_t m_AssetSelectionAnchorIndex = 0;
    bool IsAssetSelected(const std::string& key, bool isFolder) const;
    void ClearAssetSelection(); // clears BOTH the primary and every extra selection
    // Ctrl-click toggle: adds the item if it's not part of the selection, removes it if it is
    // (promoting an extra to primary if the primary itself gets removed) — same shape as the
    // scene-entity toggle-select this mirrors.
    void ToggleAssetSelection(const std::string& key, bool isFolder);

    // Every Asset Browser delete path (Delete key, right-click Delete Folder/Remove from
    // Library) funnels through these, matching Unity's own "Delete" (with a confirmation dialog)
    // vs. Shift+"Delete" (skip it) split. Folders now delete recursively — everything nested
    // under one goes with it — which is exactly why the dialog exists; a single asset is arguably
    // safe enough not to need it, but confirming both keeps the behavior predictable rather than
    // "sometimes asks, sometimes doesn't." Deleting with multiple assets selected deletes all of
    // them behind one confirmation, listing the count.
    void RequestDeleteAsset(World& world, AssetLibrary& assets, const std::string& key, bool isFolder, bool skipDialog);
    void RequestDeleteAssets(World& world, AssetLibrary& assets, const std::vector<AssetKeyRef>& items, bool skipDialog);
    // The actual removal of one item, called either directly (skipDialog) or from the
    // confirmation popup's Delete button, once per pending item. No-op for a key that isn't a
    // recognized AssetLibrary entry (e.g. a Scene).
    // Returns false and fills m_DeleteError with a human-readable reason when the item can't be
    // removed (e.g. a scene file that's write-protected or currently open).
    bool PerformAssetDelete(World& world, AssetLibrary& assets, const std::string& key, bool isFolder);
    // Drawn once at the Asset Browser's top level (not nested under any per-cell PushID) so it's
    // reachable by name regardless of which cell's context menu requested it.
    void DrawDeleteConfirmPopup(World& world, AssetLibrary& assets);
    std::vector<AssetKeyRef> m_PendingDelete;
    bool m_OpenDeleteConfirmRequested = false;
    std::string m_DeleteError;                  // non-empty -> the "Can't Delete" modal shows it
    bool m_OpenDeleteErrorRequested = false;

    // Copies every selected asset's file on disk (Model/Texture/Sound/Prefab only - not a Scene
    // or a folder; non-duplicable entries in the selection are silently skipped) to a numbered
    // sibling each and registers it in the library the same way importing it fresh would -
    // Ctrl+D's Asset Browser meaning, distinct from DuplicateSelection (which duplicates a placed
    // scene entity instead).
    void DuplicateSelectedAsset(World& world, AssetLibrary& assets);

    // True for the frame(s) the Asset Browser (or a child region inside it) has keyboard focus -
    // several shortcuts below (F to frame-in-browser, Ctrl+D to duplicate the asset rather than
    // the scene selection, Enter/Backspace/arrow-key navigation) only make sense while it does,
    // mirroring Unity's own "available when the browser view has focus" scoping.
    bool m_AssetBrowserFocused = false;
    // One-shot: Ctrl+F sets this, DrawAssetBrowser's search box calls SetKeyboardFocusHere and
    // clears it on the next frame it draws.
    bool m_AssetSearchFocusRequested = false;

    void CommitRename(World& world, AssetLibrary& assets);

    // --- Play mode (see OnEnterPlayMode/OnExitPlayMode) -------------------------------------
    // The scene as it was the instant Play was pressed, as a SceneSerializer JSON string — the
    // same snapshot format undo/redo already uses. Empty when not in (or never entered) play.
    std::string m_PlayModeSnapshot;
    // Selection captured by stable OrderComponent value on Play, re-resolved to fresh entity
    // ids on Stop — the registry is rebuilt in between and entt recycles ids (#110).
    std::vector<int> m_PlaySelectionOrders;
    // #199: handles for every AudioSourceComponent with Play On Start, begun the instant Play
    // mode is entered. Kept per-entity (rather than relying on AudioEngine::StopAll()) so exiting
    // Play stops exactly these voices and leaves an unrelated editor preview sound (Inspector
    // Preview button, Asset Browser) started mid-Play alone. Cleared on both enter and exit.
    std::unordered_map<entt::entity, AudioEngine::SoundHandle> m_PlayModeAudioHandles;

    // --- Console ------------------------------------------------------------------------
    void DrawConsole();
    bool m_ShowConsole = true;
    bool m_ConsoleShowInfo = true;
    bool m_ConsoleShowWarning = true;
    bool m_ConsoleShowError = true;
    bool m_ConsoleAutoScroll = true;
    bool m_ConsoleShowTimestamps = true;
    std::string m_ConsoleFilter;
    unsigned int m_ConsoleSeenRevision = 0; // only auto-scroll when Log actually gained an entry

    // #219: indices into Log::Entries() that currently pass the level toggles + text filter,
    // rebuilt only when one of those inputs (or the log itself, via Log::Revision()) changes —
    // not every frame — so DrawConsole can drive ImGuiListClipper over a stable list instead of
    // re-filtering and submitting all 1000 possible entries each frame.
    std::vector<size_t> m_ConsoleFilteredIndices;
    unsigned int m_ConsoleFilterCacheRevision = (unsigned int)-1; // forces a rebuild on first draw
    std::string m_ConsoleFilterCacheFilter;
    bool m_ConsoleFilterCacheShowInfo = true;
    bool m_ConsoleFilterCacheShowWarning = true;
    bool m_ConsoleFilterCacheShowError = true;

    // The spinning corner monogram is governed by EditorSettings (EngineMarkEnabled / SpinSpeed /
    // Rgb) so the choice persists and the Preferences + Window-menu controls share one source of
    // truth. (Was m_ShowEngineMark — a session-only bool — before the Preferences controls landed.)

    // Contrast-adaptive text tint for the transparent viewport HUDs: sample the scene behind a
    // screen-space box, ease the eased/target luminance pair (throttled ~10 Hz via `accum`), and
    // push ImGuiCol_Text + ImGuiCol_TextDisabled so the readout rides white-on-dark / dark-on-
    // light like the corner mark. Always pushes exactly 2 style colours — caller pops them after
    // its content. Same maths as DrawEngineMark / DrawViewportStatusBar.
    void PushAdaptiveHudText(AsyncLuminanceReadback& rb, ImVec2 centerScreen, float boxPx, float dt,
                             float& easedLum, float& targetLum, float& sampleAccum);

    // --- Statistics --------------------------------------------------------------------
    // Compact transparent HUD pinned to the Scene viewport's top-left corner (#149): FPS/ms,
    // draw/tri/vert counts, per-category entity counts, the Profiler sample list, and the
    // GL-state-cache bind table. Auto-sized to its text, click-through, toggled via
    // EditorSettings::SceneShowStats.
    void DrawStatsPanel(World& world, float dt);
    // Thin always-on strip along the bottom of the Scene viewport: FPS / ms / draws / tris /
    // selection count / active tool — the at-a-glance surface (#92). The Statistics panel above
    // is the deeper readout.
    void DrawViewportStatusBar();
    // Panel visibility lives in EditorSettings::SceneShowStats (persisted), not a plain member.
    RenderStats m_RenderStats;
    // Smoothed so the number is readable instead of flickering every frame.
    float m_SmoothedFrameMs = 16.6f;

    // --- Clipboard (Ctrl+C / Ctrl+X / Ctrl+V) ---------------------------------------------
    // Copied entities as a scene-fragment JSON string, so a paste rebuilds them through the
    // exact same code path a scene load uses (materials, sounds, components and all) instead
    // of needing its own parallel copy routine.
    std::string m_Clipboard;
    void CopySelection(World& world);
    void PasteClipboard(World& world, AssetLibrary& assets);

    // --- Hierarchy ------------------------------------------------------------------------
    std::string m_HierarchyFilter;   // plain substring, or "t:SomeTag" to filter by TagComponent
    entt::entity m_RenamingEntity = entt::null;
    bool m_EntityRenameJustStarted = false;
    char m_EntityRenameBuffer[128] = {};
    // Right-click context menu shared by the Hierarchy's rows and its empty space; `entity` is
    // entt::null for the empty-space case (only the create/paste items apply then).
    void DrawHierarchyContextMenu(World& world, AssetLibrary& assets, entt::entity entity);
    // One Hierarchy row plus its children — recursive, so parenting shows as real nesting.
    void DrawHierarchyNode(World& world, AssetLibrary& assets, entt::entity entity, bool isLevelGeometry);
    // Alt-clicking a parent's foldout arrow expands/collapses every descendant, not just its
    // immediate children (Unity's Hierarchy gesture) — ImGui tracks each TreeNodeEx's open state
    // itself, keyed by ID, so this walks the same PushID(entity)/"##node" nesting DrawHierarchyNode
    // uses and pokes that stored state directly rather than keeping a second, parallel expanded-set
    // the way the Asset Browser's folder tree does. Must be called from inside the same ID stack
    // position the target entity's own row would be drawn at (i.e. from within its parent's
    // PushID scope), so the computed IDs line up with the ones TreeNodeEx will look up next frame.
    void SetHierarchyExpandedRecursive(World& world, entt::entity entity, bool open);
    bool MatchesHierarchyFilter(const World& world, entt::entity entity) const;
    void BeginRenameEntity(entt::entity entity);
    // Spawns a Transform+Name entity at the given spot and selects it (Unity's Create Empty),
    // optionally with a LightComponent already attached.
    entt::entity CreateEmptyAt(World& world, Camera* editorCamera, const char* name, bool asLight);
    void CreateEmptyParentForSelection(World& world); // Hierarchy right-click "Group into Empty Parent" (#71)
    // Hierarchy right-click "Unparent" (#220) — moves the whole selection to the scene root, the
    // same operation the drag-to-empty-space gesture performs, but always reachable even once the
    // tree fills the panel and that drop zone collapses to nothing.
    void UnparentSelection(World& world);

    // --- Inspector: Add / Remove Component -------------------------------------------------
    void DrawAddComponentMenu(World& world, AssetLibrary& assets, entt::entity entity);
    // Draws one removable component section with a header and a trailing "x" — returns true if
    // the body should be drawn (i.e. the header is expanded and the component wasn't removed),
    // in which case the caller MUST call EndComponentSection() after drawing the body (it
    // indents on open; Begin/End keeps that indent balanced). `defaultOpen` lets a
    // frequently-tweaked component (Transform) start expanded while a set-and-forget one
    // (Audio Source) starts collapsed, so a component-heavy Inspector doesn't open as one long
    // wall of fields.
    // Used by both the single-select and multi-select Inspector paths (#155) so they share one
    // heading language — hence no entity argument. Collapse state is keyed by `label`.
    bool BeginComponentSection(const char* icon, const char* label,
        bool removable, bool& removedOut, bool defaultOpen = true, const char* tooltip = nullptr);
    void EndComponentSection();

    ShadingMode m_ShadingMode = ShadingMode::Shaded;
    // Unity's Pivot/Center toggle: false = gizmo sits on the object's own origin, true = on the
    // center of its bounding box (or of the whole group, for a multi-selection).
    bool m_GizmoPivotCenter = false;

    // PBR + texture-map editing for one or more selected entities, with a mixed-value dash for
    // fields the selected materials disagree on (single selection never shows one — `sel` of
    // size 1 can't disagree with itself). Only meaningful when every entity in `sel` has a mesh;
    // the caller checks that (single-select callers pass a one-element `sel`).
    void DrawMaterialEditor(World& world, AssetLibrary& assets,
                            const std::vector<entt::entity>& sel);
    void DrawGizmo(World& world, Camera& editorCamera);
    // Small screen-space markers for entities with no mesh (lights, empties) — without these
    // they'd be invisible and unclickable in the viewport, since there's nothing to rasterize.
    void DrawEntityIcons(World& world, Camera& editorCamera);
    // 3D wireframe shapes for lights — range sphere (point), cone (spot), aim arrow
    // (directional) — projected to screen and drawn into the Scene window's draw list, same
    // clipping treatment as DrawEntityIcons. Gated on EditorSettings::ShowLightGizmos.
    void DrawLightGizmos(World& world, Camera& editorCamera);

    // Unity-style grab dots on the selected light's gizmo: drag to scale Range, open/close the
    // spot cone, or re-aim a spot/directional light — no trip to the Inspector. Draws the dots
    // and runs their hover/drag; called from Draw() just before HandleViewportPicking so a
    // handle grab suppresses the box-select/deselect click.
    enum class LightHandle { None, Range, SpotAngle, Aim };
    void UpdateLightHandles(World& world, Camera& editorCamera);
    // The grab dots only appear once you've clicked the light in the VIEWPORT (its icon) — not
    // when it's selected from the Hierarchy / Lights panel / a box-select. Holds that light;
    // cleared by any other selection change. (#140 follow-up.)
    entt::entity m_LightHandleArmedFor = entt::null;
    LightHandle m_HotLightHandle = LightHandle::None; // hovered, or (while dragging) the grabbed one
    int   m_HotLightHandleIndex  = -1;   // which dot of that kind, for the hover highlight
    bool  m_LightHandleDragging  = false;
    bool  m_LightHandleEngaged   = false; // this frame a handle owns the cursor -> no viewport pick
    glm::vec3 m_LightHandleGrabAxis{0.0f}; // world direction the grabbed dot slides along
    float m_LightHandleGrabParam = 0.0f;  // aim-handle distance captured at grab time
    void HandleViewportPicking(World& world, Camera& editorCamera);
    // Blender/Godot-style navigation gizmo (ImViewGuizmo) pinned to the viewport's top-right
    // corner: a rotate ring plus small dolly/pan buttons underneath. Camera is yaw/pitch, not
    // quaternion, so this converts to/from a quaternion around the call into the library.
    void DrawViewGizmo(World& world, Camera& editorCamera);
    // True for the frame(s) the mouse is hovering or dragging the nav gizmo above — set inside
    // DrawViewGizmo() (called before HandleViewportPicking() in Draw()) so picking can skip
    // starting a box-select/pick from a click that's actually meant for the nav gizmo. The nav
    // gizmo's own overlay window is NoInputs (so it doesn't steal WantCaptureMouse from the
    // rest of the viewport), so without this its clicks fell straight through to picking.
    bool m_ViewGizmoBlocking = false;

    // Lets a Model entry from the Asset Browser be dropped into the 3D view to place a new
    // instance, active only while a drag from the Asset Browser is in progress so it never
    // otherwise sits on top of the viewport intercepting camera input.
    void DrawViewportDropTarget(World& world, AssetLibrary& assets, Camera& editorCamera);

    // Raycasts from the mouse through the viewport (existing box colliders first, then the
    // Y=0 ground plane, then a fixed fallback distance if neither hits) and applies grid
    // snapping (XZ only — Y comes from the hit itself) — the "where does the cursor point"
    // half of a viewport drop, shared by the model and prefab placement paths below.
    glm::vec3 ComputeDropRayPosition(World& world, Camera& editorCamera) const;
    // ComputeDropRayPosition(), then rests `model`'s own lowest vertex on that point instead of
    // its (possibly arbitrary) pivot — same technique SnapSelectionToGround() already uses for
    // an already-placed object, just evaluated for one that isn't placed yet. This is what the
    // live drag preview and the on-release commit BOTH call, so what you see while dragging is
    // exactly what you get.
    glm::vec3 ComputeModelDropPosition(World& world, Model& model, Camera& editorCamera) const;
    DragPreview m_DragPreview;

    // Axis-aligned/isometric view presets (Front/Back/Left/Right/Top/Bottom/Iso) and the
    // orthographic projection they use — the "look at things in iso mode" feature. Numpad
    // shortcuts and a View menu both funnel through SnapToView()/ToggleOrthographic() below.
    struct ViewTransition {
        bool Active = false;
        float T = 0.0f;
        glm::vec3 FromPos{0.0f}, ToPos{0.0f};
        float FromYaw = 0.0f, ToYaw = 0.0f, FromPitch = 0.0f, ToPitch = 0.0f;
        float FromOrthoHalfHeight = 0.0f, ToOrthoHalfHeight = 0.0f;
        float FromFov = 60.0f, ToFov = 60.0f; // eased too, for "look through light" (#140 phase 4)
    };
    ViewTransition m_ViewTransition;

    // Look through a light (#140 phase 4): move the editor camera to the light's POV — spot uses
    // 2x its cone angle for FOV, point ~90, directional goes orthographic. Esc restores the
    // camera that was stashed on entry. State is transient, never serialized.
    bool m_LookThroughActive = false;
    entt::entity m_LookThroughLight = entt::null;
    struct CameraPose {
        glm::vec3 Pos{0.0f};
        float Yaw = 0.0f, Pitch = 0.0f, Fov = 60.0f, OrthoHalfHeight = 0.0f;
        bool Ortho = false;
    };
    CameraPose m_LookThroughRestore;
    // While looking through a light, flying the editor camera drives the light with it (fly it
    // into place / re-aim it from its own POV). One undo point is captured the first frame it
    // actually moves; these hold the light's pose at entry so that first move is detectable.
    bool m_LookThroughMoved = false;
    glm::vec3 m_LookThroughStartPos{0.0f};
    glm::vec3 m_LookThroughStartRot{0.0f};
    // The Inspector has no camera in scope, so its "Look through" button just records the light
    // here; Draw() acts on it once, with editorCamera available.
    entt::entity m_PendingLookThrough = entt::null;
    void LookThroughLight(World& world, Camera& editorCamera, entt::entity light);
    void ExitLookThrough(Camera& editorCamera);
    // Per-frame: consume Esc, drop out if the light is gone, draw the "Looking through…" banner.
    void UpdateLookThrough(World& world, Camera& editorCamera);
    // Drop `light` straight down onto the nearest mesh AABB below it (+ a small lift). Returns
    // false (and does nothing) if there's no geometry under it. One PushUndo.
    bool DropLightToSurface(World& world, entt::entity light);

    // What to orbit/frame around for a view snap: the current selection's bounds center if
    // there is one, else whatever the camera is currently looking straight at (a raycast into
    // the world), else the whole scene's bounds centre, else a fixed distance in front of the
    // camera. `outFrameRadius` is >0 only for the whole-scene fallback — SnapToView uses it to
    // pull the camera to a distance that actually fits the scene in view (rather than keeping
    // whatever distance it had, which is how a preset used to leave you staring at black).
    void ComputeViewPivot(World& world, Camera& editorCamera, glm::vec3& outPivot,
                          float& outFrameRadius) const;
    // Smoothly (see UpdateViewTransition) reorients the camera to `yaw`/`pitch`, switches
    // projection mode, and repositions it to keep the same pivot centered and the same apparent
    // scale (the ortho<->perspective size conversion uses Fov, so switching mid-view doesn't jump).
    void SnapToView(World& world, Camera& editorCamera, float yaw, float pitch, bool orthographic);
    void ToggleOrthographic(World& world, Camera& editorCamera);
    // Advances the in-progress view transition (if any) by `dt`, easing yaw/pitch (shortest
    // angular path), position, and ortho size toward their targets. Any manual camera input
    // this frame (WASD, look-drag, orbit, pan, scroll) cancels the transition in place instead
    // of fighting it to completion — the same feel as interrupting the nav gizmo's axis-snap
    // animation by moving the mouse.
    void UpdateViewTransition(Camera& editorCamera, float dt);
};
