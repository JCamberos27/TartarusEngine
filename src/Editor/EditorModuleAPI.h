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
//   15 - History HUD (frame only): the module owns the pinned bottom-right transparent HUD
//        window, its position + height-ceiling math and the contrast-adaptive text tint.
//        GetHistoryHudFrame (draw? + viewport rect + UI scale + row count), DrawHistoryListBody
//        (the click-to-jump rows — undo/redo stacks + World& stay host-side) and
//        SampleHistoryHudLuminance (host-driven PBO readback, like the Stats HUD's).
//        GetShowHistory/SetShowHistory (v4) still carry the toggle + the title-bar X.
//   16 - Editor chrome polish: OpenProjectSettings (Project Settings opens from its own menu-bar
//        item beside Preferences, not a Window-menu toggle) and GetAdaptiveHudContrast (the
//        Preferences ▸ Viewport switch the module Stats/History HUDs read to drop their
//        luminance sampling + backing pill).
//   17 - Defect #54 (Phase 1): removed SampleViewportLuminance, SampleHistoryHudLuminance and
//        GetAdaptiveHudContrast. Every viewport HUD now draws a fixed opaque plate behind fixed
//        light text (EditorUIPrimitives.h) instead of sampling the rendered scene's luminance —
//        the whole async-GPU-readback subsystem (AsyncLuminanceReadback, SampleTextureLuminance,
//        7+ scratch FBOs, 14+ PBOs, a GLStateCache-bypassing glGetIntegerv call) is gone with it.
//   18 - Phase 1 item 9 (theme collapse): removed GetEditorTheme. EditorTheme collapsed from
//        three values (Bento/Prism/Windows XP) to two (Dark/Light); its one module consumer, the
//        toolbar's Windows-XP Luna-chrome override, is deleted along with the theme, so no module
//        code reads the raw theme index anymore.
//   19 - Phase 1 item 5: GetMonoFont, exposing the host-baked JetBrains Mono face so a module can
//        route its own numeric readouts (the Stats HUD) through it, the same way the Console does.
//   20 - Phase 3 item 1 (document strip): GetSceneDisplayName, GetSceneDirty and DoSaveScene, so
//        the toolbar can show the open scene's name + an unsaved-changes dot and offer a Save
//        button — until now the only way to save was the File menu or Ctrl+S.
//   21 - Phase 3 item 2 (Zone B): DrawPlayControlsBody renders Play/Stop/Pause/Step/Restore
//        inline in the toolbar strip. Replaces the floating `##PlayStopButton` overlay for
//        windowed play (still host-drawn directly, unchanged API, for the one case with no
//        toolbar to embed into: maximized play).
//   22 - Q12 / Phase 4 #6: GetInPlayMode, so a docked panel (Inspector, Hierarchy) can tint
//        itself while Playing, matching the host's own amber viewport-border banner.
//   23 - Phase 5 item 3: AssetFolderHistoryBack/Forward + Can* queries, so the Asset Browser's
//        new Back/Forward buttons can walk the host's folder-navigation trail. Navigating TO a
//        folder (breadcrumb segments, Up) still goes through the existing SetCurrentAssetFolder —
//        no new pointer needed there, since the host now records history inside it.
//   24 - Phase 5 item 3 (remainder): ToggleAssetViewMode, an explicit Grid/List toggle button next
//        to the icon-size slider. Grid vs. list mode is still derived from icon size (via
//        GetAssetGridMetrics, unchanged) — this just flips it, and the host remembers the last
//        grid zoom level so toggling back to Grid doesn't reset it to the default.
//   25 - Phase 5 item 6 (remainder): Hierarchy type-filter chips + sort control, mirroring the
//        Asset Browser's own Get/SetAssetSort (API v13). Get/SetHierarchyTypeFilter takes a
//        kHierarchyFilter* bitmask (0 = no filter); Get/SetHierarchySort takes a packed
//        mode*2+desc int. Row height itself (24px) needed no new pointer — it's a host-side style
//        tweak inside the existing DrawHierarchyTreeBody callback.
//   26 - Phase 5 item 4: Asset Browser Details view, a third view mode alongside Grid/List.
//        GetAssetViewMode reads it (0 Grid, 1 List, 2 Details); ToggleAssetViewMode (v24) now
//        cycles through all three instead of just Grid<->List. Details renders as single-column
//        rows (like List) with Type/Size/Modified text appended — kAssetDetails*ColW are shared
//        so the module's header row and the host's per-row text land at the same X.
//   27 - Phase 6 item 4 (remainder): Console click-to-navigate. SelectEntityRaw resolves a raw
//        entt integral (as printed by std::to_string(entt::to_integral(e)) in PhysX/Scene log
//        lines) back to a live entity and selects it if still valid. PingAssetPath jumps the
//        Asset Browser to a path if it resolves to a real, existing project file — a registered
//        AssetLibrary entry (model/texture/material/sound/prefab), or a raw scenes/screenshots
//        file, the same two categories the Console's own listings already cover.
//   28 - Phase 6 item 14: notification bell in the top toolbar. GetNotificationUnreadCount reads
//        the badge (Warning/Error entries only — captures and other info/success notifications
//        never count); MarkNotificationsRead clears it (called the moment the bell's dropdown
//        opens); DrawNotificationsPopupBody renders that dropdown's contents (host code, same
//        Draw*PopupBody callback pattern the capture options popup already uses).
//   29 - Phase 6 item 11: shortcut coverage pass. Registered a dozen previously-mouse-only
//        actions (panel toggles, gizmo space/pivot, grid/snap, draw-mode cycling, capture,
//        focus-scene, snap-to-ground) in the Shortcuts table, and added a Help menu whose
//        "Shortcuts" item (OpenShortcutsReference) jumps straight to the existing press-to-bind
//        editor (Preferences category 5) instead of leaving it something you only find browsing.
//   30 - Phase 6 item 5: Statistics panel rebuild. GetFrameTimeHistory reads the host's 120-frame
//        raw (unsmoothed) ring buffer for the panel's sparkline.
//   31 - #182: SelectEntityRaw -> SelectEntityByOrder. Log lines now name entities by their
//        stable OrderComponent value ("entity #12"), because raw entt ids are recycled on every
//        undo/redo, Play->Stop and scene reload, so an old Console link selected the wrong one.
//   32 - #172 / #187: OnLoad(state, size) -> bool and SaveState, the reload contract shared with
//        the game module (Core/HotReloadSwap.h): a build whose OnLoad fails is rolled back to the
//        previous one, and module-held UI state can be carried across a reload.
constexpr std::uint32_t kEditorModuleAPIVersion = 32;

