#include "WeaponPrefab.h"
#include "WeaponAttachments.h"
#include "ProjectComponentMigration.h"
#include "ComponentJson.h"
#include "ProjectPaths.h"
#include "LiveReload.h"
#include "AssetDatabase.h"
#include "SceneReferences.h"
#include "Components.h"
#include "FirstPersonAnimation.h"
#include "SceneSerializer.h"
#include "World.h"
#include "Log.h"
#include <json.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace Gameplay {
namespace {

constexpr const char* kAttachmentComponents[] = {"Muzzle Attachment", "Grip Attachment", "Optic Attachment"};
bool IsAttachment(const nlohmann::json& node) {
    for (const char* key : kAttachmentComponents) if (node.contains(key)) return true;
    return false;
}

// Every soldier's gun reads its prefab when it spawns (its animation set, muzzle effects, gameplay stats and particles):
// parsed and resolved once per file version and asset database revision - it was ~12 ms of file reading and JSON
// parsing per respawn, mid-fight. Only results that succeeded are kept.
struct CachedPrefab {
    std::filesystem::file_time_type Time{};
    std::uint64_t Assets = ~0ull;
    std::shared_ptr<const nlohmann::json> Json;
    bool HaveResolve = false;
    std::string Set;
    MuzzleEffectSettings Muzzle;
    bool HaveGameplay = false;
    FirstPersonWeaponGameplay Gameplay;
    std::string Fragment; // the particle fragment (InstantiateWeaponParticles with meshes), "" until made
};
CachedPrefab& PrefabEntry(const std::string& prefab) {
    static std::unordered_map<std::string, CachedPrefab> cache;
    const std::string path = ProjectPaths::Resolve(prefab);
    const auto found = cache.find(path);
    // A shipped game's prefabs don't change under it: skip the per-lookup stat once cached.
    if (found != cache.end() && !LiveReload::Enabled() && found->second.Assets == AssetDatabase::Revision()) return found->second;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(std::filesystem::u8path(path), ec);
    CachedPrefab& c = cache[path];
    if (c.Time != time || c.Assets != AssetDatabase::Revision()) {
        c = CachedPrefab{};
        c.Time = time;
        c.Assets = AssetDatabase::Revision();
    }
    return c;
}
// The prefab's JSON, parsed once per version (null when it can't be opened; a parse error throws, as before).
// A prefab saved while weapons were C# scripts reads as the native components it holds now.
std::shared_ptr<const nlohmann::json> PrefabJson(const std::string& prefab) {
    CachedPrefab& c = PrefabEntry(prefab);
    if (!c.Json) {
        std::ifstream file(ProjectPaths::Resolve(prefab));
        if (!file) return nullptr;
        auto json = nlohmann::json::parse(file);
        for (const char* collection : {"empties", "models", "boxes"})
            if (json.contains(collection)) for (auto& node : json.at(collection)) ConvertScriptDefinitions(node);
        c.Json = std::make_shared<const nlohmann::json>(std::move(json));
    }
    return c.Json;
}
// The prefab's Weapon Definition block (null when it has none).
const nlohmann::json* WeaponBlock(const nlohmann::json& prefab) {
    for (const char* collection : {"empties", "models", "boxes"})
        if (prefab.contains(collection))
            for (const auto& node : prefab.at(collection))
                if (const auto it = node.find("Weapon Definition"); it != node.end()) return &*it;
    return nullptr;
}

nlohmann::json ParticleFragment(const nlohmann::json& prefab) {
    using Json = nlohmann::json;
    std::unordered_map<int, Json> nodes;
    int weapon = -1;
    for (const char* collection : {"empties", "models", "boxes"}) {
        if (!prefab.contains(collection)) continue;
        for (const auto& node : prefab.at(collection)) {
            const int id = node.value("id", -1);
            if (id < 0) continue;
            auto transformNode = node;
            if (std::string(collection) == "boxes") {
                transformNode["position"] = node.value("center", nlohmann::json::array({0,0,0}));
                transformNode["scale"] = node.value("size", nlohmann::json::array({1,1,1}));
            }
            nodes.emplace(id, std::move(transformNode));
            if (weapon < 0 && node.contains("Weapon Definition")) weapon = id;
        }
    }
    Json fragment = {{"formatVersion", prefab.value("formatVersion", 4)}, {"empties", Json::array()}};
    if (weapon < 0) return fragment;
    std::unordered_set<int> needed;
    std::unordered_set<int> attachments;
    for (const auto& [id, node] : nodes) if (IsAttachment(node)) attachments.insert(id);
    std::unordered_set<int> rendered;
    for (const auto& [id, node] : nodes) {
        std::unordered_set<int> ancestors;
        int walk = id;
        while (nodes.count(walk) && ancestors.insert(walk).second && walk != weapon)
            walk = nodes.at(walk).value("parentId", -1);
        const bool attachment = std::any_of(ancestors.begin(), ancestors.end(), [&](int a) { return attachments.count(a) != 0; });
        if (walk == weapon && (node.contains("Particle System") || attachment)) {
            needed.insert(ancestors.begin(), ancestors.end());
            if (attachment) rendered.insert(id);
        }
    }
    if (needed.empty()) return fragment;
    needed.insert(weapon);
    fragment["models"] = Json::array();
    std::vector<int> ordered(needed.begin(), needed.end());
    std::sort(ordered.begin(), ordered.end(), [&](int a, int b) {
        const int ao = nodes.at(a).value("order", a), bo = nodes.at(b).value("order", b);
        return ao == bo ? a < b : ao < bo;
    });
    for (int id : ordered) {
        const auto& source = nodes.at(id);
        Json node;
        for (const char* key : {"id", "order", "parentId", "name", "position", "rotation", "scale", "active", "Particle System", "C# Script",
                                "Muzzle Attachment", "Grip Attachment", "Optic Attachment"})
            if (source.contains(key)) node[key] = source.at(key);
        if (rendered.count(id) && source.contains("path")) {
            for (const char* key : {"path", "pathGuid", "materials", "visible", "castShadows"})
                if (source.contains(key)) node[key] = source.at(key);
        }
        if (id == weapon) {
            node["parentId"] = -1;
            node["position"] = {0, 0, 0}; node["rotation"] = {0, 0, 0, 1}; node["scale"] = {1, 1, 1};
            node["name"] = "[Runtime] Weapon Attachments";
        }
        fragment[node.contains("path") ? "models" : "empties"].push_back(std::move(node));
    }
    return fragment;
}
}

