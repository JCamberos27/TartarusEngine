#include "NpcDirector.h"

#include "AssetLibrary.h"
#include "Audio/FoleyAudio.h"
#include "Combat/CombatFx.h"
#include "Components.h"
#include "FirstPersonPresentation.h"
#include "GameModuleAPI.h" // RaycastHit, QueryFilter, kPlayerEntity
#include "Log.h"
#include "NpcBrain.h"
#include "NpcConfig.h"
#include "OutfitSystem.h"
#include "Profiler.h"
#include "PhysicsWorld.h"
#include "ProjectPaths.h"
#include "SceneRenderer.h"
#include "SceneSerializer.h"
#include "World.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

constexpr float kRadius = kNpcCapsuleRadius;
constexpr float kStandCyl = 0.6f;    // capsule 1.8 m standing
constexpr float kCrouchCyl = 0.33f;  // 1.26 m crouched
constexpr float kDownedGrace = 0.25f; // seconds after going down wounded that body rounds don't finish it
constexpr float kDyingWalkSpeed = 0.68f; // m/s: the gut-shot stagger (InjuredBelly_Loco_Walk_Fwd's own pace)

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

// ---- squad callouts (plain scheduling, no audio) ----
namespace {

struct CallDef { float Priority, Cooldown, Duration; bool Responds; };
// Order = CallKind. Priority decides who may cut the channel; Duration is how long the call holds the channel;
// Responds: a squadmate may answer with a "copy".
const CallDef kCalls[(int)CallKind::Count] = {
    {7, 8.0f, 2.63f, true},   {5, 6.0f, 2.15f, false},  {6, 6.0f, 2.26f, false},  {5, 8.0f, 1.80f, true},
    {5, 8.0f, 1.79f, true},   {4, 8.0f, 1.67f, true},   {6, 10.0f, 1.99f, true},  {5, 10.0f, 2.18f, false},
    {8, 5.0f, 2.36f, true},   {9, 4.0f, 2.17f, false},  {3, 10.0f, 1.38f, false}, {4, 12.0f, 1.90f, false},
    {6, 20.0f, 1.91f, false}, {3, 12.0f, 1.69f, false}, {4, 10.0f, 1.77f, true},  {3, 10.0f, 1.90f, false},
    {2, 15.0f, 1.64f, false}, {1, 25.0f, 1.96f, false}, {8, 3.0f, 2.16f, false},  {0, 0.0f, 1.12f, false},
};
constexpr float kCallGap = 0.18f;       // silence on the channel after a call
constexpr float kCopyDelay = 0.3f;

// A repeatable pseudo-random 0..1 (the AI's own generator is left alone, so its stream is unchanged).
float Hash01(int a, float b) {
    const float v = std::sin((float)a * 12.9898f + b * 78.233f) * 43758.5453f;
    return v - std::floor(v);
}

} // namespace

NpcDirector::CallChannel& NpcDirector::CallChan(int squad) {
    squad = std::max(0, squad);
    while ((int)m_CallChannels.size() <= squad) m_CallChannels.emplace_back();
    return m_CallChannels[(size_t)squad];
}

void NpcDirector::Callout(Npc& n, CallKind ev) {
    if (n.Dummy) return; // no radio
    const CallDef& def = kCalls[(int)ev];
    if (m_Now - n.LastCallout < 2.5f && def.Priority < 7) return; // urgent calls skip the per-soldier gap
    CallChannel& c = CallChan(n.Squad);
    if (def.Cooldown > 0.0f && m_Now - c.LastEvent[(int)ev] < def.Cooldown) return;
    if (m_Now < c.BusyUntil) {
        if (def.Priority <= (float)c.OnAirPriority) return;
        c.BusyUntil = m_Now; // cut in
    }
    c.RespPending = false;
    c.OnAirPriority = (int)def.Priority;
    c.BusyUntil = m_Now + def.Duration + kCallGap;
    c.LastEvent[(int)ev] = m_Now;
    if (def.Responds) {
        // A squadmate to answer "copy": another one who's alive within earshot.
        bool anyone = false;
        for (const auto& up : m_Npcs) {
            if (!up || up->Dead || up->Index == n.Index || up->Squad != n.Squad) continue;
            if (glm::length(up->Feet - n.Feet) < 40.0f) { anyone = true; break; }
        }
        m_CallRng = m_CallRng * 1664525u + 1013904223u;
        if (anyone && (float)(m_CallRng >> 8) / 16777216.0f < 0.6f) {
            c.RespPending = true;
            c.RespAt = c.BusyUntil + kCopyDelay;
        }
    }
    n.LastCallout = m_Now;
    // Squadmates in earshot who aren't busy shooting glance toward whoever called.
    for (auto& o : m_Npcs) {
        if (!o || o->Dead || o.get() == &n || o->Squad != n.Squad || o->Intent.Fire) continue;
        if (glm::length(o->Feet - n.Feet) > 18.0f) continue;
        o->GlanceAt = n.Eye;
        o->GlanceUntil = m_Now + 0.7f + 0.6f * Hash01(o->Index, m_Now);
    }
}

void NpcDirector::UpdateCallouts(const PlayerSnapshot& p) {
    // A pending "copy" takes the channel once the call has ended.
    for (CallChannel& c : m_CallChannels) {
        if (!c.RespPending || m_Now < c.RespAt) continue;
        c.RespPending = false;
        if (m_Now >= c.BusyUntil) {
            const CallDef& def = kCalls[(int)CallKind::Copy];
            c.OnAirPriority = (int)def.Priority;
            c.BusyUntil = m_Now + def.Duration + kCallGap;
            c.LastEvent[(int)CallKind::Copy] = m_Now;
        }
    }
    // The player went down: the nearest soldier who was in the fight confirms it.
    if (p.Dead && !m_PlayerWasDead) {
        Npc* who = nullptr;
        float best = 1e9f;
        for (auto& up : m_Npcs) {
            if (!up || up->Dead || !up->Mem.Known) continue;
            const float d = glm::length(up->Feet - p.Feet);
            if (d < best) { best = d; who = up.get(); }
        }
        if (who) Callout(*who, CallKind::TargetDown);
    }
    m_PlayerWasDead = p.Dead;

    for (auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        Npc& n = *up;
        const bool reloading = n.Reloading && n.Mem.Known;
        if (reloading && !n.VcReloading) Callout(n, CallKind::Reloading);
        n.VcReloading = reloading;
        const bool covering = n.Intent.Suppress && n.TriggerHeld;
        if (covering && !n.VcCovering) Callout(n, CallKind::Covering);
        n.VcCovering = covering;
        const bool suspicious = !n.Mem.Known && n.Mem.Awareness > 0.3f;
        if (suspicious && !n.VcSuspicious) Callout(n, CallKind::Suspicious);
        n.VcSuspicious = suspicious;
        // Idle chatter, now and then, while nothing is going on.
        if (n.Doing == Behaviour::Idle && !n.Mem.Known) {
            if (n.NextChatter <= 0.0f) n.NextChatter = m_Now + 10.0f + 25.0f * Hash01(n.Index, m_Now);
            else if (m_Now >= n.NextChatter) {
                Callout(n, CallKind::Idle);
                n.NextChatter = m_Now + 25.0f + 35.0f * Hash01(n.Index + 7, m_Now);
            }
        }
    }
}

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
    bool Bare = false; // undressed for a Bare spawn: pooled only for another
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
        m_Cfg=s;
    } else m_Cfg=SquadSettingsComponent{};
    m_Cfg.SquadSize=std::clamp(m_Cfg.SquadSize,0,16);
    m_SquadSize=m_Cfg.SquadSize;m_RespawnDelay=m_Cfg.RespawnDelay;m_Difficulty=m_Cfg.Difficulty;m_DamageScale=m_Cfg.NpcDamageScale;m_Respawn=m_Cfg.Respawn;
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
        s.Weapon = std::clamp(c.Weapon, 0, 3);
        s.Squad = std::clamp(c.Squad, 0, 16);
        s.Skill = std::clamp(c.Skill, 0.0f, 1.0f);
        s.Brain = std::clamp(c.Brain, 0, 1);
        s.OutfitSeed = std::clamp(c.OutfitSeed, 0, 100000);
        // NPC_TEST_BARE=1 (--npc-test): every soldier bare, the animation and the skinning seen without clothing.
        static const bool allBare = [] {
            char* e = nullptr;
            size_t n = 0;
            _dupenv_s(&e, &n, "NPC_TEST_BARE");
            const bool on = e && *e && *e != '0';
            std::free(e);
            return on;
        }();
        s.Bare = c.Bare || allBare;
        const auto* order = reg.try_get<OrderComponent>(e);
        found.push_back({order ? order->Value : (int)entt::to_integral(e), s});
    }
    if (found.empty()) return false;
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& f : found) m_Spawns.push_back(f.second);
    ApplySettings(reg);
    // The soldier's body, read once.
    {
        std::ifstream in(std::filesystem::u8path(ProjectPaths::Resolve(m_Cfg.BodyPrefab)));
        if (!in) {
            Log::Error(std::string("Enemy AI: can't read project body prefab ") + m_Cfg.BodyPrefab);
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
        m_HoldSettings.GripInset = fpb.GripInset;
        m_HoldSettings.FingerCurlExtra = fpb.FingerCurlExtra;
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
        m_HoldSettings.FootSlide = IK::FootSlideFrom(fpb); // foot pinning and stride warping
    }
    m_Active = true;
    m_Started = false;
    m_Now = 0.0f;
    m_CallChannels.clear();
    m_CallRng = 0xBA4Cu;
    m_PlayerWasDead = false;
    Log::Info("Enemy AI: " + std::to_string(m_Spawns.size()) + " spawn(s), squad of " + std::to_string(m_SquadSize) + ".");
    return true;
}

