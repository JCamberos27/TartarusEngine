#include "DevSpawner.h"
#include "AI/NpcDirector.h"
#include "Creatures/SpiderSystem.h"
#include "Creatures/SpiderDamage.h"
#include "Npc/NpcDroppedWeapon.h"
#include "FirstPersonPresentation.h"
#include "Components.h"
#include "World.h"
#include "AssetLibrary.h"
#include "Model.h"
#include "SceneSerializer.h"
#include "PhysicsWorld.h"
#include "GameModuleAPI.h"
#include "FKeyRow.h"
#include "EditorSound.h"
#include "Shortcuts.h"
#include "Log.h"
#include "ProjectPaths.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <cstdio>

namespace {

struct NpcVariant { const char* Name; int Brain, Weapon; bool Passive; };
const NpcVariant kNpcs[] = {
    {"Soldier AK", 0, 0, false},      {"Soldier AK (Passive)", 0, 0, true},
    {"Soldier Shotgun", 0, 1, false}, {"Soldier Shotgun (Passive)", 0, 1, true},
    {"Soldier Unarmed", 0, 3, false}, {"Soldier Unarmed (Passive)", 0, 3, true},
    {"Training Dummy", 1, 3, false},
};
const char* kSpiders[] = {"Hunter", "Hunter (Passive)", "Scripted", "Swarm x5"};
const char* kItems[] = {"AK", "Shotgun", "Ammo Box", "Physics Crate"};
constexpr int kNpcCount = (int)(sizeof(kNpcs) / sizeof(kNpcs[0]));
constexpr int kSpiderCount = 4, kItemCount = 4;
const char* kSpiderPrefab = "assets/Characters/Spider/Spider.json";

bool Ray(const glm::vec3& from, const glm::vec3& dir, float maxDist, glm::vec3& point, glm::vec3& normal) {
    RaycastHit h;
    const float o[3] = {from.x, from.y, from.z}, d[3] = {dir.x, dir.y, dir.z};
    if (!PhysicsWorld::RaycastSolid(o, d, maxDist, QueryFilter{}, h) || !h.Hit) return false;
    point = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
    normal = glm::vec3(h.Normal[0], h.Normal[1], h.Normal[2]);
    return true;
}

// The soldier's prefab copied onto its own body, as a soldier's dead hand lets it go.
std::unique_ptr<NpcDroppedWeapon> DropGun(World& world, AssetLibrary& assets, const std::string& prefab, const glm::vec3& at) {
    const entt::entity tmp = SceneSerializer::InstantiatePrefab(world, assets, ProjectPaths::Resolve(prefab));
    if (tmp == entt::null) return nullptr;
    if (auto* t = world.Registry.try_get<TransformComponent>(tmp)) { t->Position = at; t->Rotation = glm::quat(glm::radians(glm::vec3(0.0f, 0.0f, 90.0f))); } // lying on its side
    DroppedWeaponSettingsComponent cfg = NpcDroppedWeapon::SettingsIn(world);
    cfg.Lifetime = 0.0f;
    auto gun = NpcDroppedWeapon::DropAt(world, tmp, glm::vec3(0.0f), glm::vec3(0.0f), cfg);
    world.DestroyEntityAndChildren(tmp);
    return gun;
}

entt::entity Box(World& world, AssetLibrary& assets, const glm::vec3& at, const glm::vec3& size, float mass, const char* name) {
    const entt::entity e = world.CreateModelEntity(assets.CreatePrimitive("cube"), at, glm::vec3(0.0f), size, name);
    ColliderComponent col; // zero extents: fit the render bounds
    world.Registry.emplace_or_replace<ColliderComponent>(e, col);
    RigidbodyComponent rb;
    rb.Mass = mass;
    world.Registry.emplace_or_replace<RigidbodyComponent>(e, rb);
    return e;
}

} // namespace

DevSpawner::DevSpawner() = default;
DevSpawner::~DevSpawner() = default;

