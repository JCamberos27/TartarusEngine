#pragma once
#include <string>
#include <vector>

// Editor-only preferences — NOT scene data. These control how the editor UI itself behaves
// (independent of any particular scene) and persist across restarts in their own small file,
// the same role Unity's EditorPrefs plays relative to a project's actual assets.
//
// A plain Meyer's-singleton struct (a function-local static, returned by reference) rather than
// a heap-allocated singleton: single-threaded UI code, no dynamic-initialization-order concerns,
// and every call site just writes EditorSettings::Get().Field like a normal member access.
struct EditorSettings {
    // Master switch for every contextual tooltip/help-marker in the editor (Inspector fields,
    // Hierarchy rows, Console controls, Asset Browser, Toolbar Settings). Route all tooltip
    // calls through EditorUI::SetTooltip/HelpMarker (see EditorUIHelpers.h) rather than calling
    // ImGui::SetTooltip directly, so this one flag actually governs all of them.
    bool ShowTooltips = true;

    // AdaptiveHudContrast removed (#54, Phase 1): every viewport HUD (Stats, History, the
    // status line, the nav-gizmo cluster, the Play/Stop button, the Game-view overlays) now
    // draws a fixed opaque plate behind fixed light text instead of sampling the scene's
    // luminance — legible over anything, nothing left to toggle.

    // Editor UI scale. 0 = follow the monitor's content scale (Windows display-scaling %), which
    // is right most of the time. Set a value (e.g. 1.25) to override it — useful when a project
    // authored on a high-DPI 4K panel is opened on a plain 1080p monitor and the whole editor
    // reads too small (or vice-versa). Fonts bake at this scale, so a change applies on the next
    // launch. Range clamped to [0.75, 2.5] by the Preferences slider.
    float UiScaleOverride = 0.0f;

    // Periodically re-saves the current scene to its own file while editing (only when there
    // are actually unsaved changes) — a safety net against a crash/force-quit losing work,
    // independent of the existing always-on "save on clean exit" behavior. Interval is in
    // minutes so the Settings UI can offer a plain, human-sized number rather than raw seconds.
    bool AutoSaveEnabled = true;
    float AutoSaveIntervalMinutes = 5.0f;

    // Frame pacing for the whole editor + game loop (main.cpp reads these every frame, so a
    // change in Preferences takes effect immediately).
    //   VSyncMode: 0 = off (uncapped unless FpsLimit is set), 1 = on (swap synced to the
    //   monitor refresh), 2 = adaptive (sync when the frame is on time, tear when it's late —
    //   needs EXT_swap_control_tear; drivers without it fall back to plain vsync).
    //   FpsLimit: 0 = no software cap; otherwise the loop sleeps each frame to hold this rate.
    //   The cap is applied whatever VSyncMode is, but it's really meant for VSyncMode 0.
    int VSyncMode = 1;
    int FpsLimit = 240;
    // #143: cap while the editor window is in the background and not in Play mode, so an idle
    // editor behind other apps doesn't render at full rate (Unity's "Interaction Mode"). 0 = no
    // extra cap. A minimized window doesn't render at all, whatever this is.
    int UnfocusedFpsLimit = 30;

    // HDR/tone mapping, SSAO, bloom, and shadow settings moved to World (#9, Phase M item 1) —
    // they're scene-authored content, not per-user editor prefs. See World.h's ExposureEV et al.
    // A pre-v3 scene's values are migrated forward from this file's on-disk legacy keys by
    // SceneSerializer's format-version migration (see kSceneFormatVersion), not read from here at
    // runtime any more.

    // --- Scene-view ground grid. GridOpacity is a 0..1 master multiplier on the shader's line
    // alpha; the grid also dissolves as the view tilts toward the horizon, like Unity's. The
    // axis lines are the coloured rules through the origin — X (red) and Z (blue) on the
    // ground, plus a green Y line running straight up.
    float GridOpacity = 0.6f;
    float GridMinorSpacing = 1.0f;   // world units between minor lines (also the grid-snap step)
    int   GridMajorEvery = 10;       // a brighter major line every N minor cells
    float GridFadeDistance = 80.0f;  // world units from the camera where the grid fully fades
    bool  GridShowAxisLines = true;
    float GridAxisThickness = 0.5f;  // screen-pixel width of the coloured X/Y/Z axis lines (slider min)