void NpcDirector::Stop(World& world) {
    for (auto& n : m_Npcs)
        if (n) Despawn(world, *n);
    m_Npcs.clear();
    for (LooseWeapon& g : m_LooseGuns) g.Gun->Stop(world);
    m_LooseGuns.clear();
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
    m_CallChannels.clear();
    m_CallRng = 0xBA4Cu;
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
    // Only what the soldiers can walk to from their spawns: a building's roof is walkable to Recast too.
    std::vector<glm::vec3> seeds;
    for (const SpawnPoint& sp : m_Spawns) seeds.push_back(sp.Pos);
    const int unreachable = m_Nav.KeepReachable(seeds, glm::vec3(2.0f, 0.6f, 2.0f));
    const int cover = m_Cover.Build(m_Nav, CoverTuning{m_Cfg.CoverSpacing, m_Cfg.CoverReach, m_Cfg.CoverKneeHeight, m_Cfg.CoverHeadHeight, m_Cfg.CoverStep});
    m_Crowd.Init(m_Nav, 16, 0.6f);
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    char msg[256];
    std::snprintf(msg, sizeof msg, "Enemy AI: navigation mesh %s (%d polygons from %d triangles, %d out of the spawns' reach), %d cover points, %.0f ms.",
                  loaded ? "loaded" : "built", m_Nav.PolyCount(), (int)tris.size() / 3, unreachable, cover, ms);
    Log::Info(msg);
}

bool NpcDirector::LateStart(World& world, AssetLibrary& assets) {
    m_Started = true;
    BuildNav(world);
    // Both guns' spent cases now, with the navigation mesh: a soldier carrying the other gun spawning mid-fight would
    // otherwise import its case's mesh then.
    for (const auto& set : {m_Cfg.RifleAnimationSet, m_Cfg.ShotgunAnimationSet}) FirstPersonPresentation::WarmEjectAssets(assets, set);
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
    // The soldiers' and their guns' shader variants compiled now, at Play start, not on the frame each first draws.
    SceneRenderer::WarmShaderVariants(world);
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
    }
    if (built->Root == entt::null) {
        for (entt::entity e : created)
            if (reg.valid(e)) world.DestroyEntityAndChildren(e);
        return nullptr;
    }
    // A prefab saved before its wardrobe's body changed gets the new parts now, before the body (and its ragdoll) reads
    // its pieces: rebuilt later, a new piece (the hands) was left out of both and played the idle on its own.
    OutfitSystem::RefreshBody(world, assets, built->Root);
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

