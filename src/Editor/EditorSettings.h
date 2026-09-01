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
    // Master switch for every contextual tooltip/help-marker in the editor (Inspector fields,
    // Hierarchy rows, Console controls, Asset Browser, Toolbar Settings). Route all tooltip
    // calls through EditorUI::SetTooltip/HelpMarker (see EditorUIHelpers.h) rather than calling
    // ImGui::SetTooltip directly, so this one flag actually governs all of them.
    bool ShowTooltips = true;

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
    int FpsLimit = 0;

    // --- HDR / tone mapping (lighting overhaul). The scene renders to a linear RGBA16F MSAA
    // target; a fullscreen pass then applies exposure -> curve -> gamma. ExposureEV is in
    // photographic stops (0 = neutral). TonemapOperator: 0 Reinhard, 1 ACES, 2 AgX.
    // MsaaSamples: 1 / 2 / 4 / 8 for the HDR target (clamped to the driver max).
    float ExposureEV = 0.0f;
    int TonemapOperator = 1;
    int MsaaSamples = 4;

    // --- Directional-sun cascaded shadow maps. 4 cascades, PCF, resolution per layer.
    // ShadowDistance caps how far (world units) the cascades reach from the camera.
    bool ShadowsEnabled = true;
    int ShadowResolution = 2048;
    int ShadowCascades = 4;          // 2..4 — fewer = cheaper, coarser far shadows
    float ShadowDistance = 80.0f;

    // Absolute path of the scene open when the editor last closed / last Open'd / Saved As.
    // Loaded on startup when the file still exists; empty (or missing file) falls back to the
    // built-in default (project/scenes/Test.json). Written by OpenScene / DoSaveAs. (#95)
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
    std::string GameViewPresetLabel = "Free Aspect";
    int GameViewPresetWidth = 0;
    int GameViewPresetHeight = 0;

    // Asset Browser layout, in actual pixels (already DPI-scaled). 0 = "use the DPI-scaled
    // default" — EditorLayer::Init picks a sensible starting width/icon size on first run, then
    // the user's last splitter drag / icon-size slider position is written back here.
    float AssetBrowserTreeWidth = 0.0f;
    float AssetBrowserIconSize = 0.0f;

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
