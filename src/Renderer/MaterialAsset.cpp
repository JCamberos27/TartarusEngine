#include "MaterialAsset.h"
#include "ShaderAsset.h"
#include "AssetLibrary.h"
#include "Texture.h"
#include "ShaderLibrary.h" // engine:// shader references (#104)
#include "Log.h"
#include "AtomicFile.h"
#include "Core/Paths.h"
#include "AssetDatabase.h" // #208 - shaderGuid
#include "SubstrateJson.h"

#include <json.hpp>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include "RenderSettingsEpoch.h"

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


// W2-084: every built-in Material field a .mat stores, in one table. Load, Save (v1 and v2),
// IsBuiltinProp, the Get*/Set* accessors, ResolveBuiltin and SyncTexturePathsFromMat all walk
// it, so a new field is one row here instead of edits in eight places.
using TexUse = AssetLibrary::TextureUse;
enum class BuiltinKind : std::uint8_t { Float, Int, Bool, Color, Vec2, Tex };
constexpr float kNoMin = -FLT_MAX, kNoMax = FLT_MAX;

struct Builtin {
    const char* Name  = nullptr;  // the shader property name, also the v2 "properties" key
    const char* V1Key = nullptr;  // the flat v1 key
    BuiltinKind Kind  = BuiltinKind::Float;
    // The field, by kind (only the one matching Kind is set).
    float& (*F)(Material&) = nullptr;
    int& (*I)(Material&) = nullptr;
    bool& (*B)(Material&) = nullptr;
    glm::vec3& (*C)(Material&) = nullptr;
    glm::vec2& (*V2)(Material&) = nullptr;
    std::shared_ptr<Texture>& (*T)(Material&) = nullptr;
    std::string MaterialAsset::* Path = nullptr; // Tex: the saved path beside the texture
    TexUse Use = TexUse::Data;                   // Tex: #369, only colour maps are sRGB
    float Min = kNoMin, Max = kNoMax;            // Float / Int: Load and Set clamp to this
    bool Always = false;          // Save writes it even at its default (the original core keys)
    bool Prop = true;             // false: a variant opt-in kept in the file, not a shader property (#354)
    bool TimesStrength = false;   // _EmissiveColor: GetColor / the bind pre-multiply _EmissiveStrength
};

constexpr Builtin Flt(const char* n, const char* v1, float& (*f)(Material&), bool always = false, float lo = kNoMin, float hi = kNoMax) {
    Builtin b{}; b.Name = n; b.V1Key = v1; b.Kind = BuiltinKind::Float; b.F = f; b.Always = always; b.Min = lo; b.Max = hi; return b;
}
constexpr Builtin Int(const char* n, const char* v1, int& (*f)(Material&), int lo, int hi) {
    Builtin b{}; b.Name = n; b.V1Key = v1; b.Kind = BuiltinKind::Int; b.I = f; b.Min = (float)lo; b.Max = (float)hi; return b;
}
constexpr Builtin Bln(const char* n, const char* v1, bool& (*f)(Material&), bool always = false, bool prop = true) {
    Builtin b{}; b.Name = n; b.V1Key = v1; b.Kind = BuiltinKind::Bool; b.B = f; b.Always = always; b.Prop = prop; return b;
}
constexpr Builtin Col(const char* n, const char* v1, glm::vec3& (*f)(Material&), bool always = false, bool timesStrength = false) {
    Builtin b{}; b.Name = n; b.V1Key = v1; b.Kind = BuiltinKind::Color; b.C = f; b.Always = always; b.TimesStrength = timesStrength; return b;
}
constexpr Builtin Vc2(const char* n, const char* v1, glm::vec2& (*f)(Material&)) {
    Builtin b{}; b.Name = n; b.V1Key = v1; b.Kind = BuiltinKind::Vec2; b.V2 = f; return b;
}
constexpr Builtin Map(const char* n, const char* v1, std::shared_ptr<Texture>& (*f)(Material&), std::string MaterialAsset::* path,
                      TexUse use = TexUse::Data, bool always = false) {
    Builtin b{}; b.Name = n; b.V1Key = v1; b.Kind = BuiltinKind::Tex; b.T = f; b.Path = path; b.Use = use; b.Always = always; return b;
}