namespace {
bool Below(const World& world, entt::entity child, entt::entity root) {
    std::unordered_set<entt::entity> visited;
    while (world.Registry.valid(child) && visited.insert(child).second) {
        if (child == root) return true;
        const auto* h = world.Registry.try_get<HierarchyComponent>(child);
        child = h ? h->Parent : entt::null;
    }
    return false;
}
entt::entity Reference(const World& world, entt::entity owner, const std::string& path) {
    return ResolveSceneReference(world, owner, path);
}
}

void WeaponAttachments::Load(World& world, entt::entity root, std::array<int, 3> preferred) {
    for (auto& group : m_Items) group.clear();
    m_Selected = {{-1, -1, -1}};
    if (!world.Registry.valid(root)) return;
    std::vector<entt::entity> ordered;
    auto collect = [&](auto view) { for (auto e : view) if (Below(world, e, root)) ordered.push_back(e); };
    collect(world.Registry.view<MuzzleAttachmentComponent>());
    collect(world.Registry.view<GripAttachmentComponent>());
    collect(world.Registry.view<OpticAttachmentComponent>());
    std::sort(ordered.begin(), ordered.end(), [&](auto a, auto b) {
        const auto* ao = world.Registry.try_get<OrderComponent>(a);
        const auto* bo = world.Registry.try_get<OrderComponent>(b);
        return (ao ? ao->Value : (int)entt::to_integral(a)) < (bo ? bo->Value : (int)entt::to_integral(b));
    });
    auto add = [&](entt::entity e, int kind, const std::string& point, const std::string& flash) {
        WeaponAttachment a; a.Entity = e;
        a.Point = Reference(world, e, point);
        a.Flash = Reference(world, e, flash);
        // An attachment cannot accidentally reference another category's objects.
        if (!Below(world, a.Point, e)) a.Point = entt::null;
        if (!Below(world, a.Flash, e)) a.Flash = entt::null;
        if (m_Selected[kind] < 0 && !world.Registry.all_of<DeactivatedTag>(e)) m_Selected[kind] = (int)m_Items[kind].size();
        m_Items[kind].push_back(std::move(a));
        return &m_Items[kind].back();
    };
    for (auto e : ordered) {
        if (const auto* m = world.Registry.try_get<MuzzleAttachmentComponent>(e); m && m->Enabled) {
            auto* a = add(e, 0, m->MuzzleTransform, m->MuzzleFlash);
            a->FiringSounds = m->FiringSounds;
            a->FiringSoundProfile = m->FiringSoundProfile;
        }
        if (const auto* g = world.Registry.try_get<GripAttachmentComponent>(e); g && g->Enabled) add(e, 1, "", "");
        if (const auto* o = world.Registry.try_get<OpticAttachmentComponent>(e); o && o->Enabled) {
            auto* a = add(e, 2, o->AimPoint, "");
            a->AimBlendTime = std::clamp(o->AimBlendTime, 0.0f, 1.0f);
            a->DefaultOptic = o->DefaultOptic;
        }
    }
    for (int kind = 0; kind < 3; ++kind) {
        if (m_Items[kind].empty()) continue;
        if (preferred[kind] >= 0 && preferred[kind] < (int)m_Items[kind].size()) m_Selected[kind] = preferred[kind];
        if (m_Selected[kind] < 0) m_Selected[kind] = 0;
        for (int i = 0; i < (int)m_Items[kind].size(); ++i) {
            const auto e = m_Items[kind][i].Entity;
            if (i == m_Selected[kind]) world.Registry.remove<DeactivatedTag>(e);
            else world.Registry.emplace_or_replace<DeactivatedTag>(e);
        }
    }
    world.SyncActiveInHierarchy();
}

