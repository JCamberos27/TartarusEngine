#include "NpcDirector.h"

#include "AssetLibrary.h"
#include "Combat/CombatFx.h"
#include "Components.h"
#include "FirstPersonPresentation.h"
#include "GameModuleAPI.h" // RaycastHit, QueryFilter, kPlayerEntity
#include "Log.h"
#include "NpcBrain.h"
#include "Profiler.h"
#include "PhysicsWorld.h"
#include "ProjectPaths.h"
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

constexpr const char* kSoldierPath = "assets/AI/Soldier.json";
constexpr const char* kAkPath = "assets/Weapons/AKS74U/AKS74U.fpsanim";
constexpr const char* kRemingtonPath = "assets/Weapons/Remington870/Remington870.fpsanim";
constexpr float kRadius = 0.3f;
constexpr float kStandCyl = 0.6f;    // capsule 1.8 m standing
constexpr float kCrouchCyl = 0.33f;  // 1.26 m crouched

unsigned Id(entt::entity e) { return (unsigned)entt::to_integral(e); }

glm::vec3 FrontOf(float yawDeg, float pitchDeg) {
    const float y = glm::radians(yawDeg), p = glm::radians(pitchDeg);
    return glm::vec3(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p));
}

void YawPitchOf(const glm::vec3& d, float& yawDeg, float& pitchDeg) {
    const float len = glm::length(d);
    if (len < 1e-5f) return;
    const glm::vec3 n = d / len;
    yawDeg = glm::degrees(std::atan2(n.z, n.x));
    pitchDeg = glm::degrees(std::asin(std::clamp(n.y, -1.0f, 1.0f)));
}

float WrapDeg(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

float SpeedFor(Gait g, bool crouch) {
    if (crouch) return g == Gait::Still ? 0.0f : 1.9f;
    switch (g) {
    case Gait::Still: return 0.0f;
    case Gait::Walk: return 1.5f;
    case Gait::Jog: return 3.2f;
    case Gait::Run: return 4.7f;
    }
    return 3.2f;
}

// A soldier's tree shown or hidden: its own checkbox on the root, the derived flag on everything under it (set here rather
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

const char* BehaviourName(Behaviour b) {
    switch (b) {
    case Behaviour::Idle: return "Idle";
    case Behaviour::Investigate: return "Investigate";
    case Behaviour::Engage: return "Engage";
    case Behaviour::TakeCover: return "Take Cover";
    case Behaviour::CoverFight: return "Cover Fight";
    case Behaviour::Flank: return "Flank";
    case Behaviour::Push: return "Push";
    case Behaviour::Search: return "Search";
    case Behaviour::Retreat: return "Retreat";
    case Behaviour::Dead: return "Dead";
    case Behaviour::Wounded: return "Wounded";
    }
    return "?";
}

const char* RoleName(NpcRole r) {
    switch (r) {
    case NpcRole::Anchor: return "Anchor";
    case NpcRole::Suppressor: return "Suppressor";
    case NpcRole::Flanker: return "Flanker";
    }
    return "?";
}

const char* NpcDirector::SubName(int s) {
    static const char* kNames[SubCount] = {"AI Perceive", "AI Brain", "AI Squads", "AI Move", "AI Aim+Fire",
                                           "AI Body", "AI Weapon", "AI Hold", "AI Ragdoll", "AI Hitbox"};
    return s >= 0 && s < SubCount ? kNames[s] : "AI ?";
}

struct NpcDirector::SoldierBody {
    entt::entity Root = entt::null;
    std::vector<std::pair<entt::entity, AnimatorControllerComponent>> Animators; // as built: each soldier starts from these
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
    const int first = think ? SubPerceive : SubBody, last = think ? SubAimFire : SubHitbox;
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

// The scene's Squad Settings and Ragdoll Settings (the first of each; the defaults without one).
void NpcDirector::ApplySettings(const entt::registry& reg) {
    if (auto settings = reg.view<const SquadSettingsComponent>(); settings.begin() != settings.end()) {
        const auto& s = reg.get<const SquadSettingsComponent>(*settings.begin());
        m_SquadSize = std::clamp(s.SquadSize, 0, 8);
        m_RespawnDelay = s.RespawnDelay;
        m_Difficulty = s.Difficulty;
        m_DamageScale = s.NpcDamageScale;
        m_Respawn = s.Respawn;
        m_Cfg = s;
    } else {
        m_Cfg = SquadSettingsComponent();
    }
    if (auto rag = reg.view<const RagdollSettingsComponent>(); rag.begin() != rag.end()) m_RagdollCfg = reg.get<const RagdollSettingsComponent>(*rag.begin());
    else m_RagdollCfg = RagdollSettingsComponent();
}

bool NpcDirector::Start(World& world, AssetLibrary& assets, const FirstPersonControllerComponent* playerConfig) {
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
        s.Weapon = c.Weapon;
        s.Squad = c.Squad;
        s.Skill = c.Skill;
        s.Brain = c.Brain;
        s.OutfitSeed = c.OutfitSeed;
        const auto* order = reg.try_get<OrderComponent>(e);
        found.push_back({order ? order->Value : (int)entt::to_integral(e), s});
    }
    if (found.empty()) return false;
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& f : found) m_Spawns.push_back(f.second);
    ApplySettings(reg);
    // The soldier's body, read once.
    {
        std::ifstream in(std::filesystem::u8path(ProjectPaths::Resolve(kSoldierPath)));
        if (!in) {
            Log::Error(std::string("Enemy AI: can't read ") + kSoldierPath + " - run tools/gen_npc_soldier.py.");
            m_Spawns.clear();
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        m_SoldierJson = ss.str();
    }
    m_ViewConfig = std::make_shared<FirstPersonControllerComponent>();
    if (playerConfig) *m_ViewConfig = *playerConfig;
    m_ViewConfig->SecondaryAnimationSet.clear();
    m_ViewConfig->GravityGun = false;
    if (m_ViewConfig->CameraBone.empty()) m_ViewConfig->CameraBone = "head";
    // The soldiers are the player's body holding the player's rigs: they hold them by the player's numbers.
    m_HoldSettings = NpcHoldSettings{};
    if (auto bodies = world.Registry.view<FirstPersonBodyComponent>(); bodies.begin() != bodies.end()) {
        const auto& fpb = bodies.get<FirstPersonBodyComponent>(*bodies.begin());
        m_HoldSettings.ClavicleFollow = fpb.ClavicleFollow;
        m_HoldSettings.ShoulderMaxAngle = fpb.ShoulderMaxAngle;
        m_HoldSettings.ShrugStart = fpb.ShrugStart;
        m_HoldSettings.ShrugMax = fpb.ShrugMax;
        m_HoldSettings.ReachLeanMax = fpb.ReachLeanMax;
        m_HoldSettings.ElbowClearance = fpb.ElbowClearance;
        m_HoldSettings.ReachSlack = fpb.ReachSlack;
        m_HoldSettings.ShoulderLineMatch = fpb.ShoulderLineMatch;
        m_HoldSettings.SpineAim = fpb.SpineAim;
        m_HoldSettings.SpineAimDown = fpb.SpineAimDown;
        m_HoldSettings.ArmedEyeOffset = fpb.ArmedEyeOffset;
        m_HoldSettings.HeadBob = fpb.HeadBob;
        m_HoldSettings.CameraSmoothing = fpb.CameraSmoothing;
        m_HoldSettings.EyeSlack = fpb.EyeSlack;
        m_HoldSettings.LookDownPush = fpb.LookDownPush;
        m_HoldSettings.LookDownStart = fpb.LookDownStart;
        // NPC body tuning
        m_HoldSettings.TurnThreshold = glm::radians(fpb.NpcTurnThreshold); // degrees in the component
        m_HoldSettings.MoveEase = fpb.NpcMoveEase;
        m_HoldSettings.FaceEase = fpb.NpcFaceEase;
        m_HoldSettings.MaxTwist = glm::radians(fpb.NpcMaxTwist); // degrees in the component
        m_HoldSettings.AimLean = glm::radians(fpb.NpcAimLean); // degrees in the component
        m_HoldSettings.AimLeanCrouched = glm::radians(fpb.NpcAimLeanCrouched); // degrees in the component
        m_HoldSettings.ReadyLeanCrouched = glm::radians(fpb.NpcReadyLeanCrouched); // degrees in the component
        m_HoldSettings.CowerHunch = glm::radians(fpb.NpcCowerHunch); // degrees in the component
        m_HoldSettings.HeadMaxYaw = glm::radians(fpb.NpcHeadMaxYaw); // degrees in the component
        m_HoldSettings.HeadMaxPitch = glm::radians(fpb.NpcHeadMaxPitch); // degrees in the component
        m_HoldSettings.FootIKMaxDrop = fpb.NpcFootIKMaxDrop;
        m_HoldSettings.FootIKMaxRaise = fpb.NpcFootIKMaxRaise;
        m_HoldSettings.FootIKPelvisRaise = fpb.NpcFootIKPelvisRaise;
        m_HoldSettings.FootIKTiltMax = glm::radians(fpb.NpcFootIKTiltMax); // degrees in the component
        m_HoldSettings.FootOffsetEase = fpb.NpcFootOffsetEase;
        m_HoldSettings.FootNormalEase = fpb.NpcFootNormalEase;
        m_HoldSettings.FootIKFade = fpb.NpcFootIKFade;
        m_HoldSettings.Spine = fpb.Spine; // per-bone spine weights and limits
    }
    m_Active = true;
    m_Started = false;
    m_Now = 0.0f;
    m_Voice.Reset();
    m_PlayerWasDead = false;
    if (!m_Voice.Loaded()) m_Voice.LoadManifest(ProjectPaths::Resolve("assets/Audio/Voice/combine"));
    Log::Info("Enemy AI: " + std::to_string(m_Spawns.size()) + " spawn(s), squad of " + std::to_string(m_SquadSize) + ".");
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
    m_Squads.clear();
    m_RespawnTimers.clear();
    m_Crowd.Clear();
    m_Nav.Clear();
    m_Cover.Clear();
    m_Noises.clear();
    m_PlayerDamage.clear();
    m_Impacts.clear();
    m_Ejections.clear();
    m_Spawns.clear();
    m_SoldierJson.clear();
    m_ViewConfig.reset();
    m_PlayerAgent = -1;
    m_Active = m_Started = false;
    m_ShootersNow = m_MaxShooters = 0;
    m_NextName = 1;
    m_Voice.Reset();
}

void NpcDirector::BuildNav(World& world) {
    (void)world;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<float> verts;
    std::vector<int> tris;
    PhysicsWorld::CollectStaticGeometry(verts, tris, /*includeKinematic=*/false);
    NavBuildSettings s;
    const std::uint64_t hash = NavMesh::HashInput(verts, tris, s);
    char name[64];
    std::snprintf(name, sizeof name, "Library/NavCache/%016llx.nav", (unsigned long long)hash);
    const std::string cache = ProjectPaths::Resolve(name);
    bool loaded = m_Nav.Load(cache, hash);
    std::string error;
    if (!loaded) {
        if (!m_Nav.Build(verts, tris, s, &error)) {
            Log::Error("Enemy AI: navigation mesh failed (" + error + ") - the soldiers can't move.");
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(cache).parent_path(), ec);
        m_Nav.Save(cache, hash);
    }
    const int cover = m_Cover.Build(m_Nav, CoverTuning{m_Cfg.CoverSpacing, m_Cfg.CoverReach, m_Cfg.CoverKneeHeight, m_Cfg.CoverHeadHeight, m_Cfg.CoverStep});
    m_Crowd.Init(m_Nav, 16, 0.6f);
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    char msg[200];
    std::snprintf(msg, sizeof msg, "Enemy AI: navigation mesh %s (%d polygons from %d triangles), %d cover points, %.0f ms.",
                  loaded ? "loaded" : "built", m_Nav.PolyCount(), (int)tris.size() / 3, cover, ms);
    Log::Info(msg);
}

bool NpcDirector::LateStart(World& world, AssetLibrary& assets) {
    m_Started = true;
    BuildNav(world);
    // Both guns' spent cases now, with the navigation mesh: a soldier carrying the other gun spawning mid-fight would
    // otherwise import its case's mesh then.
    for (const char* set : {kAkPath, kRemingtonPath}) FirstPersonPresentation::WarmEjectAssets(assets, set);
    if (m_Player.Valid) m_PlayerAgent = m_Crowd.Add(m_Player.Feet, m_Player.Radius, m_Player.Height, 6.0f, /*steer=*/false);
    const int want = std::min<int>(m_SquadSize, (int)m_Spawns.size() * 2);
    for (int i = 0; i < want; ++i) Spawn(world, assets, i % (int)m_Spawns.size());
    // Two spare bodies, built now and laid out of the way: the first replacements come before any corpse has gone.
    for (int i = 0; i < 2 && m_Respawn && !m_SoldierJson.empty(); ++i)
        if (auto spare = BuildBody(world, assets)) {
            for (const auto& [e, ac] : spare->Animators)
                if (world.Registry.valid(e)) world.Registry.remove<AnimatorControllerComponent>(e);
            SetTreeActive(world, spare->Root, false);
            m_Pool.push_back(std::move(spare));
        }
    return true;
}

std::shared_ptr<NpcDirector::SoldierBody> NpcDirector::BuildBody(World& world, AssetLibrary& assets) {
    std::vector<entt::entity> created;
    if (!SceneSerializer::AppendEntitiesFromString(world, assets, m_SoldierJson, created) || created.empty()) {
        Log::Error("Enemy AI: couldn't build a soldier from Soldier.json.");
        return nullptr;
    }
    auto& reg = world.Registry;
    auto built = std::make_shared<SoldierBody>();
    for (entt::entity e : created) {
        const auto* h = reg.try_get<HierarchyComponent>(e);
        if (built->Root == entt::null && (!h || h->Parent == entt::null)) built->Root = e;
        if (const auto* ac = reg.try_get<AnimatorControllerComponent>(e)) built->Animators.push_back({e, *ac});
    }
    if (built->Root == entt::null) {
        for (entt::entity e : created)
            if (reg.valid(e)) world.DestroyEntityAndChildren(e);
        return nullptr;
    }
    return built;
}

int NpcDirector::Spawn(World& world, AssetLibrary& assets, int spawnIndex) {
    if (spawnIndex < 0 || spawnIndex >= (int)m_Spawns.size() || m_SoldierJson.empty()) return -1;
    const SpawnPoint& sp = m_Spawns[(size_t)spawnIndex];
    const auto spawnT0 = std::chrono::steady_clock::now();
    auto since = [&]() { return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - spawnT0).count(); };
    auto& reg = world.Registry;
    // A body from the pool (a corpse that has gone) when there is one: shown again, its animators as they were built.
    std::shared_ptr<SoldierBody> built;
    while (!built && !m_Pool.empty()) {
        built = std::move(m_Pool.back());
        m_Pool.pop_back();
        if (!built || !reg.valid(built->Root)) built.reset();
    }
    const bool reused = built != nullptr;
    if (built) {
        for (const auto& [e, ac] : built->Animators)
            if (reg.valid(e)) reg.emplace_or_replace<AnimatorControllerComponent>(e, ac);
        SetTreeActive(world, built->Root, true);
        ++m_Reused;
    } else if (!(built = BuildBody(world, assets))) {
        return -1;
    }
    const float entitiesMs = since();
    const entt::entity root = built->Root;
    m_Bodies[root] = built;
    auto n = std::make_unique<Npc>();
    n->Index = (int)m_Npcs.size();
    for (int i = 0; i < (int)m_Npcs.size(); ++i)
        if (!m_Npcs[(size_t)i]) { n->Index = i; break; }
    n->Name = "Soldier " + std::to_string(m_NextName++);
    n->Root = root;
    if (auto* name = reg.try_get<NameComponent>(root)) name->Name = "[Runtime] " + n->Name;
    n->SpawnIndex = spawnIndex;
    n->Squad = sp.Squad;
    n->Dummy = sp.Brain == 1;
    n->Skill = sp.Skill;

    // Feet on the walkable mesh, a little apart from anyone already standing there.
    glm::vec3 feet = sp.Pos;
    for (int tries = 0; tries < 8; ++tries) {
        bool crowded = false;
        for (auto& o : m_Npcs)
            if (o && !o->Dead && glm::length(o->Feet - feet) < 1.0f) { crowded = true; break; }
        if (!crowded) break;
        const float a = (float)tries * 2.4f;
        feet = sp.Pos + glm::vec3(std::cos(a), 0.0f, std::sin(a)) * (1.2f + 0.3f * (float)tries);
    }
    glm::vec3 snapped;
    if (m_Nav.Closest(feet, snapped, glm::vec3(2.0f, 3.0f, 2.0f))) feet = snapped;
    n->Feet = feet;
    n->PostPos = feet;
    world.SetWorldPose(root, feet, glm::angleAxis(sp.Yaw, glm::vec3(0, 1, 0)));
    n->Body.SetHoldSettings(m_HoldSettings);
    if (!n->Body.Start(world, root)) {
        Log::Error("Enemy AI: the soldier's body has no animated skeleton.");
        m_Bodies.erase(root);
        world.DestroyEntityAndChildren(root);
        return -1;
    }
    n->Body.WarmHoldTables(world);
    const float bodyMs = since();
    n->Cct = PhysicsWorld::CreateNpcCharacter(Id(root), kRadius, kStandCyl, &feet.x);
    n->Agent = m_Crowd.Add(feet, 0.34f, 1.8f, 3.2f);
    auto& health = reg.emplace_or_replace<HealthComponent>(root);
    health.Max = 100.0f;
    health.Current = 100.0f;
    n->Health = n->MaxHealth = health.Max;

    // The weapon: the player's own, on a camera at the soldier's eyes.
    int weapon = sp.Weapon;
    if (weapon == 2) weapon = (int)(m_Rng() % 2u);
    FirstPersonControllerComponent cfg = *m_ViewConfig;
    cfg.AnimationSet = weapon == 1 ? kRemingtonPath : kAkPath;
    n->Class = weapon == 1 ? WeaponClass::Shotgun : WeaponClass::Rifle;
    n->Weapon = std::make_unique<FirstPersonPresentation>();
    FirstPersonPresentation::Options opt;
    opt.OwnerView = false;
    opt.HotReload = false;
    opt.CornerPeek = false;
    const float weaponT0 = since();
    if (!n->Weapon->Start(world, assets, cfg, opt) || !n->Weapon->IsActive()) {
        Log::Warn("Enemy AI: " + n->Name + " has no weapon (" + n->Weapon->LastError() + ").");
    } else {
        n->Gun = n->Weapon->Set().Gameplay;
        n->Weapon->SetLocomotionSpeeds(3.2f, 4.7f);
    }
    const float weaponMs = since() - weaponT0;
    n->WeaponCam.Fov = cfg.ViewModelFov;
    const glm::vec3 fwd(std::sin(sp.Yaw), 0.0f, std::cos(sp.Yaw));
    YawPitchOf(fwd, n->AimYaw, n->AimPitch);
    n->LookYaw = n->AimYaw;
    n->Eye = feet + glm::vec3(0.0f, 1.62f, 0.0f);
    n->SightEye = n->Eye;
    n->Intent.AimPoint = n->Intent.LookPoint = n->Eye + fwd * 10.0f;
    n->Doing = Behaviour::Idle;
    n->DoingSince = m_Now;
    n->NextLook = m_Now + 0.05f * (float)n->Index;
    n->NextThink = m_Now + 0.1f;
    n->Morale = 1.0f;
    n->SpawnedAt = m_Now;
    {
        char msg[160];
        std::snprintf(msg, sizeof msg, "Enemy AI: %s spawned in %.1f ms (entities %.1f%s, body %.1f, weapon %.1f ms).", n->Name.c_str(),
                      since(), entitiesMs, reused ? " reused" : "", bodyMs - entitiesMs, weaponMs);
        Log::Info(msg);
    }

    while ((int)m_Squads.size() <= n->Squad) m_Squads.emplace_back();
    if (!n->Dummy) m_Squads[(size_t)n->Squad].Members.push_back(n->Index); // a dummy has no part in the squad's tactics
    const int idx = n->Index;
    if (idx < (int)m_Npcs.size()) m_Npcs[(size_t)idx] = std::move(n);
    else m_Npcs.push_back(std::move(n));
    return idx;
}

