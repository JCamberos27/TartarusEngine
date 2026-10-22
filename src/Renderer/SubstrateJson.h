#pragma once
#include "Substrate.h"
#include <json.hpp>
#include <algorithm>
#include <string>

// SubstrateDesc <-> its JSON block (Substrate.h). Templated on the JSON type (json / ordered_json)
// and on how a path is read and written: a .decal stores plain strings, a scene's embedded
// material {path, pathGuid} refs. Wrong types fall back to the defaults; never throws.
namespace SubstrateJson {

inline std::string PlainPath(const nlohmann::json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_object() && v.contains("path") && v["path"].is_string()) return v["path"].get<std::string>();
    return {};
}

// `readPath(const Json&) -> std::string`. False (out untouched) unless `j` is an object.
template <class Json, class ReadPath> bool Read(const Json& j, SubstrateDesc& out, ReadPath readPath) {
    if (!j.is_object()) return false;
    SubstrateDesc s;
    auto path = [&](const char* k) -> std::string {
        auto it = j.find(k);
        return it == j.end() || it->is_null() ? std::string() : readPath(*it);
    };
    auto num = [&](const char* k, float def) {
        auto it = j.find(k);
        return it != j.end() && it->is_number() ? it->template get<float>() : def;
    };
    auto arr = [&](const char* k, float* dst, int n) {
        auto it = j.find(k);
        if (it == j.end() || !it->is_array() || (int)it->size() < n) return;
        for (int i = 0; i < n; ++i)
            if (!(*it)[i].is_number()) return;
        for (int i = 0; i < n; ++i) dst[i] = (*it)[i].template get<float>();
    };
    s.Albedo = path("albedo");
    s.Normal = path("normal");
    s.Roughness = path("roughness");
    s.AO = path("ao");
    s.Tiling = std::max(num("tiling", s.Tiling), 0.01f);
    arr("tint", &s.Tint.x, 3);
    s.Strength = std::clamp(num("strength", s.Strength), 0.0f, 1.0f);
    arr("roughnessRange", &s.RoughRange.x, 2);
    s.RoughRange = glm::clamp(s.RoughRange, glm::vec2(0.0f), glm::vec2(1.0f));
    if (auto it = j.find("mirror"); it != j.end()) {
        if (it->is_boolean()) s.Mirror = it->template get<bool>();
        else if (it->is_number()) s.Mirror = it->template get<double>() != 0.0;
    }
    arr("crop", &s.Crop.x, 4);
    s.Crop = glm::clamp(s.Crop, glm::vec4(0.0f), glm::vec4(1.0f));
    out = s;
    return true;
}

// `writePath(const std::string&) -> Json`, called for non-empty paths ("" is written as "").
template <class Json, class WritePath> Json Write(const SubstrateDesc& s, WritePath writePath) {
    auto p = [&](const std::string& v) -> Json { return v.empty() ? Json(std::string()) : Json(writePath(v)); };
    Json j = Json::object();
    j["albedo"] = p(s.Albedo);
    j["normal"] = p(s.Normal);
    j["roughness"] = p(s.Roughness);
    j["ao"] = p(s.AO);
    j["tiling"] = s.Tiling;
    j["tint"] = {s.Tint.r, s.Tint.g, s.Tint.b};
    j["strength"] = s.Strength;
    j["roughnessRange"] = {s.RoughRange.x, s.RoughRange.y};
    j["mirror"] = s.Mirror;
    j["crop"] = {s.Crop.x, s.Crop.y, s.Crop.z, s.Crop.w};
    return j;
}

} // namespace SubstrateJson
