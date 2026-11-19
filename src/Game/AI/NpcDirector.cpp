#include "NpcDirector.h"

#include "AssetLibrary.h"
#include "Audio/FoleyAudio.h"
#include "Combat/CombatFx.h"
#include "Components.h"
#include "FirstPersonWeaponGameplay.h"
#include "GameModuleAPI.h" // RaycastHit, QueryFilter
#include "Log.h"
#include "NpcConfig.h"
#include "OutfitSystem.h"
#include "Profiler.h"
#include "PhysicsWorld.h"
#include "Core/Paths.h"
#include "SceneRenderer.h"
#include "SceneSerializer.h"
#include "World.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

constexpr float kRadius = kNpcCapsuleRadius;
constexpr float kStandCyl = 0.6f;    // capsule 1.8 m standing
constexpr float kRoamRadius = 7.0f;  // m round its post a roaming dummy wanders
constexpr float kArrived = 0.55f;    // m from its goal it counts as there
constexpr float kGlanceRate = 0.48f; // glances a second a standing dummy takes (Roam; the old 0.008 a frame at 60 fps)

unsigned Id(entt::entity e) { return (unsigned)entt::to_integral(e); }

// The standing pace's ease up from a halt and down into a stop (s), as the player's Ground Accel / Decel Time; a stop ends this
// short of its goal (m).
constexpr float kStartTime = 0.12f, kStopTime = 0.15f, kArriveSlack = 0.05f;
constexpr float kTravelTurnRate = 140.0f; // degrees/s the travel direction may swing (Move): a walker's arc round a corner, not a whip
// Turn first (the owner, 2026-11-18: "Turn first" - a narrow exception to "movement stays capsule-led", for these starts only):
// a dummy setting off from a stand with its way more than kTurnFirstOver behind its facing is held while its body turns on the
// spot, and sets off (the usual kStartTime ease) once the body is within its Turn Threshold of the way (what a standing body
// accepts without turning; the start takes turn the rest), or after kTurnFirstMost at the latest. Led by the capsule, the
// 170-180 deg start turned the body at a walking pace for a second: its tail, still 20 deg off 0.7 s in, re-searched the start
// takes (npcloco `walk left`: 2.40 jumps a second cruising against 2.30). Only for a body on motion matching (a body in its bind
// pose never turns). Measured on npcloco: released within 66 deg (the Turn Threshold) or 60, every leg passes; at 50 walk back's
// pose step p99 is 22.9 deg, at 30 walk forward and left jump 2.40/s, at 10 go home's turn slides a foot 21 cm (on the old slide meter).
// The spot turn's own match is still poor (a jog take's standing frame): the handoff's Phase 5 open item 1.
constexpr float kTurnFirstOver = 90.0f, kTurnFirstMost = 2.0f; // degrees, s
constexpr float kTurnFirstFrom = 0.3f; // m/s: a start is from below this


// A dummy moves at the database's measured gait speeds (the speeds decision: the capsule at the mocap's own pace, only small
// warping): the walk, the jog, and for the run the sprint style (the old jog set: the pack has no sustained run). (Without the
// database - CI - the gait table's defaults, the same numbers.)
float SpeedFor(Gait g, const NpcBody& body) {
    const GaitSpeedTable& t = body.Motion().Gaits();
    switch (g) {
    case Gait::Still: return 0.0f;
    case Gait::Walk: return t.Walk;
    case Gait::Jog: return t.Jog;
    case Gait::Run: return t.Legs[5] > 0 ? t.Sprint : t.Jog;
    }
    return t.Walk;
}

int StyleFor(Gait g) {
    return g == Gait::Jog ? (int)MotionLabel::StyleJog : g == Gait::Run ? (int)MotionLabel::StyleSprint : (int)MotionLabel::StyleWalk;
}

// A dummy's tree shown or hidden: its own checkbox on the root, the derived flag on everything under it (set here rather
// than by World::SyncActiveInHierarchy, which walks the whole scene).
void SetTreeActive(World& world, entt::entity root, bool active) {
    auto& reg = world.Registry;
    if (!reg.valid(root)) return;
    if (active) reg.remove<DeactivatedTag>(root);
    else reg.emplace_or_replace<DeactivatedTag>(root);
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        if (!reg.valid(e)) continue;
        if (active) reg.remove<InactiveTag>(e);
        else reg.emplace_or_replace<InactiveTag>(e);
        if (const auto* h = reg.try_get<HierarchyComponent>(e)) stack.insert(stack.end(), h->Children.begin(), h->Children.end());
    }
}

} // namespace

const char* NpcDirector::SubName(int s) {
    static const char* kNames[SubCount] = {"AI Move", "AI Body", "AI Ragdoll", "AI Hitbox"};
    return s >= 0 && s < SubCount ? kNames[s] : "AI ?";
}

struct NpcDirector::DummyBody {
    entt::entity Root = entt::null;
    std::vector<std::pair<entt::entity, AnimatorControllerComponent>> Animators; // as built: each dummy starts from these
};

// Adds the time it lives to a sub-system's per-frame total (flushed by FlushCosts).
struct NpcDirector::SubTimer {
    NpcDirector& D;
    Sub S;
    std::chrono::steady_clock::time_point T0 = std::chrono::steady_clock::now();
    SubTimer(NpcDirector& d, Sub s) : D(d), S(s) {}
    ~SubTimer() { D.m_SubFrame[S] += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - T0).count(); }
};

void NpcDirector::FlushCosts(bool think) {
    const int first = think ? SubMove : SubBody, last = think ? SubMove : SubHitbox;
    const bool counting = m_Frame > 120; // the navigation mesh builds on the first Think
    for (int i = first; i <= last; ++i) {
        const float ms = m_SubFrame[i];
        m_SubFrame[i] = 0.0f;
        Profiler::PushSample(SubName(i), ms);
        if (!counting) continue;
        m_SubStat[i].Sum += ms;
        m_SubStat[i].Max = std::max(m_SubStat[i].Max, ms);
        ++m_SubStat[i].Frames;
    }
    CostStats& total = think ? m_ThinkStat : m_LateStat;
    const float ms = think ? m_ThinkMs : m_LateMs;
    if (counting) {
        total.Sum += ms;
        total.Max = std::max(total.Max, ms);
        ++total.Frames;
    }
}

NpcDirector::NpcDirector() = default;
NpcDirector::~NpcDirector() = default;

// The scene's Dummy Settings and Ragdoll Settings (the first of each; the defaults without one).
void NpcDirector::ApplySettings(const entt::registry& reg) {
    if (auto settings = reg.view<const SquadSettingsComponent>(); settings.begin() != settings.end())
        m_Cfg = reg.get<const SquadSettingsComponent>(*settings.begin());
    else m_Cfg = SquadSettingsComponent{};
    if (auto rag = reg.view<const RagdollSettingsComponent>(); rag.begin() != rag.end()) m_RagdollCfg = reg.get<const RagdollSettingsComponent>(*rag.begin());
    else m_RagdollCfg = RagdollSettingsComponent();
}