    // Absolute path of the scene open when the editor last closed / last Open'd / Saved As.
    // Loaded on startup when the file still exists; empty (or missing file) falls back to the
    // built-in default (project/scenes/Sandbox.json). Written by OpenScene / DoSaveAs. (#95)
    std::string LastScenePath;
    // File > Open Recent, newest first. Save() moves LastScenePath to the front, so every place
    // that records the last scene also records it here.
    std::vector<std::string> RecentScenes;

    // GameViewPanel's own preferences (see GameViewPanel::LoadSettings/SaveSettings) — kept here
    // rather than in a separate file so they persist through the same Load()/Save() call every
    // other editor preference already goes through. GameViewPresetWidth/Height are only
    // meaningful (nonzero) when the saved preset was a custom fixed resolution, not a built-in.
    bool GameViewMaximizeOnPlay = false;
    bool GameViewShowStats = true;

    // Scene viewport's Statistics overlay (FPS / frame ms / draw calls / triangles / vertices /
    // entity counts) — the editor-side counterpart of GameViewShowStats. Toggled from
    // View > Statistics or the toolbar chart button; persisted so the choice survives a restart.
    bool SceneShowStats = false;

    // Editor fly-camera (#236 R2) — FOV in degrees; fly speed in world units/sec at 1x (Shift
    // still ×3). Adjustable in View ▸ Camera and, for fly speed, by scrolling while holding RMB.
    float SceneCameraFov = 75.0f;
    float SceneCameraFlySpeed = 8.0f;
    float SceneCameraNear = 0.05f;
    float SceneCameraFar = 500.0f;

    // View ▸ Mute Audio (#236 R2) — master-mutes the audio engine. Persisted so a muted
    // session stays muted after a restart.
    bool AudioMuted = false;

    // Asset Browser search scope (#236 G): false = current folder + subfolders (Unity's
    // default), true = the whole project. Toggled by the button next to the search box.
    bool AssetSearchGlobal = false;

    std::string GameViewPresetLabel = "Free Aspect";
    int GameViewPresetWidth = 0;
    int GameViewPresetHeight = 0;

    // Asset Browser layout, in actual pixels (already DPI-scaled). 0 = "use the DPI-scaled
    // default" — EditorLayer::Init picks a sensible starting width/icon size on first run, then
    // the user's last splitter drag / icon-size slider position is written back here.
    float AssetBrowserTreeWidth = 0.0f;
    float AssetBrowserIconSize = 0.0f;
    // Asset Browser grid sort (#236 G). Mode: 0 Name, 1 Type, 2 Date modified, 3 Size.
    // Folders always sort before files regardless. Descending flips within each group.
    int  AssetSortMode = 0;
    bool AssetSortDesc = false;
    // Phase 5 item 4 — Details view is a separate bool from Grid/List (which is still derived
    // from AssetBrowserIconSize): true shows Name/Type/Size/Modified rows instead of the grid.
    bool AssetDetailsMode = false;

    // Hierarchy row sort (Phase 5 item 6). Mode: 0 Creation order (default, OrderComponent), 1
    // Name, 2 Type. Only applies at each sibling level - parenting itself is untouched.
    int  HierarchySortMode = 0;
    bool HierarchySortDesc = false;
    // Type-filter chips bitmask: bit0 Mesh, bit1 Light, bit2 Camera, bit3 Empty/other. 0 or
    // all-bits-set both mean "no filter" (every kind shown).
    int  HierarchyTypeFilterMask = 0;

    // --- Corner "engine mark": the spinning TE monogram in the Scene viewport's bottom-left.
    // EngineMarkSpinSpeed is radians/sec (0 parks it). EngineMarkPrism paints the monogram with
    // a slowly-drifting spectral gradient instead of the default contrast-adaptive grey.
    // Controls live in Preferences > Viewport and the Window menu.
    bool  EngineMarkEnabled   = true;
    float EngineMarkSpinSpeed = 0.52f;
    bool  EngineMarkPrism     = false;

