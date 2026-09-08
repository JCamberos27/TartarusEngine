#pragma once

#include <cstdint>
#include <cstddef>

// The editor host owns ImGui, the renderer, scene data, undo history, and window lifetime.
// Reloadable editor modules describe their UI through this deliberately small callback surface.
// That keeps an in-flight scene safe when a DLL is replaced.
//
// Version history:
//   1 - status-panel placeholder only.
//   2 - ImGui context/allocator sharing, Log accessors, tooltip + save-dialog callbacks and the
//       host-owned Console panel state, so the Console panel itself can live in the module.
//   3 - Stats-panel accessors: viewport rect + UI scale, RenderStats / Profiler / GL-frame-stats
//       / entity-count readouts, the smoothed frame time, the engine-mark hide write-back, and a
//       viewport-luminance sample so the module can own the HUD's contrast-adaptive text tint.
//   4 - Toolbar / menus: the top strip + its dropdown menus + the window min/max/close controls.
//       Window metrics + theme + drag-region write-back, GLFW window-control callbacks, typed
//       getters/setters for the icon-row toggles (gizmo op/space/pivot, grid, snap, shading,
//       history, stats, light gizmos, ortho), undo/redo + snap-to-ground commands, and five
//       Draw*Body callbacks that render the host-owned menu/popup contents into the module's
//       menus (scene load/save, entity creation, camera framing — all still host-side).
//   5 - Asset Browser (thin slice): the dock window, the +Create/Import toolbar row, breadcrumb,
//       search + Filters popup, the left folder-tree pane and the tree/grid splitter move into
//       the module. Panel-state getters/setters (visibility, search text, label-menu filter,
//       current folder, tree width), a frame snapshot of the folder list + known labels,
//       undoable folder commands (create / rename / move-asset-into), import-via-dialog, and one
//       DrawAssetGridBody callback that keeps the asset grid + thumbnails + context menus +
//       delete/duplicate host-side.
//   6 - Asset grid layout slice: the module now owns the ##AssetList child, the ImGuiListClipper
//       row loop + per-row SameLine wrapping, and the footer child. DrawAssetGridBody is replaced
//       by AssetGridFrameBegin / AssetGridCellCount / GetAssetGridMetrics / DrawAssetCell /
//       HandleAssetGridBackground / GetAssetSelectionSummary / Get+SetAssetIconSize /
//       AssetGridFrameEnd. Per-cell draw (thumbnails, the five context menus, rename, drag,
//       multi-select) stays host-side inside DrawAssetCell.
//   7 - Scene Hierarchy (thin slice): the module owns the panel frame — Begin("Scene Hierarchy"),
//       the search box, the Expand-all / Collapse-all buttons. Get/SetShowHierarchy,
//       Get/SetHierarchyFilter, HierarchyExpandAll, and one DrawHierarchyTreeBody callback that
//       keeps the whole EnTT entity tree (rows, drag-reparent, context menus, selection, undo,
//       Ctrl+A) host-side.
//   8 - Inspector (frame only): the module owns Begin("Inspector") + End + visibility.
//       Get/SetShowInspector and one DrawInspectorBody callback — the ~1090-line body (every
//       component editor, PBR material, add-component, per-field undo) stays host-side.
//   9-11 - Grid & Snap popover, Gizmos dropdown, Gizmos master toggle (see the tagged sections).
//   12 - Viewport tools (#236 E): Hand tool (Q) + Lock View to Selected (Shift+F) getters/setters,
//        and GizmoOp gains a 5th value (4 = Universal / combined transform, via Get/SetGizmoOp).
//   13 - Asset Browser sort + refresh (#236 G): Get/SetAssetSort (packed mode*2+desc),
//        RefreshAssetBrowser, GetAssetRefreshFlash. Also Console (#236 A5): EditorConsoleState
//        gains Collapse / ClearOnPlay / ErrorPause.
//   14 - Measure / ruler tool + Duplicate Array (#236 R2): Get/SetMeasureTool,
//        RequestDuplicateArray for the toolbar buttons. Plus Get/ToggleInspectorLock so the
//        Inspector's padlock can move into its title bar.
constexpr std::uint32_t kEditorModuleAPIVersion = 14;