int NpcDirector::Spawn(World& world, AssetLibrary& assets, int spawnIndex) {
    if (spawnIndex < 0 || spawnIndex >= (int)m_Spawns.size() || m_SoldierJson.empty()) return -1;
    SpawnPoint sp = m_Spawns[(size_t)spawnIndex];
    sp.Pos += SpawnShift;
    if (TestDummies) {
        sp.Brain = 1;
        sp.Weapon = spawnIndex == 1 ? 3 : 0;
        sp.Bare = spawnIndex != 1;
    }
    PROFILE_SCOPE("AI Spawn");
    const auto spawnT0 = std::chrono::steady_clock::now();
    auto since = [&]() { return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - spawnT0).count(); };
    auto& reg = world.Registry;
    // A body from the pool (a corpse that has gone) when there is one: shown again, its animators as they were built.
    std::shared_ptr<SoldierBody> built;
    for (size_t k = m_Pool.size(); k-- > 0 && !built;) {
        if (m_Pool[k] && m_Pool[k]->Bare != sp.Bare) continue; // dressed and undressed bodies are kept apart
        built = std::move(m_Pool[k]);
        m_Pool.erase(m_Pool.begin() + (long long)k);
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
    } else if (sp.Bare) {
        // Bare: every clothing item off, so the wardrobe dresses the body in its own skin (as Character Outfit's Bare Body).
        if (OutfitSystem::Pieces(world, built->Root).empty()) OutfitSystem::AdoptExisting(world, assets, built->Root);
        Wardrobe::Request bare = OutfitSystem::CurrentRequest(world, built->Root);
        bare.Items.clear();
        const auto r = OutfitSystem::Apply(world, assets, built->Root, bare);
        if (!r.Ok) Log::Warn("Enemy AI: a Bare spawn kept its clothes - " + r.Error);
        built->Bare = true;
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
    n->Dummy = (ForceBrain >= 0 ? ForceBrain : sp.Brain) == 1;
    n->Skill = sp.Skill;

    // Feet on the walkable mesh, a little apart from anyone already standing there - and off the dead: a living capsule meets a
    // corpse's parts, and one put down in the pile on the spawn was thrown up onto it and out at up to 19 m/s. Stepped aside only
    // to somewhere walkable from the spawn (not through the wall into the next room); with nowhere clear, the spawn itself. Not on
    // the player either: in a small room the step aside put a soldier 0.3 m from him, inside his capsule (and under him).
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
        Log::Error("Enemy AI: the soldier's body has no animated skeleton.");
        m_Bodies.erase(root);
        world.DestroyEntityAndChildren(root);
        return -1;
    }
    n->Body.WarmHoldTables(world);
    const float bodyMs = since();
    n->Cct = PhysicsWorld::CreateNpcCharacter(Id(root), kRadius, kStandCyl, &feet.x);
    n->Agent = m_Crowd.Add(feet, kNpcNavRadius, 1.8f, 3.2f);
    // The soldier prefab's Health (Max, Invulnerable) when it has one; 100 otherwise.
    auto& health = reg.get_or_emplace<HealthComponent>(root);
    health.Max = std::clamp(health.Max, 1.0f, 100000.0f);
    health.Current = health.Max;
    n->Health = n->MaxHealth = health.Max;

    // The weapon: the player's own, on a camera at the soldier's eyes.
    int weapon = ForceWeapon >= 0 ? std::clamp(ForceWeapon, 0, 3) : sp.Weapon;
    if (weapon == 2) weapon = (int)(m_Rng() % 2u);
    FirstPersonControllerComponent cfg = *m_ViewConfig;
    cfg.AnimationSet = weapon == 1 ? m_Cfg.ShotgunAnimationSet : m_Cfg.RifleAnimationSet;
    // A copied player configuration must not override the chosen NPC weapon with slot 0.
    cfg.PrimaryWeaponPrefab = weapon == 1 ? m_Cfg.ShotgunPrefab : m_Cfg.RiflePrefab;
    cfg.SecondaryWeaponPrefab.clear();
    n->Class = weapon == 1 ? WeaponClass::Shotgun : WeaponClass::Rifle;
    FirstPersonPresentation::Options opt;
    opt.OwnerView = false;
    opt.HotReload = false;
    opt.CornerPeek = false;
    const float weaponT0 = since();
    if (weapon != 3) { // 3: None - unarmed, the arms keep the clips' pose
        n->Weapon = std::make_unique<FirstPersonPresentation>();
        if (!n->Weapon->Start(world, assets, cfg, opt) || !n->Weapon->IsActive()) {
            Log::Warn("Enemy AI: " + n->Name + " has no weapon (" + n->Weapon->LastError() + ").");
        } else {
            n->Gun = n->Weapon->Set().Gameplay;
            n->Weapon->SetLocomotionSpeeds(3.2f, 4.7f);
        }
    }
    const float weaponMs = since() - weaponT0;
    if (!reused) SceneRenderer::WarmShaderVariants(world, built->Root); // a new body's variants, before its first draw
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
        char msg[256];
        std::snprintf(msg, sizeof msg, "Enemy AI: %s spawned at (%.2f, %.2f, %.2f) from spawn %d (%.2f, %.2f, %.2f) in %.1f ms (entities %.1f%s, body %.1f, weapon %.1f ms).",
                      n->Name.c_str(), feet.x, feet.y, feet.z, spawnIndex, sp.Pos.x, sp.Pos.y, sp.Pos.z,
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
    if (n.Dropped) n.Dropped->Stop(world);
    n.Dropped.reset();
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
        // Health is the soldier's Health component: something else (the dev panel, a script) may have changed it.
        if (const auto* h = world.Registry.try_get<HealthComponent>(up->Root)) {
            up->Health = h->Current; up->MaxHealth = h->Max;
            if (h->Current <= 0.0f) { Kill(world, *up, {0, 0, 1}, up->Feet, 0); continue; }
        }
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
        // Down and not helped: it bleeds out - gut-shot; on its belly it drags itself on until it is finished off.
        if (n.Wounded && !n.Prone && m_Now - n.WoundedAt > m_Cfg.BleedOutTime) {
            Kill(world, n, FrontOf(n.AimYaw, 0.0f) * -1.0f, n.Feet + glm::vec3(0.0f, 0.8f, 0.0f), 6.0f);
            continue;
        }
        // A dummy keeps the intent it spawned with (stand, facing the way it was placed) until it goes down wounded.
        if (!Frozen && (!n.Dummy || n.Wounded)) {
            SubTimer timer(*this, SubBrain);
            NpcBrain::Think(*this, world, n, p, dt);
        }
        if (const auto it = m_Scripted.find(n.Index); it != m_Scripted.end()) { // a test's order
            n.Intent.Move = true;
            n.Intent.MoveTarget = it->second.first;
            n.Intent.Pace = it->second.second;
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
                if (n.Prone) speed = m_Cfg.CrawlSpeed;                      // a crawl
                else if (n.Dying) speed = kDyingWalkSpeed;                   // a stagger, doubled over
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

    UpdatePersistence(world, p, dt);
    int shooters = 0;
    for (auto& up : m_Npcs) {
        if (!up) continue;
        Npc& n = *up;
        if (n.Dead) continue; // (a corpse lies until UpdatePersistence lets it go)
        {
            SubTimer timer(*this, SubMove);
            if (n.Ragdoll) FollowFall(n, dt); // going down as a ragdoll: the capsule keeps under him
            else Move(world, n, dt);
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
    UpdateCallouts(p);
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
                if (!n.Mem.Known) { n.Mem.Known = true; Callout(n, CallKind::ContactRelay); }
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
            Callout(n, CallKind::Gunfire);
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
        Callout(n, CallKind::Contact);
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
    Callout(*best, CallKind::Covering);
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
    Callout(n, CallKind::Melee);
}

float NpcDirector::FloorRoom(const glm::vec3& feet, const glm::vec3& dir, float maxDist) const {
    const glm::vec3 d = glm::vec3(dir.x, 0.0f, dir.z) / std::max(1e-4f, glm::length(glm::vec2(dir.x, dir.z)));
    // A shoulder, at the height of a body on the floor (clear of the skirting and rugs); the room is to the sphere's far edge.
    constexpr float kShoulder = 0.18f, kHeight = 0.3f;
    const float o[3] = {feet.x, feet.y + kHeight, feet.z}, v[3] = {d.x, d.y, d.z};
    QueryFilter f;
    f.HitTriggers = 0;
    RaycastHit hit;
    const float sweep = std::max(0.01f, maxDist - kShoulder);
    return PhysicsWorld::SphereCastSolid(o, v, kShoulder, sweep, f, hit) && hit.Hit ? hit.Distance + kShoulder : maxDist;
}

int NpcDirector::BodiesAcross(const glm::vec3& feet, const glm::vec3& dir, float maxDist) const {
    const glm::vec3 d = glm::vec3(dir.x, 0.0f, dir.z) / std::max(1e-4f, glm::length(glm::vec2(dir.x, dir.z)));
    constexpr float kLane = 0.45f; // half the width of a body on the floor
    int bodies = 0;
    for (const auto& o : m_Npcs) {
        if (!o || !o->Dead || !o->Ragdoll || !o->Ragdoll->Active()) continue;
        for (int part : {0, 1, 2, 7, 8, 9, 10}) { // the pelvis, chest, head and legs: what lies across a crawl
            const glm::vec3 at = o->Ragdoll->PartPosition(part) - feet;
            const float along = at.x * d.x + at.z * d.z;
            if (at.y < 0.7f && along > 0.3f && along < maxDist && std::abs(at.x * d.z - at.z * d.x) < kLane) {
                ++bodies;
                break;
            }
        }
    }
    return bodies;
}

void NpcDirector::Move(World& world, Npc& n, float dt) {
    if (n.Cct == PhysicsWorld::kNoCharacter) return;
    glm::vec3 want(0.0f);
    if (n.Intent.Move && n.Agent >= 0) want = m_Crowd.Velocity(n.Agent);
    want.y = 0.0f;
    // Gut-shot, doubled over, he only walks forward: the crowd's avoidance never backs him away from where he's going.
    if (n.Dying && glm::dot(want, want) > 1e-6f) {
        glm::vec3 way = m_Crowd.NextCorner(n.Agent) - n.Feet;
        way.y = 0.0f;
        if (glm::length(way) > 0.05f) {
            way = glm::normalize(way);
            want = way * std::max(0.0f, glm::dot(want, way));
        }
    }
    // On his belly he is a two-metre body, not the upright capsule: he goes where his head points (no sideways slide; the
    // body steers toward the path, arcing round at a turn), and he stops short of a wall with his hands, not his pelvis. A
    // turn that would swing his head or the dragged foot into something he doesn't make.
    glm::vec3 steer(0.0f);
    bool headBlocked = false;
    if (n.Prone) {
        // He heads for the route's next corner at the crawl's pace - not the crowd's velocity, which is how the agent really
        // moved: his hands at a wall, it falls to nothing, and with it the turn that would get him off the wall.
        const float yaw = n.Body.Yaw();
        const glm::vec3 fwd(std::sin(yaw), 0.0f, std::cos(yaw));
        glm::vec3 toCorner(0.0f);
        if (n.Intent.Move && n.Agent >= 0) {
            toCorner = m_Crowd.NextCorner(n.Agent) - n.Feet;
            toCorner.y = 0.0f;
        }
        // (Pressed against a doorjamb, the crowd's agent can lose its corridor - its next corner is where it stands: he heads for
        // the goal itself then, and the stuck swing below gets him off the jamb.)
        if (n.Intent.Move && glm::length(toCorner) < 0.1f) {
            toCorner = n.Intent.MoveTarget - n.Feet;
            toCorner.y = 0.0f;
        }
        const float cornerDist = glm::length(toCorner);
        const float speed = cornerDist > 0.1f ? m_Cfg.CrawlSpeed : 0.0f;
        if (speed > 0.05f) {
            steer = toCorner / cornerDist;
            // Whiskers: the room ahead to either side at the hands' reach (and wider). A wall near one side turns him off it,
            // and in a doorway the two frames balance and he goes through the middle - the route only keeps a standing
            // body's width off a corner, not a two-metre one's.
            {
                const glm::vec3 turnPlus(std::cos(yaw), 0.0f, -std::sin(yaw)); // the way a larger yaw turns him
                constexpr float kReach = kProneHead + 0.4f;
                float lean = 0.0f;
                for (float a : {0.45f, 0.9f}) {
                    const glm::vec3 plus(std::sin(yaw + a), 0.0f, std::cos(yaw + a)), minus(std::sin(yaw - a), 0.0f, std::cos(yaw - a));
                    lean += (a < 0.6f ? 1.0f : 0.6f) * (FloorRoom(n.Feet, plus, kReach) - FloorRoom(n.Feet, minus, kReach));
                }
                // (Never past the way to the corner: the whiskers only bend it.)
                const glm::vec3 bent = steer + turnPlus * std::clamp(0.9f * lean, -0.8f, 0.8f);
                if (glm::dot(bent, steer) > 0.2f) steer = glm::normalize(bent);
            }
            // Stuck - his hands at a doorjamb or a corner, the route's corner straight on past it: he swings off toward the open
            // side, crawls clear on the angle, and the route takes him again (a few of these and he's through the middle).
            if (n.CrawlStuck > 0.3f) {
                constexpr float kSwing = 0.6f, kLook = kProneHead + 0.3f;
                // The side, once: the roomier (a doorway's two jambs alike, the route's side), then kept till he's clear - picked
                // afresh every frame it dithers between the two and he never turns.
                if (n.CrawlSwing == 0.0f) {
                    const glm::vec3 plus(std::sin(yaw + kSwing), 0.0f, std::cos(yaw + kSwing)), minus(std::sin(yaw - kSwing), 0.0f, std::cos(yaw - kSwing));
                    const float room = FloorRoom(n.Feet, plus, kLook) - FloorRoom(n.Feet, minus, kLook);
                    const float route = std::atan2(fwd.x * steer.z - fwd.z * steer.x, glm::dot(fwd, steer)); // (negative: at a larger yaw)
                    n.CrawlSwing = std::abs(room) > 0.1f ? (room > 0.0f ? 1.0f : -1.0f) : (route <= 0.0f ? 1.0f : -1.0f);
                    // A jamb (or a corner's edge) under one shoulder: straight ahead off each shoulder, the blocked side's lane
                    // tells him which way the open doorway is - away from it, whatever the wider room says.
                    const glm::vec3 turnPlus(std::cos(yaw), 0.0f, -std::sin(yaw));
                    const bool plusBlocked = FloorRoom(n.Feet + turnPlus * 0.25f, fwd, kProneHead + 0.1f) < kProneHead + 0.05f;
                    const bool minusBlocked = FloorRoom(n.Feet - turnPlus * 0.25f, fwd, kProneHead + 0.1f) < kProneHead + 0.05f;
                    if (plusBlocked != minusBlocked) n.CrawlSwing = plusBlocked ? -1.0f : 1.0f;
                    n.CrawlSwingYaw = yaw + n.CrawlSwing * 0.7f; // (40 degrees off: enough to clear a jamb, not to turn back)
                }
                // Round that far and still at the wall (a corner): another step the same way.
                if (std::abs(FirstPersonBodyWrapAngle(n.CrawlSwingYaw - yaw)) < glm::radians(3.0f)) n.CrawlSwingYaw = yaw + n.CrawlSwing * 0.7f;
                steer = glm::vec3(std::sin(n.CrawlSwingYaw), 0.0f, std::cos(n.CrawlSwingYaw));
            } else if (n.CrawlStuck <= 0.0f) {
                n.CrawlSwing = 0.0f;
            }
            const float off = std::atan2(fwd.x * steer.z - fwd.z * steer.x, glm::dot(fwd, steer)); // negative: the path is at a larger yaw
            if (std::abs(off) > 0.05f) {
                const float to = yaw - (off > 0.0f ? 0.2f : -0.2f); // a little way round
                const glm::vec3 next(std::sin(to), 0.0f, std::cos(to));
                const float headNow = FloorRoom(n.Feet, fwd, kProneHead), feetNow = FloorRoom(n.Feet, -fwd, kProneFeet);
                const float headNext = FloorRoom(n.Feet, next, kProneHead), feetNext = FloorRoom(n.Feet, -next, kProneFeet);
                // (Unless he is stuck - his hands at a wall a moment: in a corner every turn brushes something, and lying
                // there for good is worse than a foot through the skirting.)
                if (n.CrawlStuck < 0.3f &&
                    ((headNext < kProneHead && headNext < headNow - 0.01f) || (feetNext < kProneFeet && feetNext < feetNow - 0.01f)))
                    steer = fwd;
            }
            want = fwd * speed * std::max(0.7f, glm::dot(fwd, steer)); // (a hard turn eases off a little, no more)
            headBlocked = FloorRoom(n.Feet, fwd, kProneHead + 0.1f) < kProneHead + 0.05f;
            if (headBlocked) want = glm::vec3(0.0f);
        }
        n.CrawlStuck = headBlocked ? n.CrawlStuck + dt : std::max(0.0f, n.CrawlStuck - 2.0f * dt);
        // Over the dead: his capsule passes through them (it would snag, climb or bulldoze them), and the body on the floor
        // shoves aside the limbs it drags across - a part under his length is pushed out to the side it lies on, and on.
        PhysicsWorld::SetNpcPassesRagdolls(n.Cct, n.Recovered);
        if (n.Recovered && glm::length(n.Velocity) > 0.1f) {
            const glm::vec3 side(fwd.z, 0.0f, -fwd.x);
            for (auto& o : m_Npcs) {
                if (!o || !o->Dead || !o->Ragdoll || !o->Ragdoll->Active()) continue;
                for (int part = 0; part < NpcRagdoll::kParts; ++part) {
                    const glm::vec3 p = o->Ragdoll->PartPosition(part);
                    const glm::vec3 at = p - n.Feet;
                    const float along = glm::dot(at, fwd), across = glm::dot(at, side);
                    if (at.y > 0.6f || along < -kProneFeet || along > kProneHead || std::abs(across) > 0.4f) continue;
                    const glm::vec3 push = glm::normalize(side * (across >= 0.0f ? 1.0f : -1.0f) + fwd * 0.5f);
                    // (Never into a wall: a limb shoved against one goes through it, a thin wall for good.)
                    if (FloorRoom(glm::vec3(p.x, n.Feet.y, p.z), push, 0.45f) < 0.45f) continue;
                    o->Ragdoll->Shove(part, push * (6.0f * dt), p); // N s: about half a metre a second on a limb, eased
                }
            }
        }
    }
    n.FallSpeed -= m_Cfg.FallGravity * dt;
    glm::vec3 disp = (want + n.PushVel) * dt + glm::vec3(0.0f, n.FallSpeed * dt, 0.0f);
    if (n.Prone || n.Dying) disp += n.Body.FallTravel(); // a collapse carries him forward
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
    if ((n.Intent.Move && glm::length(want) > 0.5f && glm::length(n.Velocity) < 0.25f) || headBlocked) n.BlockedTime += dt;
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
    if (n.Prone || n.Dying) { // wounded: no stance to lean or duck from, no gun to aim; on the floor, no feet to stand on
        in.Prone = n.Prone;
        in.HurtLeg = n.HurtLeg;
        in.Dying = n.Dying;
        in.Collapse = n.Collapsed;
        in.Steer = steer;
        n.CrawlSteer = steer;
        in.Pivot = n.CrawlStuck > 0.3f;
        if (n.Prone || n.Collapsed) in.FootIK = false;
        in.Lean = in.Cower = 0.0f;
        in.Aiming = false;
    }
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
    if (float feet[2]; !FoleyAudio::Get().Tuning().StepsFromFeet || !n.Body.FootHeights(feet)) // else LateUpdate steps on the feet
        FoleyAudio::Get().NpcWalk(world, n.Index, n.Feet, n.Velocity, sprinting, dt);
    // (After the tick, before the animators: the weapon clears its triggers at the start of each frame.)
    if (n.WeaponAction) w.TriggerAction(n.WeaponAction);
    n.WeaponAction = nullptr;
}

void NpcDirector::LateUpdate(World& world, float dt, const PlayerSnapshot& p) {
    if (!m_Active || !m_Started) return;
    const auto t0 = std::chrono::steady_clock::now();
    for (LooseWeapon& g : m_LooseGuns) g.Gun->Update(world, dt);
    for (auto& up : m_Npcs) {
        if (!up) continue;
        Npc& n = *up;
        if (n.Dead) {
            if (n.DeathPending) {
                // One last animated frame, then the ragdoll takes the pose the body died in.
                n.LateDt += dt;
                if (n.Animate) { // (a held frame has no fresh pose to solve on: the ragdoll takes the one it has)
                    LatePose(world, n, n.LateDt, p, /*alive=*/false);
                    // The flinch of the rounds that killed him stays on that pose (dropped first, the hand it had kicked snapped
                    // back 20 cm in the frame he died), and the ragdoll takes it from there.
                    if (const auto it = m_Flinch.find(n.Index); it != m_Flinch.end() && it->second.Active(m_Now)) it->second.Apply(n.Body, m_Now);
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
            if (n.Dropped) n.Dropped->Update(world, dt);
            if (n.FallSoundAt > 0.0f && m_Now >= n.FallSoundAt) {
                n.FallSoundAt = -1.0f;
                if (Fx) Fx->Play(CombatFx::Cue::BodyFall, n.Ragdoll ? n.Ragdoll->Root() : n.Feet);
            }
            continue;
        }
        if (n.Dropped) n.Dropped->Update(world, dt); // shot down by a leg, it let its gun fall
        if (n.FallPending) {
            // Shot down by a leg this frame: one last animated frame (its late pose), then the ragdoll takes him down from it.
            n.LateDt += dt;
            if (n.Animate) {
                LatePose(world, n, n.LateDt, p, /*alive=*/true);
                m_DeathBones[n.Index].Dt = n.LateDt;
                n.LateDt = 0.0f;
            }
            if (TrackDeathPop) TrackJump(world, n);
            StartWoundFall(world, n);
        }
        if (n.Ragdoll) {
            UpdateWoundFall(world, n, dt, p);
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
        // Footsteps on the posed feet's touch-downs (a body without foot bones walks by the stride rule, in the weapon update).
        if (float feet[2]; n.Body.FootHeights(feet))
            FoleyAudio::Get().NpcFeet(world, n.Index, n.Feet, feet, n.Body.Sprinting(), glm::length(glm::vec2(n.Velocity.x, n.Velocity.z)), lateDt);
        UpdateHitboxes(n, p, /*posed=*/true);
        // The hit flinch goes on after the hitboxes took their pose (and the eye and the aim were read): visual only.
        if (const auto it = m_Flinch.find(n.Index); it != m_Flinch.end()) {
            if (it->second.Active(m_Now)) it->second.Apply(n.Body, m_Now);
            else m_Flinch.erase(it);
        }
        if (TrackDeathPop) {
            static const char* kBones[3] = {"head", "hand_l", "foot_l"};
            n.HaveLastBones = true;
            for (int k = 0; k < 3; ++k) n.Body.BoneWorld(kBones[k], n.LastBones[k]);
        }
        // Up off the wound fall, blending into the animated pose: the bones' jumps over the handover and the blend (tests). Blended
        // in, the body is his own again - the brain's crawl waits for that (a body crawling off while it still blends whips its limbs).
        if (!n.Recovered && n.CanCrawl && n.DownAt >= 0.0f) {
            const float jump = TrackJump(world, n);
            if (jump >= 0.0f) {
                if (n.HandoverJump < 0.0f) n.HandoverJump = jump;
                n.BlendJump = std::max(n.BlendJump, jump);
            }
            if (!n.Body.Recovering()) {
                n.Recovered = true;
                n.NextThink = m_Now;
            }
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
    // Reloading (or checking the gun) the gun's camera levels: the clips reach for the pouch and the shells in the gun's
    // frame, so with the gun pitched at the target the hand reached for them up or down in the air.
    const bool reloading = n.Weapon && n.Weapon->IsActive() && n.Weapon->HoldsOnBody();
    n.ReloadHold += ((reloading ? 1.0f : 0.0f) - n.ReloadHold) * (1.0f - std::exp(-dt / 0.15f));
    n.WeaponCam.Pitch = n.AimPitch * (1.0f - n.ReloadHold);
    n.Body.SetFreeHand(n.ReloadHold);
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
    // Up off a wall: if the gun as it would be held - out along the camera's line, and along the hold's own (the camera's front
    // with last frame's muzzle drop and its swing off to the side, while untucked: at a doorjamb or the arch's edge the camera's line
    // went clear through the opening while the gun, angled across the body, went into the jamb) - would meet something solid within its reach and a hand past, it comes up
    // toward high ready and in, by how far it would go in (turning in cover, the gun swept through the wall beside him). Measured
    // on the untucked lines, so the tuck can't clear its own check and drop the gun back in.
    {
        float want = 0.0f;
        glm::vec3 fp, muzzle, bore;
        if (alive && n.Weapon->MuzzleFrames(fp, muzzle, bore)) {
            const glm::vec3 front = FrontOf(n.WeaponCam.Yaw, n.WeaponCam.Pitch);
            const glm::vec3 side = glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::vec3 right = glm::dot(side, side) > 1e-6f ? glm::normalize(side) : glm::vec3(0.0f);
            if (n.GunTuck < 0.05f && glm::length(muzzle - n.Eye) > 0.1f) {
                const glm::vec3 line = glm::normalize(muzzle - n.Eye);
                n.GunReach = std::clamp(glm::length(muzzle - n.Eye), 0.5f, 1.1f);
                n.GunDrop = line.y - front.y;
                n.GunSide = glm::dot(line, right);
            }
            const glm::vec3 held = glm::normalize(front + glm::vec3(0.0f, n.GunDrop, 0.0f) + right * n.GunSide);
            const float reach = n.GunReach + 0.15f;
            QueryFilter f;
            f.HitTriggers = 0;
            const float o[3] = {n.Eye.x, n.Eye.y, n.Eye.z};
            for (const glm::vec3& d : {front, held}) {
                const float v[3] = {d.x, d.y, d.z};
                RaycastHit hit;
                if (!PhysicsWorld::RaycastSolid(o, v, reach, f, hit) || !hit.Hit) continue;
                const float w = std::clamp((reach - hit.Distance) / 0.45f, 0.0f, 1.0f);
                if (w <= want) continue;
                want = w;
                // Along the wall it would go into, the way the gun already leans (square on to it: either way, the same one each time).
                const glm::vec3 wall(hit.Normal[0], 0.0f, hit.Normal[2]);
                if (glm::dot(wall, wall) > 1e-4f) {
                    const glm::vec3 nrm = glm::normalize(wall), flat(front.x, 0.0f, front.z);
                    glm::vec3 along = flat - nrm * glm::dot(flat, nrm);
                    if (glm::dot(along, along) < 0.04f) along = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), nrm);
                    // Tucked, it stays on the side it went to: square on to the wall the nearer side flips frame to frame, and the
                    // gun swung across with the hand on it, 60 cm in a frame.
                    if (n.GunTuck > 0.05f && glm::dot(along, FrontOf(n.TuckYaw, 0.0f)) < 0.0f) along = -along;
                    // (And in a corner, one wall to the next, it turns at a hand's pace.)
                    const float to = glm::degrees(std::atan2(along.z, along.x)), most = 360.0f * dt;
                    n.TuckYaw = n.GunTuck > 0.05f ? WrapDeg(n.TuckYaw + std::clamp(WrapDeg(to - n.TuckYaw), -most, most)) : to;
                }
            }
        }
        // (Quick in, slower back out: it doesn't flutter at the wall's edge.) In: swung along the wall, up a little, drawn back.
        n.GunTuck += (want - n.GunTuck) * AiFollow(dt, want > n.GunTuck ? 0.06f : 0.25f);
        if (n.GunTuck > 1e-3f) {
            n.WeaponCam.Yaw = WrapDeg(n.WeaponCam.Yaw + WrapDeg(n.TuckYaw - n.WeaponCam.Yaw) * n.GunTuck);
            n.WeaponCam.Pitch = glm::mix(n.WeaponCam.Pitch, 35.0f, n.GunTuck);
            n.WeaponCam.Position -= FrontOf(n.AimYaw, 0.0f) * (0.12f * n.GunTuck);
        }
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
                                        dt, closeToPlayer, n.Weapon->ThirdPersonArms(), n.Weapon->GunSocket());
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
    // Going down as a ragdoll (the wound fall): the animators are held where they were until the body takes its pose back.
    if (n.Ragdoll) n.Animate = false;
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
                     !shotgun && (n.Tracer++ % 3) == 0, (std::uint32_t)n.Index + 1u, n.Weapon->MuzzleEffects());
        if (!p.Valid || p.Dead || (t.Hit && t.Entity == kPlayerEntity)) continue;
        const glm::vec3 seg = t.End - t.Origin;
        const float len2 = glm::dot(seg, seg);
        if (len2 < 1e-4f) continue;
        const float u = std::clamp(glm::dot(p.Eye - t.Origin, seg) / len2, 0.0f, 1.0f);
        const glm::vec3 closest = t.Origin + seg * u;
        if (const float miss = glm::length(closest - p.Eye); miss < Fx->FlybyReach() && u * std::sqrt(len2) > 3.0f && u < 0.999f) Fx->Whizz(closest, miss);
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
            if (m_FleshHits.size() < 64)
                m_FleshHits.push_back({hit.Point, hit.Direction, kPlayerEntity, -1, e.Amount, false, zone == HitZone::Head, false, hit.Pellets,
                                       hit.Origin, false});
            Callout(n, CallKind::PlayerHurt);
            continue;
        }
        bool onNpc = false;
        for (auto& o : m_Npcs) {
            if (!o || o->Dead || o.get() == &n || Id(o->Root) != hit.Entity) continue;
            // A friend in the way (a pellet off the line, a step into the burst): it flinches and keeps
            // its head down, but squads don't kill their own.
            o->Suppression = std::min(1.0f, o->Suppression + 0.3f);
            o->Body.Flinch(world, hit.Direction);
            if (m_FleshHits.size() < 64)
                m_FleshHits.push_back({hit.Point, hit.Direction, hit.Entity, -1, DamageForHit(n.Gun, HitZone::Torso, dist) * 0.5f,
                                       false, false, false, hit.Pellets, hit.Origin, false});
            onNpc = true;
            break;
        }
        if (!onNpc && m_Impacts.size() < 256) m_Impacts.push_back({hit.Point, hit.Normal, hit.Entity, hit.HoleRadius});
    }
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
        // Alive and going down as a ragdoll, the round may name one of its own parts past the hitboxes' eleven: the neck counts as
        // the chest, a hand as its forearm, a foot as its calf.
        if (part >= NpcRagdoll::kParts) {
            static const int kAs[NpcRagdoll::kRagParts - NpcRagdoll::kParts] = {1, 4, 6, 8, 10};
            part = kAs[std::min(part - NpcRagdoll::kParts, NpcRagdoll::kRagParts - NpcRagdoll::kParts - 1)];
        }
        const HitZone zone = part >= 0 ? ZoneOfRegion(RegionFromPart(part)) : ZoneFromCapsuleHeight(point.y, n.Feet.y, n.Crouched ? 1.26f : 1.8f);
        const float dmg = DamageForHit(weapon, zone, dist);
        if (Fx) Fx->Play(CombatFx::Cue::FleshHit, point, false, 0.7f);
        ApplyDamage(world, n, dmg, zone, point, dir, -1, part);
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
    // Its Health component holds the hit points (and may make it invulnerable: the hit registers, nothing else).
    auto* health = world.Registry.try_get<HealthComponent>(n.Root);
    if (health) {
        if (health->Invulnerable) return;
        n.Health = health->Current;
        n.MaxHealth = health->Max;
    }
    const bool wasWounded = n.Wounded;
    // Just gone down: the rest of the burst that dropped it doesn't finish it before it has even fallen (a head shot does).
    if (wasWounded && attacker < 0 && zone != HitZone::Head && m_Now - n.WoundedAt < kDownedGrace) return;
    // The finer region: from the bone when the hitboxes answered, else from the zone (the capsule's lower half is legs).
    const HitRegion region = part >= 0 ? RegionFromPart(part)
                             : zone == HitZone::Head ? HitRegion::Head
                             : zone == HitZone::Limb ? HitRegion::Leg
                                                     : HitRegion::Torso;
    n.Health -= amount;
    // The player's round in a leg puts it down on its belly, crawling (Leg Down chance: always, but for the tests), whatever
    // its health; one in the body that would leave it under 20% - or kill it - may put it down dying instead (Wound chance).
    // Once a life, left with at least a tenth of its health. (A player's round does 25-35 a hit, so "under 20% but alive"
    // alone almost never came up.)
    int downed = -2; // -2 not downed; else BecomeWounded's hurtLeg (-1 a knee, 0 / 1 that leg)
    if (attacker < 0 && !n.WoundRolled && !wasWounded && amount < n.MaxHealth &&
        (region == HitRegion::Leg || (region == HitRegion::Torso && n.Health < 0.2f * n.MaxHealth))) {
        n.WoundRolled = true;
        const bool leg = region == HitRegion::Leg;
        if (std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng) < (leg ? m_LegDownChance : m_WoundChance)) {
            downed = -1;
            if (leg) {
                // The hitbox's leg (7, 8 left; 9, 10 right), else the side of the body the round struck (model +X is its left).
                if (part >= 7 && part <= 10) downed = part <= 8 ? 0 : 1;
                else downed = glm::dot(point - n.Feet, glm::vec3(std::cos(n.Body.Yaw()), 0.0f, -std::sin(n.Body.Yaw()))) >= 0.0f ? 0 : 1;
            }
            n.Health = std::max(n.Health, 0.1f * n.MaxHealth);
        }
    }
    n.LastHurt = m_Now;
    n.LastHurtFrom = -dir;
    ++n.Hits;
    if (health) health->Current = std::max(0.0f, n.Health);
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
    if (m_RagdollCfg.HitFlinch && part >= 0) m_Flinch[n.Index].Hit(n.Body, part, dir, amount, m_Now, m_RagdollCfg); // a kick on the struck region
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
    if (downed > -2) {
        // The round that put it down, for the wound fall's ragdoll (a leg: StartWoundFall).
        n.FallHitDir = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec3(0.0f, 0.0f, 1.0f);
        n.FallHitPoint = point;
        n.FallHitShove = 30.0f + 0.4f * amount;
        n.FallHitPart = part;
        BecomeWounded(world, n, downed);
    }
    if (n.Health < 0.35f * n.MaxHealth && !n.Wounded) Callout(n, CallKind::Wounded);
}

void NpcDirector::BecomeWounded(World& world, Npc& n, int hurtLeg) {
    n.Wounded = true;
    n.WoundedAt = m_Now;
    // Shot down by a leg, on its belly and crawling; else gut-shot, doubled over, a few steps, then down. Either way the gun
    // falls from its hands (they go to the wound), and it goes down where it stands: no shove or old goal slides it about.
    n.Prone = hurtLeg >= 0;
    n.Dying = !n.Prone;
    n.HurtLeg = hurtLeg;
    n.TriggerHeld = false;
    // On its belly by way of the floor: the leg gives and he goes down as a powered ragdoll (after this frame's late pose, so it
    // starts from the pose he was seen in), not the collapse clip; the bones' pose now, for the limbs' velocity.
    if (n.Prone) {
        n.FallPending = true;
        n.Recovered = n.CanCrawl = false;
        n.DownAt = n.FallTime = -1.0f;
        n.HandoverJump = n.BlendJump = -1.0f;
        n.Body.SkipCollapseClip(true);
        NpcRagdoll::Capture(n.Body, m_DeathBones[n.Index].Bones);
        m_DeathBones[n.Index].Dt = 0.0f;
    }
    if (n.Weapon && n.Weapon->IsActive()) n.Dropped = NpcDroppedWeapon::Drop(world, n.Body, n.Weapon->WeaponEntity(), glm::vec3(0.0f, -1.0f, 0.0f));
    if (n.Weapon) n.Weapon->Stop(world);
    n.Weapon.reset();
    n.PushVel = glm::vec3(0.0f);
    n.StaggerUntil = 0.0f;
    n.Intent.Move = false;
    if (n.Agent >= 0) m_Crowd.Stop(n.Agent);
    n.HasGoal = false;
    n.Doing = Behaviour::Wounded;
    n.DoingSince = m_Now;
    n.Phase = -1; // the brain enters it next think: the stagger's goal, the cover to crawl to (NpcBrain::Enter)
    n.NextThink = m_Now;
    n.Role = NpcRole::Anchor;
    n.HasFlankToken = n.HasPushToken = false;
    // Said at once, whatever it shouted last.
    n.LastCallout = m_Now;
}

void NpcDirector::Kill(World& world, Npc& n, const glm::vec3& dir, const glm::vec3& point, float shove, int part) {
    if (n.Dead) return;
    n.Dead = true;
    n.Wounded = false;
    n.FallPending = false; // (killed in the frame it went down: the death's ragdoll, from the same pose)
    m_Scripted.erase(n.Index);
    if (auto* h = world.Registry.try_get<HealthComponent>(n.Root)) h->Current = 0.0f;
    n.Health = 0.0f;
    n.DiedAt = m_Now;
    n.Doing = Behaviour::Dead;
    n.TriggerHeld = false;
    m_Cover.Release(n.Index, m_Now);
    n.Hitboxes.reset(); // the ragdoll's parts take over the rounds
    n.DiedFlinching = FlinchActive(n.Index); // (tests: the death pop's log; the flinch goes with the last animated frame, LateUpdate)
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
            if (glm::length(o->Feet - n.Feet) < 25.0f) Callout(*o, CallKind::ManDown);
        }
    }
    if (m_Respawn) m_RespawnTimers.push_back(m_Now + m_RespawnDelay);
}

