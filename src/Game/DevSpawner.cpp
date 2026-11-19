#include "DevSpawner.h"
#include "AI/NpcDirector.h"
#include "FirstPersonPresentation.h"
#include "Components.h"
#include "World.h"
#include "AssetLibrary.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "GameModuleAPI.h"
#include "FKeyRow.h"
#include "EditorSound.h"
#include "Shortcuts.h"
#include "Log.h"
#include "Core/Paths.h"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace {

struct NpcVariant { const char* Name; bool Roam; };
const NpcVariant kNpcs[] = {{"Dummy", true}, {"Dummy (Standing)", false}};
const char* kItems[] = {"Ammo Box", "Physics Crate"};
constexpr int kNpcCount = (int)(sizeof(kNpcs) / sizeof(kNpcs[0]));
constexpr int kItemCount = (int)(sizeof(kItems) / sizeof(kItems[0]));

bool Ray(const glm::vec3& from, const glm::vec3& dir, float maxDist, glm::vec3& point, glm::vec3& normal) {
    RaycastHit h;
    const float o[3] = {from.x, from.y, from.z}, d[3] = {dir.x, dir.y, dir.z};
    if (!PhysicsWorld::RaycastSolid(o, d, maxDist, QueryFilter{}, h) || !h.Hit) return false;
    point = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
    normal = glm::vec3(h.Normal[0], h.Normal[1], h.Normal[2]);
    return true;
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

// The models and materials a prefab (or scene fragment) names, started loading in the background: the first F9 /
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
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::u8path(path), ec)) continue; // (a checkout without the Drive's assets)
        if ((*it)[2].str() == "mat") assets.RequestMaterialAsync(path);
        else assets.RequestModelAsync(path);
    }
}

} // namespace

void DevSpawner::Place(const Context& c, glm::vec3& at, float& yawDeg) const {
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
}

void DevSpawner::Update(const Context& c) {
    if (!c.WorldPtr || !c.Assets) return;
    // Once a Play, as soon as the dummies' settings name a body prefab.
    if (!m_Warmed && c.Npcs && !c.Npcs->Settings().BodyPrefab.empty()) {
        WarmPrefab(*c.Assets, c.Npcs->Settings().BodyPrefab);
        m_Warmed = true;
    }
    // The first F9 in a scene with no Dummy Spawn started the dummies: their navmesh is up after a Think.
    if (m_WaitSquad && c.Npcs && c.Npcs->Started()) { m_WaitSquad = false; SpawnNpc(c); }
    if (c.Keys) {
        auto variant = [&](int& cur, int count, const char* key, const char* title, const char* const* names) {
            cur = (cur + 1) % count;
            EditorSound::Play(EditorSound::Event::FKeyCycle, -1.0f, 1.0f + 0.05f * (float)cur);
            FKeyRow::Show(key, title, std::string(names[cur]) + "  (next)", cur, count);
        };
        static const char* npcNames[kNpcCount];
        for (int i = 0; i < kNpcCount; ++i) npcNames[i] = kNpcs[i].Name;
        if (Shortcuts::TriggeredGlfw("fkey.nextNpc")) variant(m_Npc, kNpcCount, "F9", "Dummy", npcNames);
        if (Shortcuts::TriggeredGlfw("fkey.nextItem")) variant(m_Item, kItemCount, "F11", "Item", kItems);
        if (Shortcuts::TriggeredGlfw("fkey.spawnNpc")) SpawnNpc(c);
        if (Shortcuts::TriggeredGlfw("fkey.spawnItem")) SpawnItem(c);
        if (Shortcuts::TriggeredGlfw("fkey.clearSpawns")) Clear(c);
    }
    Pickups(c);
}