// ImGui's own allocator signatures, spelled out here so this header stays free of <imgui.h>
// (the host and the module each compile their own ImGui translation units; only the context and
// the allocator pair need to be shared across the boundary).
using EditorModuleImGuiAllocFn = void* (*)(std::size_t size, void* userData);
using EditorModuleImGuiFreeFn  = void  (*)(void* ptr, void* userData);

// Mirrors LogLevel from Core/Log.h as a plain int-valued enum so the module never has to include
// (or link against) the engine's logging translation unit — see EditorModuleHostAPI::LogGetEntry.
enum EditorModuleLogLevel : int {
    EditorModuleLogLevel_Info    = 0,
    EditorModuleLogLevel_Warning = 1,
    EditorModuleLogLevel_Error   = 2,
};

// Console UI state. Deliberately POD with a fixed-size filter buffer: it lives in the HOST so it
// survives a module reload (rebuilding TartarusEditor.dll mustn't clear the user's filter text,
// level toggles or panel visibility), and the host's own toolbar needs the visibility flag for
// its "Toggle Console" button. Anything derived from this (e.g. the cached filtered index list)
// stays module-side and is simply rebuilt after a reload.
struct EditorConsoleState {
    bool Visible = false; // hidden on launch; toggled from the toolbar. Not persisted.
    bool ShowInfo = true;
    bool ShowWarning = true;
    bool ShowError = true;
    bool AutoScroll = true;
    bool ShowTimestamps = true;
    // #236 A5. Collapse: every identical message shows once with a summed (xN), Unity-style,
    // not just consecutive duplicates. ClearOnPlay: the host wipes the log on entering Play.
    // ErrorPause: the host freezes the running sim the frame a new error is logged.
    bool Collapse = false;
    bool ClearOnPlay = false;
    bool ErrorPause = false;
    // Matches the 128-byte buffer the panel's InputTextWithHint has always used.
    char Filter[128] = {};
    // Only auto-scroll when Log actually gained an entry, rather than fighting the user's
    // scrollback every frame.
    unsigned int SeenRevision = 0;
};

// --- Stats panel data (API v3) ---------------------------------------------------------------
// Plain PODs mirroring the host-side structs the Stats HUD reads (EditorLayer::RenderStats,
// Profiler::Entry, GLStateCache::FrameStats) so none of those host headers cross the boundary.
struct EditorModuleRenderStats {
    int DrawCalls = 0;
    int Triangles = 0;
    int Vertices = 0;
    int Culled = 0;
    bool LightBufferOverflowed = false;
    bool ClusterSaturated = false;
};

struct EditorModuleProfilerSample {
    char Name[32] = {};
    float Milliseconds = 0.0f;
};

struct EditorModuleGLFrameStats {
    int ProgramBinds = 0, ProgramBindsSkipped = 0;
    int TextureBinds = 0, TextureBindsSkipped = 0;
    int VaoBinds = 0, VaoBindsSkipped = 0;
};

struct EditorModuleHostAPI {
    std::uint32_t Version = kEditorModuleAPIVersion;
    void (*DrawStatusPanel)(const char* title, const char* message, const char* accent) = nullptr;

    // --- ImGui sharing ---------------------------------------------------------------------
    // The module compiles its own copy of ImGui's sources, so it must be pointed at the host's
    // single ImGuiContext (all ImGui state lives there) and the host's allocator pair before it
    // makes any ImGui call. This is ImGui's documented multi-module setup.
    void* (*GetImGuiContext)() = nullptr;
    void (*GetImGuiAllocators)(EditorModuleImGuiAllocFn* outAlloc,
                               EditorModuleImGuiFreeFn* outFree,
                               void** outUserData) = nullptr;

    // --- Log ------------------------------------------------------------------------------
    // Log's storage is a function-local static inside the host's Log.cpp. Compiling Log.cpp into
    // the module as well would give the module its own private, permanently empty log, so the
    // module never links it and reads everything through these host-side accessors instead.
    unsigned int (*LogRevision)() = nullptr;
    int (*LogEntryCount)() = nullptr;
    // Fills the out-params for one entry; the returned strings point at the host's own storage
    // and stay valid until the log next changes (i.e. for the duration of the caller's use
    // within a frame). Returns false for an out-of-range index.
    bool (*LogGetEntry)(int index, int* outLevel, const char** outMessage,
                        const char** outTime, int* outCount) = nullptr;
    int (*LogCountOf)(int level) = nullptr;
    void (*LogClear)() = nullptr;
    void (*LogInfo)(const char* message) = nullptr;
    void (*LogError)(const char* message) = nullptr;

