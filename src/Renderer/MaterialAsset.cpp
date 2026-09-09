#include "MaterialAsset.h"
#include "AssetLibrary.h"
#include "Log.h"

#include <json.hpp>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace {

// Helpers for glm ↔ json (a plain array of floats, not an object).
json Vec3ToJson(const glm::vec3& v) { return {v.x, v.y, v.z}; }
glm::vec3 JsonToVec3(const json& j, const glm::vec3& def = {}) {
    if (!j.is_array() || j.size() < 3) return def;
    return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
}

} // namespace

std::shared_ptr<MaterialAsset> MaterialAsset::Load(const std::string& path, AssetLibrary* lib) {
    std::ifstream f(path);
    if (!f.is_open()) return nullptr;

    json j;
    try { j = json::parse(f); } catch (...) {
        Log::Error("MaterialAsset: JSON parse error in '" + path + "'");
        return nullptr;
    }

    auto ma = std::make_shared<MaterialAsset>();
    ma->Path = path;
    ma->Name = j.value("name", std::filesystem::path(path).stem().string());

    auto& m = ma->Mat;
    m.BaseColor       = JsonToVec3(j.value("baseColor",      json::array({1,1,1})), {1,1,1});
    m.Metallic        = j.value("metallic",        0.0f);
    m.Roughness       = j.value("roughness",       0.5f);
    m.EmissiveColor   = JsonToVec3(j.value("emissiveColor",   json::array({0,0,0})), {});
    m.EmissiveStrength= j.value("emissiveStrength",1.0f);
    m.Triplanar       = j.value("triplanar",       false);
    m.TriplanarScale  = j.value("triplanarScale",  1.0f);

    ma->AlbedoMapPath           = j.value("albedoMap",            std::string());
    ma->NormalMapPath           = j.value("normalMap",            std::string());
    ma->MetallicRoughnessMapPath= j.value("metallicRoughnessMap", std::string());
    ma->MetallicMapPath         = j.value("metallicMap",          std::string());
    ma->RoughnessMapPath        = j.value("roughnessMap",         std::string());
    ma->AOMapPath               = j.value("aoMap",                std::string());
    ma->EmissiveMapPath         = j.value("emissiveMap",          std::string());

    if (lib) {
        auto loadTex = [&](const std::string& p) -> std::shared_ptr<Texture> {
            return p.empty() ? nullptr : lib->LoadTexture(p);
        };
        m.AlbedoMap            = loadTex(ma->AlbedoMapPath);
        m.NormalMap            = loadTex(ma->NormalMapPath);
        m.MetallicRoughnessMap = loadTex(ma->MetallicRoughnessMapPath);
        m.MetallicMap          = loadTex(ma->MetallicMapPath);
        m.RoughnessMap         = loadTex(ma->RoughnessMapPath);
        m.AOMap                = loadTex(ma->AOMapPath);
        m.EmissiveMap          = loadTex(ma->EmissiveMapPath);
    }

    return ma;
}

bool MaterialAsset::Save() const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(Path).parent_path(), ec);

    std::ofstream f(Path);
    if (!f.is_open()) return false;

    const auto& m = Mat;
    json j;
    j["matVersion"]          = 1;
    j["name"]                = Name;
    j["baseColor"]           = Vec3ToJson(m.BaseColor);
    j["metallic"]            = m.Metallic;
    j["roughness"]           = m.Roughness;
    j["emissiveColor"]       = Vec3ToJson(m.EmissiveColor);
    j["emissiveStrength"]    = m.EmissiveStrength;
    j["triplanar"]           = m.Triplanar;
    j["triplanarScale"]      = m.TriplanarScale;
    j["albedoMap"]           = AlbedoMapPath;
    j["normalMap"]           = NormalMapPath;
    j["metallicRoughnessMap"]= MetallicRoughnessMapPath;
    j["metallicMap"]         = MetallicMapPath;
    j["roughnessMap"]        = RoughnessMapPath;
    j["aoMap"]               = AOMapPath;
    j["emissiveMap"]         = EmissiveMapPath;

    f << j.dump(2) << "\n";
    return f.good();
}

std::shared_ptr<MaterialAsset> MaterialAsset::CreateDefault(const std::string& path) {
    auto ma = std::make_shared<MaterialAsset>();
    ma->Path = path;
    ma->Name = std::filesystem::path(path).stem().string();
    // Mat defaults are already correct: white, 0 metallic, 0.5 roughness.
    if (!ma->Save()) return nullptr;
    return ma;
}
