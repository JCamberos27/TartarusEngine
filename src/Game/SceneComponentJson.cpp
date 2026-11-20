#include "SceneSerializerInternal.h"

namespace SceneDetail {


// --- One reflected field <-> JSON, in one place -------------------------------------------
// The generic component write/read (WriteCommonComponents / ReadCommonComponents), and the
// prefab per-field override diff/replay (#302 Part B), all encode a reflected field the same
// way. `fp` is the field address from ReflectField::Address.
// #132 - ReflectField::AssetPath strings. The file part is what gets a GUID; a "#name" suffix
// (an animation clip inside a model file) rides along unchanged.
json AssetPathStringToJson(const std::string& value) {
    const std::string g = AssetDatabase::RefGuid(value);
    if (g.empty()) return value;
    return json{{"path", value}, {"pathGuid", g}};
}

std::string AssetPathStringFromJson(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (!v.is_object()) return {};
    const auto pit = v.find("path");
    const auto git = v.find("pathGuid");
    return AssetDatabase::FollowRef((pit != v.end() && pit->is_string()) ? pit->get<std::string>() : std::string(),
                                    (git != v.end() && git->is_string()) ? git->get<std::string>() : std::string());
}

json ReflectFieldToJson(const ReflectField& f, const void* fp) {
    switch (f.Type) {
        case ReflectFieldType::Bool:   return *static_cast<const bool*>(fp);
        case ReflectFieldType::Int:    return *static_cast<const int*>(fp);
        case ReflectFieldType::Float: {
            const float v = *static_cast<const float*>(fp);
            return std::isfinite(v) ? v : 0.0f; // NaN / inf would be written as null and fail the whole load
        }
        case ReflectFieldType::Vec3:
        case ReflectFieldType::Color:  return Vec3ToJson(*static_cast<const glm::vec3*>(fp));
        case ReflectFieldType::String:
            if (f.AssetPath) return AssetPathStringToJson(*static_cast<const std::string*>(fp)); // #132
            return *static_cast<const std::string*>(fp);
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

void ReflectFieldFromJson(const ReflectField& f, void* fp, const json& v, AssetLibrary* assets) {
    switch (f.Type) {
        case ReflectFieldType::Bool:   *static_cast<bool*>(fp)  = v.get<bool>(); break;
        case ReflectFieldType::Int:    *static_cast<int*>(fp)   = v.get<int>(); break;
        case ReflectFieldType::Float:  if (!v.is_null()) *static_cast<float*>(fp) = v.get<float>(); break; // (null: a NaN saved by an older build - keep the default)
        case ReflectFieldType::Vec3:
        case ReflectFieldType::Color:  *static_cast<glm::vec3*>(fp) = JsonToVec3(v); break;
        case ReflectFieldType::String:
            *static_cast<std::string*>(fp) = f.AssetPath ? AssetPathStringFromJson(v) : v.get<std::string>(); // #132
            break;
        case ReflectFieldType::AssetRef: {
            auto& path = *static_cast<std::string*>(fp);
            path = ResolveAssetRef(v);  // handles both v1 plain string and v2 {path,pathGuid}
            // Make the referenced asset list in the library even if nothing else imported it,
            // so the Inspector's picker can still show / re-select it.
            if (assets && !path.empty() && f.AssetKind == ReflectAssetKind::Sound) assets->RegisterSound(path);
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
        case ReflectFieldType::String:
        case ReflectFieldType::AssetRef: {
            // #132 - a path may be stored as a plain string (older files) or {"path","pathGuid"}:
            // the same path either way is the same value, not a prefab override.
            auto pathOf = [](const json& v) -> json {
                if (v.is_object()) { const auto it = v.find("path"); return it != v.end() ? *it : json(); }
                return v;
            };
            return pathOf(a) == pathOf(b);
        }
        default:
            return a == b;
    }
}

// Editor Enhancers / vHierarchy row style ("hierStyle"). Colour as "#RRGGBB[AA]" so a scene
// diff reads; every key optional, and the whole object omitted for an unstyled row.
json HierStyleToJson(const HierarchyStyleComponent& st) {
    json h = json::object();
    if (!st.Icon.empty()) h["icon"] = st.Icon;
    if (st.Color != 0) {
        const unsigned r = st.Color & 255, g = (st.Color >> 8) & 255, b = (st.Color >> 16) & 255, a = (st.Color >> 24) & 255;
        char buf[10];
        if (a == 255) std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
        else          std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", r, g, b, a);
        h["color"] = buf;
    }
    if (st.FillMode != HierarchyStyleComponent::Gradient) h["fill"] = (int)st.FillMode;
    if (st.Separator) h["separator"] = true;
    return h;
}

void ReadHierStyle(const json& j, World& world, entt::entity entity) {
    const auto it = j.find("hierStyle");
    if (it == j.end() || !it->is_object()) return;
    HierarchyStyleComponent st;
    if (auto ic = it->find("icon"); ic != it->end() && ic->is_string()) st.Icon = ic->get<std::string>();
    if (auto c = it->find("color"); c != it->end() && c->is_string()) {
        std::string hex = c->get<std::string>();
        if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
        unsigned v[4] = {0, 0, 0, 255};
        bool ok = hex.size() == 6 || hex.size() == 8;
        for (size_t k = 0; ok && k < hex.size() / 2; ++k) {
            char* end = nullptr;
            const std::string byte = hex.substr(k * 2, 2);
            v[k] = (unsigned)std::strtoul(byte.c_str(), &end, 16);
            ok = end && *end == '\0';
        }
        if (ok) st.Color = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
    }
    if (auto f = it->find("fill"); f != it->end() && f->is_number_integer())
        st.FillMode = (std::uint8_t)std::clamp(f->get<int>(), 0, 2);
    if (auto sp = it->find("separator"); sp != it->end() && sp->is_boolean()) st.Separator = sp->get<bool>();
    if (!st.IsEmpty()) world.Registry.emplace_or_replace<HierarchyStyleComponent>(entity, std::move(st));
}

// Components that any entity kind can carry, written/read as one shared block so a box, a
// placed model, and a bare (Renderable-less) entity all round-trip them identically. Each is
// omitted entirely when absent, keeping saves of simple scenes as small as they were before
// these components existed — and keeping every field optional on load, so an older file that
// predates any of them still parses.
void WriteCommonComponents(json& j, const World& world, entt::entity entity) {
    if (const auto* order = world.Registry.try_get<OrderComponent>(entity)) j["order"] = order->Value;
    if (const auto* tag = world.Registry.try_get<TagComponent>(entity)) j["tag"] = tag->Tag;
    if (world.Registry.all_of<DeactivatedTag>(entity)) j["active"] = false;
    if (world.Registry.all_of<StaticTag>(entity)) j["static"] = true;
    // Layer slot (#236 A1). Omitted for the default (0) so scenes that never touch layers are
    // byte-identical to before; slot names live in project/layers.json, not the scene.
    if (const auto* layer = world.Registry.try_get<LayerComponent>(entity); layer && layer->Layer != 0)
        j["layer"] = layer->Layer;
    if (world.Registry.all_of<HiddenInSceneTag>(entity)) j["sceneHidden"] = true; // #236 B — editor SceneVis
    if (world.Registry.all_of<SceneLockedTag>(entity)) j["sceneLocked"] = true;
    if (const auto* hs = world.Registry.try_get<HierarchyStyleComponent>(entity); hs && !hs->IsEmpty())
        j["hierStyle"] = HierStyleToJson(*hs); // Editor Enhancers / vHierarchy

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
        if (!collider->Material.empty())    cj["material"]        = collider->Material; // #170
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
            if (!f.NonSerialized) cj[ReflectFieldKey(f)] = ReflectFieldToJson(f, f.Address(comp));
        j[ReflectComponentKey(rc.Meta)] = cj; // Name unless Key is set: a renamed label keeps its files
    }
}

// The hand-written "collider" / "joint" blocks <-> components. Shared by the scene loader and the
// prefab override diff, which has to decode a prefab's pristine block to compare it field by field.
ColliderComponent ColliderFromJson(const json& c) {
    ColliderComponent collider;
    collider.IsTrigger = c.value("isTrigger", false);
    int shape = c.value("shape", 0); // absent => Box (0), the pre-#185 shape
    collider.Kind = (shape >= 0 && shape <= 4) ? (ColliderComponent::Shape)shape
                                               : ColliderComponent::Shape::Box;
    if (collider.Kind != (ColliderComponent::Shape)shape)
        Log::Warn("Scene: collider shape " + std::to_string(shape) + " is not a known shape; loaded as a Box.");
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
    if (c.contains("material") && c["material"].is_string()) collider.Material = c["material"].get<std::string>();
    return collider;
}

JointComponent JointFromJson(const json& jc) {
    JointComponent joint;
    int ty = jc.value("type", 0);
    joint.Kind = (ty >= 0 && ty <= 4) ? (JointComponent::Type)ty : JointComponent::Type::Fixed;
    if (joint.Kind != (JointComponent::Type)ty)
        Log::Warn("Scene: joint type " + std::to_string(ty) + " is not a known type; loaded as Fixed.");
    joint.ConnectedOrder = jc.value("connectedOrder", -1);
    if (jc.contains("anchor")) joint.Anchor = JsonToVec3(jc["anchor"], glm::vec3(0.0f));
    if (jc.contains("axis"))   joint.Axis   = JsonToVec3(jc["axis"], glm::vec3(1.0f, 0.0f, 0.0f));
    joint.BreakForce  = jc.value("breakForce", 0.0f);
    joint.BreakTorque = jc.value("breakTorque", 0.0f);
    joint.UseLimit    = jc.value("useLimit", false);
    joint.LimitLower  = jc.value("limitLower", -45.0f);
    joint.LimitUpper  = jc.value("limitUpper", 45.0f);
    return joint;
}

// A component block (an object under a capitalised key, the way the generic pass writes them) that no registered
// component reads: a component that was renamed without a LegacyKeys entry, or removed. Its data is dropped on the
// next save, so say so, once per key per run. (The hand-written blocks are lowercase: "collider", "joint", ...)
void WarnUnknownComponentBlocks(const json& j) {
    static std::mutex warnedMutex;
    static std::set<std::string> warned;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        if (!it->is_object() || key.empty() || !std::isupper((unsigned char)key[0])) continue;
        bool known = false;
        for (const auto& rc : ComponentRegistry::All())
            if (ReflectComponentMatches(rc.Meta, key.c_str())) { known = true; break; }
        if (known) continue;
        std::lock_guard<std::mutex> lock(warnedMutex);
        if (warned.insert(key).second)
            Log::Warn("Scene: no component reads the \"" + key + "\" block; its data will be dropped on save.");
    }
}

void ReadCommonComponents(const json& j, World& world, AssetLibrary& assets, entt::entity entity) {
    if(j.contains("C# Script")) {
        auto migrated=j;
        if(Gameplay::ConvertScriptDefinitions(migrated)) {
            ReadCommonComponents(migrated,world,assets,entity);
            return;
        }
    }
    if (j.contains("tag")) world.Registry.emplace_or_replace<TagComponent>(entity, j["tag"].get<std::string>());
    if (!j.value("active", true)) world.Registry.emplace_or_replace<DeactivatedTag>(entity);
    if (j.value("static", false)) world.Registry.emplace_or_replace<StaticTag>(entity);
    if (const int layer = j.value("layer", 0); layer > 0 && layer < LayerRegistry::kCount) // #150: range-checked
        world.Registry.emplace_or_replace<LayerComponent>(entity, LayerComponent{layer});
    if (j.value("sceneHidden", false)) world.Registry.emplace_or_replace<HiddenInSceneTag>(entity); // #236 B
    if (j.value("sceneLocked", false)) world.Registry.emplace_or_replace<SceneLockedTag>(entity);
    ReadHierStyle(j, world, entity); // Editor Enhancers / vHierarchy

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
    if (j.contains("collider"))
        world.Registry.emplace_or_replace<ColliderComponent>(entity, ColliderFromJson(j["collider"]));
    if (j.contains("joint")) // #185 PR 11
        world.Registry.emplace_or_replace<JointComponent>(entity, JointFromJson(j["joint"]));
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
        const json* found = nullptr;
        if (auto it = j.find(ReflectComponentKey(rc.Meta)); it != j.end()) found = &*it;
        for (size_t i = 0; !found && i < rc.Meta.LegacyKeys.size(); ++i)
            if (const char* legacy = rc.Meta.LegacyKeys[i]; legacy)
                if (auto it = j.find(legacy); it != j.end()) found = &*it;
        if (!found) continue;
        rc.Add(world.Registry, entity);
        void* comp = rc.Get(world.Registry, entity);
        const json& cj = *found;
        for (const auto& f : rc.Meta.Fields) {
            if (f.NonSerialized) continue; // vInspector display-only field
            // #43: key off ReflectFieldKey, with a LegacyNames fallback — so a scene saved under a
            // field's old key (before an Inspector-label rename) still loads instead of silently
            // keeping the component's default for that field.
            const char* key = ReflectFieldKey(f);
            if (cj.contains(key)) { ReflectFieldFromJson(f, f.Address(comp), cj.at(key), &assets); continue; }
            for (int i = 0; i < f.LegacyNameCount; ++i) {
                if (f.LegacyNames[i] && cj.contains(f.LegacyNames[i])) {
                    ReflectFieldFromJson(f, f.Address(comp), cj.at(f.LegacyNames[i]), &assets);
                    break;
                }
            }
        }
    }
    WarnUnknownComponentBlocks(j);
}

} // namespace SceneDetail