    // --- Editor services --------------------------------------------------------------------
    // Routed through the host so the module doesn't duplicate EditorSettings (another singleton)
    // or need the GLFW window handle the native file dialog is parented to.
    void (*SetTooltip)(const char* text) = nullptr;
    // Writes the chosen path into `outPath`; returns false if the user cancelled (or the buffer
    // was too small). Plain char buffer rather than a std::string return: no STL across the ABI.
    bool (*SaveFileDialog)(const char* filter, const char* defaultExt,
                           char* outPath, int outPathSize) = nullptr;

    // Host-owned, reload-surviving Console panel state. Never null when Version matches.
    EditorConsoleState* (*ConsoleState)() = nullptr;

    // --- Stats panel (API v3) ------------------------------------------------------------------
    // The Stats HUD is a transparent, click-through overlay the module owns (its own ImGui::Begin
    // with NoInputs/NoBackground), pinned to the Scene viewport. Everything it displays is
    // host-owned state read through the callbacks below; the module keeps only derived data and
    // the eased contrast tint, both of which simply rebuild after a reload.

    // Scene-viewport rect (screen space) and the editor UI scale the HUD lays itself out against.
    // *outEnabled is false when the Stats overlay is toggled off (View > Statistics) or the Scene
    // viewport isn't showing / is degenerate — the module then draws nothing and clears the
    // engine-mark hide flag.
    void (*GetViewportRect)(float* outX, float* outY, float* outW, float* outH,
                            float* outUIScale, bool* outEnabled) = nullptr;

    // One-frame-behind render stats for the frame that just finished.
    void (*GetRenderStats)(EditorModuleRenderStats* out) = nullptr;

    // Fills outArr with up to maxCount profiler samples from the most recently completed frame
    // (gpu=false: CPU scopes; gpu=true: GPU timer queries, which trail a few frames further).
    // Returns the number of samples written.
    int (*GetProfilerSamples)(EditorModuleProfilerSample* outArr, int maxCount, bool gpu) = nullptr;

    // GLStateCache bind counters for the frame that just finished.
    void (*GetGLFrameStats)(EditorModuleGLFrameStats* out) = nullptr;

    // Live tallies over the host's World (computed host-side; the module never sees EnTT).
    void (*GetSceneEntityCounts)(int* outEntities, int* outRenderers, int* outColliders,
                                 int* outLights, int* outInactive) = nullptr;

    // Exponentially-smoothed frame time (ms). The host keeps updating this every frame even while
    // the panel is hidden — the viewport status bar reads it too — so the module only reads.
    float (*GetSmoothedFrameMs)() = nullptr;

    // The module's one write-back: true while the (height-capped) Stats HUD reaches far enough
    // down the viewport to collide with the corner engine-mark monogram, so the host hides it.
    // Called once per module Draw (false on the early-out paths).
    void (*SetHideEngineMark)(bool hide) = nullptr;

    // Kicks the host's async PBO luminance readback under the given screen-space box (the host
    // owns the Scene framebuffer + GL context) and returns the most-recently-completed average
    // luminance there, 0..1, or -1 if no sample has landed yet. The module throttles calls to
    // ~10 Hz and does its own easing + text tint.
    float (*SampleViewportLuminance)(float screenCenterX, float screenCenterY, float boxPx) = nullptr;

    // --- Toolbar / menus (API v4) ------------------------------------------------------------
    // The top toolbar strip, its dropdown menus and the window min/max/close controls live in
    // the module now (EditorModuleToolbar.cpp). The module owns the pinned "##Toolbar" window,
    // the Windows-XP chrome, the icon-button row layout and the menu/popup scaffolding; the deep
    // host logic behind the menus (scene load/save, entity creation, camera framing, undo) stays
    // host-side and is rendered into the module's menus through the Draw*Body callbacks below.