bool NpcDirector::Start(World& world, AssetLibrary& assets, bool allowEmpty) {
    (void)assets;
    Stop(world);
    auto& reg = world.Registry;
    std::vector<std::pair<int, SpawnPoint>> found;
    for (auto e : reg.view<NpcSpawnComponent>()) {
        if (reg.any_of<InactiveTag>(e)) continue;
        const auto& c = reg.get<NpcSpawnComponent>(e);
        const glm::mat4 w = world.ComposeWorldTransform(e);
        SpawnPoint s;
        s.Pos = glm::vec3(w[3]);
        s.Yaw = NpcYawOf(glm::vec3(w[2]), 0.0f);
        s.Roam = c.Roam;
        const auto* order = reg.try_get<OrderComponent>(e);
        found.push_back({order ? order->Value : (int)entt::to_integral(e), s});
    }
    if (found.empty() && !allowEmpty) return false;
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& f : found) m_Spawns.push_back(f.second);
    ApplySettings(reg);
    // The dummy's body, read once.
    {
        std::ifstream in(std::filesystem::u8path(ProjectPaths::Resolve(m_Cfg.BodyPrefab)));
        if (!in) {
            Log::Error(std::string("Dummies: can't read the body prefab ") + m_Cfg.BodyPrefab);
            m_Spawns.clear();
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        m_BodyJson = ss.str();
    }
    // The dummies are the player's body: they move by the player's numbers.
    m_HoldSettings = NpcHoldSettings{};
    if (auto bodies = world.Registry.view<FirstPersonBodyComponent>(); bodies.begin() != bodies.end()) {
        const auto& fpb = bodies.get<FirstPersonBodyComponent>(*bodies.begin());
        m_HoldSettings.TurnThreshold = glm::radians(fpb.NpcTurnThreshold); // degrees in the component
        m_HoldSettings.FaceEase = fpb.NpcFaceEase;
        m_HoldSettings.HeadMaxYaw = glm::radians(fpb.NpcHeadMaxYaw);
        m_HoldSettings.HeadMaxPitch = glm::radians(fpb.NpcHeadMaxPitch);
        m_HoldSettings.FootIKMaxDrop = fpb.NpcFootIKMaxDrop;
        m_HoldSettings.FootIKMaxRaise = fpb.NpcFootIKMaxRaise;
        m_HoldSettings.FootIKPelvisRaise = fpb.NpcFootIKPelvisRaise;
        m_HoldSettings.FootIKTiltMax = glm::radians(fpb.NpcFootIKTiltMax);
        m_HoldSettings.FootOffsetEase = fpb.NpcFootOffsetEase;
        m_HoldSettings.FootNormalEase = fpb.NpcFootNormalEase;
        m_HoldSettings.FootIKFade = fpb.NpcFootIKFade;
        m_HoldSettings.Spine = fpb.Spine; // per-bone spine weights and limits
        m_HoldSettings.FootLock = IK::FootLockFrom(fpb);   // planted feet held in the world
    }
    m_Active = true;
    m_Started = false;
    m_Now = 0.0f;
    Log::Info("Dummies: " + std::to_string(m_Spawns.size()) + " spawn(s).");
    return true;
}

void NpcDirector::Stop(World& world) {
    for (auto& n : m_Npcs)
        if (n) Despawn(world, *n);
    m_Npcs.clear();
    for (const auto& b : m_Pool)
        if (b && world.Registry.valid(b->Root)) world.DestroyEntityAndChildren(b->Root);
    m_Pool.clear();
    m_Bodies.clear();
    m_RespawnTimers.clear();
    m_Crowd.Clear();
    m_Nav.Clear();
    m_FleshHits.clear();
    m_Flinch.clear();
    m_DeathBones.clear();
    m_Scripted.clear();
    m_Spawns.clear();
    m_BodyJson.clear();
    m_PlayerAgent = -1;
    m_Active = m_Started = false;
    m_NextName = 1;
}

void NpcDirector::BuildNav() {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<float> verts;
    std::vector<int> tris;
    PhysicsWorld::CollectStaticGeometry(verts, tris, /*includeKinematic=*/false);
    NavBuildSettings s;
    const std::uint64_t hash = NavMesh::HashInput(verts, tris, s);
    char name[64];
    std::snprintf(name, sizeof name, "Library/NavCache/%016llx.nav", (unsigned long long)hash);
    const std::string cache = ProjectPaths::Resolve(name);
    m_NavCachePath = cache;
    m_NavHash = hash;
    bool loaded = m_Nav.Load(cache, hash);
    std::string error;
    if (!loaded) {
        if (!m_Nav.Build(verts, tris, s, &error)) {
            Log::Error("Dummies: navigation mesh failed (" + error + ") - the dummies can't move.");
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(cache).parent_path(), ec);
        m_Nav.Save(cache, hash);
    }
    // Only what the dummies can walk to from their spawns: a building's roof is walkable to Recast too.
    std::vector<glm::vec3> seeds;
    for (const SpawnPoint& sp : m_Spawns) seeds.push_back(sp.Pos);
    if (seeds.empty() && m_Player.Valid) seeds.push_back(m_Player.Feet); // started by F9 with no Dummy Spawn: where the player is
    const int unreachable = m_Nav.KeepReachable(seeds, glm::vec3(2.0f, 0.6f, 2.0f));
    m_Crowd.Init(m_Nav, 16, 0.6f);
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    char msg[256];
    std::snprintf(msg, sizeof msg, "Dummies: navigation mesh %s (%d polygons from %d triangles, %d out of the spawns' reach), %.0f ms.",
                  loaded ? "loaded" : "built", m_Nav.PolyCount(), (int)tris.size() / 3, unreachable, ms);
    Log::Info(msg);
}

bool NpcDirector::LateStart(World& world, AssetLibrary& assets) {
    m_Started = true;
    BuildNav();
    if (m_Player.Valid) m_PlayerAgent = m_Crowd.Add(m_Player.Feet, m_Player.Radius, m_Player.Height, 6.0f, /*steer=*/false);
    for (int i = 0; i < (int)m_Spawns.size(); ++i) Spawn(world, assets, i);
    // A spare body, built now and laid out of the way: the first replacement comes before any corpse has gone.
    if (m_Cfg.Respawn && !m_BodyJson.empty())
        if (auto spare = BuildBody(world, assets)) {
            for (const auto& [e, ac] : spare->Animators)
                if (world.Registry.valid(e)) world.Registry.remove<AnimatorControllerComponent>(e);
            SetTreeActive(world, spare->Root, false);
            m_Pool.push_back(std::move(spare));
        }
    // The dummies' shader variants compiled now, at Play start, not on the frame each first draws.
    SceneRenderer::WarmShaderVariants(world);
    return true;
}

