#include "LayerRegistry.h"
#include "SceneSerializer.h"
#include "AtomicFile.h"
#include "World.h"
#include "AssetLibrary.h"
#include "ComponentRegistry.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
#include "MaterialAsset.h"
#include "AssetDatabase.h"
#include "AssetGuid.h"
#include "ProjectPaths.h"
#include "EditorSettings.h"

#include "Log.h"

#include <json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_map>
#include <set>
#include <functional>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>
#include <cctype>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <cmath>

using json = nlohmann::json;

namespace {

// Bumped whenever a scene-file change would make an older build misread a newer save (a field
// whose meaning changed, not merely a new optional field — those need no bump at all, since every
// read here already defaults gracefully when a key is absent). #195: scenes previously carried no
// version at all, so there's no way to tell "old build, old file" apart from "old build, file from
// a build that changed something incompatible" — this is the fix. Starting at 1 rather than 0 so
// that 0 unambiguously means "no formatVersion field was written" (a legacy pre-#195 file), not
// "written by version 0".
// v3 (#9, Phase M item 1): post-processing/shadow settings move from editor_prefs.json onto the
// scene itself (see World.h's ExposureEV et al.) — they're scene-authored content, not per-user
// editor prefs. One-way: a pre-v3 file has its values migrated forward from editor_prefs.json on
// load (see ApplySceneJson's MigratePostProcessSettingsFromPrefs call) and is immediately
// re-saved by SceneSerializer::Load(), after writing a ".bak" of the original.
constexpr int kSceneFormatVersion = 3;

// Set by ApplySceneJson when the file being loaded declares a formatVersion newer than this build
// understands; read (and cleared) via SceneSerializer::TakeLoadWarning() so a caller like the
// editor can pop a dialog in addition to the Console line ApplySceneJson already logs. Plain
// (non-thread-local) static: scene loads happen on the main thread only.
std::string g_LastLoadWarning;

// Populated by ApplySceneJson during a pre-v3 migration; drained by SceneSerializer::Load() to
// log exactly what moved (review §7 Q13: "logs exactly what moved"). Plain static — same
// single-threaded assumption as g_LastLoadWarning above.
std::vector<std::string> g_MigrationLog;

// #9, Phase M item 1 — a pre-v3 scene carries no post-processing/shadow keys of its own; those
// values lived in editor_prefs.json. Read them straight off that file on disk (EditorSettings no
// longer declares these fields, so Get() can't provide them) and copy the user's real authored
// values onto World, rather than silently resetting to compiled defaults.
void MigratePostProcessSettingsFromPrefs(World& world) {
    std::ifstream in(EditorSettings::PrefsFilePath());
    // Starts as an object, not null: with no editor_prefs.json (a fresh machine, or a built game,
    // which has its own user folder) json::value() on a null threw type_error 306 and the whole
    // pre-v3 scene failed to load. Wrong-typed values fall back to the defaults the same way.
    json prefs = json::object();
    if (in.is_open()) {
        try { in >> prefs; } catch (const std::exception&) { prefs = json::object(); }
        if (!prefs.is_object()) prefs = json::object();
    }
    auto num = [&prefs](const char* key, auto fallback) {
        const auto it = prefs.find(key);
        return (it != prefs.end() && it->is_number()) ? it->get<decltype(fallback)>() : fallback;
    };
    auto flag = [&prefs](const char* key, bool fallback) {
        const auto it = prefs.find(key);
        return (it != prefs.end() && it->is_boolean()) ? it->get<bool>() : fallback;
    };
    world.ExposureEV       = num("exposureEV", 0.0f);
    world.TonemapOperator  = num("tonemapOperator", 1);
    world.MsaaSamples      = num("msaaSamples", 4);
    world.SsaoEnabled      = flag("ssaoEnabled", false);
    world.BloomEnabled     = flag("bloomEnabled", false);
    world.BloomThreshold   = num("bloomThreshold", 1.0f);
    world.BloomKnee        = num("bloomKnee", 0.5f);
    world.BloomIntensity   = num("bloomIntensity", 0.25f);
    world.ShadowsEnabled   = flag("shadowsEnabled", true);
    world.ShadowResolution = num("shadowResolution", 4096);
    world.ShadowCascades   = num("shadowCascades", 4);
    world.ShadowDistance   = num("shadowDistance", 500.0f);
    g_MigrationLog.push_back("post-processing/shadow settings (exposure=" +
        std::to_string(world.ExposureEV) + ", bloom=" + (world.BloomEnabled ? "on" : "off") +
        ", ssao=" + (world.SsaoEnabled ? "on" : "off") + ") migrated from editor_prefs.json");
}

// #302 Part B — session-lived cache of parsed .prefab files for the Inspector's per-field
// override check (called every frame per visible field on a prefab-instance entity). Cleared on
// full scene load (ApplySceneJson) and via SceneSerializer::ClearPrefabPristineCache(). The
// save-time diff reads the file directly — a save is rare and must see the latest on disk.
std::unordered_map<std::string, json> g_prefabPristineCache;

// Guards ApplySceneJson against runaway recursion when a prefab instance stub expands another
// scene fragment (#236 A2). A flattened .prefab never contains a "prefabInstances" key, so in
// normal use this only ever reaches depth 2; the cap just stops a hand-broken / cyclic file
// from looping forever. Main-thread only, same as g_LastLoadWarning.
int g_ApplyDepth = 0;
constexpr int kMaxApplyDepth = 8;
struct ApplyDepthGuard {
    ApplyDepthGuard()  { ++g_ApplyDepth; }
    ~ApplyDepthGuard() { --g_ApplyDepth; }
};

// A non-finite component anywhere in the scene (nan/inf slipped past the Inspector, or a
// corrupt file) serializes as JSON `null` / a bare `nan` token, neither of which reloads —
// the whole scene is then lost. Scrub to 0 at the one choke point every vector passes
// through, on both write and read, and say so in the Console (#34 D4).
inline float FiniteOr(float x, float fallback, const char* what) {
    if (std::isfinite(x)) return x;
    Log::Warn(std::string("Scene: non-finite ") + what + " value replaced with " + std::to_string(fallback));
    return fallback;
}

json Vec3ToJson(const glm::vec3& v) {
    return json::array({FiniteOr(v.x, 0.0f, "vector"), FiniteOr(v.y, 0.0f, "vector"), FiniteOr(v.z, 0.0f, "vector")});
}

glm::vec3 JsonToVec3(const json& j, const glm::vec3& fallback = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() != 3) return fallback;
    if (!j[0].is_number() || !j[1].is_number() || !j[2].is_number()) return fallback;
    return glm::vec3(FiniteOr(j[0].get<float>(), fallback.x, "vector"),
                     FiniteOr(j[1].get<float>(), fallback.y, "vector"),
                     FiniteOr(j[2].get<float>(), fallback.z, "vector"));
}

std::string PathOrEmpty(const std::shared_ptr<Texture>& tex) {
    return tex ? tex->Path() : std::string();
}

// --- Portable asset paths (audit #364 / BUG-101) -----------------------------------------
// Serialized scenes must not carry machine-absolute paths, or they break on any other machine
// or checkout location. On WRITE, a path under the project root becomes a '/'-normalised
// project-relative path; a path outside the project tree (a shared asset library elsewhere) is
// left absolute rather than turned into a pile of "../..". On READ, a relative path resolves
// against the current project root, and a legacy absolute path that no longer exists is rebased
// by matching its longest tail that does exist under this project root.
// A synthetic reference, not a filesystem path: World's "primitive://..." level-geometry ids,
// or any other "scheme:"-prefixed id. Never rewritten on the way in or out.
bool IsSyntheticRef(const std::string& p) {
    if (p.rfind("primitive:", 0) == 0) return true;
    // scheme:// with a multi-character scheme (a lone "C:" drive letter is not one).
    auto colon = p.find(':');
    if (colon != std::string::npos && colon >= 2) {
        bool alnum = true;
        for (size_t i = 0; i < colon; ++i) if (!std::isalnum((unsigned char)p[i])) { alnum = false; break; }
        if (alnum && colon + 1 < p.size() && p[colon + 1] == '/') return true;
    }
    return false;
}

std::string AssetPathForWrite(const std::string& path) {
    if (path.empty() || IsSyntheticRef(path)) return path;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::path(ProjectPaths::Root()), ec);
    if (ec) return path;
    fs::path abs = fs::weakly_canonical(fs::path(path), ec);
    if (ec) abs = fs::path(path);
    fs::path rel = abs.lexically_relative(root);
    if (rel.empty() || rel.begin() == rel.end()) return path;
    if (rel.begin()->string() == "..") return path; // outside the project tree
    return rel.generic_string();
}

std::string AssetPathForRead(const std::string& path) {
    if (path.empty() || IsSyntheticRef(path)) return path;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(path);
    fs::path root(ProjectPaths::Root());
    if (p.is_relative())
        return (root / p).lexically_normal().string();
    if (fs::exists(p, ec)) return path;
    // Legacy absolute path authored elsewhere: try each suffix of it under this project root.
    std::vector<fs::path> parts(p.begin(), p.end());
    for (size_t i = 0; i < parts.size(); ++i) {
        fs::path tail;
        for (size_t j = i; j < parts.size(); ++j) tail /= parts[j];
        fs::path cand = (root / tail).lexically_normal();
        if (fs::exists(cand, ec)) return cand.string();
    }
    return path; // unresolved — downstream logs a concrete "not found"
}

// PR 2 (#333): path string → {"path":"...", "pathGuid":"..."} for WRITE. Returns a plain
// string if the GUID isn't known yet so that v1-era paths still round-trip cleanly.
// The stored path is always project-relative when possible (audit #364).
json PathRef(const std::string& path) {
    if (path.empty()) return path;
    std::string stored = AssetPathForWrite(path);
    AssetGuid g = AssetDatabase::GuidForPath(path); // GUID lookup needs the real (absolute) path
    if (g.IsValid()) return json{{"path", stored}, {"pathGuid", g.ToString()}};
    return stored;
}

// PR 2 (#333): dual path+guid READ. Prefers GUID if present and resolvable, falls back to
// the plain path. Accepts both the v2 {"path","pathGuid"} object and the v1 plain string.
std::string ResolveAssetRef(const json& val) {
    if (val.is_string()) return AssetPathForRead(val.get<std::string>());
    if (!val.is_object()) return {};
    std::string path = AssetPathForRead(val.value("path", std::string()));
    AssetGuid g = AssetGuid::FromString(val.value("pathGuid", std::string()));
    return AssetDatabase::Resolve(g, path);
}
// Overload for objects that carry the path and guid as sibling string keys.
std::string ResolveAssetRef(const json& obj, const char* pathKey, const char* guidKey) {
    std::string path = AssetPathForRead(obj.value(pathKey, std::string()));
    AssetGuid g = AssetGuid::FromString(obj.value(guidKey, std::string()));
    return AssetDatabase::Resolve(g, path);
}

std::shared_ptr<Texture> LoadIfPresent(AssetLibrary& assets, const json& obj, const char* key) {
    if (!obj.contains(key)) return nullptr;
    std::string path = ResolveAssetRef(obj[key]);
    if (path.empty()) return nullptr;
    return assets.LoadTexture(path);
}

// Material slots of a RenderableComponent <-> JSON. Shared by boxes and models: #120 — boxes
// used to write only their colour, so a .mat or an embedded PBR material assigned to a
// level-geometry box was silently dropped on save, on every undo/redo and on Play -> Stop.
json MaterialSlotsToJson(const std::vector<std::shared_ptr<MaterialAsset>>& matSlots) {
    if (matSlots.empty()) return nullptr;
    json arr = json::array();
    for (const auto& slot : matSlots) {
        if (!slot) { arr.push_back(nullptr); continue; }
        if (!slot->Path.empty()) {
            json entry;
            AssetGuid g = AssetDatabase::GuidForPath(slot->Path);
            if (g.IsValid()) entry["guid"] = g.ToString();
            entry["path"] = AssetPathForWrite(slot->Path); // audit #364
            arr.push_back(entry);
        } else {
            const auto& smat = slot->Mat;
            json em = {
                {"factorsScaleMaps", true}, // #102 — see ReadEmbeddedMat
                {"baseColor", Vec3ToJson(smat.BaseColor)},
                {"metallic", smat.Metallic},
                {"roughness", smat.Roughness},
                {"emissiveColor", Vec3ToJson(smat.EmissiveColor)},
                {"emissiveStrength", smat.EmissiveStrength},
                {"triplanar", smat.Triplanar},
                {"triplanarScale", smat.TriplanarScale},
                {"albedoMap",            PathRef(PathOrEmpty(smat.AlbedoMap))},
                {"normalMap",            PathRef(PathOrEmpty(smat.NormalMap))},
                {"metallicRoughnessMap", PathRef(PathOrEmpty(smat.MetallicRoughnessMap))},
                {"metallicMap",          PathRef(PathOrEmpty(smat.MetallicMap))},
                {"roughnessMap",         PathRef(PathOrEmpty(smat.RoughnessMap))},
                {"aoMap",                PathRef(PathOrEmpty(smat.AOMap))},
                {"emissiveMap",          PathRef(PathOrEmpty(smat.EmissiveMap))},
            };
            // #120 — the advanced lobes and surface settings were not persisted for embedded
            // materials either. Written only when not default, to keep scene diffs small.
            if (smat.ClearCoat != 0.0f)            em["clearCoat"] = smat.ClearCoat;
            if (smat.ClearCoatRoughness != 0.5f)   em["clearCoatRoughness"] = smat.ClearCoatRoughness;
            if (smat.Anisotropy != 0.0f)           em["anisotropy"] = smat.Anisotropy;
            if (smat.AnisotropyRotation != 0.0f)   em["anisotropyRotation"] = smat.AnisotropyRotation;
            if (smat.Sheen != glm::vec3(0.0f))     em["sheen"] = Vec3ToJson(smat.Sheen);
            if (smat.SheenRoughness != 0.5f)       em["sheenRoughness"] = smat.SheenRoughness;
            if (smat.SubsurfaceEnabled)            em["subsurface"] = true;
            if (smat.SubsurfaceColor != glm::vec3(1.0f, 0.8f, 0.6f)) em["subsurfaceColor"] = Vec3ToJson(smat.SubsurfaceColor);
            if (smat.Thickness != 0.5f)            em["thickness"] = smat.Thickness;
            if (smat.TransmissionStrength != 0.0f) em["transmission"] = smat.TransmissionStrength;
            if (smat.IOR != 1.5f)                  em["ior"] = smat.IOR;
            if (smat.ReflectionProbes)             em["reflectionProbes"] = true;
            // #102 / #113 surface options
            if (smat.UVTiling != glm::vec2(1.0f))  em["uvTiling"] = {smat.UVTiling.x, smat.UVTiling.y};
            if (smat.UVOffset != glm::vec2(0.0f))  em["uvOffset"] = {smat.UVOffset.x, smat.UVOffset.y};
            if (smat.NormalStrength != 1.0f)       em["normalStrength"] = smat.NormalStrength;
            if (smat.NormalFlipY)                  em["normalFlipY"] = true;
            if (smat.DoubleSided)                  em["doubleSided"] = true;
            if (smat.UseVertexColor)               em["vertexColors"] = true;
            if (smat.HeightMap)                    em["heightMap"] = PathRef(PathOrEmpty(smat.HeightMap));
            if (smat.ParallaxScale != 0.02f)       em["parallaxScale"] = smat.ParallaxScale;
            if (smat.DetailAlbedoMap)              em["detailAlbedoMap"] = PathRef(PathOrEmpty(smat.DetailAlbedoMap));
            if (smat.DetailNormalMap)              em["detailNormalMap"] = PathRef(PathOrEmpty(smat.DetailNormalMap));
            if (smat.DetailTiling != glm::vec2(4.0f)) em["detailTiling"] = {smat.DetailTiling.x, smat.DetailTiling.y};
            if (smat.AlphaCutoff != 0.5f)          em["alphaCutoff"] = smat.AlphaCutoff;
            if (slot->RenderQueue != MaterialAsset::Queue::Opaque) em["renderQueue"] = (int)slot->RenderQueue;
            if (slot->Opacity != 1.0f)             em["opacity"] = slot->Opacity;
            arr.push_back({{"embedded", std::move(em)}});
        }
    }
    return arr;
}