#define MAT_FIELD(T, field) [](Material& m) -> T& { return m.field; }
constexpr Builtin kBuiltins[] = {
    Col("_BaseColor",            "baseColor",            MAT_FIELD(glm::vec3, BaseColor), true),
    Flt("_Metallic",             "metallic",             MAT_FIELD(float, Metallic), true),
    Flt("_Roughness",            "roughness",            MAT_FIELD(float, Roughness), true),
    Col("_EmissiveColor",        "emissiveColor",        MAT_FIELD(glm::vec3, EmissiveColor), true, true),
    Flt("_EmissiveStrength",     "emissiveStrength",     MAT_FIELD(float, EmissiveStrength), true),
    Bln("_Triplanar",            "triplanar",            MAT_FIELD(bool, Triplanar), true),
    Flt("_TriplanarScale",       "triplanarScale",       MAT_FIELD(float, TriplanarScale), true),
    Flt("_TriplanarBreakup",     "triplanarBreakup",     MAT_FIELD(float, TriplanarBreakup)),
    // PR10-12 lobes
    Flt("_ClearCoat",            "clearCoat",            MAT_FIELD(float, ClearCoat)),
    Flt("_ClearCoatRoughness",   "clearCoatRoughness",   MAT_FIELD(float, ClearCoatRoughness)),
    Flt("_Anisotropy",           "anisotropy",           MAT_FIELD(float, Anisotropy)),
    Flt("_AnisotropyRotation",   "anisotropyRotation",   MAT_FIELD(float, AnisotropyRotation)),
    Col("_Sheen",                "sheen",                MAT_FIELD(glm::vec3, Sheen)),
    Flt("_SheenRoughness",       "sheenRoughness",       MAT_FIELD(float, SheenRoughness)),
    Col("_SubsurfaceColor",      "subsurfaceColor",      MAT_FIELD(glm::vec3, SubsurfaceColor)),
    Flt("_Thickness",            "thickness",            MAT_FIELD(float, Thickness)),
    Flt("_TransmissionStrength", "transmission",         MAT_FIELD(float, TransmissionStrength)),
    Flt("_IOR",                  "ior",                  MAT_FIELD(float, IOR)),
    // #354: variant opt-ins with no natural "off" value (not shader Properties()).
    Bln("_SubsurfaceEnabled",    "subsurface",           MAT_FIELD(bool, SubsurfaceEnabled), false, false),
    Bln("_ReflectionProbes",     "reflectionProbes",     MAT_FIELD(bool, ReflectionProbes), false, false),
    // Maps (the first seven are always written, even empty)
    Map("_AlbedoMap",            "albedoMap",            MAT_FIELD(std::shared_ptr<Texture>, AlbedoMap), &MaterialAsset::AlbedoMapPath, TexUse::Color, true),
    Map("_NormalMap",            "normalMap",            MAT_FIELD(std::shared_ptr<Texture>, NormalMap), &MaterialAsset::NormalMapPath, TexUse::Normal, true),
    Map("_MetallicRoughnessMap", "metallicRoughnessMap", MAT_FIELD(std::shared_ptr<Texture>, MetallicRoughnessMap), &MaterialAsset::MetallicRoughnessMapPath, TexUse::Data, true),
    Map("_MetallicMap",          "metallicMap",          MAT_FIELD(std::shared_ptr<Texture>, MetallicMap), &MaterialAsset::MetallicMapPath, TexUse::Data, true),
    Map("_RoughnessMap",         "roughnessMap",         MAT_FIELD(std::shared_ptr<Texture>, RoughnessMap), &MaterialAsset::RoughnessMapPath, TexUse::Data, true),
    Map("_AOMap",                "aoMap",                MAT_FIELD(std::shared_ptr<Texture>, AOMap), &MaterialAsset::AOMapPath, TexUse::Data, true),
    Map("_EmissiveMap",          "emissiveMap",          MAT_FIELD(std::shared_ptr<Texture>, EmissiveMap), &MaterialAsset::EmissiveMapPath, TexUse::Color, true),
    Map("_ClearCoatMap",         "clearCoatMap",         MAT_FIELD(std::shared_ptr<Texture>, ClearCoatMap), &MaterialAsset::ClearCoatMapPath),
    Map("_ThicknessMap",         "thicknessMap",         MAT_FIELD(std::shared_ptr<Texture>, ThicknessMap), &MaterialAsset::ThicknessMapPath),
    // #102 / #113 surface options
    Map("_HeightMap",            "heightMap",            MAT_FIELD(std::shared_ptr<Texture>, HeightMap), &MaterialAsset::HeightMapPath),
    Map("_DetailAlbedoMap",      "detailAlbedoMap",      MAT_FIELD(std::shared_ptr<Texture>, DetailAlbedoMap), &MaterialAsset::DetailAlbedoMapPath, TexUse::Color),
    Map("_DetailNormalMap",      "detailNormalMap",      MAT_FIELD(std::shared_ptr<Texture>, DetailNormalMap), &MaterialAsset::DetailNormalMapPath, TexUse::Normal),
    Map("_GrungeMap",            "grungeMap",            MAT_FIELD(std::shared_ptr<Texture>, GrungeMap), &MaterialAsset::GrungeMapPath, TexUse::Color),
    Flt("_GrungeStrength",       "grungeStrength",       MAT_FIELD(float, GrungeStrength)),
    Flt("_GrungeScale",          "grungeScale",          MAT_FIELD(float, GrungeScale)),
    Bln("_ReceiveDecals",        "receiveDecals",        MAT_FIELD(bool, ReceiveDecals)),
    Bln("_NoWetness",            "noWetness",            MAT_FIELD(bool, NoWetness)),
    Bln("_Foliage",              "foliage",              MAT_FIELD(bool, Foliage)),
    Flt("_WindStrength",         "windStrength",         MAT_FIELD(float, WindStrength)),
    Flt("_WindFrequency",        "windFrequency",        MAT_FIELD(float, WindFrequency)),
    Flt("_WindFlutter",          "windFlutter",          MAT_FIELD(float, WindFlutter)),
    Flt("_WindHeight",           "windHeight",           MAT_FIELD(float, WindHeight)),
    Flt("_Translucency",         "translucency",         MAT_FIELD(float, Translucency)),
    Vc2("_UVTiling",             "uvTiling",             MAT_FIELD(glm::vec2, UVTiling)),
    Vc2("_UVOffset",             "uvOffset",             MAT_FIELD(glm::vec2, UVOffset)),
    Vc2("_DetailTiling",         "detailTiling",         MAT_FIELD(glm::vec2, DetailTiling)),
    Flt("_NormalStrength",       "normalStrength",       MAT_FIELD(float, NormalStrength)),
    Bln("_NormalFlipY",          "normalFlipY",          MAT_FIELD(bool, NormalFlipY)),
    Bln("_DoubleSided",          "doubleSided",          MAT_FIELD(bool, DoubleSided)),
    Bln("_VertexColors",         "vertexColors",         MAT_FIELD(bool, UseVertexColor)),
    Bln("_SpecularHighlights",   "specularHighlights",   MAT_FIELD(bool, SpecularHighlights)),
    Bln("_GlossyReflections",    "glossyReflections",    MAT_FIELD(bool, GlossyReflections)),
    Flt("_ParallaxScale",        "parallaxScale",        MAT_FIELD(float, ParallaxScale)),
    // Shell fur (Material::FurDesc); read by FurRenderer, not the surface program.
    Bln("_FurEnabled",           "furEnabled",           MAT_FIELD(bool, Fur.Enabled)),
    Flt("_FurLength",            "furLength",            MAT_FIELD(float, Fur.Length), false, 0.0f),
    Flt("_FurDensity",           "furDensity",           MAT_FIELD(float, Fur.Density), false, 1.0f),
    Int("_FurShells",            "furShells",            MAT_FIELD(int, Fur.Shells), 0, Material::kMaxFurShells),
    Flt("_FurGravity",           "furGravity",           MAT_FIELD(float, Fur.Gravity)),
    Col("_FurTint",              "furTint",              MAT_FIELD(glm::vec3, Fur.Tint)),
    Flt("_FurRim",               "furRim",               MAT_FIELD(float, Fur.Rim), false, 0.0f),
    Map("_FurMask",              "furMask",              MAT_FIELD(std::shared_ptr<Texture>, Fur.Mask), &MaterialAsset::FurMaskPath),
};
#undef MAT_FIELD
constexpr std::size_t kBuiltinCount = sizeof(kBuiltins) / sizeof(kBuiltins[0]);