void NpcDirector::Despawn(World& world, Npc& n, bool keepBody) {
    m_Cover.Release(n.Index, m_Now);
    n.Hitboxes.reset();
    n.Ragdoll.reset();
    if (n.Weapon) n.Weapon->Stop(world);
    n.Weapon.reset();
    n.Body.Stop();
    if (n.Cct != PhysicsWorld::kNoCharacter) PhysicsWorld::DestroyNpcCharacter(n.Cct);
    n.Cct = PhysicsWorld::kNoCharacter;
    if (n.Agent >= 0) m_Crowd.Remove(n.Agent);
    n.Agent = -1;
    if (n.Root != entt::null && world.Registry.valid(n.Root)) {
        auto found = m_Bodies.find(n.Root);
        // Kept for the next spawn (as many as a squad's worth): hidden, its animators off (the ragdoll took them).
        if (keepBody && found != m_Bodies.end() && found->second && (int)m_Pool.size() < std::max(m_SquadSize, 1)) {
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
    for (auto& s : m_Squads) s.Members.erase(std::remove(s.Members.begin(), s.Members.end(), n.Index), s.Members.end());
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

    // The player's noise: gunfire carries across the map, footsteps a few metres.
    if (p.Valid && !p.Dead) {
        if (p.Fired) m_Noises.push_back({p.Eye, 85.0f, 1.0f, m_Now, -1});
        const float speed = glm::length(glm::vec2(p.Velocity.x, p.Velocity.z));
        m_FootstepTimer -= dt;
        if (speed > 1.0f && !p.Crouched && m_FootstepTimer <= 0.0f) {
            const bool sprint = speed > 4.5f;
            m_Noises.push_back({p.Feet, sprint ? 16.0f : 7.0f, sprint ? 0.35f : 0.18f, m_Now, -1});
            m_FootstepTimer = sprint ? 0.32f : 0.45f;
        }
        if (p.Reloading) m_Noises.push_back({p.Eye, 9.0f, 0.15f, m_Now, -2});
        if (glm::length(p.Feet - m_PlayerPost) > 2.5f) { m_PlayerPost = p.Feet; m_PlayerStill = 0.0f; }
        else m_PlayerStill += dt;
        if (m_PlayerAgent < 0 && m_Crowd.Valid()) m_PlayerAgent = m_Crowd.Add(p.Feet, p.Radius, p.Height, 6.0f, false);
        if (m_PlayerAgent >= 0) m_Crowd.Sync(m_PlayerAgent, p.Feet, p.Velocity);
    }

    bool anyoneSees = false;
    for (auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        SubTimer timer(*this, SubPerceive);
        Perceive(world, *up, p, dt);
        anyoneSees = anyoneSees || up->Mem.Visible;
    }
    m_PlayerUnseen = anyoneSees ? 0.0f : m_PlayerUnseen + dt;
    {
        SubTimer timer(*this, SubSquads);
        UpdateSquads(p, dt);
    }

    for (auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        Npc& n = *up;
        // Down and not helped: it bleeds out.
        if (n.Wounded && m_Now - n.WoundedAt > m_Cfg.BleedOutTime) {
            Kill(world, n, FrontOf(n.AimYaw, 0.0f) * -1.0f, n.Feet + glm::vec3(0.0f, 0.8f, 0.0f), 6.0f);
            continue;
        }
        if (!Frozen && !n.Dummy) { // a dummy keeps the intent it spawned with: stand, facing the way it was placed
            SubTimer timer(*this, SubBrain);
            NpcBrain::Think(*this, world, n, p, dt);
        }
        // Movement goal -> the crowd.
        if (n.Agent >= 0) {
            const bool crouchMove = n.Intent.Crouch && n.Intent.Pace != Gait::Run;
            if (n.Intent.Move) {
                const glm::vec3 cur = m_Crowd.Position(n.Agent);
                (void)cur;
                if (!n.HasGoal || glm::length(n.Goal - n.Intent.MoveTarget) > 0.4f || !m_Crowd.HasTarget(n.Agent)) {
                    if (m_Crowd.SetTarget(n.Agent, n.Intent.MoveTarget)) {
                        n.Goal = n.Intent.MoveTarget;
                        n.HasGoal = true;
                        n.GoalSetAt = m_Now;
                    }
                }
                float speed = SpeedFor(n.Intent.Pace, crouchMove);
                if (n.Wounded) speed = m_Cfg.CrawlSpeed;                    // a crawl
                else if (m_Now < n.LimpUntil) speed *= m_Cfg.LimpSpeedScale; // a bad leg
                if (m_Now < n.StaggerUntil) speed *= 0.2f;              // reeling from a heavy hit
                m_Crowd.SetMaxSpeed(n.Agent, speed);
            } else if (n.HasGoal) {
                m_Crowd.Stop(n.Agent);
                n.HasGoal = false;
            }
        }
    }
    m_Crowd.Update(dt);

    int shooters = 0;
    for (auto& up : m_Npcs) {
        if (!up) continue;
        Npc& n = *up;
        if (n.Dead) {
            // The body lies there a while, then goes (and its replacement is on the way).
            if (m_Now - n.DiedAt > m_Cfg.CorpseTime) {
                Despawn(world, n, /*keepBody=*/true);
                up.reset();
            }
            continue;
        }
        {
            SubTimer timer(*this, SubMove);
            Move(world, n, dt);
        }
        {
            SubTimer timer(*this, SubAimFire);
            AimAndFire(world, n, p, dt);
            UpdateLod(world, n, p);
        }
        if (n.TriggerHeld) ++shooters;
    }
    m_ShootersNow = shooters;
    m_MaxShooters = std::max(m_MaxShooters, shooters);
    Respawns(world, assets, p);
    UpdateVoice(p);
    m_Noises.erase(std::remove_if(m_Noises.begin(), m_Noises.end(), [&](const Noise& z) { return m_Now - z.Time > 0.6f; }),
                   m_Noises.end());
    m_ThinkMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    FlushCosts(true);
}

void NpcDirector::Perceive(World& world, Npc& n, const PlayerSnapshot& p, float dt) {
    (void)world;
    n.Suppression = std::max(0.0f, n.Suppression - dt * 0.25f);
    n.ReactionLeft = std::max(0.0f, n.ReactionLeft - dt);
    if (n.Suppression > 0.5f) { if (n.PinnedSince < 0.0f) n.PinnedSince = m_Now; }
    else if (n.Suppression < 0.3f) n.PinnedSince = -1.0f;
    const glm::vec3 look = n.Intent.Aim ? FrontOf(n.AimYaw, n.AimPitch) : FrontOf(n.LookYaw, n.LookPitch);
    n.SightEye = n.Eye + glm::vec3(0.0f, 0.08f, 0.0f) + glm::normalize(glm::vec3(look.x, 0.0f, look.z) + glm::vec3(1e-5f)) * 0.08f;

    // Hearing: every noise since the last frame.
    for (const Noise& z : m_Noises) {
        if (z.Source == n.Index || m_Now - z.Time > dt + 1e-4f) continue;
        if (z.Source >= 0) {
            // A squadmate shooting: they've seen something - look where they look.
            const Npc* o = z.Source < (int)m_Npcs.size() ? m_Npcs[(size_t)z.Source].get() : nullptr;
            if (o && o->Squad == n.Squad && o->Mem.Known && glm::length(z.Pos - n.SightEye) < z.Radius) {
                n.Mem.Awareness = 1.0f;
                if (!n.Mem.Known) { n.Mem.Known = true; Callout(n, Bark::ContactRelay); }
                if (o->Mem.LastSeen > n.Mem.LastSeen) {
                    n.Mem.LastKnown = o->Mem.LastKnown;
                    n.Mem.LastSeen = o->Mem.LastSeen;
                    n.Mem.Uncertainty = std::max(n.Mem.Uncertainty, o->Mem.Uncertainty + 1.0f);
                }
            }
            continue;
        }
        const bool wasKnown = n.Mem.Known;
        HearNoise(n.Mem, n.SightEye, z.Pos, z.Radius, z.Loudness, m_Now);
        if (!wasKnown && n.Mem.Known) {
            n.ReactionLeft = std::max(n.ReactionLeft, ReactionTime(n.Skill, m_Difficulty, true, 0.5f));
            Callout(n, Bark::Gunfire);
        }
    }

    if (m_Now < n.NextLook) return;
    const float lookDt = std::clamp(m_Now - (n.NextLook - 0.12f), 0.0f, 0.5f);
    n.NextLook = m_Now + 0.1f + 0.04f * std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng);
    int visible = 0;
    glm::vec3 best{0.0f};
    float angleDeg = 180.0f, dist = 1e9f;
    if (p.Valid && !p.Dead) {
        const glm::vec3 to = p.Eye - n.SightEye;
        dist = glm::length(to);
        const glm::vec3 flatLook = glm::normalize(glm::vec3(look.x, 0.0f, look.z) + glm::vec3(1e-5f, 0, 0));
        // Peripheral angle is measured flat for the body's facing and from the look for the eyes.
        angleDeg = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(to), look), -1.0f, 1.0f)));
        const float flatAngle = glm::degrees(std::acos(std::clamp(
            glm::dot(glm::normalize(glm::vec3(to.x, 0.0f, to.z) + glm::vec3(1e-5f, 0, 0)), flatLook), -1.0f, 1.0f)));
        angleDeg = std::min(angleDeg, flatAngle);
        PerceptionSettings ps;
        if (dist < ps.Range && angleDeg < ps.PeripheralHalfAngle) {
            const glm::vec3 right = glm::normalize(glm::cross(glm::normalize(glm::vec3(to.x, 0.0f, to.z) + glm::vec3(1e-5f, 0, 0)), glm::vec3(0, 1, 0)));
            const glm::vec3 chest = p.Feet + glm::vec3(0.0f, p.Height * 0.72f, 0.0f);
            const glm::vec3 pts[5] = {chest, p.Eye - glm::vec3(0.0f, 0.04f, 0.0f), p.Feet + glm::vec3(0.0f, p.Height * 0.45f, 0.0f),
                                      chest + right * 0.2f, chest - right * 0.2f};
            PhysicsWorld::ScopedQueryPolicy policy(Id(n.Root), /*hitPlayer=*/true);
            for (const glm::vec3& q : pts) {
                glm::vec3 d = q - n.SightEye;
                const float len = glm::length(d);
                if (len < 1e-3f) continue;
                d /= len;
                const float o[3] = {n.SightEye.x, n.SightEye.y, n.SightEye.z}, dd[3] = {d.x, d.y, d.z};
                RaycastHit hit;
                QueryFilter f;
                f.HitTriggers = 0;
                if (PhysicsWorld::RaycastFiltered(o, dd, len + 0.5f, f, hit) && hit.Hit && hit.Entity == kPlayerEntity) {
                    if (visible == 0) best = q;
                    ++visible;
                }
            }
        }
    }
    n.VisiblePoints = visible;
    if (visible > 0) { n.SeenPoint = best; n.LastOwnSight = m_Now; }
    DetectionInput di;
    di.Distance = dist;
    di.AngleDeg = angleDeg;
    di.VisiblePoints = visible;
    di.TargetSpeed = glm::length(glm::vec2(p.Velocity.x, p.Velocity.z));
    di.TargetCrouched = p.Crouched;
    di.TargetFiring = p.Fired;
    di.Suppression = n.Suppression;
    di.Alertness = n.Mem.Known ? 1.0f : (n.Doing == Behaviour::Investigate ? 0.6f : 0.0f);
    const float rate = DetectionRate(di);
    const bool wasVisible = n.Mem.Visible;
    const float lastSeen = n.Mem.LastSeen;
    const bool known = UpdateMemory(n.Mem, visible > 0, p.Feet, p.Velocity, rate, m_Now, lookDt);
    if (known) {
        n.ReactionLeft = ReactionTime(n.Skill, m_Difficulty, angleDeg > 30.0f, std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng));
        n.FirstShot = true;
        // Caught out - the player close, or off to the side - most flinch down for a beat before they react.
        if ((dist < 12.0f || angleDeg > 35.0f) &&
            std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng) < 0.75f - 0.45f * n.Skill) {
            n.CowerUntil = m_Now + 0.3f;
            n.ReactionLeft = std::max(n.ReactionLeft, 0.35f);
            ++m_Tactics.Startles;
        }
        Callout(n, Bark::Contact);
    } else if (n.Mem.Visible && !wasVisible && n.Mem.Known) {
        // Back in sight after a while: a shorter reaction, and the first round may go wide again.
        if (m_Now - lastSeen > 1.5f) {
            n.ReactionLeft = std::max(n.ReactionLeft, 0.6f * ReactionTime(n.Skill, m_Difficulty, angleDeg > 30.0f, 0.5f));
            n.FirstShot = m_Now - lastSeen > 4.0f;
        }
        n.TimeOnTarget = 0.0f;
    }
}

