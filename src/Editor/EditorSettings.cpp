#include "EditorSettings.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "UserPaths.h"
#include "AtomicFile.h"

#include <json.hpp>
#include <fstream>
#include <filesystem>

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
    s.ShowTooltips = root.value("showTooltips", s.ShowTooltips);
    s.UiScaleOverride = root.value("uiScaleOverride", s.UiScaleOverride);
    s.AutoSaveEnabled = root.value("autoSaveEnabled", s.AutoSaveEnabled);
    s.AutoSaveIntervalMinutes = root.value("autoSaveIntervalMinutes", s.AutoSaveIntervalMinutes);
    s.VSyncMode = root.value("vsyncMode", s.VSyncMode);
    s.FpsLimit = root.value("fpsLimit", s.FpsLimit);
    // exposureEV/tonemapOperator/msaaSamples/ssaoEnabled/bloom*/shadow* intentionally no longer
    // read here (#9, Phase M item 1) — moved to World/scene data. A pre-v3 scene's values are
    // migrated forward by SceneSerializer reading this file's legacy keys directly (see
    // EditorSettings::PrefsFilePath()); an old prefs file's stray keys are simply ignored here.
    s.GridOpacity = root.value("gridOpacity", s.GridOpacity);
    s.GridMinorSpacing = root.value("gridMinorSpacing", s.GridMinorSpacing);
    s.GridMajorEvery = root.value("gridMajorEvery", s.GridMajorEvery);
    s.GridFadeDistance = root.value("gridFadeDistance", s.GridFadeDistance);
    s.GridShowAxisLines = root.value("gridShowAxisLines", s.GridShowAxisLines);
    s.GridAxisThickness = root.value("gridAxisThickness", s.GridAxisThickness);
    s.LastScenePath = root.value("lastScenePath", s.LastScenePath);
    s.GameViewMaximizeOnPlay = root.value("gameViewMaximizeOnPlay", s.GameViewMaximizeOnPlay);
    s.GameViewShowStats = root.value("gameViewShowStats", s.GameViewShowStats);
    s.SceneShowStats = root.value("sceneShowStats", s.SceneShowStats);
    s.SceneCameraFov = root.value("sceneCameraFov", s.SceneCameraFov);
    s.SceneCameraFlySpeed = root.value("sceneCameraFlySpeed", s.SceneCameraFlySpeed);
    s.SceneCameraNear = root.value("sceneCameraNear", s.SceneCameraNear);
    s.SceneCameraFar = root.value("sceneCameraFar", s.SceneCameraFar);
    s.AudioMuted = root.value("audioMuted", s.AudioMuted);
    s.AssetSearchGlobal = root.value("assetSearchGlobal", s.AssetSearchGlobal);
    s.GameViewPresetLabel = root.value("gameViewPresetLabel", s.GameViewPresetLabel);
    s.GameViewPresetWidth = root.value("gameViewPresetWidth", s.GameViewPresetWidth);
    s.GameViewPresetHeight = root.value("gameViewPresetHeight", s.GameViewPresetHeight);
    s.AssetBrowserTreeWidth = root.value("assetBrowserTreeWidth", s.AssetBrowserTreeWidth);
    s.AssetBrowserIconSize = root.value("assetBrowserIconSize", s.AssetBrowserIconSize);
    s.AssetSortMode = root.value("assetSortMode", s.AssetSortMode);
    s.AssetSortDesc = root.value("assetSortDesc", s.AssetSortDesc);
    s.AssetDetailsMode = root.value("assetDetailsMode", s.AssetDetailsMode);
    s.HierarchySortMode = root.value("hierarchySortMode", s.HierarchySortMode);
    s.HierarchySortDesc = root.value("hierarchySortDesc", s.HierarchySortDesc);
    s.HierarchyTypeFilterMask = root.value("hierarchyTypeFilterMask", s.HierarchyTypeFilterMask);
    s.EngineMarkEnabled = root.value("engineMarkEnabled", s.EngineMarkEnabled);
    s.EngineMarkSpinSpeed = root.value("engineMarkSpinSpeed", s.EngineMarkSpinSpeed);
    s.EngineMarkPrism = root.value("engineMarkPrism", s.EngineMarkPrism);
    s.ReduceMotion = root.value("reduceMotion", s.ReduceMotion);
    s.DefaultLayoutPreset = root.value("defaultLayoutPreset", s.DefaultLayoutPreset);
    s.ShowLightGizmos = root.value("showLightGizmos", s.ShowLightGizmos);
    s.LightGizmoSelectedOnly = root.value("lightGizmoSelectedOnly", s.LightGizmoSelectedOnly);
    s.LightGizmoOpacity = root.value("lightGizmoOpacity", s.LightGizmoOpacity);
    s.LightGizmoScale = root.value("lightGizmoScale", s.LightGizmoScale);
    s.ShowColliders = root.value("showColliders", s.ShowColliders);
    s.ToolPaletteCollapsed = root.value("toolPaletteCollapsed", s.ToolPaletteCollapsed);
    s.PhysicsDebugInput = root.value("physicsDebugInput", s.PhysicsDebugInput);
    s.ShowPhysicsPanel = root.value("showPhysicsPanel", s.ShowPhysicsPanel);
    s.PhysicsHudOverlay = root.value("physicsHudOverlay", s.PhysicsHudOverlay);
    s.PhysicsDebugDrawFlags = root.value("physicsDebugDrawFlags", s.PhysicsDebugDrawFlags);
    // PhysicsSimTimeScale (Phase 6 item 13 / Appendix B #39) is deliberately NOT loaded - it stays
    // session-only (see the struct field's comment), so a session left slowed/frozen can't leave
    // physics silently frozen the next time the editor opens.
    s.PlayDebugOverlay = root.value("playDebugOverlay", s.PlayDebugOverlay);
    s.LayerVisibleMask = root.value("layerVisibleMask", s.LayerVisibleMask);
    s.LayerPickLockMask = root.value("layerPickLockMask", s.LayerPickLockMask);
    s.CaptureMode = root.value("captureMode", s.CaptureMode);
    s.CaptureScale = root.value("captureScale", s.CaptureScale);
    s.CaptureResPreset = root.value("captureResPreset", s.CaptureResPreset);
    s.CaptureFormat = root.value("captureFormat", s.CaptureFormat);
    s.CaptureFlash = root.value("captureFlash", s.CaptureFlash);
    s.CaptureSound = root.value("captureSound", s.CaptureSound);
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

    // Atomic: a crash mid-write (a toggle spree can still trigger one write) must leave the
    // previous editor_prefs.json intact, not truncated (audit CPP-206). Lives under UserPaths
    // now, not project/ (#42).
    if (!AtomicFile::WriteJson(PrefsPath(), root))
        Log::Warn(std::string("EditorSettings: failed to write '") + PrefsPath() + "'.");
}