// Field reads on a const Material (the table holds one mutable accessor per field).
template <class T> const T& ReadField(T& (*f)(Material&), const Material& m) { return f(const_cast<Material&>(m)); }

const Material& DefaultMaterial() { static const Material kDefault; return kDefault; }

// A shader-property built-in by name (the variant opt-ins are not shader properties).
const Builtin* FindBuiltin(const std::string& n) {
    static const std::unordered_map<std::string_view, const Builtin*> kByName = [] {
        std::unordered_map<std::string_view, const Builtin*> map;
        for (const Builtin& b : kBuiltins) if (b.Prop) map.emplace(b.Name, &b);
        return map;
    }();
    const auto it = kByName.find(n);
    return it != kByName.end() ? it->second : nullptr;
}
const Builtin* FindBuiltin(const std::string& n, BuiltinKind k) {
    const Builtin* b = FindBuiltin(n);
    return b && b->Kind == k ? b : nullptr;
}

// The value the renderer binds: .x for Float / Int / Bool (0 or 1), .xy for Vec2, .xyz for Color.
glm::vec4 BindValue(const Builtin& b, const Material& m) {
    switch (b.Kind) {
    case BuiltinKind::Float: return {ReadField(b.F, m), 0.0f, 0.0f, 0.0f};
    case BuiltinKind::Int:   return {(float)ReadField(b.I, m), 0.0f, 0.0f, 0.0f};
    case BuiltinKind::Bool:  return {ReadField(b.B, m) ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    case BuiltinKind::Color: return glm::vec4(ReadField(b.C, m) * (b.TimesStrength ? m.EmissiveStrength : 1.0f), 0.0f);
    case BuiltinKind::Vec2:  return glm::vec4(ReadField(b.V2, m), 0.0f, 0.0f);
    case BuiltinKind::Tex:   break;
    }
    return glm::vec4(0.0f);
}
// ResolveBuiltin hands out plain function pointers (ShaderAsset keeps them per property), one per row.
template <std::size_t I> glm::vec4 BindValueAt(const Material& m) { return BindValue(kBuiltins[I], m); }
template <std::size_t I> const std::shared_ptr<Texture>& TextureAt(const Material& m) { return ReadField(kBuiltins[I].T, m); }
using BindValFn = glm::vec4 (*)(const Material&);
using BindTexFn = const std::shared_ptr<Texture>& (*)(const Material&);
template <std::size_t... I> constexpr std::array<BindValFn, sizeof...(I)> BindValFns(std::index_sequence<I...>) { return {{&BindValueAt<I>...}}; }
template <std::size_t... I> constexpr std::array<BindTexFn, sizeof...(I)> BindTexFns(std::index_sequence<I...>) { return {{&TextureAt<I>...}}; }
constexpr auto kBindVal = BindValFns(std::make_index_sequence<kBuiltinCount>{});
constexpr auto kBindTex = BindTexFns(std::make_index_sequence<kBuiltinCount>{});

void LoadBuiltin(const Builtin& b, const Reader& r, const char* k, MaterialAsset& ma) {
    Material& m = ma.Mat;
    const Material& def = DefaultMaterial();
    switch (b.Kind) {
    case BuiltinKind::Float: b.F(m) = std::clamp(r.Num(k, ReadField(b.F, def)), b.Min, b.Max); break;
    case BuiltinKind::Int:   b.I(m) = std::clamp((int)std::lround(r.Num(k, (float)ReadField(b.I, def))), (int)b.Min, (int)b.Max); break;
    case BuiltinKind::Bool:  b.B(m) = r.Bool(k, ReadField(b.B, def)); break;
    case BuiltinKind::Color: b.C(m) = r.Vec3(k, ReadField(b.C, def)); break;
    case BuiltinKind::Vec2:  b.V2(m) = r.Vec2(k, ReadField(b.V2, def)); break;
    case BuiltinKind::Tex:   ma.*b.Path = r.Str(k); break;
    }
}

// Writes each built-in under its v2 or v1 key: the Always rows every time, the rest only when not
// at Material's default (so a file only names what it changes).
void SaveBuiltins(json& out, bool v2, const MaterialAsset& ma) {
    const Material& m = ma.Mat;
    const Material& def = DefaultMaterial();
    for (const Builtin& b : kBuiltins) {
        const char* k = v2 ? b.Name : b.V1Key;
        switch (b.Kind) {
        case BuiltinKind::Float: if (b.Always || ReadField(b.F, m) != ReadField(b.F, def)) out[k] = ReadField(b.F, m); break;
        case BuiltinKind::Int:   if (b.Always || ReadField(b.I, m) != ReadField(b.I, def)) out[k] = ReadField(b.I, m); break;
        case BuiltinKind::Bool:  if (b.Always || ReadField(b.B, m) != ReadField(b.B, def)) out[k] = ReadField(b.B, m); break;
        case BuiltinKind::Color: if (b.Always || ReadField(b.C, m) != ReadField(b.C, def)) out[k] = Vec3ToJson(ReadField(b.C, m)); break;
        case BuiltinKind::Vec2: {
            const glm::vec2& v = ReadField(b.V2, m);
            if (b.Always || v != ReadField(b.V2, def)) out[k] = {v.x, v.y};
            break;
        }
        case BuiltinKind::Tex: if (b.Always || !(ma.*b.Path).empty()) out[k] = ma.*b.Path; break;
        }
    }
}
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

static std::shared_ptr<MaterialAsset> LoadMaterialFromJson(const json& j, const std::string& path, AssetLibrary* lib);

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
// Save side: texture paths under the project are written project-relative, so a .mat is
// portable between machines and checkouts (an absolute C:/Users/... path only works on the
// machine that saved it). Paths outside the project stay absolute.
void RelativizeTexturePaths(json& j) {
    const bool v2 = j.contains("properties") && j["properties"].is_object();
    json& props = v2 ? j["properties"] : j;
    for (auto& [key, value] : props.items()) {
        if (!value.is_string() || key == "name" || key == "shader") continue;
        const std::string stored = value.get<std::string>();
        if (stored.empty() || !std::filesystem::path(stored).is_absolute() || !IsTextureAsset(stored)) continue;
        value = ProjectPaths::Relativize(stored);
    }
}

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
static std::string ResolveShaderPath(const std::string& ref) {
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

static std::shared_ptr<MaterialAsset> LoadMaterialFromJson(const json& j, const std::string& path, AssetLibrary* lib) {
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

    // Substrate (Substrate.h): a top-level block in both formats, plain project-relative paths.
    if (auto sub = j.find("substrate"); sub != j.end()) SubstrateJson::Read(*sub, m.Substrate, SubstrateJson::PlainPath);

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
    for (const Builtin& b : kBuiltins) LoadBuiltin(b, r, v2 ? b.Name : b.V1Key, *ma);

    if (lib) {
        // A project-relative path (what Save writes, so .mat files are portable) resolves
        // against the project root, not the process's working directory.
        // #369 - only albedo, detail albedo and emissive are colour; every other map holds raw
        // values, so it must not default to sRGB (see AssetLibrary::ResolveTextureSettings).
        for (const Builtin& b : kBuiltins)
            if (b.Kind == BuiltinKind::Tex) {
                const std::string& texPath = (*ma).*b.Path;
                b.T(m) = texPath.empty() ? nullptr : lib->LoadTexture(AbsoluteAssetPath(texPath), b.Use);
            }
        MaterialAsset::UpgradeLegacyMapFactors(m, top.Bool("factorsScaleMaps", false)); // #102

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

        // The cover layer's maps are colour / normal / raw values, not sRGB all (other custom maps keep the default).
        auto CoverTexUse = [](const std::string& n) {
            using U = AssetLibrary::TextureUse;
            if (n.rfind("_Cover", 0) != 0) return U::Color;
            return n.find("Normal") != std::string::npos ? U::Normal : n.find("Albedo") != std::string::npos ? U::Color : U::Data;
        };
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
                    mp.Tex = mp.TexPath.empty() ? nullptr : lib->LoadTexture(AbsoluteAssetPath(mp.TexPath), CoverTexUse(p.Name));
                    break;
                }
                ma->Mat.ExtraProps.emplace(p.Name, std::move(mp));
            }
        }
    }

    return ma;
}

