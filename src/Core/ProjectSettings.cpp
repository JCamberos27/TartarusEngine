#include "ProjectSettings.h"
#include "InputMap.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "AtomicFile.h"
#include "AssetDatabase.h" // #132 - build scenes by GUID

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace ProjectSettings {
namespace {

PhysicsSettings g_Physics;
TimeSettings g_Time;
AudioSettings g_Audio; // #171
BuildSettings g_Build; // #174
std::vector<std::string> g_Tags;

const std::string& SettingsPath() {
    static const std::string path = ProjectPaths::Resolve("settings.json");
    return path;
}

std::string SanitizeTag(const std::string& in) {
    std::string s;
    for (char c : in) if (static_cast<unsigned char>(c) >= 0x20 && c != '\x7f') s += c;
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    if (s.size() > 40) s.resize(40);
    return s;
}

glm::vec3 Vec3Or(const json& j, const char* key, const glm::vec3& fallback) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 3) return fallback;
    const json& a = *it;
    if (!a[0].is_number() || !a[1].is_number() || !a[2].is_number()) return fallback;
    return glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
}

} // namespace

const PhysicsSettings& Physics()       { return g_Physics; }
PhysicsSettings&       MutablePhysics() { return g_Physics; }
const TimeSettings&    Time()           { return g_Time; }
TimeSettings&          MutableTime()    { return g_Time; }
const AudioSettings&   Audio()          { return g_Audio; }
AudioSettings&         MutableAudio()   { return g_Audio; }
const BuildSettings&   Build()          { return g_Build; }
BuildSettings&         MutableBuild()   { return g_Build; }

const std::vector<std::string>& Tags() { return g_Tags; }

void AddTag(const std::string& name) {
    std::string t = SanitizeTag(name);
    if (t.empty()) return;
    if (std::find(g_Tags.begin(), g_Tags.end(), t) != g_Tags.end()) return;
    g_Tags.push_back(std::move(t));
}

void RemoveTag(const std::string& name) {
    g_Tags.erase(std::remove(g_Tags.begin(), g_Tags.end(), name), g_Tags.end());
}

void Load() {
    g_Physics = PhysicsSettings{};
    g_Build = BuildSettings{};
    g_Tags.clear();

    std::ifstream in(SettingsPath());
    if (!in.is_open()) return; // no file yet — defaults stand, not an error

    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        Log::Warn(std::string("ProjectSettings: failed to parse '") + SettingsPath() + "': " + e.what());
        return;
    }

    if (const auto it = root.find("physics"); it != root.end() && it->is_object()) {
        const json& p = *it;
        g_Physics.Gravity          = Vec3Or(p, "gravity", g_Physics.Gravity);
        // #152 — number-typed reads that tolerate a wrong-typed value (p.value() throws
        // json::type_error on e.g. "fixedTimestep": "0.02", which stopped the editor starting).
        auto num = [&p](const char* key, auto fallback) {
            const auto f = p.find(key);
            if (f == p.end() || !f->is_number()) {
                if (f != p.end()) Log::Warn(std::string("ProjectSettings: ignoring non-numeric '") + key + "'.");
                return fallback;
            }
            return f->get<decltype(fallback)>();
        };
        g_Physics.FixedTimestep      = std::clamp(num("fixedTimestep", g_Physics.FixedTimestep), 0.001f, 0.1f);
        g_Physics.SolverIterations   = std::clamp(num("solverIterations", g_Physics.SolverIterations), 1, 64);
        g_Physics.PlayerPushStrength = std::clamp(num("playerPushStrength", g_Physics.PlayerPushStrength), 0.0f, 50.0f);
        g_Physics.PlayerLayer        = std::clamp(num("playerLayer", g_Physics.PlayerLayer), 0, LayerRegistry::kCount - 1);
        if (const auto m = p.find("layerCollision"); m != p.end() && m->is_array()) { // #185 PR 8
            // #150: a file from the 8-layer era stores 8-bit rows; the layers it never knew about
            // (8..31) collide with everything, as new layers do by default.
            const bool legacy8 = m->size() <= 8;
            for (int i = 0; i < LayerRegistry::kCount && i < (int)m->size(); ++i)
                if ((*m)[i].is_number_unsigned() || (*m)[i].is_number_integer()) {
                    unsigned row = (*m)[i].get<unsigned>();
                    if (legacy8) row = (row & 0xFFu) | 0xFFFFFF00u;
                    g_Physics.LayerCollisionMask[i] = row;
                }
        }
    }

    if (const auto it = root.find("time"); it != root.end() && it->is_object()) { // #144
        auto num = [&it](const char* key, float fallback) {
            const auto f = it->find(key);
            if (f == it->end() || !f->is_number()) {
                if (f != it->end()) Log::Warn(std::string("ProjectSettings: ignoring non-numeric time '") + key + "'.");
                return fallback;
            }
            return f->get<float>();
        };
        g_Time.MaximumDeltaTime = std::clamp(num("maximumDeltaTime", g_Time.MaximumDeltaTime), 0.01f, 1.0f);
        g_Time.TimeScale        = std::clamp(num("timeScale", g_Time.TimeScale), 0.0f, 100.0f);
    }

    if (const auto it = root.find("audio"); it != root.end() && it->is_object()) { // #171
        g_Audio.MasterVolume = std::clamp(it->value("masterVolume", 1.0f), 0.0f, 1.0f);
        if (const auto b = it->find("busVolumes"); b != it->end() && b->is_array())
            for (int i = 0; i < 5 && i < (int)b->size(); ++i)
                if ((*b)[i].is_number()) g_Audio.BusVolume[i] = std::clamp((*b)[i].get<float>(), 0.0f, 1.0f);
    }

    // #145 - Input Manager actions. A missing or empty list means the defaults.
    {
        std::vector<InputMap::Action> actions;
        if (const auto it = root.find("input"); it != root.end()) actions = InputMap::FromJson(*it);
        InputMap::Actions() = actions.empty() ? InputMap::Defaults() : std::move(actions);
    }

    if (const auto it = root.find("build"); it != root.end() && it->is_object()) { // #174
        const json& b = *it;
        auto str = [&b](const char* key, const std::string& fallback) {
            const auto f = b.find(key);
            return (f != b.end() && f->is_string()) ? f->get<std::string>() : fallback;
        };
        auto integer = [&b](const char* key, int fallback) {
            const auto f = b.find(key);
            return (f != b.end() && f->is_number()) ? f->get<int>() : fallback;
        };
        auto boolean = [&b](const char* key, bool fallback) {
            const auto f = b.find(key);
            return (f != b.end() && f->is_boolean()) ? f->get<bool>() : fallback;
        };
        g_Build.ProductName = str("productName", g_Build.ProductName);
        g_Build.CompanyName = str("companyName", g_Build.CompanyName);
        g_Build.Version     = str("version", g_Build.Version);
        g_Build.OutputDir   = str("outputDir", g_Build.OutputDir);
        g_Build.Width       = std::clamp(integer("width", g_Build.Width), 320, 16384);
        g_Build.Height      = std::clamp(integer("height", g_Build.Height), 200, 16384);
        g_Build.Fullscreen  = boolean("fullscreen", g_Build.Fullscreen);
        g_Build.VSync       = boolean("vsync", g_Build.VSync);
        g_Build.DevelopmentBuild = boolean("developmentBuild", g_Build.DevelopmentBuild);
        const auto guids = b.find("sceneGuids"); // #132 - parallel to "scenes"
        if (const auto s = b.find("scenes"); s != b.end() && s->is_array())
            for (size_t i = 0; i < s->size(); ++i) {
                const json& sc = (*s)[i];
                if (!sc.is_string() || sc.get<std::string>().empty()) continue;
                std::string rel = sc.get<std::string>();
                // A scene renamed or moved since: follow its GUID (looked up only when the path is gone).
                std::error_code ec;
                if (guids != b.end() && guids->is_array() && i < guids->size() && (*guids)[i].is_string() &&
                    !std::filesystem::exists(ProjectPaths::Resolve(rel), ec)) {
                    const std::string moved = AssetDatabase::PathForGuid(AssetGuid::FromString((*guids)[i].get<std::string>()));
                    if (!moved.empty() && std::filesystem::exists(moved, ec)) {
                        const std::string movedRel = ProjectPaths::Relativize(moved);
                        Log::Info("Build Settings: scene '" + rel + "' moved to '" + movedRel + "'.");
                        rel = movedRel;
                    }
                }
                g_Build.Scenes.push_back(rel);
            }
    }

    if (const auto it = root.find("tags"); it != root.end() && it->is_array()) {
        for (const auto& t : *it) {
            if (t.is_string()) AddTag(t.get<std::string>());
        }
    }
}