void NpcDirector::FinishDeath(World& world, Npc& n) {
    PROFILE_SCOPE("AI Death");
    n.DeathPending = false;
    // The gun leaves his hands (copied before the weapon presentation goes) and the arm solve lets go.
    // (Shot down by a leg, it dropped its gun already: that one lies where it fell.)
    const bool dropsGun = n.Weapon && n.Weapon->IsActive();
    if (dropsGun) n.Dropped = NpcDroppedWeapon::Drop(world, n.Body, n.Weapon->WeaponEntity(), n.DeathDir * n.DeathShove);
    if (n.Weapon) n.Weapon->Stop(world);
    n.Weapon.reset();
    // Killed while going down (or lying where he fell) as a ragdoll: the same ragdoll dies - the death's settings from now on and
    // the round's shove on the part it struck - so not a bone moves at the handover.
    if (n.Ragdoll && n.Ragdoll->Active()) {
        for (entt::entity e : n.Body.Pieces())
            if (world.Registry.valid(e)) world.Registry.remove<AnimatorControllerComponent>(e);
        n.Body.SetPoseOwnedElsewhere(true);
        m_DeathBones.erase(n.Index);
        int part = n.DeathPart;
        if (part < 0 || part >= NpcRagdoll::kRagParts) {
            float best = 1e9f;
            for (int i = 0; i < NpcRagdoll::kRagParts; ++i)
                if (const float d = glm::length(n.Ragdoll->PartPosition(i) - n.DeathPoint); d < best) { best = d; part = i; }
        }
        n.Ragdoll->Die(part, n.DeathDir, std::max(0.0f, (n.DeathShove - 30.0f) / 0.4f), n.DeathPoint, m_RagdollCfg);
        n.DeathPop = 0.0f;
        return;
    }
    // Down: the animators stop and the physics takes the body, shoved along the round's line on the bone it struck.
    for (entt::entity e : n.Body.Pieces())
        if (world.Registry.valid(e)) world.Registry.remove<AnimatorControllerComponent>(e);
    n.Body.SetPoseOwnedElsewhere(true);
    n.FallSoundAt = m_Now + 0.45f;
    n.Ragdoll = std::make_unique<NpcRagdoll>();
    n.Ragdoll->ProneDeath = n.Prone; // finished off on the floor: a flinch, then slack (no fall to make)
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
    // The gun stays in the hand nearest its grip for a moment, then the fingers let go.
    if (n.Dropped && dropsGun) {
        const glm::vec3 at = n.Dropped->Position(world);
        const int hand = glm::length(n.Ragdoll->PartPosition(NpcRagdoll::kHandR) - at) <= glm::length(n.Ragdoll->PartPosition(NpcRagdoll::kHandL) - at)
                             ? NpcRagdoll::kHandR : NpcRagdoll::kHandL;
        n.Dropped->SetOwner(n.Ragdoll->Id(), hand);
    }
    if (TrackDeathPop && n.HaveLastBones) {
        static const char* kBones[3] = {"head", "hand_l", "foot_l"};
        float pop = 0.0f;
        for (int k = 0; k < 3; ++k) {
            glm::vec3 b;
            if (!n.Body.BoneWorld(kBones[k], b)) continue;
            const float d = glm::length(b - n.LastBones[k]);
            pop = std::max(pop, d);
            if (d > 0.15f) // which bone, for the test's log
                std::printf("[NpcTest] %s death pop: %s %.0f cm (last animated (%.2f %.2f %.2f), ragdoll (%.2f %.2f %.2f))%s%s\n", n.Name.c_str(),
                            kBones[k], d * 100.0f, n.LastBones[k].x, n.LastBones[k].y, n.LastBones[k].z, b.x, b.y, b.z, n.Prone ? ", prone" : "",
                            n.DiedFlinching ? ", mid-flinch" : "");
        }
        n.DeathPop = pop;
    }
}