    // Window metrics the strip pins itself against, forced every frame: main-window pixel width,
    // toolbar height (host-owned kToolbarHeight * UI scale) and the editor UI scale.
    void (*GetToolbarMetrics)(float* outWinW, float* outToolbarH, float* outUIScale) = nullptr;
    // EditorSettings::EditorTheme — 2 is Windows XP, where the strip paints the blue Luna chrome.
    int (*GetEditorTheme)() = nullptr;
    // The toolbar's empty area is the window drag handle (the OS caption is gone). Reported back
    // every frame; the host forwards it to Window's WM_NCHITTEST.
    void (*SetTitleBarDragHovered)(bool hovered) = nullptr;
    // Custom window controls operate on the host's GLFW window (the module never sees the handle).
    void (*WindowMinimize)() = nullptr;
    void (*WindowToggleMaximize)() = nullptr;
    void (*WindowClose)() = nullptr;
    bool (*WindowIsMaximized)() = nullptr;

    // Icon-row state. Enums cross as int — GizmoOp: 0..3 Translate/Rotate/Scale/Rect;
    // ShadingMode: 0..2 Shaded/Wireframe/Unlit.
    int  (*GetGizmoOp)() = nullptr;          void (*SetGizmoOp)(int op) = nullptr;
    int  (*GetShadingMode)() = nullptr;      void (*SetShadingMode)(int mode) = nullptr;
    bool (*GetGizmoLocalSpace)() = nullptr;  void (*SetGizmoLocalSpace)(bool on) = nullptr;
    bool (*GetGizmoPivotCenter)() = nullptr; void (*SetGizmoPivotCenter)(bool on) = nullptr;
    bool (*GetShowGrid)() = nullptr;         void (*SetShowGrid)(bool on) = nullptr;
    bool (*GetGridSnapEnabled)() = nullptr;  void (*SetGridSnapEnabled)(bool on) = nullptr;
    bool (*GetShowHistory)() = nullptr;      void (*SetShowHistory)(bool on) = nullptr;
    // These two persist into EditorSettings; the setter calls EditorSettings::Save() host-side.
    bool (*GetShowStats)() = nullptr;        void (*SetShowStats)(bool on) = nullptr;
    bool (*GetShowLightGizmos)() = nullptr;  void (*SetShowLightGizmos)(bool on) = nullptr;
    bool (*IsOrthographic)() = nullptr;      void (*ToggleOrthographic)() = nullptr;
    void (*ToolbarUndo)() = nullptr;
    void (*ToolbarRedo)() = nullptr;
    bool (*CanSnapSelectionToGround)() = nullptr;
    void (*SnapSelectionToGround)() = nullptr;
    void (*RequestResetLayout)() = nullptr;
    void (*OpenPreferences)() = nullptr;

    // Menu / popup bodies rendered host-side into the module-begun menu or popup — the module
    // calls these between its own BeginMenu/EndMenu (or BeginPopup/EndPopup). The single shared
    // ImGuiContext means host-side ImGui calls in here land in the module's menu exactly as if
    // the module had made them. DrawAddEntityMenuItems wraps EditorLayer::DrawAddEntityItems,
    // which the not-yet-migrated Hierarchy panel also calls — one implementation, host-side.
    void (*DrawFileMenuBody)() = nullptr;
    void (*DrawAddEntityMenuItems)() = nullptr;
    void (*DrawViewMenuBody)() = nullptr;
    void (*DrawWindowMenuBody)() = nullptr;
    void (*DrawCaptureOptionsPopupBody)() = nullptr;
    // Fire a screenshot with the current EditorSettings capture options (the Print Screen path
    // shares this); kCaptureRes and EditorLayer::RequestCapture stay host-side.
    void (*RequestCapture)() = nullptr;
    // Fills `out` with the capture button's tooltip ("Capture screenshot — <mode><x2+?> (Print
    // Screen)"), built host-side from EditorSettings so the module needn't read those fields.
    void (*GetCaptureButtonTooltip)(char* out, int outSize) = nullptr;

    // --- Asset Browser, thin slice (API v5) -------------------------------------------------
    // The dock window, the +Create/Import toolbar row, the breadcrumb, the search box + Filters
    // popup, the left folder-tree pane and the tree/grid splitter live in the module now
    // (EditorModuleAssetBrowser.cpp). The asset grid itself — tiles, thumbnails (GL), context
    // menus, rename-in-place, drag sources, delete/duplicate, the scenes/screenshots virtual
    // folders — stays host-side and is rendered into the module's window through
    // DrawAssetGridBody. AssetLibrary (STL-heavy, traffics shared_ptr<Model>) never crosses the
    // boundary: folders come back as a per-frame string snapshot, and every mutation is an
    // undoable command callback.