void Save() {
    json root;
    root["physics"] = {
        {"gravity", json::array({g_Physics.Gravity.x, g_Physics.Gravity.y, g_Physics.Gravity.z})},
        {"fixedTimestep", g_Physics.FixedTimestep},
        {"solverIterations", g_Physics.SolverIterations},
        {"playerPushStrength", g_Physics.PlayerPushStrength},
        {"playerLayer", g_Physics.PlayerLayer},
        {"layerCollision", std::vector<unsigned>(std::begin(g_Physics.LayerCollisionMask), std::end(g_Physics.LayerCollisionMask))},
    };
    root["time"] = {
        {"maximumDeltaTime", g_Time.MaximumDeltaTime},
        {"timeScale", g_Time.TimeScale},
    };
    root["input"] = InputMap::ToJson(InputMap::Actions()); // #145
    root["audio"] = {
        {"masterVolume", g_Audio.MasterVolume},
        {"busVolumes", std::vector<float>(std::begin(g_Audio.BusVolume), std::end(g_Audio.BusVolume))},
    };
    root["build"] = {
        {"productName", g_Build.ProductName},
        {"companyName", g_Build.CompanyName},
        {"version", g_Build.Version},
        {"outputDir", g_Build.OutputDir},
        {"scenes", g_Build.Scenes},
        {"sceneGuids", [] {
            json a = json::array();
            for (const std::string& sc : g_Build.Scenes) {
                const AssetGuid g = AssetDatabase::GuidForPath(ProjectPaths::Resolve(sc));
                a.push_back(g.IsValid() ? g.ToString() : std::string());
            }
            return a;
        }()},
        {"width", g_Build.Width},
        {"height", g_Build.Height},
        {"fullscreen", g_Build.Fullscreen},
        {"vsync", g_Build.VSync},
        {"developmentBuild", g_Build.DevelopmentBuild},
    };
    root["tags"] = g_Tags;

    // Atomic: a crash mid-write must not truncate project settings (audit CPP-206).
    if (!AtomicFile::WriteJson(SettingsPath(), root))
        Log::Warn(std::string("ProjectSettings: could not write '") + SettingsPath() + "'");
}

} // namespace ProjectSettings
