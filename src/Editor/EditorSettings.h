#pragma once
#include <string>

// Editor-only preferences — NOT scene data. These control how the editor UI itself behaves
// (independent of any particular scene) and persist across restarts in their own small file,
// the same role Unity's EditorPrefs plays relative to a project's actual assets.
//
// A plain Meyer's-singleton struct (a function-local static, returned by reference) rather than
// a heap-allocated singleton: single-threaded UI code, no dynamic-initialization-order concerns,
// and every call site just writes EditorSettings::Get().Field like a normal member access.
struct EditorSettings {
    // Editor theme, applied by EditorLayer::ApplyThemeStyle() (colours + metrics) at launch and
    // live when changed in Preferences > General. (#234 renumbered these when Dark Slate was
    // dropped; EditorSettings::Load remaps any out-of-range value to 0.)
    // 0 = Bento — the default (dark SaaS-dashboard: layered charcoal surfaces, hairline borders,
    //     rounded-card geometry, cyan selection / blue active accents),
    // 1 = Prism — the Bento layout, but the accent / buttons / text tint drift through the
    //     spectrum together each frame (spread across ~half the wheel so several hues show),
    // 2 = Windows XP — the Luna "Blue" scheme: beige chrome, black text, white fields, Luna-blue
    //     selection; compact geometry (the shared metric baseline, not Bento's).
    int EditorTheme = 0;

    // Master switch for every contextual tooltip/help-marker in the editor (Inspector fields,
    // Hierarchy rows, Console controls, Asset Browser, Toolbar Settings). Route all tooltip
    // calls through EditorUI::SetTooltip/HelpMarker (see EditorUIHelpers.h) rather than calling
    // ImGui::SetTooltip directly, so this one flag actually governs all of them.
    bool ShowTooltips = true;

    // The transparent viewport HUDs (Stats, History, the bottom status line, the nav-gizmo
    // cluster, the Play/Stop button, the corner monogram, the Game-view overlays) each sample
    // the scene luminance behind themselves and ease their text — and, where they have one,
    // their backing pill — between light and dark so they stay readable over any render
    // (#178 / #229). Off: they all draw static near-white text with no backing pill.
    bool AdaptiveHudContrast = true;

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

    // --- HDR / tone mapping (lighting overhaul). The scene renders to a linear RGBA16F MSAA
    // target; a fullscreen pass then applies exposure -> curve -> gamma. ExposureEV is in
    // photographic stops (0 = neutral). TonemapOperator: 0 Reinhard, 1 ACES, 2 AgX.
    // MsaaSamples: 1 / 2 / 4 / 8 for the HDR target (clamped to the driver max).
    float ExposureEV = 0.0f;
    int TonemapOperator = 1;
    int MsaaSamples = 4;

    // --- Screen-space ambient occlusion (PR15). Off by default; toggle in Lighting > Post-processing.
    bool SsaoEnabled = false;

    // --- Directional-sun cascaded shadow maps. 4 cascades, PCF, resolution per layer.
    // ShadowDistance caps how far (world units) the cascades reach from the camera.
    bool ShadowsEnabled = true;
    int ShadowResolution = 4096;
    int ShadowCascades = 4;          // 2..4 — fewer = cheaper, coarser far shadows
    float ShadowDistance = 500.0f;

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
    // built-in default (project/scenes/Showcase.json). Written by OpenScene / DoSaveAs. (#95)
    std::string LastScenePath;

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

    // --- Corner "engine mark": the spinning TE monogram in the Scene viewport's bottom-left.
    // EngineMarkSpinSpeed is radians/sec (0 parks it). EngineMarkPrism paints the monogram with
    // a slowly-drifting spectral gradient instead of the default contrast-adaptive grey.
    // Controls live in Preferences > Viewport and the Window menu.
    bool  EngineMarkEnabled   = true;
    float EngineMarkSpinSpeed = 0.52f;
    bool  EngineMarkPrism     = false;

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

    // #185 — Debug physics harness while playing: G sets off a shockwave at the Player,
    // left-click raycasts from the eye and shoves whatever it hits. A dev/testing aid for the
    // gameplay force + query API until real gameplay drives it; on by default, toggle in the
    // toolbar Gizmos popup.
    bool  PhysicsDebugInput      = true;

    // --- Physics visual debugger (#185) --------------------------------------------------
    bool     ShowPhysicsPanel      = false;  // the dockable Physics debug window
    bool     PhysicsHudOverlay     = false;  // corner stats overlay while playing
    unsigned PhysicsDebugDrawFlags = 0;      // PhysicsWorld::PhysicsDebugDrawFlag bitmask
    float    PhysicsSimTimeScale   = 1.0f;   // slow-mo / freeze on the physics step [0, 2]
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

    static EditorSettings& Get() {
        static EditorSettings instance;
        return instance;
    }

    // Reads editor_prefs.json from the working directory into Get(), if present. Missing or
    // unparsable file silently keeps the compiled-in defaults above — first launch, or a
    // hand-deleted prefs file, is not an error.
    static void Load();

    // Writes the current Get() state to editor_prefs.json. Called immediately whenever a
    // preference changes (not batched/on-exit-only) so a crash or force-quit never loses a
    // just-made preference change.
    static void Save();

private:
    EditorSettings() = default;
};