// Shot down by a leg: the leg gives and he goes down with the physics - a powered ragdoll of the living body, from the pose he
// was seen in, along the way he was going and the round's shove, biased forward and over the leg that gave (a man whose leg is
// shot out pitches over it; he isn't blown back like a corpse). The hurt leg is weak at once, the other holds a beat (Wound Fall
// Leg Hold) and gives, the arms brace toward the fall, and the walls and furniture he meets are the ragdoll's own contacts: each
// fall comes out its own. The capsule stays (it follows the pelvis, FollowFall) but never pushes him; the hitboxes ride his pose.
void NpcDirector::StartWoundFall(World& world, Npc& n) {
    PROFILE_SCOPE("AI Wound Fall");
    (void)world;
    n.FallPending = false;
    n.RollTries = 0;
    n.NextRollAt = 0.0f;
    n.RollHeaveUntil = -1.0f;
    n.TurnedOver = false;
    // The death's ragdoll settings, for a living body: the good leg's hold, the tone it keeps on the floor; always powered (the
    // drives are what make it fall like a man, and the motor knows when he is down).
    RagdollSettingsComponent cfg = m_RagdollCfg;
    cfg.PoweredRagdoll = true;
    cfg.StaggerLegStrength = cfg.WoundFallLegStrength;
    cfg.StaggerTime = cfg.WoundFallLegHold;
    cfg.ToneResidual = std::max(cfg.ToneResidual, cfg.WoundFallTone);
    const float yaw = n.Body.Yaw();
    const glm::vec3 fwd(std::sin(yaw), 0.0f, std::cos(yaw)), left(std::cos(yaw), 0.0f, -std::sin(yaw)); // (model +X is its left)
    const glm::vec3 shot(n.FallHitDir.x, 0.0f, n.FallHitDir.z);
    NpcRagdoll::Wound wound;
    // (The round's own line counts only across him or from behind: one from the front would tip him onto his back - he goes down
    // over the leg that gave, the way he faces, whichever way it came.)
    const float along = glm::dot(shot, fwd);
    const glm::vec3 fall = fwd + 0.6f * (n.HurtLeg == 0 ? left : -left) + 0.35f * (along < 0.0f ? shot - fwd * along : shot) + 0.15f * n.Velocity;
    wound.FallDir = glm::dot(fall, fall) > 1e-6f ? glm::normalize(fall) : fwd;
    wound.Leg = n.HurtLeg;
    wound.Push = std::max(cfg.WoundFallPush, 0.0f);
    if (TestFallBackwards) { // (a test's: over backwards, hard, so he lands on his back)
        TestFallBackwards = false;
        wound.FallDir = -fwd;
        wound.Push = 2.5f * std::max(cfg.WoundFallPush, 1.0f);
    }
    // The leg's part the round struck (else the hurt leg's thigh); half a killing round's shove: a leg round knocks the leg, not the man.
    const int part = n.FallHitPart >= 7 && n.FallHitPart <= 10 ? n.FallHitPart : (n.HurtLeg == 1 ? 9 : 7);
    const glm::vec3 impulse = n.FallHitDir * (0.5f * n.FallHitShove);
    n.Ragdoll = std::make_unique<NpcRagdoll>();
    const auto bones = m_DeathBones.find(n.Index);
    const bool started = n.Ragdoll->Start(n.Body, n.Velocity, impulse, n.FallHitPoint, part, &cfg, bones != m_DeathBones.end() ? &bones->second.Bones : nullptr,
                                          bones != m_DeathBones.end() ? bones->second.Dt : 0.0f, &wound);
    m_DeathBones.erase(n.Index);
    if (!started) {
        // (No ragdoll to be had: the collapse clip it is, and the brain's wait for it.)
        n.Ragdoll.reset();
        n.Body.SkipCollapseClip(false);
        n.Recovered = n.CanCrawl = true;
        n.DownAt = m_Now;
        return;
    }
    n.Body.SetPoseOwnedElsewhere(true);
    n.Ragdoll->Update(0.0f); // the pieces take the parts' first pose now
    if (n.Cct != PhysicsWorld::kNoCharacter) PhysicsWorld::IgnoreRagdollForNpc(n.Cct, n.Ragdoll->Id());
    // The gun he let go of passes through the hands it was in (and whatever of him it starts inside) until it is clear.
    if (n.Dropped) n.Dropped->SetOwner(n.Ragdoll->Id(), -1);
    m_Flinch.erase(n.Index); // (the flinch's kick is the animated pose's; the physics has the round now)
    n.FallStartedAt = m_Now;
    n.FallSoundAt = m_Now + 0.5f;
}

