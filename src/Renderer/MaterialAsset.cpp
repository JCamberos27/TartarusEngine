#include "MaterialAsset.h"
#include "ShaderAsset.h"
#include "AssetLibrary.h"
#include "Texture.h"
#include "ShaderLibrary.h" // engine:// shader references (#104)
#include "Log.h"
#include "AtomicFile.h"
#include "ProjectPaths.h"
#include "AssetDatabase.h" // #208 - shaderGuid

#include <json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

using json = nlohmann::json;

namespace {

// Helpers for glm ↔ json (a plain array of floats, not an object).
json Vec3ToJson(const glm::vec3& v) { return {v.x, v.y, v.z}; }
glm::vec3 JsonToVec3(const json& j, const glm::vec3& def = {}) {
    if (!j.is_array() || j.size() < 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number())
        return def;
    return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
}
// 2-4 numbers; missing components come from def (#105 — Vec2/Vec4 used to be saved as 3 floats).
glm::vec4 JsonToVec4(const json& j, const glm::vec4& def) {
    glm::vec4 out = def;
    if (!j.is_array()) return def;
    for (size_t i = 0; i < j.size() && i < 4; ++i) {
        if (!j[i].is_number()) return def;
        out[(int)i] = j[i].get<float>();
    }
    return out;
}

// #105 — type-checked reads. nlohmann's value()/get<>() throw type_error on a wrong-typed field
// (e.g. "_Metallic": "0.5"), which escaped Load() uncaught and closed the editor. A wrong type
// now falls back to the default with a warning naming the file and key.
struct Reader {
    const json& obj;
    const std::string& path;
    const json* Find(const char* k) const {
        auto it = obj.find(k);
        return it == obj.end() || it->is_null() ? nullptr : &*it;
    }
    void Warn(const char* k, const char* want) const {
        Log::Warn("MaterialAsset: '" + path + "': \"" + k + "\" should be " + want + " - using the default.", LogContext::Asset(path));
    }
    float Num(const char* k, float def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_number()) return v->get<float>();
        Warn(k, "a number"); return def;
    }
    int Int(const char* k, int def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_number()) return (int)v->get<double>();
        Warn(k, "a number"); return def;
    }
    bool Bool(const char* k, bool def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_boolean()) return v->get<bool>();
        if (v->is_number()) return v->get<double>() != 0.0;
        Warn(k, "true/false"); return def;
    }
    std::string Str(const char* k) const {
        const json* v = Find(k);
        if (!v) return {};
        if (v->is_string()) return v->get<std::string>();
        Warn(k, "a string"); return {};
    }
    glm::vec3 Vec3(const char* k, const glm::vec3& def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_array() && v->size() >= 3 && (*v)[0].is_number() && (*v)[1].is_number() && (*v)[2].is_number())
            return JsonToVec3(*v, def);
        Warn(k, "an array of 3 numbers"); return def;
    }
    glm::vec2 Vec2(const char* k, const glm::vec2& def) const {
        const json* v = Find(k);
        if (!v) return def;
        if (v->is_array() && v->size() >= 2 && (*v)[0].is_number() && (*v)[1].is_number())
            return {(*v)[0].get<float>(), (*v)[1].get<float>()};
        Warn(k, "an array of 2 numbers"); return def;
    }
};

} // namespace


namespace {
// #6 Defect #50 — a non-empty path that couldn't be loaded becomes a "missing" placeholder
// (still carrying Path/Name) rather than nullptr, which was indistinguishable from "no override
// was ever set" once stored in a mesh's material slot (RenderableComponent::Materials).
std::shared_ptr<MaterialAsset> MissingPlaceholder(const std::string& path) {
    auto ma = std::make_shared<MaterialAsset>();
    ma->Path = path;
    ma->Name = std::filesystem::path(path).stem().string();
    ma->Missing = true;
    return ma;
}
} // namespace

std::shared_ptr<MaterialAsset> LoadMaterialFromJson(const json& j, const std::string& path, AssetLibrary* lib);