std::shared_ptr<MaterialAsset> ReadEmbeddedMat(AssetLibrary& assets, const json& mj) {
    auto ma = std::make_shared<MaterialAsset>();
    ma->Mat.BaseColor       = JsonToVec3(mj.value("baseColor", json::array({1, 1, 1})), glm::vec3(1.0f));
    ma->Mat.Metallic        = mj.value("metallic", 0.0f);
    ma->Mat.Roughness       = mj.value("roughness", 0.5f);
    ma->Mat.EmissiveColor   = JsonToVec3(mj.value("emissiveColor", json::array({0, 0, 0})));
    ma->Mat.EmissiveStrength= mj.value("emissiveStrength", 1.0f);
    ma->Mat.Triplanar       = mj.value("triplanar", false);
    ma->Mat.TriplanarScale  = mj.value("triplanarScale", 1.0f);
    ma->Mat.AlbedoMap            = LoadIfPresent(assets, mj, "albedoMap");
    ma->Mat.NormalMap            = LoadIfPresent(assets, mj, "normalMap");
    ma->Mat.MetallicRoughnessMap = LoadIfPresent(assets, mj, "metallicRoughnessMap");
    ma->Mat.MetallicMap          = LoadIfPresent(assets, mj, "metallicMap");
    ma->Mat.RoughnessMap         = LoadIfPresent(assets, mj, "roughnessMap");
    ma->Mat.AOMap                = LoadIfPresent(assets, mj, "aoMap");
    ma->Mat.EmissiveMap          = LoadIfPresent(assets, mj, "emissiveMap");
    MaterialAsset::UpgradeLegacyMapFactors(ma->Mat, mj.value("factorsScaleMaps", false));
    ma->Mat.ClearCoat            = mj.value("clearCoat", 0.0f);
    ma->Mat.ClearCoatRoughness   = mj.value("clearCoatRoughness", 0.5f);
    ma->Mat.Anisotropy           = mj.value("anisotropy", 0.0f);
    ma->Mat.AnisotropyRotation   = mj.value("anisotropyRotation", 0.0f);
    ma->Mat.Sheen                = JsonToVec3(mj.value("sheen", json::array({0, 0, 0})));
    ma->Mat.SheenRoughness       = mj.value("sheenRoughness", 0.5f);
    ma->Mat.SubsurfaceEnabled    = mj.value("subsurface", false);
    ma->Mat.SubsurfaceColor      = JsonToVec3(mj.value("subsurfaceColor", json::array({1.0, 0.8, 0.6})), glm::vec3(1.0f, 0.8f, 0.6f));
    ma->Mat.Thickness            = mj.value("thickness", 0.5f);
    ma->Mat.TransmissionStrength = mj.value("transmission", 0.0f);
    ma->Mat.IOR                  = mj.value("ior", 1.5f);
    ma->Mat.ReflectionProbes     = mj.value("reflectionProbes", false);
    { // #102 / #113 surface options
        auto vec2Or = [&](const char* k, glm::vec2 def) {
            const auto it = mj.find(k);
            if (it == mj.end() || !it->is_array() || it->size() < 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) return def;
            return glm::vec2((*it)[0].get<float>(), (*it)[1].get<float>());
        };
        ma->Mat.UVTiling       = vec2Or("uvTiling", glm::vec2(1.0f));
        ma->Mat.UVOffset       = vec2Or("uvOffset", glm::vec2(0.0f));
        ma->Mat.DetailTiling   = vec2Or("detailTiling", glm::vec2(4.0f));
        ma->Mat.NormalStrength = mj.value("normalStrength", 1.0f);
        ma->Mat.NormalFlipY    = mj.value("normalFlipY", false);
        ma->Mat.DoubleSided    = mj.value("doubleSided", false);
        ma->Mat.UseVertexColor = mj.value("vertexColors", false);
        ma->Mat.ParallaxScale  = mj.value("parallaxScale", 0.02f);
        ma->Mat.HeightMap       = LoadIfPresent(assets, mj, "heightMap");
        ma->Mat.DetailAlbedoMap = LoadIfPresent(assets, mj, "detailAlbedoMap");
        ma->Mat.DetailNormalMap = LoadIfPresent(assets, mj, "detailNormalMap");
    }
    ma->Mat.AlphaCutoff          = mj.value("alphaCutoff", 0.5f);
    ma->RenderQueue              = (MaterialAsset::Queue)std::clamp(mj.value("renderQueue", 0), 0, 2);
    ma->Mat.AlphaClip            = ma->RenderQueue == MaterialAsset::Queue::AlphaTest;
    ma->Opacity                  = mj.value("opacity", 1.0f);
    return ma;
}

// #163 - Cast/Receive Shadows. Written only when not the default, so untouched scenes don't change.
void WriteRendererFlags(json& obj, const RenderableComponent& rc) {
    if (rc.CastShadows != RenderableComponent::ShadowCasting::On) obj["castShadows"] = (int)rc.CastShadows;
    if (!rc.ReceiveShadows) obj["receiveShadows"] = false;
}
void ReadRendererFlags(const json& obj, RenderableComponent& rc) {
    const auto it = obj.find("castShadows");
    rc.CastShadows = (it != obj.end() && it->is_number_integer())
        ? (RenderableComponent::ShadowCasting)std::clamp(it->get<int>(), 0, 3)
        : RenderableComponent::ShadowCasting::On;
    rc.ReceiveShadows = obj.value("receiveShadows", true);
}

void ReadMaterialSlots(AssetLibrary& assets, const json& obj, RenderableComponent& rc) {
    if (obj.contains("materials") && obj["materials"].is_array()) {
        for (const auto& slot_j : obj["materials"]) {
            if (slot_j.is_null()) { rc.Materials.push_back(nullptr); continue; }
            if (slot_j.contains("embedded")) {
                rc.Materials.push_back(ReadEmbeddedMat(assets, slot_j["embedded"]));
            } else {
                std::string path = ResolveAssetRef(slot_j);
                rc.Materials.push_back(path.empty() ? nullptr : assets.LoadMaterial(path));
            }
        }
    } else if (obj.contains("material")) {
        // Legacy pre-PR5 format: single embedded Material -> slot 0
        rc.Materials.assign(1, ReadEmbeddedMat(assets, obj["material"]));
    }
}

// --- One reflected field <-> JSON, in one place -------------------------------------------
// The generic component write/read (WriteCommonComponents / ReadCommonComponents), and the
// prefab per-field override diff/replay (#302 Part B), all encode a reflected field the same
// way. `fp` is the field address from ReflectField::Address.
json ReflectFieldToJson(const ReflectField& f, const void* fp) {
    switch (f.Type) {
        case ReflectFieldType::Bool:   return *static_cast<const bool*>(fp);
        case ReflectFieldType::Int:    return *static_cast<const int*>(fp);
        case ReflectFieldType::Float:  return *static_cast<const float*>(fp);
        case ReflectFieldType::Vec3:
        case ReflectFieldType::Color:  return Vec3ToJson(*static_cast<const glm::vec3*>(fp));
        case ReflectFieldType::String: return *static_cast<const std::string*>(fp);
        case ReflectFieldType::AssetRef: return PathRef(*static_cast<const std::string*>(fp));
        case ReflectFieldType::Enum: {
            const int v = *static_cast<const int*>(fp);
            // Round-trip the label text so a reordered EnumLabels list doesn't rewrite scenes;
            // fall back to the raw int if it's somehow out of range.
            if (v >= 0 && v < f.EnumCount) return json(ReflectEnumLabel(f, v));
            return v;
        }
    }
    return nullptr;
}

void ReflectFieldFromJson(const ReflectField& f, void* fp, const json& v, AssetLibrary& assets) {
    switch (f.Type) {
        case ReflectFieldType::Bool:   *static_cast<bool*>(fp)  = v.get<bool>(); break;
        case ReflectFieldType::Int:    *static_cast<int*>(fp)   = v.get<int>(); break;
        case ReflectFieldType::Float:  *static_cast<float*>(fp) = v.get<float>(); break;
        case ReflectFieldType::Vec3:
        case ReflectFieldType::Color:  *static_cast<glm::vec3*>(fp) = JsonToVec3(v); break;
        case ReflectFieldType::String: *static_cast<std::string*>(fp) = v.get<std::string>(); break;
        case ReflectFieldType::AssetRef: {
            auto& path = *static_cast<std::string*>(fp);
            path = ResolveAssetRef(v);  // handles both v1 plain string and v2 {path,pathGuid}
            // Make the referenced asset list in the library even if nothing else imported it,
            // so the Inspector's picker can still show / re-select it.
            if (!path.empty() && f.AssetKind == ReflectAssetKind::Sound) assets.RegisterSound(path);
            break;
        }
        case ReflectFieldType::Enum:
            if      (v.is_string())         *static_cast<int*>(fp) = ReflectEnumIndex(f, v.get<std::string>().c_str());
            else if (v.is_number_integer()) *static_cast<int*>(fp) = v.get<int>();
            break;
    }
}

// Tolerant equality for the override diff: floats/vectors within a small epsilon (JSON
// re-encoding and matrix decompose both perturb the low bits), everything else exact.
bool ReflectJsonNearlyEqual(ReflectFieldType t, const json& a, const json& b) {
    auto close = [](double x, double y) { return std::fabs(x - y) <= 1e-4; };
    switch (t) {
        case ReflectFieldType::Float:
            return a.is_number() && b.is_number() && close(a.get<double>(), b.get<double>());
        case ReflectFieldType::Vec3:
        case ReflectFieldType::Color:
            if (!a.is_array() || !b.is_array() || a.size() != 3 || b.size() != 3) return a == b;
            for (int i = 0; i < 3; ++i)
                if (!a[i].is_number() || !b[i].is_number() || !close(a[i].get<double>(), b[i].get<double>()))
                    return false;
            return true;
        default:
            return a == b;
    }
}

// Components that any entity kind can carry, written/read as one shared block so a box, a
// placed model, and a bare (Renderable-less) entity all round-trip them identically. Each is
// omitted entirely when absent, keeping saves of simple scenes as small as they were before
// these components existed — and keeping every field optional on load, so an older file that
// predates any of them still parses.
void WriteCommonComponents(json& j, const World& world, entt::entity entity) {
    if (const auto* order = world.Registry.try_get<OrderComponent>(entity)) j["order"] = order->Value;
    if (const auto* tag = world.Registry.try_get<TagComponent>(entity)) j["tag"] = tag->Tag;
    if (world.Registry.all_of<InactiveTag>(entity)) j["active"] = false;
    if (world.Registry.all_of<StaticTag>(entity)) j["static"] = true;
    // Layer slot (#236 A1). Omitted for the default (0) so scenes that never touch layers are
    // byte-identical to before; slot names live in project/layers.json, not the scene.
    if (const auto* layer = world.Registry.try_get<LayerComponent>(entity); layer && layer->Layer != 0)
        j["layer"] = layer->Layer;
    if (world.Registry.all_of<HiddenInSceneTag>(entity)) j["sceneHidden"] = true; // #236 B — editor SceneVis
    if (world.Registry.all_of<SceneLockedTag>(entity)) j["sceneLocked"] = true;

    // LightComponent moved onto reflection (#302 Wave 2b) — it round-trips through the generic
    // "Light" block below. The old flat "light" object (incl. the legacy "castShadows" bool and
    // nested "shadow") is still READ (see LoadEntity) for scenes authored before the migration.

    // Boxes get a Collider from World::CreateBox already; this only records one that was added
    // manually (to a placed model, via the Inspector's Add Component). Shape/HalfExtents/Center
    // are #185 PR 2 — omitted when at their defaults (Box, auto-size, no offset) so legacy
    // scenes and every CreateBox collider serialize byte-identically to before.
    if (const auto* collider = world.Registry.try_get<ColliderComponent>(entity)) {
        json cj = {{"isTrigger", collider->IsTrigger}};
        if (collider->Kind != ColliderComponent::Shape::Box) cj["shape"] = (int)collider->Kind;
        if (collider->HalfExtents != glm::vec3(0.0f))
            cj["halfExtents"] = {collider->HalfExtents.x, collider->HalfExtents.y, collider->HalfExtents.z};
        if (collider->Center != glm::vec3(0.0f))
            cj["center"] = {collider->Center.x, collider->Center.y, collider->Center.z};
        if (collider->Bounciness != 0.0f) cj["bounciness"] = collider->Bounciness; // #185 PR 7
        if (collider->Friction != 0.6f)   cj["friction"]   = collider->Friction;
        // #170 / #204 - only when they differ from the pre-existing single-friction behaviour.
        if (collider->StaticFriction != collider->Friction) cj["staticFriction"] = collider->StaticFriction;
        if (collider->FrictionCombine != 0) cj["frictionCombine"] = collider->FrictionCombine;
        if (collider->BounceCombine != 0)   cj["bounceCombine"]   = collider->BounceCombine;
        j["collider"] = cj;
    }
    // Joint (#185 PR 11) — hand-serialised (its "other end" isn't a plain reflectable field).
    if (const auto* joint = world.Registry.try_get<JointComponent>(entity)) {
        j["joint"] = {
            {"type", (int)joint->Kind},
            {"connectedOrder", joint->ConnectedOrder},
            {"anchor", {joint->Anchor.x, joint->Anchor.y, joint->Anchor.z}},
            {"axis", {joint->Axis.x, joint->Axis.y, joint->Axis.z}},
            {"breakForce", joint->BreakForce},
            {"breakTorque", joint->BreakTorque},
            {"useLimit", joint->UseLimit},
            {"limitLower", joint->LimitLower},
            {"limitUpper", joint->LimitUpper},
        };
    }
    // CameraComponent moved onto reflection (#302 Wave 1a) — it now round-trips through the
    // generic "Camera" block below. The old flat "camera" object is still READ (see LoadEntity)
    // for scenes authored before the migration.

    // #184: components registered through the reflection system serialize generically — one JSON
    // object per component keyed by its Meta.Name, one entry per reflected field. No per-component
    // code here; adding a reflected component adds nothing to this file. TransformController was
    // the first migration off hand-written code onto this path (was a "transformController" object
    // with its own key names; no shipped scene ever set it, since it had no Inspector section).
    // Animator is the second — unlike TransformController, real scenes DO carry authored
    // "animator" blocks (it's ship-visible: moving colour-cycling lights), so ReadCommonComponents
    // below keeps a permanent legacy-format read fallback even though this write path only ever
    // emits the new "Animator" key from here on.
    for (const auto& rc : ComponentRegistry::All()) {
        if (!rc.Meta.GenericSerialize) continue; // hand-coded elsewhere (e.g. Mesh Renderer)
        if (!rc.Has(world.Registry, entity)) continue;
        // const_cast is safe: the component is a live mutable object; this path only reads it.
        void* comp = const_cast<void*>(rc.GetConst(world.Registry, entity));
        json cj;
        for (const auto& f : rc.Meta.Fields)
            cj[ReflectFieldKey(f)] = ReflectFieldToJson(f, f.Address(comp));
        j[rc.Meta.Name] = cj;
    }
}

