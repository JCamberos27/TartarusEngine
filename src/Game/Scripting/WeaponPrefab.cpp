#include "WeaponPrefab.h"
#include "WeaponAttachments.h"
#include "ScriptComponent.h"
#include "ProjectPaths.h"
#include "AssetDatabase.h"
#include "ScriptReferences.h"
#include "Components.h"
#include "FirstPersonAnimation.h"
#include "ScriptRuntime.h"
#include <optional>
#include <limits>
#include "SceneSerializer.h"
#include "World.h"
#include "Log.h"
#include <json.hpp>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace Scripting {
namespace {

constexpr const char* kWeaponClass="Tartarus.Gameplay.WeaponDefinition";
std::optional<nlohmann::json> WeaponFields(const nlohmann::json& node) {
    if(!node.contains("C# Script")) return std::nullopt;
    const auto& script=node.at("C# Script");
    if(script.value("Class",std::string{})==kWeaponClass && script.value("Enabled",true))
        return nlohmann::json::parse(script.value("Fields JSON",std::string("{}")));
    const auto extra=nlohmann::json::parse(script.value("Scripts",std::string("[]")));
    if(!extra.is_array())throw std::runtime_error("C# Scripts must be an array");
    for(const auto& slot:extra)
        if(slot.value("class",std::string{})==kWeaponClass && slot.value("enabled",true))
            return slot.value("fields",nlohmann::json::object());
    return std::nullopt;
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
            if (weapon < 0 && (node.contains("Weapon Definition") || WeaponFields(node))) weapon = id;
        }
    }
    Json fragment = {{"formatVersion", prefab.value("formatVersion", 4)}, {"empties", Json::array()}};
    if (weapon < 0) return fragment;
    std::unordered_set<int> needed;
    std::unordered_set<int> attachments;
    for (const auto& [id, node] : nodes) {
        if (!node.contains("C# Script")) continue;
        const auto& script = node.at("C# Script");
        const auto className = script.value("Class", std::string{});
        if (className == "Tartarus.Gameplay.MuzzleAttachment" || className == "Tartarus.Gameplay.GripAttachment" ||
            className == "Tartarus.Gameplay.OpticAttachment") attachments.insert(id);
        const auto extra = Json::parse(script.value("Scripts", std::string("[]")), nullptr, false);
        if (extra.is_array()) for (const auto& slot : extra) {
            const auto name = slot.value("class", std::string{});
            if (name == "Tartarus.Gameplay.MuzzleAttachment" || name == "Tartarus.Gameplay.GripAttachment" ||
                name == "Tartarus.Gameplay.OpticAttachment") attachments.insert(id);
        }
    }
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
        for (const char* key : {"id", "order", "parentId", "name", "position", "rotation", "scale", "active", "Particle System", "C# Script"})
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
    for (auto e : world.Registry.view<CSharpScriptComponent>()) if (Below(world, e, root)) ordered.push_back(e);
    std::sort(ordered.begin(), ordered.end(), [&](auto a, auto b) {
        const auto* ao = world.Registry.try_get<OrderComponent>(a);
        const auto* bo = world.Registry.try_get<OrderComponent>(b);
        return (ao ? ao->Value : (int)entt::to_integral(a)) < (bo ? bo->Value : (int)entt::to_integral(b));
    });
    for (auto e : ordered) for (const auto& slot : GetSlots(world.Registry.get<CSharpScriptComponent>(e))) {
        if (!slot.Enabled) continue;
        int kind = slot.Class == "Tartarus.Gameplay.MuzzleAttachment" ? 0 :
                   slot.Class == "Tartarus.Gameplay.GripAttachment" ? 1 :
                   slot.Class == "Tartarus.Gameplay.OpticAttachment" ? 2 : -1;
        if (kind < 0) continue;
        const auto fields = nlohmann::json::parse(slot.Fields, nullptr, false);
        if (!fields.is_object()) { Log::Warn("Weapon attachment has invalid script fields: " + slot.Class); continue; }
        try {
        WeaponAttachment a; a.Entity = e;
        a.Point = Reference(world, e, fields.value(kind == 0 ? "MuzzleTransform" : "AimPoint", kind == 0 ? "Muzzle" : kind == 2 ? "Aim Point" : ""));
        a.Flash = Reference(world, e, fields.value("MuzzleFlash", std::string("Muzzle")));
        // An attachment cannot accidentally reference another category's objects.
        if (!Below(world, a.Point, e)) a.Point = entt::null;
        if (!Below(world, a.Flash, e)) a.Flash = entt::null;
        a.FiringSounds = fields.value("FiringSounds", std::string{});
        a.FiringSoundProfile = fields.value("FiringSoundProfile", std::string{});
        a.AimBlendTime = std::clamp(fields.value("AimBlendTime", .2f), 0.0f, 1.0f);
        a.DefaultOptic = fields.value("DefaultOptic", false);
        if (m_Selected[kind] < 0 && !world.Registry.all_of<DeactivatedTag>(e)) m_Selected[kind] = (int)m_Items[kind].size();
        m_Items[kind].push_back(std::move(a));
        } catch (const nlohmann::json::exception& ex) {
            Log::Warn("Weapon attachment has invalid script fields: " + slot.Class + ": " + ex.what());
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
        std::ifstream file(ProjectPaths::Resolve(prefab));
        if (!file) { error = "Cannot open weapon prefab: " + prefab; return entt::null; }
        auto fragment = ParticleFragment(nlohmann::json::parse(file));
        // Authoring validation can inspect the full hierarchy without a graphics context.
        if (!loadMeshes && fragment.contains("models")) {
            for (auto node : fragment.at("models")) {
                node.erase("path"); node.erase("pathGuid"); node.erase("materials");
                fragment["empties"].push_back(std::move(node));
            }
            fragment["models"] = nlohmann::json::array();
        }
        if (fragment.at("empties").empty()) return entt::null;
        std::vector<entt::entity> created;
        if (!SceneSerializer::AppendEntitiesFromString(world, assets, fragment.dump(), created)) {
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


bool MigrateLegacyWeaponDefinition(nlohmann::json& entity) {
    using Json=nlohmann::json;
    if(!entity.contains("Weapon Definition"))return false;
    Json fields=Json::object();
    for(const auto& [oldName,value]:entity.at("Weapon Definition").items()) {
        std::string name=oldName;name.erase(std::remove(name.begin(),name.end(),' '),name.end());
        auto converted=value;
        if((name=="AnimationSet" || name=="FlameTexture") && value.is_string())converted={{"path",value},{"pathGuid",""}};
        if((name=="LightColor" || name=="FlameColor") && value.is_array())
            converted={{"X",value.at(0)},{"Y",value.at(1)},{"Z",value.at(2)}};
        fields[name]=std::move(converted);
    }
    auto script=entity.value("C# Script",Json::object());
    auto extra=Json::parse(script.value("Scripts",std::string("[]")));
    if(!extra.is_array())throw std::runtime_error("C# Scripts must be an array");
    bool found=false;
    if(script.value("Class",std::string{})==kWeaponClass) {
        auto current=Json::parse(script.value("Fields JSON",std::string("{}")));
        for(const auto& [name,value]:fields.items())if(!current.contains(name))current[name]=value;
        script["Fields JSON"]=current.dump();found=true;
    }
    for(auto& slot:extra)if(slot.value("class",std::string{})==kWeaponClass) {
        auto current=slot.value("fields",Json::object());
        for(const auto& [name,value]:fields.items())if(!current.contains(name))current[name]=value;
        slot["fields"]=std::move(current);found=true;
    }
    if(!found && !entity.contains("C# Script")) {
        script["Class"]=kWeaponClass;script["Source"]="assets/Scripts/WeaponDefinition.cs";
        script["Fields JSON"]=fields.dump();script["Enabled"]=true;
        script["Next Script ID"]=std::max(1,script.value("Next Script ID",0));
    } else if(!found) {
        int id=std::max(1,script.value("Next Script ID",1));
        for(const auto& slot:extra) {
            const int previous=slot.at("id").get<int>();
            if(previous<1 || previous>=std::numeric_limits<int>::max())throw std::runtime_error("Invalid C# script slot ID");
            id=std::max(id,previous+1);
        }
        if(id==std::numeric_limits<int>::max())throw std::runtime_error("Too many C# script slots");
        extra.push_back({{"id",id},{"source","assets/Scripts/WeaponDefinition.cs"},{"class",kWeaponClass},{"fields",fields},{"enabled",true}});
        script["Next Script ID"]=id+1;
    }
    script["Scripts"]=extra.dump();entity["C# Script"]=std::move(script);
    entity.erase("Weapon Definition");return true;
}

bool ResolveWeaponPrefab(const std::string& prefab,std::string& animationSet,std::string& error,
                         MuzzleEffectSettings* muzzle) {
    try {
        error.clear();
        std::ifstream file(ProjectPaths::Resolve(prefab));
        if(!file) {error="Cannot open weapon prefab: "+prefab;return false;}
        const auto json=nlohmann::json::parse(file);
        for(const char* collection:{"empties","models","boxes"}) {
            if(!json.contains(collection))continue;
            for(auto entity:json.at(collection)) {
                MigrateLegacyWeaponDefinition(entity);
                const auto authored=WeaponFields(entity);
                if(!authored)continue;
                // Defaults, field types and the authored schema belong to the project C# class.
                std::string resolved;
                if(!ResolveScriptFields(kWeaponClass,authored->dump(),resolved)) {
                    error="Cannot resolve C# WeaponDefinition settings. Check the gameplay build: "+prefab;return false;
                }
                const auto fields=nlohmann::json::parse(resolved);
                const auto& asset=fields.at("AnimationSet");
                const std::string set=AssetDatabase::FollowRef(asset.value("path",std::string{}),asset.value("pathGuid",std::string{}));
                if(set.empty()) {error="WeaponDefinition has no Animation Set: "+prefab;return false;}
                if(muzzle) {
                    MuzzleEffectSettings result;
                    result.Enabled=fields.at("MuzzleEnabled").get<bool>();
#define READ_EFFECT(Type, Member) result.Member=fields.at(#Member).get<Type>()
                    READ_EFFECT(float,FlashTime);READ_EFFECT(float,LightIntensity);READ_EFFECT(float,PlayerFlashScale);READ_EFFECT(float,LightRange);
                    READ_EFFECT(int,MuzzleStyle);READ_EFFECT(float,FlameGlow);READ_EFFECT(float,FlameScale);
                    READ_EFFECT(float,FlameLifetimeMin);READ_EFFECT(float,FlameLifetimeMax);READ_EFFECT(float,FlameLengthMin);READ_EFFECT(float,FlameLengthMax);
                    READ_EFFECT(float,FlameWidthMin);READ_EFFECT(float,FlameWidthMax);READ_EFFECT(std::string,FlashSprite);READ_EFFECT(float,FlashLifetime);
                    READ_EFFECT(float,FlashSizeMin);READ_EFFECT(float,FlashSizeMax);READ_EFFECT(float,FlashIntensity);READ_EFFECT(bool,SideJets);READ_EFFECT(bool,CoreGlow);
                    READ_EFFECT(int,SparkCount);READ_EFFECT(bool,Smoke);READ_EFFECT(bool,AfterfireSmoke);READ_EFFECT(float,SmokeScale);
                    READ_EFFECT(float,SmokeLifetimeMin);READ_EFFECT(float,SmokeLifetimeMax);READ_EFFECT(float,SmokeAlpha);
#undef READ_EFFECT
                    auto color=[&](const char* name) {const auto& c=fields.at(name);return glm::vec3(c.at("X").get<float>(),c.at("Y").get<float>(),c.at("Z").get<float>());};
                    result.LightColor=color("LightColor");result.FlameColor=color("FlameColor");
                    const auto& flame=fields.at("FlameTexture");
                    result.FlameTexture=AssetDatabase::FollowRef(flame.value("path",std::string{}),flame.value("pathGuid",std::string{}));
                    const auto fragment=ParticleFragment(json);
                    for(const char* list:{"empties","models"})if(fragment.contains(list))
                        for(const auto& node:fragment.at(list))result.PrefabParticles=result.PrefabParticles || node.contains("Particle System");
                    *muzzle=std::move(result);
                }
                animationSet=set;return true;
            }
        }
        error="Prefab has no enabled C# WeaponDefinition: "+prefab;
    } catch(const std::exception& e) {error="Invalid weapon prefab '"+prefab+"': "+e.what();}
    return false;
}
void ReadProjectWeaponGameplay(FirstPersonWeaponGameplay& result,const nlohmann::json& fields) {
#define READ(Type, Member) result.Member=fields.at(#Member).get<Type>()
    READ(int,Magazine);READ(float,RoundsPerMinute);READ(bool,AllowFullAuto);READ(int,BurstRounds);
    READ(float,ReloadHoldSeconds);READ(float,RegripMin);READ(float,RegripMax);READ(float,ImpactImpulse);READ(float,ImpactMaxSpeed);
    READ(float,BulletHoleRadius);READ(float,ZeroDistance);READ(bool,HasSightLine);READ(int,Pellets);
    READ(float,SpreadHip);READ(float,SpreadAds);READ(bool,CycleAfterShot);READ(float,CycleDelay);
    READ(float,Damage);READ(float,HeadMultiplier);READ(float,LimbMultiplier);READ(float,FalloffStart);READ(float,FalloffEnd);READ(float,FalloffMin);
#undef READ
    result.Reload=(FirstPersonWeaponGameplay::ReloadMode)fields.at("Reload").get<int>();
    for(auto pair:{std::pair{"SightOrigin",&result.SightOrigin},std::pair{"SightDirection",&result.SightDirection}}) {
        const auto& v=fields.at(pair.first);*pair.second={v.at("X").get<float>(),v.at("Y").get<float>(),v.at("Z").get<float>()};
    }
}
bool ResolveWeaponGameplay(const std::string& prefab,FirstPersonWeaponGameplay& gameplay,std::string& error) {
    try {
        std::ifstream file(ProjectPaths::Resolve(prefab));if(!file){error="Cannot open weapon prefab: "+prefab;return false;}
        const auto json=nlohmann::json::parse(file);
        for(const char* collection:{"empties","models","boxes"})if(json.contains(collection))for(auto entity:json.at(collection)) {
            MigrateLegacyWeaponDefinition(entity);const auto authored=WeaponFields(entity);if(!authored)continue;
            auto fields=*authored;
            // Old prefabs receive stats from their old animation descriptor exactly once on read.
            if(!fields.contains("Magazine")) {
                const auto& ref=fields.at("AnimationSet");
                const auto path=AssetDatabase::FollowRef(ref.value("path",std::string{}),ref.value("pathGuid",std::string{}));
                std::ifstream descriptor(ProjectPaths::Resolve(path));
                if(descriptor) {
                    const auto legacy=nlohmann::json::parse(descriptor);std::string imported;
                    if(!RequestProject("weapon.import",legacy.value("gameplay",nlohmann::json::object()).dump(),imported)){error="Legacy weapon stats could not migrate";return false;}
                    const auto defaults=nlohmann::json::parse(imported);
                    for(const auto& [name,value]:defaults.items())if(!fields.contains(name))fields[name]=value;
                }
            }
            std::string resolved;
            if(!RequestProject("weapon.resolve",fields.dump(),resolved)){error="Project weapon stats could not resolve";return false;}
            ReadProjectWeaponGameplay(gameplay,nlohmann::json::parse(resolved));error.clear();return true;
        }
        error="Prefab has no enabled project WeaponDefinition";
    } catch(const std::exception& why){error=why.what();}
    return false;
}

}