bool MaterialAsset::SetShader(const std::string& reference, AssetLibrary& assets) {
    const std::string ref = reference.empty() ? "engine://Standard.shader" : reference;
    auto shader = assets.LoadShader(ResolveShaderPath(ref));
    if (!shader) return false;
    Shader = std::move(shader); ShaderPath = ref;
    for (const auto& p : Shader->Properties()) {
        if (IsBuiltinProp(p.Name)) continue;
        const auto existing = Mat.ExtraProps.find(p.Name);
        if (existing != Mat.ExtraProps.end() && existing->second.Type == p.Type) continue;
        MaterialProp value; value.Type = p.Type;
        value.F = p.DefaultFloat; value.I = (int)p.DefaultFloat;
        value.B = p.DefaultBool; value.V = p.DefaultVec;
        Mat.ExtraProps[p.Name] = std::move(value);
    }
    const auto& state = Shader->RenderState();
    RenderQueue = state.Queue >= 0 ? (Queue)state.Queue : Queue::Opaque;
    NoteRenderSettingsChanged();
    QueueIndex = state.Queue >= 0 ? state.QueueIndex : 2000;
    Mat.AlphaClip = RenderQueue == Queue::AlphaTest;
    return true;
}

bool MaterialAsset::Save() const {
    const auto& m = Mat;
    json j;
    j["factorsScaleMaps"] = true; // #102 — see UpgradeLegacyMapFactors
    if (!m.Substrate.Albedo.empty() || !m.Substrate.Preset.empty())
        j["substrate"] = SubstrateJson::Write<json>(m.Substrate, [](const std::string& p) { return ProjectPaths::Relativize(p); });

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
        SaveBuiltins(props, true, *this);

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
        SaveBuiltins(j, false, *this); // v1 keys
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
    RelativizeTexturePaths(j);
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

bool MaterialAsset::IsBuiltinProp(const std::string& n) { return FindBuiltin(n) != nullptr; }

// Get* / Set*: the built-in field of that name and kind, else the custom property (ExtraProps).
const std::shared_ptr<Texture>& MaterialAsset::GetTexture(const Material& m, const std::string& n) {
    static const std::shared_ptr<Texture> sNull;
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Tex)) return ReadField(b->T, m);
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() ? it->second.Tex : sNull;
}

