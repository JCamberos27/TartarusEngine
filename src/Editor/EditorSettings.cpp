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
    s.ShowTooltips = root.value("showTooltips", s.ShowTooltips);
    s.AutoSaveEnabled = root.value("autoSaveEnabled", s.AutoSaveEnabled);
    s.AutoSaveIntervalMinutes = root.value("autoSaveIntervalMinutes", s.AutoSaveIntervalMinutes);
    s.VSyncMode = root.value("vsyncMode", s.VSyncMode);
    s.FpsLimit = root.value("fpsLimit", s.FpsLimit);
    s.ExposureEV = root.value("exposureEV", s.ExposureEV);
    s.TonemapOperator = root.value("tonemapOperator", s.TonemapOperator);
    s.MsaaSamples = root.value("msaaSamples", s.MsaaSamples);
    s.ShadowsEnabled = root.value("shadowsEnabled", s.ShadowsEnabled);
    s.ShadowResolution = root.value("shadowResolution", s.ShadowResolution);
    s.ShadowDistance = root.value("shadowDistance", s.ShadowDistance);
    s.GameViewMaximizeOnPlay = root.value("gameViewMaximizeOnPlay", s.GameViewMaximizeOnPlay);
    s.GameViewShowStats = root.value("gameViewShowStats", s.GameViewShowStats);
    s.SceneShowStats = root.value("sceneShowStats", s.SceneShowStats);
    s.GameViewPresetLabel = root.value("gameViewPresetLabel", s.GameViewPresetLabel);
    s.GameViewPresetWidth = root.value("gameViewPresetWidth", s.GameViewPresetWidth);
    s.GameViewPresetHeight = root.value("gameViewPresetHeight", s.GameViewPresetHeight);
    s.AssetBrowserTreeWidth = root.value("assetBrowserTreeWidth", s.AssetBrowserTreeWidth);
    s.AssetBrowserIconSize = root.value("assetBrowserIconSize", s.AssetBrowserIconSize);
}

void EditorSettings::Save() {
    std::ofstream out(PrefsPath());
    if (!out.is_open()) {
        Log::Warn(std::string("EditorSettings: failed to open '") + PrefsPath() + "' for writing.");
        return;
    }

    json root;
    root["showTooltips"] = Get().ShowTooltips;
    root["autoSaveEnabled"] = Get().AutoSaveEnabled;
    root["autoSaveIntervalMinutes"] = Get().AutoSaveIntervalMinutes;
    root["vsyncMode"] = Get().VSyncMode;
    root["fpsLimit"] = Get().FpsLimit;
    root["exposureEV"] = Get().ExposureEV;
    root["tonemapOperator"] = Get().TonemapOperator;
    root["msaaSamples"] = Get().MsaaSamples;
    root["shadowsEnabled"] = Get().ShadowsEnabled;
    root["shadowResolution"] = Get().ShadowResolution;
    root["shadowDistance"] = Get().ShadowDistance;
    root["gameViewMaximizeOnPlay"] = Get().GameViewMaximizeOnPlay;
    root["gameViewShowStats"] = Get().GameViewShowStats;
    root["sceneShowStats"] = Get().SceneShowStats;
    root["gameViewPresetLabel"] = Get().GameViewPresetLabel;
    root["gameViewPresetWidth"] = Get().GameViewPresetWidth;
    root["gameViewPresetHeight"] = Get().GameViewPresetHeight;
    root["assetBrowserTreeWidth"] = Get().AssetBrowserTreeWidth;
    root["assetBrowserIconSize"] = Get().AssetBrowserIconSize;
    out << root.dump(2);
}