void NpcDirector::UpdateSquads(const PlayerSnapshot& p, float dt) {
    (void)dt;
    for (size_t si = 0; si < m_Squads.size(); ++si) {
        Squad& s = m_Squads[si];
        // What the squad knows: the freshest sighting among its members, passed on after a beat.
        const TargetMemory* freshest = nullptr;
        for (int i : s.Members) {
            const Npc* n = i < (int)m_Npcs.size() ? m_Npcs[(size_t)i].get() : nullptr;
            if (!n || n->Dead || !n->Mem.Known) continue;
            if (!freshest || n->Mem.LastSeen > freshest->LastSeen) freshest = &n->Mem;
        }
        if (freshest) { s.Shared = *freshest; s.SharedAt = m_Now; }
        for (int i : s.Members) {
            Npc* n = i < (int)m_Npcs.size() ? m_Npcs[(size_t)i].get() : nullptr;
            if (!n || n->Dead || !freshest) continue;
            if (s.Shared.LastSeen > n->Mem.LastSeen + 0.5f && m_Now - s.Shared.LastSeen > 0.4f) {
                const bool was = n->Mem.Known;
                n->Mem.Known = true;
                n->Mem.Awareness = 1.0f;
                n->Mem.LastKnown = s.Shared.LastKnown;
                n->Mem.LastVelocity = s.Shared.LastVelocity;
                n->Mem.LastSeen = s.Shared.LastSeen;
                n->Mem.Uncertainty = s.Shared.Uncertainty + 1.5f;
                if (!was) n->ReactionLeft = std::max(n->ReactionLeft, 0.4f);
            }
        }
        // The player reloading, seen or heard: the moment to push.
        if (p.Reloading)
            for (int i : s.Members) {
                const Npc* n = i < (int)m_Npcs.size() ? m_Npcs[(size_t)i].get() : nullptr;
                if (n && !n->Dead && n->Mem.Known && (n->Mem.Visible || glm::length(n->SightEye - p.Eye) < 12.0f)) {
                    if (m_Now - s.PlayerReloadingSeen > 3.0f) {
                        s.PlayerReloadingSeen = m_Now;
                        s.PushUntil = m_Now + 4.5f;
                    }
                    break;
                }
            }

        // Roles, twice a second: the healthiest shotgun (else rifle) not nearest the player flanks,
        // the rest anchor (nearest) or suppress.
        if (m_Now >= s.NextRoles) {
            s.NextRoles = m_Now + 0.5f;
            static thread_local std::vector<Npc*> alive;
            alive.clear();
            for (int i : s.Members)
                if (Npc* n = i < (int)m_Npcs.size() ? m_Npcs[(size_t)i].get() : nullptr; n && !n->Dead) alive.push_back(n);
            const glm::vec3 target = s.Shared.LastKnown;
            std::sort(alive.begin(), alive.end(), [&](const Npc* a, const Npc* b) {
                return glm::length(a->Feet - target) < glm::length(b->Feet - target);
            });
            Npc* flanker = nullptr;
            float bestScore = -1.0f;
            for (size_t k = 0; k < alive.size(); ++k) {
                if (alive.size() >= 2 && k == 0) continue; // the nearest holds
                Npc* n = alive[k];
                const float score = n->Health / n->MaxHealth + (n->Class == WeaponClass::Shotgun ? 0.6f : 0.0f) + 0.2f * n->Skill;
                if (score > bestScore && n->Health > 0.45f * n->MaxHealth && !n->Wounded) { bestScore = score; flanker = n; }
            }
            if (alive.size() < 2) flanker = nullptr;
            // The pincer's second flanker keeps its token while it's fit and still working round.
            if (s.PincerHolder >= 0) {
                const Npc* h = s.PincerHolder < (int)m_Npcs.size() ? m_Npcs[(size_t)s.PincerHolder].get() : nullptr;
                if (!h || h->Dead || h->Wounded || h == flanker) s.PincerHolder = -1;
            }
            for (size_t k = 0; k < alive.size(); ++k) {
                Npc* n = alive[k];
                n->Role = n == flanker || n->Index == s.PincerHolder
                              ? NpcRole::Flanker
                              : (k == 0 || n->Class == WeaponClass::Shotgun ? NpcRole::Anchor : NpcRole::Suppressor);
            }
            // The flank token goes to the flanker; one at a time, and not straight after a death.
            if (s.FlankHolder >= 0) {
                const Npc* h = s.FlankHolder < (int)m_Npcs.size() ? m_Npcs[(size_t)s.FlankHolder].get() : nullptr;
                if (!h || h->Dead || h->Role != NpcRole::Flanker) s.FlankHolder = -1;
            }
            if (s.FlankHolder < 0 && flanker && flanker->Mem.Known && m_Now - s.LastDeath > 4.0f && m_Now - s.FlankDoneAt > 8.0f)
                s.FlankHolder = flanker->Index;
            // A pincer: with four or more up and the first flanker on its way, a second goes round the other side (its cover
            // is picked 80 degrees or more round from the first's - NpcBrain::FindCover).
            const Npc* first = s.FlankHolder >= 0 && s.FlankHolder < (int)m_Npcs.size() ? m_Npcs[(size_t)s.FlankHolder].get() : nullptr;
            if (s.PincerHolder < 0 && alive.size() >= 4 && m_Difficulty >= 0.75f && first && first->Doing == Behaviour::Flank &&
                first->Cover >= 0 && m_Now - s.LastDeath > 4.0f && m_Now - s.PincerDoneAt > 12.0f) {
                Npc* second = nullptr;
                float best = -1.0f;
                for (size_t k = 1; k < alive.size(); ++k) { // not the nearest: it holds
                    Npc* n = alive[k];
                    if (n == first || n->Wounded || !n->Mem.Known || n->Health < 0.5f * n->MaxHealth || n->Doing != Behaviour::CoverFight)
                        continue;
                    const float score = n->Health / n->MaxHealth + 0.3f * n->Skill;
                    if (score > best) { best = score; second = n; }
                }
                if (second) {
                    s.PincerHolder = second->Index;
                    second->Role = NpcRole::Flanker;
                }
            }
            // A badly hurt player in view: press them before they recover.
            if (p.Valid && !p.Dead && p.Health < 0.4f && m_Now - s.PlayerHurtPushAt > 10.0f && m_Now >= s.PushUntil)
                for (Npc* n : alive)
                    if (n->Mem.Visible) {
                        s.PlayerHurtPushAt = m_Now;
                        s.PushUntil = m_Now + 4.5f;
                        ++m_Tactics.HurtPushes;
                        break;
                    }
            // The push token: during a push, the healthiest of the nearer half.
            if (m_Now < s.PushUntil) {
                if (s.PushHolder < 0)
                    for (Npc* n : alive)
                        if (n->Health > 0.5f * n->MaxHealth && n->Index != s.FlankHolder && n->Index != s.PincerHolder && !n->Wounded) {
                            s.PushHolder = n->Index;
                            break;
                        }
            } else {
                s.PushHolder = -1;
            }
            for (Npc* n : alive) {
                n->HasFlankToken = n->Index == s.FlankHolder || n->Index == s.PincerHolder;
                n->HasPushToken = n->Index == s.PushHolder;
            }
        }
        UpdateCoverFire(s);
        // Attack tokens: how many shoot at once. Holders keep theirs a few seconds while they can
        // still see; the rest wait their turn, so the player always has a moment to act.
        if (m_Now >= s.NextTokens) {
            s.NextTokens = m_Now + 0.25f;
            const int maxShooters = std::clamp((int)std::round(1.6f + 0.9f * m_Difficulty), 1, 4);
            auto wants = [&](const Npc* n) {
                return n && !n->Dead && n->Intent.Fire && (n->Mem.Visible || n->Intent.Suppress);
            };
            s.Attackers.erase(std::remove_if(s.Attackers.begin(), s.Attackers.end(),
                                             [&](int i) {
                                                 const Npc* n = i < (int)m_Npcs.size() ? m_Npcs[(size_t)i].get() : nullptr;
                                                 return !wants(n);
                                             }),
                              s.Attackers.end());
            // Fill free slots, the closest first.
            static thread_local std::vector<Npc*> cands;
            cands.clear();
            for (int i : s.Members)
                if (Npc* n = i < (int)m_Npcs.size() ? m_Npcs[(size_t)i].get() : nullptr;
                    wants(n) && std::find(s.Attackers.begin(), s.Attackers.end(), i) == s.Attackers.end())
                    cands.push_back(n);
            std::sort(cands.begin(), cands.end(), [&](const Npc* a, const Npc* b) {
                return glm::length(a->Feet - p.Feet) < glm::length(b->Feet - p.Feet);
            });
            for (Npc* n : cands)
                if ((int)s.Attackers.size() < maxShooters) s.Attackers.push_back(n->Index);
            for (int i : s.Members)
                if (Npc* n = i < (int)m_Npcs.size() ? m_Npcs[(size_t)i].get() : nullptr)
                    n->HasAttackToken = std::find(s.Attackers.begin(), s.Attackers.end(), i) != s.Attackers.end();
        }
    }
}