std::shared_ptr<NpcDirector::DummyBody> NpcDirector::BuildBody(World& world, AssetLibrary& assets) {
    std::vector<entt::entity> created;
    if (!SceneSerializer::AppendEntitiesFromString(world, assets, m_BodyJson, created) || created.empty()) {
        Log::Error("Dummies: couldn't build a body from " + m_Cfg.BodyPrefab + ".");
        return nullptr;
    }
    auto& reg = world.Registry;
    auto built = std::make_shared<DummyBody>();
    for (entt::entity e : created) {
        const auto* h = reg.try_get<HierarchyComponent>(e);
        if (built->Root == entt::null && (!h || h->Parent == entt::null)) built->Root = e;
    }
    if (built->Root == entt::null) {
        for (entt::entity e : created)
            if (reg.valid(e)) world.DestroyEntityAndChildren(e);
        return nullptr;
    }
    // A prefab saved before its wardrobe's body changed gets the new parts now, before the body (and its ragdoll) reads
    // its pieces.
    OutfitSystem::RefreshBody(world, assets, built->Root);
    // Bare: every clothing item off, so the wardrobe dresses the body in its own skin (as Character Outfit's Bare Body).
    {
        if (OutfitSystem::Pieces(world, built->Root).empty()) OutfitSystem::AdoptExisting(world, assets, built->Root);
        Wardrobe::Request bare = OutfitSystem::CurrentRequest(world, built->Root);
        bare.Items.clear();
        const auto r = OutfitSystem::Apply(world, assets, built->Root, bare);
        if (!r.Ok) Log::Warn("Dummies: a body kept its clothes - " + r.Error);
    }
    std::vector<entt::entity> all;
    for (entt::entity e : created)
        if (reg.valid(e)) all.push_back(e);
    if (const auto* h = reg.try_get<HierarchyComponent>(built->Root))
        for (entt::entity c : h->Children)
            if (reg.valid(c) && std::find(all.begin(), all.end(), c) == all.end()) all.push_back(c);
    for (entt::entity e : all)
        if (const auto* ac = reg.try_get<AnimatorControllerComponent>(e)) built->Animators.push_back({e, *ac});
    return built;
}

int NpcDirector::SpawnAt(World& world, AssetLibrary& assets, const glm::vec3& pos, float yawDeg, bool roam) {
    if (!m_Started) return -1; // the navigation mesh comes on the first Think
    m_DevPoint = SpawnPoint{};
    m_DevPoint.Pos = pos;
    m_DevPoint.Yaw = glm::radians(yawDeg);
    m_DevPoint.Roam = roam;
    const int idx = Spawn(world, assets, kDevSpawn);
    if (Npc* n = Find(idx)) n->DevSpawned = true;
    return idx;
}

void NpcDirector::Remove(World& world, int index) {
    Npc* n = Find(index);
    if (!n) return;
    Despawn(world, *n);
    m_Npcs[(size_t)index].reset();
}

int NpcDirector::Spawn(World& world, AssetLibrary& assets, int spawnIndex) {
    const bool dev = spawnIndex == kDevSpawn;
    if ((!dev && (spawnIndex < 0 || spawnIndex >= (int)m_Spawns.size())) || m_BodyJson.empty()) return -1;
    const SpawnPoint sp = dev ? m_DevPoint : m_Spawns[(size_t)spawnIndex];
    PROFILE_SCOPE("AI Spawn");
    auto& reg = world.Registry;
    // A body from the pool (a corpse that has gone) when there is one: shown again, its animators as they were built.
    std::shared_ptr<DummyBody> built;
    for (size_t k = m_Pool.size(); k-- > 0 && !built;) {
        built = std::move(m_Pool[k]);
        m_Pool.erase(m_Pool.begin() + (long long)k);
        if (!built || !reg.valid(built->Root)) built.reset();
    }
    const bool reused = built != nullptr;
    if (built) {
        for (const auto& [e, ac] : built->Animators)
            if (reg.valid(e)) reg.emplace_or_replace<AnimatorControllerComponent>(e, ac);
        SetTreeActive(world, built->Root, true);
    } else if (!(built = BuildBody(world, assets))) {
        return -1;
    }
    const entt::entity root = built->Root;
    m_Bodies[root] = built;
    auto n = std::make_unique<Npc>();
    n->Index = (int)m_Npcs.size();
    for (int i = 0; i < (int)m_Npcs.size(); ++i)
        if (!m_Npcs[(size_t)i]) { n->Index = i; break; }
    n->Name = "Dummy " + std::to_string(m_NextName++);
    n->Root = root;
    if (auto* name = reg.try_get<NameComponent>(root)) name->Name = "[Runtime] " + n->Name;
    n->SpawnIndex = spawnIndex;
    n->Roam = sp.Roam;

    // Feet on the walkable mesh, a little apart from anyone already standing there - and off the dead (a living capsule meets
    // a corpse's parts, and one put down in a pile was thrown up out of it) and off the player. Stepped aside only to
    // somewhere walkable from the spawn; with nowhere clear, the spawn itself.
    auto crowdedAt = [&](const glm::vec3& at) {
        if (m_Player.Valid && glm::length(glm::vec2(m_Player.Feet.x - at.x, m_Player.Feet.z - at.z)) < 1.2f) return true;
        for (auto& o : m_Npcs) {
            if (!o) continue;
            if (!o->Dead && glm::length(o->Feet - at) < 1.0f) return true;
            if (o->Ragdoll)
                for (int i = 0; i < NpcRagdoll::kRagParts; ++i) {
                    const glm::vec3 d = o->Ragdoll->PartPosition(i) - at;
                    if (d.x * d.x + d.z * d.z < 0.55f * 0.55f && d.y < 1.0f) return true;
                }
        }
        return false;
    };
    glm::vec3 feet = sp.Pos;
    for (int tries = 0; tries < 12 && crowdedAt(feet); ++tries) {
        const float a = (float)tries * 2.4f;
        const glm::vec3 next = sp.Pos + glm::vec3(std::cos(a), 0.0f, std::sin(a)) * (0.9f + 0.25f * (float)tries);
        glm::vec3 on;
        if (m_Nav.Closest(next, on, glm::vec3(0.5f, 2.0f, 0.5f)) && m_Nav.Walkable(sp.Pos, on) && !crowdedAt(on)) { feet = on; break; }
    }
    glm::vec3 snapped;
    if (m_Nav.Closest(feet, snapped, glm::vec3(2.0f, 3.0f, 2.0f))) feet = snapped;
    n->Feet = feet;
    n->PostPos = feet;
    world.SetWorldPose(root, feet, glm::angleAxis(sp.Yaw, glm::vec3(0, 1, 0)));
    n->Body.SetHoldSettings(m_HoldSettings);
    if (!n->Body.Start(world, root)) {
        Log::Error("Dummies: the body has no animated skeleton.");
        m_Bodies.erase(root);
        world.DestroyEntityAndChildren(root);
        return -1;
    }
    n->Cct = PhysicsWorld::CreateNpcCharacter(Id(root), kRadius, kStandCyl, &feet.x);
    n->Agent = m_Crowd.Add(feet, kNpcNavRadius, 1.8f, 3.2f);
    // The prefab's Health (Max, Invulnerable) when it has one; 100 otherwise.
    auto& health = reg.get_or_emplace<HealthComponent>(root);
    health.Max = std::clamp(health.Max, 1.0f, 100000.0f);
    health.Current = health.Max;
    n->Health = n->MaxHealth = health.Max;
    if (!reused) SceneRenderer::WarmShaderVariants(world, built->Root); // a new body's variants, before its first draw
    const glm::vec3 fwd(std::sin(sp.Yaw), 0.0f, std::cos(sp.Yaw));
    n->Eye = feet + glm::vec3(0.0f, 1.62f, 0.0f);
    n->LookPoint = n->Eye + fwd * 10.0f;
    n->SpawnedAt = m_Now;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    n->IdleUntil = m_Now + 0.5f + 2.0f * unit(m_Rng); // a beat before it sets off
    {
        char msg[192];
        std::snprintf(msg, sizeof msg, "Dummies: %s spawned at (%.2f, %.2f, %.2f) from spawn %d%s%s.", n->Name.c_str(), feet.x, feet.y, feet.z,
                      spawnIndex, reused ? ", reused body" : "", n->Roam ? ", roaming" : "");
        Log::Info(msg);
    }
    const int idx = n->Index;
    if (idx < (int)m_Npcs.size()) m_Npcs[(size_t)idx] = std::move(n);
    else m_Npcs.push_back(std::move(n));
    return idx;
}