bool WeaponAttachments::Cycle(World& world, AttachmentKind kind) {
    const int k = (int)kind, n = (int)m_Items[k].size();
    if (n < 2) return false;
    world.Registry.emplace_or_replace<DeactivatedTag>(m_Items[k][m_Selected[k]].Entity);
    m_Selected[k] = (m_Selected[k] + 1) % n;
    world.Registry.remove<DeactivatedTag>(m_Items[k][m_Selected[k]].Entity);
    world.SyncActiveInHierarchy();
    return true;
}
const WeaponAttachment* WeaponAttachments::Selected(AttachmentKind kind) const {
    const int k = (int)kind, i = m_Selected[k];
    return i >= 0 && i < (int)m_Items[k].size() ? &m_Items[k][i] : nullptr;
}
bool WeaponAttachments::Pose(const World& world, entt::entity root, AttachmentKind kind, glm::mat4& out) const {
    const auto* a = Selected(kind);
    if (!a || !world.Registry.valid(a->Point) || !world.Registry.valid(root)) return false;
    out = glm::inverse(world.ComposeWorldTransform(root)) * world.ComposeWorldTransform(a->Point);
    return true;
}
bool WeaponAttachments::OwnsEmitter(const World& world, entt::entity emitter) const {
    if (world.Registry.all_of<InactiveTag>(emitter)) return false;
    const auto* a = Selected(AttachmentKind::Muzzle);
    return !a || (world.Registry.valid(a->Flash) && Below(world, emitter, a->Flash));
}
bool WeaponAttachments::ReferenceOpticPose(const World& world, entt::entity root, glm::mat4& out) const {
    if (!world.Registry.valid(root)) return false;
    const WeaponAttachment* reference = nullptr;
    for (const auto& a : m_Items[(int)AttachmentKind::Optic]) {
        if (!world.Registry.valid(a.Point)) continue;
        if (!reference || a.DefaultOptic) reference = &a;
        if (a.DefaultOptic) break;
    }
    if (!reference) return false;
    out = glm::inverse(world.ComposeWorldTransform(root)) * world.ComposeWorldTransform(reference->Point);
    return true;
}