// Fire and maneuver (see NpcBrain's bound): anyone but the waiting soldier shooting covers its bound. When nobody is, a
// squadmate fighting from cover is told to come up and give covering fire.
void NpcDirector::UpdateCoverFire(Squad& s) {
    auto get = [&](int i) { return i >= 0 && i < (int)m_Npcs.size() ? m_Npcs[(size_t)i].get() : nullptr; };
    for (int i : s.Members)
        if (const Npc* n = get(i); n && !n->Dead && n->Index != s.CoverRequest && n->TriggerHeld) {
            s.CoverFireUntil = m_Now + 0.5f;
            break;
        }
    if (s.CoverRequest >= 0 && m_Now - s.CoverRequestAt > 0.3f) s.CoverRequest = -1; // it stopped waiting
    if (s.CoverFirer >= 0) {
        const Npc* f = get(s.CoverFirer);
        if (!f || f->Dead || m_Now >= f->CoverFireOrder) s.CoverFirer = -1;
    }
    if (s.CoverRequest < 0 || s.CoverFirer >= 0 || m_Now < s.CoverFireUntil) return;
    Npc* best = nullptr;
    float bestScore = 0.0f;
    for (int i : s.Members) {
        Npc* n = get(i);
        if (!n || n->Dead || n->Wounded || n->Index == s.CoverRequest || !n->Mem.Known || n->Reloading) continue;
        if (n->Class == WeaponClass::Shotgun || n->Doing != Behaviour::CoverFight || n->Cover < 0) continue;
        if (n->Weapon && n->Weapon->IsActive() && n->Weapon->Ammo() == 0) continue;
        const float score = 1.0f + (n->Role == NpcRole::Suppressor ? 0.5f : 0.0f) + n->Skill * 0.2f;
        if (score > bestScore) { bestScore = score; best = n; }
    }
    if (!best) return;
    best->CoverFireOrder = m_Now + 2.5f;
    s.CoverFirer = best->Index;
    ++m_Tactics.CoverOrders;
    Callout(*best, Bark::Covering);
}

