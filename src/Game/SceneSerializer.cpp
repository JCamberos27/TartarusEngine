#include "SceneSerializer.h"
#include "World.h"
#include "AssetLibrary.h"
#include "ComponentRegistry.h"
#include "Model.h"
#include "Texture.h"
#include "Material.h"
#include "MaterialAsset.h"
#include "AssetDatabase.h"
#include "AssetGuid.h"

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
constexpr int kSceneFormatVersion = 2;

// Set by ApplySceneJson when the file being loaded declares a formatVersion newer than this build
// understands; read (and cleared) via SceneSerializer::TakeLoadWarning() so a caller like the
// editor can pop a dialog in addition to the Console line ApplySceneJson already logs. Plain
// (non-thread-local) static: scene loads happen on the main thread only.
std::string g_LastLoadWarning;

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

// PR 2 (#333): path string → {"path":"...", "pathGuid":"..."} for WRITE. Returns a plain
// string if the GUID isn't known yet so that v1-era paths still round-trip cleanly.
json PathRef(const std::string& path) {
    if (path.empty()) return path;
    AssetGuid g = AssetDatabase::GuidForPath(path);
    if (g.IsValid()) return json{{"path", path}, {"pathGuid", g.ToString()}};
    return path;
}

// PR 2 (#333): dual path+guid READ. Prefers GUID if present and resolvable, falls back to
// the plain path. Accepts both the v2 {"path","pathGuid"} object and the v1 plain string.
std::string ResolveAssetRef(const json& val) {
    if (val.is_string()) return val.get<std::string>();
    if (!val.is_object()) return {};
    std::string path = val.value("path", std::string());
    AssetGuid g = AssetGuid::FromString(val.value("pathGuid", std::string()));
    return AssetDatabase::Resolve(g, path);
}
// Overload for objects that carry the path and guid as sibling string keys.
std::string ResolveAssetRef(const json& obj, const char* pathKey, const char* guidKey) {
    std::string path = obj.value(pathKey, std::string());
    AssetGuid g = AssetGuid::FromString(obj.value(guidKey, std::string()));
    return AssetDatabase::Resolve(g, path);
}

std::shared_ptr<Texture> LoadIfPresent(AssetLibrary& assets, const json& obj, const char* key) {
    if (!obj.contains(key)) return nullptr;
    std::string path = ResolveAssetRef(obj[key]);
    if (path.empty()) return nullptr;
    return assets.LoadTexture(path);
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
            cj[f.Name] = ReflectFieldToJson(f, f.Address(comp));
        j[rc.Meta.Name] = cj;
    }
}