namespace {

// #132 - a .mat's texture references are paths (in "properties" for v2, at the top level for
// v1) plus, since #132, a "textureGuids" object keyed the same way. A texture file is found
// by its absolute path, or project-relative.
std::string AbsoluteAssetPath(const std::string& p) {
    if (p.empty() || std::filesystem::path(p).is_absolute()) return p;
    return ProjectPaths::Resolve(p);
}

bool IsTextureAsset(const std::string& absPath) {
    const std::string type = AssetDatabase::AssetType(absPath);
    return type == "texture" || type == "hdri";
}

// Save side: record the GUID of every texture the material references.
void WriteTextureGuids(json& j) {
    const bool v2 = j.contains("properties") && j["properties"].is_object();
    const json& props = v2 ? j["properties"] : j;
    json guids = json::object();
    for (const auto& [key, value] : props.items()) {
        if (!value.is_string() || key == "name" || key == "shader") continue;
        const std::string abs = AbsoluteAssetPath(value.get<std::string>());
        std::error_code ec;
        if (abs.empty() || !IsTextureAsset(abs) || !std::filesystem::exists(abs, ec)) continue;
        const AssetGuid g = AssetDatabase::EnsureGuid(abs);
        if (g.IsValid()) guids[key] = g.ToString();
    }
    if (!guids.empty()) j["textureGuids"] = std::move(guids);
}

// Load side: a texture whose stored path is gone but whose GUID now lives elsewhere (renamed or
// moved) is re-pointed at its new location, so the material keeps its maps.
json FollowMovedTextures(const json& in, const std::string& matPath) {
    const auto guids = in.find("textureGuids");
    if (guids == in.end() || !guids->is_object()) return in;
    json j = in;
    const bool v2 = j.contains("properties") && j["properties"].is_object();
    json& props = v2 ? j["properties"] : j;
    for (const auto& [key, gv] : guids->items()) {
        if (!gv.is_string()) continue;
        const auto it = props.find(key);
        if (it == props.end() || !it->is_string()) continue;
        std::error_code ec;
        const std::string stored = it->get<std::string>();
        if (!stored.empty() && std::filesystem::exists(AbsoluteAssetPath(stored), ec)) continue;
        const std::string moved = AssetDatabase::PathForGuid(AssetGuid::FromString(gv.get<std::string>()));
        if (moved.empty() || !std::filesystem::exists(moved, ec)) continue;
        Log::Info("Material '" + ProjectPaths::Relativize(matPath) + "': " + key + " moved to " +
                  ProjectPaths::Relativize(moved) + ".", LogContext::Asset(matPath));
        *it = moved;
    }
    return j;
}

} // namespace

std::shared_ptr<MaterialAsset> MaterialAsset::Load(const std::string& path, AssetLibrary* lib) {
    std::ifstream f(path);
    if (!f.is_open()) return MissingPlaceholder(path);

    json j;
    try { j = json::parse(f); } catch (...) {
        Log::Error("MaterialAsset: JSON parse error in '" + path + "'", LogContext::Asset(path));
        return MissingPlaceholder(path);
    }

    if (!j.is_object()) {
        Log::Error("MaterialAsset: '" + path + "' is not a JSON object.", LogContext::Asset(path));
        return MissingPlaceholder(path);
    }
    try {
        return LoadMaterialFromJson(FollowMovedTextures(j, path), path, lib); // #132
    } catch (const std::exception& e) { // #105 — belt and braces behind the typed reads
        Log::Error("MaterialAsset: '" + path + "' is malformed: " + e.what(), LogContext::Asset(path));
        return MissingPlaceholder(path);
    }
}

// #104 / #208 — a .mat's "shader" reference: "engine://Standard.shader" is the engine's copy,
// "project://..." or a plain relative path is project-relative (resolved against the project
// root, so a .mat works from any working directory, #354). A project-relative reference to a
// file the project doesn't have, whose name IS a built-in engine shader, uses the engine one:
// that's every material written while a copy of Standard.shader lived in project/shaders/.
std::string ResolveShaderPath(const std::string& ref) {
    if (ref.rfind("engine://", 0) == 0) return ShaderLibrary::ResolveRef(ref, {});
    std::string rel = ref.rfind("project://", 0) == 0 ? ref.substr(10) : ref;
    std::filesystem::path p(rel);
    std::string resolved = p.is_relative() ? ProjectPaths::Resolve(rel) : rel;
    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec)) {
        const std::string engine = ShaderLibrary::ResolveRef("engine://" + p.filename().string(), {});
        if (std::filesystem::exists(engine, ec)) return engine;
    }
    return resolved;
}