using namespace SceneDetail;


bool ReadComponentJson(const char* component, const json& block, void* instance) {
    if (!block.is_object() || !instance) return false;
    for (const auto& rc : ComponentRegistry::All()) {
        if (!ReflectComponentMatches(rc.Meta, component)) continue;
        for (const auto& f : rc.Meta.Fields) {
            if (f.NonSerialized) continue;
            const char* key = ReflectFieldKey(f);
            if (block.contains(key)) { ReflectFieldFromJson(f, f.Address(instance), block.at(key), nullptr); continue; }
            for (int i = 0; i < f.LegacyNameCount; ++i)
                if (f.LegacyNames[i] && block.contains(f.LegacyNames[i])) {
                    ReflectFieldFromJson(f, f.Address(instance), block.at(f.LegacyNames[i]), nullptr);
                    break;
                }
        }
        return true;
    }
    return false;
}

// --- #178 Preset assets ---------------------------------------------------------------------
// A .preset file is {"preset":1,"component":"<name>","fields":{...}}, where `fields` is exactly
// the block a scene writes for that component - same keys, same encoding - so asset references
// inside a preset follow their GUID the way scene references do.
namespace {
const RegisteredComponent* FindSerialisableComponent(const char* name) {
    if (!name || !*name) return nullptr;
    for (const auto& rc : ComponentRegistry::All())
        if (rc.Meta.GenericSerialize && ReflectComponentMatches(rc.Meta, name)) return &rc;
    return nullptr; // unknown, or hand-coded (Mesh Renderer and friends)
}
} // namespace