// The capsule under the falling body: toward the pelvis over the floor (at most 4 m/s), as low as a crouch; the walls stop it where
// they stop him. Where it stands when he is down is where his root goes (RecoverFromFall).
void NpcDirector::FollowFall(Npc& n, float dt) {
    if (n.Cct == PhysicsWorld::kNoCharacter || !n.Ragdoll || dt <= 0.0f) return;
    const glm::vec3 pelvis = n.Ragdoll->Root();
    glm::vec3 to(pelvis.x - n.Feet.x, 0.0f, pelvis.z - n.Feet.z);
    if (const float l = glm::length(to), most = 4.0f * dt; l > most) to *= most / l;
    if (!n.Crouched) {
        PhysicsWorld::ResizeNpcCharacter(n.Cct, kCrouchCyl);
        n.Crouched = true;
    }
    n.FallSpeed -= m_Cfg.FallGravity * dt;
    const glm::vec3 disp = to + glm::vec3(0.0f, n.FallSpeed * dt, 0.0f);
    const float d[3] = {disp.x, disp.y, disp.z};
    if (PhysicsWorld::MoveNpcCharacter(n.Cct, d, dt) & PhysicsWorld::CC_DOWN) n.FallSpeed = -1.0f;
    float feet[3];
    if (PhysicsWorld::GetNpcCapsule(n.Cct, feet, nullptr, nullptr)) {
        const glm::vec3 f(feet[0], feet[1], feet[2]);
        n.Velocity = glm::vec3(f.x - n.Feet.x, 0.0f, f.z - n.Feet.z) / dt;
        n.Feet = f;
    }
    if (n.Agent >= 0) m_Crowd.Sync(n.Agent, n.Feet);
    n.BlockedTime = 0.0f;
}