std::shared_ptr<MaterialAsset> LoadMaterialFromJson(const json& j, const std::string& path, AssetLibrary* lib) {
    using Queue = MaterialAsset::Queue;
    const Reader top{j, path};
    auto ma = std::make_shared<MaterialAsset>();
    ma->Path = path;
    ma->Name = top.Str("name");
    if (ma->Name.empty()) ma->Name = std::filesystem::path(path).stem().string();

    int matVersion = top.Int("matVersion", 1);

    auto& m = ma->Mat;
    // #104 — custom shader keywords switched on for this material (strings only; duplicates dropped).
    if (auto kw = j.find("keywords"); kw != j.end() && kw->is_array()) {
        for (const auto& k : *kw)
            if (k.is_string() && std::find(m.ShaderKeywords.begin(), m.ShaderKeywords.end(), k.get<std::string>()) == m.ShaderKeywords.end())
                m.ShaderKeywords.push_back(k.get<std::string>());
    }

    // Queue / surface fields — both formats (#105: v1 used to drop them, so a transparent or
    // cutout v1 material reverted to opaque on reload). Opaque by default.
    ma->RenderQueue = (Queue)std::clamp(top.Int("renderQueue", (int)Queue::Opaque), 0, 2);
    ma->QueueIndex  = top.Int("queueIndex", 2000);
    ma->Opacity     = std::clamp(top.Num("opacity", 1.0f), 0.0f, 1.0f);
    // #101 — the AlphaTest queue now actually clips (it rendered exactly like Opaque).
    m.AlphaClip   = ma->RenderQueue == Queue::AlphaTest;
    m.AlphaCutoff = std::clamp(top.Num("alphaCutoff", 0.5f), 0.0f, 1.0f);

    // v2 keeps everything under "properties" with shader-style names; v1 is flat with camelCase
    // names. The advanced lobes are read in both (v1 only has them if saved by this build).
    const bool v2 = matVersion >= 2;
    static const json kEmpty = json::object();
    const json& propsJson = v2 ? ((j.contains("properties") && j["properties"].is_object()) ? j["properties"] : kEmpty) : j;
    const Reader r{propsJson, path};
    auto key = [&](const char* v2Key, const char* v1Key) { return v2 ? v2Key : v1Key; };

    if (v2) {
        ma->ShaderPath = top.Str("shader");
        // #208 - a project shader reference also carries the descriptor's GUID: if the .shader
        // was renamed or moved, follow it instead of falling back to the default shader.
        if (ma->ShaderPath.rfind("engine://", 0) != 0) {
            const auto gj = j.find("shaderGuid");
            const AssetGuid g = (gj != j.end() && gj->is_string()) ? AssetGuid::FromString(gj->get<std::string>()) : AssetGuid{};
            const std::string moved = g.IsValid() ? AssetDatabase::PathForGuid(g) : std::string();
            std::error_code ec;
            if (!moved.empty() && std::filesystem::exists(moved, ec) &&
                !std::filesystem::exists(ResolveShaderPath(ma->ShaderPath), ec)) {
                Log::Info("Material '" + path + "': shader moved to " + ProjectPaths::Relativize(moved) + ".");
                ma->ShaderPath = ProjectPaths::Relativize(moved);
            }
        }
    }
    m.BaseColor        = r.Vec3(key("_BaseColor", "baseColor"), {1, 1, 1});
    m.Metallic         = r.Num(key("_Metallic", "metallic"), 0.0f);
    m.Roughness        = r.Num(key("_Roughness", "roughness"), 0.5f);
    m.EmissiveColor    = r.Vec3(key("_EmissiveColor", "emissiveColor"), {});
    m.EmissiveStrength = r.Num(key("_EmissiveStrength", "emissiveStrength"), 1.0f);
    m.Triplanar        = r.Bool(key("_Triplanar", "triplanar"), false);
    m.TriplanarScale   = r.Num(key("_TriplanarScale", "triplanarScale"), 1.0f);
    // PR10-12 lobes
    m.ClearCoat            = r.Num(key("_ClearCoat", "clearCoat"), 0.0f);
    m.ClearCoatRoughness   = r.Num(key("_ClearCoatRoughness", "clearCoatRoughness"), 0.5f);
    m.Anisotropy           = r.Num(key("_Anisotropy", "anisotropy"), 0.0f);
    m.AnisotropyRotation   = r.Num(key("_AnisotropyRotation", "anisotropyRotation"), 0.0f);
    m.Sheen                = r.Vec3(key("_Sheen", "sheen"), {});
    m.SheenRoughness       = r.Num(key("_SheenRoughness", "sheenRoughness"), 0.5f);
    m.SubsurfaceColor      = r.Vec3(key("_SubsurfaceColor", "subsurfaceColor"), {1, 0.8f, 0.6f});
    m.Thickness            = r.Num(key("_Thickness", "thickness"), 0.5f);
    m.TransmissionStrength = r.Num(key("_TransmissionStrength", "transmission"), 0.0f);
    m.IOR                  = r.Num(key("_IOR", "ior"), 1.5f);
    // #354: variant opt-ins with no natural "off" value (not shader Properties()).
    m.SubsurfaceEnabled = r.Bool(key("_SubsurfaceEnabled", "subsurface"), false);
    m.ReflectionProbes  = r.Bool(key("_ReflectionProbes", "reflectionProbes"), false);
    // Texture paths
    ma->AlbedoMapPath            = r.Str(key("_AlbedoMap", "albedoMap"));
    ma->NormalMapPath            = r.Str(key("_NormalMap", "normalMap"));
    ma->MetallicRoughnessMapPath = r.Str(key("_MetallicRoughnessMap", "metallicRoughnessMap"));
    ma->MetallicMapPath          = r.Str(key("_MetallicMap", "metallicMap"));
    ma->RoughnessMapPath         = r.Str(key("_RoughnessMap", "roughnessMap"));
    ma->AOMapPath                = r.Str(key("_AOMap", "aoMap"));
    ma->EmissiveMapPath          = r.Str(key("_EmissiveMap", "emissiveMap"));
    ma->ClearCoatMapPath         = r.Str(key("_ClearCoatMap", "clearCoatMap"));
    ma->ThicknessMapPath         = r.Str(key("_ThicknessMap", "thicknessMap"));
    // #102 / #113 — surface options.
    ma->HeightMapPath            = r.Str(key("_HeightMap", "heightMap"));
    ma->DetailAlbedoMapPath      = r.Str(key("_DetailAlbedoMap", "detailAlbedoMap"));
    ma->DetailNormalMapPath      = r.Str(key("_DetailNormalMap", "detailNormalMap"));
    m.UVTiling     = r.Vec2(key("_UVTiling", "uvTiling"), {1, 1});
    m.UVOffset     = r.Vec2(key("_UVOffset", "uvOffset"), {0, 0});
    m.DetailTiling = r.Vec2(key("_DetailTiling", "detailTiling"), {4, 4});
    m.NormalStrength = r.Num(key("_NormalStrength", "normalStrength"), 1.0f);
    m.NormalFlipY    = r.Bool(key("_NormalFlipY", "normalFlipY"), false);
    m.DoubleSided    = r.Bool(key("_DoubleSided", "doubleSided"), false);
    m.UseVertexColor = r.Bool(key("_VertexColors", "vertexColors"), false);
    m.ParallaxScale  = r.Num(key("_ParallaxScale", "parallaxScale"), 0.02f);

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
    MaterialAsset::UpgradeLegacyMapFactors(m, top.Bool("factorsScaleMaps", false)); // #102
        m.ClearCoatMap         = loadTex(ma->ClearCoatMapPath);
        m.ThicknessMap         = loadTex(ma->ThicknessMapPath);
        m.HeightMap            = loadTex(ma->HeightMapPath);
        m.DetailAlbedoMap      = loadTex(ma->DetailAlbedoMapPath);
        m.DetailNormalMap      = loadTex(ma->DetailNormalMapPath);

        // Resolve shader asset when path is set (AssetLibrary handles caching). A project-
        // relative "shader" value is resolved against the project root so a .mat works from any
        // working directory (fixtures under project/, #354).
        if (!ma->ShaderPath.empty()) ma->Shader = lib->LoadShader(ResolveShaderPath(ma->ShaderPath));
        // #104 — a shader's `Queue` is the default for materials that don't choose their own.
        if (ma->Shader && ma->Shader->RenderState().Queue >= 0 && !j.contains("renderQueue")) {
            const ShaderRenderState& st = ma->Shader->RenderState();
            ma->RenderQueue = (Queue)st.Queue;
            if (!j.contains("queueIndex")) ma->QueueIndex = st.QueueIndex;
            m.AlphaClip = ma->RenderQueue == Queue::AlphaTest;
        }

        // Typed store for every linked-shader property that isn't a built-in Material field
        // (#354). Value comes from the .mat "properties" object; a missing key falls back to the
        // property's declared default. Needs the resolved shader for the type + default list.
        if (ma->Shader) {
            const json props = (matVersion >= 2 && j.contains("properties") && j["properties"].is_object())
                                   ? j["properties"] : json::object();
            for (const ShaderProperty& p : ma->Shader->Properties()) {
                if (MaterialAsset::IsBuiltinProp(p.Name)) continue;
                MaterialProp mp;
                mp.Type = p.Type;
                const Reader pr{props, path};
                const json* v = pr.Find(p.Name.c_str());
                switch (p.Type) {
                case ShaderPropType::Float: case ShaderPropType::Int:
                    mp.F = pr.Num(p.Name.c_str(), p.DefaultFloat);
                    mp.I = p.Type == ShaderPropType::Int ? pr.Int(p.Name.c_str(), (int)p.DefaultFloat) : (int)mp.F;
                    if (p.Type == ShaderPropType::Int) mp.F = (float)mp.I;
                    break;
                case ShaderPropType::Bool:
                    mp.B = pr.Bool(p.Name.c_str(), p.DefaultBool);
                    break;
                case ShaderPropType::Color: case ShaderPropType::Vec2:
                case ShaderPropType::Vec3:  case ShaderPropType::Vec4:
                    // #105 — 2..4 components; older files wrote Vec2/Vec4/Color as 3 floats.
                    mp.V = v ? JsonToVec4(*v, p.DefaultVec) : p.DefaultVec;
                    break;
                case ShaderPropType::Texture2D:
                    mp.TexPath = v && v->is_string() ? v->get<std::string>() : std::string();
                    mp.Tex = mp.TexPath.empty() ? nullptr : lib->LoadTexture(mp.TexPath);
                    break;
                }
                ma->Mat.ExtraProps.emplace(p.Name, std::move(mp));
            }
        }
    }

    return ma;
}