void NpcDirector::Despawn(World& world, Npc& n, bool keepBody) {
    n.Hitboxes.reset();
    n.Ragdoll.reset();
    n.Body.Stop();
    FoleyAudio::Get().NpcGone(n.Index); // its stride / foot state goes with it
    if (n.Cct != PhysicsWorld::kNoCharacter) PhysicsWorld::DestroyNpcCharacter(n.Cct);
    n.Cct = PhysicsWorld::kNoCharacter;
    if (n.Agent >= 0) m_Crowd.Remove(n.Agent);
    n.Agent = -1;
    if (n.Root != entt::null && world.Registry.valid(n.Root)) {
        auto found = m_Bodies.find(n.Root);
        // Kept for the next spawn (a few): hidden, its animators off (the ragdoll took them).
        if (keepBody && found != m_Bodies.end() && found->second && (int)m_Pool.size() < 4) {
            for (const auto& [e, ac] : found->second->Animators)
                if (world.Registry.valid(e)) world.Registry.remove<AnimatorControllerComponent>(e);
            SetTreeActive(world, n.Root, false);
            m_Pool.push_back(std::move(found->second));
        } else {
            world.DestroyEntityAndChildren(n.Root);
        }
        if (found != m_Bodies.end()) m_Bodies.erase(found);
    }
    n.Root = entt::null;
}

Npc* NpcDirector::Find(int index) {
    return index >= 0 && index < (int)m_Npcs.size() ? m_Npcs[(size_t)index].get() : nullptr;
}

// --- per frame ----------------------------------------------------------------------------------

void NpcDirector::Think(World& world, AssetLibrary& assets, float dt, const PlayerSnapshot& p) {
    if (!m_Active || dt <= 0.0f) return;
    const auto t0 = std::chrono::steady_clock::now();
    m_Player = p;
    m_Now += dt;
    ++m_Frame;
    if (!m_Started) {
        if (!PhysicsWorld::IsActive()) return;
        LateStart(world, assets);
    }
    // The player stands in the crowd too: the dummies walk round them.
    if (p.Valid) {
        if (m_PlayerAgent < 0 && m_Crowd.Valid()) m_PlayerAgent = m_Crowd.Add(p.Feet, p.Radius, p.Height, 6.0f, false);
        if (m_PlayerAgent >= 0) m_Crowd.Sync(m_PlayerAgent, p.Feet, p.Velocity);
    }

    for (auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        Npc& n = *up;
        // Health is the dummy's Health component: something else (the dev panel, a script) may have changed it.
        if (const auto* h = world.Registry.try_get<HealthComponent>(n.Root)) {
            n.Health = h->Current; n.MaxHealth = h->Max;
            if (h->Current <= 0.0f) { Kill(world, n, {0, 0, 1}, n.Feet, 0); continue; }
        }
        Roam(n, dt);
        if (const auto it = m_Scripted.find(n.Index); it != m_Scripted.end()) { // a test's order
            n.WantMove = true;
            n.MoveTarget = it->second.first;
            n.Pace = it->second.second;
        }
        // Movement goal -> the crowd.
        if (n.Agent >= 0) {
            if (n.WantMove) {
                if (!n.HasGoal || glm::length(n.Goal - n.MoveTarget) > 0.4f || !m_Crowd.HasTarget(n.Agent)) {
                    if (m_Crowd.SetTarget(n.Agent, n.MoveTarget)) {
                        n.Goal = n.MoveTarget;
                        n.HasGoal = true;
                    }
                }
                float speed = SpeedFor(n.Pace, n.Body);
                if (m_Now < n.StaggerUntil) speed *= 0.2f; // reeling from a heavy hit
                m_Crowd.SetMaxSpeed(n.Agent, speed);
                n.PaceSpeed = speed;
            } else if (n.HasGoal) {
                m_Crowd.Stop(n.Agent);
                n.HasGoal = false;
            }
        }
    }
    m_Crowd.Update(dt);

    UpdatePersistence(world, p);
    for (auto& up : m_Npcs) {
        if (!up || up->Dead) continue; // (a corpse lies until UpdatePersistence lets it go)
        {
            SubTimer timer(*this, SubMove);
            Move(world, *up, dt);
        }
        UpdateLod(world, *up, p);
    }
    Respawns(world, assets);
    m_ThinkMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    FlushCosts(true);
}

// A roaming dummy wanders round its post: it stands a few seconds, looking about now and then, walks to a random reachable
// point within kRoamRadius, and stands again. Stuck on the way (or too long getting there), it gives that point up.
void NpcDirector::Roam(Npc& n, float dt) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    if (!n.Roam || n.Agent < 0) {
        n.WantMove = false;
        return;
    }
    if (n.RoamPhase == 0) {
        n.WantMove = false;
        if (m_Now >= n.IdleUntil) {
            glm::vec3 pt;
            if (m_Nav.RandomPointNear(n.PostPos, kRoamRadius, unit(m_Rng), unit(m_Rng), pt) &&
                glm::length(glm::vec2(pt.x - n.Feet.x, pt.z - n.Feet.z)) > 1.5f) {
                n.MoveTarget = pt;
                n.Pace = Gait::Walk;
                n.RoamPhase = 1;
                n.RoamSince = m_Now;
                n.BlockedTime = 0.0f;
            } else {
                n.IdleUntil = m_Now + 0.5f;
            }
        }
        // Now and then a glance somewhere new (about one every two seconds, whatever the frame rate); otherwise the look holds.
        if (unit(m_Rng) < kGlanceRate * dt) {
            const float a = unit(m_Rng) * 6.283f;
            n.LookPoint = n.Eye + glm::vec3(std::cos(a), -0.1f, std::sin(a)) * 8.0f;
        }
        return;
    }
    n.WantMove = true;
    glm::vec3 ahead = n.MoveTarget - n.Feet;
    ahead.y = 0.0f;
    if (glm::dot(ahead, ahead) > 1e-4f) n.LookPoint = n.MoveTarget + glm::vec3(0.0f, 1.5f, 0.0f) + glm::normalize(ahead) * 6.0f;
    const bool arrived = glm::length(glm::vec2(ahead.x, ahead.z)) < kArrived;
    if (arrived || n.BlockedTime > 1.6f || m_Now - n.RoamSince > 30.0f) {
        n.RoamPhase = 0;
        n.WantMove = false;
        n.BlockedTime = 0.0f;
        n.IdleUntil = m_Now + 2.0f + 4.0f * unit(m_Rng);
    }
}