entt::entity InstantiateWeaponParticles(World& world, AssetLibrary& assets, const std::string& prefab,
                                       std::vector<entt::entity>& emitters, std::string& error, bool loadMeshes) {
    emitters.clear();
    error.clear();
    try {
        CachedPrefab& cached = PrefabEntry(prefab);
        std::string text = loadMeshes ? cached.Fragment : std::string();
        if (text.empty()) {
            const auto json = PrefabJson(prefab);
            if (!json) { error = "Cannot open weapon prefab: " + prefab; return entt::null; }
            auto fragment = ParticleFragment(*json);
            // Authoring validation can inspect the full hierarchy without a graphics context.
            if (!loadMeshes && fragment.contains("models")) {
                for (auto node : fragment.at("models")) {
                    node.erase("path"); node.erase("pathGuid"); node.erase("materials");
                    fragment["empties"].push_back(std::move(node));
                }
                fragment["models"] = nlohmann::json::array();
            }
            if (fragment.at("empties").empty()) return entt::null;
            text = fragment.dump();
            if (loadMeshes) PrefabEntry(prefab).Fragment = text;
        }
        std::vector<entt::entity> created;
        if (!SceneSerializer::AppendEntitiesFromString(world, assets, text, created)) {
            for (auto e : created) if (world.Registry.valid(e)) world.DestroyEntityAndChildren(e);
            error = "Cannot load weapon particle systems: " + prefab; return entt::null;
        }
        entt::entity root = entt::null;
        for (auto e : created) {
            const auto* hierarchy = world.Registry.try_get<HierarchyComponent>(e);
            if (!hierarchy || hierarchy->Parent == entt::null) root = e;
            if (auto* ps = world.Registry.try_get<ParticleSystemComponent>(e)) {
                // Shots play the authored burst; edit-mode preview remains freely configurable.
                ps->Emitting = false;
                emitters.push_back(e);
            }
        }
        return root;
    } catch (const std::exception& e) {
        error = "Invalid weapon particles '" + prefab + "': " + e.what(); return entt::null;
    }
}


bool ResolveWeaponPrefab(const std::string& prefab,std::string& animationSet,std::string& error,
                         MuzzleEffectSettings* muzzle) {
    try {
        error.clear();
        if(const CachedPrefab& cached=PrefabEntry(prefab); cached.HaveResolve) {
            animationSet=cached.Set;
            if(muzzle)*muzzle=cached.Muzzle;
            return true;
        }
        const auto jsonPtr=PrefabJson(prefab);
        if(!jsonPtr) {error="Cannot open weapon prefab: "+prefab;return false;}
        const nlohmann::json& json=*jsonPtr;
        const nlohmann::json* block=WeaponBlock(json);
        if(!block) {error="Prefab has no Weapon Definition: "+prefab;return false;}
        WeaponDefinitionComponent weapon;
        ReadComponentJson("Weapon Definition",*block,&weapon);
        if(weapon.AnimationSet.empty()) {error="Weapon Definition has no Animation Set: "+prefab;return false;}
        MuzzleEffectSettings result=weapon.Muzzle;
        const auto fragment=ParticleFragment(json);
        for(const char* list:{"empties","models"})if(fragment.contains(list))
            for(const auto& node:fragment.at(list))result.PrefabParticles=result.PrefabParticles || node.contains("Particle System");
        CachedPrefab& entry=PrefabEntry(prefab);
        entry.Set=weapon.AnimationSet;entry.Muzzle=result;entry.HaveResolve=true;
        if(muzzle)*muzzle=std::move(result);
        animationSet=weapon.AnimationSet;return true;
    } catch(const std::exception& e) {error="Invalid weapon prefab '"+prefab+"': "+e.what();}
    return false;
}
bool ResolveWeaponGameplay(const std::string& prefab,FirstPersonWeaponGameplay& gameplay,std::string& error) {
    try {
        if(const CachedPrefab& cached=PrefabEntry(prefab); cached.HaveGameplay) {gameplay=cached.Gameplay;error.clear();return true;}
        const auto jsonPtr=PrefabJson(prefab);
        if(!jsonPtr){error="Cannot open weapon prefab: "+prefab;return false;}
        const nlohmann::json* block=WeaponBlock(*jsonPtr);
        if(!block){error="Prefab has no Weapon Definition";return false;}
        WeaponDefinitionComponent weapon;
        // Old prefabs take their stats from their old animation descriptor's gameplay block; the prefab's own values win.
        if(!block->contains("Magazine")) {
            ReadComponentJson("Weapon Definition",*block,&weapon);
            std::ifstream descriptor(ProjectPaths::Resolve(weapon.AnimationSet));
            if(descriptor) {
                const auto legacy=nlohmann::json::parse(descriptor);
                if(!ImportLegacyWeaponGameplay(legacy.value("gameplay",nlohmann::json::object()),weapon.Gameplay,error)) {
                    error="Legacy weapon stats could not migrate: "+error;return false;
                }
            }
        }
        ReadComponentJson("Weapon Definition",*block,&weapon);
        if(!NormalizeWeaponGameplay(weapon.Gameplay,error))return false;
        gameplay=weapon.Gameplay;
        CachedPrefab& entry=PrefabEntry(prefab);entry.Gameplay=gameplay;entry.HaveGameplay=true;
        return true;
    } catch(const std::exception& why){error=why.what();}
    return false;
}

}
