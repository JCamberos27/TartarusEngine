#include "EditorSettings.h"
#include "Log.h"
#include "ProjectPaths.h"

#include <json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace {
// Resolved under the project folder rather than the working directory, so preferences live
// alongside the source instead of inside build/ (see ProjectPaths.h).
const std::string& PrefsPath() {
    static const std::string path = ProjectPaths::Resolve("editor_prefs.json");
    return path;
}
}

void EditorSettings::Load() {
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
    s.EditorTheme = root.value("editorTheme", s.EditorTheme);
    // Themes were renumbered when Dark Slate was dropped (#234): 0 Bento, 1 Prism, 2 Windows XP.
    // An out-of-range value from an older prefs file (e.g. the interim "3 = Bento") lands on the
    // new default, Bento.
    if (s.EditorTheme < 0 || s.EditorTheme > 2) s.EditorTheme = 0;
    s.ShowTooltips = root.value("showTooltips", s.ShowTooltips);
    s.UiScaleOverride = root.value("uiScaleOverride", s.UiScaleOverride);
    s.AutoSaveEnabled = root.value("autoSaveEnabled", s.AutoSaveEnabled);
    s.AutoSaveIntervalMinutes = root.value("autoSaveIntervalMinutes", s.AutoSaveIntervalMinutes);
    s.VSyncMode = root.value("vsyncMode", s.VSyncMode);
    s.FpsLimit = root.value("fpsLimit", s.FpsLimit);
    s.ExposureEV = root.value("exposureEV", s.ExposureEV);
    s.TonemapOperator = root.value("tonemapOperator", s.TonemapOperator);
    s.MsaaSamples = root.value("msaaSamples", s.MsaaSamples);
    s.ShadowsEnabled = root.value("shadowsEnabled", s.ShadowsEnabled);
    s.ShadowResolution = root.value("shadowResolution", s.ShadowResolution);
    s.ShadowCascades = root.value("shadowCascades", s.ShadowCascades);
    s.ShadowDistance = root.value("shadowDistance", s.ShadowDistance);
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
    s.EngineMarkEnabled = root.value("engineMarkEnabled", s.EngineMarkEnabled);
    s.EngineMarkSpinSpeed = root.value("engineMarkSpinSpeed", s.EngineMarkSpinSpeed);
    s.EngineMarkPrism = root.value("engineMarkPrism", s.EngineMarkPrism);
    s.ShowLightGizmos = root.value("showLightGizmos", s.ShowLightGizmos);
    s.LightGizmoSelectedOnly = root.value("lightGizmoSelectedOnly", s.LightGizmoSelectedOnly);
    s.LightGizmoOpacity = root.value("lightGizmoOpacity", s.LightGizmoOpacity);
    s.LightGizmoScale = root.value("lightGizmoScale", s.LightGizmoScale);
    s.ShowColliders = root.value("showColliders", s.ShowColliders);
    s.PhysicsDebugInput = root.value("physicsDebugInput", s.PhysicsDebugInput);
    s.ShowPhysicsPanel = root.value("showPhysicsPanel", s.ShowPhysicsPanel);
    s.PhysicsHudOverlay = root.value("physicsHudOverlay", s.PhysicsHudOverlay);
    s.PhysicsDebugDrawFlags = root.value("physicsDebugDrawFlags", s.PhysicsDebugDrawFlags);
    s.PhysicsSimTimeScale = root.value("physicsSimTimeScale", s.PhysicsSimTimeScale);
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
    std::ofstream out(PrefsPath());
    if (!out.is_open()) {
        Log::Warn(std::string("EditorSettings: failed to open '") + PrefsPath() + "' for writing.");
        return;
    }

    json root;
    root["editorTheme"] = Get().EditorTheme;
    root["showTooltips"] = Get().ShowTooltips;
    root["uiScaleOverride"] = Get().UiScaleOverride;
    root["autoSaveEnabled"] = Get().AutoSaveEnabled;
    root["autoSaveIntervalMinutes"] = Get().AutoSaveIntervalMinutes;
    root["vsyncMode"] = Get().VSyncMode;
    root["fpsLimit"] = Get().FpsLimit;
    root["exposureEV"] = Get().ExposureEV;
    root["tonemapOperator"] = Get().TonemapOperator;
    root["msaaSamples"] = Get().MsaaSamples;
    root["shadowsEnabled"] = Get().ShadowsEnabled;
    root["shadowResolution"] = Get().ShadowResolution;
    root["shadowCascades"] = Get().ShadowCascades;
    root["shadowDistance"] = Get().ShadowDistance;
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
    root["engineMarkEnabled"] = Get().EngineMarkEnabled;
    root["engineMarkSpinSpeed"] = Get().EngineMarkSpinSpeed;
    root["engineMarkPrism"] = Get().EngineMarkPrism;
    root["showLightGizmos"] = Get().ShowLightGizmos;
    root["lightGizmoSelectedOnly"] = Get().LightGizmoSelectedOnly;
    root["lightGizmoOpacity"] = Get().LightGizmoOpacity;
    root["lightGizmoScale"] = Get().LightGizmoScale;
    root["showColliders"] = Get().ShowColliders;
    root["physicsDebugInput"] = Get().PhysicsDebugInput;
    root["showPhysicsPanel"] = Get().ShowPhysicsPanel;
    root["physicsHudOverlay"] = Get().PhysicsHudOverlay;
    root["physicsDebugDrawFlags"] = Get().PhysicsDebugDrawFlags;
    root["physicsSimTimeScale"] = Get().PhysicsSimTimeScale;
    root["playDebugOverlay"] = Get().PlayDebugOverlay;
    root["layerVisibleMask"] = Get().LayerVisibleMask;
    root["layerPickLockMask"] = Get().LayerPickLockMask;
    root["captureMode"] = Get().CaptureMode;
    root["captureScale"] = Get().CaptureScale;
    root["captureResPreset"] = Get().CaptureResPreset;
    root["captureFormat"] = Get().CaptureFormat;
    root["captureFlash"] = Get().CaptureFlash;
    root["captureSound"] = Get().CaptureSound;
    out << root.dump(2);
}
