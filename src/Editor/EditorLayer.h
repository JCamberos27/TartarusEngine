#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <set>
#include <memory>

struct GLFWwindow;
class World;
class Camera;
class AssetLibrary;
class Texture;

enum class GizmoOp { Translate, Rotate, Scale };

// In-game editor overlay (Dear ImGui + ImGuizmo): import assets, place/inspect
// entities, manipulate them with viewport gizmos. Toggle with F1; gameplay pauses
// while the editor is open.
class EditorLayer {
public:
    // Declared (rather than left implicit) and defined in the .cpp: m_LogoTexture is a
    // unique_ptr<Texture>, and Texture is only forward-declared here, so the implicit
    // destructor can't be instantiated wherever an EditorLayer is destroyed (e.g. main.cpp,
    // which never includes Texture.h).
    EditorLayer();
    ~EditorLayer();

    void Init(GLFWwindow* window);
    void Shutdown();

    void BeginFrame();
    void Draw(World& world, AssetLibrary& assets, Camera& editorCamera, float dt);
    void EndFrame();

    // A small floating Play/Stop button centered above the viewport — drawn every frame in
    // BOTH modes (unlike Draw(), which only runs in editor mode), so there's always a way
    // back to the editor without reaching for F1. `editorMode` picks which icon/label shows;
    // a click just raises the request flag, since main.cpp (not EditorLayer) owns the actual
    // editorMode/camera-handoff/cursor-lock switch.
    void DrawPlayStopButton(bool editorMode);
    bool ConsumePlayStopRequest() {
        bool requested = m_PlayStopRequested;
        m_PlayStopRequested = false;
        return requested;
    }

    bool WantsCaptureMouse() const;
    bool WantsCaptureKeyboard() const;

    // True while the pointer is hovering or actively dragging a gizmo handle. The editor
    // camera should ignore look input in that case so it doesn't fight the gizmo drag.
    bool GizmoEngaged() const { return m_GizmoEngaged; }

    // Grid settings, read by main.cpp to draw the actual 3D grid (an OpenGL draw call
    // outside ImGui's frame, so EditorLayer only owns the settings, not the rendering).
    bool ShowGrid() const { return m_ShowGrid; }
    float GridMinorSpacing() const { return m_GridSize; }

    // The viewport's actual on-screen rect this frame — the dockspace's central node, i.e.
    // whatever's left after the Hierarchy/Inspector/Asset Browser panels take their share.
    // Recomputed every frame in Draw() (editor mode only), so it tracks live panel resizes.
    // main.cpp reads this to size the 3D render (glViewport + camera aspect) to match instead
    // of always rendering to the full window underneath the docked panels.
    glm::vec2 ViewportPos() const { return m_ViewportPos; }
    glm::vec2 ViewportSize() const { return m_ViewportSize; }

    // The scene file currently open — starts at "scene.json" to match the historical
    // auto-save/auto-load default; changes when the user uses Open/Save As. main.cpp reads
    // this for the auto-save-on-exit so it writes back to wherever the user actually has open.
    const std::string& CurrentScenePath() const { return m_CurrentScenePath; }
    // True once any edit has happened since the last Save/Save As/Open/New — main.cpp reads
    // this to show an unsaved-changes indicator in the window title.
    bool IsDirty() const { return m_Dirty; }

    // The full selection (primary + any Ctrl+Click extras) as (isModel, index) pairs, for
    // main.cpp's outline-render pass — empty when nothing is selected or outside editor mode.
    std::vector<std::pair<bool, int>> GetSelectedItems() const {
        std::vector<std::pair<bool, int>> result;
        if (m_SelectedModel >= 0) result.push_back({true, m_SelectedModel});
        else if (m_SelectedBox >= 0) result.push_back({false, m_SelectedBox});
        for (const auto& item : m_ExtraSelection) result.push_back({item.IsModel, item.Index});
        return result;
    }

private:
    GLFWwindow* m_Window = nullptr;
    bool m_PlayStopRequested = false;
    // Set by Settings > Reset Layout; consumed at the top of Draw()'s dockspace setup to
    // rebuild the default panel arrangement from scratch.
    bool m_ResetLayoutRequested = false;

    // Defaults to a plausible full-window size so nothing divides by zero if anything reads
    // these before the first Draw() has run; overwritten every editor-mode frame after that.
    glm::vec2 m_ViewportPos{0.0f, 0.0f};
    glm::vec2 m_ViewportSize{1280.0f, 720.0f};

    int m_SelectedBox = -1;   // primary selection: index into World::Boxes, or -1
    int m_SelectedModel = -1; // primary selection: index into World::Models, or -1

    // Additional objects co-selected with the primary via Ctrl+Click, for group operations
    // (move/rotate/scale several objects together with the gizmo, delete them all at once).
    // The Inspector still edits only the primary's fields — syncing N objects' individual
    // material/animation state is a much bigger feature this doesn't attempt.
    struct SelectedItem {
        bool IsModel;
        int Index;
    };
    std::vector<SelectedItem> m_ExtraSelection;
    glm::mat4 m_GroupGizmoMatrix{1.0f}; // pivot frame for the multi-select gizmo, updated across a drag