glm::vec3 MaterialAsset::GetColor(const Material& m, const std::string& n) {
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Color)) return glm::vec3(BindValue(*b, m)); // emissive pre-multiplied
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() ? glm::vec3(it->second.V) : glm::vec3(0.0f);
}

glm::vec3 MaterialAsset::GetAuthoredColor(const Material& m, const std::string& n) {
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Color)) return ReadField(b->C, m);
    return GetColor(m, n);
}

int MaterialAsset::GetInt(const Material& m, const std::string& n) {
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Int)) return ReadField(b->I, m);
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() ? it->second.I : 0;
}

glm::vec4 MaterialAsset::GetVec(const Material& m, const std::string& n) {
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Vec2)) return glm::vec4(ReadField(b->V2, m), 0.0f, 0.0f);
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() ? it->second.V : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
}

float MaterialAsset::GetFloat(const Material& m, const std::string& n) {
    if (const Builtin* b = FindBuiltin(n); b && (b->Kind == BuiltinKind::Float || b->Kind == BuiltinKind::Int))
        return BindValue(*b, m).x;
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() ? it->second.F : 0.0f;
}

bool MaterialAsset::GetBool(const Material& m, const std::string& n) {
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Bool)) return ReadField(b->B, m);
    auto it = m.ExtraProps.find(n);
    return it != m.ExtraProps.end() && it->second.B;
}