std::string SceneSerializer::ComponentToPresetJson(const World& world, entt::entity entity,
                                                   const char* component) {
    const RegisteredComponent* rc = FindSerialisableComponent(component);
    if (!rc || !world.Registry.valid(entity) || !rc->Has(world.Registry, entity)) return {};
    // const_cast is safe: the component is a live mutable object; this path only reads it.
    void* comp = const_cast<void*>(rc->GetConst(world.Registry, entity));
    json fields;
    for (const auto& f : rc->Meta.Fields)
        if (!f.NonSerialized) fields[ReflectFieldKey(f)] = ReflectFieldToJson(f, f.Address(comp));
    return json{{"preset", 1}, {"component", rc->Meta.Name}, {"fields", std::move(fields)}}.dump(2);
}

std::string SceneSerializer::PresetComponentName(const std::string& presetJson) {
    json j = json::parse(presetJson, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return {};
    const auto it = j.find("component");
    if (it == j.end() || !it->is_string()) return {};
    const std::string name = it->get<std::string>();
    return FindSerialisableComponent(name.c_str()) ? name : std::string();
}

bool SceneSerializer::ApplyComponentPresetJson(World& world, AssetLibrary& assets, entt::entity entity,
                                               const std::string& presetJson) {
    json j = json::parse(presetJson, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !world.Registry.valid(entity)) return false;
    const auto cit = j.find("component");
    if (cit == j.end() || !cit->is_string()) return false;
    const RegisteredComponent* rc = FindSerialisableComponent(cit->get<std::string>().c_str());
    if (!rc) return false;
    const auto fit = j.find("fields");
    if (fit == j.end() || !fit->is_object()) return false;

    if (!rc->Has(world.Registry, entity)) rc->Add(world.Registry, entity);
    void* comp = rc->Get(world.Registry, entity);
    // A field the preset does not mention keeps its current value rather than snapping to the
    // component default, so a preset saved before a field existed stays usable. Same legacy-key
    // fallback the scene loader uses (#43).
    for (const auto& f : rc->Meta.Fields) {
        if (f.NonSerialized) continue;
        const char* key = ReflectFieldKey(f);
        if (fit->contains(key)) { ReflectFieldFromJson(f, f.Address(comp), fit->at(key), &assets); continue; }
        for (int i = 0; i < f.LegacyNameCount; ++i) {
            if (f.LegacyNames[i] && fit->contains(f.LegacyNames[i])) {
                ReflectFieldFromJson(f, f.Address(comp), fit->at(f.LegacyNames[i]), &assets);
                break;
            }
        }
    }
    return true;
}