void NpcDirector::Move(World& world, Npc& n, float dt) {
    if (n.Cct == PhysicsWorld::kNoCharacter) return;
    glm::vec3 want(0.0f);
    if (n.WantMove && n.Agent >= 0) want = m_Crowd.Velocity(n.Agent);
    want.y = 0.0f;
    // The pace is the player body's: the gait's own speed right up to where its stop begins, then a stop the length of the
    // body's stop clip (an ease over kStopTime) - not the crowd's long creep in and a halt in a frame; from a halt, an ease up
    // to pace. The crowd keeps the way (its path, the avoidance). The body's wish goes with it: the pace toward the goal,
    // nothing once the stop has begun.
    glm::vec3 wish(0.0f);
    {
        const glm::vec3 flatVel(n.Velocity.x, 0.0f, n.Velocity.z);
        const float cur = glm::length(flatVel), crowd = glm::length(want);
        glm::vec3 dir = crowd > 0.05f ? want / crowd : cur > 0.05f ? flatVel / cur : glm::vec3(0.0f);
        // On the move the way it goes swings round at a body's pace, not in a frame (a path corner, the crowd's arrival
        // shuffle): the legs can't follow a snap, and the planted feet skated with it.
        if (cur > 0.3f && glm::dot(dir, dir) > 0.25f) {
            const glm::vec3 was = flatVel / cur;
            const float turn = std::atan2(was.x * dir.z - was.z * dir.x, glm::dot(was, dir)), most = glm::radians(kTravelTurnRate) * dt;
            if (std::abs(turn) > most) {
                const float a = std::copysign(most, turn), c = std::cos(a), s = std::sin(a);
                dir = glm::vec3(was.x * c - was.z * s, 0.0f, was.x * s + was.z * c);
            }
        }
        float target = n.WantMove && crowd > 0.05f ? n.PaceSpeed : 0.0f;
        // Turn first: held (no pace, no wish) while the body turns on the spot to the way it is going.
        if (target > 0.0f && n.Body.MotionMatching() && (n.TurningFirst || cur < kTurnFirstFrom)) {
            const float off = std::abs(std::remainder(NpcYawOf(dir, n.Body.Yaw()) - n.Body.Yaw(), 6.2831853f));
            if (!n.TurningFirst && off > glm::radians(kTurnFirstOver)) { n.TurningFirst = true; n.TurnFirstTime = 0.0f; }
            if (n.TurningFirst) {
                n.TurnFirstTime += dt;
                if (off < m_HoldSettings.TurnThreshold || n.TurnFirstTime > kTurnFirstMost) n.TurningFirst = false;
            }
        } else {
            n.TurningFirst = false;
        }
        if (n.TurningFirst) target = 0.0f;
        bool braking = target <= 0.0f;
        if (target > 0.0f && n.Agent >= 0) {
            glm::vec3 corner = m_Crowd.NextCorner(n.Agent) - n.MoveTarget;
            const float remain = glm::length(glm::vec2(n.MoveTarget.x - n.Feet.x, n.MoveTarget.z - n.Feet.z));
            if (glm::length(glm::vec2(corner.x, corner.z)) < 0.3f) { // the last leg of the route: the stop is on it
                const float stopAt = std::max(0.0f, remain - kArriveSlack) / kStopTime;
                if (stopAt < target) { target = stopAt; braking = true; }
            } else {
                target = std::min(target, std::max(crowd, 0.5f * n.PaceSpeed)); // round a corner the crowd eases the pace a little
            }
        }
        const float tau = target > cur ? kStartTime : kStopTime;
        const float pace = target + (cur - target) * std::exp(-dt / tau);
        want = dir * (pace < 0.02f ? 0.0f : pace);
        if (!braking) wish = dir * n.PaceSpeed;
    }
    n.FallSpeed -= m_Cfg.FallGravity * dt;
    const glm::vec3 disp = (want + n.PushVel) * dt + glm::vec3(0.0f, n.FallSpeed * dt, 0.0f);
    n.PushVel *= std::exp(-7.0f * dt); // a stagger's shove dies away
    const float d[3] = {disp.x, disp.y, disp.z};
    const unsigned hit = PhysicsWorld::MoveNpcCharacter(n.Cct, d, dt);
    if (hit & PhysicsWorld::CC_DOWN) n.FallSpeed = -1.0f;
    n.OnFloor = (hit & PhysicsWorld::CC_DOWN) != 0;
    float feet[3], r = 0.0f, cyl = 0.0f;
    if (PhysicsWorld::GetNpcCapsule(n.Cct, feet, &r, &cyl)) {
        const glm::vec3 f(feet[0], feet[1], feet[2]);
        const glm::vec3 moved = f - n.Feet;
        n.Velocity = glm::vec3(moved.x, 0.0f, moved.z) / dt;
        n.Feet = f;
    }
    if (n.Agent >= 0) m_Crowd.Sync(n.Agent, n.Feet);
    // Getting stuck: asked to move, barely moving.
    if (n.WantMove && glm::length(want) > 0.5f && glm::length(n.Velocity) < 0.25f) n.BlockedTime += dt;
    else n.BlockedTime = std::max(0.0f, n.BlockedTime - dt * 2.0f);

    // The body.
    NpcBodyInput in;
    in.Feet = n.Feet;
    in.Velocity = n.Velocity;
    in.Wish = wish;
    in.Grounded = n.OnFloor;
    in.FallSpeed = std::max(0.0f, -n.FallSpeed);
    in.AccelTime = kStartTime;
    in.DecelTime = kStopTime;
    in.FacingYaw = NpcYawOf(n.LookPoint - n.Feet, n.Body.Yaw());
    if (n.TurningFirst) in.FacingYaw = NpcYawOf(m_Crowd.Velocity(n.Agent), in.FacingYaw); // turning first: to the way it is going
    in.LookPoint = n.LookPoint;
    in.Sprint = n.Pace == Gait::Run && n.WantMove;
    in.FootIK = FootIKEverywhere || (n.PlayerDist < m_Cfg.FootIKRange && n.OnScreen); // feet on slopes and steps where they can be seen
    in.Style = StyleFor(n.Pace);
    // The trajectory follows the route's corners (the crowd's), not the straight wish - while it goes on, not once it stops.
    if (n.WantMove && n.Agent >= 0 && glm::dot(wish, wish) > 1e-4f) in.PathCount = m_Crowd.Corners(n.Agent, in.Path, 4, &in.PathArrives);
    n.Body.Tick(world, in, dt);
}

