#include "ProjectSettings.h"
#include "Log.h"
#include "ProjectPaths.h"

#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>

using json = nlohmann::json;

namespace ProjectSettings {
namespace {

PhysicsSettings g_Physics;
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
        g_Physics.FixedTimestep    = p.value("fixedTimestep", g_Physics.FixedTimestep);
        g_Physics.SolverIterations = p.value("solverIterations", g_Physics.SolverIterations);
        if (const auto m = p.find("layerCollision"); m != p.end() && m->is_array()) { // #185 PR 8
            for (int i = 0; i < 8 && i < (int)m->size(); ++i)
                if ((*m)[i].is_number_unsigned() || (*m)[i].is_number_integer())
                    g_Physics.LayerCollisionMask[i] = (*m)[i].get<unsigned>() & 0xFFu;
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
        {"layerCollision", json::array({g_Physics.LayerCollisionMask[0], g_Physics.LayerCollisionMask[1],
                                        g_Physics.LayerCollisionMask[2], g_Physics.LayerCollisionMask[3],
                                        g_Physics.LayerCollisionMask[4], g_Physics.LayerCollisionMask[5],
                                        g_Physics.LayerCollisionMask[6], g_Physics.LayerCollisionMask[7]})},
    };
    root["tags"] = g_Tags;

    std::ofstream out(SettingsPath());
    if (!out.is_open()) {
        Log::Warn(std::string("ProjectSettings: could not open '") + SettingsPath() + "' for writing");
        return;
    }
    out << root.dump(2) << '\n';
}

} // namespace ProjectSettings
