#include "MaterialAsset.h"
#include "ShaderAsset.h"
#include "AssetLibrary.h"
#include "Texture.h"
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

    int matVersion = j.value("matVersion", 1);

    auto& m = ma->Mat;

    if (matVersion >= 2) {
        // v2 format: "shader" path + "properties" object.
        ma->ShaderPath = j.value("shader", std::string());

        // Queue fields (Opaque by default — dormant on existing materials).
        ma->RenderQueue = (Queue)j.value("renderQueue", (int)Queue::Opaque);
        ma->QueueIndex  = j.value("queueIndex",  2000);
        ma->Opacity     = j.value("opacity",      1.0f);

        // Populate Material struct from "properties" (also fills legacy path strings).
        if (j.contains("properties") && j["properties"].is_object()) {
            const auto& props = j["properties"];
            auto strProp = [&](const char* k) { return props.value(k, std::string()); };
            // Scalars / colors
            m.BaseColor        = JsonToVec3(props.value("_BaseColor",      json::array({1,1,1})), {1,1,1});
            m.Metallic         = props.value("_Metallic",         0.0f);
            m.Roughness        = props.value("_Roughness",        0.5f);
            m.EmissiveColor    = JsonToVec3(props.value("_EmissiveColor",  json::array({0,0,0})), {});
            m.EmissiveStrength = props.value("_EmissiveStrength", 1.0f);
            m.Triplanar        = props.value("_Triplanar",        false);
            m.TriplanarScale   = props.value("_TriplanarScale",   1.0f);
            // Scalars / PR10
            m.ClearCoat          = props.value("_ClearCoat",          0.0f);
            m.ClearCoatRoughness = props.value("_ClearCoatRoughness",  0.5f);
            m.Anisotropy         = props.value("_Anisotropy",          0.0f);
            m.AnisotropyRotation = props.value("_AnisotropyRotation",  0.0f);
            // Scalars / PR11
            m.Sheen               = JsonToVec3(props.value("_Sheen",           json::array({0,0,0})), {});
            m.SheenRoughness      = props.value("_SheenRoughness",  0.5f);
            m.SubsurfaceColor     = JsonToVec3(props.value("_SubsurfaceColor", json::array({1,0.8f,0.6f})), {1,0.8f,0.6f});
            m.Thickness           = props.value("_Thickness",        0.5f);
            // Scalars / PR12
            m.TransmissionStrength = props.value("_TransmissionStrength", 0.0f);
            m.IOR                  = props.value("_IOR",                  1.5f);
            // Texture paths
            ma->AlbedoMapPath            = strProp("_AlbedoMap");
            ma->NormalMapPath            = strProp("_NormalMap");
            ma->MetallicRoughnessMapPath = strProp("_MetallicRoughnessMap");
            ma->MetallicMapPath          = strProp("_MetallicMap");
            ma->RoughnessMapPath         = strProp("_RoughnessMap");
            ma->AOMapPath                = strProp("_AOMap");
            ma->EmissiveMapPath          = strProp("_EmissiveMap");
            ma->ClearCoatMapPath         = strProp("_ClearCoatMap");
            ma->ThicknessMapPath         = strProp("_ThicknessMap");
        }
    } else {
        // v1 format: flat property keys, no shader reference.
        m.BaseColor        = JsonToVec3(j.value("baseColor",      json::array({1,1,1})), {1,1,1});
        m.Metallic         = j.value("metallic",         0.0f);
        m.Roughness        = j.value("roughness",        0.5f);
        m.EmissiveColor    = JsonToVec3(j.value("emissiveColor",  json::array({0,0,0})), {});
        m.EmissiveStrength = j.value("emissiveStrength", 1.0f);
        m.Triplanar        = j.value("triplanar",        false);
        m.TriplanarScale   = j.value("triplanarScale",   1.0f);

        ma->AlbedoMapPath            = j.value("albedoMap",            std::string());
        ma->NormalMapPath            = j.value("normalMap",            std::string());
        ma->MetallicRoughnessMapPath = j.value("metallicRoughnessMap", std::string());
        ma->MetallicMapPath          = j.value("metallicMap",          std::string());
        ma->RoughnessMapPath         = j.value("roughnessMap",         std::string());
        ma->AOMapPath                = j.value("aoMap",                std::string());
        ma->EmissiveMapPath          = j.value("emissiveMap",          std::string());
    }

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
        m.ClearCoatMap         = loadTex(ma->ClearCoatMapPath);
        m.ThicknessMap         = loadTex(ma->ThicknessMapPath);

        // Resolve shader asset when path is set (AssetLibrary handles caching).
        if (!ma->ShaderPath.empty())
            ma->Shader = lib->LoadShader(ma->ShaderPath);
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

    if (!ShaderPath.empty()) {
        // v2 format: shader reference + properties map
        j["matVersion"]  = 2;
        j["name"]        = Name;
        j["shader"]      = ShaderPath;
        if (RenderQueue != Queue::Opaque) j["renderQueue"] = (int)RenderQueue;
        if (QueueIndex  != 2000)          j["queueIndex"]  = QueueIndex;
        if (Opacity     != 1.0f)          j["opacity"]     = Opacity;
        json& props     = j["properties"];
        props["_BaseColor"]           = Vec3ToJson(m.BaseColor);
        props["_Metallic"]            = m.Metallic;
        props["_Roughness"]           = m.Roughness;
        props["_EmissiveColor"]       = Vec3ToJson(m.EmissiveColor);
        props["_EmissiveStrength"]    = m.EmissiveStrength;
        props["_Triplanar"]           = m.Triplanar;
        props["_TriplanarScale"]      = m.TriplanarScale;
        props["_AlbedoMap"]           = AlbedoMapPath;
        props["_NormalMap"]           = NormalMapPath;
        props["_MetallicRoughnessMap"]= MetallicRoughnessMapPath;
        props["_MetallicMap"]         = MetallicMapPath;
        props["_RoughnessMap"]        = RoughnessMapPath;
        props["_AOMap"]               = AOMapPath;
        props["_EmissiveMap"]         = EmissiveMapPath;
        if (m.ClearCoat           != 0.0f) props["_ClearCoat"]          = m.ClearCoat;
        if (m.ClearCoatRoughness  != 0.5f) props["_ClearCoatRoughness"] = m.ClearCoatRoughness;
        if (m.Anisotropy          != 0.0f) props["_Anisotropy"]         = m.Anisotropy;
        if (m.AnisotropyRotation  != 0.0f) props["_AnisotropyRotation"] = m.AnisotropyRotation;
        if (!ClearCoatMapPath.empty())      props["_ClearCoatMap"]         = ClearCoatMapPath;
        if (m.Sheen != glm::vec3(0.0f))    props["_Sheen"]               = Vec3ToJson(m.Sheen);
        if (m.SheenRoughness  != 0.5f)     props["_SheenRoughness"]      = m.SheenRoughness;
        auto defSSS = glm::vec3(1.0f, 0.8f, 0.6f);
        if (m.SubsurfaceColor != defSSS)   props["_SubsurfaceColor"]     = Vec3ToJson(m.SubsurfaceColor);
        if (m.Thickness       != 0.5f)     props["_Thickness"]           = m.Thickness;
        if (!ThicknessMapPath.empty())              props["_ThicknessMap"]            = ThicknessMapPath;
        if (m.TransmissionStrength != 0.0f)         props["_TransmissionStrength"]    = m.TransmissionStrength;
        if (m.IOR                  != 1.5f)         props["_IOR"]                     = m.IOR;
    } else {
        // v1 format (backward compatible)
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
    }

    f << j.dump(2) << "\n";
    return f.good();
}