void NpcDirector::LateUpdate(World& world, float dt, const PlayerSnapshot& p) {
    if (!m_Active || !m_Started) return;
    const auto t0 = std::chrono::steady_clock::now();
    for (auto& up : m_Npcs) {
        if (!up) continue;
        Npc& n = *up;
        if (n.Dead) {
            if (n.DeathPending) {
                // One last animated frame, then the ragdoll takes the pose the body died in.
                n.LateDt += dt;
                if (n.Animate) { // (a held frame has no fresh pose to solve on: the ragdoll takes the one it has)
                    // (The flinch of the rounds before stays on that pose - LatePose kicks it - and the ragdoll takes it from there.)
                    LatePose(world, n, n.LateDt);
                    // The pose it was hit in (Kill: the last late pose's, n.LastPose) is n.LateDt older than this one.
                    m_DeathBones[n.Index].Dt = n.LateDt;
                    n.LateDt = 0.0f;
                }
                m_Flinch.erase(n.Index);
                FinishDeath(world, n);
            }
            if (n.Ragdoll) {
                SubTimer timer(*this, SubRagdoll);
                n.Ragdoll->Update(dt);
            }
            if (n.FallSoundAt > 0.0f && m_Now >= n.FallSoundAt) {
                n.FallSoundAt = -1.0f;
                if (Fx) Fx->Play(CombatFx::Cue::BodyFall, n.Ragdoll ? n.Ragdoll->Root() : n.Feet);
            }
            continue;
        }
        n.LateDt += dt;
        if (!n.Animate) {
            // Held this frame (animation LOD): the pose stays, the hitboxes ride the body.
            UpdateHitboxes(n, p, /*posed=*/false);
            continue;
        }
        const float lateDt = n.LateDt;
        n.LateDt = 0.0f;
        if (const auto it = m_Flinch.find(n.Index); it != m_Flinch.end() && !it->second.Active(m_Now)) m_Flinch.erase(it); // settled
        LatePose(world, n, lateDt);
        // Footsteps on the posed feet's touch-downs.
        if (float feet[2]; n.Body.FootHeights(feet))
            FoleyAudio::Get().NpcFeet(world, n.Index, n.Feet, feet, n.Body.Sprinting(), glm::length(glm::vec2(n.Velocity.x, n.Velocity.z)), lateDt);
        UpdateHitboxes(n, p, /*posed=*/true);
        // The finished pose, as drawn (the flinch in it): a kill before the next late pose - a round of the player's, which comes
        // after this frame's animators - takes it as the pose the body had a frame (n.LateDt) before it died (Kill).
        NpcRagdoll::Capture(n.Body, n.LastPose);
        if (TrackDeathPop) {
            static const char* kBones[3] = {"head", "hand_l", "foot_l"};
            n.HaveLastBones = true;
            for (int k = 0; k < 3; ++k) n.Body.BoneWorld(kBones[k], n.LastBones[k]);
        }
    }
    m_LateMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    FlushCosts(false);
}

// The dummy's late pose: a hit flinch's kick, foot IK, the look, the pose onto every piece; the eye.
// The flinch goes on the driver before its foot pass (NpcBody::LateUpdate): a leg hit bends the knee over a planted foot instead of
// sliding it, and the driver's pose carries the kick to every piece (one skeleton: the arms piece kicks with the torso).
void NpcDirector::LatePose(World& world, Npc& n, float dt) {
    {
        SubTimer timer(*this, SubBody);
        NpcBody::PoseKick kick;
        if (const auto it = m_Flinch.find(n.Index); it != m_Flinch.end() && it->second.Active(m_Now)) {
            const NpcFlinch* flinch = &it->second;
            const float now = m_Now;
            kick = [flinch, now](const Model& m, std::vector<LocalTRS>& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals) {
                flinch->ApplyPose(m, pose, parents, globals, now);
            };
        }
        n.Body.LateUpdate(world, dt, kick);
    }
    if (glm::vec3 feet[2]; IK::FootSlideMeterOn() && n.Body.FootWorld(feet)) IK::FootSlideMeterFor(n.Name).Step(feet, n.Feet.y, dt);
    n.Eye = n.Body.Eye();
}

// Per dummy: how often its pose is worked out. Near and in view: every frame. Further (25 m) every second, past 50 m or out
// of view every fourth, staggered between dummies. Just hurt or just spawned stays at full rate. The skipped frames keep the
// last pose; time is kept for the next one.
void NpcDirector::UpdateLod(World& world, Npc& n, const PlayerSnapshot& p) {
    int lod = 1;
    float dist = 0.0f;
    bool onScreen = true;
    if (p.Valid) {
        const glm::vec3 to = n.Feet + glm::vec3(0.0f, 0.9f, 0.0f) - p.Eye;
        dist = glm::length(to);
        // The view's cone, roughly: a wide FOV and a margin for the body's size.
        const float margin = std::atan2(1.6f, std::max(dist, 0.5f));
        onScreen = dist < 3.0f || std::acos(std::clamp(glm::dot(to / std::max(dist, 1e-4f), p.Forward), -1.0f, 1.0f)) < glm::radians(62.0f) + margin;
        if (!NoLod) lod = !onScreen || dist > 50.0f ? 4 : dist > 25.0f ? 2 : 1;
    }
    n.Lod = lod;
    n.PlayerDist = dist;
    n.OnScreen = onScreen;
    const bool busy = m_Now - n.LastHurt < 0.5f || m_Now - n.SpawnedAt < 0.5f;
    n.Animate = busy || lod == 1 || ((m_Frame + n.Index) % lod) == 0;
    // (Any Animator Controller left on a piece - a prefab from before Phase 6 - is always held: the matcher poses the body, stepping
    // on the frames posed - LatePose.)
    for (entt::entity e : n.Body.Pieces())
        if (e != entt::null && world.Registry.valid(e))
            if (auto* ac = world.Registry.try_get<AnimatorControllerComponent>(e)) ac->SkipUpdate = true;
}

// The per-bone hitboxes follow the skeleton for dummies within Hitbox Range of the player (made on first need, off beyond
// it, where the capsule answers instead).
void NpcDirector::UpdateHitboxes(Npc& n, const PlayerSnapshot& p, bool posed) {
    if (n.Cct == PhysicsWorld::kNoCharacter) return;
    const bool near = !p.Valid || glm::length(n.Feet - p.Feet) < m_Cfg.HitboxRange;
    if (!near) {
        if (n.Hitboxes) n.Hitboxes->SetActive(false);
        return;
    }
    SubTimer timer(*this, SubHitbox);
    if (!n.Hitboxes) {
        if (n.HitboxTries >= 120) return; // no usable skeleton
        ++n.HitboxTries;
        auto h = std::make_unique<NpcHitboxes>();
        if (h->Start(n.Body, n.Cct)) n.Hitboxes = std::move(h);
        return;
    }
    n.Hitboxes->SetActive(true);
    if (posed) n.Hitboxes->Pose(n.Body);
    else n.Hitboxes->Follow(n.Body);
}