// A rifle-butt strike at a player in arm's reach: started here, the blow resolved MeleeHitTime later (still in reach and
// in front, it lands). The gun's thrust is drawn in LatePose.
void NpcDirector::UpdateMelee(Npc& n, const PlayerSnapshot& p) {
    const glm::vec3 to(p.Feet.x - n.Feet.x, 0.0f, p.Feet.z - n.Feet.z);
    const float dist = glm::length(to);
    const float yaw = n.Body.Yaw();
    const glm::vec3 fwd(std::sin(yaw), 0.0f, std::cos(yaw));
    const float facing = dist > 1e-3f ? glm::degrees(std::acos(std::clamp(glm::dot(fwd, to / dist), -1.0f, 1.0f))) : 0.0f;
    const bool level = std::abs(p.Feet.y - n.Feet.y) < 1.0f;
    if (!n.MeleeLanded && m_Now - n.MeleeAt >= m_Cfg.MeleeHitTime) {
        n.MeleeLanded = true;
        if (p.Valid && !p.Dead && level && dist < 2.3f && facing < 70.0f) {
            DamageEvent e;
            e.Source = Id(n.Root);
            e.Target = kPlayerEntity;
            e.Amount = m_Cfg.MeleeDamage * std::clamp(m_Difficulty, 0.5f, 1.5f);
            e.Zone = HitZone::Torso;
            e.Point = p.Feet + glm::vec3(0.0f, p.Height * 0.75f, 0.0f);
            e.Direction = dist > 1e-3f ? to / dist : fwd;
            e.SourcePos = n.Eye;
            m_PlayerDamage.push_back(e);
            ++m_Tactics.MeleeHits;
        }
    }
    if (HoldFire || n.Dummy || n.Wounded || n.Reloading || !n.Mem.Known || !p.Valid || p.Dead || !level || m_Now - n.MeleeAt < m_Cfg.MeleeTime) return;
    if (!WantsMelee(dist, facing, m_Now - n.MeleeAt)) return;
    n.MeleeAt = m_Now;
    n.MeleeLanded = false;
    n.BurstLeft = 0;
    ++m_Tactics.Melees;
    Callout(n, Bark::Melee);
}

void NpcDirector::Move(World& world, Npc& n, float dt) {
    if (n.Cct == PhysicsWorld::kNoCharacter) return;
    glm::vec3 want(0.0f);
    if (n.Intent.Move && n.Agent >= 0) want = m_Crowd.Velocity(n.Agent);
    want.y = 0.0f;
    n.FallSpeed -= m_Cfg.FallGravity * dt;
    const glm::vec3 disp = (want + n.PushVel) * dt + glm::vec3(0.0f, n.FallSpeed * dt, 0.0f);
    n.PushVel *= std::exp(-7.0f * dt); // a stagger's shove dies away
    const float d[3] = {disp.x, disp.y, disp.z};
    const unsigned hit = PhysicsWorld::MoveNpcCharacter(n.Cct, d, dt);
    if (hit & PhysicsWorld::CC_DOWN) n.FallSpeed = -1.0f;
    float feet[3], r = 0.0f, cyl = 0.0f;
    if (PhysicsWorld::GetNpcCapsule(n.Cct, feet, &r, &cyl)) {
        const glm::vec3 f(feet[0], feet[1], feet[2]);
        const glm::vec3 moved = f - n.Feet;
        n.Velocity = glm::vec3(moved.x, 0.0f, moved.z) / dt;
        n.Feet = f;
    }
    if (n.Agent >= 0) m_Crowd.Sync(n.Agent, n.Feet);
    // Getting stuck: asked to move, barely moving.
    if (n.Intent.Move && glm::length(want) > 0.5f && glm::length(n.Velocity) < 0.25f) n.BlockedTime += dt;
    else n.BlockedTime = std::max(0.0f, n.BlockedTime - dt * 2.0f);
    // Stance: crouching shrinks the capsule; standing waits for headroom.
    const bool wantCrouch = n.Intent.Crouch && n.Intent.Pace != Gait::Run;
    if (wantCrouch != n.Crouched) {
        if (wantCrouch) {
            PhysicsWorld::ResizeNpcCharacter(n.Cct, kCrouchCyl);
            n.Crouched = true;
        } else if (PhysicsWorld::NpcFitsAt(n.Cct, kStandCyl)) {
            PhysicsWorld::ResizeNpcCharacter(n.Cct, kStandCyl);
            n.Crouched = false;
        }
    }

    // The body.
    NpcBodyInput in;
    in.Feet = n.Feet;
    in.Velocity = n.Velocity;
    const glm::vec3 face = n.Intent.Aim ? n.Intent.AimPoint : n.Intent.LookPoint;
    in.FacingYaw = NpcYawOf(face - n.Feet, n.Body.Yaw());
    in.HoldFacing = n.Intent.FaceAim;
    in.AimPoint = n.Intent.AimPoint;
    // The stance stays aimed through a reload (worked at the hip, the player's body's chest keeps following its view
    // there too: dropping it at each reload's start and end rocked the torso); blind fire and a rifle-butt strike aren't.
    in.Aiming = n.Intent.Aim && !n.Intent.BlindFire && m_Now - n.MeleeAt >= m_Cfg.MeleeTime;
    in.LookPoint = n.Intent.LookPoint;
    in.Crouched = n.Crouched;
    in.Sprint = n.Intent.Pace == Gait::Run && n.Intent.Move;
    in.Lean = n.Intent.Lean;
    in.Cower = n.Intent.Cower;
    in.FootIK = FootIKEverywhere || (n.PlayerDist < m_Cfg.FootIKRange && n.OnScreen); // feet on slopes and steps where they can be seen
    n.Body.Tick(world, in, dt);
}

void NpcDirector::AimAndFire(World& world, Npc& n, const PlayerSnapshot& p, float dt) {
    // Aim: a critically damped spring on the weapon camera's yaw and pitch toward the aim point. The
    // gun's own recoil kick shows on the rig; the spring brings the muzzle back down.
    // The eyes go where the brain looks (quickly); the gun follows the aim, or - off the sights - the chest's heading,
    // level: the player's hip carry, not a gun swinging round after every glance.
    // A reload is worked at the hip, as the player's is when not aiming: the sights come down while the hands are busy
    // (an ADS-carried reload swings the reaching hand across the face as the gun bobs on the aim), the view level on
    // the threat, then back onto the sights when the new magazine is in.
    n.Reloading = n.Pumping = false;
    if (n.Weapon && n.Weapon->IsActive()) {
        const std::string& st = n.Weapon->CurrentState();
        n.Reloading = st.find("Reload") != std::string::npos;
        n.Pumping = st == "Pump";
    }
    UpdateMelee(n, p);
    const bool meleeing = m_Now - n.MeleeAt < m_Cfg.MeleeTime;
    const bool aimGun = n.Intent.Aim && !n.Reloading && !n.Intent.BlindFire && !meleeing;
    const glm::vec3 target = n.Intent.Aim ? n.Intent.AimPoint : n.Intent.LookPoint;
    {
        float ly = n.LookYaw, lp = n.LookPitch;
        YawPitchOf(target - n.Eye, ly, lp);
        const float k = AiFollow(dt, 0.12f);
        n.LookYaw = WrapDeg(n.LookYaw + WrapDeg(ly - n.LookYaw) * k);
        n.LookPitch += (std::clamp(lp, -60.0f, 60.0f) - n.LookPitch) * k;
    }
    float wantYaw = n.AimYaw, wantPitch = n.AimPitch;
    YawPitchOf(target - n.Eye, wantYaw, wantPitch);
    if (n.Reloading) {
        // Facing what it was looking at, the view near level - as the player reloads: the reload clips are authored
        // against a level camera, and pitched well down the hands worked the magazine tipped toward the floor.
        wantPitch = std::clamp(wantPitch, -15.0f, 15.0f);
    } else if (!n.Intent.Aim) {
        // At the hip, as the player stands with the gun down off the sights: the view along the chest, about level
        // (what it looks at only nods it a little). A pitched-down "low ready" was the soldiers' own pose: the weapon
        // clips are authored level, and an idle regrip played under it threw the gun 9 cm up and back.
        const float lookPitch = std::clamp(wantPitch, -15.0f, 10.0f);
        const float chest = n.Body.Yaw();
        YawPitchOf(glm::vec3(std::sin(chest), 0.0f, std::cos(chest)), wantYaw, wantPitch);
        wantPitch = lookPitch;
    }
    const float omega = 9.0f + 7.0f * n.Skill;
    const float dy = WrapDeg(wantYaw - n.AimYaw), dp = wantPitch - n.AimPitch;
    n.AimYawRate += (omega * omega * dy - 2.0f * omega * n.AimYawRate) * dt;
    n.AimPitchRate += (omega * omega * dp - 2.0f * omega * n.AimPitchRate) * dt;
    n.AimYaw = WrapDeg(n.AimYaw + n.AimYawRate * dt);
    n.AimPitch = std::clamp(n.AimPitch + n.AimPitchRate * dt, -80.0f, 80.0f);
    const glm::vec3 aimDir = FrontOf(n.AimYaw, n.AimPitch);
    const glm::vec3 wantDir = glm::normalize(target - n.Eye + glm::vec3(1e-5f));
    const float errorDeg = glm::degrees(std::acos(std::clamp(glm::dot(aimDir, wantDir), -1.0f, 1.0f)));
    if (n.Mem.Visible && aimGun && errorDeg < 6.0f) n.TimeOnTarget += dt;
    else n.TimeOnTarget = std::max(0.0f, n.TimeOnTarget - dt * 2.0f);

    if (!n.Weapon || !n.Weapon->IsActive()) return;
    FirstPersonPresentation& w = *n.Weapon;
    n.WeaponCam.Position = n.Eye;
    n.WeaponCam.Yaw = n.AimYaw;
    n.WeaponCam.Pitch = n.AimPitch;
    n.WeaponCam.Roll = 0.0f;
    {
        PhysicsWorld::ScopedQueryPolicy policy(Id(n.Root), /*hitPlayer=*/true);
        w.Update(world, n.WeaponCam);
    }
    if (!n.FullAutoSet) {
        if (n.Gun.AllowFullAuto && !w.IsFullAuto()) w.ToggleFireMode();
        n.FullAutoSet = true;
    }
    // Rounds spent since last frame end a burst.
    if (n.AmmoSeen >= 0 && w.Ammo() < n.AmmoSeen) {
        const int spent = n.AmmoSeen - w.Ammo();
        n.ShotsFired += spent;
        n.LastShot = m_Now;
        n.FirstShot = false;
        if (n.BurstLeft > 0) n.BurstLeft = std::max(0, n.BurstLeft - spent);
        m_Noises.push_back({n.Eye, 80.0f, 1.0f, m_Now, n.Index});
    }
    n.AmmoSeen = w.Ammo();
    n.BurstPause = std::max(0.0f, n.BurstPause - dt);

    const bool sprinting = n.Intent.Pace == Gait::Run && n.Intent.Move && glm::length(n.Velocity) > 3.5f;
    const bool reloading = n.Reloading;
    const float tolerance = n.Intent.Suppress ? 9.0f : 4.5f + 3.0f * std::clamp(8.0f / std::max(glm::length(target - n.Eye), 1.0f), 0.0f, 1.0f);
    // A hand off the gun signalling: the order first, then the shooting.
    bool canFire = n.Intent.Fire && !HoldFire && !n.Dummy && n.HasAttackToken && n.ReactionLeft <= 0.0f && errorDeg < tolerance && !reloading &&
                   !n.Body.Signalling() && !meleeing &&
                   !sprinting && w.IsEquipped() && p.Valid && !p.Dead && (n.Mem.Visible || n.Intent.Suppress) && w.Ammo() > 0;
    // Never through a friend: the first thing along the line must not be a squadmate (checked every
    // frame the trigger could be down, so a friend stepping into the line stops the burst).
    if (canFire) {
        PhysicsWorld::ScopedQueryPolicy policy(Id(n.Root), /*hitPlayer=*/true);
        const float o[3] = {n.Eye.x, n.Eye.y, n.Eye.z}, d[3] = {wantDir.x, wantDir.y, wantDir.z};
        RaycastHit hit;
        QueryFilter f;
        f.HitTriggers = 0;
        if (PhysicsWorld::RaycastFiltered(o, d, glm::length(target - n.Eye) + 1.0f, f, hit) && hit.Hit && hit.Entity != kPlayerEntity)
            for (auto& o2 : m_Npcs)
                if (o2 && !o2->Dead && o2.get() != &n && Id(o2->Root) == hit.Entity) { canFire = false; n.BlockedTime += 0.3f; break; }
    }

    bool pressed = false, held = false;
    if (n.Class == WeaponClass::Rifle) {
        if (n.BurstLeft == 0 && n.BurstPause <= 0.0f && canFire) {
            std::uniform_int_distribution<int> len(n.Intent.Suppress ? 5 : 3, n.Intent.Suppress ? 9 : 6);
            n.BurstLeft = len(m_Rng);
            pressed = true;
        }
        if (n.BurstLeft > 0 && canFire) held = true;
        else if (n.BurstLeft > 0) n.BurstLeft = 0; // lost the shot: let go
        if (n.TriggerHeld && !held) {
            std::uniform_real_distribution<float> pause(n.Intent.Suppress ? 0.25f : 0.35f, n.Intent.Suppress ? 0.5f : 0.85f);
            n.BurstPause = pause(m_Rng);
        }
    } else {
        if (canFire && n.BurstPause <= 0.0f && w.Chambered()) {
            pressed = held = true;
            n.BurstPause = std::uniform_real_distribution<float>(0.5f, 0.95f)(m_Rng);
        }
    }
    n.TriggerHeld = held;
    // Loading the tube a shell at a time with the player at arm's length: the trigger ends the reload after the shell in
    // hand, as the player's does, and the rifle butt is free to swing.
    if (reloading && n.Class == WeaponClass::Shotgun && w.Ammo() > 0 && n.Mem.Known && p.Valid && !p.Dead &&
        glm::length(glm::vec2(p.Feet.x - n.Feet.x, p.Feet.z - n.Feet.z)) < 2.5f)
        pressed = true;
    w.UpdateTrigger(pressed, held);
    if ((n.Intent.Reload || w.Ammo() == 0) && !reloading && !held) w.Reload();
    // Standing, the gun is told it stands: the capsule creeps a few cm/s (the crowd's nudges), and the weapon's clips leave
    // a regrip or an idle at any speed over 0.05 m/s - the regrip started and was cut off a frame later, over and over.
    // (The body counts as moving from the same 0.25 m/s; the player's own gun reads 0 standing still.)
    const glm::vec3 gunVelocity = glm::length(glm::vec2(n.Velocity.x, n.Velocity.z)) > 0.25f ? n.Velocity : glm::vec3(0.0f);
    w.Tick(dt, gunVelocity, sprinting, aimGun && !sprinting, 0.0f, true);
    // (After the tick, before the animators: the weapon clears its triggers at the start of each frame.)
    if (n.WeaponAction) w.TriggerAction(n.WeaponAction);
    n.WeaponAction = nullptr;
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
                    LatePose(world, n, n.LateDt, p, /*alive=*/false);
                    m_DeathBones[n.Index].Dt = n.LateDt;
                    n.LateDt = 0.0f;
                }
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
            // Held this frame (animation LOD): the pose stays, the hitboxes ride the body, the gun's queued reports go out.
            UpdateHitboxes(n, p, /*posed=*/false);
            if (n.Weapon && n.Weapon->IsActive())
                for (const CasingSpawn& c : n.Weapon->TakeEjections()) if (m_Ejections.size() < 64) m_Ejections.push_back(c);
            continue;
        }
        const float lateDt = n.LateDt;
        n.LateDt = 0.0f;
        LatePose(world, n, lateDt, p, /*alive=*/true);
        UpdateHitboxes(n, p, /*posed=*/true);
        if (TrackDeathPop) {
            static const char* kBones[3] = {"head", "hand_l", "foot_l"};
            n.HaveLastBones = true;
            for (int k = 0; k < 3; ++k) n.Body.BoneWorld(kBones[k], n.LastBones[k]);
        }
    }
    m_LateMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    FlushCosts(false);
}