    bool HasGroupSelection() const { return !m_ExtraSelection.empty(); }
    bool HasAnySelection() const { return m_SelectedBox >= 0 || m_SelectedModel >= 0; }
    bool IsSelected(bool isModel, int index) const {
        if (isModel ? (m_SelectedModel == index) : (m_SelectedBox == index)) return true;
        for (const auto& item : m_ExtraSelection) {
            if (item.IsModel == isModel && item.Index == index) return true;
        }
        return false;
    }
    // Replaces the primary selection (plain click) or toggles membership in the group
    // (Ctrl+Click) — the one place both the Hierarchy and viewport picking route through so
    // they stay consistent.
    void SelectItem(bool isModel, int index, bool addToSelection);
    void ClearSelection();
    void DeleteSelection(World& world);
    void DuplicateSelection(World& world, AssetLibrary& assets);
    void DrawGroupGizmo(World& world, Camera& editorCamera);
    // Repositions the camera along its current view direction so the whole selection (single
    // or group) fits in frame, without changing where it's looking (matches most editors'
    // basic "frame selection" behavior — it centers distance, not aim).
    void FocusOnSelection(World& world, Camera& editorCamera);

    std::string m_CurrentScenePath = "scene.json";
    bool m_Dirty = false;

    GizmoOp m_GizmoOp = GizmoOp::Translate;
    bool m_GizmoLocalSpace = false; // false = world-aligned handles, true = aligned to the object's own rotation
    float m_GizmoSize = 0.1f;       // ImGuizmo's clip-space size units; 0.1 is its own built-in default
    bool m_GizmoEngaged = false;
    bool m_GizmoWasUsing = false;
    bool m_PrevLeftMouseDown = false;

    // Box/marquee select: press-drag-release in empty viewport space. Whether it turns out to
    // be a plain click (single-object pick, existing behavior) or a real drag (select every
    // object whose screen-space bounds fall in the rectangle) is only decided on release.
    bool m_BoxSelectActive = false;
    glm::vec2 m_BoxSelectStart{0.0f, 0.0f};
    void AddToSelectionIfAbsent(bool isModel, int index); // additive-only: never toggles an already-selected item off
    bool m_LayoutLocked = true;

    bool m_ShowGrid = true;
    float m_GridSize = 1.0f;          // minor grid line spacing, world units
    bool m_GridSnapEnabled = true;    // hold Ctrl to invert momentarily, Blender-style
    float m_SnapTranslation = 1.0f;
    float m_SnapRotationDeg = 15.0f;
    float m_SnapScale = 0.1f;
    float m_VertexSnapRadius = 1.25f; // generous catch radius by default — easy to snap, narrow it in the toolbar if it's too eager
    float m_VertexPickPixels = 35.0f; // how close (screen pixels) the cursor must be to a vertex to grab/preview it

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

    // Undo/redo: whole-scene JSON snapshots (via SceneSerializer), pushed at the start of
    // a discrete edit (gizmo drag, field drag, import, delete) rather than every frame.
    std::vector<std::string> m_UndoStack;
    std::vector<std::string> m_RedoStack;
    static constexpr size_t kMaxHistory = 100;

    void PushUndo(const World& world);
    void Undo(World& world, AssetLibrary& assets);
    void Redo(World& world, AssetLibrary& assets);

    // Studio watermark, drawn small and translucent in a corner of the viewport — loaded once
    // in Init() from assets/branding/ (a build-time copy of extern/branding/, same treatment
    // as the icon font). Null and silently skipped if the file's missing.
    std::unique_ptr<Texture> m_LogoTexture;

    void ApplyPanelPlacement(glm::vec2 pos, glm::vec2 size) const;
    // Full-width strip above the viewport: everything lives here now — one-click toggles in
    // the row below the menu bar, and File/Import/Add/Settings as dropdown menus (Settings
    // holds what used to be a separate always-open side panel: environment, grid/snap tuning,
    // vertex snap tuning, controls reference).
    void DrawTopToolbar(World& world, AssetLibrary& assets, Camera& editorCamera);
    void DrawHierarchy(World& world, AssetLibrary& assets);
    void DrawInspector(World& world, AssetLibrary& assets, float dt);
    void DrawAssetBrowser(World& world, AssetLibrary& assets);
    std::string m_AssetSearchFilter;

    // Loads `path` as the active scene, resetting selection/undo state the same way the
    // File > Open... dialog does — shared so the Asset Browser's Scenes folder can open a
    // scene with one double-click instead of going through the OS file dialog.
    void OpenScene(World& world, AssetLibrary& assets, const std::string& path);

    // Unity Project-window style browsing: the current virtual folder ("" = root), the
    // browser-local selection (separate from the scene selection — this is for F2/right-click
    // actions on library assets, not placed objects), and inline-rename state shared by both
    // folders and assets (they use the same InputText-in-place mechanic).
    std::string m_CurrentAssetFolder;
    std::string m_SelectedAssetKey;
    bool m_SelectedAssetIsFolder = false;
    std::string m_RenamingAssetKey; // empty = not renaming anything right now
    bool m_RenamingIsFolder = false;
    bool m_RenamingJustStarted = false; // one-shot: focus + select-all the rename field on the frame it opens
    char m_RenameBuffer[128] = {};
    void BeginRenameAsset(const std::string& key, bool isFolder, const std::string& currentName);
    void CommitRename(AssetLibrary& assets);

    void DrawMaterialEditor(World& world, AssetLibrary& assets);
    void DrawGizmo(World& world, Camera& editorCamera);
    void HandleViewportPicking(World& world, Camera& editorCamera);

    // Lets a Model entry from the Asset Browser be dropped into the 3D view to place a new
    // instance, active only while a drag from the Asset Browser is in progress so it never
    // otherwise sits on top of the viewport intercepting camera input.
    void DrawViewportDropTarget(World& world, AssetLibrary& assets, Camera& editorCamera);
};
