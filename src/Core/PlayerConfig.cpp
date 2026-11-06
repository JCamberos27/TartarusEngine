#include "PlayerConfigJson.h"
#include "AtomicFile.h"
#include "Log.h"

#include <algorithm>
#include <cctype>
#include <fstream>

using json = nlohmann::json;

void BuildFieldsFromJson(const json& j, PlayerBuildFields& out) {
    auto str = [&j](const char* key, std::string& v) {
        if (const auto f = j.find(key); f != j.end() && f->is_string()) v = f->get<std::string>();
    };
    auto integer = [&j](const char* key, int& v, int lo, int hi) {
        if (const auto f = j.find(key); f != j.end() && f->is_number()) v = f->get<int>();
        v = std::clamp(v, lo, hi);
    };
    auto boolean = [&j](const char* key, bool& v) {
        if (const auto f = j.find(key); f != j.end() && f->is_boolean()) v = f->get<bool>();
    };
    str("productName", out.ProductName);
    integer("width", out.Width, 320, 16384);
    integer("height", out.Height, 200, 16384);
    boolean("fullscreen", out.Fullscreen);
    boolean("vsync", out.VSync);
    boolean("developmentBuild", out.DevelopmentBuild);
    str("iconPath", out.IconPath);
    str("splashPath", out.SplashPath);
    if (const auto s = j.find("scenes"); s != j.end() && s->is_array()) {
        out.Scenes.clear();
        for (const auto& sc : *s)
            if (sc.is_string() && !sc.get<std::string>().empty()) out.Scenes.push_back(sc.get<std::string>());
    }
}

void BuildFieldsToJson(const PlayerBuildFields& in, json& j) {
    j["productName"] = in.ProductName;
    j["scenes"] = in.Scenes;
    j["width"] = in.Width;
    j["height"] = in.Height;
    j["fullscreen"] = in.Fullscreen;
    j["vsync"] = in.VSync;
    j["developmentBuild"] = in.DevelopmentBuild;
    j["iconPath"] = in.IconPath;
    j["splashPath"] = in.SplashPath;
}

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
    BuildFieldsFromJson(j, c);
    out = std::move(c);
    return true;
}

bool PlayerConfig::Save(const std::string& path) const {
    json j = json::object();
    BuildFieldsToJson(*this, j);
    return AtomicFile::WriteJson(path, j);
}

std::string PlayerConfig::UserFolderName() const {
    std::string name;
    for (char c : ProductName)
        name += (std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') ? c : '_';
    return name.empty() ? std::string("TartarusGame") : name;
}