// The soldier's late pose: the spine aimed, the gun's camera hung off the shoulders, this frame's rounds fired, the
// gun seated and the hands onto it. `alive` false: its dying frame - the same pose, but no more rounds.
void NpcDirector::LatePose(World& world, Npc& n, float dt, const PlayerSnapshot& p, bool alive) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const bool armed = n.Weapon && n.Weapon->IsActive();
    n.WeaponCam.Yaw = n.AimYaw;
    n.WeaponCam.Pitch = n.AimPitch;
    n.WeaponCam.Roll = 0.0f;
    {
        SubTimer timer(*this, SubBody);
        n.Body.LateUpdate(world, dt, armed ? &n.WeaponCam : nullptr);
    }
    n.Eye = n.Body.Eye();
    if (!armed) {
        n.Body.HoldWeapon(world, entt::null, entt::null, nullptr, n.WeaponCam, dt, false);
        return;
    }
    // The weapon's camera hangs off the body's shoulders as the rig's does off its own (the player's
    // shoulder lock), so the rig's hands come out where this body's arms reach.
    n.WeaponCam.Position = n.Body.WeaponEye(world, n.Weapon->ArmsEntity(), n.Weapon->CameraBone(), n.WeaponCam, dt);
    // Blind fire: the gun eased up over low cover, or out past the edge of high cover, while the head stays down behind it.
    if (n.Intent.BlindFire && alive) {
        n.BlindOffset = glm::vec3(0.0f, 0.34f, 0.0f) + FrontOf(n.AimYaw, 0.0f) * 0.12f;
        if (n.Cover >= 0 && n.PeekSide >= 0) {
            const CoverPoint& c = m_Cover.Points()[(size_t)n.Cover];
            const glm::vec3 out(c.PeekPos[n.PeekSide].x - c.Pos.x, 0.0f, c.PeekPos[n.PeekSide].z - c.Pos.z);
            if (c.High && glm::dot(out, out) > 1e-4f)
                n.BlindOffset = glm::normalize(out) * 0.45f + FrontOf(n.AimYaw, 0.0f) * 0.1f + glm::vec3(0.0f, 0.12f, 0.0f);
        }
    }
    n.BlindLift += ((n.Intent.BlindFire && alive ? 1.0f : 0.0f) - n.BlindLift) * AiFollow(dt, 0.09f);
    if (n.BlindLift > 1e-3f) n.WeaponCam.Position += n.BlindOffset * n.BlindLift;
    // A rifle-butt strike: drawn back, driven forward at the player's chest with a twist, then recovered.
    if (const float mt = (m_Now - n.MeleeAt) / m_Cfg.MeleeTime; alive && mt >= 0.0f && mt < 1.0f) {
        float reach;
        if (mt < 0.3f) reach = -0.12f * Smoothstep01(mt / 0.3f);
        else if (mt < 0.42f) reach = -0.12f + 0.52f * Smoothstep01((mt - 0.3f) / 0.12f);
        else reach = 0.4f * (1.0f - Smoothstep01((mt - 0.42f) / 0.58f));
        n.WeaponCam.Position += FrontOf(n.AimYaw, n.AimPitch) * reach;
        n.WeaponCam.Roll = 28.0f * std::sin(3.14159265f * mt);
    }
    // Where this frame's rounds go: on the player (the point it sees best, or the chest) when
    // the roll hits, else a near miss the player hears go by. Suppressing: about where they were.
    if (alive && n.TriggerHeld) {
        glm::vec3 shot;
        if ((n.Intent.Suppress && !n.Mem.Visible) || n.Intent.BlindFire) {
            // Blind fire sprays: it can't see where its rounds go.
            const glm::vec3 j(unit(m_Rng) - 0.5f, unit(m_Rng) * 0.8f, unit(m_Rng) - 0.5f);
            shot = n.Mem.Predicted(m_Now) + glm::vec3(0.0f, 1.1f, 0.0f) +
                   j * (0.8f + n.Mem.Uncertainty * 0.3f) * (n.Intent.BlindFire ? 2.4f : 1.0f);
        } else {
            AccuracyInput ai;
            ai.Distance = glm::length(p.Eye - n.Eye);
            ai.TargetSpeed = glm::length(glm::vec2(p.Velocity.x, p.Velocity.z));
            ai.TimeOnTarget = n.TimeOnTarget;
            ai.SelfSpeed = glm::length(n.Velocity);
            ai.Suppression = n.Suppression;
            ai.Skill = n.Skill;
            ai.Difficulty = m_Difficulty;
            ai.VisibleFraction = (float)n.VisiblePoints / 5.0f;
            ai.TargetCrouched = p.Crouched;
            const glm::vec3 toNpc = glm::normalize(n.Eye - p.Eye + glm::vec3(1e-5f));
            ai.OutsideTargetView = glm::dot(toNpc, p.Forward) < std::cos(glm::radians(55.0f));
            ai.Flinching = m_Now - n.LastHurt < 0.35f;
            ai.Weapon = n.Class;
            // Wounded, it shoots at half its accuracy.
            const bool hit = !n.FirstShot && unit(m_Rng) < HitProbability(ai) * (n.Wounded ? 0.5f : 1.0f);
            const glm::vec3 seen = n.VisiblePoints > 0 ? n.SeenPoint : p.Feet + glm::vec3(0.0f, p.Height * 0.7f, 0.0f);
            if (hit) {
                shot = seen + glm::vec3(unit(m_Rng) - 0.5f, unit(m_Rng) - 0.5f, unit(m_Rng) - 0.5f) * 0.18f;
            } else {
                // A near miss: past the head or a shoulder, close enough to crack by.
                const glm::vec3 across = glm::normalize(glm::cross(p.Eye - n.Eye, glm::vec3(0, 1, 0)) + glm::vec3(1e-5f));
                const float side = (unit(m_Rng) < 0.5f ? -1.0f : 1.0f) * (0.45f + 0.7f * unit(m_Rng));
                shot = seen + across * side + glm::vec3(0.0f, -0.2f + 0.7f * unit(m_Rng), 0.0f);
            }
        }
        n.Weapon->SetShotTarget(&shot);
    } else {
        n.Weapon->SetShotTarget(nullptr);
    }
    {
        SubTimer timer(*this, SubWeapon);
        PhysicsWorld::ScopedQueryPolicy policy(Id(n.Root), /*hitPlayer=*/true);
        n.Weapon->LateUpdate(world, n.WeaponCam);
    }
    // The gun seated in the shoulder and clear of the body, the arms onto it, the head onto the stock -
    // the player's world body's solve. The drawn surfaces are checked near the player, and in view (where it shows).
    glm::vec3 muzzleShift(0.0f);
    {
        SubTimer holdTimer(*this, SubHold);
        FirstPersonWorldGunInput gun;
        const bool haveGun = n.Weapon->WorldGunInput(gun);
        const bool closeToPlayer = !p.Valid || (glm::length(n.Eye - p.Eye) < m_Cfg.MeshCheckRange && n.OnScreen) || MeshChecksEverywhere;
        muzzleShift = n.Body.HoldWeapon(world, n.Weapon->ArmsEntity(), n.Weapon->WeaponEntity(), haveGun ? &gun : nullptr, n.WeaponCam,
                                        dt, closeToPlayer);
    }
    n.Eye = n.Body.Eye();
    if (alive) {
        SubTimer shotsTimer(*this, SubWeapon); // the rounds' reports, flashes, tracers and hits count to the weapon
        HandleShots(world, n, p, muzzleShift);
    }
    for (const CasingSpawn& c : n.Weapon->TakeEjections()) if (m_Ejections.size() < 64) m_Ejections.push_back(c);
}