void ReadCommonComponents(const json& j, World& world, AssetLibrary& assets, entt::entity entity) {
    if (j.contains("tag")) world.Registry.emplace_or_replace<TagComponent>(entity, j["tag"].get<std::string>());
    if (!j.value("active", true)) world.Registry.emplace_or_replace<InactiveTag>(entity);
    if (j.value("static", false)) world.Registry.emplace_or_replace<StaticTag>(entity);
    if (const int layer = j.value("layer", 0); layer > 0 && layer < LayerRegistry::kCount) // #150: range-checked
        world.Registry.emplace_or_replace<LayerComponent>(entity, LayerComponent{layer});
    if (j.value("sceneHidden", false)) world.Registry.emplace_or_replace<HiddenInSceneTag>(entity); // #236 B
    if (j.value("sceneLocked", false)) world.Registry.emplace_or_replace<SceneLockedTag>(entity);

    // Legacy pre-#302 format: LightComponent moved onto reflection (keyed "Light" below), but
    // scenes authored before the migration carry the old flat "light" object with its own key
    // names (and the legacy "castShadows" bool). Read it only when the new key is absent, so a
    // re-saved file goes through the generic path instead.
    if (!j.contains("Light") && j.contains("light")) {
        const json& l = j["light"];
        LightComponent light;
        std::string lightKind = l.value("kind", std::string("point"));
        light.Kind = lightKind == "spot" ? LightComponent::Type::Spot
                   : lightKind == "directional" ? LightComponent::Type::Directional
                   : LightComponent::Type::Point;
        light.Color = JsonToVec3(l.value("color", json::array({1, 1, 1})), glm::vec3(1.0f));
        light.Intensity = l.value("intensity", 5.0f);
        light.Range = l.value("range", 12.0f);
        light.SpotAngleDegrees = l.value("spotAngle", 35.0f);
        light.AngularSizeDegrees = l.value("angularSize", 0.53f);
        light.ColorTempK = l.value("colorTempK", 0.0f);
        // Nested "shadow" object (phase 2) with a fallback to the legacy flat "castShadows" bool
        // so pre-phase-2 scenes still light up their shadow casters. A directional light with no
        // shadow info at all predates per-light control, when the sun always cast under the global
        // toggle — default it on so those scenes look unchanged; point/spot stay opt-in.
        bool sunDefault = light.Kind == LightComponent::Type::Directional;
        bool legacyCast = l.value("castShadows", sunDefault);
        if (l.contains("shadow") && l["shadow"].is_object()) {
            const json& s = l["shadow"];
            light.Shadow.Enabled    = s.value("enabled", legacyCast);
            light.Shadow.Bias       = s.value("bias", 1.0f);
            light.Shadow.NormalBias = s.value("normalBias", 1.0f);
            light.Shadow.Softness   = s.value("softness", 1.0f);
            light.Shadow.NearPlane  = s.value("nearPlane", 0.05f);
            light.Shadow.Resolution = s.value("resolution", 0);
            light.Shadow.UpdateMode = s.value("updateMode", 0);
        } else {
            light.Shadow.Enabled = legacyCast;
        }
        world.Registry.emplace_or_replace<LightComponent>(entity, light);
    }
    if (j.contains("collider")) {
        const json& c = j["collider"];
        ColliderComponent collider;
        collider.IsTrigger = c.value("isTrigger", false);
        int shape = c.value("shape", 0); // absent => Box (0), the pre-#185 shape
        collider.Kind = (shape >= 0 && shape <= 4) ? (ColliderComponent::Shape)shape
                                                   : ColliderComponent::Shape::Box;
        if (c.contains("halfExtents"))
            collider.HalfExtents = JsonToVec3(c["halfExtents"], glm::vec3(0.0f));
        if (c.contains("center"))
            collider.Center = JsonToVec3(c["center"], glm::vec3(0.0f));
        collider.Bounciness = c.value("bounciness", 0.0f); // #185 PR 7
        collider.Friction   = c.value("friction", 0.6f);
        // Absent (every scene before #170's physics materials) = the same as Friction.
        collider.StaticFriction = c.contains("staticFriction") && c["staticFriction"].is_number()
                                      ? c["staticFriction"].get<float>() : collider.Friction;
        auto combine = [&c](const char* key) {
            const int v = (c.contains(key) && c[key].is_number_integer()) ? c[key].get<int>() : 0;
            return (v >= 0 && v <= 3) ? v : 0;
        };
        collider.FrictionCombine = combine("frictionCombine");
        collider.BounceCombine   = combine("bounceCombine");
        world.Registry.emplace_or_replace<ColliderComponent>(entity, collider);
    }
    if (j.contains("joint")) { // #185 PR 11
        const json& jc = j["joint"];
        JointComponent joint;
        int ty = jc.value("type", 0);
        joint.Kind = (ty >= 0 && ty <= 4) ? (JointComponent::Type)ty : JointComponent::Type::Fixed;
        joint.ConnectedOrder = jc.value("connectedOrder", -1);
        if (jc.contains("anchor")) joint.Anchor = JsonToVec3(jc["anchor"], glm::vec3(0.0f));
        if (jc.contains("axis"))   joint.Axis   = JsonToVec3(jc["axis"], glm::vec3(1.0f, 0.0f, 0.0f));
        joint.BreakForce  = jc.value("breakForce", 0.0f);
        joint.BreakTorque = jc.value("breakTorque", 0.0f);
        joint.UseLimit    = jc.value("useLimit", false);
        joint.LimitLower  = jc.value("limitLower", -45.0f);
        joint.LimitUpper  = jc.value("limitUpper", 45.0f);
        world.Registry.emplace_or_replace<JointComponent>(entity, joint);
    }
    // Legacy pre-#302 format: CameraComponent moved onto reflection (keyed "Camera" below), but
    // scenes authored before the migration carry the old flat "camera" object — read it only
    // when the new key is absent, so a re-saved file goes through the generic path instead.
    if (!j.contains("Camera") && j.contains("camera")) {
        const json& c = j["camera"];
        CameraComponent cam;
        cam.FovDegrees = c.value("fov", 60.0f);
        cam.NearPlane = c.value("near", 0.1f);
        cam.FarPlane = c.value("far", 1000.0f);
        world.Registry.emplace_or_replace<CameraComponent>(entity, cam);
    }
    // Legacy pre-#184 format: AnimatorComponent moved onto reflection (keyed "Animator" below),
    // but real authored scenes carry the old flat "animator" object, so it's still read here —
    // only when the new key is absent, so a re-saved file goes through the generic path instead.
    if (!j.contains("Animator") && j.contains("animator")) {
        const json& a = j["animator"];
        AnimatorComponent anim;
        anim.SpinDegPerSec = JsonToVec3(a.value("spin", json::array({0, 0, 0})));
        anim.OrbitAxis = JsonToVec3(a.value("orbitAxis", json::array({0, 1, 0})), glm::vec3(0.0f, 1.0f, 0.0f));
        anim.OrbitDegPerSec = a.value("orbitDegPerSec", 0.0f);
        anim.OrbitRadius = a.value("orbitRadius", 0.0f);
        anim.BobAmplitude = a.value("bobAmplitude", 0.0f);
        anim.BobFreqHz = a.value("bobFreqHz", 0.0f);
        anim.ColorCycleHzPerSec = a.value("colorCycleHzPerSec", 0.0f);
        world.Registry.emplace_or_replace<AnimatorComponent>(entity, anim);
    }

    // Legacy pre-#302 format: AudioSourceComponent moved onto reflection (keyed "Audio Source"
    // below). Scenes authored before the migration carry flat "sound*" keys on the model object.
    // Read them only when the new key is absent, so a re-saved file goes through the generic path.
    if (!j.contains("Audio Source")) {
        const std::string soundPath = j.value("soundPath", std::string());
        if (!soundPath.empty()) {
            assets.RegisterSound(soundPath);
            auto& audio = world.Registry.emplace_or_replace<AudioSourceComponent>(entity, soundPath);
            audio.Volume = j.value("soundVolume", 1.0f);
            audio.Loop = j.value("soundLoop", false);
            audio.PlayOnStart = j.value("soundPlayOnStart", false);
        }
    }

    // #184: mirror of the generic write — restore each registered component present in `j`.
    // Missing fields keep the component's own default (the component was just default-added).
    for (const auto& rc : ComponentRegistry::All()) {
        if (!rc.Meta.GenericSerialize) continue; // hand-coded elsewhere (e.g. Mesh Renderer)
        if (!j.contains(rc.Meta.Name)) continue;
        rc.Add(world.Registry, entity);
        void* comp = rc.Get(world.Registry, entity);
        const json& cj = j.at(rc.Meta.Name);
        for (const auto& f : rc.Meta.Fields) {
            // #43: key off ReflectFieldKey, with a LegacyNames fallback — so a scene saved under a
            // field's old key (before an Inspector-label rename) still loads instead of silently
            // keeping the component's default for that field.
            const char* key = ReflectFieldKey(f);
            if (cj.contains(key)) { ReflectFieldFromJson(f, f.Address(comp), cj.at(key), assets); continue; }
            for (int i = 0; i < f.LegacyNameCount; ++i) {
                if (f.LegacyNames[i] && cj.contains(f.LegacyNames[i])) {
                    ReflectFieldFromJson(f, f.Address(comp), cj.at(f.LegacyNames[i]), assets);
                    break;
                }
            }
        }
    }
}

// --- Prefab per-field overrides (#236 A2 stage 3 / #302 Part B) --------------------------
// The .prefab file is the pristine source of truth. Diffing at save time can't instantiate a
// copy (BuildSceneJson holds a `const World&`), so instead each live instance entity is
// compared field-by-field against the matching entity object in the .prefab JSON. Pairing is by
// index: PrefabInstanceComponent::InstanceEntities is in the same order the file lists entities
// (boxes, then models, then empties, each in file order — see InstantiatePrefab / ApplySceneJson).

// The .prefab file's entity objects, flattened into one list in that canonical order.
std::vector<const json*> PrefabEntityObjects(const json& prefab) {
    std::vector<const json*> out;
    for (const char* key : {"boxes", "models", "empties"})
        if (auto it = prefab.find(key); it != prefab.end() && it->is_array())
            for (const json& e : *it) out.push_back(&e);
    return out;
}

// Append override entries for everything on `live` that differs from `pristine`:
//   { e, c, f, v }                    — a changed reflected field, or a Transform channel / Name
//   { e, op:"addComponent", c }       — a reflected component the instance gained (its fields
//                                       follow as normal { e, c, f, v } entries)
//   { e, op:"removeComponent", c }    — a reflected component the prefab has that the instance
//                                       deleted
// `localIndex` is the entity's prefab-local index; `isRoot` only skips Transform/Name (those
// round-trip through the stub's own top-level fields) — component add/remove applies to the
// root too.
void DiffPrefabEntity(const World& world, entt::entity live, const json& pristine,
                      int localIndex, bool isRoot,
                      const std::function<TransformComponent(entt::entity)>& effectiveTransform,
                      json& outOverrides) {
    auto emit = [&](const char* comp, const char* field, json value) {
        outOverrides.push_back({{"e", localIndex}, {"c", comp}, {"f", field}, {"v", std::move(value)}});
    };

    // Reflected components — presence diff + field diff.
    for (const auto& rc : ComponentRegistry::All()) {
        const bool onLive = rc.Has(world.Registry, live);
        const bool onPrefab = pristine.contains(rc.Meta.Name);
        if (!onLive && !onPrefab) continue;

        if (onLive && !onPrefab) {
            // Instance gained this component — record the add, then every field as an override
            // (there's no pristine value to compare against, so all of them count).
            outOverrides.push_back({{"e", localIndex}, {"op", "addComponent"}, {"c", rc.Meta.Name}});
            const void* comp = rc.GetConst(world.Registry, live);
            for (const auto& f : rc.Meta.Fields)
                emit(rc.Meta.Name, ReflectFieldKey(f), ReflectFieldToJson(f, f.Address(const_cast<void*>(comp))));
            continue;
        }
        if (!onLive && onPrefab) {
            outOverrides.push_back({{"e", localIndex}, {"op", "removeComponent"}, {"c", rc.Meta.Name}});
            continue;
        }

        // On both — diff each field.
        const json& pc = pristine.at(rc.Meta.Name);
        const void* comp = rc.GetConst(world.Registry, live);
        for (const auto& f : rc.Meta.Fields) {
            const char* key = ReflectFieldKey(f);
            const json* pv = pc.contains(key) ? &pc.at(key) : nullptr;
            for (int i = 0; !pv && i < f.LegacyNameCount; ++i)
                if (f.LegacyNames[i] && pc.contains(f.LegacyNames[i])) pv = &pc.at(f.LegacyNames[i]);
            if (!pv) continue;
            json now = ReflectFieldToJson(f, f.Address(const_cast<void*>(comp)));
            if (!ReflectJsonNearlyEqual(f.Type, now, *pv)) emit(rc.Meta.Name, key, now);
        }
    }

    if (isRoot) return;

    // Transform (non-root; the root's transform is the stub's own position/rotation/scale). The
    // box/model/empty writers always emit all three, so `contains` is just defensiveness.
    const TransformComponent t = effectiveTransform(live);
    const struct { const char* f; glm::vec3 v; } tf[] = {
        {"position", t.Position}, {"rotation", t.RotationEuler}, {"scale", t.Scale}};
    for (const auto& e : tf) {
        if (!pristine.contains(e.f)) continue;
        json now = Vec3ToJson(e.v);
        if (!ReflectJsonNearlyEqual(ReflectFieldType::Vec3, now, pristine.at(e.f)))
            emit("Transform", e.f, now);
    }

    // Name.
    if (const auto* nc = world.Registry.try_get<NameComponent>(live)) {
        const std::string was = pristine.value("name", std::string());
        if (nc->Name != was) emit("Name", "name", nc->Name);
    }
}

