#include "EditorSettings.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "UserPaths.h"
#include "AtomicFile.h"

#include <json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>

using json = nlohmann::json;

namespace {
// Per-user, per-machine state (#42) — NOT under the project folder, so two teammates (or two
// machines) on the same project stop fighting over UI scale/theme/layout on every commit. See
// UserPaths.h.
const std::string& PrefsPath() {
    static const std::string path = UserPaths::Resolve("editor_prefs.json");
    return path;
}

// One-time migration (#42): editor_prefs.json used to live under the version-controlled project/
// folder. If the new user-scoped copy doesn't exist yet but an old project-scoped one does,
// carry it over once rather than silently resetting every preference to default on first run
// after upgrading.
void MigrateLegacyPrefsIfNeeded() {
    std::error_code ec;
    if (std::filesystem::exists(PrefsPath(), ec)) return;
    const std::string legacyPath = ProjectPaths::Resolve("editor_prefs.json");
    if (!std::filesystem::exists(legacyPath, ec)) return;
    std::filesystem::copy_file(legacyPath, PrefsPath(), ec);
    if (ec) Log::Warn("EditorSettings: found a legacy project/editor_prefs.json but couldn't migrate it.");
    else Log::Info("EditorSettings: migrated editor_prefs.json out of project/ to per-user storage.");
}

// Save() is called from ~40 preference controls, several of them every frame of a slider drag
// (#221) and dozens of times during a checkbox spree. Rather than a full atomic rewrite per
// call, Save() just raises this flag and Flush() (once per frame from the editor loop, and on
// shutdown) does the one real write (audit CPP-206 / PERF-211).
bool g_PrefsDirty = false;

// #126 — root.value(key, default) THROWS json::type_error when the key exists with the wrong
// type (a hand edit, an older/newer build), and nothing caught it, so one bad key in
// editor_prefs.json stopped the editor from starting on every launch. This reads the key if it
// has a usable type and otherwise keeps the default, with a warning.
template <class T>
T SafeValue(const json& root, const char* key, const T& fallback) {
    auto it = root.find(key);
    if (it == root.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (const std::exception&) {
        Log::Warn(std::string("EditorSettings: ignoring '") + key + "' (unexpected type " +
                  it->type_name() + ") - using the default.");
        return fallback;
    }
}
}

const std::string& EditorSettings::PrefsFilePath() {
    return PrefsPath();
}

void EditorSettings::Load() {
    MigrateLegacyPrefsIfNeeded();
    std::ifstream in(PrefsPath());
    if (!in.is_open()) return; // no prefs file yet — defaults stand, not an error

    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        Log::Warn(std::string("EditorSettings: failed to parse '") + PrefsPath() + "': " + e.what());
        return;
    }