// Per soldier, after it has fired: how often its pose is worked out. Near and in view: every frame. Further (25 m) every
// second, past 50 m or out of view every fourth, staggered between soldiers. Anything that has to read right (firing, just
// hurt, just spawned) stays at full rate. The skipped frames keep the last pose; time is kept for the next one.
void NpcDirector::UpdateLod(World& world, Npc& n, const PlayerSnapshot& p) {
    int lod = 1;
    float dist = 0.0f;
    bool onScreen = true;
    if (p.Valid) {
        const glm::vec3 to = n.Feet + glm::vec3(0.0f, 0.9f, 0.0f) - p.Eye;
        dist = glm::length(to);
        // The view's cone, roughly: a wide FOV and a margin for the body's size.
        const float margin = std::atan2(1.6f, std::max(dist, 0.5f));
        onScreen = dist < 3.0f || std::acos(std::clamp(glm::dot(to / dist, p.Forward), -1.0f, 1.0f)) < glm::radians(62.0f) + margin;
        if (!NoLod && !MeshChecksEverywhere) lod = !onScreen || dist > 50.0f ? 4 : dist > 25.0f ? 2 : 1;
    }
    n.Lod = lod;
    n.PlayerDist = dist;
    n.OnScreen = onScreen;
    const bool busy = n.TriggerHeld || m_Now - n.LastShot < 0.4f || m_Now - n.LastHurt < 0.5f || m_Now - n.SpawnedAt < 0.5f;
    n.Animate = busy || lod == 1 || ((m_Frame + n.Index) % lod) == 0;
    auto hold = [&](entt::entity e) {
        if (e == entt::null || !world.Registry.valid(e)) return;
        if (auto* ac = world.Registry.try_get<AnimatorControllerComponent>(e)) ac->SkipUpdate = !n.Animate;
    };
    for (entt::entity e : n.Body.Pieces()) hold(e);
    if (n.Weapon && n.Weapon->IsActive()) {
        hold(n.Weapon->ArmsEntity());
        hold(n.Weapon->WeaponEntity());
    }
}

// The per-bone hitboxes follow the skeleton for soldiers within Hitbox Range of the player (made on first need, off
// beyond it, where the capsule answers instead).
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

void NpcDirector::HandleShots(World& world, Npc& n, const PlayerSnapshot& p, const glm::vec3& muzzleShift) {
    // What the round looked and sounded like: the report and flash at the (shouldered) muzzle, a
    // tracer every third round, and the crack of anything passing close to the player's head.
    for (const FirstPersonPresentation::ShotTrace& t : n.Weapon->TakeShotTraces()) {
        if (!Fx) continue;
        const bool shotgun = n.Class == WeaponClass::Shotgun;
        if (t.FirstPellet)
            Fx->Shot(world, shotgun ? CombatFx::Gun::Shotgun : CombatFx::Gun::Rifle, t.Origin + muzzleShift, t.End, false,
                     !shotgun && (n.Tracer++ % 3) == 0);
        if (!p.Valid || p.Dead || (t.Hit && t.Entity == kPlayerEntity)) continue;
        const glm::vec3 seg = t.End - t.Origin;
        const float len2 = glm::dot(seg, seg);
        if (len2 < 1e-4f) continue;
        const float u = std::clamp(glm::dot(p.Eye - t.Origin, seg) / len2, 0.0f, 1.0f);
        const glm::vec3 closest = t.Origin + seg * u;
        if (glm::length(closest - p.Eye) < 1.6f && u * std::sqrt(len2) > 3.0f && u < 0.999f) Fx->Whizz(closest);
    }
    // The gun's own noises: a reload starting, the pump racked.
    if (Fx && n.Weapon) {
        const bool reloading = n.Weapon->IsReloading();
        if (reloading && !n.FxReloading) Fx->Play(CombatFx::Cue::Reload, n.Eye - glm::vec3(0.0f, 0.4f, 0.0f));
        n.FxReloading = reloading;
        const bool pumping = n.Pumping;
        if (pumping && !n.FxPumping) Fx->Play(CombatFx::Cue::Pump, n.Eye - glm::vec3(0.0f, 0.3f, 0.0f));
        n.FxPumping = pumping;
    }
    for (const FirstPersonPresentation::ShotHit& hit : n.Weapon->TakeShotHits()) {
        const float dist = glm::length(hit.Point - hit.Origin);
        if (hit.Entity == kPlayerEntity) {
            if (!p.Valid || p.Dead) continue;
            const HitZone zone = ZoneFromCapsuleHeight(hit.Point.y, p.Feet.y, p.Height);
            DamageEvent e;
            e.Source = Id(n.Root);
            e.Target = kPlayerEntity;
            e.Amount = DamageForHit(n.Gun, zone, dist) * m_DamageScale;
            e.Zone = zone;
            e.Point = hit.Point;
            e.Direction = hit.Direction;
            e.SourcePos = n.Eye;
            m_PlayerDamage.push_back(e);
            Callout(n, Bark::PlayerHurt);
            continue;
        }
        bool onNpc = false;
        for (auto& o : m_Npcs) {
            if (!o || o->Dead || o.get() == &n || Id(o->Root) != hit.Entity) continue;
            // A friend in the way (a pellet off the line, a step into the burst): it flinches and keeps
            // its head down, but squads don't kill their own.
            o->Suppression = std::min(1.0f, o->Suppression + 0.3f);
            o->Body.Flinch(world, hit.Direction);
            (void)dist;
            onNpc = true;
            break;
        }
        if (!onNpc && m_Impacts.size() < 256) m_Impacts.push_back({hit.Point, hit.Normal, hit.Entity, hit.HoleRadius});
    }
}

bool NpcDirector::OnPlayerHit(World& world, unsigned entity, const glm::vec3& point, const glm::vec3& origin, const glm::vec3& dir,
                              const FirstPersonWeaponGameplay& weapon, bool* killed, bool* head) {
    if (!m_Active) return false;
    for (auto& up : m_Npcs) {
        if (!up || Id(up->Root) != entity) continue;
        Npc& n = *up;
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
                    for (int i = 0; i < NpcRagdoll::kParts; ++i) {
                        const float d2 = glm::length(n.Ragdoll->PartPosition(i) - point);
                        if (d2 < best) { best = d2; part = i; }
                    }
                }
                const float amount = DamageForHit(weapon, HitZone::Torso, dist);
                const glm::vec3 d = glm::length(dir) > 1e-6f ? glm::normalize(dir) : glm::vec3(0.0f, 0.0f, 1.0f);
                n.Ragdoll->Shove(part, d * NpcRagdoll::PartMass(&m_RagdollCfg, part) * (m_RagdollCfg.CorpseShotBase + m_RagdollCfg.CorpseShotPerDamage * amount), point);
                if (Fx) Fx->Play(CombatFx::Cue::FleshHit, point, false, 0.5f);
            }
            return true;
        }
        const HitZone zone = part >= 0 ? ZoneOfRegion(RegionFromPart(part)) : ZoneFromCapsuleHeight(point.y, n.Feet.y, n.Crouched ? 1.26f : 1.8f);
        const float dmg = DamageForHit(weapon, zone, dist);
        if (Fx) Fx->Play(CombatFx::Cue::FleshHit, point, false, 0.7f);
        ApplyDamage(world, n, dmg, zone, point, dir, -1, part);
        if (killed) *killed = n.Dead;
        if (head) *head = zone == HitZone::Head;
        return true;
    }
    return false;
}

void NpcDirector::OnPlayerShotLine(const glm::vec3& origin, const glm::vec3& end) {
    // Rounds cracking past (or smacking into the cover) within 1.6 m of a head keep it down.
    const glm::vec3 seg = end - origin;
    const float len2 = glm::dot(seg, seg);
    if (len2 < 1e-4f) return;
    for (auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        const float t = std::clamp(glm::dot(up->SightEye - origin, seg) / len2, 0.0f, 1.0f);
        const float d = glm::length(origin + seg * t - up->SightEye);
        if (d < 1.6f && t > 0.02f) {
            up->Suppression = std::min(1.0f, up->Suppression + 0.22f * (1.6f - d));
            // A round within a metre makes most duck for a beat (the steadier, less often); not again straight away.
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            if (d < 1.0f && m_Now > up->CowerUntil + 0.8f && unit(m_Rng) < 0.65f - 0.35f * up->Skill)
                up->CowerUntil = m_Now + 0.35f + 0.3f * unit(m_Rng);
            if (!up->Mem.Known) {
                up->Mem.Known = true;
                up->Mem.Awareness = 1.0f;
                up->Mem.LastKnown = origin;
                up->Mem.LastSeen = m_Now;
                up->Mem.Uncertainty = 3.0f;
            }
        }
    }
}

void NpcDirector::ApplyDamage(World& world, Npc& n, float amount, HitZone zone, const glm::vec3& point, const glm::vec3& dir, int attacker,
                              int part) {
    if (n.Dead || amount <= 0.0f) return;
    const bool wasWounded = n.Wounded;
    // The finer region: from the bone when the hitboxes answered, else from the zone (the capsule's lower half is legs).
    const HitRegion region = part >= 0 ? RegionFromPart(part)
                             : zone == HitZone::Head ? HitRegion::Head
                             : zone == HitZone::Limb ? HitRegion::Leg
                                                     : HitRegion::Torso;
    n.Health -= amount;
    n.LastHurt = m_Now;
    n.LastHurtFrom = -dir;
    ++n.Hits;
    if (auto* h = world.Registry.try_get<HealthComponent>(n.Root)) h->Current = std::max(0.0f, n.Health);
    n.Suppression = std::min(1.0f, n.Suppression + 0.5f);
    n.TimeOnTarget *= 0.3f;
    if (attacker < 0) {
        // Shot by the player: now it knows where from.
        const bool was = n.Mem.Known;
        n.Mem.Known = true;
        n.Mem.Awareness = 1.0f;
        if (!n.Mem.Visible) {
            n.Mem.LastKnown = m_Player.Feet;
            n.Mem.LastSeen = m_Now;
            n.Mem.Uncertainty = 1.5f;
        }
        if (!was) n.ReactionLeft = std::max(n.ReactionLeft, 0.35f);
    }
    // Dead: out of health, a head shot that is enough, or hit again while already down.
    if (n.Health <= 0.0f || (zone == HitZone::Head && attacker < 0 && amount >= 60.0f) || (wasWounded && attacker < 0)) {
        Kill(world, n, dir, point, 30.0f + 0.4f * amount, part);
        return;
    }
    // It flinches the way it was hit (where it was hit), and a leg hit leaves it limping.
    n.Body.Flinch(world, dir, &point, part);
    n.NextThink = std::min(n.NextThink, m_Now); // rethink now
    if (region == HitRegion::Leg) n.LimpUntil = m_Now + m_Cfg.LimpTime;
    if (attacker < 0 && amount >= m_Cfg.HeavyHitDamage) {
        // A heavy hit: a beat of aim lost, and a small shove back along the round.
        n.StaggerUntil = m_Now + m_Cfg.StaggerTime;
        n.ReactionLeft = std::max(n.ReactionLeft, m_Cfg.StaggerTime);
        const glm::vec3 flat(dir.x, 0.0f, dir.z);
        if (glm::dot(flat, flat) > 1e-6f) n.PushVel += glm::normalize(flat) * 1.8f;
        if (glm::length(n.PushVel) > 2.5f) n.PushVel = glm::normalize(n.PushVel) * 2.5f;
    }
    // Close to death from a leg or the body: it may go down wounded instead of fighting on (once a life).
    if (attacker < 0 && !n.Dummy && !n.WoundRolled && n.Health < 0.2f * n.MaxHealth && (region == HitRegion::Leg || region == HitRegion::Torso)) {
        n.WoundRolled = true;
        if (std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng) < m_WoundChance) BecomeWounded(n);
    }
    if (n.Health < 0.35f * n.MaxHealth && !n.Wounded) Callout(n, Bark::Wounded);
}