void MaterialAsset::SyncTexturePathsFromMat() {
    auto pathOf = [](const std::shared_ptr<Texture>& t) { return t ? t->Path() : std::string(); };
    for (const Builtin& b : kBuiltins)
        if (b.Kind == BuiltinKind::Tex) this->*b.Path = pathOf(b.T(Mat));
    for (auto& [name, p] : Mat.ExtraProps)
        if (p.Type == ShaderPropType::Texture2D) p.TexPath = pathOf(p.Tex);
}

void MaterialAsset::ResolveBuiltin(const std::string& n, ShaderPropType type,
                                   const std::shared_ptr<Texture>& (*&tex)(const Material&),
                                   glm::vec4 (*&val)(const Material&)) {
    tex = nullptr;
    val = nullptr;
    const Builtin* b = FindBuiltin(n);
    if (!b) return;
    const std::size_t i = (std::size_t)(b - kBuiltins);
    // The getter the inspector uses for that property type: GetTexture / GetColor / GetVec /
    // GetFloat / GetBool (Bool, Int).
    switch (type) {
    case ShaderPropType::Texture2D: if (b->Kind == BuiltinKind::Tex) tex = kBindTex[i]; break;
    case ShaderPropType::Color:
    case ShaderPropType::Vec3:  if (b->Kind == BuiltinKind::Color) val = kBindVal[i]; break;
    case ShaderPropType::Vec2:  if (b->Kind == BuiltinKind::Vec2) val = kBindVal[i]; break;
    case ShaderPropType::Float: if (b->Kind == BuiltinKind::Float || b->Kind == BuiltinKind::Int) val = kBindVal[i]; break;
    case ShaderPropType::Bool:
    case ShaderPropType::Int:   if (b->Kind == BuiltinKind::Bool || b->Kind == BuiltinKind::Int) val = kBindVal[i]; break;
    default: break;
    }
}

