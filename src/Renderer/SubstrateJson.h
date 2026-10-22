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

// The values present in `j` over `s` (paths through readPath).
template <class Json, class ReadPath> void ReadFields(const Json& j, SubstrateDesc& s, ReadPath readPath) {
    auto path = [&](const char* k, std::string& dst) {
        auto it = j.find(k);
        if (it != j.end()) dst = it->is_null() ? std::string() : readPath(*it);
    };
    auto num = [&](const char* k, float& dst) {
        auto it = j.find(k);
        if (it != j.end() && it->is_number()) dst = it->template get<float>();
    };
    auto arr = [&](const char* k, float* dst, int n) {
        auto it = j.find(k);
        if (it == j.end() || !it->is_array() || (int)it->size() < n) return;
        for (int i = 0; i < n; ++i)
            if (!(*it)[i].is_number()) return;
        for (int i = 0; i < n; ++i) dst[i] = (*it)[i].template get<float>();
    };
    path("albedo", s.Albedo);
    path("normal", s.Normal);
    path("roughness", s.Roughness);
    path("ao", s.AO);
    num("tiling", s.Tiling);
    s.Tiling = std::max(s.Tiling, 0.01f);
    arr("tint", &s.Tint.x, 3);
    num("strength", s.Strength);
    s.Strength = std::clamp(s.Strength, 0.0f, 1.0f);
    arr("roughnessRange", &s.RoughRange.x, 2);
    s.RoughRange = glm::clamp(s.RoughRange, glm::vec2(0.0f), glm::vec2(1.0f));
    if (auto it = j.find("mirror"); it != j.end()) {
        if (it->is_boolean()) s.Mirror = it->template get<bool>();
        else if (it->is_number()) s.Mirror = it->template get<double>() != 0.0;
    }
    arr("crop", &s.Crop.x, 4);
    s.Crop = glm::clamp(s.Crop, glm::vec4(0.0f), glm::vec4(1.0f));
    num("detail", s.Detail);
    s.Detail = std::clamp(s.Detail, 0.0f, 2.0f);
}

// `readPath(const Json&) -> std::string`. A string or {"preset": ...} names a .substrate preset
// (other keys override it); any other object is an inline block. False (out untouched) otherwise.
template <class Json, class ReadPath> bool Read(const Json& j, SubstrateDesc& out, ReadPath readPath) {
    SubstrateDesc s;
    if (j.is_string()) {
        const std::string p = readPath(j);
        s = SubstratePresets::Get(p);
        s.Preset = p;
        out = s;
        return true;
    }
    if (!j.is_object()) return false;
    if (auto it = j.find("preset"); it != j.end() && !it->is_null()) {
        const std::string p = readPath(*it);
        if (!p.empty()) {
            s = SubstratePresets::Get(p);
            s.Preset = p;
        }
    }
    ReadFields(j, s, readPath);
    out = s;
    return true;
}

// `writePath(const std::string&) -> Json`, called for non-empty paths ("" is written as ""). With a
// Preset, only the preset reference and the values that differ from it.
template <class Json, class WritePath> Json Write(const SubstrateDesc& s, WritePath writePath) {
    auto p = [&](const std::string& v) -> Json { return v.empty() ? Json(std::string()) : Json(writePath(v)); };
    Json j = Json::object();
    const bool preset = !s.Preset.empty();
    const SubstrateDesc base = preset ? SubstratePresets::Get(s.Preset) : SubstrateDesc{};
    if (preset) j["preset"] = p(s.Preset);
    auto want = [&](bool differs) { return !preset || differs; };
    if (want(s.Albedo != base.Albedo)) j["albedo"] = p(s.Albedo);
    if (want(s.Normal != base.Normal)) j["normal"] = p(s.Normal);
    if (want(s.Roughness != base.Roughness)) j["roughness"] = p(s.Roughness);
    if (want(s.AO != base.AO)) j["ao"] = p(s.AO);
    if (want(s.Tiling != base.Tiling)) j["tiling"] = s.Tiling;
    if (want(s.Tint != base.Tint)) j["tint"] = {s.Tint.r, s.Tint.g, s.Tint.b};
    if (want(s.Strength != base.Strength)) j["strength"] = s.Strength;
    if (want(s.RoughRange != base.RoughRange)) j["roughnessRange"] = {s.RoughRange.x, s.RoughRange.y};
    if (want(s.Mirror != base.Mirror)) j["mirror"] = s.Mirror;
    if (want(s.Crop != base.Crop)) j["crop"] = {s.Crop.x, s.Crop.y, s.Crop.z, s.Crop.w};
    if (want(s.Detail != base.Detail) && (preset || s.Detail != 0.0f)) j["detail"] = s.Detail;
    return j;
}

} // namespace SubstrateJson