void NpcDirector::UpdateWoundFall(World& world, Npc& n, float dt, const PlayerSnapshot& p) {
    {
        SubTimer timer(*this, SubRagdoll);
        n.Ragdoll->Update(dt);
    }
    n.LateDt = 0.0f;
    if (n.FallSoundAt > 0.0f && m_Now >= n.FallSoundAt) {
        n.FallSoundAt = -1.0f;
        if (Fx) Fx->Play(CombatFx::Cue::BodyFall, n.Ragdoll->Root());
    }
    // He sees from his head as it lies; the rounds find him on the ragdoll's pose (the hitboxes posed from it, its parts too).
    if (glm::vec3 head; n.Body.BoneWorld("head", head)) n.Eye = head;
    UpdateHitboxes(n, p, /*posed=*/true);
    if (n.DownAt >= 0.0f) { // on his back: he rolls himself over, and crawls from his belly
        RollOver(world, n, dt);
        return;
    }
    TrackJump(world, n); // (the last ragdoll frame, for the handover's jump)
    // Down - the pelvis or chest on the ground - and every part slow: the animated pose takes him back. Never before the limbs have
    // had a moment to slump, and never later than Wound Recover Max Time (draped over something, still sliding).
    const RagdollSettingsComponent& c = m_RagdollCfg;
    const float t = m_Now - n.FallStartedAt;
    float lin = 0.0f, ang = 0.0f;
    n.Ragdoll->Motion(lin, ang);
    const bool down = n.Ragdoll->Motor().GroundedAt() >= 0.0f;
    if ((t >= c.WoundRecoverMinTime && down && lin < c.WoundRecoverSpeed) || t >= c.WoundRecoverMaxTime) RecoverFromFall(world, n);
}

// Down from the wound fall. Which way up he lies: the front of his chest (across the shoulders, up the spine) and of his pelvis
// (across the hips) - facing the floor, he is on his belly and crawls on; facing up, he is on his back, and a body on its back can't
// morph into a belly crawl: he stays as he fell (the ragdoll keeps him) and bleeds out there. On his belly the root goes under the
// pelvis (where the capsule followed it), facing along his body (pelvis to head, the crawl clips' axis), and every bone blends from
// where it lies into the prone idle (NpcBody::BeginRecover).
void NpcDirector::RecoverFromFall(World& world, Npc& n, bool turnOver) {
    const RagdollSettingsComponent& c = m_RagdollCfg;
    const glm::vec3 pelvis = n.Ragdoll->PartPosition(0), head = n.Ragdoll->PartPosition(2);
    const glm::vec3 spine = head - pelvis;
    auto front = [&](const char* l, const char* r) {
        glm::vec3 a(0.0f), b(0.0f);
        if (!n.Body.BoneWorld(l, a) || !n.Body.BoneWorld(r, b)) return 0.0f;
        const glm::vec3 f = glm::cross(a - b, spine); // its left across its up: its front
        return glm::length(f) > 1e-5f ? glm::normalize(f).y : 0.0f;
    };
    n.FallFrontUp = 0.5f * (front("upperarm_l", "upperarm_r") + front("thigh_l", "thigh_r"));
    n.DownAt = m_Now;
    n.FallTime = m_Now - n.FallStartedAt;
    n.CanCrawl = turnOver || n.FallFrontUp <= c.WoundFaceUp;
    n.TurnedOver = turnOver;
    n.Recovered = !n.CanCrawl; // (on his back that is all there is to it; on his belly, once the blend below is done)
    n.NextThink = m_Now;
    if (TrackDeathPop)
        std::printf("[NpcTest] %s down %.2f s after the hit, on his %s (front %.2f up), pelvis %.0f cm up\n", n.Name.c_str(), n.FallTime,
                    n.CanCrawl && !turnOver ? "belly" : "back", n.FallFrontUp, (pelvis.y - n.Feet.y) * 100.0f);
    if (TrackDeathPop && turnOver) std::printf("[NpcTest] %s: the heaves came to nothing - turned over onto his belly by the blend\n", n.Name.c_str());
    if (!n.CanCrawl) return;
    if (n.Cct != PhysicsWorld::kNoCharacter) PhysicsWorld::IgnoreRagdollForNpc(n.Cct, -1);
    if (n.Dropped) n.Dropped->ForgetOwner();
    // (Turned over, slower: from his back, every bone has half a turn to make round his spine - a roll, not a snap.)
    n.Body.BeginRecover(world, n.Feet, NpcYawOf(spine, n.Body.Yaw()), turnOver ? c.WoundRollBlendTime : c.WoundBlendTime, c.WoundBlendSpeed, "ProneIdle");
    n.Ragdoll.reset();
}