bool MaterialAsset::Save() const {
    const auto& m = Mat;
    json j;
    j["factorsScaleMaps"] = true; // #102 — see UpgradeLegacyMapFactors

    if (!ShaderPath.empty()) {
        // v2 format: shader reference + properties map
        j["matVersion"]  = 2;
        j["name"]        = Name;
        j["shader"]      = ShaderPath;
        if (ShaderPath.rfind("engine://", 0) != 0) { // #208 - see LoadMaterialFromJson
            const std::string resolved = ResolveShaderPath(ShaderPath);
            std::error_code ec;
            if (std::filesystem::exists(resolved, ec) &&
                !std::filesystem::path(ProjectPaths::Relativize(resolved)).is_absolute()) { // a project file, not the engine fallback
                const AssetGuid g = AssetDatabase::EnsureGuid(resolved);
                if (g.IsValid()) j["shaderGuid"] = g.ToString();
            }
        }
        if (!m.ShaderKeywords.empty()) j["keywords"] = m.ShaderKeywords; // #104 custom keywords
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
        // #102 / #113 surface options (non-defaults only)
        if (m.UVTiling != glm::vec2(1.0f))          props["_UVTiling"]       = {m.UVTiling.x, m.UVTiling.y};
        if (m.UVOffset != glm::vec2(0.0f))          props["_UVOffset"]       = {m.UVOffset.x, m.UVOffset.y};
        if (m.NormalStrength != 1.0f)               props["_NormalStrength"] = m.NormalStrength;
        if (m.NormalFlipY)                          props["_NormalFlipY"]    = true;
        if (m.DoubleSided)                          props["_DoubleSided"]    = true;
        if (m.UseVertexColor)                       props["_VertexColors"]   = true;
        if (!HeightMapPath.empty())                 props["_HeightMap"]      = HeightMapPath;
        if (m.ParallaxScale != 0.02f)               props["_ParallaxScale"]  = m.ParallaxScale;
        if (!DetailAlbedoMapPath.empty())           props["_DetailAlbedoMap"] = DetailAlbedoMapPath;
        if (!DetailNormalMapPath.empty())           props["_DetailNormalMap"] = DetailNormalMapPath;
        if (m.DetailTiling != glm::vec2(4.0f))      props["_DetailTiling"]   = {m.DetailTiling.x, m.DetailTiling.y};
        if (m.TransmissionStrength != 0.0f)         props["_TransmissionStrength"]    = m.TransmissionStrength;
        if (m.IOR                  != 1.5f)         props["_IOR"]                     = m.IOR;
        if (m.SubsurfaceEnabled)                    props["_SubsurfaceEnabled"]      = true;
        if (m.ReflectionProbes)                     props["_ReflectionProbes"]       = true;

        // Non-builtin linked-shader properties (#354).
        for (const auto& [name, p] : m.ExtraProps) {
            switch (p.Type) {
            // #105 — each by its declared type: Int as an integer, Vec2 as 2 numbers, Vec4/Color
            // with w (both used to be written as 3 floats, losing w).
            case ShaderPropType::Float:                             props[name] = p.F; break;
            case ShaderPropType::Int:                               props[name] = p.I; break;
            case ShaderPropType::Bool:                              props[name] = p.B; break;
            case ShaderPropType::Vec2:                              props[name] = {p.V.x, p.V.y}; break;
            case ShaderPropType::Vec3:                              props[name] = Vec3ToJson(glm::vec3(p.V)); break;
            case ShaderPropType::Color: case ShaderPropType::Vec4:  props[name] = {p.V.x, p.V.y, p.V.z, p.V.w}; break;
            case ShaderPropType::Texture2D:                         props[name] = p.TexPath; break;
            }
        }
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
        // #105 — v1 used to drop the lobes, so a clear-coat / sheen / transmission v1 material
        // lost them on reload. Written only when not default.
        if (m.ClearCoat           != 0.0f) j["clearCoat"]          = m.ClearCoat;
        if (m.ClearCoatRoughness  != 0.5f) j["clearCoatRoughness"] = m.ClearCoatRoughness;
        if (m.Anisotropy          != 0.0f) j["anisotropy"]         = m.Anisotropy;
        if (m.AnisotropyRotation  != 0.0f) j["anisotropyRotation"] = m.AnisotropyRotation;
        if (!ClearCoatMapPath.empty())      j["clearCoatMap"]       = ClearCoatMapPath;
        if (m.Sheen != glm::vec3(0.0f))    j["sheen"]              = Vec3ToJson(m.Sheen);
        if (m.SheenRoughness      != 0.5f) j["sheenRoughness"]     = m.SheenRoughness;
        if (m.SubsurfaceColor != glm::vec3(1.0f, 0.8f, 0.6f)) j["subsurfaceColor"] = Vec3ToJson(m.SubsurfaceColor);
        if (m.Thickness           != 0.5f) j["thickness"]          = m.Thickness;
        if (!ThicknessMapPath.empty())      j["thicknessMap"]       = ThicknessMapPath;
        if (m.UVTiling != glm::vec2(1.0f))  j["uvTiling"]           = {m.UVTiling.x, m.UVTiling.y};
        if (m.UVOffset != glm::vec2(0.0f))  j["uvOffset"]           = {m.UVOffset.x, m.UVOffset.y};
        if (m.NormalStrength != 1.0f)       j["normalStrength"]     = m.NormalStrength;
        if (m.NormalFlipY)                  j["normalFlipY"]        = true;
        if (m.DoubleSided)                  j["doubleSided"]        = true;
        if (m.UseVertexColor)               j["vertexColors"]       = true;
        if (!HeightMapPath.empty())         j["heightMap"]          = HeightMapPath;
        if (m.ParallaxScale != 0.02f)       j["parallaxScale"]      = m.ParallaxScale;
        if (!DetailAlbedoMapPath.empty())   j["detailAlbedoMap"]    = DetailAlbedoMapPath;
        if (!DetailNormalMapPath.empty())   j["detailNormalMap"]    = DetailNormalMapPath;
        if (m.DetailTiling != glm::vec2(4.0f)) j["detailTiling"]    = {m.DetailTiling.x, m.DetailTiling.y};
        if (m.TransmissionStrength != 0.0f) j["transmission"]      = m.TransmissionStrength;
        if (m.IOR                 != 1.5f) j["ior"]                = m.IOR;
        if (m.SubsurfaceEnabled)            j["subsurface"]         = true;
        if (m.ReflectionProbes)             j["reflectionProbes"]   = true;
    }
    // Queue / surface fields, both formats (#105 — v1 used to drop them).
    // Written explicitly when the shader declares a Queue, so choosing Opaque on a shader whose
    // default is Transparent survives a reload (#104).
    const bool shaderHasQueue = Shader && Shader->RenderState().Queue >= 0;
    if (RenderQueue != Queue::Opaque || shaderHasQueue) j["renderQueue"] = (int)RenderQueue;
    if (QueueIndex  != 2000)          j["queueIndex"]  = QueueIndex;
    if (Opacity     != 1.0f)          j["opacity"]     = Opacity;
    if (m.AlphaCutoff != 0.5f)        j["alphaCutoff"] = m.AlphaCutoff; // #101

    // Atomic write: a crash mid-save must not truncate the .mat (audit CPP-206).
    WriteTextureGuids(j); // #132
    return AtomicFile::WriteJson(Path, j);
}