    // Phase 6 item 12 — parks the corner monogram's spin and hue drift, and makes the nav-gizmo
    // view-preset camera transition (EditorLayer_Gizmos.cpp's m_ViewTransition) snap instantly
    // instead of easing over its usual 0.28s, for anyone sensitive to on-screen motion. Doesn't
    // touch EngineMarkSpinSpeed itself - that preference is preserved, just not applied while
    // this is on, so turning it back off resumes the user's own spin rate.
    bool ReduceMotion = false;

    // Phase 6 item 10 — name of the layout preset (<user>/layouts/<name>.ini, #184) that
    // Window > Reset Layout rebuilds to, in place of the four shipped Default/Wide/Tall/Focus
    // arrangements. Empty means "use the shipped Default layout" — the original behavior.
    std::string DefaultLayoutPreset;

    // --- Light gizmos: the 3D wireframe shapes drawn for each light in the Scene viewport
    // (range sphere for point, cone for spot, aim arrow for directional). Controls live in
    // Preferences > Viewport and the Window menu. LightGizmoSelectedOnly (default: on) draws
    // the shape only for lights in the current selection, so a scene full of lights isn't a
    // wall of overlapping rings; turn it off for a whole-scene lighting overview. Opacity/scale
    // tune how loud they read.
    bool  ShowLightGizmos        = true;
    bool  LightGizmoSelectedOnly = true;
    float LightGizmoOpacity      = 0.5f;
    float LightGizmoScale        = 1.0f;

    // --- Collider gizmos (#185): wireframe of every ColliderComponent's shape in the Scene
    // viewport, both edit and Play mode — solid colliders green, triggers yellow (orange while
    // occupied, #185 PR 5). On by default now that the physics system is real; toggled from the
    // toolbar Gizmos popup.
    bool  ShowColliders          = true;
    bool  ShowBodyDebug          = false; // the Gizmos > Player body overlay (foot IK rays, headings, eye anchor)

    // --- Tool palette (Phase 3 item 3): the vertical Hand/Translate/Rotate/Scale/Rect/Universal
    // + Measure/Duplicate-Array + space/pivot rail docked to the Scene viewport's left edge,
    // replacing those buttons' old home in the top toolbar strip. Collapsed to a thin strip
    // (just the chevron) when true; expanded (the default) otherwise.
    bool  ToolPaletteCollapsed   = false;

    // #185 — Debug physics harness while playing: G sets off a shockwave at the Player,
    // left-click raycasts from the eye and shoves whatever it hits. A dev/testing aid for the
    // gameplay force + query API until real gameplay drives it; on by default, toggle in the
    // toolbar Gizmos popup.
    bool  PhysicsDebugInput      = true;

    // --- Physics visual debugger (#185) --------------------------------------------------
    bool     ShowPhysicsPanel      = false;  // the dockable Physics debug window
    // The Asset Library panel: an asset collection outside the project (a folder of packs laid
    // out <Category>/<Asset>/) to browse and import from. Per machine, like the other paths here.
    bool        ShowAssetLibrary = false;
    std::string AssetLibraryPath;
    bool     PhysicsHudOverlay     = false;  // corner stats overlay while playing
    unsigned PhysicsDebugDrawFlags = 0;      // PhysicsWorld::PhysicsDebugDrawFlag bitmask
    bool LogPhysicsEvents = false;           // #169 - trigger / hit / joint-break lines in the Console
    // Slow-mo / freeze on the physics step [0, 2]. Phase 6 item 13 / Appendix B #39 -
    // deliberately session-only (NOT read/written by Load/Save below): a session left at 0x
    // must not silently freeze physics again the next time the editor opens.
    float    PhysicsSimTimeScale   = 1.0f;
    bool     PlayDebugOverlay      = false;  // F3: draw the collider wireframes + debug channels
                                             // over the game view during maximized play too