void MaterialAsset::SetTexture(Material& m, const std::string& n, const std::shared_ptr<Texture>& tex) {
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Tex)) { b->T(m) = tex; return; }
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) { it->second.Tex = tex; it->second.TexPath = tex ? tex->Path() : std::string(); }
}

void MaterialAsset::SetColor(Material& m, const std::string& n, const glm::vec3& v) {
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Color)) { b->C(m) = v; return; }
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) it->second.V = glm::vec4(v, it->second.V.w);
}

void MaterialAsset::SetFloat(Material& m, const std::string& n, float v) {
    if (const Builtin* b = FindBuiltin(n)) {
        if (b->Kind == BuiltinKind::Float) { b->F(m) = std::clamp(v, b->Min, b->Max); return; }
        if (b->Kind == BuiltinKind::Int) { b->I(m) = std::clamp((int)std::lround(v), (int)b->Min, (int)b->Max); return; }
    }
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) { it->second.F = v; it->second.I = (int)v; }
}

void MaterialAsset::SetInt(Material& m, const std::string& n, int v) {
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Int)) { b->I(m) = std::clamp(v, (int)b->Min, (int)b->Max); return; }
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) { it->second.I = v; it->second.F = (float)v; }
}

void MaterialAsset::SetVec(Material& m, const std::string& n, const glm::vec4& v) {
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Vec2)) { b->V2(m) = glm::vec2(v); return; }
    auto it = m.ExtraProps.find(n);
    if (it != m.ExtraProps.end()) it->second.V = v;
}

void MaterialAsset::SetBool(Material& m, const std::string& n, bool v) {
    if (const Builtin* b = FindBuiltin(n, BuiltinKind::Bool)) { b->B(m) = v; return; }
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