// --- Property access by shader property name ---

void MaterialAsset::UpgradeLegacyMapFactors(Material& m, bool savedWithScaling) {
    if (savedWithScaling) return;
    if (m.MetallicMap || m.MetallicRoughnessMap) m.Metallic = 1.0f;
    if (m.RoughnessMap || m.MetallicRoughnessMap) m.Roughness = 1.0f;
}

void MaterialAsset::DefaultFactorsForNewMap(Material& m, const std::string& n) {
    if (n == "_EmissiveMap" && m.EmissiveMap && m.EmissiveColor == glm::vec3(0.0f)) m.EmissiveColor = glm::vec3(1.0f);
    if ((n == "_MetallicMap" && m.MetallicMap) || (n == "_MetallicRoughnessMap" && m.MetallicRoughnessMap)) m.Metallic = 1.0f;
    if ((n == "_RoughnessMap" && m.RoughnessMap) || (n == "_MetallicRoughnessMap" && m.MetallicRoughnessMap)) m.Roughness = 1.0f;
}

bool MaterialAsset::IsBuiltinProp(const std::string& n) {
    static const std::unordered_set<std::string> kBuiltin = {
        "_BaseColor", "_Metallic", "_Roughness", "_EmissiveColor", "_EmissiveStrength",
        "_Triplanar", "_TriplanarScale",
        "_ClearCoat", "_ClearCoatRoughness", "_Anisotropy", "_AnisotropyRotation",
        "_Sheen", "_SheenRoughness", "_SubsurfaceColor", "_Thickness",
        "_TransmissionStrength", "_IOR",
        "_AlbedoMap", "_NormalMap", "_MetallicRoughnessMap", "_MetallicMap", "_RoughnessMap",
        "_AOMap", "_EmissiveMap", "_ClearCoatMap", "_ThicknessMap",
        "_UVTiling", "_UVOffset", "_NormalStrength", "_NormalFlipY", "_DoubleSided", "_VertexColors",
        "_HeightMap", "_ParallaxScale", "_DetailAlbedoMap", "_DetailNormalMap", "_DetailTiling",
    };
    return kBuiltin.count(n) != 0;
}

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
    if (n == "_HeightMap")            return m.HeightMap;
    if (n == "_DetailAlbedoMap")      return m.DetailAlbedoMap;
    if (n == "_DetailNormalMap")      return m.DetailNormalMap;
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() ? it->second.Tex : sNull;
}