// The models and materials a prefab (or scene fragment) names, started loading in the background: the first F9 / F10 /
// F11 of a Play then finds them in memory instead of importing them on the spot.
void WarmPrefab(AssetLibrary& assets, const std::string& rel) {
    std::ifstream in(std::filesystem::u8path(ProjectPaths::Resolve(rel)));
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    static const std::regex kPath("\"(assets/[^\"]+\\.(fbx|FBX|glb|gltf|obj|mat))\"");
    const std::string text = ss.str();
    for (std::sregex_iterator it(text.begin(), text.end(), kPath), end; it != end; ++it) {
        const std::string path = ProjectPaths::Resolve((*it)[1].str());
        if ((*it)[2].str() == "mat") assets.RequestMaterialAsync(path);
        else assets.RequestModelAsync(path);
    }
}

void DevSpawner::Warm(const Context& c) {
    const SquadSettingsComponent squad = c.Npcs ? c.Npcs->Settings() : SquadSettingsComponent{};
    const std::string prefabs[] = {kSpiderPrefab, squad.BodyPrefab, squad.RiflePrefab, squad.ShotgunPrefab};
    if (m_Warmed < 4 && !prefabs[m_Warmed].empty()) WarmPrefab(*c.Assets, prefabs[m_Warmed]);
    ++m_Warmed;
}

bool DevSpawner::Place(const Context& c, glm::vec3& at, float& yawDeg) const {
    const glm::vec3 f = glm::normalize(c.Front);
    glm::vec3 p, n;
    if (Ray(c.Eye, f, 30.0f, p, n)) {
        // A wall: step back off it and down to its foot.
        if (n.y < 0.6f) {
            glm::vec3 q, m;
            const glm::vec3 off = p + glm::vec3(n.x, 0.0f, n.z) * 0.6f + glm::vec3(0.0f, 0.3f, 0.0f);
            p = Ray(off, glm::vec3(0.0f, -1.0f, 0.0f), 6.0f, q, m) ? q : off;
        }
    } else {
        // Nothing in range: near the player, 4 m ahead, on whatever is under it.
        const glm::vec3 flat = glm::normalize(glm::vec3(f.x, 0.0f, f.z) + glm::vec3(1e-5f));
        const glm::vec3 ahead = c.Eye + flat * 4.0f;
        glm::vec3 q, m;
        p = Ray(ahead, glm::vec3(0.0f, -1.0f, 0.0f), 8.0f, q, m) ? q : ahead - glm::vec3(0.0f, 1.6f, 0.0f);
    }
    at = p;
    const glm::vec3 to = c.Eye - p; // facing the player
    yawDeg = glm::degrees(std::atan2(to.x, to.z));
    return true;
}

void DevSpawner::Update(const Context& c) {
    if (!c.WorldPtr || !c.Assets) return;
    if (m_Warmed < 4) Warm(c); // one prefab a frame
    // The first F9 in a scene with no NPC Spawn started the soldiers: their navmesh is up after a Think.
    if (m_WaitSquad && c.Npcs && c.Npcs->Started()) { m_WaitSquad = false; SpawnNpc(c); }
    if (c.Keys) {
        auto variant = [&](int& cur, int count, const char* key, const char* title, const char* const* names) {
            cur = (cur + 1) % count;
            EditorSound::Play(EditorSound::Event::FKeyCycle, -1.0f, 1.0f + 0.05f * (float)cur);
            FKeyRow::Show(key, title, std::string(names[cur]) + "  (next)", cur, count);
        };
        static const char* npcNames[kNpcCount];
        for (int i = 0; i < kNpcCount; ++i) npcNames[i] = kNpcs[i].Name;
        if (Shortcuts::TriggeredGlfw("fkey.nextNpc")) variant(m_Npc, kNpcCount, "F9", "NPC", npcNames);
        if (Shortcuts::TriggeredGlfw("fkey.nextSpider")) variant(m_Spider, kSpiderCount, "F10", "Spider", kSpiders);
        if (Shortcuts::TriggeredGlfw("fkey.nextItem")) variant(m_Item, kItemCount, "F11", "Item", kItems);
        if (Shortcuts::TriggeredGlfw("fkey.spawnNpc")) SpawnNpc(c);
        if (Shortcuts::TriggeredGlfw("fkey.spawnSpider")) SpawnSpider(c);
        if (Shortcuts::TriggeredGlfw("fkey.spawnItem")) SpawnItem(c);
        if (Shortcuts::TriggeredGlfw("fkey.clearSpawns")) Clear(c);
    }
    Pickups(c);
}