// Asset Browser Details-view column widths (API v26), in unscaled px (the caller applies UI
// scale). Name gets whatever's left of the row after these three.
constexpr float kAssetDetailsTypeColW = 90.0f;
constexpr float kAssetDetailsSizeColW = 80.0f;
constexpr float kAssetDetailsModifiedColW = 150.0f;

// Hierarchy type-filter chip bits (API v25). Passed to Get/SetHierarchyTypeFilter as a bitmask;
// 0 means "no filter" (every kind shown). Mirrors the priority used by the per-row kind badge
// (mesh > light > camera > empty) but as independent bits so a chip toggles regardless of which
// kind a row's badge happens to show.
constexpr int kHierarchyFilterMesh   = 1 << 0;
constexpr int kHierarchyFilterLight  = 1 << 1;
constexpr int kHierarchyFilterCamera = 1 << 2;
constexpr int kHierarchyFilterOther  = 1 << 3;

// ImGui's own allocator signatures, spelled out here so this header stays free of <imgui.h>
// (the host and the module each compile their own ImGui translation units; only the context and
// the allocator pair need to be shared across the boundary).
using EditorModuleImGuiAllocFn = void* (*)(std::size_t size, void* userData);
using EditorModuleImGuiFreeFn  = void  (*)(void* ptr, void* userData);