// --- Property access by shader property name ---

const std::shared_ptr<Texture>& MaterialAsset::GetTexture(const Material& m, const std::string& n) {
    static const std::shared_ptr<Texture> sNull;
    if (n == "_AlbedoMap")            return m.AlbedoMap;
    if (n == "_NormalMap")            return m.NormalMap;
    if (n == "_MetallicRoughnessMap") return m.MetallicRoughnessMap;
    if (n == "_MetallicMap")          return m.MetallicMap;
    if (n == "_RoughnessMap")         return m.RoughnessMap;
    if (n == "_AOMap")                return m.AOMap;
    if (n == "_EmissiveMap")          return m.EmissiveMap;
    if (n == "_ClearCoatMap")         return m.ClearCoatMap;
    if (n == "_ThicknessMap")         return m.ThicknessMap;
    return sNull;
}

glm::vec3 MaterialAsset::GetColor(const Material& m, const std::string& n) {
    if (n == "_BaseColor")      return m.BaseColor;
    if (n == "_EmissiveColor")  return m.EmissiveColor * m.EmissiveStrength; // pre-multiply
    if (n == "_Sheen")          return m.Sheen;
    if (n == "_SubsurfaceColor")return m.SubsurfaceColor;
    return {};
}

float MaterialAsset::GetFloat(const Material& m, const std::string& n) {
    if (n == "_Metallic")            return m.Metallic;
    if (n == "_Roughness")           return m.Roughness;
    if (n == "_EmissiveStrength")    return m.EmissiveStrength;
    if (n == "_TriplanarScale")      return m.TriplanarScale;
    if (n == "_ClearCoat")           return m.ClearCoat;
    if (n == "_ClearCoatRoughness")  return m.ClearCoatRoughness;
    if (n == "_Anisotropy")          return m.Anisotropy;
    if (n == "_AnisotropyRotation")  return m.AnisotropyRotation;
    if (n == "_SheenRoughness")          return m.SheenRoughness;
    if (n == "_Thickness")               return m.Thickness;
    if (n == "_TransmissionStrength")    return m.TransmissionStrength;
    if (n == "_IOR")                     return m.IOR;
    return 0.0f;
}