void DevSpawner::SpawnNpc(const Context& c) {
    const NpcVariant& v = kNpcs[m_Npc];
    if (!c.Npcs) return;
    if (!c.Npcs->Active()) {
        // No NPC Spawn here: start the soldiers on this press (a one-off navmesh build, cached after).
        if (c.StartSquad && c.StartSquad()) {
            m_WaitSquad = true;
            FKeyRow::Show("F9", "NPC", "Building the navmesh...");
        } else {
            FKeyRow::Show("F9", "NPC", "Can't start soldiers here");
            EditorSound::Play(EditorSound::Event::Blocked);
        }
        return;
    }
    if (!c.Npcs->Started()) { m_WaitSquad = true; return; }
    glm::vec3 at;
    float yaw = 0.0f;
    Place(c, at, yaw);
    const int idx = c.Npcs->SpawnAt(*c.WorldPtr, *c.Assets, at, yaw, v.Brain, v.Weapon, v.Passive);
    if (idx < 0) {
        FKeyRow::Show("F9", "NPC", "No room there");
        EditorSound::Play(EditorSound::Event::Blocked);
        return;
    }
    m_NpcIds.push_back(idx);
    EditorSound::Play(EditorSound::Event::SpawnNpc);
    FKeyRow::Show("F9", "NPC", v.Name, m_Npc, kNpcCount);
}

void DevSpawner::SpawnSpider(const Context& c) {
    if (!c.Spiders) return;
    glm::vec3 at;
    float yaw = 0.0f;
    Place(c, at, yaw);
    const int count = m_Spider == 3 ? 5 : 1;
    int made = 0;
    for (int i = 0; i < count; ++i) {
        // A swarm: a ring a metre across around the spot.
        const float a = 6.2831853f * (float)i / (float)count;
        const glm::vec3 p = at + (count > 1 ? glm::vec3(std::cos(a), 0.0f, std::sin(a)) * 0.9f : glm::vec3(0.0f)) + glm::vec3(0.0f, 0.15f, 0.0f);
        const entt::entity e = c.Spiders->Spawn(*c.WorldPtr, *c.Assets, kSpiderPrefab, p,
                                                glm::angleAxis(glm::radians(yaw), glm::vec3(0.0f, 1.0f, 0.0f)));
        if (e == entt::null) continue;
        if (auto* sc = c.WorldPtr->Registry.try_get<SpiderComponent>(e)) {
            sc->Brain = m_Spider == 2 ? 1 : 0;
            sc->Passive = m_Spider == 1;
        }
        m_SpiderIds.push_back(e);
        ++made;
        char msg[160];
        std::snprintf(msg, sizeof msg, "F10: %s spider at (%.2f, %.2f, %.2f), aimed from (%.2f, %.2f, %.2f).", kSpiders[m_Spider], p.x, p.y, p.z,
                      c.Eye.x, c.Eye.y, c.Eye.z);
        Log::Info(msg);
    }
    if (made == 0) {
        FKeyRow::Show("F10", "Spider", "Couldn't spawn");
        EditorSound::Play(EditorSound::Event::Blocked);
        return;
    }
    EditorSound::Play(EditorSound::Event::SpawnSpider);
    FKeyRow::Show("F10", "Spider", kSpiders[m_Spider], m_Spider, kSpiderCount);
}

void DevSpawner::SpawnItem(const Context& c) {
    World& world = *c.WorldPtr;
    glm::vec3 at;
    float yaw = 0.0f;
    Place(c, at, yaw);
    const glm::vec3 drop = at + glm::vec3(0.0f, 0.35f, 0.0f);
    const SquadSettingsComponent squad = c.Npcs ? c.Npcs->Settings() : SquadSettingsComponent{};
    bool ok = true;
    if (m_Item <= 1) {
        Pickup p;
        p.Kind = m_Item;
        p.Gun = DropGun(world, *c.Assets, m_Item == 0 ? squad.RiflePrefab : squad.ShotgunPrefab, drop);
        ok = p.Gun != nullptr;
        if (ok) m_Pickups.push_back(std::move(p));
    } else if (m_Item == 2) {
        Pickup p;
        p.Kind = 2;
        p.Prop = Box(world, *c.Assets, drop, glm::vec3(0.35f, 0.2f, 0.25f), 4.0f, "[Runtime] Ammo Box");
        m_Pickups.push_back(std::move(p));
    } else {
        m_Props.push_back(Box(world, *c.Assets, at + glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.8f), 25.0f, "[Runtime] Physics Crate"));
    }
    if (!ok) {
        FKeyRow::Show("F11", "Item", "Couldn't spawn");
        EditorSound::Play(EditorSound::Event::Blocked);
        return;
    }
    EditorSound::Play(EditorSound::Event::SpawnItem);
    FKeyRow::Show("F11", "Item", kItems[m_Item], m_Item, kItemCount);
}