void DevSpawner::SpawnNpc(const Context& c) {
    const NpcVariant& v = kNpcs[m_Npc];
    if (!c.Npcs) return;
    if (!c.Npcs->Active()) {
        // No Dummy Spawn here: start the dummies on this press (a one-off navmesh build, cached after).
        if (c.StartSquad && c.StartSquad()) {
            m_WaitSquad = true;
            FKeyRow::Show("F9", "Dummy", "Building the navmesh...");
        } else {
            FKeyRow::Show("F9", "Dummy", "Can't start dummies here");
            EditorSound::Play(EditorSound::Event::Blocked);
        }
        return;
    }
    if (!c.Npcs->Started()) { m_WaitSquad = true; return; }
    glm::vec3 at;
    float yaw = 0.0f;
    Place(c, at, yaw);
    const int idx = c.Npcs->SpawnAt(*c.WorldPtr, *c.Assets, at, yaw, v.Roam);
    if (idx < 0) {
        FKeyRow::Show("F9", "Dummy", "No room there");
        EditorSound::Play(EditorSound::Event::Blocked);
        return;
    }
    m_NpcIds.push_back(idx);
    EditorSound::Play(EditorSound::Event::SpawnNpc);
    FKeyRow::Show("F9", "Dummy", v.Name, m_Npc, kNpcCount);
}

void DevSpawner::SpawnItem(const Context& c) {
    World& world = *c.WorldPtr;
    glm::vec3 at;
    float yaw = 0.0f;
    Place(c, at, yaw);
    const glm::vec3 drop = at + glm::vec3(0.0f, 0.35f, 0.0f);
    if (m_Item == 0) m_Pickups.push_back(Box(world, *c.Assets, drop, glm::vec3(0.35f, 0.2f, 0.25f), 4.0f, "[Runtime] Ammo Box"));
    else m_Props.push_back(Box(world, *c.Assets, at + glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.8f), 25.0f, "[Runtime] Physics Crate"));
    EditorSound::Play(EditorSound::Event::SpawnItem);
    FKeyRow::Show("F11", "Item", kItems[m_Item], m_Item, kItemCount);
}

void DevSpawner::Pickups(const Context& c) {
    World& world = *c.WorldPtr;
    if (!c.Interact || m_Pickups.empty()) return;
    // The one under the crosshair: nearest the view ray, within reach.
    const glm::vec3 f = glm::normalize(c.Front);
    int best = -1;
    float bestOff = 0.45f;
    for (int i = 0; i < (int)m_Pickups.size(); ++i) {
        const entt::entity p = m_Pickups[(size_t)i];
        glm::vec3 pos;
        if (const auto* t = world.Registry.valid(p) ? world.Registry.try_get<TransformComponent>(p) : nullptr) pos = t->Position;
        else continue;
        const float along = glm::dot(pos - c.Eye, f);
        if (along < 0.0f || along > 2.6f) continue;
        const float off = glm::length(pos - (c.Eye + f * along));
        if (off < bestOff) { bestOff = off; best = i; }
    }
    if (best < 0) return;
    const entt::entity p = m_Pickups[(size_t)best];
    if (c.Gun && c.Gun->IsActive()) c.Gun->RefillAmmo();
    if (world.Registry.valid(p)) world.DestroyEntityAndChildren(p);
    m_Pickups.erase(m_Pickups.begin() + best);
    EditorSound::Play(EditorSound::Event::SpawnItem, -1.0f, 1.25f);
    FKeyRow::Show("E", "Picked up", "Ammo");
}

void DevSpawner::Clear(const Context& c) {
    World& world = *c.WorldPtr;
    const int n = Spawned();
    if (c.Npcs)
        for (int idx : m_NpcIds)
            if (const Npc* npc = c.Npcs->Find(idx); npc && npc->DevSpawned) c.Npcs->Remove(world, idx); // the slot may be a scene dummy's now
    Stop(world);
    EditorSound::Play(EditorSound::Event::ClearSpawns);
    FKeyRow::Show("F12", "Cleared", std::to_string(n) + (n == 1 ? " thing" : " things"));
}

void DevSpawner::Stop(World& world) {
    m_NpcIds.clear();
    m_WaitSquad = false;
    for (entt::entity p : m_Pickups)
        if (world.Registry.valid(p)) world.DestroyEntityAndChildren(p);
    m_Pickups.clear();
    for (entt::entity e : m_Props)
        if (world.Registry.valid(e)) world.DestroyEntityAndChildren(e);
    m_Props.clear();
}

void DevSpawner::Forget() {
    m_Warmed = false;
    m_NpcIds.clear();
    m_Props.clear();
    m_Pickups.clear();
    m_WaitSquad = false;
}

int DevSpawner::Spawned() const {
    return (int)(m_NpcIds.size() + m_Props.size() + m_Pickups.size());
}