    EditorSettings& s = Get();
    // editorTheme (int) is intentionally no longer read: the theme picker was removed and the
    // editor now has one style. An old prefs file's stray "editorTheme" key is simply ignored.
    s.ShowTooltips = SafeValue(root, "showTooltips", s.ShowTooltips);
    s.UiScaleOverride = SafeValue(root, "uiScaleOverride", s.UiScaleOverride);
    s.AutoSaveEnabled = SafeValue(root, "autoSaveEnabled", s.AutoSaveEnabled);
    s.AutoSaveIntervalMinutes = SafeValue(root, "autoSaveIntervalMinutes", s.AutoSaveIntervalMinutes);
    s.VSyncMode = SafeValue(root, "vsyncMode", s.VSyncMode);
    s.FpsLimit = SafeValue(root, "fpsLimit", s.FpsLimit);
    s.UnfocusedFpsLimit = SafeValue(root, "unfocusedFpsLimit", s.UnfocusedFpsLimit);
    if (auto wp = root.find("windowPlacement"); wp != root.end() && wp->is_object()) {
        s.WindowX = SafeValue(*wp, "x", 0);
        s.WindowY = SafeValue(*wp, "y", 0);
        s.WindowWidth = SafeValue(*wp, "width", 0);
        s.WindowHeight = SafeValue(*wp, "height", 0);
        s.WindowMaximized = SafeValue(*wp, "maximized", true);
        s.WindowPlacementValid = s.WindowWidth > 0 && s.WindowHeight > 0;
    }
    // exposureEV/tonemapOperator/msaaSamples/ssaoEnabled/bloom*/shadow* intentionally no longer
    // read here (#9, Phase M item 1) — moved to World/scene data. A pre-v3 scene's values are
    // migrated forward by SceneSerializer reading this file's legacy keys directly (see
    // EditorSettings::PrefsFilePath()); an old prefs file's stray keys are simply ignored here.
    s.GridOpacity = SafeValue(root, "gridOpacity", s.GridOpacity);
    s.GridMinorSpacing = SafeValue(root, "gridMinorSpacing", s.GridMinorSpacing);
    s.GridMajorEvery = SafeValue(root, "gridMajorEvery", s.GridMajorEvery);
    s.GridFadeDistance = SafeValue(root, "gridFadeDistance", s.GridFadeDistance);
    s.GridShowAxisLines = SafeValue(root, "gridShowAxisLines", s.GridShowAxisLines);
    s.GridAxisThickness = SafeValue(root, "gridAxisThickness", s.GridAxisThickness);
    s.LastScenePath = SafeValue(root, "lastScenePath", s.LastScenePath);
    s.GameViewMaximizeOnPlay = SafeValue(root, "gameViewMaximizeOnPlay", s.GameViewMaximizeOnPlay);
    s.GameViewShowStats = SafeValue(root, "gameViewShowStats", s.GameViewShowStats);
    s.SceneShowStats = SafeValue(root, "sceneShowStats", s.SceneShowStats);
    s.SceneCameraFov = SafeValue(root, "sceneCameraFov", s.SceneCameraFov);
    s.SceneCameraFlySpeed = SafeValue(root, "sceneCameraFlySpeed", s.SceneCameraFlySpeed);
    s.SceneCameraNear = SafeValue(root, "sceneCameraNear", s.SceneCameraNear);
    s.SceneCameraFar = SafeValue(root, "sceneCameraFar", s.SceneCameraFar);
    s.AudioMuted = SafeValue(root, "audioMuted", s.AudioMuted);
    s.AssetSearchGlobal = SafeValue(root, "assetSearchGlobal", s.AssetSearchGlobal);
    s.GameViewPresetLabel = SafeValue(root, "gameViewPresetLabel", s.GameViewPresetLabel);
    s.GameViewPresetWidth = SafeValue(root, "gameViewPresetWidth", s.GameViewPresetWidth);
    s.GameViewPresetHeight = SafeValue(root, "gameViewPresetHeight", s.GameViewPresetHeight);
    s.AssetBrowserTreeWidth = SafeValue(root, "assetBrowserTreeWidth", s.AssetBrowserTreeWidth);
    s.AssetBrowserIconSize = SafeValue(root, "assetBrowserIconSize", s.AssetBrowserIconSize);
    s.AssetSortMode = SafeValue(root, "assetSortMode", s.AssetSortMode);
    s.AssetSortDesc = SafeValue(root, "assetSortDesc", s.AssetSortDesc);
    s.AssetDetailsMode = SafeValue(root, "assetDetailsMode", s.AssetDetailsMode);
    s.HierarchySortMode = SafeValue(root, "hierarchySortMode", s.HierarchySortMode);
    s.HierarchySortDesc = SafeValue(root, "hierarchySortDesc", s.HierarchySortDesc);
    s.HierarchyTypeFilterMask = SafeValue(root, "hierarchyTypeFilterMask", s.HierarchyTypeFilterMask);
    s.EngineMarkEnabled = SafeValue(root, "engineMarkEnabled", s.EngineMarkEnabled);
    s.EngineMarkSpinSpeed = SafeValue(root, "engineMarkSpinSpeed", s.EngineMarkSpinSpeed);
    s.EngineMarkPrism = SafeValue(root, "engineMarkPrism", s.EngineMarkPrism);
    s.ReduceMotion = SafeValue(root, "reduceMotion", s.ReduceMotion);
    s.DefaultLayoutPreset = SafeValue(root, "defaultLayoutPreset", s.DefaultLayoutPreset);
    s.ShowLightGizmos = SafeValue(root, "showLightGizmos", s.ShowLightGizmos);
    s.LightGizmoSelectedOnly = SafeValue(root, "lightGizmoSelectedOnly", s.LightGizmoSelectedOnly);
    s.LightGizmoOpacity = SafeValue(root, "lightGizmoOpacity", s.LightGizmoOpacity);
    s.LightGizmoScale = SafeValue(root, "lightGizmoScale", s.LightGizmoScale);
    s.ShowColliders = SafeValue(root, "showColliders", s.ShowColliders);
    s.ToolPaletteCollapsed = SafeValue(root, "toolPaletteCollapsed", s.ToolPaletteCollapsed);
    s.PhysicsDebugInput = SafeValue(root, "physicsDebugInput", s.PhysicsDebugInput);
    s.ShowPhysicsPanel = SafeValue(root, "showPhysicsPanel", s.ShowPhysicsPanel);
    s.PhysicsHudOverlay = SafeValue(root, "physicsHudOverlay", s.PhysicsHudOverlay);
    s.PhysicsDebugDrawFlags = SafeValue(root, "physicsDebugDrawFlags", s.PhysicsDebugDrawFlags);
    s.LogPhysicsEvents = SafeValue(root, "logPhysicsEvents", s.LogPhysicsEvents);
    // PhysicsSimTimeScale (Phase 6 item 13 / Appendix B #39) is deliberately NOT loaded - it stays
    // session-only (see the struct field's comment), so a session left slowed/frozen can't leave
    // physics silently frozen the next time the editor opens.
    s.PlayDebugOverlay = SafeValue(root, "playDebugOverlay", s.PlayDebugOverlay);
    s.LayerVisibleMask = SafeValue(root, "layerVisibleMask", s.LayerVisibleMask);
    s.LayerPickLockMask = SafeValue(root, "layerPickLockMask", s.LayerPickLockMask);
    s.CaptureMode = SafeValue(root, "captureMode", s.CaptureMode);
    s.CaptureScale = SafeValue(root, "captureScale", s.CaptureScale);
    s.CaptureResPreset = SafeValue(root, "captureResPreset", s.CaptureResPreset);
    s.CaptureFormat = SafeValue(root, "captureFormat", s.CaptureFormat);
    s.CaptureFlash = SafeValue(root, "captureFlash", s.CaptureFlash);
    s.CaptureSound = SafeValue(root, "captureSound", s.CaptureSound);
    s.ViewShowGrid = SafeValue(root, "viewShowGrid", s.ViewShowGrid);
    s.ViewShowGizmo = SafeValue(root, "viewShowGizmo", s.ViewShowGizmo);
    s.ViewFrameOnSelect = SafeValue(root, "viewFrameOnSelect", s.ViewFrameOnSelect);
    s.GizmoSize = SafeValue(root, "gizmoSize", s.GizmoSize);
    s.VertexPickPixels = SafeValue(root, "vertexPickPixels", s.VertexPickPixels);
    s.SnapEnabled = SafeValue(root, "snapEnabled", s.SnapEnabled);
    s.SnapTranslation = SafeValue(root, "snapTranslation", s.SnapTranslation);
    s.SnapRotationDeg = SafeValue(root, "snapRotationDeg", s.SnapRotationDeg);
    s.SnapScale = SafeValue(root, "snapScale", s.SnapScale);
    s.GizmoLocalSpace = SafeValue(root, "gizmoLocalSpace", s.GizmoLocalSpace);
    s.GizmoPivotCenter = SafeValue(root, "gizmoPivotCenter", s.GizmoPivotCenter);
    s.ActiveTool = SafeValue(root, "activeTool", s.ActiveTool);
    s.ShadingMode = SafeValue(root, "shadingMode", s.ShadingMode);