    // --- Per-layer viewport mask (#236 A1). One bit per LayerRegistry slot (bit N = layer N).
    // LayerVisibleMask: a clear bit hides that layer's entities in the Scene viewport draw (they
    // stay in the scene, the Hierarchy, and every save). LayerPickLockMask: a set bit makes that
    // layer's entities unclickable in the viewport (Hierarchy selection still works). Per-user,
    // not content — the slot *names* are project-scoped (LayerRegistry / project/layers.json).
    // Edited from the toolbar's Gizmos/visibility popup.
    unsigned LayerVisibleMask  = 0xFFFFFFFFu;
    unsigned LayerPickLockMask = 0u;

    // --- Capture (screenshot) tool. Images always write to project/screenshots/ (so the Asset
    // Browser's "Screenshots" folder finds them). Controls live in Preferences > Capture and the
    // toolbar's camera button. CaptureMode: 0 full editor window, 1 Scene viewport only,
    // 2 Scene viewport with all editor overlays hidden, 3 Game view. CaptureScale (1/2/4)
    // supersamples the viewport modes. CaptureResPreset: 0 = match the viewport (with
    // CaptureScale applied), 1..4 = a fixed 720p/1080p/1440p/2160p render. CaptureFormat: 0 PNG, 1 JPG.
    int  CaptureMode      = 1;
    int  CaptureScale     = 1;
    int  CaptureResPreset = 0;
    int  CaptureFormat    = 0;
    bool CaptureFlash  = true;
    bool CaptureSound  = true;

    // --- Scene-view tool state (#135). EditorLayer owns the live copies (m_ShowGrid,
    // m_SnapTranslation, ...), which dozens of menus/hotkeys/toolbar buttons edit directly;
    // Init() seeds them from here and SyncViewportPrefs() writes them back each frame when any
    // changed. ActiveTool is a GizmoOp, ShadingMode an EditorLayer::ShadingMode.
    bool  ViewShowGrid         = true;
    bool  ViewShowGizmo        = true;
    bool  ViewFrameOnSelect    = false;
    float GizmoSize            = 0.15f;
    float VertexPickPixels     = 35.0f;
    bool  SnapEnabled          = true;
    float SnapTranslation      = 1.0f;
    float SnapRotationDeg      = 15.0f;
    float SnapScale            = 0.1f;
    bool  GizmoLocalSpace      = false;
    bool  GizmoPivotCenter     = false;
    int   ActiveTool           = 0;
    int   ShadingMode          = 0;

    // --- Main window placement (#143). Written on clean exit, applied at startup when it still
    // lands on a connected monitor; otherwise the window opens maximized as before. The rect is
    // the restored ("normal") one, so a window closed maximized comes back maximized and
    // un-maximizes to where it was. Coordinates: see Window::Placement.
    // #154 — F11 / Game-view fullscreen: 0 Borderless (covers the monitor, instant alt-tab),
    // 1 Exclusive (takes over the video mode). FullscreenMonitor: -1 = the monitor the window is
    // on, else an index into the connected displays.
    int  FullscreenMode = 0;
    int  FullscreenMonitor = -1;

    bool WindowPlacementValid = false;
    int  WindowX = 0, WindowY = 0, WindowWidth = 0, WindowHeight = 0;
    bool WindowMaximized = true;

    static EditorSettings& Get() {
        static EditorSettings instance;
        return instance;
    }

    // Reads editor_prefs.json from per-user storage into Get(), if present (see UserPaths.h).
    // Missing or unparsable file silently keeps the compiled-in defaults above — first launch,
    // or a hand-deleted prefs file, is not an error.
    static void Load();

    // Marks preferences dirty. Cheap — call it freely whenever a preference changes (every
    // frame of a slider drag is fine). The actual atomic write is coalesced into Flush().
    static void Save();

    // Writes the current Get() state to editor_prefs.json (atomically) if Save() has been
    // called since the last write. The editor loop calls this once per frame and again on
    // shutdown, so at most one prefs write happens per frame regardless of how many controls
    // changed (audit CPP-206 / PERF-211).
    static void Flush();

    // Absolute path to editor_prefs.json (per-user storage, see UserPaths.h). Exposed so
    // SceneSerializer's format-version migration (#9, Phase M item 1) can read a pre-v3 scene's
    // legacy post-processing/shadow values straight off disk — those fields no longer exist on
    // this struct, so they can't be read through Get() any more.
    static const std::string& PrefsFilePath();

private:
    EditorSettings() = default;
};
