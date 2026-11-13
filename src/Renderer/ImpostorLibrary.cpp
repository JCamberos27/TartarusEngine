#include "ImpostorLibrary.h"

#include "Core/Env.h"
#include "Core/Paths.h"
#include "Core/ProjectSettings.h"
#include "Model.h"

#include <json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace {

constexpr float kDefaultStartDistance = 40.0f; // Jacob's pick (2026-11-12): the probe's 40 m cut

struct State {
    bool Loaded = false;
    std::vector<ImpostorSet> Sets;
    float StartDistance = kDefaultStartDistance;
    std::map<std::vector<std::string>, int> ByModels;  // the sorted LOD 0 paths -> set
    std::map<std::vector<const Model*>, int> ByPointer; // Match's cache
    int TestSet = -1;
    float TestStart = 0.0f;
};

State& S() {
    static State s;
    return s;
}

void Load() {
    State& s = S();
    if (s.Loaded) return;
    s.Loaded = true;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::path(ProjectPaths::Resolve("assets/Impostors"));
    if (!fs::is_directory(root, ec)) return;
    for (const fs::directory_entry& dir : fs::directory_iterator(root, ec)) {
        const fs::path manifest = dir.path() / "impostors.json";
        if (!dir.is_directory(ec) || !fs::exists(manifest, ec)) continue;
        nlohmann::json j;
        try {
            std::ifstream f(manifest);
            f >> j;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[Impostors] %s: %s\n", ProjectPaths::Relativize(manifest.string()).c_str(), e.what());
            continue;
        }
        const std::string folder = ProjectPaths::Relativize(dir.path().string()) + "/";
        const int frames = j.value("frames", 8);
        if (j.contains("startDistance")) s.StartDistance = j["startDistance"].get<float>();
        if (!j.contains("impostors") || !j["impostors"].is_object()) continue;
        for (const auto& [name, v] : j["impostors"].items()) {
            ImpostorSet set;
            set.Name = name;
            set.Frames = frames;
            for (const auto& m : v.value("lod0Models", nlohmann::json::array())) set.Models.push_back(m.get<std::string>());
            std::sort(set.Models.begin(), set.Models.end());
            const auto c = v.value("centre", std::vector<float>{0.0f, 0.0f, 0.0f});
            if (c.size() == 3) set.Centre = glm::vec3(c[0], c[1], c[2]);
            set.Radius = v.value("radius", 0.0f);
            const auto lo = v.value("boundsMin", std::vector<float>{}), hi = v.value("boundsMax", std::vector<float>{});
            if (lo.size() == 3 && hi.size() == 3) {
                set.BoundsMin = glm::vec3(lo[0], lo[1], lo[2]);
                set.BoundsMax = glm::vec3(hi[0], hi[1], hi[2]);
            } else { // an older bake: the sphere's box
                set.BoundsMin = set.Centre - glm::vec3(set.Radius);
                set.BoundsMax = set.Centre + glm::vec3(set.Radius);
            }
            set.WindBend = v.value("windBend", 0.0f);
            set.WindHeight = std::max(v.value("windHeight", 1.0f), 0.01f);
            set.Albedo = folder + v.value("albedo", std::string());
            set.NormalDepth = folder + v.value("normalDepth", std::string());
            set.Mask = folder + v.value("mask", std::string());
            if (set.Models.empty() || !(set.Radius > 0.0f)) continue;
            // A set whose atlases aren't fetched (Drive assets missing) draws nothing; leave its trees on their LODs.
            if (!fs::exists(ProjectPaths::Resolve(set.Albedo), ec) || !fs::exists(ProjectPaths::Resolve(set.NormalDepth), ec) ||
                !fs::exists(ProjectPaths::Resolve(set.Mask), ec))
                continue;
            s.ByModels[set.Models] = (int)s.Sets.size();
            s.Sets.push_back(std::move(set));
        }
    }
}

} // namespace

namespace ImpostorLibrary {

float StartDistance() {
    if (S().TestSet >= 0) return S().TestStart;
    Load();
    static const float over = Env::Float(Env::Var::ImpostorDistance, -1.0f);
    return over > 0.0f ? over : S().StartDistance;
}

bool Available() {
    if (S().TestSet >= 0) return true;
    static const bool off = Env::Flag(Env::Var::NoImpostors);
    if (off) return false;
    Load();
    return !S().Sets.empty();
}

bool Enabled() {
    return Available() && (S().TestSet >= 0 || ProjectSettings::Quality().Impostors);
}

const std::vector<ImpostorSet>& Sets() {
    Load();
    return S().Sets;
}

int Match(const std::vector<const Model*>& lod0) {
    if (S().TestSet >= 0) return S().TestSet;
    if (lod0.empty() || !Available()) return -1;
    State& s = S();
    std::vector<const Model*> key = lod0;
    std::sort(key.begin(), key.end());
    if (const auto it = s.ByPointer.find(key); it != s.ByPointer.end()) return it->second;
    std::vector<std::string> paths;
    paths.reserve(key.size());
    for (const Model* m : key) paths.push_back(ProjectPaths::Relativize(m->Path()));
    std::sort(paths.begin(), paths.end());
    const auto found = s.ByModels.find(paths);
    const int set = found == s.ByModels.end() ? -1 : found->second;
    s.ByPointer.emplace(std::move(key), set);
    return set;
}

void Reload() {
    S() = State{};
}

void TestOverride(int set, float startDistance) {
    S().TestSet = set;
    S().TestStart = startDistance;
}

} // namespace ImpostorLibrary