bool NpcDirector::OnPlayerHit(World& world, unsigned entity, const glm::vec3& point, const glm::vec3& origin, const glm::vec3& dir,
                              const FirstPersonWeaponGameplay& weapon, bool* killed, bool* head, bool* aliveWhenHit) {
    if (killed) *killed = false;
    if (head) *head = false;
    if (aliveWhenHit) *aliveWhenHit = false;
    if (!m_Active) return false;
    for (auto& up : m_Npcs) {
        if (!up || Id(up->Root) != entity) continue;
        Npc& n = *up;
        if (aliveWhenHit) *aliveWhenHit = !n.Dead;
        // Which bone: a ray down the round's line, against the hitboxes (or, on a corpse, the ragdoll's parts).
        int part = -1;
        {
            const glm::vec3 d = glm::length(dir) > 1e-6f ? glm::normalize(dir) : glm::normalize(point - origin + glm::vec3(1e-6f));
            const float o[3] = {origin.x, origin.y, origin.z}, dd[3] = {d.x, d.y, d.z};
            PhysicsWorld::BodyPartHit bp;
            if (PhysicsWorld::RaycastBodyParts(o, dd, glm::length(point - origin) + 0.2f, bp) && bp.Entity == entity) part = bp.Part;
        }
        const float dist = glm::length(point - origin);
        if (n.Dead) {
            // A corpse shot: the part it hit takes a shove, wakes, and shows it.
            if (n.Ragdoll) {
                if (part < 0) {
                    float best = 1e9f;
                    for (int i = 0; i < NpcRagdoll::kRagParts; ++i) {
                        const float d2 = glm::length(n.Ragdoll->PartPosition(i) - point);
                        if (d2 < best) { best = d2; part = i; }
                    }
                }
                const float amount = DamageForHit(weapon, HitZone::Torso, dist);
                const glm::vec3 d = glm::length(dir) > 1e-6f ? glm::normalize(dir) : glm::vec3(0.0f, 0.0f, 1.0f);
                n.Ragdoll->HitCorpse(part, d, amount, point, m_RagdollCfg);
                if (Fx) Fx->Play(CombatFx::Cue::FleshHit, point, false, 0.5f);
                if (m_FleshHits.size() < 64)
                    m_FleshHits.push_back({point, d, entity, part, amount, false, part == 2, true, weapon.Pellets, origin, true});
            }
            return true;
        }
        if (part >= NpcRagdoll::kParts) part = -1; // (alive, only the hitboxes' eleven answer)
        const HitZone zone = part >= 0 ? ZoneOfRegion(RegionFromPart(part)) : ZoneFromCapsuleHeight(point.y, n.Feet.y, 1.8f);
        const float dmg = DamageForHit(weapon, zone, dist);
        if (Fx) Fx->Play(CombatFx::Cue::FleshHit, point, false, 0.7f);
        ApplyDamage(world, n, dmg, zone, point, dir, part);
        if (killed) *killed = n.Dead;
        if (head) *head = zone == HitZone::Head;
        if (m_FleshHits.size() < 64) {
            const glm::vec3 d = glm::length(dir) > 1e-6f ? glm::normalize(dir) : glm::vec3(0.0f, 0.0f, 1.0f);
            m_FleshHits.push_back({point, d, entity, part, dmg, n.Dead, zone == HitZone::Head, false, weapon.Pellets, origin, true});
        }
        return true;
    }
    return false;
}

void NpcDirector::ApplyDamage(World& world, Npc& n, float amount, HitZone zone, const glm::vec3& point, const glm::vec3& dir, int part) {
    if (n.Dead || amount <= 0.0f) return;
    // Its Health component holds the hit points (and may make it invulnerable: the hit registers, nothing else).
    auto* health = world.Registry.try_get<HealthComponent>(n.Root);
    if (health) {
        if (health->Invulnerable) return;
        n.Health = health->Current;
        n.MaxHealth = health->Max;
    }
    n.Health -= amount;
    n.LastHurt = m_Now;
    if (health) health->Current = std::max(0.0f, n.Health);
    // Dead: out of health, or a head shot that is enough.
    if (n.Health <= 0.0f || (zone == HitZone::Head && amount >= 60.0f)) {
        Kill(world, n, dir, point, 30.0f + 0.4f * amount, part);
        return;
    }
    // It flinches the way it was hit (where it was hit: a procedural kick on the pose, LatePose) ...
    if (m_RagdollCfg.HitFlinch && part >= 0) m_Flinch[n.Index].Hit(n.Body, part, dir, amount, m_Now, m_RagdollCfg); // a kick on the struck region
    // ... and a heavy hit slows it a beat and shoves it back along the round.
    if (amount >= m_Cfg.HeavyHitDamage) {
        n.StaggerUntil = m_Now + m_Cfg.StaggerTime;
        const glm::vec3 flat(dir.x, 0.0f, dir.z);
        if (glm::dot(flat, flat) > 1e-6f) n.PushVel += glm::normalize(flat) * 1.8f;
        if (glm::length(n.PushVel) > 2.5f) n.PushVel = glm::normalize(n.PushVel) * 2.5f;
    }
}

void NpcDirector::Kill(World& world, Npc& n, const glm::vec3& dir, const glm::vec3& point, float shove, int part) {
    if (n.Dead) return;
    n.Dead = true;
    m_Scripted.erase(n.Index);
    if (auto* h = world.Registry.try_get<HealthComponent>(n.Root)) h->Current = 0.0f;
    n.Health = 0.0f;
    n.DiedAt = m_Now;
    n.WantMove = false;
    n.Hitboxes.reset(); // the ragdoll's parts take over the rounds
    n.DiedFlinching = FlinchActive(n.Index); // (tests: the death pop's log; the flinch goes with the last animated frame, LateUpdate)
    if (n.Cct != PhysicsWorld::kNoCharacter) PhysicsWorld::DestroyNpcCharacter(n.Cct);
    n.Cct = PhysicsWorld::kNoCharacter;
    if (n.Agent >= 0) m_Crowd.Remove(n.Agent);
    n.Agent = -1;
    // The body goes down in this frame's late pose (LateUpdate -> FinishDeath): the animators run one more frame, so the ragdoll
    // starts from the pose it was seen in.
    n.DeathPending = true;
    // For the limbs' velocity: the last late pose's finished pose (LateUpdate's n.LastPose), not the bones as they are now - a
    // player's round lands after this frame's animators, when the bones already hold this frame's clip pose without its foot IK,
    // look and flinch. Its age (Dt) is set with the dying frame's late pose; none (Valid false, a dummy killed before it was ever
    // posed): every part takes the body's velocity.
    m_DeathBones[n.Index].Bones = n.LastPose;
    m_DeathBones[n.Index].Dt = 0.0f;
    n.DeathDir = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec3(0.0f, 0.0f, 1.0f);
    n.DeathPoint = point;
    n.DeathShove = shove;
    n.DeathPart = part;
    if (m_Cfg.Respawn && !n.DevSpawned) m_RespawnTimers.push_back({m_Now + m_Cfg.RespawnDelay, n.SpawnIndex});
}