    // Panel state (host-owned so it survives a reload; the module's tree-expansion set does not).
    bool (*GetShowAssetBrowser)() = nullptr;   void (*SetShowAssetBrowser)(bool on) = nullptr;
    void (*GetAssetSearch)(char* out, int n) = nullptr;         void (*SetAssetSearch)(const char* s) = nullptr;
    void (*GetAssetLabelMenuFilter)(char* out, int n) = nullptr; void (*SetAssetLabelMenuFilter)(const char* s) = nullptr;
    void (*GetCurrentAssetFolder)(char* out, int n) = nullptr;
    // Also clears the asset selection and points it at the folder, matching the old tree-click.
    void (*SetCurrentAssetFolder)(const char* folder) = nullptr;
    float (*GetAssetTreeWidth)() = nullptr;
    // `commit` true on drag-end: persist to EditorSettings + Save(); false while dragging.
    void (*SetAssetTreeWidth)(float px, bool commit) = nullptr;
    // One-shot: true (once) after the host's Ctrl+F shortcut asked to focus the search box.
    bool (*ConsumeAssetSearchFocus)() = nullptr;
    // The module reports its window focus each frame; the host uses it for the scenes/screenshots
    // listing refresh and Ctrl+A-select-all.
    void (*SetAssetBrowserFocused)(bool focused) = nullptr;

    // Folder-tree expansion — kept host-side so the Left/Right-arrow tree shortcuts keep working
    // and so it survives a reload. `recursive` matches the Alt+click "and everything under it".
    bool (*IsAssetFolderExpanded)(const char* path) = nullptr;
    void (*SetAssetFolderExpanded)(const char* path, bool expand, bool recursive) = nullptr;
    // Per-frame setup the host does before the module draws the tree: ensure the "Scenes"/
    // "Screenshots" virtual folders exist, refresh the on-disk listing caches, and expand the
    // ancestors of the current folder when it changed from elsewhere. Fills `outReveal` with the
    // folder that should scroll itself into view this frame (empty = none) — the old
    // m_RevealAssetFolderInTree one-shot.
    void (*AssetTreeFrameSetup)(char* outReveal, int n) = nullptr;

    // Folder list + labels — a snapshot valid for the current frame.
    int  (*GetFolderCount)() = nullptr;
    bool (*GetFolder)(int index, char* out, int n) = nullptr;
    int  (*GetKnownLabelCount)() = nullptr;
    bool (*GetKnownLabel)(int index, char* out, int n) = nullptr;

    // Undoable AssetLibrary commands (host does PushUndo + the mutation).
    void (*CreateFolderUndoable)(const char* path) = nullptr;
    void (*RenameFolderUndoable)(const char* oldPath, const char* newPath) = nullptr;
    void (*MoveAssetToFolderUndoable)(const char* assetKey, const char* folder) = nullptr;
    // Start the in-place rename editor on a folder (host BeginRenameAsset(path, isFolder=true)).
    void (*BeginRenameFolder)(const char* path) = nullptr;
    // kind 0/1/2 = model / texture / sound; host opens FileDialog then ImportDroppedFile into
    // `intoFolder` (the current folder).
    void (*ImportAssetViaDialog)(int kind, const char* intoFolder) = nullptr;

    // --- Asset grid layout slice (API v6) -------------------------------------------------
    // The module owns the ##AssetList child, the clipper row loop + per-row SameLine wrapping,
    // and the footer child. Per-frame order: BeginChild -> AssetGridFrameBegin -> for each
    // visible cell DrawAssetCell(i, w, h, gridMode) -> HandleAssetGridBackground -> EndChild ->
    // footer (GetAssetSelectionSummary + the icon-size slider via Get/SetAssetIconSize) ->
    // AssetGridFrameEnd. Cell width/height and cellsPerRow are pure math the module does from
    // GetAssetGridMetrics; DrawAssetCell just fills the rect it is handed.
    void  (*AssetGridFrameBegin)() = nullptr;
    int   (*AssetGridCellCount)() = nullptr;
    void  (*GetAssetGridMetrics)(float* outIconSize, float* outUIScale, float* outListMinIcon) = nullptr;
    void  (*DrawAssetCell)(int index, float cellW, float cellH, bool gridMode) = nullptr;
    void  (*HandleAssetGridBackground)() = nullptr;
    void  (*GetAssetSelectionSummary)(char* out, int n) = nullptr;
    float (*GetAssetIconSize)() = nullptr;
    void  (*SetAssetIconSize)(float px, bool commit) = nullptr;
    void  (*AssetGridFrameEnd)() = nullptr;