    // #126 — values that are the right type but nonsensical (0 / negative / NaN from a hand
    // edit or an older build) would otherwise produce NaN projections, a zero-size grid, a
    // zero-interval autosave, etc. Clamp them into the ranges the Preferences UI allows.
    auto clampF = [](float& v, float lo, float hi, float fallback) {
        if (!std::isfinite(v)) v = fallback;
        v = std::clamp(v, lo, hi);
    };
    if (s.UiScaleOverride != 0.0f) clampF(s.UiScaleOverride, 0.75f, 2.5f, 0.0f);
    clampF(s.AutoSaveIntervalMinutes, 1.0f, 60.0f, 5.0f);
    s.VSyncMode = std::clamp(s.VSyncMode, 0, 2);
    if (s.FpsLimit < 0) s.FpsLimit = 0;
    s.FpsLimit = std::min(s.FpsLimit, 1000);
    s.UnfocusedFpsLimit = std::clamp(s.UnfocusedFpsLimit, 0, 1000);
    clampF(s.GridOpacity, 0.0f, 1.0f, 0.6f);
    clampF(s.GridMinorSpacing, 0.05f, 50.0f, 1.0f);
    s.GridMajorEvery = std::clamp(s.GridMajorEvery, 2, 100);
    clampF(s.GridFadeDistance, 10.0f, 1000.0f, 80.0f);
    clampF(s.GridAxisThickness, 0.5f, 4.0f, 0.5f);
    clampF(s.SceneCameraFov, 30.0f, 110.0f, 75.0f);
    clampF(s.SceneCameraFlySpeed, 0.5f, 200.0f, 8.0f);
    clampF(s.SceneCameraNear, 0.001f, 10.0f, 0.05f);
    clampF(s.SceneCameraFar, 1.0f, 10000.0f, 500.0f);
    if (s.SceneCameraFar <= s.SceneCameraNear) s.SceneCameraFar = s.SceneCameraNear + 1.0f;
    clampF(s.LightGizmoOpacity, 0.0f, 1.0f, 0.5f);
    clampF(s.LightGizmoScale, 0.25f, 3.0f, 1.0f);
    clampF(s.EngineMarkSpinSpeed, 0.0f, 4.0f, 0.52f);
    s.CaptureMode = std::clamp(s.CaptureMode, 0, 3);
    s.CaptureScale = std::clamp(s.CaptureScale, 1, 4);
    s.CaptureFormat = std::clamp(s.CaptureFormat, 0, 1);
    // #135 — same ranges as the Preferences / toolbar sliders.
    clampF(s.GizmoSize, 0.05f, 0.40f, 0.15f);
    clampF(s.VertexPickPixels, 5.0f, 150.0f, 35.0f);
    clampF(s.SnapTranslation, 0.001f, 100.0f, 1.0f);
    clampF(s.SnapRotationDeg, 0.1f, 180.0f, 15.0f);
    clampF(s.SnapScale, 0.001f, 10.0f, 0.1f);
    s.ActiveTool = std::clamp(s.ActiveTool, 0, 4);   // GizmoOp::Translate..Universal
    s.ShadingMode = std::clamp(s.ShadingMode, 0, 5); // EditorLayer::ShadingMode::Shaded..Mip
}