void NpcDirector::FinishDeath(World& world, Npc& n) {
    PROFILE_SCOPE("AI Death");
    n.DeathPending = false;
    // Down: the animators stop and the physics takes the body, shoved along the round's line on the bone it struck.
    for (entt::entity e : n.Body.Pieces())
        if (world.Registry.valid(e)) world.Registry.remove<AnimatorControllerComponent>(e);
    n.Body.SetPoseOwnedElsewhere(true);
    n.FallSoundAt = m_Now + 0.45f;
    n.Ragdoll = std::make_unique<NpcRagdoll>();
    // The bones' velocity: the finished pose of the late pose before the dummy died (Kill: n.LastPose) against the dying frame's,
    // over the real time between (LOD-held frames included; a held dying frame has no new pose: no relative motion, just the body's).
    if (!n.Ragdoll->Start(n.Body, n.Velocity, n.DeathDir * n.DeathShove, n.DeathPoint, n.DeathPart, &m_RagdollCfg, &m_DeathBones[n.Index].Bones,
                          m_DeathBones[n.Index].Dt)) {
        n.Ragdoll.reset();
        m_DeathBones.erase(n.Index);
        if (n.Root != entt::null && world.Registry.valid(n.Root)) {
            world.Registry.emplace_or_replace<DeactivatedTag>(n.Root);
            world.SyncActiveInHierarchy();
        }
        return;
    }
    m_DeathBones.erase(n.Index);
    n.Ragdoll->Update(0.0f); // the pieces take the parts' first pose now
    if (TrackDeathPop && n.HaveLastBones) {
        static const char* kBones[3] = {"head", "hand_l", "foot_l"};
        float pop = 0.0f;
        for (int k = 0; k < 3; ++k) {
            glm::vec3 b;
            if (!n.Body.BoneWorld(kBones[k], b)) continue;
            const float d = glm::length(b - n.LastBones[k]);
            pop = std::max(pop, d);
            if (d > 0.15f) // which bone, for the test's log
                std::printf("[NpcTest] %s death pop: %s %.0f cm (last animated (%.2f %.2f %.2f), ragdoll (%.2f %.2f %.2f))%s\n", n.Name.c_str(),
                            kBones[k], d * 100.0f, n.LastBones[k].x, n.LastBones[k].y, n.LastBones[k].z, b.x, b.y, b.z,
                            n.DiedFlinching ? ", mid-flinch" : "");
        }
        n.DeathPop = pop;
    }
}

bool NpcDirector::PlayerSees(const PlayerSnapshot& p, const glm::vec3& point) {
    if (!p.Valid) return false;
    glm::vec3 d = point - p.Eye;
    const float dist = glm::length(d);
    if (dist < 2.0f) return true;   // at the player's feet: as good as seen (it would vanish at the screen's edge)
    if (dist > 250.0f) return false;
    d /= dist;
    // A cone a little wider than any screen's view (the camera's field of view isn't the director's to know).
    const float fl = glm::length(p.Forward);
    if (fl > 1e-4f && glm::dot(d, p.Forward / fl) < std::cos(glm::radians(62.0f))) return false;
    // Not behind solid geometry (the world's static and kinematic colliders; bodies and ragdolls don't hide it).
    QueryFilter f;
    f.HitTriggers = 0;
    RaycastHit hit;
    const float o[3] = {p.Eye.x, p.Eye.y, p.Eye.z}, v[3] = {d.x, d.y, d.z};
    return !(PhysicsWorld::RaycastSolid(o, v, dist, f, hit) && hit.Hit && hit.Distance < dist - 0.25f);
}

// Corpses lie at least Corpse Time and go only once the player hasn't seen them for Unseen Time; past Max Corpses the oldest
// unseen go first, whatever their age.
void NpcDirector::UpdatePersistence(World& world, const PlayerSnapshot& p) {
    std::vector<Npc*> corpses;
    for (auto& up : m_Npcs)
        if (up && up->Dead && !up->DeathPending) corpses.push_back(up.get());
    std::sort(corpses.begin(), corpses.end(), [](const Npc* a, const Npc* b) { return a->DiedAt < b->DiedAt; });
    const int cap = std::max(m_Cfg.MaxCorpses, 1);
    int over = (int)corpses.size() - cap;
    for (Npc* n : corpses) {
        bool seen = false;
        if (n->Ragdoll) {
            for (int part : {0, 1, 2}) if (!seen) seen = PlayerSees(p, n->Ragdoll->PartPosition(part));
        } else {
            seen = PlayerSees(p, n->Feet + glm::vec3(0.0f, 0.5f, 0.0f));
        }
        if (seen) n->UnseenSince = -1.0f;
        else if (n->UnseenSince < 0.0f) n->UnseenSince = m_Now;
        const float unseenFor = seen ? 0.0f : m_Now - n->UnseenSince;
        if (unseenFor < m_Cfg.UnseenTime || (over <= 0 && m_Now - n->DiedAt <= m_Cfg.CorpseTime)) continue;
        --over;
        Despawn(world, *n, /*keepBody=*/true);
        for (auto& up : m_Npcs)
            if (up.get() == n) { up.reset(); break; }
    }
}

// A dead dummy comes back at its own spawn after Respawn Delay (not while the player stands on it: a second later).
void NpcDirector::Respawns(World& world, AssetLibrary& assets) {
    for (size_t i = 0; i < m_RespawnTimers.size();) {
        PendingRespawn& r = m_RespawnTimers[i];
        if (m_Now < r.At) { ++i; continue; }
        if (r.SpawnIndex < 0 || r.SpawnIndex >= (int)m_Spawns.size()) { m_RespawnTimers.erase(m_RespawnTimers.begin() + (long long)i); continue; }
        if (Spawn(world, assets, r.SpawnIndex) >= 0) m_RespawnTimers.erase(m_RespawnTimers.begin() + (long long)i);
        else r.At = m_Now + 1.0f;
    }
}

std::vector<NpcDirector::FleshHit> NpcDirector::TakeFleshHits() {
    std::vector<FleshHit> out;
    out.swap(m_FleshHits);
    return out;
}

void NpcDirector::DebugLines(std::vector<float>& out) const {
    auto line = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec4& c) {
        out.insert(out.end(), {a.x, a.y, a.z, c.r, c.g, c.b, c.a, b.x, b.y, b.z, c.r, c.g, c.b, c.a});
    };
    for (const auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        const Npc& n = *up;
        line(n.Eye, n.LookPoint, glm::vec4(0.6f, 1.0f, 0.6f, 0.6f));
        if (n.HasGoal) line(n.Feet + glm::vec3(0, 0.1f, 0), n.Goal + glm::vec3(0, 0.1f, 0), glm::vec4(0.3f, 0.8f, 1.0f, 0.9f));
    }
}