// Apply one loaded override entry to a live instance entity.
void ApplyPrefabOverride(World& world, AssetLibrary& assets, entt::entity e,
                         const std::string& comp, const std::string& field, const json& v) {
    if (comp == "Transform") {
        auto& t = world.Registry.get<TransformComponent>(e);
        if      (field == "position") t.Position = JsonToVec3(v);
        else if (field == "rotation") t.RotationEuler = JsonToVec3(v);
        else if (field == "scale")    t.Scale = JsonToVec3(v);
        return;
    }
    if (comp == "Name") {
        world.Registry.emplace_or_replace<NameComponent>(e, NameComponent{v.get<std::string>()});
        return;
    }
    for (const auto& rc : ComponentRegistry::All()) {
        if (rc.Meta.Name != comp) continue;
        if (!rc.Has(world.Registry, e)) rc.Add(world.Registry, e);
        void* c = rc.Get(world.Registry, e);
        for (const auto& f : rc.Meta.Fields)
            if (ReflectFieldMatchesKey(f, field.c_str())) { ReflectFieldFromJson(f, f.Address(c), v, assets); return; }
        return;
    }
}

// EnTT views iterate a pool newest-entity-first. Writing in that order would reverse the scene
// on every save (load re-creates in file order, the next save reverses it again), so the file
// churns and the Hierarchy's ordering flips each time. Collecting and reversing restores
// creation order, which is stable across any number of round-trips.
template <typename View>
std::vector<entt::entity> InCreationOrder(const entt::registry& reg, View view) {
    std::vector<entt::entity> entities(view.begin(), view.end());
    std::sort(entities.begin(), entities.end(), [&](entt::entity a, entt::entity b) {
        const auto* oa = reg.try_get<OrderComponent>(a);
        const auto* ob = reg.try_get<OrderComponent>(b);
        int va = oa ? oa->Value : 0, vb = ob ? ob->Value : 0;
        return va != vb ? va < vb : a < b; // stable tiebreak for pre-OrderComponent data
    });
    return entities;
}

// When `only` is non-null, just the entities it names are written (an entity-subset fragment
// for the clipboard or a prefab) and the sky settings are left out, since pasting a couple of
// objects must not also overwrite the destination scene's environment.
// flattenPrefabInstances: write prefab-instance subtrees in full (as plain entities, no stub,
// no link) instead of collapsing them to a stub. Used when saving a .prefab file — a prefab
// asset must be self-contained; nested prefab links are stage 4.
json BuildSceneJson(const World& world, const std::set<entt::entity>* only = nullptr,
                    bool flattenPrefabInstances = false) {
    json root;
    auto included = [&](entt::entity e) { return !only || only->count(e) > 0; };
    if (!only) {
        // Omitted for entity-subset fragments (clipboard/prefab) same as the sky colors below —
        // a fragment is spliced into whatever scene is already loaded, never loaded standalone,
        // so it has no independent format to version.
        root["formatVersion"] = kSceneFormatVersion;
        root["skyHorizonColor"] = Vec3ToJson(world.SkyHorizonColor);
        root["skyZenithColor"] = Vec3ToJson(world.SkyZenithColor);
        root["skyAmbientIntensity"] = world.SkyAmbientIntensity; // #196
        // PR13: HDRI sky source (defaults omitted for backwards compatibility)
        if (world.SkySourceMode != World::SkySource::Procedural)
            root["skySource"] = (int)world.SkySourceMode;
        if (!world.SkyHdriPath.empty()) {
            root["skyHdriPath"] = AssetPathForWrite(world.SkyHdriPath); // audit #364 — was stored absolute
            // #132 — and by GUID, so renaming/moving the .hdr doesn't break the sky.
            AssetGuid g = AssetDatabase::GuidForPath(world.SkyHdriPath); // registered by ScanProject
            if (g.IsValid()) root["skyHdriGuid"] = g.ToString();
        }
        if (world.SkyRotationDegrees != 0.0f)
            root["skyRotationDegrees"] = world.SkyRotationDegrees;
        if (world.SkyHdriSun != World::HdriSunMode::Auto)
            root["skyHdriSun"] = (int)world.SkyHdriSun; // #277
        if (world.SkyHdriSunThreshold != 50.0f)
            root["skyHdriSunThreshold"] = world.SkyHdriSunThreshold;

        // #9, Phase M item 1 — post-processing/shadow settings, scene-authored since v3.
        root["exposureEV"] = world.ExposureEV;
        root["tonemapOperator"] = world.TonemapOperator;
        root["msaaSamples"] = world.MsaaSamples;
        root["ssaoEnabled"] = world.SsaoEnabled;
        root["ssaoRadius"] = world.SsaoRadius;
        root["ssaoBias"] = world.SsaoBias;
        root["ssaoIntensity"] = world.SsaoIntensity;
        root["bloomEnabled"] = world.BloomEnabled;
        root["bloomThreshold"] = world.BloomThreshold;
        root["bloomKnee"] = world.BloomKnee;
        root["bloomIntensity"] = world.BloomIntensity;
        // #162
        root["fxaa"] = world.FxaaEnabled;
        root["gradeTemperature"] = world.GradeTemperature;
        root["gradeTint"] = world.GradeTint;
        root["gradeContrast"] = world.GradeContrast;
        root["gradeSaturation"] = world.GradeSaturation;
        root["gradeColorFilter"] = {world.GradeColorFilter.r, world.GradeColorFilter.g, world.GradeColorFilter.b};
        root["vignetteIntensity"] = world.VignetteIntensity;
        root["vignetteSmoothness"] = world.VignetteSmoothness;
        root["fogEnabled"] = world.FogEnabled;
        root["fogMode"] = world.FogMode;
        root["fogColor"] = {world.FogColor.r, world.FogColor.g, world.FogColor.b};
        root["fogDensity"] = world.FogDensity;
        root["fogStart"] = world.FogStart;
        root["fogEnd"] = world.FogEnd;
        root["fogHeightFalloff"] = world.FogHeightFalloff;
        root["fogBaseHeight"] = world.FogBaseHeight;
        root["shadowsEnabled"] = world.ShadowsEnabled;
        root["shadowResolution"] = world.ShadowResolution;
        root["shadowCascades"] = world.ShadowCascades;
        root["shadowDistance"] = world.ShadowDistance;
    }

    // Every box/model entity gets a stable 0-based id (assigned in the exact order written
    // below) so HierarchyComponent parent links — engine-internal entt::entity handles — can
    // round-trip through JSON as plain integers instead.
    std::unordered_map<entt::entity, int> idOf;
    int nextId = 0;
    auto assignId = [&](entt::entity e) { idOf[e] = nextId++; };
    auto parentIdOf = [&](entt::entity e) -> int {
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        if (!hier || hier->Parent == entt::null) return -1;
        auto it = idOf.find(hier->Parent);
        return it != idOf.end() ? it->second : -1;
    };

    // An entity whose real parent isn't part of this subset becomes a "false root" once
    // written — parentIdOf reports -1 for it, exactly like a genuinely unparented entity. But
    // its TransformComponent is still LOCAL (relative to that real, excluded parent); writing
    // those raw numbers under a parentId of -1 would have a reload place it using local
    // coordinates as if they were world coordinates, landing it wherever that ratio happens to
    // put it instead of where it visually was. Decomposing the live world-space matrix instead
    // keeps a copy/prefab of a child object anchored where it actually appeared. An entity
    // whose parent IS included, or that's genuinely unparented, is untouched (returned as-is) —
    // this only ever changes behavior for the subset case (`only != nullptr`), never a full
    // scene save, since `included()` is unconditionally true there.
    auto effectiveTransform = [&](entt::entity e) -> TransformComponent {
        const auto& t = world.Registry.get<TransformComponent>(e);
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        bool becomingFalseRoot = hier && hier->Parent != entt::null && !included(hier->Parent);
        if (!becomingFalseRoot) return t;

        glm::mat4 worldMatrix = world.ComposeWorldTransform(e);
        glm::vec3 pos, scale, skew; glm::vec4 persp; glm::quat rot;
        glm::decompose(worldMatrix, scale, rot, pos, skew, persp);
        float ex, ey, ez;
        glm::extractEulerAngleYXZ(glm::mat4_cast(rot), ey, ex, ez);

        TransformComponent out;
        out.Position = pos;
        out.Scale = scale;
        out.RotationEuler = glm::degrees(glm::vec3(ex, ey, ez));
        return out;
    };

    json boxes = json::array();
    // #122 — NameComponent is not required: an entity without one (created by code, or a future
    // component-only path) used to be silently dropped from every save. It is written as
    // "GameObject" instead.
    auto nameOf = [&](entt::entity e) -> std::string {
        const auto* nc = world.Registry.try_get<NameComponent>(e);
        return nc ? nc->Name : std::string("GameObject");
    };
    auto boxView = world.Registry.view<const TransformComponent,
        const RenderableComponent, const LevelGeometryTag>();
    auto modelViewForIds = world.Registry.view<const TransformComponent,
        const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    // Entities with no mesh at all — lights and plain empties used as grouping pivots. They
    // live in their own array rather than "models" because every "models" entry needs a `path`
    // to reload geometry from, and these have none.
    auto emptyViewForIds = world.Registry.view<const TransformComponent>(
        entt::exclude<RenderableComponent>);

    std::vector<entt::entity> boxEntities = InCreationOrder(world.Registry, boxView);
    std::vector<entt::entity> modelEntities = InCreationOrder(world.Registry, modelViewForIds);
    std::vector<entt::entity> emptyEntities = InCreationOrder(world.Registry, emptyViewForIds);

    // #236 A2 — a live prefab instance (root has PrefabInstanceComponent) is written as one
    // compact stub in "prefabInstances"; its descendants are not written at all. The owned set
    // is recomputed here from each root's HierarchyComponent, so no per-descendant marker is
    // needed. `flattenPrefabInstances` (saving a .prefab) disables all of this.
    std::set<entt::entity> prefabOwned;       // descendants — skipped entirely
    std::vector<entt::entity> prefabRoots;    // holders — written as stubs, in creation order
    if (!flattenPrefabInstances) {
        std::function<void(entt::entity)> markSubtree = [&](entt::entity e) {
            const auto* h = world.Registry.try_get<HierarchyComponent>(e);
            if (!h) return;
            for (entt::entity c : h->Children) {
                if (!included(c) || !prefabOwned.insert(c).second) continue;
                markSubtree(c);
            }
        };
        auto collect = [&](const std::vector<entt::entity>& v) {
            for (entt::entity e : v) {
                if (!included(e) || !world.Registry.all_of<PrefabInstanceComponent>(e)) continue;
                prefabRoots.push_back(e);
                markSubtree(e);
            }
        };
        collect(boxEntities); collect(modelEntities); collect(emptyEntities);
    }
    // An id goes to every entity that will be referenced by a parentId — normal writable
    // entities and prefab-instance roots (stubs), but not pure prefab descendants.
    auto skipWrite = [&](entt::entity e) {
        return !included(e) || prefabOwned.count(e) > 0;
    };
    auto isPrefabRoot = [&](entt::entity e) {
        return !flattenPrefabInstances && world.Registry.all_of<PrefabInstanceComponent>(e);
    };

    for (auto entity : boxEntities)   { if (!skipWrite(entity)) assignId(entity); }
    for (auto entity : modelEntities) { if (!skipWrite(entity)) assignId(entity); }
    for (auto entity : emptyEntities) { if (!skipWrite(entity)) assignId(entity); }

    for (auto entity : boxEntities) {
        if (skipWrite(entity) || isPrefabRoot(entity)) continue; // #236 A2
        TransformComponent transform = effectiveTransform(entity);
        const std::string name = nameOf(entity);
        const auto& renderable = boxView.get<const RenderableComponent>(entity);
        json b = {
            {"center", Vec3ToJson(transform.Position)},
            {"size", Vec3ToJson(transform.Scale)},
            {"color", Vec3ToJson(renderable.ModelRef->MeshMaterial(0).BaseColor)},
            {"rotation", Vec3ToJson(transform.RotationEuler)},
            {"name", name},
            {"id", idOf[entity]},
            {"parentId", parentIdOf(entity)},
        };
        if (json slots = MaterialSlotsToJson(renderable.Materials); !slots.is_null()) b["materials"] = std::move(slots); // #120
        WriteRendererFlags(b, renderable); // #163
        WriteCommonComponents(b, world, entity);
        boxes.push_back(b);
    }
    root["boxes"] = boxes;

    json empties = json::array();
    for (auto entity : emptyEntities) {
        if (skipWrite(entity) || isPrefabRoot(entity)) continue; // #236 A2
        TransformComponent transform = effectiveTransform(entity);
        json e = {
            {"name", nameOf(entity)},
            {"position", Vec3ToJson(transform.Position)},
            {"rotation", Vec3ToJson(transform.RotationEuler)},
            {"scale", Vec3ToJson(transform.Scale)},
            {"id", idOf[entity]},
            {"parentId", parentIdOf(entity)},
        };
        WriteCommonComponents(e, world, entity);
        empties.push_back(e);
    }
    root["empties"] = empties;

    json models = json::array();
    auto modelView = world.Registry.view<const TransformComponent,
        const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    for (auto entity : modelEntities) {
        if (skipWrite(entity) || isPrefabRoot(entity)) continue; // #236 A2
        TransformComponent transform = effectiveTransform(entity);
        const auto& renderable = modelView.get<const RenderableComponent>(entity);

        json m;
        m["path"] = AssetPathForWrite(renderable.ModelRef->Path()); // audit #364
        {
            AssetGuid g = AssetDatabase::GuidForPath(renderable.ModelRef->Path());
            if (g.IsValid()) m["pathGuid"] = g.ToString();
        }
        m["name"] = nameOf(entity);
        m["position"] = Vec3ToJson(transform.Position);
        m["rotation"] = Vec3ToJson(transform.RotationEuler);
        m["scale"] = Vec3ToJson(transform.Scale);
        // AudioSourceComponent moved onto reflection (#302 Wave 3): it now round-trips through
        // the generic "Audio Source" block written by WriteCommonComponents below (for every
        // entity kind, not just models). The old flat "sound*" keys are still READ.
        m["id"] = idOf[entity];
        m["parentId"] = parentIdOf(entity);

        if (json slots = MaterialSlotsToJson(renderable.Materials); !slots.is_null()) m["materials"] = std::move(slots);
        WriteRendererFlags(m, renderable); // #163
        WriteCommonComponents(m, world, entity);
        models.push_back(m);
    }
    root["models"] = models;

    // #236 A2 — prefab-instance stubs. Just the source path plus the root's own transform /
    // name / tags (the transform-only overrides); the subtree is rebuilt from the .prefab on
    // load and these re-applied on top.
    if (!prefabRoots.empty()) {
        json instances = json::array();
        for (entt::entity e : prefabRoots) {
            const auto& pi = world.Registry.get<PrefabInstanceComponent>(e);
            TransformComponent t = effectiveTransform(e);
            json s;
            s["source"] = AssetPathForWrite(pi.SourcePath); // audit #364
            {
                AssetGuid g = AssetDatabase::GuidForPath(pi.SourcePath);
                if (g.IsValid()) s["sourceGuid"] = g.ToString();
            }
            s["name"]     = nameOf(e);
            s["position"] = Vec3ToJson(t.Position);
            s["rotation"] = Vec3ToJson(t.RotationEuler);
            s["scale"]    = Vec3ToJson(t.Scale);
            s["id"]       = idOf[e];
            s["parentId"] = parentIdOf(e);
            if (const auto* o = world.Registry.try_get<OrderComponent>(e)) s["order"] = o->Value;
            if (world.Registry.all_of<InactiveTag>(e)) s["active"] = false;
            if (world.Registry.all_of<StaticTag>(e)) s["static"] = true;
            if (const auto* lc = world.Registry.try_get<LayerComponent>(e); lc && lc->Layer != 0)
                s["layer"] = lc->Layer;
            if (const auto* tag = world.Registry.try_get<TagComponent>(e)) s["tag"] = tag->Tag;
            // #122 — the root's editor visibility / lock (non-stub entities get these through
            // WriteCommonComponents).
            if (world.Registry.all_of<HiddenInSceneTag>(e)) s["sceneHidden"] = true;
            if (world.Registry.all_of<SceneLockedTag>(e)) s["sceneLocked"] = true;

            // #302 Part B — per-field overrides: diff each live instance entity against its
            // pristine counterpart in the .prefab file, by prefab-local index.
            if (!pi.InstanceEntities.empty()) {
                std::ifstream pf(pi.SourcePath);
                json prefab;
                bool prefabOk = false;
                if (pf.is_open()) {
                    // #82 — a corrupt / merge-conflicted .prefab must not abort the whole scene
                    // save: skip this instance's override diff (its stub still saves) and warn.
                    try { pf >> prefab; prefabOk = true; }
                    catch (const std::exception& ex) {
                        Log::Warn("Scene: couldn't parse prefab '" + pi.SourcePath +
                                  "' while saving - its per-field overrides were not re-diffed: " + ex.what());
                    }
                }
                if (prefabOk) {
                    std::vector<const json*> pristineEnts = PrefabEntityObjects(prefab);
                    json overrides = json::array();
                    const std::size_t n = std::min(pi.InstanceEntities.size(), pristineEnts.size());
                    for (std::size_t i = 0; i < n; ++i) {
                        entt::entity le = pi.InstanceEntities[i];
                        if (!world.Registry.valid(le)) continue;
                        DiffPrefabEntity(world, le, *pristineEnts[i], (int)i, /*isRoot=*/le == e,
                                         effectiveTransform, overrides);
                    }
                    if (!overrides.empty()) s["overrides"] = std::move(overrides);
                }
            }

            instances.push_back(std::move(s));
        }
        root["prefabInstances"] = std::move(instances);
    }

    return root;
}