// Forward-declared, not included: GetMonoFont (API v19) hands back an opaque ImFont* a module
// only ever passes straight to its own ImGui::PushFont — same free-of-<imgui.h> reasoning as the
// allocator signatures above.
struct ImFont;

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

    // --- Console click-to-navigate (API v27) ------------------------------------------------
    // Resolves a reference parsed out of a log message's text and jumps the editor to it.
    // Both return false (no-op) when the reference doesn't resolve to anything live/present —
    // the caller uses that to decide whether a row's context-menu item should even appear.
    bool (*SelectEntityByOrder)(int orderValue) = nullptr; // "entity #N" -> OrderComponent N (v31)
    bool (*PingAssetPath)(const char* path) = nullptr;

    // --- Notification bell (API v28) ---------------------------------------------------------
    int (*GetNotificationUnreadCount)() = nullptr;
    void (*MarkNotificationsRead)() = nullptr;
    void (*DrawNotificationsPopupBody)() = nullptr;

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

    // Phase 6 item 5 — raw (unsmoothed) per-frame ms, oldest-to-newest, up to `maxCount` entries
    // (the host keeps 120). Returns how many it actually wrote. For the Statistics panel's
    // frame-time sparkline; the smoothed figure above is still what the FPS text itself shows.
    int (*GetFrameTimeHistory)(float* out, int maxCount) = nullptr;

    // The JetBrains Mono face (API v19, Phase 1 item 5), for the Console body and numeric
    // readouts (e.g. this panel's own FPS/frame-time/draw-call numbers). Wrap the text that
    // wants it in ImGui::PushFont(host.GetMonoFont())/PopFont() — null-safe: PushFont(nullptr)
    // is a documented no-op in this ImGui version, so a caller doesn't need its own null check
    // if the bundled TTF somehow failed to load.
    ImFont* (*GetMonoFont)() = nullptr;

    // The module's one write-back: true while the (height-capped) Stats HUD reaches far enough
    // down the viewport to collide with the corner engine-mark monogram, so the host hides it.
    // Called once per module Draw (false on the early-out paths).
    void (*SetHideEngineMark)(bool hide) = nullptr;

    // --- Toolbar / menus (API v4) ------------------------------------------------------------
    // The top toolbar strip, its dropdown menus and the window min/max/close controls live in
    // the module now (EditorModuleToolbar.cpp). The module owns the pinned "##Toolbar" window,
    // the icon-button row layout and the menu/popup scaffolding; the deep host logic behind the
    // menus (scene load/save, entity creation, camera framing, undo) stays host-side and is
    // rendered into the module's menus through the Draw*Body callbacks below.

    // Window metrics the strip pins itself against, forced every frame: main-window pixel width,
    // toolbar height (host-owned kToolbarHeight * UI scale) and the editor UI scale.
    void (*GetToolbarMetrics)(float* outWinW, float* outToolbarH, float* outUIScale) = nullptr;
    // GetEditorTheme (API v4) removed at API v18 (Phase 1 item 9) — its only consumer was the
    // toolbar's Windows-XP Luna-chrome override, and that theme no longer exists. The strip now
    // just reads style.Colors[] like every other panel; no module code needs the raw theme index.
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
    void (*OpenProjectSettings)() = nullptr; // v16 — menu-bar item beside Preferences
    void (*OpenShortcutsReference)() = nullptr; // v29 — Help menu's Shortcuts item

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
    // kind 0/1/2 = model / texture / sound, 3 = any (Phase 5 item 5's unified "Import Asset...";
    // kind only picks the dialog's filter — ImportDroppedFile infers the real type from the
    // extension regardless); host opens FileDialog then ImportDroppedFile into `intoFolder` (the
    // current folder).
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

    // --- History HUD, frame only (API v15) ---------------------------------------------
    // GetHistoryHudFrame is superseded as of Phase 3 item 8: History is a real dockable panel
    // now (EditorModuleHistory.cpp just reads GetShowHistory/SetShowHistory, API v4, like every
    // other panel), so nothing calls this pin/height-ceiling query anymore. No struct layout
    // change, so no version bump — left in place rather than renumbering every positional field
    // after it (here and in HotReloadEditorModule.cpp's kHostAPI initializer) for one dead
    // callback; fair game to actually remove in a future cleanup pass.
    bool (*GetHistoryHudFrame)(float* outVpX, float* outVpY, float* outVpW, float* outVpH,
                               float* outUIScale, int* outRowCount) = nullptr;
    // Renders the click-to-jump list into the module's window, between its heading Separator and
    // its End: every undo-stack row, the highlighted "Current" marker, every redo-stack row, and
    // their per-row tooltips + JumpToUndo/RedoEntry calls. The undo/redo stacks, World& and
    // AssetLibrary& never cross the boundary.
    void (*DrawHistoryListBody)() = nullptr;

    // --- Reflection probes (API v16 / PR14) --------------------------------------------
    // Called by the editor to trigger a probe bake on the next frame. The host rebuilds
    // ReflectionProbeArray from the current world and marks all probes dirty; shader
    // variants with _REFLECTION_PROBES receive corrected probe data on subsequent draws.
    void (*RequestBakeReflectionProbes)() = nullptr;

    // --- Document strip (API v20, Phase 3 item 1) --------------------------------------
    // The toolbar's new left-hand cluster: the open scene's filename ("Untitled" for a new,
    // unsaved scene), an unsaved-changes dot, and a Save button — the audit's callout that there
    // was no visible Save control anywhere in the editor.
    void (*GetSceneDisplayName)(char* out, int n) = nullptr;
    bool (*GetSceneDirty)() = nullptr;
    void (*DoSaveScene)() = nullptr;

    // --- Play controls, Zone B (API v21, Phase 3 item 2) -------------------------------
    // Renders Play (or Stop + Pause/Resume + Step + Fullscreen/Restore) inline into the toolbar —
    // the host reads its own cached play/pause/maximize state (pushed in once a frame from
    // main.cpp, which owns the actual simulation clock) and raises the same request flags the old
    // floating overlay did.
    void (*DrawPlayControlsBody)() = nullptr;

    // --- Play-mode panel tint (API v22, Q12 / Phase 4 #6) ------------------------------
    // True while Playing - the same flag the host's own amber viewport-border banner reads. A
    // module panel (Inspector, Hierarchy) uses this to tint itself too, per Q12's settled answer:
    // editing stays fully live in Play mode (it's genuinely useful for tuning values), so this is
    // a reminder that edits here revert on Stop, not a lock.
    bool (*GetInPlayMode)() = nullptr;

    // --- Asset Browser folder history, Phase 5 item 3 (API v23) ------------------------
    // Back/Forward over the trail NavigateAssetFolder (host-side) records every time
    // SetCurrentAssetFolder (above) points the browser somewhere new. The Can* queries drive the
    // module's Back/Forward button enabled state.
    void (*AssetFolderHistoryBack)() = nullptr;
    void (*AssetFolderHistoryForward)() = nullptr;
    bool (*CanAssetFolderHistoryBack)() = nullptr;
    bool (*CanAssetFolderHistoryForward)() = nullptr;

    // --- Asset Browser view-mode toggle, Phase 5 item 3 remainder (API v24) ------------
    // Cycles Grid -> List -> Details -> Grid (Details added in v26). Grid<->List still drives the
    // existing icon-size mechanism (Get/SetAssetIconSize, GetAssetGridMetrics) - the host
    // remembers the icon size it was at before collapsing to list, so toggling back to Grid
    // restores that zoom instead of resetting to the default. Details is a separate bool the
    // icon-size slider doesn't touch either way.
    void (*ToggleAssetViewMode)() = nullptr;

    // --- Hierarchy type filter + sort, Phase 5 item 6 remainder (API v25) --------------
    // GetHierarchyTypeFilter/SetHierarchyTypeFilter carry a kHierarchyFilter* bitmask for the
    // toolbar's four kind chips. GetHierarchySort/SetHierarchySort carry a packed mode*2+desc int,
    // same shape as Get/SetAssetSort above (mode: 0 Creation order, 1 Name, 2 Type).
    int  (*GetHierarchyTypeFilter)() = nullptr; void (*SetHierarchyTypeFilter)(int mask) = nullptr;
    int  (*GetHierarchySort)() = nullptr;       void (*SetHierarchySort)(int packed) = nullptr;

    // --- Asset Browser Details view, Phase 5 item 4 (API v26) --------------------------
    // 0 Grid, 1 List, 2 Details. Drives whether the module forces single-column rows (List and
    // Details both do) and whether it draws the Name/Type/Size/Modified header row.
    int (*GetAssetViewMode)() = nullptr;
};