    // --- Scene Hierarchy, thin slice (API v7) ---------------------------------------------
    // The module owns Begin("Scene Hierarchy"), the search box and the Expand/Collapse-all
    // buttons. The whole entity tree — rows, drag-reparent, the row + empty-space context menus,
    // click/shift/ctrl selection, delta undo, Ctrl+A — stays host-side inside DrawHierarchyTreeBody
    // (EnTT + Components.h never cross the boundary).
    bool (*GetShowHierarchy)() = nullptr;      void (*SetShowHierarchy)(bool on) = nullptr;
    void (*GetHierarchyFilter)(char* out, int n) = nullptr;
    void (*SetHierarchyFilter)(const char* s) = nullptr;
    void (*HierarchyExpandAll)(bool open) = nullptr;
    void (*DrawHierarchyTreeBody)() = nullptr;

    // --- Inspector, frame only (API v8) -------------------------------------------------
    // The module owns Begin("Inspector") + End + visibility; the ~1090-line body (every
    // component editor, PBR material, add-component, per-field undo — EnTT + material shared_ptr
    // throughout) stays host-side inside DrawInspectorBody.
    bool (*GetShowInspector)() = nullptr;      void (*SetShowInspector)(bool on) = nullptr;
    void (*DrawInspectorBody)() = nullptr;

    // --- Grid & Snap popover (API v9) ---------------------------------------------------
    // The toolbar's magnet button gets a caret that opens this popup; the body (grid spacing,
    // per-op snap increments — plain floats/ints in EditorSettings + EditorLayer) is host-side.
    void (*DrawGridSnapPopupBody)() = nullptr;

    // --- Gizmos dropdown (API v10) ----------------------------------------------------
    // Master viewport-gizmo switch + per-type visibility; body is host-side (EditorLayer flags
    // + EditorSettings::ShowLightGizmos).
    void (*DrawGizmosPopupBody)() = nullptr;

    // --- Gizmos master toggle button (API v11) --------------------------------------
    bool (*GetGizmosMasterVisible)() = nullptr;  void (*SetGizmosMasterVisible)(bool on) = nullptr;

    // --- Viewport tools (API v12) --------------------------------------------------
    bool (*GetHandTool)() = nullptr;             void (*SetHandTool)(bool on) = nullptr;
    bool (*GetLockViewToSelection)() = nullptr;  void (*SetLockViewToSelection)(bool on) = nullptr;

    // --- Asset Browser sort + refresh (API v13) ---------------------------------
    int  (*GetAssetSort)() = nullptr;   void (*SetAssetSort)(int packed) = nullptr;
    void (*RefreshAssetBrowser)() = nullptr;
    float (*GetAssetRefreshFlash)() = nullptr;

    // --- Measure / ruler tool + Duplicate Array (API v14, #236 R2) ------------------
    bool (*GetMeasureTool)() = nullptr;  void (*SetMeasureTool)(bool on) = nullptr;
    void (*RequestDuplicateArray)() = nullptr; // opens the array-duplicate modal (host self-gates on a selection)

    // --- Inspector lock in the title bar (API v14, #236 R2 Inspector tail) ----------
    bool (*GetInspectorLocked)() = nullptr;
    void (*ToggleInspectorLock)() = nullptr; // host does the selection-snapshot capture

    // --- Asset Browser search scope + favourites view (API v14, #236 G) -----------
    // Search scope: false = current folder + subfolders; true = whole project.
    bool (*GetAssetSearchGlobal)() = nullptr;  void (*SetAssetSearchGlobal)(bool on) = nullptr;
    // Favourites-only grid filter (the toolbar star toggle).
    bool (*GetAssetFavoritesOnly)() = nullptr; void (*SetAssetFavoritesOnly)(bool on) = nullptr;
};

struct EditorModuleAPI {
    std::uint32_t Version = kEditorModuleAPIVersion;
    void (*OnLoad)() = nullptr;
    void (*OnUnload)() = nullptr;
    void (*Draw)(const EditorModuleHostAPI& host) = nullptr;
};

using GetEditorModuleAPIFn = const EditorModuleAPI* (*)();