// #83 — every field read below uses json::value()/get<T>(), which THROW json::type_error on a
// wrong-typed value ("tag": 5, "name": null, a bool written as "true", ...). The wrapper turns
// any such exception into an ordinary load failure with a logged reason instead of letting it
// escape to main()'s outermost catch and close the editor. Callers restore the previous world
// on failure (see SceneSerializer::Load / Undo).
bool ApplySceneJsonImpl(World& world, AssetLibrary& assets, const json& root,
    bool clearFirst, std::vector<entt::entity>* outCreated,
    std::unordered_map<int, entt::entity>* outSourceOrder);
bool ApplySceneJson(World& world, AssetLibrary& assets, const json& root,
    bool clearFirst = true, std::vector<entt::entity>* outCreated = nullptr,
    std::unordered_map<int, entt::entity>* outSourceOrder = nullptr) {
    try {
        return ApplySceneJsonImpl(world, assets, root, clearFirst, outCreated, outSourceOrder);
    } catch (const std::exception& e) {
        Log::Error(std::string("Scene: the scene data is malformed and could not be loaded: ") + e.what());
        return false;
    }
}

// `clearFirst` false ADDS to the existing scene instead of replacing it — the difference
// between loading a scene and pasting/instantiating a fragment into one. `outCreated`, when
// given, collects every entity this call created so the caller can select or offset them.
bool ApplySceneJsonImpl(World& world, AssetLibrary& assets, const json& root,
    bool clearFirst, std::vector<entt::entity>* outCreated,
    std::unordered_map<int, entt::entity>* outSourceOrder) {
    ApplyDepthGuard depthGuard; // #236 A2 — bounds prefab-stub expansion recursion
    if (g_ApplyDepth > kMaxApplyDepth) {
        Log::Error("Scene: prefab instance nesting too deep (" + std::to_string(kMaxApplyDepth) +
                   "); stopping expansion. Is a .prefab referencing itself?");
        return false;
    }
    auto created = [&](entt::entity e) { if (outCreated) outCreated->push_back(e); };

    // #195: 0 (the default when the key is absent) means a legacy pre-#195 file or an entity-subset
    // fragment (BuildSceneJson never writes the field for those) — both fall straight through to
    // the existing presence-check reads below exactly as before this field existed. A recognized
    // version in between would take its own migration branch here as new incompatible versions are
    // added; there's only ever been version 1 so far, so there's nothing to branch on yet. A
    // version newer than this build knows about is still loaded best-effort (every read below
    // already tolerates an unrecognized/missing key), but is very likely missing data this build
    // can't interpret, so it's called out loudly rather than silently: Console now, and stashed for
    // the caller (e.g. the editor) to also raise as a dialog via SceneSerializer::TakeLoadWarning().
    g_LastLoadWarning.clear();
    int formatVersion = root.value("formatVersion", 0);
    if (formatVersion > kSceneFormatVersion) {
        std::string msg = "Scene: this file's format version (" + std::to_string(formatVersion) +
            ") is newer than this build supports (" + std::to_string(kSceneFormatVersion) +
            "). It was saved by a newer version of the engine — some data may be missing or "
            "misinterpreted after loading.";
        Log::Error(msg);
        g_LastLoadWarning = msg;
    }

    if (clearFirst) { world.Registry.clear(); g_prefabPristineCache.clear(); } // #302 Part B
    if (!clearFirst) {
        // A fragment carries no environment settings (BuildSceneJson omits them for subsets),
        // and must not disturb the scene's own.
    } else
    if (root.contains("skyHorizonColor") || root.contains("skyZenithColor")) {
        world.SkyHorizonColor = JsonToVec3(root.value("skyHorizonColor", json::array({0.53f, 0.72f, 0.86f})), glm::vec3(0.53f, 0.72f, 0.86f));
        world.SkyZenithColor = JsonToVec3(root.value("skyZenithColor", json::array({0.20f, 0.40f, 0.75f})), glm::vec3(0.20f, 0.40f, 0.75f));
    } else if (root.contains("skyColor")) {
        // Migrate scenes saved before the sky became a horizon/zenith gradient — keep the old
        // flat color as the horizon and fall back to the default zenith blue above it.
        world.SkyHorizonColor = JsonToVec3(root["skyColor"], glm::vec3(0.53f, 0.72f, 0.86f));
        world.SkyZenithColor = glm::vec3(0.20f, 0.40f, 0.75f);
    } else {
        world.SkyHorizonColor = glm::vec3(0.53f, 0.72f, 0.86f);
        world.SkyZenithColor = glm::vec3(0.20f, 0.40f, 0.75f);
    }
    // #196: scenes saved before IBL existed carry no ambient intensity — 1.0 (the physically
    // consistent value) is the right default for them, same as a brand-new scene.
    if (clearFirst) {
        world.SkyAmbientIntensity    = root.value("skyAmbientIntensity",    1.0f);
        // PR13: HDRI sky source fields (absent in old scenes → Procedural defaults)
        world.SkySourceMode          = (World::SkySource)std::clamp(root.value("skySource", 0), 0, 1); // #122
        world.SkyHdriPath            = ResolveAssetRef(root, "skyHdriPath", "skyHdriGuid"); // audit #364, #132
        world.SkyRotationDegrees     = root.value("skyRotationDegrees",     0.0f);
        world.SkyHdriSun             = (World::HdriSunMode)std::clamp(root.value("skyHdriSun", 0), 0, 2); // #277
        world.SkyHdriSunThreshold    = std::max(1.0f, root.value("skyHdriSunThreshold", 50.0f));

        // #9, Phase M item 1 — v3+ scenes own these directly; a pre-v3 file has none of these
        // keys (BuildSceneJson only started writing them at v3), which is exactly the signal to
        // migrate the user's real values forward from editor_prefs.json instead of defaulting.
        if (root.contains("exposureEV")) {
            world.ExposureEV       = root.value("exposureEV", 0.0f);
            world.TonemapOperator  = root.value("tonemapOperator", 1);
            world.MsaaSamples      = root.value("msaaSamples", 4);
            world.SsaoEnabled      = root.value("ssaoEnabled", false);
            world.SsaoRadius       = std::clamp(root.value("ssaoRadius", 0.5f), 0.05f, 5.0f);
            world.SsaoBias         = std::clamp(root.value("ssaoBias", 0.025f), 0.0f, 0.5f);
            world.SsaoIntensity    = std::clamp(root.value("ssaoIntensity", 1.0f), 0.1f, 4.0f);
            world.BloomEnabled     = root.value("bloomEnabled", false);
            world.BloomThreshold   = root.value("bloomThreshold", 1.0f);
            world.BloomKnee        = root.value("bloomKnee", 0.5f);
            world.BloomIntensity   = root.value("bloomIntensity", 0.25f);
            // #162 - absent in older scenes: neutral.
            world.FxaaEnabled        = root.value("fxaa", false);
            world.GradeTemperature   = std::clamp(root.value("gradeTemperature", 0.0f), -100.0f, 100.0f);
            world.GradeTint          = std::clamp(root.value("gradeTint", 0.0f), -100.0f, 100.0f);
            world.GradeContrast      = std::clamp(root.value("gradeContrast", 0.0f), -100.0f, 100.0f);
            world.GradeSaturation    = std::clamp(root.value("gradeSaturation", 0.0f), -100.0f, 100.0f);
            world.GradeColorFilter   = glm::vec3(1.0f);
            if (root.contains("gradeColorFilter") && root["gradeColorFilter"].is_array() && root["gradeColorFilter"].size() == 3)
                for (int i = 0; i < 3; ++i)
                    if (root["gradeColorFilter"][i].is_number())
                        world.GradeColorFilter[i] = std::max(0.0f, root["gradeColorFilter"][i].get<float>());
            world.VignetteIntensity  = std::clamp(root.value("vignetteIntensity", 0.0f), 0.0f, 1.0f);
            world.VignetteSmoothness = std::clamp(root.value("vignetteSmoothness", 0.4f), 0.01f, 1.0f);
            world.FogEnabled       = root.value("fogEnabled", false);
            world.FogMode          = std::clamp(root.value("fogMode", 2), 1, 3);
            world.FogColor         = glm::vec3(0.55f, 0.62f, 0.72f);
            if (root.contains("fogColor") && root["fogColor"].is_array() && root["fogColor"].size() == 3)
                for (int i = 0; i < 3; ++i)
                    if (root["fogColor"][i].is_number())
                        world.FogColor[i] = std::max(0.0f, root["fogColor"][i].get<float>());
            world.FogDensity       = std::max(0.0f, root.value("fogDensity", 0.01f));
            world.FogStart         = root.value("fogStart", 10.0f);
            world.FogEnd           = root.value("fogEnd", 300.0f);
            world.FogHeightFalloff = std::max(0.0f, root.value("fogHeightFalloff", 0.0f));
            world.FogBaseHeight    = root.value("fogBaseHeight", 0.0f);
            world.ShadowsEnabled   = root.value("shadowsEnabled", true);
            world.ShadowResolution = root.value("shadowResolution", 4096);
            world.ShadowCascades   = root.value("shadowCascades", 4);
            world.ShadowDistance   = root.value("shadowDistance", 500.0f);
        } else {
            MigratePostProcessSettingsFromPrefs(world);
        }
    }

    // Reconstructs HierarchyComponent parent links from the "id"/"parentId" fields written by
    // BuildSceneJson — both entries are created first (order-independent), then parents are
    // applied in a second pass so a parentId can point forward or backward in the file.
    std::unordered_map<int, entt::entity> idToEntity;
    std::vector<std::pair<entt::entity, int>> pendingParents;

    // OrderComponent (see Components.h): a full load restores it verbatim so save->load->save is
    // order-stable; a paste/prefab append (!clearFirst) keeps the fresh trailing value CreateXxx
    // already assigned, so it lands after the current scene. Legacy files with no "order" field
    // get sequential file-order values here, matching the old boxes->models->empties behaviour.
    int fallbackOrder = 0;
    int maxLoadedOrder = -1;
    // #119 — append mode: the fragment's own order values -> the new copies, used below to
    // repoint intra-fragment joint references and handed back to the caller.
    std::unordered_map<int, entt::entity> sourceOrder;
    auto applyOrder = [&](entt::entity e, const json& j) {
        if (!clearFirst) {
            if (auto it = j.find("order"); it != j.end() && it->is_number_integer())
                sourceOrder[it->get<int>()] = e;
            return;
        }
        int order = j.value("order", fallbackOrder);
        fallbackOrder = std::max(fallbackOrder, order) + 1;
        maxLoadedOrder = std::max(maxLoadedOrder, order);
        world.Registry.emplace_or_replace<OrderComponent>(e, order);
    };

    if (root.contains("boxes")) {
        for (const auto& b : root["boxes"]) {
            // "alive" is a pre-ECS field: a shot-dead box used to be soft-deleted (kept in the
            // file, flagged inert) rather than removed, so an old save can still contain one —
            // skip creating it at all, which is externally indistinguishable from the old
            // soft-deleted state (nothing before ever displayed or collided with a dead box).
            if (!b.value("alive", true)) continue;

            glm::vec3 center = JsonToVec3(b.value("center", json::array({0, 0, 0})));
            glm::vec3 size = JsonToVec3(b.value("size", json::array({1, 1, 1})), glm::vec3(1.0f));
            glm::vec3 color = JsonToVec3(b.value("color", json::array({1, 1, 1})), glm::vec3(1.0f));
            glm::vec3 rotation = JsonToVec3(b.value("rotation", json::array({0, 0, 0})));
            std::string name = b.value("name", std::string());
            entt::entity e = world.CreateBox(center, size, color, rotation, name);
            ReadMaterialSlots(assets, b, world.Registry.get<RenderableComponent>(e)); // #120
            ReadRendererFlags(b, world.Registry.get<RenderableComponent>(e)); // #163
            ReadCommonComponents(b, world, assets, e);
            applyOrder(e, b);
            created(e);

            int id = b.value("id", -1);
            if (id >= 0) idToEntity[id] = e;
            int parentId = b.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(e, parentId);
        }
    }

    if (root.contains("models")) {
        for (const auto& m : root["models"]) {
            std::string modelPath = ResolveAssetRef(m, "path", "pathGuid");
            if (modelPath.empty()) continue;

            auto model = assets.InstantiateModel(modelPath);

            std::string name = m.value("name", std::string("Model"));
            glm::vec3 position = JsonToVec3(m.value("position", json::array({0, 0, 0})));
            glm::vec3 rotation = JsonToVec3(m.value("rotation", json::array({0, 0, 0})));
            glm::vec3 scale = JsonToVec3(m.value("scale", json::array({1, 1, 1})), glm::vec3(1.0f));

            entt::entity e = world.CreateModelEntity(model, position, rotation, scale, name);
            auto& rc = world.Registry.get<RenderableComponent>(e);

            ReadMaterialSlots(assets, m, rc);
            ReadRendererFlags(m, rc); // #163
            ReadCommonComponents(m, world, assets, e); // handles the "Audio Source" block + the legacy "sound*" shim
            applyOrder(e, m);
            created(e);

            int id = m.value("id", -1);
            if (id >= 0) idToEntity[id] = e;
            int parentId = m.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(e, parentId);
        }
    }

    if (root.contains("empties")) {
        for (const auto& en : root["empties"]) {
            glm::vec3 position = JsonToVec3(en.value("position", json::array({0, 0, 0})));
            glm::vec3 rotation = JsonToVec3(en.value("rotation", json::array({0, 0, 0})));
            glm::vec3 scale = JsonToVec3(en.value("scale", json::array({1, 1, 1})), glm::vec3(1.0f));
            entt::entity e = world.CreateEmptyEntity(position, rotation, scale,
                en.value("name", std::string("Empty")));
            ReadCommonComponents(en, world, assets, e);
            applyOrder(e, en);
            created(e);

            int id = en.value("id", -1);
            if (id >= 0) idToEntity[id] = e;
            int parentId = en.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(e, parentId);
        }
    }

    // #236 A2 — expand prefab-instance stubs: rebuild the subtree from the .prefab, then apply
    // the stub's transform-only overrides on the root. A missing source becomes a visible
    // broken placeholder rather than a silent hole.
    if (root.contains("prefabInstances") && root["prefabInstances"].is_array()) {
        for (const auto& s : root["prefabInstances"]) {
            const std::string src = ResolveAssetRef(s, "source", "sourceGuid");
            glm::vec3 position = JsonToVec3(s.value("position", json::array({0, 0, 0})));
            glm::vec3 rotation = JsonToVec3(s.value("rotation", json::array({0, 0, 0})));
            glm::vec3 scale = JsonToVec3(s.value("scale", json::array({1, 1, 1})), glm::vec3(1.0f));
            const std::string name = s.value("name", std::string("Prefab Instance"));

            std::vector<entt::entity> instCreated;
            entt::entity rootE = entt::null;
            if (!src.empty())
                rootE = SceneSerializer::InstantiatePrefab(world, assets, src, &instCreated);

            const bool missing = (rootE == entt::null);
            if (missing) {
                rootE = world.CreateEmptyEntity(position, rotation, scale, name);
                world.Registry.emplace_or_replace<PrefabInstanceComponent>(
                    rootE, PrefabInstanceComponent{src, /*Missing=*/true});
                if (!src.empty())
                    Log::Warn("Prefab instance: source '" + src + "' could not be loaded; "
                              "inserted a placeholder so the reference isn't lost.");
                instCreated.push_back(rootE);
            } else {
                auto& t = world.Registry.get<TransformComponent>(rootE);
                t.Position = position;
                t.RotationEuler = rotation;
                t.Scale = scale;
                world.Registry.emplace_or_replace<NameComponent>(rootE, NameComponent{name});
            }

            // Root-level overrides carried by the stub.
            if (!s.value("active", true)) world.Registry.emplace_or_replace<InactiveTag>(rootE);
            else                          world.Registry.remove<InactiveTag>(rootE);
            if (s.value("static", false)) world.Registry.emplace_or_replace<StaticTag>(rootE);
            if (const int layer = s.value("layer", 0); layer > 0 && layer < LayerRegistry::kCount)
                world.Registry.emplace_or_replace<LayerComponent>(rootE, LayerComponent{layer});
            if (s.contains("tag"))
                world.Registry.emplace_or_replace<TagComponent>(rootE, s["tag"].get<std::string>());
            if (s.value("sceneHidden", false)) world.Registry.emplace_or_replace<HiddenInSceneTag>(rootE); // #122
            if (s.value("sceneLocked", false)) world.Registry.emplace_or_replace<SceneLockedTag>(rootE);

            // #302 Part B — overrides: replay onto the freshly rebuilt subtree, by prefab-local
            // index into instCreated (== the .prefab file's entity order). Two passes so a field
            // override can't land before its addComponent (matters for hand-edited files; the
            // diff already emits them in order).
            if (!missing && s.contains("overrides") && s["overrides"].is_array()) {
                auto entityFor = [&](const json& ov) -> entt::entity {
                    const int idx = ov.value("e", -1);
                    return (idx >= 0 && idx < (int)instCreated.size() && world.Registry.valid(instCreated[idx]))
                               ? instCreated[idx] : entt::null;
                };
                for (const auto& ov : s["overrides"]) {                     // pass 1: structural
                    const std::string op = ov.value("op", std::string());
                    if (op.empty()) continue;
                    entt::entity e = entityFor(ov);
                    if (e == entt::null) continue;
                    const std::string cn = ov.value("c", std::string());
                    for (const auto& rc : ComponentRegistry::All()) {
                        if (rc.Meta.Name != cn) continue;
                        if (op == "addComponent")         { if (!rc.Has(world.Registry, e)) rc.Add(world.Registry, e); }
                        else if (op == "removeComponent") { if (rc.Has(world.Registry, e)) rc.Remove(world.Registry, e); }
                        break;
                    }
                }
                for (const auto& ov : s["overrides"]) {                     // pass 2: field values
                    if (ov.contains("op") || !ov.contains("v")) continue;
                    entt::entity e = entityFor(ov);
                    if (e == entt::null) continue;
                    ApplyPrefabOverride(world, assets, e,
                                        ov.value("c", std::string()), ov.value("f", std::string()), ov["v"]);
                }
            }

            applyOrder(rootE, s);
            for (entt::entity e : instCreated) created(e);

            const int id = s.value("id", -1);
            if (id >= 0) idToEntity[id] = rootE;
            const int parentId = s.value("parentId", -1);
            if (parentId >= 0) pendingParents.emplace_back(rootE, parentId);
        }
    }

    if (clearFirst && maxLoadedOrder >= 0) world.EnsureNextOrderAtLeast(maxLoadedOrder + 1);

    for (const auto& [child, parentId] : pendingParents) {
        auto it = idToEntity.find(parentId);
        if (it != idToEntity.end()) world.AttachChildRaw(child, it->second);
    }

    // #119 — a copied joint whose partner was copied with it must connect to the partner's copy,
    // not the original (a partner outside the fragment keeps the original reference).
    if (!clearFirst) {
        for (const auto& [oldOrder, e] : sourceOrder) {
            auto* joint = world.Registry.try_get<JointComponent>(e);
            if (!joint || joint->ConnectedOrder < 0) continue;
            auto partner = sourceOrder.find(joint->ConnectedOrder);
            if (partner == sourceOrder.end()) continue;
            if (const auto* o = world.Registry.try_get<OrderComponent>(partner->second))
                joint->ConnectedOrder = o->Value;
        }
        if (outSourceOrder) *outSourceOrder = std::move(sourceOrder);
    }

    return true;
}