void DevSpawner::Pickups(const Context& c) {
    World& world = *c.WorldPtr;
    for (Pickup& p : m_Pickups)
        if (p.Gun) p.Gun->Update(world, c.Dt);
    if (!c.Interact || m_Pickups.empty()) return;
    // The one under the crosshair: nearest the view ray, within reach.
    const glm::vec3 f = glm::normalize(c.Front);
    int best = -1;
    float bestOff = 0.45f;
    for (int i = 0; i < (int)m_Pickups.size(); ++i) {
        const Pickup& p = m_Pickups[(size_t)i];
        glm::vec3 pos;
        if (p.Gun) pos = p.Gun->Position(world);
        else if (const auto* t = world.Registry.valid(p.Prop) ? world.Registry.try_get<TransformComponent>(p.Prop) : nullptr) pos = t->Position;
        else continue;
        const float along = glm::dot(pos - c.Eye, f);
        if (along < 0.0f || along > 2.6f) continue;
        const float off = glm::length(pos - (c.Eye + f * along));
        if (off < bestOff) { bestOff = off; best = i; }
    }
    if (best < 0) return;
    Pickup& p = m_Pickups[(size_t)best];
    const char* what = p.Kind == 0 ? "AK" : p.Kind == 1 ? "Shotgun" : "Ammo";
    if (c.Gun && c.Gun->IsActive()) {
        if (p.Kind <= 1 && p.Kind < c.Gun->SlotCount()) c.Gun->SelectSlot(p.Kind);
        c.Gun->RefillAmmo();
    }
    if (p.Gun) p.Gun->Stop(world);
    if (world.Registry.valid(p.Prop)) world.DestroyEntityAndChildren(p.Prop);
    m_Pickups.erase(m_Pickups.begin() + best);
    EditorSound::Play(EditorSound::Event::SpawnItem, -1.0f, 1.25f);
    FKeyRow::Show("E", "Picked up", what);
}

void DevSpawner::Clear(const Context& c) {
    World& world = *c.WorldPtr;
    const int n = Spawned();
    if (c.Npcs)
        for (int idx : m_NpcIds)
            if (const Npc* npc = c.Npcs->Find(idx); npc && npc->DevSpawned) c.Npcs->Remove(world, idx); // the slot may be a squad soldier's now
    for (entt::entity e : m_SpiderIds) {
        if (!world.Registry.valid(e)) continue;
        if (c.SpiderHurt) c.SpiderHurt->Forget(e);
        world.DestroyEntityAndChildren(e);
    }
    m_NpcIds.clear();
    m_SpiderIds.clear();
    m_WaitSquad = false;
    Stop(world);
    EditorSound::Play(EditorSound::Event::ClearSpawns);
    FKeyRow::Show("F12", "Cleared", std::to_string(n) + (n == 1 ? " thing" : " things"));
}

void DevSpawner::Stop(World& world) {
    m_NpcIds.clear();
    m_SpiderIds.clear();
    m_WaitSquad = false;
    for (Pickup& p : m_Pickups) {
        if (p.Gun) p.Gun->Stop(world);
        if (world.Registry.valid(p.Prop)) world.DestroyEntityAndChildren(p.Prop);
    }
    m_Pickups.clear();
    for (entt::entity e : m_Props)
        if (world.Registry.valid(e)) world.DestroyEntityAndChildren(e);
    m_Props.clear();
}

void DevSpawner::Forget() {
    m_Warmed = 0;
    m_NpcIds.clear();
    m_SpiderIds.clear();
    m_Props.clear();
    m_Pickups.clear();
    m_WaitSquad = false;
}

int DevSpawner::Spawned() const {
    return (int)(m_NpcIds.size() + m_SpiderIds.size() + m_Props.size() + m_Pickups.size());
}