void NpcDirector::BecomeWounded(Npc& n) {
    n.Wounded = true;
    n.WoundedAt = m_Now;
    n.Doing = Behaviour::Wounded;
    n.DoingSince = m_Now;
    n.Phase = 0;
    n.NextThink = m_Now;
    n.Role = NpcRole::Anchor;
    n.HasFlankToken = n.HasPushToken = false;
    // Said at once, whatever it shouted last.
    n.Callout = "Unit down, need assist";
    n.CalloutAt = m_Now;
    n.LastCallout = m_Now;
}

void NpcDirector::Kill(World& world, Npc& n, const glm::vec3& dir, const glm::vec3& point, float shove, int part) {
    (void)world;
    if (n.Dead) return;
    n.Dead = true;
    n.Wounded = false;
    n.Health = 0.0f;
    n.DiedAt = m_Now;
    n.Doing = Behaviour::Dead;
    n.TriggerHeld = false;
    m_Cover.Release(n.Index, m_Now);
    n.Hitboxes.reset(); // the ragdoll's parts take over the rounds
    if (n.Cct != PhysicsWorld::kNoCharacter) PhysicsWorld::DestroyNpcCharacter(n.Cct);
    n.Cct = PhysicsWorld::kNoCharacter;
    if (n.Agent >= 0) m_Crowd.Remove(n.Agent);
    n.Agent = -1;
    // The body goes down in this frame's late pose (LateUpdate -> FinishDeath): the animators and the gun run one more
    // frame, so the ragdoll starts from the pose it was seen in.
    // Where it fell: cover near here is a worse bet for a while.
    if (!n.Dummy) {
        m_DeathPos[m_DeathNext] = n.Feet;
        m_DeathTime[m_DeathNext] = m_Now;
        m_DeathNext = (m_DeathNext + 1) % kDeathMemory;
        m_DeathCount = std::min(m_DeathCount + 1, kDeathMemory);
    }
    n.DeathPending = true;
    NpcRagdoll::Capture(n.Body, m_DeathBones[n.Index].Bones); // last frame's pose, for the limbs' velocity
    m_DeathBones[n.Index].Dt = 0.0f;
    n.DeathDir = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec3(0.0f, 0.0f, 1.0f);
    n.DeathPoint = point;
    n.DeathShove = shove;
    n.DeathPart = part;
    if (n.Squad < (int)m_Squads.size() && !n.Dummy) { // a dummy going down is no loss to the squad
        Squad& s = m_Squads[(size_t)n.Squad];
        s.LastDeath = m_Now;
        for (int i : s.Members) {
            Npc* o = i < (int)m_Npcs.size() ? m_Npcs[(size_t)i].get() : nullptr;
            if (!o || o->Dead) continue;
            o->Morale = std::max(0.0f, o->Morale - 0.25f);
            if (glm::length(o->Feet - n.Feet) < 25.0f) Callout(*o, Bark::ManDown);
        }
    }
    if (m_Respawn) m_RespawnTimers.push_back(m_Now + m_RespawnDelay);
}

void NpcDirector::FinishDeath(World& world, Npc& n) {
    n.DeathPending = false;
    if (n.Weapon) n.Weapon->Stop(world);
    n.Weapon.reset();
    // Down: the animators stop and the physics takes the body, shoved along the round's line on the bone it struck.
    for (entt::entity e : n.Body.Pieces())
        if (world.Registry.valid(e)) world.Registry.remove<AnimatorControllerComponent>(e);
    n.Body.SetPoseOwnedElsewhere(true);
    n.FallSoundAt = m_Now + 0.45f;
    n.Ragdoll = std::make_unique<NpcRagdoll>();
    // The bones' velocity: the pose they had when the soldier was hit (last frame's, before the animators moved on) against the
    // dying frame's, over the real time between (a held frame has no new pose: no relative motion, just the body's).
    if (!n.Ragdoll->Start(n.Body, n.Velocity, n.DeathDir * n.DeathShove, n.DeathPoint, n.DeathPart, &m_RagdollCfg, &m_DeathBones[n.Index].Bones, m_DeathBones[n.Index].Dt)) {
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
            if (n.Body.BoneWorld(kBones[k], b)) pop = std::max(pop, glm::length(b - n.LastBones[k]));
        }
        n.DeathPop = pop;
    }
}

void NpcDirector::Respawns(World& world, AssetLibrary& assets, const PlayerSnapshot& p) {
    for (size_t i = 0; i < m_RespawnTimers.size();) {
        if (m_Now < m_RespawnTimers[i]) { ++i; continue; }
        int alive = 0;
        for (auto& n : m_Npcs) if (n && !n->Dead) ++alive;
        if (alive >= m_SquadSize) { m_RespawnTimers.erase(m_RespawnTimers.begin() + (long long)i); continue; }
        // The spawn furthest out of the player's sight (at least 20 m off, unseen if possible).
        int best = -1;
        float bestScore = -1e9f;
        for (int s = 0; s < (int)m_Spawns.size(); ++s) {
            const glm::vec3 at = m_Spawns[(size_t)s].Pos + glm::vec3(0.0f, 1.6f, 0.0f);
            const float d = p.Valid ? glm::length(at - p.Eye) : 50.0f;
            bool seen = false;
            if (p.Valid && !p.Dead) seen = !CoverSystem::Shielded(m_Spawns[(size_t)s].Pos, 1.6f, p.Eye);
            const float score = d + (seen ? -40.0f : 0.0f) + (d < 20.0f ? -100.0f : 0.0f);
            if (score > bestScore) { bestScore = score; best = s; }
        }
        if (best >= 0 && Spawn(world, assets, best) >= 0) m_RespawnTimers.erase(m_RespawnTimers.begin() + (long long)i);
        else m_RespawnTimers[i] = m_Now + 2.0f;
    }
}

void NpcDirector::OnPlayerRespawned() {
    for (auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        up->Mem = TargetMemory{};
        up->Mem.Awareness = 0.3f;
        up->FirstShot = true;
        up->Doing = Behaviour::Idle;
        up->Phase = 0;
        up->NextThink = m_Now;
    }
    for (auto& s : m_Squads) {
        s.Shared = TargetMemory{};
        s.PushUntil = 0.0f;
        s.FlankHolder = s.PushHolder = s.PincerHolder = -1;
        s.CoverRequest = s.CoverFirer = -1;
        s.Attackers.clear();
    }
    m_PlayerDamage.clear();
}

std::vector<DamageEvent> NpcDirector::TakePlayerDamage() {
    std::vector<DamageEvent> out;
    out.swap(m_PlayerDamage);
    return out;
}

std::vector<NpcDirector::Impact> NpcDirector::TakeImpacts() {
    std::vector<Impact> out;
    out.swap(m_Impacts);
    return out;
}

std::vector<CasingSpawn> NpcDirector::TakeEjections() {
    std::vector<CasingSpawn> out;
    out.swap(m_Ejections);
    return out;
}

void NpcDirector::DebugLines(std::vector<float>& out) const {
    auto line = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec4& c) {
        out.insert(out.end(), {a.x, a.y, a.z, c.r, c.g, c.b, c.a, b.x, b.y, b.z, c.r, c.g, c.b, c.a});
    };
    auto ring = [&](const glm::vec3& c, float r, const glm::vec4& col) {
        for (int i = 0; i < 16; ++i) {
            const float a0 = 6.2831853f * (float)i / 16.0f, a1 = 6.2831853f * (float)(i + 1) / 16.0f;
            line(c + glm::vec3(std::cos(a0), 0, std::sin(a0)) * r, c + glm::vec3(std::cos(a1), 0, std::sin(a1)) * r, col);
        }
    };
    for (const CoverPoint& c : m_Cover.Points()) {
        const glm::vec4 col = c.ClaimedBy >= 0 ? glm::vec4(1.0f, 0.3f, 0.2f, 0.9f) : c.High ? glm::vec4(0.3f, 0.5f, 1.0f, 0.6f)
                                                                                            : glm::vec4(0.3f, 1.0f, 0.4f, 0.6f);
        line(c.Pos + glm::vec3(0, 0.05f, 0), c.Pos + glm::vec3(0, 0.05f, 0) + c.Normal * 0.4f, col);
    }
    for (const auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        const Npc& n = *up;
        const glm::vec4 col = n.Mem.Visible ? glm::vec4(1.0f, 0.2f, 0.2f, 1.0f) : n.Mem.Known ? glm::vec4(1.0f, 0.7f, 0.1f, 1.0f)
                                                                                                : glm::vec4(0.6f, 1.0f, 0.6f, 1.0f);
        const glm::vec3 look = FrontOf(n.AimYaw, n.AimPitch);
        line(n.SightEye, n.SightEye + look * 3.0f, col);
        if (n.Mem.Known) {
            ring(n.Mem.LastKnown + glm::vec3(0, 0.05f, 0), std::max(0.3f, n.Mem.Uncertainty), glm::vec4(1.0f, 0.6f, 0.0f, 0.7f));
            line(n.SightEye, n.Mem.LastKnown + glm::vec3(0, 1.2f, 0), glm::vec4(1.0f, 0.6f, 0.0f, 0.25f));
        }
        if (n.HasGoal) line(n.Feet + glm::vec3(0, 0.1f, 0), n.Goal + glm::vec3(0, 0.1f, 0), glm::vec4(0.3f, 0.8f, 1.0f, 0.9f));
        if (n.HasAttackToken) ring(n.Feet + glm::vec3(0, 0.05f, 0), 0.5f, glm::vec4(1.0f, 0.1f, 0.1f, 1.0f));
        if (n.HasFlankToken) ring(n.Feet + glm::vec3(0, 0.08f, 0), 0.65f, glm::vec4(0.8f, 0.2f, 1.0f, 1.0f));
    }
}