// Persists the Asset Browser's whole library (not just what's placed in the scene) plus its
// virtual folder structure and any renamed assets — otherwise an imported-but-unused asset,
// or one you'd organized into a folder, would simply vanish on the next launch.
void AppendAssetLibraryJson(json& root, const AssetLibrary& assets) {
    json modelPaths = json::array();
    for (const auto& m : assets.Models()) modelPaths.push_back(PathRef(m->Path()));
    root["libraryModels"] = modelPaths;

    json texPaths = json::array();
    for (const auto& t : assets.Textures()) texPaths.push_back(PathRef(t->Path()));
    root["libraryTextures"] = texPaths;

    {
        json arr = json::array();
        for (const auto& m : assets.Materials()) arr.push_back(PathRef(m->Path));
        root["libraryMaterials"] = arr;
    }

    {
        json arr = json::array();
        for (const auto& p : assets.Sounds()) arr.push_back(PathRef(p));
        root["librarySounds"] = arr;
    }
    {
        json arr = json::array();
        for (const auto& p : assets.Prefabs()) arr.push_back(PathRef(p));
        root["libraryPrefabs"] = arr;
    }
    root["assetFolders"] = assets.Folders();

    json meta = json::array();
    auto findOrCreate = [&](const std::string& key) -> json& {
        const std::string stored = AssetPathForWrite(key); // audit #364 — portable path
        for (auto& entry : meta) {
            if (entry["path"] == stored) return entry;
        }
        json entry{{"path", stored}};
    AssetGuid g = AssetDatabase::GuidForPath(key);
    if (g.IsValid()) entry["guid"] = g.ToString();
    meta.push_back(std::move(entry));
        return meta.back();
    };
    for (const auto& [key, folder] : assets.AssetFolders()) findOrCreate(key)["folder"] = folder;
    for (const auto& [key, name] : assets.DisplayNames()) findOrCreate(key)["displayName"] = name;
    for (const auto& [key, labels] : assets.LabelsMap()) findOrCreate(key)["labels"] = labels;
    for (const auto& [key, s] : assets.TextureSettingsMap()) {
        findOrCreate(key)["textureImport"] = {
            {"textureType", (int)s.TextureType}, {"generateMipmaps", s.GenerateMipmaps},
            {"isSRGB", s.IsSRGB}, {"filterMode", (int)s.FilterMode},
            {"wrapMode", (int)s.WrapMode}, {"maxTextureSize", s.MaxTextureSize},
            {"anisoLevel", s.AnisoLevel}, {"compression", (int)s.CompressionMode},
        };
    }
    for (const auto& [key, s] : assets.ModelSettingsMap()) {
        findOrCreate(key)["modelImport"] = {
            {"globalScale", s.GlobalScale}, {"importNormals", s.ImportNormals},
            {"importAnimations", s.ImportAnimations}, {"importSkeleton", s.ImportSkeleton},
            {"optimizeGraph", s.OptimizeGraph}, {"materialImportMode", (int)s.MaterialImportMode},
        };
    }
    // Drop assetMeta entries whose asset no longer exists on disk (audit #364): stale rows for
    // deleted files — e.g. old "Untitled.json" scenes — otherwise persist forever and, when
    // authored on another machine, carry a dead absolute path into every save.
    {
        json live = json::array();
        for (auto& entry : meta) {
            AssetGuid g = AssetGuid::FromString(entry.value("guid", std::string()));
            std::string resolved = AssetDatabase::Resolve(g, AssetPathForRead(entry.value("path", std::string())));
            std::error_code ec;
            if (!resolved.empty() && std::filesystem::exists(resolved, ec) && !ec)
                live.push_back(std::move(entry));
        }
        meta = std::move(live);
    }
    root["assetMeta"] = meta;
}

void ApplyAssetLibraryJson(AssetLibrary& assets, const json& root) {
    // assetMeta is read FIRST, before any LoadModel/LoadTexture call below, so that the import
    // settings (and folder/display-name/labels) are already known by the time an asset is
    // actually loaded. LoadModel/LoadTexture consult GetModelSettings/GetTextureSettings
    // themselves, so populating these maps up front makes each asset get imported exactly once,
    // with the correct settings, instead of once with defaults and once more via Reimport.
    if (root.contains("assetMeta")) {
        for (const auto& entry : root["assetMeta"]) {
            // v2: prefer guid resolution; v1: plain path.
            AssetGuid g = AssetGuid::FromString(entry.value("guid", std::string()));
            std::string fallback = AssetPathForRead(entry.value("path", std::string())); // audit #364
            std::string path = AssetDatabase::Resolve(g, fallback);
            if (path.empty()) continue;
            if (entry.contains("folder")) assets.SetAssetFolder(path, entry["folder"].get<std::string>());
            if (entry.contains("displayName")) assets.SetDisplayName(path, entry["displayName"].get<std::string>());
            if (entry.contains("labels")) {
                std::set<std::string> labels;
                for (const auto& l : entry["labels"]) labels.insert(l.get<std::string>());
                assets.SetLabels(path, labels);
            }
            if (entry.contains("textureImport")) {
                const auto& t = entry["textureImport"];
                TextureImportSettings s;
                // Clamp: a scene saved before #198 removed the unused Cubemap enum value could
                // still carry that old index (3) — fall back to Default rather than construct an
                // out-of-range enum.
                int rawType = t.value("textureType", 0);
                s.TextureType = (rawType >= 0 && rawType <= (int)TextureImportSettings::Type::Sprite2D)
                    ? (TextureImportSettings::Type)rawType : TextureImportSettings::Type::Default;
                s.GenerateMipmaps = t.value("generateMipmaps", true);
                s.IsSRGB = t.value("isSRGB", true);
                s.FilterMode = (TextureImportSettings::Filter)std::clamp(t.value("filterMode", 1), 0, 2); // #122
                s.WrapMode = (TextureImportSettings::Wrap)std::clamp(t.value("wrapMode", 0), 0, 1);
                s.MaxTextureSize = t.value("maxTextureSize", 2048);
                s.AnisoLevel = std::clamp(t.value("anisoLevel", 8), 1, 16);
                s.CompressionMode = (TextureImportSettings::Compression)std::clamp(t.value("compression", 0), 0, 2);
                assets.SetTextureSettings(path, s);
            }
            if (entry.contains("modelImport")) {
                const auto& m = entry["modelImport"];
                ModelImportSettings s;
                s.GlobalScale = m.value("globalScale", 1.0f);
                s.ImportNormals = m.value("importNormals", true);
                s.ImportAnimations = m.value("importAnimations", true);
                s.ImportSkeleton = m.value("importSkeleton", true);
                s.OptimizeGraph = m.value("optimizeGraph", true);
                s.MaterialImportMode = (ModelImportSettings::MaterialMode)std::clamp(m.value("materialImportMode", 0), 0, 2); // #122
                assets.SetModelSettings(path, s);
            }
        }
    }
    if (root.contains("libraryModels")) {
        for (const auto& p : root["libraryModels"]) {
            std::string path = ResolveAssetRef(p);
            if (!path.empty()) assets.LoadModel(path);
        }
    }
    if (root.contains("libraryTextures")) {
        for (const auto& p : root["libraryTextures"]) {
            std::string path = ResolveAssetRef(p);
            if (!path.empty()) assets.LoadTexture(path);
        }
    }
    if (root.contains("libraryMaterials")) {
        for (const auto& p : root["libraryMaterials"]) {
            std::string path = ResolveAssetRef(p);
            if (!path.empty()) assets.LoadMaterial(path);
        }
    }
    if (root.contains("librarySounds")) {
        for (const auto& p : root["librarySounds"]) {
            std::string path = ResolveAssetRef(p);
            if (!path.empty()) assets.RegisterSound(path);
        }
    }
    if (root.contains("libraryPrefabs")) {
        for (const auto& p : root["libraryPrefabs"]) {
            std::string path = ResolveAssetRef(p);
            if (!path.empty()) assets.RegisterPrefab(path);
        }
    }
    if (root.contains("assetFolders")) {
        for (const auto& f : root["assetFolders"]) assets.CreateFolder(f.get<std::string>());
    }
}