glm::vec3 MaterialAsset::GetColor(const Material& m, const std::string& n) {
    if (n == "_BaseColor")      return m.BaseColor;
    if (n == "_EmissiveColor")  return m.EmissiveColor * m.EmissiveStrength; // pre-multiply
    if (n == "_Sheen")          return m.Sheen;
    if (n == "_SubsurfaceColor")return m.SubsurfaceColor;
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() ? glm::vec3(it->second.V) : glm::vec3(0.0f);
}

glm::vec3 MaterialAsset::GetAuthoredColor(const Material& m, const std::string& n) {
    if (n == "_EmissiveColor") return m.EmissiveColor;
    return GetColor(m, n);
}

int MaterialAsset::GetInt(const Material& m, const std::string& n) {
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() ? it->second.I : 0;
}

glm::vec4 MaterialAsset::GetVec(const Material& m, const std::string& n) {
    if (n == "_UVTiling")     return glm::vec4(m.UVTiling, 0.0f, 0.0f);
    if (n == "_UVOffset")     return glm::vec4(m.UVOffset, 0.0f, 0.0f);
    if (n == "_DetailTiling") return glm::vec4(m.DetailTiling, 0.0f, 0.0f);
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() ? it->second.V : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
}

void MaterialAsset::SetInt(Material& m, const std::string& n, int v) {
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) { it->second.I = v; it->second.F = (float)v; }
}