bool MaterialAsset::GetBool(const Material& m, const std::string& n) {
    if (n == "_Triplanar") return m.Triplanar;
    return false;
}

void MaterialAsset::SetTexture(Material& m, const std::string& n, const std::shared_ptr<Texture>& tex) {
    if (n == "_AlbedoMap")            { m.AlbedoMap = tex; return; }
    if (n == "_NormalMap")            { m.NormalMap = tex; return; }
    if (n == "_MetallicRoughnessMap") { m.MetallicRoughnessMap = tex; return; }
    if (n == "_MetallicMap")          { m.MetallicMap = tex; return; }
    if (n == "_RoughnessMap")         { m.RoughnessMap = tex; return; }
    if (n == "_AOMap")                { m.AOMap = tex; return; }
    if (n == "_EmissiveMap")          { m.EmissiveMap = tex; return; }
    if (n == "_ClearCoatMap")         { m.ClearCoatMap = tex; return; }
    if (n == "_ThicknessMap")         { m.ThicknessMap = tex; return; }
}

void MaterialAsset::SetColor(Material& m, const std::string& n, const glm::vec3& v) {
    if (n == "_BaseColor")       { m.BaseColor = v; return; }
    if (n == "_EmissiveColor")   { m.EmissiveColor = v; return; }
    if (n == "_Sheen")           { m.Sheen = v; return; }
    if (n == "_SubsurfaceColor") { m.SubsurfaceColor = v; return; }
}

void MaterialAsset::SetFloat(Material& m, const std::string& n, float v) {
    if (n == "_Metallic")            { m.Metallic = v; return; }
    if (n == "_Roughness")           { m.Roughness = v; return; }
    if (n == "_EmissiveStrength")    { m.EmissiveStrength = v; return; }
    if (n == "_TriplanarScale")      { m.TriplanarScale = v; return; }
    if (n == "_ClearCoat")           { m.ClearCoat = v; return; }
    if (n == "_ClearCoatRoughness")  { m.ClearCoatRoughness = v; return; }
    if (n == "_Anisotropy")          { m.Anisotropy = v; return; }
    if (n == "_AnisotropyRotation")  { m.AnisotropyRotation = v; return; }
    if (n == "_SheenRoughness")          { m.SheenRoughness = v; return; }
    if (n == "_Thickness")               { m.Thickness = v; return; }
    if (n == "_TransmissionStrength")    { m.TransmissionStrength = v; return; }
    if (n == "_IOR")                     { m.IOR = v; return; }
}

void MaterialAsset::SetBool(Material& m, const std::string& n, bool v) {
    if (n == "_Triplanar") m.Triplanar = v;
}

std::shared_ptr<MaterialAsset> MaterialAsset::CreateDefault(const std::string& path) {
    auto ma = std::make_shared<MaterialAsset>();
    ma->Path = path;
    ma->Name = std::filesystem::path(path).stem().string();
    // Mat defaults are already correct: white, 0 metallic, 0.5 roughness.
    if (!ma->Save()) return nullptr;
    return ma;
}