const json* CachedPrefabJson(const std::string& path) {
    auto it = g_prefabPristineCache.find(path);
    if (it != g_prefabPristineCache.end()) return &it->second;
    std::ifstream f(path);
    if (!f.is_open()) return nullptr;
    json parsed;
    try { f >> parsed; } catch (...) { return nullptr; }
    return &(g_prefabPristineCache[path] = std::move(parsed));
}

// Given any entity, find its prefab-instance root, the pristine .prefab entity object it was
// built from, and whether it IS the root. Returns {nullptr,...} if it isn't part of a live
// instance (or the .prefab is unreadable / the entity is a since-added child not in the file).
struct PristineHit { const json* ent = nullptr; bool isRoot = false; };
PristineHit FindPristineEntity(const World& world, entt::entity entity) {
    entt::entity root = entt::null;
    for (entt::entity cur = entity; cur != entt::null; ) {
        if (world.Registry.all_of<PrefabInstanceComponent>(cur)) { root = cur; break; }
        const auto* h = world.Registry.try_get<HierarchyComponent>(cur);
        cur = h ? h->Parent : entt::null;
    }
    if (root == entt::null) return {};
    const auto& pi = world.Registry.get<PrefabInstanceComponent>(root);
    int idx = -1;
    for (std::size_t i = 0; i < pi.InstanceEntities.size(); ++i)
        if (pi.InstanceEntities[i] == entity) { idx = (int)i; break; }
    if (idx < 0) return {};
    const json* pj = CachedPrefabJson(pi.SourcePath);
    if (!pj) return {};
    std::vector<const json*> ents = PrefabEntityObjects(*pj);
    if (idx >= (int)ents.size()) return {};
    return { ents[idx], root == entity };
}

// The pristine JSON value for (component, field) on `hit.ent`, or a null json if absent.
// `component` is a ReflectComponent::Name (a display label, e.g. from PrefabOverrideLabel/
// DrawReflectedField) — translated to its JSON Key before touching `hit.ent`, since a
// GenericSerialize=false component (Collider/Joint) can have a legacy JSON key that differs from
// its Name (see ReflectComponent::Key). Using Name directly here would find nothing under the
// pristine entity's actual (differently-spelled) key and silently report "not overridden" for
// every field of such a component, no matter what the live value actually is.
json PristineFieldValue(const PristineHit& hit, const char* component, const char* field) {
    if (!hit.ent) return nullptr;
    if (std::strcmp(component, "Transform") == 0)
        return hit.ent->contains(field) ? hit.ent->at(field) : json(nullptr);
    if (std::strcmp(component, "Name") == 0)
        return json(hit.ent->value("name", std::string()));
    const char* jsonKey = component;
    for (const auto& rc : ComponentRegistry::All())
        if (std::strcmp(rc.Meta.Name, component) == 0) { jsonKey = ReflectComponentKey(rc.Meta); break; }
    if (!hit.ent->contains(jsonKey)) return nullptr;
    const json& cj = hit.ent->at(jsonKey);
    return cj.contains(field) ? cj.at(field) : json(nullptr);
}

// The live value of (component, field) on `entity`, JSON-encoded the same way the .prefab file
// stores it. Null json if the field / component isn't applicable to this entity.
json LiveFieldValue(const World& world, entt::entity entity, const char* component, const char* field) {
    if (std::strcmp(component, "Transform") == 0) {
        const auto* t = world.Registry.try_get<TransformComponent>(entity);
        if (!t) return nullptr;
        if (std::strcmp(field, "position") == 0) return Vec3ToJson(t->Position);
        if (std::strcmp(field, "rotation") == 0) return Vec3ToJson(t->RotationEuler);
        if (std::strcmp(field, "scale")    == 0) return Vec3ToJson(t->Scale);
        return nullptr;
    }
    if (std::strcmp(component, "Name") == 0) {
        const auto* nc = world.Registry.try_get<NameComponent>(entity);
        return nc ? json(nc->Name) : json(nullptr);
    }
    for (const auto& rc : ComponentRegistry::All()) {
        if (std::strcmp(rc.Meta.Name, component) != 0 || !rc.Has(world.Registry, entity)) continue;
        const void* comp = rc.GetConst(world.Registry, entity);
        for (const auto& f : rc.Meta.Fields)
            if (ReflectFieldMatchesKey(f, field))
                return ReflectFieldToJson(f, f.Address(const_cast<void*>(comp)));
        return nullptr;
    }
    return nullptr;
}

// Mutable counterpart of PrefabEntityObjects: the idx-th entity object in canonical order
// (boxes, then models, then empties, each in file order), or nullptr if out of range.
json* PrefabEntityObjectMutable(json& prefab, int idx) {
    int seen = 0;
    for (const char* key : {"boxes", "models", "empties"})
        if (auto it = prefab.find(key); it != prefab.end() && it->is_array())
            for (json& e : *it) { if (seen++ == idx) return &e; }
    return nullptr;
}

// audit #364 validation guard: after a scene JSON is built, warn about any string value that
// still looks machine-absolute (drive letter, UNC prefix) or contains a "../" traversal. These
// don't survive a move to another machine/checkout. A path to a genuinely out-of-project shared
// asset library legitimately stays absolute, so this warns (names the JSON pointer) rather than
// failing the save.
bool LooksNonPortable(const std::string& s) {
    if (s.size() >= 2 && std::isalpha((unsigned char)s[0]) && s[1] == ':' &&
        (s.size() == 2 || s[2] == '/' || s[2] == '\\'))
        return true;                                   // C:\ or C:/
    if (s.rfind("\\\\", 0) == 0) return true;          // \\server\share (UNC)
    if (s.find("/../") != std::string::npos || s.find("\\..\\") != std::string::npos ||
        s.rfind("../", 0) == 0 || s.rfind("..\\", 0) == 0)
        return true;
    return false;
}

void WarnNonPortablePaths(const json& node, const std::string& pointer) {
    if (node.is_string()) {
        const std::string& s = node.get_ref<const std::string&>();
        if (LooksNonPortable(s))
            Log::Warn("Scene: non-portable path at '" + pointer + "': '" + s +
                      "' — will not resolve on another machine/checkout (audit #364).");
    } else if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it)
            WarnNonPortablePaths(it.value(), pointer + "/" + it.key());
    } else if (node.is_array()) {
        for (size_t i = 0; i < node.size(); ++i)
            WarnNonPortablePaths(node[i], pointer + "/" + std::to_string(i));
    }
}

} // namespace

bool SceneSerializer::Save(const World& world, const AssetLibrary& assets, const std::string& path) {
    // #82 — build the whole document in memory FIRST, and only then touch the file, through
    // AtomicFile (temp + flush + atomic replace). The old version opened a truncating ofstream
    // before building, so any exception while building (e.g. a corrupt prefab) left the scene
    // file empty; and it never checked the write, so a full disk / locked file still reported
    // success.
    std::string text;
    try {
        json root = BuildSceneJson(world);
        AppendAssetLibraryJson(root, assets);
        WarnNonPortablePaths(root, ""); // audit #364 — surface any absolute/'..' path before it's written
        text = root.dump(2);
    } catch (const std::exception& e) {
        Log::Error("Scene: couldn't serialize the scene for '" + path + "' (file left untouched): " + e.what());
        return false;
    }
    if (!AtomicFile::WriteBytes(std::filesystem::path(path), text, /*binary=*/false)) {
        Log::Error("Scene: failed to write '" + path + "' - the previous version on disk was kept.");
        return false;
    }
    return true;
}

std::string SceneSerializer::TakeLoadWarning() {
    std::string warning = std::move(g_LastLoadWarning);
    g_LastLoadWarning.clear();
    return warning;
}

bool SceneSerializer::Load(World& world, AssetLibrary& assets, const std::string& path, bool persistMigration) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        Log::Error("Scene: failed to parse '" + path + "': " + e.what());
        return false;
    }
    // Release the file now: the migration write-back below replaces `path` atomically, and on
    // Windows that fails with "Access is denied" while this stream still holds it open.
    in.close();
    // Valid JSON but the wrong shape (e.g. a bare `[]` or a number) parses fine above but throws
    // a json::type_error the moment anything below calls .value()/.contains() on it, since those
    // require an object. Reject it here as a load failure — same contract as a parse error —
    // instead of letting that exception escape uncaught all the way to main()'s outermost catch
    // and take down the whole process/--smoke-test batch over one bad scene (audit #76).
    if (!root.is_object()) {
        Log::Error("Scene: '" + path + "' is valid JSON but not a scene object (top-level type is " +
                   std::string(root.type_name()) + ").");
        return false;
    }

    // #9, Phase M item 1: one .bak of the whole file before a pre-v3 load mutates anything in
    // memory, per the review's Q13 "one backup" requirement. .json.bak is already in .gitignore.
    // Skipped when the caller won't persist the migration either (audit #77) — no write-back
    // coming means there's nothing for the backup to protect against.
    const int formatVersion = root.value("formatVersion", 0);
    const bool migrating = formatVersion < kSceneFormatVersion;
    if (migrating && persistMigration) {
        std::error_code ec;
        std::filesystem::copy_file(path, path + ".bak", std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) Log::Warn("Scene: couldn't write a backup of '" + path + "' before migrating it: " + ec.message());
    }

    g_MigrationLog.clear();
    // #83 — keep the current world so a malformed file can't leave a half-loaded one behind.
    std::string previous;
    try { previous = SaveToString(world, assets); } catch (...) {}
    bool ok = true;
    try {
        ApplyAssetLibraryJson(assets, root); // before ApplySceneJson: harmless either order, but library assets should exist first
    } catch (const std::exception& e) {
        Log::Error("Scene: asset library data in '" + path + "' is malformed: " + e.what());
        ok = false;
    }
    if (ok) ok = ApplySceneJson(world, assets, root);
    if (!ok) {
        Log::Error("Scene: '" + path + "' could not be loaded; the previous scene was kept.");
        if (!previous.empty()) LoadFromString(world, assets, previous);
        return false;
    }

    if (ok && migrating && !g_MigrationLog.empty()) {
        Log::Info((persistMigration ? "Scene: upgraded '" : "Scene: would upgrade (not persisting) '") + path +
                   "' to format v" + std::to_string(kSceneFormatVersion) + ":");
        for (const auto& line : g_MigrationLog) Log::Info("  - " + line);
        if (persistMigration) {
            // Write the upgrade back immediately (review §7 Q13: "one build upgrade" moves the
            // project in one pass) so a second load doesn't re-read the now-stale prefs values.
            Save(world, assets, path);
        }
        // persistMigration == false: the migrated data lives only in `world`/`assets` for this
        // run (a --smoke-test scene, a --resave input) — `path` on disk is never touched.
    }
    return ok;
}

std::string SceneSerializer::SaveToString(const World& world) {
    return BuildSceneJson(world).dump();
}

bool SceneSerializer::SaveSnapshotToFile(const std::string& entitySnapshot, const AssetLibrary& assets,
                                         const std::string& path) {
    std::string text;
    try {
        json root = json::parse(entitySnapshot);
        if (!root.is_object()) return false;
        AppendAssetLibraryJson(root, assets);
        text = root.dump(2);
    } catch (const std::exception&) {
        return false;
    }
    return AtomicFile::WriteBytes(std::filesystem::path(path), text, /*binary=*/false);
}

std::string SceneSerializer::SaveToString(const World& world, const AssetLibrary& assets) {
    json root = BuildSceneJson(world);
    // Marks this snapshot as carrying real AssetLibrary state, so LoadFromString below knows to
    // treat it as a full replace rather than mistaking an entity-only snapshot (the play-mode
    // one) for "nothing changed, don't touch the library."
    root["hasAssetLibrarySnapshot"] = true;
    AppendAssetLibraryJson(root, assets);
    return root.dump();
}