void MaterialAsset::SetVec(Material& m, const std::string& n, const glm::vec4& v) {
    if (n == "_UVTiling")     { m.UVTiling = glm::vec2(v); return; }
    if (n == "_UVOffset")     { m.UVOffset = glm::vec2(v); return; }
    if (n == "_DetailTiling") { m.DetailTiling = glm::vec2(v); return; }
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) it->second.V = v;
}

void MaterialAsset::SyncTexturePathsFromMat() {
    auto pathOf = [](const std::shared_ptr<Texture>& t) { return t ? t->Path() : std::string(); };
    AlbedoMapPath            = pathOf(Mat.AlbedoMap);
    NormalMapPath            = pathOf(Mat.NormalMap);
    MetallicRoughnessMapPath = pathOf(Mat.MetallicRoughnessMap);
    MetallicMapPath          = pathOf(Mat.MetallicMap);
    RoughnessMapPath         = pathOf(Mat.RoughnessMap);
    AOMapPath                = pathOf(Mat.AOMap);
    EmissiveMapPath          = pathOf(Mat.EmissiveMap);
    ClearCoatMapPath         = pathOf(Mat.ClearCoatMap);  // #104 — these two were never synced by
    ThicknessMapPath         = pathOf(Mat.ThicknessMap);  // the Inspector, so edits didn't save
    HeightMapPath            = pathOf(Mat.HeightMap);
    DetailAlbedoMapPath      = pathOf(Mat.DetailAlbedoMap);
    DetailNormalMapPath      = pathOf(Mat.DetailNormalMap);
    for (auto& [name, p] : Mat.ExtraProps)
        if (p.Type == ShaderPropType::Texture2D) p.TexPath = pathOf(p.Tex);
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
    if (n == "_NormalStrength")          return m.NormalStrength;
    if (n == "_ParallaxScale")           return m.ParallaxScale;
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() ? it->second.F : 0.0f;
}