void ReadCommonComponents(const json& j, World& world, AssetLibrary& assets, entt::entity entity) {
    if (j.contains("tag")) world.Registry.emplace_or_replace<TagComponent>(entity, j["tag"].get<std::string>());
    if (!j.value("active", true)) world.Registry.emplace_or_replace<InactiveTag>(entity);
    if (j.value("static", false)) world.Registry.emplace_or_replace<StaticTag>(entity);
    if (const int layer = j.value("layer", 0); layer != 0)
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
            if (!cj.contains(f.Name)) continue;
            ReflectFieldFromJson(f, f.Address(comp), cj.at(f.Name), assets);
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
                emit(rc.Meta.Name, f.Name, ReflectFieldToJson(f, f.Address(const_cast<void*>(comp))));
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
            if (!pc.contains(f.Name)) continue;
            json now = ReflectFieldToJson(f, f.Address(const_cast<void*>(comp)));
            if (!ReflectJsonNearlyEqual(f.Type, now, pc.at(f.Name))) emit(rc.Meta.Name, f.Name, now);
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
            if (f.Name == field) { ReflectFieldFromJson(f, f.Address(c), v, assets); return; }
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
        if (!world.SkyHdriPath.empty())
            root["skyHdriPath"] = world.SkyHdriPath;
        if (world.SkyRotationDegrees != 0.0f)
            root["skyRotationDegrees"] = world.SkyRotationDegrees;
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
    auto boxView = world.Registry.view<const TransformComponent, const NameComponent,
        const RenderableComponent, const LevelGeometryTag>();
    auto modelViewForIds = world.Registry.view<const TransformComponent, const NameComponent,
        const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    // Entities with no mesh at all — lights and plain empties used as grouping pivots. They
    // live in their own array rather than "models" because every "models" entry needs a `path`
    // to reload geometry from, and these have none.
    auto emptyViewForIds = world.Registry.view<const TransformComponent, const NameComponent>(
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
        const auto& name = boxView.get<const NameComponent>(entity);
        const auto& renderable = boxView.get<const RenderableComponent>(entity);
        json b = {
            {"center", Vec3ToJson(transform.Position)},
            {"size", Vec3ToJson(transform.Scale)},
            {"color", Vec3ToJson(renderable.ModelRef->MeshMaterial(0).BaseColor)},
            {"rotation", Vec3ToJson(transform.RotationEuler)},
            {"name", name.Name},
            {"id", idOf[entity]},
            {"parentId", parentIdOf(entity)},
        };
        WriteCommonComponents(b, world, entity);
        boxes.push_back(b);
    }
    root["boxes"] = boxes;

    json empties = json::array();
    for (auto entity : emptyEntities) {
        if (skipWrite(entity) || isPrefabRoot(entity)) continue; // #236 A2
        TransformComponent transform = effectiveTransform(entity);
        const auto& name = emptyViewForIds.get<const NameComponent>(entity);
        json e = {
            {"name", name.Name},
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
    auto modelView = world.Registry.view<const TransformComponent, const NameComponent,
        const RenderableComponent>(entt::exclude<LevelGeometryTag>);
    for (auto entity : modelEntities) {
        if (skipWrite(entity) || isPrefabRoot(entity)) continue; // #236 A2
        TransformComponent transform = effectiveTransform(entity);
        const auto& name = modelView.get<const NameComponent>(entity);
        const auto& renderable = modelView.get<const RenderableComponent>(entity);

        json m;
        m["path"] = renderable.ModelRef->Path();
        {
            AssetGuid g = AssetDatabase::GuidForPath(renderable.ModelRef->Path());
            if (g.IsValid()) m["pathGuid"] = g.ToString();
        }
        m["name"] = name.Name;
        m["position"] = Vec3ToJson(transform.Position);
        m["rotation"] = Vec3ToJson(transform.RotationEuler);
        m["scale"] = Vec3ToJson(transform.Scale);
        // AudioSourceComponent moved onto reflection (#302 Wave 3): it now round-trips through
        // the generic "Audio Source" block written by WriteCommonComponents below (for every
        // entity kind, not just models). The old flat "sound*" keys are still READ.
        m["id"] = idOf[entity];
        m["parentId"] = parentIdOf(entity);

        const auto& matSlots = renderable.Materials;
        if (!matSlots.empty()) {
            json arr = json::array();
            for (const auto& slot : matSlots) {
                if (!slot) { arr.push_back(nullptr); continue; }
                if (!slot->Path.empty()) {
                    json entry;
                    AssetGuid g = AssetDatabase::GuidForPath(slot->Path);
                    if (g.IsValid()) entry["guid"] = g.ToString();
                    entry["path"] = slot->Path;
                    arr.push_back(entry);
                } else {
                    const auto& smat = slot->Mat;
                    arr.push_back({{"embedded", {
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
                    }}});
                }
            }
            m["materials"] = arr;
        }
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
            s["source"] = pi.SourcePath;
            {
                AssetGuid g = AssetDatabase::GuidForPath(pi.SourcePath);
                if (g.IsValid()) s["sourceGuid"] = g.ToString();
            }
            s["name"]     = world.Registry.get<NameComponent>(e).Name;
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

            // #302 Part B — per-field overrides: diff each live instance entity against its
            // pristine counterpart in the .prefab file, by prefab-local index.
            if (!pi.InstanceEntities.empty()) {
                std::ifstream pf(pi.SourcePath);
                if (pf.is_open()) {
                    json prefab; pf >> prefab;
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

// `clearFirst` false ADDS to the existing scene instead of replacing it — the difference
// between loading a scene and pasting/instantiating a fragment into one. `outCreated`, when
// given, collects every entity this call created so the caller can select or offset them.
bool ApplySceneJson(World& world, AssetLibrary& assets, const json& root,
    bool clearFirst = true, std::vector<entt::entity>* outCreated = nullptr) {
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
        world.SkySourceMode          = (World::SkySource)root.value("skySource",          0);
        world.SkyHdriPath            = root.value("skyHdriPath",            std::string());
        world.SkyRotationDegrees     = root.value("skyRotationDegrees",     0.0f);
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
    auto applyOrder = [&](entt::entity e, const json& j) {
        if (!clearFirst) return;
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

            auto ReadEmbeddedMat = [&](const json& mj) -> std::shared_ptr<MaterialAsset> {
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
                return ma;
            };

            if (m.contains("materials") && m["materials"].is_array()) {
                for (const auto& slot_j : m["materials"]) {
                    if (slot_j.is_null()) { rc.Materials.push_back(nullptr); continue; }
                    if (slot_j.contains("embedded")) {
                        rc.Materials.push_back(ReadEmbeddedMat(slot_j["embedded"]));
                    } else {
                        std::string path = ResolveAssetRef(slot_j);
                        rc.Materials.push_back(path.empty() ? nullptr : assets.LoadMaterial(path));
                    }
                }
            } else if (m.contains("material")) {
                // Legacy pre-PR5 format: single embedded Material → slot 0
                rc.Materials.assign(1, ReadEmbeddedMat(m["material"]));
            }
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
            if (const int layer = s.value("layer", 0); layer != 0)
                world.Registry.emplace_or_replace<LayerComponent>(rootE, LayerComponent{layer});
            if (s.contains("tag"))
                world.Registry.emplace_or_replace<TagComponent>(rootE, s["tag"].get<std::string>());

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
        for (auto& entry : meta) {
            if (entry["path"] == key) return entry;
        }
        json entry{{"path", key}};
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
        };
    }
    for (const auto& [key, s] : assets.ModelSettingsMap()) {
        findOrCreate(key)["modelImport"] = {
            {"globalScale", s.GlobalScale}, {"importNormals", s.ImportNormals},
            {"importAnimations", s.ImportAnimations}, {"importSkeleton", s.ImportSkeleton},
            {"optimizeGraph", s.OptimizeGraph}, {"materialImportMode", (int)s.MaterialImportMode},
        };
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
            std::string fallback = entry.value("path", std::string());
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
                s.FilterMode = (TextureImportSettings::Filter)t.value("filterMode", 1);
                s.WrapMode = (TextureImportSettings::Wrap)t.value("wrapMode", 0);
                s.MaxTextureSize = t.value("maxTextureSize", 2048);
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
                s.MaterialImportMode = (ModelImportSettings::MaterialMode)m.value("materialImportMode", 0);
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
json PristineFieldValue(const PristineHit& hit, const char* component, const char* field) {
    if (!hit.ent) return nullptr;
    if (std::strcmp(component, "Transform") == 0)
        return hit.ent->contains(field) ? hit.ent->at(field) : json(nullptr);
    if (std::strcmp(component, "Name") == 0)
        return json(hit.ent->value("name", std::string()));
    if (!hit.ent->contains(component)) return nullptr;
    const json& cj = hit.ent->at(component);
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
            if (std::strcmp(f.Name, field) == 0)
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

} // namespace

bool SceneSerializer::Save(const World& world, const AssetLibrary& assets, const std::string& path) {
    std::ofstream out(path);
    if (!out.is_open()) {
        Log::Error("Scene: failed to open '" + path + "' for writing.");
        return false;
    }
    json root = BuildSceneJson(world);
    AppendAssetLibraryJson(root, assets);
    out << root.dump(2);
    return true;
}

std::string SceneSerializer::TakeLoadWarning() {
    std::string warning = std::move(g_LastLoadWarning);
    g_LastLoadWarning.clear();
    return warning;
}

bool SceneSerializer::Load(World& world, AssetLibrary& assets, const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        Log::Error("Scene: failed to parse '" + path + "': " + e.what());
        return false;
    }
    ApplyAssetLibraryJson(assets, root); // before ApplySceneJson: harmless either order, but library assets should exist first
    return ApplySceneJson(world, assets, root);
}

std::string SceneSerializer::SaveToString(const World& world) {
    return BuildSceneJson(world).dump();
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
        std::set<std::string> keepModels, keepTextures, keepSounds, keepPrefabs, keepFolders, keepMaterials;
        if (root.contains("libraryModels")) for (const auto& p : root["libraryModels"]) keepModels.insert(p.get<std::string>());
        if (root.contains("libraryTextures")) for (const auto& p : root["libraryTextures"]) keepTextures.insert(p.get<std::string>());
        if (root.contains("librarySounds")) for (const auto& p : root["librarySounds"]) keepSounds.insert(p.get<std::string>());
        if (root.contains("libraryPrefabs")) for (const auto& p : root["libraryPrefabs"]) keepPrefabs.insert(p.get<std::string>());
        if (root.contains("assetFolders")) for (const auto& f : root["assetFolders"]) keepFolders.insert(f.get<std::string>());
        if (root.contains("libraryMaterials")) for (const auto& p : root["libraryMaterials"]) keepMaterials.insert(p.get<std::string>());
        assets.PruneToKeepSet(keepModels, keepTextures, keepSounds, keepPrefabs, keepFolders, keepMaterials);
        assets.ClearMetadataOnly();
        ApplyAssetLibraryJson(assets, root);
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
    const std::string& data, std::vector<entt::entity>& outCreated) {
    json root;
    try {
        root = json::parse(data);
    } catch (const std::exception& e) {
        Log::Error(std::string("Scene: failed to parse entity fragment: ") + e.what());
        return false;
    }
    return ApplySceneJson(world, assets, root, /*clearFirst=*/false, &outCreated);
}

bool SceneSerializer::SavePrefab(const World& world, entt::entity root, const std::string& path) {
    if (!world.Registry.valid(root)) return false;
    std::ofstream out(path);
    if (!out.is_open()) {
        Log::Error("Prefab: failed to open '" + path + "' for writing.");
        return false;
    }
    // Re-parsed and re-dumped with indentation so a prefab file is human-readable/diffable,
    // unlike the compact in-memory clipboard form the same function produces. flatten=true: a
    // .prefab asset is self-contained — if `root` (or a child) is itself a prefab instance it is
    // baked in fully here, not left as a nested link (#236 A2 — nested prefabs are stage 4).
    out << json::parse(SaveEntitiesToString(world, {root}, /*flattenPrefabInstances=*/true)).dump(2);
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
            if (std::strcmp(f.Name, field) != 0) continue;
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
    else                                              (*pe)[component][field] = value;

    std::ofstream out(pi.SourcePath);
    if (!out.is_open()) return false;
    out << prefab.dump(2);
    out.close();

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
            return rc.Has(world.Registry, entity) && !hit.ent->contains(component);
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
        cj[f.Name] = ReflectFieldToJson(f, f.Address(const_cast<void*>(comp)));
    (*pe)[component] = std::move(cj);

    std::ofstream out(pi.SourcePath);
    if (!out.is_open()) return false;
    out << prefab.dump(2);
    out.close();

    g_prefabPristineCache.erase(pi.SourcePath);
    Log::Info("Applied component '" + std::string(component) + "' to prefab '" + pi.SourcePath +
              "'. Other instances update on their next load.");
    return true;
}