struct EditorModuleAPI {
    std::uint32_t Version = kEditorModuleAPIVersion;
    // Reload contract (#172/#187, shared with the game module; see Core/HotReloadSwap.h): the
    // host validates a rebuilt DLL, asks the OLD module to SaveState, calls its OnUnload and frees
    // it, and only then calls the NEW module's OnLoad with that state. The two never overlap.
    //   OnLoad: `state` is whatever the previous module's SaveState wrote (nullptr/0 on the first
    //     load or when it saved nothing). Return false to reject the build: undo anything this
    //     call set up first (the host won't call OnUnload), and the host reloads the previous
    //     build and hands it the same state. Every member may be null except Draw.
    //   SaveState: called with (nullptr, 0) to ask how many bytes are needed, then with a buffer
    //     of that size; return the bytes written. Panel state that must survive a reload goes
    //     either here or, better, in host-owned state (EditorConsoleState, the ImGui context's
    //     window settings), which survives reloads and doesn't need a format.
    bool (*OnLoad)(const void* state, std::size_t stateSize) = nullptr;
    void (*OnUnload)() = nullptr;
    void (*Draw)(const EditorModuleHostAPI& host) = nullptr;
    std::size_t (*SaveState)(void* buffer, std::size_t capacity) = nullptr;
};

using GetEditorModuleAPIFn = const EditorModuleAPI* (*)();
