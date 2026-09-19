#include "PlayerConfig.h"
#include "AtomicFile.h"
#include "Log.h"

#include <json.hpp>

#include <algorithm>
#include <fstream>

using json = nlohmann::json;

bool PlayerConfig::Load(const std::string& path, PlayerConfig& out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        Log::Error(std::string("player.json is not valid JSON: ") + e.what());
        return false;
    }
    if (!j.is_object()) return false;

    PlayerConfig c;
    auto str = [&j](const char* key, const std::string& fallback) {
        const auto f = j.find(key);
        return (f != j.end() && f->is_string()) ? f->get<std::string>() : fallback;
    };
    auto integer = [&j](const char* key, int fallback) {
        const auto f = j.find(key);
        return (f != j.end() && f->is_number()) ? f->get<int>() : fallback;
    };
    auto boolean = [&j](const char* key, bool fallback) {
        const auto f = j.find(key);
        return (f != j.end() && f->is_boolean()) ? f->get<bool>() : fallback;
    };
    c.ProductName = str("productName", c.ProductName);
    c.CompanyName = str("companyName", c.CompanyName);
    c.Version     = str("version", c.Version);
    c.Width       = std::clamp(integer("width", c.Width), 320, 16384);
    c.Height      = std::clamp(integer("height", c.Height), 200, 16384);
    c.Fullscreen  = boolean("fullscreen", c.Fullscreen);
    c.VSync       = boolean("vsync", c.VSync);
    c.DevelopmentBuild = boolean("developmentBuild", c.DevelopmentBuild);
    if (const auto s = j.find("scenes"); s != j.end() && s->is_array())
        for (const auto& sc : *s)
            if (sc.is_string() && !sc.get<std::string>().empty()) c.Scenes.push_back(sc.get<std::string>());
    out = std::move(c);
    return true;
}

bool PlayerConfig::Save(const std::string& path) const {
    json j = {
        {"productName", ProductName},
        {"companyName", CompanyName},
        {"version", Version},
        {"scenes", Scenes},
        {"width", Width},
        {"height", Height},
        {"fullscreen", Fullscreen},
        {"vsync", VSync},
        {"developmentBuild", DevelopmentBuild},
    };
    return AtomicFile::WriteJson(path, j);
}