void EditorSettings::Save() {
    // Coalesced: the real write happens in Flush(), called once per editor frame and on
    // shutdown. Worst case a change made this frame is lost to a crash before the frame ends;
    // that beats dozens of non-atomic full rewrites during a slider drag / toggle spree.
    g_PrefsDirty = true;
}

void EditorSettings::Flush() {
    if (!g_PrefsDirty) return;
    g_PrefsDirty = false;

    json root;
    root["showTooltips"] = Get().ShowTooltips;
    root["uiScaleOverride"] = Get().UiScaleOverride;
    root["autoSaveEnabled"] = Get().AutoSaveEnabled;
    root["autoSaveIntervalMinutes"] = Get().AutoSaveIntervalMinutes;
    root["vsyncMode"] = Get().VSyncMode;
    root["fpsLimit"] = Get().FpsLimit;
    root["unfocusedFpsLimit"] = Get().UnfocusedFpsLimit;
    if (Get().WindowPlacementValid) {
        root["windowPlacement"] = {{"x", Get().WindowX}, {"y", Get().WindowY},
                                   {"width", Get().WindowWidth}, {"height", Get().WindowHeight},
                                   {"maximized", Get().WindowMaximized}};
    }
    // exposureEV/tonemapOperator/msaaSamples/ssaoEnabled/bloom*/shadow* intentionally no longer
    // written here — see the matching comment in Load(). Once every project has been opened at
    // least once under formatVersion 3+, an old prefs file's stray legacy keys simply age out.
    root["gridOpacity"] = Get().GridOpacity;
    root["gridMinorSpacing"] = Get().GridMinorSpacing;
    root["gridMajorEvery"] = Get().GridMajorEvery;
    root["gridFadeDistance"] = Get().GridFadeDistance;
    root["gridShowAxisLines"] = Get().GridShowAxisLines;
    root["gridAxisThickness"] = Get().GridAxisThickness;
    root["lastScenePath"] = Get().LastScenePath;
    root["gameViewMaximizeOnPlay"] = Get().GameViewMaximizeOnPlay;
    root["gameViewShowStats"] = Get().GameViewShowStats;
    root["sceneShowStats"] = Get().SceneShowStats;
    root["sceneCameraFov"] = Get().SceneCameraFov;
    root["sceneCameraFlySpeed"] = Get().SceneCameraFlySpeed;
    root["sceneCameraNear"] = Get().SceneCameraNear;
    root["sceneCameraFar"] = Get().SceneCameraFar;
    root["audioMuted"] = Get().AudioMuted;
    root["assetSearchGlobal"] = Get().AssetSearchGlobal;
    root["gameViewPresetLabel"] = Get().GameViewPresetLabel;
    root["gameViewPresetWidth"] = Get().GameViewPresetWidth;
    root["gameViewPresetHeight"] = Get().GameViewPresetHeight;
    root["assetBrowserTreeWidth"] = Get().AssetBrowserTreeWidth;
    root["assetBrowserIconSize"] = Get().AssetBrowserIconSize;
    root["assetSortMode"] = Get().AssetSortMode;
    root["assetSortDesc"] = Get().AssetSortDesc;
    root["assetDetailsMode"] = Get().AssetDetailsMode;
    root["hierarchySortMode"] = Get().HierarchySortMode;
    root["hierarchySortDesc"] = Get().HierarchySortDesc;
    root["hierarchyTypeFilterMask"] = Get().HierarchyTypeFilterMask;
    root["engineMarkEnabled"] = Get().EngineMarkEnabled;
    root["engineMarkSpinSpeed"] = Get().EngineMarkSpinSpeed;
    root["engineMarkPrism"] = Get().EngineMarkPrism;
    root["reduceMotion"] = Get().ReduceMotion;
    root["defaultLayoutPreset"] = Get().DefaultLayoutPreset;
    root["showLightGizmos"] = Get().ShowLightGizmos;
    root["lightGizmoSelectedOnly"] = Get().LightGizmoSelectedOnly;
    root["lightGizmoOpacity"] = Get().LightGizmoOpacity;
    root["lightGizmoScale"] = Get().LightGizmoScale;
    root["showColliders"] = Get().ShowColliders;
    root["toolPaletteCollapsed"] = Get().ToolPaletteCollapsed;
    root["physicsDebugInput"] = Get().PhysicsDebugInput;
    root["showPhysicsPanel"] = Get().ShowPhysicsPanel;
    root["physicsHudOverlay"] = Get().PhysicsHudOverlay;
    root["physicsDebugDrawFlags"] = Get().PhysicsDebugDrawFlags;
    root["logPhysicsEvents"] = Get().LogPhysicsEvents;
    // PhysicsSimTimeScale is session-only - see the matching comment in Load().
    root["playDebugOverlay"] = Get().PlayDebugOverlay;
    root["layerVisibleMask"] = Get().LayerVisibleMask;
    root["layerPickLockMask"] = Get().LayerPickLockMask;
    root["captureMode"] = Get().CaptureMode;
    root["captureScale"] = Get().CaptureScale;
    root["captureResPreset"] = Get().CaptureResPreset;
    root["captureFormat"] = Get().CaptureFormat;
    root["captureFlash"] = Get().CaptureFlash;
    root["captureSound"] = Get().CaptureSound;
    root["viewShowGrid"] = Get().ViewShowGrid;
    root["viewShowGizmo"] = Get().ViewShowGizmo;
    root["viewFrameOnSelect"] = Get().ViewFrameOnSelect;
    root["gizmoSize"] = Get().GizmoSize;
    root["vertexPickPixels"] = Get().VertexPickPixels;
    root["snapEnabled"] = Get().SnapEnabled;
    root["snapTranslation"] = Get().SnapTranslation;
    root["snapRotationDeg"] = Get().SnapRotationDeg;
    root["snapScale"] = Get().SnapScale;
    root["gizmoLocalSpace"] = Get().GizmoLocalSpace;
    root["gizmoPivotCenter"] = Get().GizmoPivotCenter;
    root["activeTool"] = Get().ActiveTool;
    root["shadingMode"] = Get().ShadingMode;

    // Atomic: a crash mid-write (a toggle spree can still trigger one write) must leave the
    // previous editor_prefs.json intact, not truncated (audit CPP-206). Lives under UserPaths
    // now, not project/ (#42).
    if (!AtomicFile::WriteJson(PrefsPath(), root))
        Log::Warn(std::string("EditorSettings: failed to write '") + PrefsPath() + "'.");
}