bool MaterialAsset::GetBool(const Material& m, const std::string& n) {
    if (n == "_Triplanar") return m.Triplanar;
    if (n == "_NormalFlipY") return m.NormalFlipY;
    if (n == "_DoubleSided") return m.DoubleSided;
    if (n == "_VertexColors") return m.UseVertexColor;
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() && it->second.B;
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
    if (n == "_HeightMap")            { m.HeightMap = tex; return; }
    if (n == "_DetailAlbedoMap")      { m.DetailAlbedoMap = tex; return; }
    if (n == "_DetailNormalMap")      { m.DetailNormalMap = tex; return; }
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) { it->second.Tex = tex; it->second.TexPath = tex ? tex->Path() : std::string(); }
}

void MaterialAsset::SetColor(Material& m, const std::string& n, const glm::vec3& v) {
    if (n == "_BaseColor")       { m.BaseColor = v; return; }
    if (n == "_EmissiveColor")   { m.EmissiveColor = v; return; }
    if (n == "_Sheen")           { m.Sheen = v; return; }
    if (n == "_SubsurfaceColor") { m.SubsurfaceColor = v; return; }
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) it->second.V = glm::vec4(v, it->second.V.w);
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
    if (n == "_NormalStrength")          { m.NormalStrength = v; return; }
    if (n == "_ParallaxScale")           { m.ParallaxScale = v; return; }
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) { it->second.F = v; it->second.I = (int)v; }
}

void MaterialAsset::SetBool(Material& m, const std::string& n, bool v) {
    if (n == "_Triplanar") { m.Triplanar = v; return; }
    if (n == "_NormalFlipY") { m.NormalFlipY = v; return; }
    if (n == "_DoubleSided") { m.DoubleSided = v; return; }
    if (n == "_VertexColors") { m.UseVertexColor = v; return; }
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) it->second.B = v;
}

std::shared_ptr<MaterialAsset> MaterialAsset::CreateDefault(const std::string& path) {
    auto ma = std::make_shared<MaterialAsset>();
    ma->Path = path;
    ma->Name = std::filesystem::path(path).stem().string();
    // Mat defaults are already correct: white, 0 metallic, 0.5 roughness.
    // #87 — new materials are v2, linked to the Standard shader like every other authored
    // material, instead of the legacy shader-less v1 format that doesn't persist transparency or
    // the advanced lobes. #104: the engine's own copy, the one Standard.shader.
    std::error_code ec;
    if (std::filesystem::exists(ShaderLibrary::ResolveRef("engine://Standard.shader", {}), ec))
        ma->ShaderPath = "engine://Standard.shader";
    if (!ma->Save()) return nullptr;
    return ma;
}
