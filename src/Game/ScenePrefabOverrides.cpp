#include "SceneSerializerInternal.h"

namespace SceneDetail {


// --- Prefab per-field overrides (#236 A2 stage 3 / #302 Part B) --------------------------
// The .prefab file is the pristine source of truth. Diffing at save time can't instantiate a
// copy (BuildSceneJson holds a `const World&`), so instead each live instance entity is
// compared field-by-field against the matching entity object in the .prefab JSON. Pairing is by
// index: PrefabInstanceComponent::InstanceEntities is in the same order the file lists entities
// (boxes, then models, then empties, each in file order — see InstantiatePrefab / ApplySceneJson).

// #122 - boxes used to write their transform as "center" / "size" while models and empties use
// "position" / "scale". Boxes now write the shared keys; this upgrades an older file's boxes in
// place so everything that compares or edits entity objects by key (prefab override diffing,
// Apply / Revert to prefab) sees one schema.
void UpgradeLegacyBoxKeys(json& root) {
    auto it = root.find("boxes");
    if (it == root.end() || !it->is_array()) return;
    for (json& b : *it) {
        if (!b.is_object()) continue;
        for (const auto& [from, to] : {std::pair<const char*, const char*>{"center", "position"}, {"size", "scale"}}) {
            if (!b.contains(from)) continue;
            if (!b.contains(to)) b[to] = b[from];
            b.erase(from);
        }
    }
}

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
        // Mesh Renderer has no JSON block of its own: whether an entity has one is structural (it
        // sits in the prefab's "boxes" / "models" array), so there is nothing to diff.
        if (!rc.Meta.GenericSerialize && !rc.Meta.GenericInspector) continue;
        // The component's JSON key, not its display Name: a hand-written block predating reflection
        // lives under a different key ("collider" / "joint", see ReflectComponent::Key). Looking up
        // the Name found nothing, so a live Collider read as "added" with every field overridden,
        // and one the instance had removed was never recorded.
        const char* jsonKey = ReflectComponentKey(rc.Meta);
        const bool onLive = rc.Has(world.Registry, live);
        const bool onPrefab = pristine.contains(jsonKey);
        if (!onLive && !onPrefab) continue;

        if (onLive && !onPrefab) {
            // Instance gained this component — record the add, then every field as an override
            // (there's no pristine value to compare against, so all of them count).
            outOverrides.push_back({{"e", localIndex}, {"op", "addComponent"}, {"c", rc.Meta.Name}});
            const void* comp = rc.GetConst(world.Registry, live);
            for (const auto& f : rc.Meta.Fields)
                if (!f.NonSerialized)
                    emit(rc.Meta.Name, ReflectFieldKey(f), ReflectFieldToJson(f, f.Address(const_cast<void*>(comp))));
            continue;
        }
        if (!onLive && onPrefab) {
            outOverrides.push_back({{"e", localIndex}, {"op", "removeComponent"}, {"c", rc.Meta.Name}});
            continue;
        }

        // On both — diff each field.
        const json& pc = pristine.at(jsonKey);
        const void* comp = rc.GetConst(world.Registry, live);
        if (!rc.Meta.GenericSerialize) {
            // A hand-written block uses its own key names, so the reflected keys below would find
            // nothing in it. Decode it into a scratch component the way a load would and compare the
            // reflected fields' values instead.
            ColliderComponent pristineCollider;
            JointComponent pristineJoint;
            const void* was = nullptr;
            if (std::strcmp(jsonKey, "collider") == 0) { pristineCollider = ColliderFromJson(pc); was = &pristineCollider; }
            else if (std::strcmp(jsonKey, "joint") == 0) { pristineJoint = JointFromJson(pc); was = &pristineJoint; }
            if (!was) continue;
            for (const auto& f : rc.Meta.Fields) {
                // A joint's partner is an OrderComponent value, repointed at the partner's copy when
                // the prefab is instantiated (#119), so it can never equal the file's value.
                if (std::strcmp(f.Name, "ConnectedOrder") == 0) continue;
                json now = ReflectFieldToJson(f, f.Address(const_cast<void*>(comp)));
                json then = ReflectFieldToJson(f, f.Address(const_cast<void*>(was)));
                if (!ReflectJsonNearlyEqual(f.Type, now, then)) emit(rc.Meta.Name, ReflectFieldKey(f), std::move(now));
            }
            continue;
        }
        for (const auto& f : rc.Meta.Fields) {
            if (f.NonSerialized) continue;
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
        {"position", t.Position}, {"scale", t.Scale}};
    for (const auto& e : tf) {
        if (!pristine.contains(e.f)) continue;
        json now = Vec3ToJson(e.v);
        if (!ReflectJsonNearlyEqual(ReflectFieldType::Vec3, now, pristine.at(e.f)))
            emit("Transform", e.f, now);
    }
    if (pristine.contains("rotation")) {
        json now = QuatToJson(t.Rotation);
        if (!RotationJsonNearlyEqual(now, pristine.at("rotation"))) emit("Transform", "rotation", std::move(now));
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
        else if (field == "rotation") SetTransformRotationFromJson(t, v);
        else if (field == "scale")    t.Scale = JsonToVec3(v);
        return;
    }
    if (comp == "Name") {
        world.Registry.emplace_or_replace<NameComponent>(e, NameComponent{v.get<std::string>()});
        return;
    }
    for (const auto& rc : ComponentRegistry::All()) {
        if (!ReflectComponentMatches(rc.Meta, comp.c_str())) continue;
        if (!rc.Has(world.Registry, e)) rc.Add(world.Registry, e);
        void* c = rc.Get(world.Registry, e);
        for (const auto& f : rc.Meta.Fields)
            if (ReflectFieldMatchesKey(f, field.c_str())) { ReflectFieldFromJson(f, f.Address(c), v, &assets); return; }
        return;
    }
}

