#include "EditorSettings.h"
#include "Log.h"
#include "Core/Paths.h"
#include "AtomicFile.h"
#include "AssetDatabase.h" // #132 - lastSceneGuid

#include <json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>

using json = nlohmann::json;
void EditorSettings::ReloadForUndo() {Get()=EditorSettings{};Load();}

namespace {
// Per-user, per-machine state (#42) — NOT under the project folder, so two teammates (or two
// machines) on the same project stop fighting over UI scale/theme/layout on every commit. See
// Core/Paths.h.
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
bool g_LayoutResetRequested = false; // Load() migrated an older file: rebuild the default dock once
int g_SavedThemeId = -1; // >= 0: SetThemeForRun changed the theme for this run only; this is the one to keep

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

// Every preference that is just a value under its own key, listed once: Load() reads each with
// SafeValue (a missing or mistyped key keeps the default) and Flush() writes each back. Keys with
// their own rules (window placement, the last scene's GUID, recent scenes, the sound mastering
// reset, mute-app names, the per-run theme, scene cameras) are handled by hand around this list,
// and ranges are clamped after loading.
// Not here on purpose: "editorTheme" (the removed theme picker; an old file's key is ignored),
// exposureEV/tonemapOperator/msaaSamples/ssaoEnabled/bloom*/shadow* (#9: moved to scene data;
// SceneSerializer reads an old file's keys directly to migrate a pre-v3 scene) and
// PhysicsSimTimeScale (session-only, so a slowed or frozen session never carries over).
template <class Visit>
void ForEachPlainField(EditorSettings& s, Visit&& field) {
    field("showTooltips", s.ShowTooltips);
    field("uiScaleOverride", s.UiScaleOverride);
    field("autoSaveEnabled", s.AutoSaveEnabled);
    field("autoSaveIntervalMinutes", s.AutoSaveIntervalMinutes);
    field("vsyncMode", s.VSyncMode);
    field("fpsLimit", s.FpsLimit);
    field("unfocusedFpsLimit", s.UnfocusedFpsLimit);
    field("renderHeight", s.RenderHeight);
    field("renderSharpness", s.RenderSharpness);
    field("fullscreenMode", s.FullscreenMode);
    field("fullscreenMonitor", s.FullscreenMonitor);
    field("gridOpacity", s.GridOpacity);
    field("gridMinorSpacing", s.GridMinorSpacing);
    field("gridMajorEvery", s.GridMajorEvery);
    field("gridFadeDistance", s.GridFadeDistance);
    field("gridShowAxisLines", s.GridShowAxisLines);
    field("gridAxisThickness", s.GridAxisThickness);
    field("lastScenePath", s.LastScenePath);
    field("gameViewMaximizeOnPlay", s.GameViewMaximizeOnPlay);
    field("playTransitionGlide", s.PlayTransitionGlide);
    field("gameViewShowStats", s.GameViewShowStats);
    field("sceneShowStats", s.SceneShowStats);
    field("sceneCameraFov", s.SceneCameraFov);
    field("sceneCameraFlySpeed", s.SceneCameraFlySpeed);
    field("sceneCameraNear", s.SceneCameraNear);
    field("sceneCameraFar", s.SceneCameraFar);
    field("audioMuted", s.AudioMuted);
    field("editorSoundsEnabled", s.EditorSoundsEnabled);
    field("editorSoundsVolume", s.EditorSoundsVolume);
    field("editorSoundsPremium", s.EditorSoundsPremium);
    field("editorSoundCategoryOn", s.EditorSoundCategoryOn);
    field("editorSoundCategoryVolume", s.EditorSoundCategoryVolume);
    field("muteAppsWhileMoving", s.MuteAppsWhileMoving);
    field("assetSearchGlobal", s.AssetSearchGlobal);
    field("gameViewPresetLabel", s.GameViewPresetLabel);
    field("gameViewPresetWidth", s.GameViewPresetWidth);
    field("gameViewPresetHeight", s.GameViewPresetHeight);
    field("assetBrowserTreeWidth", s.AssetBrowserTreeWidth);
    field("assetBrowserIconSize", s.AssetBrowserIconSize);
    field("assetSortMode", s.AssetSortMode);
    field("assetSortDesc", s.AssetSortDesc);
    field("assetDetailsMode", s.AssetDetailsMode);
    field("hierarchySortMode", s.HierarchySortMode);
    field("hierarchySortDesc", s.HierarchySortDesc);
    field("hierarchyTypeFilterMask", s.HierarchyTypeFilterMask);
    field("reduceMotion", s.ReduceMotion);
    field("motionSpeed", s.MotionSpeed);
    field("motionThemeTweens", s.MotionThemeTweens);
    field("motionInertia", s.MotionInertia);
    field("motionGradients", s.MotionGradients);
    field("motionRollingNumbers", s.MotionRollingNumbers);
    field("motionViewport", s.MotionViewport);
    field("motionStartup", s.MotionStartup);
    field("glassViewportHuds", s.GlassViewportHuds);
    field("glassFrostedBlur", s.GlassFrostedBlur);
    field("glassStatsPill", s.GlassStatsPill);
    field("glassSeeThroughWindow", s.GlassSeeThroughWindow);
    field("showGlassHierarchy", s.ShowGlassHierarchy);
    field("showGlassInspector", s.ShowGlassInspector);
    field("showGlassAssets", s.ShowGlassAssets);
    field("defaultLayoutPreset", s.DefaultLayoutPreset);
    field("showLightGizmos", s.ShowLightGizmos);
    field("lightGizmoSelectedOnly", s.LightGizmoSelectedOnly);
    field("lightGizmoOpacity", s.LightGizmoOpacity);
    field("lightGizmoScale", s.LightGizmoScale);
    field("showColliders", s.ShowColliders);
    field("collidersSelectedOnly", s.CollidersSelectedOnly);
    field("showDecalGizmos", s.ShowDecalGizmos);
    field("showProbeGizmos", s.ShowProbeGizmos);
    field("showVolumeGizmos", s.ShowVolumeGizmos);
    field("volumeGizmosSelectedOnly", s.VolumeGizmosSelectedOnly);
    field("entityIconsMode", s.EntityIconsMode);
    field("entityIconsThroughWalls", s.EntityIconsThroughWalls);
    field("showBodyDebug", s.ShowBodyDebug);
    field("decalFavorites", s.DecalFavorites);
    field("decalRecent", s.DecalRecent);
    field("decalPalette", s.DecalPalette);
    field("decalSourceCategory", s.DecalSourceCategory);
    field("decalLibraryTile", s.DecalLibraryTile);
    field("decalBrushRadius", s.DecalBrushRadius);
    field("decalBrushSpacing", s.DecalBrushSpacing);
    field("decalBrushDensity", s.DecalBrushDensity);
    field("decalBrushSize", s.DecalBrushSize);
    field("decalBrushSizeJitter", s.DecalBrushSizeJitter);
    field("decalBrushRotationJitter", s.DecalBrushRotationJitter);
    field("decalBrushOpacity", s.DecalBrushOpacity);
    field("decalBrushAlign", s.DecalBrushAlign);
    field("decalBrushFollowStroke", s.DecalBrushFollowStroke);
    field("decalGrimeSpacing", s.DecalGrimeSpacing);
    field("decalGrimeDensity", s.DecalGrimeDensity);
    field("decalGrimeMax", s.DecalGrimeMax);
    field("physicsDebugInput", s.PhysicsDebugInput);
    field("showPhysicsPanel", s.ShowPhysicsPanel);
    field("showAssetLibrary", s.ShowAssetLibrary);
    field("assetLibraryPath", s.AssetLibraryPath);
    field("physicsHudOverlay", s.PhysicsHudOverlay);
    field("physicsDebugDrawFlags", s.PhysicsDebugDrawFlags);
    field("logPhysicsEvents", s.LogPhysicsEvents);
    field("playDebugOverlay", s.PlayDebugOverlay);
    field("fRowOverlay", s.FRowOverlay);
    field("fRowLightDebug", s.FRowLightDebug);
    field("postFxOff", s.PostFxOff);
    field("fRowLook", s.FRowLook);
    field("fRowStyle", s.FRowStyle);
    field("fRowOutfitSeed", s.FRowOutfitSeed);
    field("layerVisibleMask", s.LayerVisibleMask);
    field("layerPickLockMask", s.LayerPickLockMask);
    field("viewShowGrid", s.ViewShowGrid);
    field("viewShowGizmo", s.ViewShowGizmo);
    field("viewFrameOnSelect", s.ViewFrameOnSelect);
    field("gizmoSize", s.GizmoSize);
    field("vertexPickPixels", s.VertexPickPixels);
    field("snapEnabled", s.SnapEnabled);
    field("snapTranslation", s.SnapTranslation);
    field("snapRotationDeg", s.SnapRotationDeg);
    field("snapScale", s.SnapScale);
    field("gizmoLocalSpace", s.GizmoLocalSpace);
    field("gizmoPivotCenter", s.GizmoPivotCenter);
    field("activeTool", s.ActiveTool);
    field("shadingMode", s.ShadingMode);
    field("enhancerHoverKeys", s.EnhancerHoverKeys);
    field("hierarchyRowStyles", s.HierarchyRowStyles);
    field("hierarchyTreeLines", s.HierarchyTreeLines);
    field("hierarchyMinimal", s.HierarchyMinimal);
    field("hierarchyZebra", s.HierarchyZebra);
    field("hierarchyMinimap", s.HierarchyMinimap);
    field("hierarchyMinimapMax", s.HierarchyMinimapMax);
    field("hierarchyMinimapWhen", s.HierarchyMinimapWhen);
    field("hierarchyNavBar", s.HierarchyNavBar);
    field("folderStyles", s.FolderStyles);
    field("folderTreeLines", s.FolderTreeLines);
    field("folderRowWash", s.FolderRowWash);
    field("folderZebra", s.FolderZebra);
    field("folderMinimal", s.FolderMinimal);
    field("folderMinimap", s.FolderMinimap);
    field("folderNavBar", s.FolderNavBar);
    field("inspectorNavBar", s.InspectorNavBar);
    field("bookmarkChips", s.BookmarkChips);
    field("inspectorAnimations", s.InspectorAnimations);
    field("inspectorMinimal", s.InspectorMinimal);
    field("inspectorTabs", s.InspectorTabs);
    field("assetTabs", s.AssetTabs);
    field("panelTabColors", s.PanelTabColors);
    field("componentHeaderColors", s.ComponentHeaderColors);
    field("favorites", s.Favorites);
    field("favoritesHoldAlt", s.FavoritesHoldAlt);
    field("ruler", s.Ruler);
    field("measureFeet", s.MeasureFeet);
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
    ForEachPlainField(s, [&](const char* key, auto& value) { value = SafeValue(root, key, value); });
    if (auto wp = root.find("windowPlacement"); wp != root.end() && wp->is_object()) {
        s.WindowX = SafeValue(*wp, "x", 0);
        s.WindowY = SafeValue(*wp, "y", 0);
        s.WindowWidth = SafeValue(*wp, "width", 0);
        s.WindowHeight = SafeValue(*wp, "height", 0);
        s.WindowMaximized = SafeValue(*wp, "maximized", true);
        s.WindowPlacementValid = s.WindowWidth > 0 && s.WindowHeight > 0;
    }
    {
        // #132 - the last scene was renamed or moved since: follow its GUID. Only looked up when
        // the path is gone, so the common case never waits on the background asset scan.
        std::error_code ec;
        const std::string guid = SafeValue(root, "lastSceneGuid", std::string());
        if (!guid.empty() && (s.LastScenePath.empty() || !std::filesystem::exists(s.LastScenePath, ec))) {
            const std::string moved = AssetDatabase::PathForGuid(AssetGuid::FromString(guid));
            if (!moved.empty() && std::filesystem::exists(moved, ec)) s.LastScenePath = moved;
        }
    }
    s.RecentScenes.clear();
    if (auto it = root.find("recentScenes"); it != root.end() && it->is_array())
        for (const auto& p : *it) if (p.is_string() && !p.get<std::string>().empty()) s.RecentScenes.push_back(p.get<std::string>());
    // The P6 remaster made every file about 10 dB louder, so a volume set against the old files (most likely turned all the
    // way up because they were too quiet) would now be far too loud: it starts again from the new default, once.
    s.EditorSoundsMastering = SafeValue(root, "editorSoundsMastering", 1);
    if (s.EditorSoundsMastering < 2) {
        s.EditorSoundsVolume = 0.70f; // the default above
        s.EditorSoundsMastering = 2;
    }
    if (auto it = root.find("muteAppNames"); it != root.end() && it->is_array()) {
        s.MuteAppNames.clear();
        for (const auto& n : *it) if (n.is_string() && !n.get<std::string>().empty()) s.MuteAppNames.push_back(n.get<std::string>());
    }
    // "glassTheme", not the retired "editorTheme": old prefs files still carry that key with
    // values from the removed theme picker, and must not switch Glass on by accident.
    s.EditorThemeId = std::clamp(SafeValue(root, "glassTheme", s.EditorThemeId), kTartarusTheme, kGlassTheme);
    s.SceneCameras.clear();
    if (auto it = root.find("sceneCameras"); it != root.end() && it->is_array()) {
        for (const auto& c : *it) {
            if (!c.is_object()) continue;
            SceneCameraPose pose;
            pose.ScenePath = SafeValue(c, "scene", std::string());
            if (pose.ScenePath.empty()) continue;
            pose.X = SafeValue(c, "x", pose.X);
            pose.Y = SafeValue(c, "y", pose.Y);
            pose.Z = SafeValue(c, "z", pose.Z);
            pose.Yaw = SafeValue(c, "yaw", pose.Yaw);
            pose.Pitch = std::clamp(SafeValue(c, "pitch", pose.Pitch), -89.0f, 89.0f);
            pose.FlySpeed = std::max(0.01f, SafeValue(c, "flySpeed", pose.FlySpeed));
            if ((int)s.SceneCameras.size() < kMaxSceneCameras) s.SceneCameras.push_back(pose);
        }
    }

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
    // #135 — same ranges as the Preferences / toolbar sliders.
    clampF(s.GizmoSize, 0.05f, 0.40f, 0.10f);
    clampF(s.VertexPickPixels, 5.0f, 150.0f, 35.0f);
    clampF(s.SnapTranslation, 0.001f, 100.0f, 1.0f);
    clampF(s.SnapRotationDeg, 0.1f, 180.0f, 15.0f);
    clampF(s.SnapScale, 0.001f, 10.0f, 0.1f);
    s.ActiveTool = std::clamp(s.ActiveTool, 0, 4);   // GizmoOp::Translate..Universal
    s.ShadingMode = std::clamp(s.ShadingMode, 0, 5); // EditorLayer::ShadingMode::Shaded..Mip
    s.RenderHeight = std::clamp(s.RenderHeight, 0, 4320);
    s.RenderSharpness = std::clamp(s.RenderSharpness, 0.0f, 1.0f);
    s.FullscreenMode = std::clamp(s.FullscreenMode, 0, 1);
    s.FullscreenMonitor = std::clamp(s.FullscreenMonitor, -1, 15);
    s.MotionSpeed = std::clamp(s.MotionSpeed, 0.5f, 2.0f);
    s.EntityIconsMode = std::clamp(s.EntityIconsMode, 0, 2);
    s.DecalLibraryTile = std::clamp(s.DecalLibraryTile, 48.0f, 200.0f);
    s.HierarchyMinimapMax = std::clamp(s.HierarchyMinimapMax, 1, 12);
    s.HierarchyMinimapWhen = std::clamp(s.HierarchyMinimapWhen, 0, 1);

    // A file from before kPrefsVersion (no "prefsVersion" key = v1): move it forward once and
    // write it straight back, so the migration never runs twice.
    const int fileVersion = SafeValue(root, "prefsVersion", 1);
    if (fileVersion < kPrefsVersion) {
        g_LayoutResetRequested = MigratePrefs(s, fileVersion);
        Log::Info("EditorSettings: prefs moved to v" + std::to_string(kPrefsVersion) + " (clean view defaults).");
        Save();
    }
}

void EditorSettings::Save() {
    // Coalesced: the real write happens in Flush(), called once per editor frame and on
    // shutdown. Worst case a change made this frame is lost to a crash before the frame ends;
    // that beats dozens of non-atomic full rewrites during a slider drag / toggle spree.
    g_PrefsDirty = true;
}

void EditorSettings::ResetViewDefaults() {
    const EditorSettings d;
    EditorThemeId = d.EditorThemeId;
    GlassViewportHuds = d.GlassViewportHuds;
    GlassStatsPill = d.GlassStatsPill;
    SceneShowStats = d.SceneShowStats;
    ShowLightGizmos = d.ShowLightGizmos;
    LightGizmoSelectedOnly = d.LightGizmoSelectedOnly;
    LightGizmoOpacity = d.LightGizmoOpacity;
    LightGizmoScale = d.LightGizmoScale;
    ShowColliders = d.ShowColliders;
    CollidersSelectedOnly = d.CollidersSelectedOnly;
    ShowDecalGizmos = d.ShowDecalGizmos;
    ShowProbeGizmos = d.ShowProbeGizmos;
    ShowVolumeGizmos = d.ShowVolumeGizmos;
    VolumeGizmosSelectedOnly = d.VolumeGizmosSelectedOnly;
    EntityIconsMode = d.EntityIconsMode;
    EntityIconsThroughWalls = d.EntityIconsThroughWalls;
    ShowBodyDebug = d.ShowBodyDebug;
    PhysicsDebugDrawFlags = d.PhysicsDebugDrawFlags;
    PlayDebugOverlay = d.PlayDebugOverlay;
    FRowOverlay = d.FRowOverlay;
    FRowLightDebug = d.FRowLightDebug;
    PostFxOff = d.PostFxOff;
    FRowLook = d.FRowLook;
    FRowStyle = d.FRowStyle;
    FRowOutfitSeed = d.FRowOutfitSeed;
    LayerVisibleMask = d.LayerVisibleMask;
    ViewShowGrid = d.ViewShowGrid;
    ViewShowGizmo = d.ViewShowGizmo;
    GridOpacity = d.GridOpacity;
    GridFadeDistance = d.GridFadeDistance;
    GridShowAxisLines = d.GridShowAxisLines;
}

bool EditorSettings::MigratePrefs(EditorSettings& s, int from) {
    bool resetLayout = false;
    if (from < 2) {
        // v2: the clean first launch. Every overlay/theme default changed, so the old file's
        // choices for them (mostly never touched, just written back) give way to the new ones.
        s.ResetViewDefaults();
        resetLayout = true; // the old dock focused Statistics; rebuild the default arrangement
    }
    if (from < 3 && std::fabs(s.GizmoSize - 0.15f) < 1e-4f) s.GizmoSize = EditorSettings{}.GizmoSize; // a size you set is kept
    s.PrefsVersion = kPrefsVersion;
    return resetLayout;
}

bool EditorSettings::TakeLayoutResetRequest() {
    const bool r = g_LayoutResetRequested;
    g_LayoutResetRequested = false;
    return r;
}

const EditorSettings::SceneCameraPose* EditorSettings::FindSceneCamera(const std::string& scenePath) const {
    for (const SceneCameraPose& p : SceneCameras)
        if (p.ScenePath == scenePath) return &p;
    return nullptr;
}

void EditorSettings::RememberSceneCamera(const SceneCameraPose& pose) {
    if (pose.ScenePath.empty()) return;
    SceneCameras.erase(std::remove_if(SceneCameras.begin(), SceneCameras.end(),
                                      [&](const SceneCameraPose& p) { return p.ScenePath == pose.ScenePath; }),
                       SceneCameras.end());
    SceneCameras.insert(SceneCameras.begin(), pose);
    if ((int)SceneCameras.size() > kMaxSceneCameras) SceneCameras.resize(kMaxSceneCameras);
}

void EditorSettings::SetThemeForRun(int themeId) {
    if (g_SavedThemeId < 0) g_SavedThemeId = Get().EditorThemeId;
    Get().EditorThemeId = themeId;
}

void EditorSettings::Flush() {
    if (!g_PrefsDirty) return;
    g_PrefsDirty = false;

    json root;
    root["prefsVersion"] = kPrefsVersion;
    ForEachPlainField(Get(), [&](const char* key, const auto& value) { root[key] = value; });
    if (Get().WindowPlacementValid) {
        root["windowPlacement"] = {{"x", Get().WindowX}, {"y", Get().WindowY},
                                   {"width", Get().WindowWidth}, {"height", Get().WindowHeight},
                                   {"maximized", Get().WindowMaximized}};
    }
    {
        auto& recent = Get().RecentScenes;
        if (!Get().LastScenePath.empty()) {
            const std::string& last = Get().LastScenePath;
            recent.erase(std::remove(recent.begin(), recent.end(), last), recent.end());
            recent.insert(recent.begin(), last);
        }
        constexpr size_t kMaxRecent = 8;
        if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
        root["recentScenes"] = recent;
    }
    if (!Get().LastScenePath.empty()) { // #132
        const AssetGuid g = AssetDatabase::GuidForPath(Get().LastScenePath);
        if (g.IsValid()) root["lastSceneGuid"] = g.ToString();
    }
    root["editorSoundsMastering"] = Get().EditorSoundsMastering;
    root["muteAppNames"] = Get().MuteAppNames;
    root["glassTheme"] = g_SavedThemeId >= 0 ? g_SavedThemeId : Get().EditorThemeId;
    {
        json cams = json::array();
        for (const SceneCameraPose& p : Get().SceneCameras)
            cams.push_back({{"scene", p.ScenePath}, {"x", p.X}, {"y", p.Y}, {"z", p.Z},
                            {"yaw", p.Yaw}, {"pitch", p.Pitch}, {"flySpeed", p.FlySpeed}});
        root["sceneCameras"] = std::move(cams);
    }

    // Atomic: a crash mid-write (a toggle spree can still trigger one write) must leave the
    // previous editor_prefs.json intact, not truncated (audit CPP-206). Lives under UserPaths
    // now, not project/ (#42).
    if (!AtomicFile::WriteJson(PrefsPath(), root))
        Log::Warn(std::string("EditorSettings: failed to write '") + PrefsPath() + "'.");
}