// Down on his back from the wound fall: he rolls himself over onto his belly - a heave up under the shoulder and the hip on one side,
// over toward the other (the good leg's: the hurt one ends on top), the chest and pelvis going with it - a few tries at most. Once
// he lies face down and still, the recovery takes him as it does a fall that landed so (UpdateWoundFall: RecoverFromFall).
void NpcDirector::RollOver(World& world, Npc& n, float dt) {
    const RagdollSettingsComponent& c = m_RagdollCfg;
    if (!n.Ragdoll || c.WoundRollImpulse <= 0.0f) return;
    // The heave under way, for a third of a second: a twist about his spine - the chest and the pelvis each turned by a pair of
    // equal and opposite pushes across them, the far side up and the near side down (a roll turns him, it doesn't lift him: a lift
    // has his whole weight to fight) - and the far arm and leg thrown over with it.
    constexpr float kHeaveTime = 0.35f;
    if (m_Now < n.RollHeaveUntil && dt > 0.0f) {
        // He pushes until he's over - his front well down - not for a set time: stopped short, he rocks back onto his back.
        glm::vec3 hl(0.0f), hr(0.0f);
        if (n.Body.BoneWorld("upperarm_l", hl) && n.Body.BoneWorld("upperarm_r", hr)) {
            const glm::vec3 fr = glm::cross(hl - hr, n.Ragdoll->PartPosition(2) - n.Ragdoll->PartPosition(0));
            if (glm::length(fr) > 1e-5f && glm::normalize(fr).y < -0.4f) {
                n.RollHeaveUntil = m_Now;
                return;
            }
        }
        const glm::vec3 up(0.0f, 1.0f, 0.0f), side = n.RollSide; // side: the way he rolls (that side goes down)
        const float j = c.WoundRollImpulse * (1.0f + (float)(n.RollTries - 1) / 3.0f) * dt / kHeaveTime;
        const bool ontoLeft = n.RollOntoLeft;                                   // (onto the side he chose) ...
        const int farArm = ontoLeft ? 5 : 3, farThigh = ontoLeft ? 9 : 7;      // ... the other comes up and over
        auto twist = [&](int part, float share) {
            const glm::vec3 at = n.Ragdoll->PartPosition(part);
            n.Ragdoll->Shove(part, up * (share * j), at - side * 0.15f); // the far side up ...
            n.Ragdoll->Shove(part, -up * (share * j), at + side * 0.15f); // ... the near side down
        };
        twist(1, 0.55f);
        twist(0, 0.35f);
        n.Ragdoll->Shove(farArm, (up * 0.15f + side * 0.25f) * j, n.Ragdoll->PartPosition(farArm));
        n.Ragdoll->Shove(farThigh, (up * 0.1f + side * 0.2f) * j, n.Ragdoll->PartPosition(farThigh));
        return;
    }
    const glm::vec3 pelvis = n.Ragdoll->PartPosition(0), head = n.Ragdoll->PartPosition(2);
    glm::vec3 la(0.0f), ra(0.0f);
    if (!n.Body.BoneWorld("upperarm_l", la) || !n.Body.BoneWorld("upperarm_r", ra)) return;
    const glm::vec3 spine = head - pelvis, across = la - ra; // up his spine; toward his left
    const glm::vec3 f = glm::cross(across, spine);
    const float frontUp = glm::length(f) > 1e-5f ? glm::normalize(f).y : 1.0f;
    float lin = 0.0f, ang = 0.0f;
    n.Ragdoll->Motion(lin, ang);
    // Face down and settling: over to the recovery (it measures again, and takes him into the crawl).
    if (frontUp <= c.WoundFaceUp - 0.1f && lin < 2.0f * c.WoundRecoverSpeed) {
        n.DownAt = -1.0f;
        n.FallStartedAt = m_Now - c.WoundRecoverMinTime;
        return;
    }
    if (TrackDeathPop && (int)(m_Now * 2.0f) != (int)((m_Now - 1.0f / 60.0f) * 2.0f))
        std::printf("[NpcTest] %s on his back: front %.2f up, parts %.2f m/s, %.1f s down, tries %d (impulse %.0f, tries %.0f)\n", n.Name.c_str(),
                    frontUp, lin, m_Now - n.DownAt, n.RollTries, c.WoundRollImpulse, c.WoundRollTries);
    if (m_Now < n.NextRollAt || m_Now - n.DownAt < 0.6f || lin > c.WoundRecoverSpeed) return;
    // Every heave spent (the last one settled) and still on his back - wedged, or the floor wouldn't give: he turns himself over
    // anyway, the animated pose taking him from how he lies onto his belly (forgiving, not physical: a man on his back for good
    // looked broken, not hurt).
    if (n.RollTries >= (int)c.WoundRollTries) {
        if (c.WoundRollBlendTime > 0.0f) RecoverFromFall(world, n, /*turnOver=*/true);
        return;
    }
    ++n.RollTries;
    n.NextRollAt = m_Now + 1.8f;
    // Over onto the good leg's side (the hurt one ends on top): his right with the left leg hurt, else his left - unless he lies
    // along a wall on that side (a man down beside one rolls away from it: every heave into it came to nothing). The body loosens
    // first (settled, its damping and friction would swallow the effort), then the heave runs (above).
    n.RollOntoLeft = n.HurtLeg != 0;
    const glm::vec3 left = glm::length(glm::vec2(across.x, across.z)) > 1e-5f ? glm::normalize(glm::vec3(across.x, 0.0f, across.z)) : glm::vec3(0.0f);
    if (glm::dot(left, left) > 0.0f) {
        // The room each side of his chest and hips, at a body's height off the floor (a roll travels about a body's width).
        constexpr float kRollReach = 1.0f, kRollNeeds = 0.7f;
        auto room = [&](const glm::vec3& dir) {
            float r = kRollReach;
            for (int part : {0, 1}) {
                const glm::vec3 at = n.Ragdoll->PartPosition(part);
                r = std::min(r, FloorRoom(glm::vec3(at.x, at.y - 0.15f, at.z), dir, kRollReach));
            }
            return r;
        };
        const float good = room(n.RollOntoLeft ? left : -left), bad = room(n.RollOntoLeft ? -left : left);
        if (good < kRollNeeds && bad > good + 0.1f) n.RollOntoLeft = !n.RollOntoLeft;
        if (TrackDeathPop)
            std::printf("[NpcTest] %s rolls toward his %s (room %.2f m on the good leg's side, %.2f m on the other)\n", n.Name.c_str(),
                        n.RollOntoLeft ? "left" : "right", good, bad);
    }
    n.RollSide = left * (n.RollOntoLeft ? 1.0f : -1.0f);
    n.RollHeaveUntil = m_Now + 0.8f; // (at most: it stops once he's over)
    n.Ragdoll->Loosen(1.8f);
    if (TrackDeathPop)
        std::printf("[NpcTest] %s rolls over (try %d, front %.2f up)\n", n.Name.c_str(), n.RollTries, frontUp);
}

float NpcDirector::TrackJump(const World& world, Npc& n) {
    static const char* kBones[6] = {"pelvis", "head", "hand_l", "hand_r", "foot_l", "foot_r"};
    float jump = n.HaveJumpBones ? 0.0f : -1.0f;
    int worst = -1;
    for (int k = 0; k < 6; ++k) {
        glm::vec3 b;
        if (!n.Body.BoneWorld(kBones[k], b)) continue;
        if (n.HaveJumpBones && glm::length(b - n.JumpBones[k]) > jump) { jump = glm::length(b - n.JumpBones[k]); worst = k; }
        n.JumpBones[k] = b;
    }
    n.HaveJumpBones = true;
    if (TrackDeathPop && n.DownAt >= 0.0f && jump > 0.05f && worst >= 0) // which bone, for the test's log
        std::printf("[NpcTest] %s jump: %s %.1f cm (%.2f s after down, blend %s, %s)\n", n.Name.c_str(), kBones[worst], jump * 100.0f,
                    n.DownAt >= 0.0f ? m_Now - n.DownAt : -1.0f, n.Body.Recovering() ? "on" : "off", n.Body.StateName(world).c_str());
    return jump;
}

bool NpcDirector::PlayerSees(const PlayerSnapshot& p, const glm::vec3& point) {
    if (!p.Valid) return false;
    const glm::vec3 d = point - p.Eye;
    const float dist = glm::length(d);
    if (dist < 2.0f) return true;   // at the player's feet: as good as seen (it would vanish at the screen's edge)
    if (dist > 250.0f) return false;
    // A cone a little wider than any screen's view (the camera's field of view isn't the director's to know).
    const float fl = glm::length(p.Forward);
    if (fl > 1e-4f && glm::dot(d / dist, p.Forward / fl) < std::cos(glm::radians(62.0f))) return false;
    return !CoverSystem::Shielded(point, 0.0f, p.Eye);
}

// Corpses and the guns they dropped lie at least Corpse Time (a gun: its own Lifetime) and go only once the player hasn't seen
// them for Unseen Time; past Max Corpses the oldest unseen go first, whatever their age.
void NpcDirector::UpdatePersistence(World& world, const PlayerSnapshot& p, float dt) {
    (void)dt;
    std::vector<Npc*> corpses;
    int guns = (int)m_LooseGuns.size();
    for (auto& up : m_Npcs)
        if (up && up->Dead && !up->DeathPending) { corpses.push_back(up.get()); if (up->Dropped) ++guns; }
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
        // The gun has its own time (0: it goes with the corpse).
        if (n->Dropped && n->Dropped->Settings().Lifetime > 0.0f) {
            n->Dropped->ForgetOwner();
            LooseWeapon g;
            g.Gun = std::move(n->Dropped);
            g.DroppedAt = n->DiedAt;
            g.UnseenSince = n->UnseenSince;
            m_LooseGuns.push_back(std::move(g));
        } else if (n->Dropped) {
            --guns;
        }
        Despawn(world, *n, /*keepBody=*/true);
        for (auto& up : m_Npcs)
            if (up.get() == n) { up.reset(); break; }
    }
    // The loose guns, oldest first.
    std::stable_sort(m_LooseGuns.begin(), m_LooseGuns.end(), [](const LooseWeapon& a, const LooseWeapon& b) { return a.DroppedAt < b.DroppedAt; });
    int gunsOver = guns - cap;
    for (size_t k = 0; k < m_LooseGuns.size();) {
        LooseWeapon& g = m_LooseGuns[k];
        const bool seen = PlayerSees(p, g.Gun->Position(world));
        if (seen) g.UnseenSince = -1.0f;
        else if (g.UnseenSince < 0.0f) g.UnseenSince = m_Now;
        const bool unseenLong = !seen && m_Now - g.UnseenSince >= m_Cfg.UnseenTime;
        if (unseenLong && (gunsOver > 0 || m_Now - g.DroppedAt > g.Gun->Settings().Lifetime)) {
            g.Gun->Stop(world);
            m_LooseGuns.erase(m_LooseGuns.begin() + (long long)k);
            --gunsOver;
            continue;
        }
        ++k;
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

std::vector<NpcDirector::FleshHit> NpcDirector::TakeFleshHits() {
    std::vector<FleshHit> out;
    out.swap(m_FleshHits);
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