const json* CachedPrefabJson(const std::string& path) {
    auto it = g_prefabPristineCache.find(path);
    if (it != g_prefabPristineCache.end()) return &it->second;
    std::ifstream f(std::filesystem::u8path(path));
    if (!f.is_open()) return nullptr;
    json parsed;
    try { f >> parsed; } catch (...) { return nullptr; }
    UpgradeLegacyBoxKeys(parsed); // #122
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
        if (std::strcmp(field, "rotation") == 0) return QuatToJson(t->Rotation);
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
    UpgradeLegacyBoxKeys(prefab); // #122 - the file is rewritten in the shared schema
    int seen = 0;
    for (const char* key : {"boxes", "models", "empties"})
        if (auto it = prefab.find(key); it != prefab.end() && it->is_array())
            for (json& e : *it) { if (seen++ == idx) return &e; }
    return nullptr;
}

} // namespace SceneDetail

using namespace SceneDetail;


// --- Prefab per-field overrides, editor helpers (#302 Part B) -------------------------------

void SceneSerializer::ClearPrefabPristineCache() { g_prefabPristineCache.clear(); }

bool SceneSerializer::PrefabChangedSinceInstanced(const std::string& path) {
    for (const auto& [key, cached] : g_prefabPristineCache) {
        if (!AssetDatabase::SamePath(key, path)) continue;
        std::ifstream f(std::filesystem::u8path(path));
        if (!f.is_open()) return false;
        json now;
        try { f >> now; } catch (...) { return false; }
        UpgradeLegacyBoxKeys(now); // #122: cached the same way
        return now != cached;
    }
    return false;
}

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
        if (std::strcmp(field, "rotation") == 0)
            return !RotationJsonNearlyEqual(QuatToJson(t.Rotation), pristine);
        json now = std::strcmp(field, "position") == 0 ? Vec3ToJson(t.Position)
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
    { std::ifstream f(std::filesystem::u8path(pi.SourcePath));
      if (!f.is_open()) return false;
      try { f >> prefab; } catch (...) { return false; } }
    json* pe = PrefabEntityObjectMutable(prefab, idx);
    if (!pe) return false;

    if (std::strcmp(component, "Transform") == 0) {
        (*pe)[field] = value;
        if (std::strcmp(field, "rotation") == 0) prefab["formatVersion"] = kSceneFormatVersion;
    }
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

    if (!AtomicFile::WriteJson(std::filesystem::u8path(pi.SourcePath), prefab)) return false; // #82

    // The cache keeps the content the instances were built from until the editor rebuilds them
    // from the new file (EditorLayer::RefreshPrefabInstances): dropping it here would make the
    // next save diff the other instances against the new file and store the field's old value
    // as an override of their own.
    Log::Info("Applied '" + std::string(component) + "." + field + "' to prefab '" + pi.SourcePath + "'.");
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
    { std::ifstream f(std::filesystem::u8path(pi.SourcePath));
      if (!f.is_open()) return false;
      try { f >> prefab; } catch (...) { return false; } }
    json* pe = PrefabEntityObjectMutable(prefab, idx);
    if (!pe) return false;

    json cj;
    const void* comp = rcp->GetConst(world.Registry, entity);
    for (const auto& f : rcp->Meta.Fields)
        if (!f.NonSerialized) cj[ReflectFieldKey(f)] = ReflectFieldToJson(f, f.Address(const_cast<void*>(comp)));
    // ReflectComponentKey, not `component` (a display Name) directly — see IsPrefabComponentAdded's
    // comment above for why (Collider/Joint's legacy lowercase JSON key).
    (*pe)[ReflectComponentKey(rcp->Meta)] = std::move(cj);

    if (!AtomicFile::WriteJson(std::filesystem::u8path(pi.SourcePath), prefab)) return false; // #82

    // The cache keeps the content the instances were built from until the editor rebuilds them
    // from the new file (EditorLayer::RefreshPrefabInstances): dropping it here would make the
    // next save diff the other instances against the new file and store their missing
    // component as a "removeComponent" override of their own.
    Log::Info("Applied component '" + std::string(component) + "' to prefab '" + pi.SourcePath + "'.");
    return true;
}