bool SceneSerializer::LoadFromString(World& world, AssetLibrary& assets, const std::string& data) {
    json root;
    try {
        root = json::parse(data);
    } catch (const std::exception& e) {
        Log::Error(std::string("Scene: failed to parse snapshot: ") + e.what());
        return false;
    }
    // Same guard as Load() (audit #76): valid-but-non-object JSON parses fine but throws on the
    // first .value()/.contains() call below, which would otherwise escape uncaught.
    if (!root.is_object()) {
        Log::Error(std::string("Scene: snapshot is valid JSON but not a scene object (top-level type is ") +
                   root.type_name() + ").");
        return false;
    }
    // Only a snapshot built via the AssetLibrary-aware SaveToString() overload (undo/redo)
    // carries this - the entity-only overload (the play-mode snapshot) has no library state at
    // all, and touching the library there would affect the Asset Browser on exiting Play for no
    // reason. This is a TRUE replace (an asset created/deleted/renamed after this snapshot was
    // taken ends up correctly gone/restored, not merged with whatever's here now) but a CHEAP
    // one: PruneToKeepSet only drops cache entries for assets that shouldn't survive - it never
    // touches anything that's staying, so a texture/model that's already loaded and still wanted
    // is never re-decoded or re-imported from disk. Only genuinely-missing entries (something
    // that was deleted after this snapshot, now being undone back) cost a real reimport, via the
    // ordinary LoadModel/LoadTexture calls ApplyAssetLibraryJson below already makes - an earlier
    // version of this used a full assets.Clear() instead, which reimported the ENTIRE library on
    // every single undo/redo step, however small the actual edit — a real, serious regression
    // (multi-second stalls long enough to trip a GPU driver watchdog on a real project's worth
    // of assets), not just a style choice being reverted here.
    if (root.value("hasAssetLibrarySnapshot", false)) {
        // #81 — library entries are written through PathRef(), so each one is either a plain
        // project-relative string or a {"path","pathGuid"} object; and the live library keys its
        // assets by the (usually absolute) path they were loaded from. Match the two in the
        // WRITTEN form (AssetPathForWrite, or the GUID when both sides have one) and hand
        // PruneToKeepSet the live path, so a still-wanted asset is recognised and kept instead of
        // the old raw get<std::string>() throwing on the first object entry (crashing Undo) or,
        // for plain strings, never matching and re-importing the whole library every step.
        auto keepSet = [&](const char* key, const std::vector<std::string>& livePaths) {
            std::set<std::string> keep;
            auto it = root.find(key);
            if (it == root.end() || !it->is_array()) return keep;
            std::set<std::string> storedPaths, storedGuids;
            for (const auto& p : *it) {
                if (p.is_string()) storedPaths.insert(p.get<std::string>());
                else if (p.is_object()) {
                    const auto path = p.find("path");
                    if (path != p.end() && path->is_string()) storedPaths.insert(path->get<std::string>());
                    const auto guid = p.find("pathGuid");
                    if (guid != p.end() && guid->is_string()) storedGuids.insert(guid->get<std::string>());
                }
            }
            for (const auto& live : livePaths) {
                const AssetGuid g = AssetDatabase::GuidForPath(live);
                if (storedPaths.count(AssetPathForWrite(live)) || storedPaths.count(live) ||
                    (g.IsValid() && storedGuids.count(g.ToString())))
                    keep.insert(live);
            }
            return keep;
        };
        std::vector<std::string> liveModels, liveTextures, liveMaterials;
        for (const auto& m : assets.Models()) liveModels.push_back(m->Path());
        for (const auto& t : assets.Textures()) liveTextures.push_back(t->Path());
        for (const auto& m : assets.Materials()) liveMaterials.push_back(m->Path);
        std::set<std::string> keepModels = keepSet("libraryModels", liveModels);
        std::set<std::string> keepTextures = keepSet("libraryTextures", liveTextures);
        std::set<std::string> keepSounds = keepSet("librarySounds", assets.Sounds());
        std::set<std::string> keepPrefabs = keepSet("libraryPrefabs", assets.Prefabs());
        std::set<std::string> keepMaterials = keepSet("libraryMaterials", liveMaterials);
        std::set<std::string> keepFolders;
        if (auto it = root.find("assetFolders"); it != root.end() && it->is_array())
            for (const auto& f : *it) if (f.is_string()) keepFolders.insert(f.get<std::string>());
        assets.PruneToKeepSet(keepModels, keepTextures, keepSounds, keepPrefabs, keepFolders, keepMaterials);
        assets.ClearMetadataOnly();
        try { ApplyAssetLibraryJson(assets, root); }
        catch (const std::exception& e) {
            Log::Error(std::string("Scene: snapshot asset library data is malformed: ") + e.what()); // #83
            return false;
        }
    }
    return ApplySceneJson(world, assets, root);
}

std::string SceneSerializer::SaveEntitiesToString(const World& world,
    const std::vector<entt::entity>& entities, bool flattenPrefabInstances) {
    // Descendants come along automatically: a fragment that kept a parent but dropped its
    // children would paste back as a visibly different object than the one that was copied.
    std::set<entt::entity> included;
    std::function<void(entt::entity)> addWithChildren = [&](entt::entity e) {
        if (e == entt::null || !world.Registry.valid(e) || !included.insert(e).second) return;
        if (const auto* hier = world.Registry.try_get<HierarchyComponent>(e)) {
            for (entt::entity child : hier->Children) addWithChildren(child);
        }
    };
    for (entt::entity e : entities) addWithChildren(e);

    return BuildSceneJson(world, &included, flattenPrefabInstances).dump();
}

bool SceneSerializer::AppendEntitiesFromString(World& world, AssetLibrary& assets,
    const std::string& data, std::vector<entt::entity>& outCreated,
    std::unordered_map<int, entt::entity>* outSourceOrder) {
    json root;
    try {
        root = json::parse(data);
    } catch (const std::exception& e) {
        Log::Error(std::string("Scene: failed to parse entity fragment: ") + e.what());
        return false;
    }
    return ApplySceneJson(world, assets, root, /*clearFirst=*/false, &outCreated, outSourceOrder);
}

bool SceneSerializer::SavePrefab(const World& world, entt::entity root, const std::string& path) {
    if (!world.Registry.valid(root)) return false;
    // Re-parsed and re-dumped with indentation so a prefab file is human-readable/diffable,
    // unlike the compact in-memory clipboard form the same function produces. flatten=true: a
    // .prefab asset is self-contained — if `root` (or a child) is itself a prefab instance it is
    // baked in fully here, not left as a nested link (#236 A2 — nested prefabs are stage 4).
    // #82 — atomic write, and the write result is checked.
    if (!AtomicFile::WriteJson(std::filesystem::path(path),
            json::parse(SaveEntitiesToString(world, {root}, /*flattenPrefabInstances=*/true)))) {
        Log::Error("Prefab: failed to write '" + path + "'.");
        return false;
    }
    Log::Info("Saved prefab '" + path + "'.");
    return true;
}

entt::entity SceneSerializer::InstantiatePrefab(World& world, AssetLibrary& assets,
    const std::string& path, std::vector<entt::entity>* outAll) {
    std::ifstream in(path);
    if (!in.is_open()) {
        Log::Error("Prefab: '" + path + "' could not be opened.");
        return entt::null;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();

    std::vector<entt::entity> created;
    if (!AppendEntitiesFromString(world, assets, buffer.str(), created) || created.empty()) {
        return entt::null;
    }
    if (outAll) outAll->insert(outAll->end(), created.begin(), created.end());

    // The prefab's root is whichever created entity has no parent inside the fragment.
    entt::entity root = created.front();
    for (entt::entity e : created) {
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        if (!hier || hier->Parent == entt::null) { root = e; break; }
    }
    // #236 A2 — link the instance to its source. Applies to every path that stamps a prefab:
    // drag-drop placement, "Place Instance", and scene-load stub expansion. The scene then
    // stores this instance as a stub, and edits to the .prefab propagate on the next load.
    // `InstanceEntities` keeps the full creation order (== the .prefab file's entity order) so
    // save-time per-field override diffing can pair each live entity with its pristine
    // counterpart in the file by index (#302 Part B).
    PrefabInstanceComponent pi{path};
    pi.InstanceEntities = created;
    world.Registry.emplace_or_replace<PrefabInstanceComponent>(root, std::move(pi));
    return root;
}

// --- Prefab per-field overrides, editor helpers (#302 Part B) -------------------------------

void SceneSerializer::ClearPrefabPristineCache() { g_prefabPristineCache.clear(); }

bool SceneSerializer::IsPrefabFieldOverridden(const World& world, entt::entity entity,
                                              const char* component, const char* field) {
    PristineHit hit = FindPristineEntity(world, entity);
    if (!hit.ent) return false;
    // The root's transform / name / tag are per-instance by design, not overrides.
    if (hit.isRoot && (std::strcmp(component, "Transform") == 0 || std::strcmp(component, "Name") == 0))
        return false;

    const json pristine = PristineFieldValue(hit, component, field);
    if (pristine.is_null()) return false; // field absent from the .prefab — nothing to diff against

    if (std::strcmp(component, "Transform") == 0) {
        const auto& t = world.Registry.get<TransformComponent>(entity);
        json now = std::strcmp(field, "position") == 0 ? Vec3ToJson(t.Position)
                 : std::strcmp(field, "rotation") == 0 ? Vec3ToJson(t.RotationEuler)
                 : std::strcmp(field, "scale")    == 0 ? Vec3ToJson(t.Scale) : json(nullptr);
        if (now.is_null()) return false;
        return !ReflectJsonNearlyEqual(ReflectFieldType::Vec3, now, pristine);
    }
    if (std::strcmp(component, "Name") == 0) {
        const auto* nc = world.Registry.try_get<NameComponent>(entity);
        return nc && json(nc->Name) != pristine;
    }
    for (const auto& rc : ComponentRegistry::All()) {
        if (std::strcmp(rc.Meta.Name, component) != 0 || !rc.Has(world.Registry, entity)) continue;
        const void* comp = rc.GetConst(world.Registry, entity);
        for (const auto& f : rc.Meta.Fields) {
            if (!ReflectFieldMatchesKey(f, field)) continue;
            json now = ReflectFieldToJson(f, f.Address(const_cast<void*>(comp)));
            return !ReflectJsonNearlyEqual(f.Type, now, pristine);
        }
        return false;
    }
    return false;
}

void SceneSerializer::RevertPrefabField(World& world, AssetLibrary& assets, entt::entity entity,
                                        const char* component, const char* field) {
    PristineHit hit = FindPristineEntity(world, entity);
    if (!hit.ent) return;
    const json pristine = PristineFieldValue(hit, component, field);
    if (pristine.is_null()) return;
    ApplyPrefabOverride(world, assets, entity, component, field, pristine);
}

bool SceneSerializer::ApplyPrefabField(World& world, entt::entity entity,
                                       const char* component, const char* field) {
    // Locate the instance root + this entity's prefab-local index.
    entt::entity root = entt::null;
    for (entt::entity cur = entity; cur != entt::null; ) {
        if (world.Registry.all_of<PrefabInstanceComponent>(cur)) { root = cur; break; }
        const auto* h = world.Registry.try_get<HierarchyComponent>(cur);
        cur = h ? h->Parent : entt::null;
    }
    if (root == entt::null) return false;
    const auto& pi = world.Registry.get<PrefabInstanceComponent>(root);
    int idx = -1;
    for (std::size_t i = 0; i < pi.InstanceEntities.size(); ++i)
        if (pi.InstanceEntities[i] == entity) { idx = (int)i; break; }
    if (idx < 0) return false;
    if (root == entity && (std::strcmp(component, "Transform") == 0 || std::strcmp(component, "Name") == 0))
        return false; // root transform/name are per-instance, never applied

    const json value = LiveFieldValue(world, entity, component, field);
    if (value.is_null()) return false;

    // Read the .prefab fresh (not the cache — about to rewrite it), edit the entity object,
    // write it back pretty-printed.
    json prefab;
    { std::ifstream f(pi.SourcePath);
      if (!f.is_open()) return false;
      try { f >> prefab; } catch (...) { return false; } }
    json* pe = PrefabEntityObjectMutable(prefab, idx);
    if (!pe) return false;

    if (std::strcmp(component, "Transform") == 0)      (*pe)[field] = value;
    else if (std::strcmp(component, "Name") == 0)      (*pe)["name"] = value;
    else {
        // `component` is a display Name; translate to its JSON Key before writing (see
        // ReflectComponent::Key / PristineFieldValue's comment) — a GenericSerialize=false
        // component (Collider/Joint) can have a legacy JSON key that differs from its Name.
        const char* jsonKey = component;
        for (const auto& rc : ComponentRegistry::All())
            if (std::strcmp(rc.Meta.Name, component) == 0) { jsonKey = ReflectComponentKey(rc.Meta); break; }
        (*pe)[jsonKey][field] = value;
    }

    if (!AtomicFile::WriteJson(std::filesystem::path(pi.SourcePath), prefab)) return false; // #82

    g_prefabPristineCache.erase(pi.SourcePath); // Inspector re-reads -> override marker clears
    Log::Info("Applied '" + std::string(component) + "." + field + "' to prefab '" + pi.SourcePath +
              "'. Other instances update on their next load.");
    return true;
}

// #315 B4b — component add/remove overrides, editor helpers.

bool SceneSerializer::IsPrefabComponentAdded(const World& world, entt::entity entity,
                                             const char* component) {
    PristineHit hit = FindPristineEntity(world, entity);
    if (!hit.ent) return false;
    for (const auto& rc : ComponentRegistry::All())
        if (std::strcmp(rc.Meta.Name, component) == 0)
            // ReflectComponentKey, not `component` (a display Name) directly — a
            // GenericSerialize=false component (Collider/Joint) can have a legacy JSON key that
            // differs from its Name (see ReflectComponent::Key). Checking Name here would find no
            // "Collider" key in the pristine JSON (it's stored as "collider") and misreport every
            // prefab instance's real, unmodified Collider as user-added.
            return rc.Has(world.Registry, entity) && !hit.ent->contains(ReflectComponentKey(rc.Meta));
    return false;
}

void SceneSerializer::RevertPrefabComponent(World& world, AssetLibrary& /*assets*/, entt::entity entity,
                                            const char* component) {
    if (!IsPrefabComponentAdded(world, entity, component)) return; // only "added" is revertable here
    for (const auto& rc : ComponentRegistry::All())
        if (std::strcmp(rc.Meta.Name, component) == 0) { rc.Remove(world.Registry, entity); return; }
}

bool SceneSerializer::ApplyPrefabComponent(World& world, entt::entity entity, const char* component) {
    entt::entity root = entt::null;
    for (entt::entity cur = entity; cur != entt::null; ) {
        if (world.Registry.all_of<PrefabInstanceComponent>(cur)) { root = cur; break; }
        const auto* h = world.Registry.try_get<HierarchyComponent>(cur);
        cur = h ? h->Parent : entt::null;
    }
    if (root == entt::null) return false;
    const auto& pi = world.Registry.get<PrefabInstanceComponent>(root);
    int idx = -1;
    for (std::size_t i = 0; i < pi.InstanceEntities.size(); ++i)
        if (pi.InstanceEntities[i] == entity) { idx = (int)i; break; }
    if (idx < 0) return false;

    const RegisteredComponent* rcp = nullptr;
    for (const auto& rc : ComponentRegistry::All())
        if (std::strcmp(rc.Meta.Name, component) == 0) { rcp = &rc; break; }
    if (!rcp || !rcp->Has(world.Registry, entity)) return false;

    json prefab;
    { std::ifstream f(pi.SourcePath);
      if (!f.is_open()) return false;
      try { f >> prefab; } catch (...) { return false; } }
    json* pe = PrefabEntityObjectMutable(prefab, idx);
    if (!pe) return false;

    json cj;
    const void* comp = rcp->GetConst(world.Registry, entity);
    for (const auto& f : rcp->Meta.Fields)
        cj[ReflectFieldKey(f)] = ReflectFieldToJson(f, f.Address(const_cast<void*>(comp)));
    // ReflectComponentKey, not `component` (a display Name) directly — see IsPrefabComponentAdded's
    // comment above for why (Collider/Joint's legacy lowercase JSON key).
    (*pe)[ReflectComponentKey(rcp->Meta)] = std::move(cj);

    if (!AtomicFile::WriteJson(std::filesystem::path(pi.SourcePath), prefab)) return false; // #82

    g_prefabPristineCache.erase(pi.SourcePath);
    Log::Info("Applied component '" + std::string(component) + "' to prefab '" + pi.SourcePath +
              "'. Other instances update on their next load.");
    return true;
}
