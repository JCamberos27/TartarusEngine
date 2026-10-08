#include "../Scripting/GameFrames.h"
#include "../Scripting/ScriptRuntime.h"
#include "../Scripting/NpcDefinitions.h"
#include "../Scripting/ScriptComponent.h"
#include <json.hpp>
#include <stdexcept>
#include "NpcDirector.h"

#include "AssetLibrary.h"
#include "Audio/FoleyAudio.h"
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

Scripting::NpcMemoryFrame PackMemory(const TargetMemory& m) {
    Scripting::NpcMemoryFrame f;f.Known=m.Known;f.Visible=m.Visible;f.Awareness=m.Awareness;f.LastSeen=m.LastSeen;f.LastHeard=m.LastHeard;f.Uncertainty=m.Uncertainty;
    f.LastKnown={m.LastKnown.x,m.LastKnown.y,m.LastKnown.z};f.LastVelocity={m.LastVelocity.x,m.LastVelocity.y,m.LastVelocity.z};return f;
}
TargetMemory UnpackMemory(const Scripting::NpcMemoryFrame& f) {
    TargetMemory m;m.Known=f.Known!=0;m.Visible=f.Visible!=0;m.Awareness=f.Awareness;m.LastSeen=f.LastSeen;m.LastHeard=f.LastHeard;m.Uncertainty=f.Uncertainty;
    m.LastKnown={f.LastKnown.x,f.LastKnown.y,f.LastKnown.z};m.LastVelocity={f.LastVelocity.x,f.LastVelocity.y,f.LastVelocity.z};return m;
}
Scripting::NpcPlayerFrame PackPlayer(const PlayerSnapshot& p) {
    Scripting::NpcPlayerFrame f;f.Valid=p.Valid;f.Crouched=p.Crouched;f.Dead=p.Dead;f.Fired=p.Fired;f.Reloading=p.Reloading;f.Sprinting=p.Sprinting;f.Height=p.Height;f.Radius=p.Radius;f.Health=p.Health;
    f.Eye={p.Eye.x,p.Eye.y,p.Eye.z};f.Feet={p.Feet.x,p.Feet.y,p.Feet.z};f.Velocity={p.Velocity.x,p.Velocity.y,p.Velocity.z};f.Forward={p.Forward.x,p.Forward.y,p.Forward.z};return f;
}
Scripting::Vec3 Pack(const glm::vec3& v){return {v.x,v.y,v.z};}
glm::vec3 Unpack(const Scripting::Vec3& v){return {v.x,v.y,v.z};}
Scripting::NpcIntentFrame PackIntent(const NpcIntent& in) {
    Scripting::NpcIntentFrame f;f.MoveTarget=Pack(in.MoveTarget);f.AimPoint=Pack(in.AimPoint);f.LookPoint=Pack(in.LookPoint);f.Lean=in.Lean;f.Cower=in.Cower;
    f.Move=in.Move;f.Crouch=in.Crouch;f.Aim=in.Aim;f.FaceAim=in.FaceAim;f.Fire=in.Fire;f.Suppress=in.Suppress;f.Reload=in.Reload;f.BlindFire=in.BlindFire;f.Pace=(int)in.Pace;return f;
}
Scripting::NpcLifeFrame Life(const Npc& n,const SquadSettingsComponent& cfg,float now,float dt=0) {
    Scripting::NpcLifeFrame f;f.Intent=PackIntent(n.Intent);f.Memory=PackMemory(n.Mem);f.Feet=Pack(n.Feet);f.Eye=Pack(n.SightEye);f.Velocity=Pack(n.Velocity);f.Push=Pack(n.PushVel);
    f.Now=now;f.Dt=dt;f.WoundedAt=n.WoundedAt;f.DiedAt=n.DiedAt;f.BleedOutTime=cfg.BleedOutTime;f.CorpseTime=cfg.CorpseTime;f.CrawlSpeed=cfg.CrawlSpeed;f.LimpScale=cfg.LimpSpeedScale;f.LimpUntil=n.LimpUntil;f.StaggerUntil=n.StaggerUntil;
    f.Gravity=cfg.FallGravity;f.FallSpeed=n.FallSpeed;f.BlockedTime=n.BlockedTime;f.MeleeAt=n.MeleeAt;f.MeleeTime=cfg.MeleeTime;f.PlayerDistance=n.PlayerDist;f.FootIKRange=cfg.FootIKRange;
    f.Suppression=n.Suppression;f.CowerUntil=n.CowerUntil;f.Skill=n.Skill;f.Morale=n.Morale;f.Dead=n.Dead;f.Wounded=n.Wounded;f.OnScreen=n.OnScreen;return f;
}
template<class Frame> void Project(const char* operation,Frame& frame){if(!Scripting::InvokeProject(operation,&frame,sizeof frame))throw std::runtime_error(std::string("Project policy unavailable: ")+operation);}

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

NpcDirector::CallChannel& NpcDirector::CallChan(int squad) {
    squad = std::max(0, squad);
    while ((int)m_CallChannels.size() <= squad) m_CallChannels.emplace_back();
    return m_CallChannels[(size_t)squad];
}

void NpcDirector::ProjectCallouts(int operation,int caller,int kind,const PlayerSnapshot& player) {
    for(const auto& n:m_Npcs)if(n)CallChan(n->Squad);
    static thread_local std::vector<Scripting::NpcCallMemberFrame> members;
    static thread_local std::vector<Scripting::NpcCallChannelFrame> channels;
    members.clear();channels.clear();
    for(const auto& n:m_Npcs) {
        Scripting::NpcCallMemberFrame f;f.Exists=n!=nullptr;
        if(n){f.Feet={n->Feet.x,n->Feet.y,n->Feet.z};f.Eye={n->Eye.x,n->Eye.y,n->Eye.z};f.GlanceAt={n->GlanceAt.x,n->GlanceAt.y,n->GlanceAt.z};f.LastCallout=n->LastCallout;f.GlanceUntil=n->GlanceUntil;f.Awareness=n->Mem.Awareness;f.NextChatter=n->NextChatter;
            f.Dummy=n->Dummy;f.Dead=n->Dead;f.Fire=n->Intent.Fire;f.Known=n->Mem.Known;f.Reloading=n->Reloading;f.Suppress=n->Intent.Suppress;f.TriggerHeld=n->TriggerHeld;f.VcReloading=n->VcReloading;f.VcCovering=n->VcCovering;f.VcSuspicious=n->VcSuspicious;f.Index=n->Index;f.Squad=n->Squad;f.Behaviour=(int)n->Doing;}
        members.push_back(f);
    }
    for(const auto& c:m_CallChannels){Scripting::NpcCallChannelFrame f;f.BusyUntil=c.BusyUntil;f.RespAt=c.RespAt;f.OnAirPriority=c.OnAirPriority;f.RespPending=c.RespPending;std::copy_n(c.LastEvent,(int)CallKind::Count,f.LastEvent);channels.push_back(f);}
    Scripting::NpcCalloutsFrame f;f.Operation=operation;f.Caller=caller;f.Kind=kind;f.Now=m_Now;f.Player=PackPlayer(player);f.PlayerWasDead=m_PlayerWasDead;f.Random=(int)m_CallRng;
    f.MemberCount=(int)members.size();f.ChannelCount=(int)channels.size();f.Members=reinterpret_cast<std::uintptr_t>(members.data());f.Channels=reinterpret_cast<std::uintptr_t>(channels.data());
    if(!Scripting::InvokeProject("npc.callouts",&f,sizeof f))throw std::runtime_error("Project squad callouts unavailable");
    m_PlayerWasDead=f.PlayerWasDead!=0;m_CallRng=(std::uint32_t)f.Random;
    for(size_t i=0;i<members.size();++i)if(auto& n=m_Npcs[i]) {const auto& v=members[i];n->LastCallout=v.LastCallout;n->GlanceAt={v.GlanceAt.x,v.GlanceAt.y,v.GlanceAt.z};n->GlanceUntil=v.GlanceUntil;n->NextChatter=v.NextChatter;n->VcReloading=v.VcReloading!=0;n->VcCovering=v.VcCovering!=0;n->VcSuspicious=v.VcSuspicious!=0;}
    for(size_t i=0;i<channels.size();++i){const auto& v=channels[i];auto& c=m_CallChannels[i];c.BusyUntil=v.BusyUntil;c.RespAt=v.RespAt;c.OnAirPriority=v.OnAirPriority;c.RespPending=v.RespPending!=0;std::copy_n(v.LastEvent,(int)CallKind::Count,c.LastEvent);}
}
void NpcDirector::Callout(Npc& npc,CallKind kind) {ProjectCallouts(0,npc.Index,(int)kind,m_Player);}
void NpcDirector::UpdateCallouts(const PlayerSnapshot& player) {ProjectCallouts(1,-1,0,player);}

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
        m_Cfg=s;
    } else m_Cfg=Scripting::DefaultSquadDefinition();
    m_SquadSize=m_Cfg.SquadSize;m_RespawnDelay=m_Cfg.RespawnDelay;m_Difficulty=m_Cfg.Difficulty;m_DamageScale=m_Cfg.NpcDamageScale;m_Respawn=m_Cfg.Respawn;
    if (auto rag = reg.view<const RagdollSettingsComponent>(); rag.begin() != rag.end()) m_RagdollCfg = reg.get<const RagdollSettingsComponent>(*rag.begin());
    else m_RagdollCfg = RagdollSettingsComponent();
}

bool NpcDirector::Start(World& world, AssetLibrary& assets, const FirstPersonControllerComponent* playerConfig) {
    (void)assets;
    Stop(world);Scripting::SyncNpcDefinitions(world);
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
        m_HoldSettings.ReachSlack = fpb.ReachSlack;
        m_HoldSettings.ShoulderLineMatch = fpb.ShoulderLineMatch;
        m_HoldSettings.SpineAim = fpb.SpineAim;
        m_HoldSettings.SpineAimDown = fpb.SpineAimDown;
        m_HoldSettings.SpineStability = fpb.SpineStability;
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
    std::string planText;if(!Scripting::RequestProject("npc.spawn-plan",nlohmann::json{{"size",m_SquadSize},{"spawns",m_Spawns.size()},{"respawn",m_Respawn},{"body",!m_SoldierJson.empty()}}.dump(),planText))return false;
    const auto plan=nlohmann::json::parse(planText);const int want=plan.at("count").get<int>();
    for(int i=0;i<want;++i)Spawn(world,assets,i%(int)m_Spawns.size());
    for(int i=0;i<plan.at("spares").get<int>();++i)
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
    std::string nameText;Scripting::RequestProject("npc.name",nlohmann::json(m_NextName++).dump(),nameText);n->Name=nlohmann::json::parse(nameText).get<std::string>();
    n->Root = root;
    if (auto* name = reg.try_get<NameComponent>(root)) name->Name = "[Runtime] " + n->Name;
    n->SpawnIndex = spawnIndex;
    n->Squad = sp.Squad;
    n->Dummy = sp.Brain == 1;
    n->Skill = sp.Skill;

    std::vector<Scripting::NpcMateFrame> members;
    for(const auto& other:m_Npcs)if(other){Scripting::NpcMateFrame f;f.Feet=Pack(other->Feet);f.Dead=other->Dead;members.push_back(f);}
    Scripting::NpcSpawnFrame spawn;spawn.Position=Pack(sp.Pos);spawn.Yaw=sp.Yaw;spawn.Now=m_Now;spawn.Index=n->Index;spawn.Weapon=sp.Weapon;
    if(sp.Weapon==2)spawn.Random=(int)m_Rng();spawn.Count=(int)members.size();spawn.Members=reinterpret_cast<std::uintptr_t>(members.data());Project("npc.spawn",spawn);
    glm::vec3 feet=Unpack(spawn.Feet);
    // Stepped aside from a squadmate on the spawn: only to somewhere walkable from it (not through the wall into the next room).
    if (feet != sp.Pos && !m_Nav.Walkable(sp.Pos, feet)) feet = sp.Pos;
    glm::vec3 snapped;
    if (m_Nav.Closest(feet, snapped, glm::vec3(2.0f, 3.0f, 2.0f))) feet = snapped;
    spawn.Position=Pack(feet);spawn.Count=0;Project("npc.spawn",spawn);
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
    n->Cct = PhysicsWorld::CreateNpcCharacter(Id(root), spawn.Radius, spawn.StandCylinder, &feet.x);
    n->Agent = m_Crowd.Add(feet, spawn.AgentRadius, spawn.AgentHeight, spawn.JogSpeed);
    auto& scripts=reg.get_or_emplace<CSharpScriptComponent>(root);
    auto slots=Scripting::GetSlots(scripts);std::string healthFields;
    for(const auto& slot:slots)if(slot.Class=="Tartarus.Gameplay.Health" && slot.Enabled){healthFields=slot.Fields;break;}
    if(healthFields.empty()){Scripting::Attach(scripts,"assets/Scripts/Health.cs","Tartarus.Gameplay.Health");healthFields="{}";}
    std::string resolved,state;
    if(!Scripting::ResolveScriptFields("Tartarus.Gameplay.Health",healthFields,resolved) || !Scripting::RequestProject("health.create",nlohmann::json{{"entity",Id(root)},{"fields",nlohmann::json::parse(resolved)}}.dump(),state))throw std::runtime_error("Project health unavailable");
    const auto health=nlohmann::json::parse(state);n->Health=health.at("Current").get<float>();n->MaxHealth=health.at("Max").get<float>();

    // The weapon: the player's own, on a camera at the soldier's eyes.
    const int weapon=spawn.Weapon;
    FirstPersonControllerComponent cfg = *m_ViewConfig;
    cfg.AnimationSet = weapon == 1 ? m_Cfg.ShotgunAnimationSet : m_Cfg.RifleAnimationSet;
    // A copied player configuration must not override the chosen NPC weapon with slot 0.
    cfg.PrimaryWeaponPrefab = weapon == 1 ? m_Cfg.ShotgunPrefab : m_Cfg.RiflePrefab;
    cfg.SecondaryWeaponPrefab.clear();
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
        n->Weapon->SetLocomotionSpeeds(spawn.JogSpeed, spawn.RunSpeed);
    }
    const float weaponMs = since() - weaponT0;
    n->WeaponCam.Fov = cfg.ViewModelFov;
    n->AimYaw=spawn.AimYaw;n->AimPitch=0;n->LookYaw=spawn.AimYaw;n->Eye=Unpack(spawn.Eye);n->SightEye=n->Eye;
    n->Intent.AimPoint=n->Intent.LookPoint=Unpack(spawn.LookPoint);n->Doing=Behaviour::Idle;n->DoingSince=m_Now;n->NextLook=spawn.NextLook;n->NextThink=spawn.NextThink;
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
    std::string healthResult;Scripting::RequestProject("health.remove",nlohmann::json{{"entity",Id(n.Root)}}.dump(),healthResult);
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

    Scripting::NpcPlayerNoiseFrame noise;noise.Player=PackPlayer(p);noise.Now=m_Now;noise.Dt=dt;noise.FootstepTimer=m_FootstepTimer;noise.Post=Pack(m_PlayerPost);noise.Still=m_PlayerStill;Project("npc.player-noise",noise);
    for(const auto& event:{noise.Gun,noise.Step,noise.Reload})if(event.Radius>0)m_Noises.push_back({Unpack(event.Position),event.Radius,event.Loudness,event.Time,event.Source});
    m_FootstepTimer=noise.FootstepTimer;m_PlayerPost=Unpack(noise.Post);m_PlayerStill=noise.Still;
    if(p.Valid && !p.Dead){
        if (m_PlayerAgent < 0 && m_Crowd.Valid()) m_PlayerAgent = m_Crowd.Add(p.Feet, p.Radius, p.Height, 6.0f, false);
        if (m_PlayerAgent >= 0) m_Crowd.Sync(m_PlayerAgent, p.Feet, p.Velocity);
    }

    bool anyoneSees = false;
    for (auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        Scripting::NpcDamageFrame health;health.Operation=2;health.UseHealth=1;health.Entity=(int)Id(up->Root);
        if(!Scripting::InvokeProject("npc.damage",&health,sizeof health))throw std::runtime_error("Project health snapshot unavailable");
        if(!health.Ignore){up->Health=health.Health;up->MaxHealth=health.MaxHealth;if(health.Kill){Kill(world,*up,{0,0,1},up->Feet,0);continue;}}
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
        auto life=Life(n,m_Cfg,m_Now);Project("npc.life",life);
        if (life.Kill) {
            Kill(world, n, FrontOf(n.AimYaw, 0.0f) * -1.0f, n.Feet + glm::vec3(0.0f, 0.8f, 0.0f), 6.0f);
            continue;
        }
        if (!Frozen && !n.Dummy) { // a dummy keeps the intent it spawned with: stand, facing the way it was placed
            SubTimer timer(*this, SubBrain);
            NpcBrain::Think(*this, world, n, p, dt);
        }
        // Movement goal -> the crowd.
        if (n.Agent >= 0) {
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
                m_Crowd.SetMaxSpeed(n.Agent,life.Speed);
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
            auto life=Life(n,m_Cfg,m_Now);Project("npc.life",life);
            if (life.Despawn) {
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
    UpdateCallouts(p);
    m_Noises.erase(std::remove_if(m_Noises.begin(), m_Noises.end(), [&](const Noise& z) { return m_Now - z.Time > 0.6f; }),
                   m_Noises.end());
    m_ThinkMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    FlushCosts(true);
}

void NpcDirector::Perceive(World& world,Npc& n,const PlayerSnapshot& p,float dt) {
    (void)world;
    static thread_local std::vector<Scripting::NpcNoiseFrame> noises;
    static thread_local std::vector<Scripting::NpcPerceptionMateFrame> mates;
    noises.clear();mates.clear();
    for(const auto& z:m_Noises) {Scripting::NpcNoiseFrame f;f.Position={z.Pos.x,z.Pos.y,z.Pos.z};f.Radius=z.Radius;f.Loudness=z.Loudness;f.Time=z.Time;f.Source=z.Source;noises.push_back(f);}
    for(const auto& o:m_Npcs) {Scripting::NpcPerceptionMateFrame f;f.Exists=o!=nullptr;if(o){f.Squad=o->Squad;f.Memory=PackMemory(o->Mem);}mates.push_back(f);}
    Scripting::NpcPerceptionFrame f;f.Memory=PackMemory(n.Mem);f.Player=PackPlayer(p);
    f.Eye={n.Eye.x,n.Eye.y,n.Eye.z};f.SightEye={n.SightEye.x,n.SightEye.y,n.SightEye.z};f.SeenPoint={n.SeenPoint.x,n.SeenPoint.y,n.SeenPoint.z};
    f.Now=m_Now;f.Dt=dt;f.Suppression=n.Suppression;f.ReactionLeft=n.ReactionLeft;f.PinnedSince=n.PinnedSince;f.Skill=n.Skill;f.Difficulty=m_Difficulty;
    f.AimYaw=n.AimYaw;f.AimPitch=n.AimPitch;f.LookYaw=n.LookYaw;f.LookPitch=n.LookPitch;f.NextLook=n.NextLook;f.LastOwnSight=n.LastOwnSight;f.CowerUntil=n.CowerUntil;f.TimeOnTarget=n.TimeOnTarget;
    f.Aim=n.Intent.Aim;f.FirstShot=n.FirstShot;f.Index=n.Index;f.Squad=n.Squad;f.Doing=(int)n.Doing;f.VisiblePoints=n.VisiblePoints;
    f.NoiseCount=(int)noises.size();f.MateCount=(int)mates.size();f.Noises=reinterpret_cast<std::uintptr_t>(noises.data());f.Mates=reinterpret_cast<std::uintptr_t>(mates.data());
    struct Scope {NpcDirector& Director;Npc& Owner;} context{*this,n};f.Context=reinterpret_cast<std::uintptr_t>(&context);
    f.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t data,Scripting::NpcServiceFrame* r)->int {
        auto& scope=*reinterpret_cast<Scope*>(data);
        if(r->Operation==0){r->Value=std::uniform_real_distribution<float>(0,1)(scope.Director.m_Rng);return 1;}
        if(r->Operation==1) {
            PhysicsWorld::ScopedQueryPolicy policy(Id(scope.Owner.Root),true);
            glm::vec3 origin(r->A.x,r->A.y,r->A.z),direction=glm::vec3(r->B.x,r->B.y,r->B.z)-origin;
            float length=glm::length(direction);if(length<1e-3f)return 0;direction/=length;
            RaycastHit hit;QueryFilter filter;filter.HitTriggers=0;
            return PhysicsWorld::RaycastFiltered(&origin.x,&direction.x,length+.5f,filter,hit) && hit.Hit && hit.Entity==kPlayerEntity;
        }
        return 0;
    });
    if(!Scripting::InvokeProject("npc.perception",&f,sizeof f))throw std::runtime_error("Project NPC perception unavailable");
    n.Mem=UnpackMemory(f.Memory);n.SightEye={f.SightEye.x,f.SightEye.y,f.SightEye.z};n.SeenPoint={f.SeenPoint.x,f.SeenPoint.y,f.SeenPoint.z};
    n.Suppression=f.Suppression;n.ReactionLeft=f.ReactionLeft;n.PinnedSince=f.PinnedSince;n.NextLook=f.NextLook;n.LastOwnSight=f.LastOwnSight;n.CowerUntil=f.CowerUntil;n.TimeOnTarget=f.TimeOnTarget;n.FirstShot=f.FirstShot!=0;n.VisiblePoints=f.VisiblePoints;m_Tactics.Startles+=f.Startles;
    if(f.Calls&1)Callout(n,CallKind::ContactRelay);if(f.Calls&2)Callout(n,CallKind::Gunfire);if(f.Calls&4)Callout(n,CallKind::Contact);
}

void NpcDirector::UpdateSquads(const PlayerSnapshot& p,float dt) {
    (void)dt;
    static thread_local std::vector<Scripting::NpcSquadMemberFrame> members;
    for(auto& squad:m_Squads) {
        members.clear();
        for(const auto& n:m_Npcs) {
            Scripting::NpcSquadMemberFrame f;f.Exists=n!=nullptr;
            if(n){f.Memory=PackMemory(n->Mem);f.Feet={n->Feet.x,n->Feet.y,n->Feet.z};f.SightEye={n->SightEye.x,n->SightEye.y,n->SightEye.z};f.Health=n->Health;f.MaxHealth=n->MaxHealth;f.Skill=n->Skill;f.ReactionLeft=n->ReactionLeft;f.CoverFireOrder=n->CoverFireOrder;
                f.Dead=n->Dead;f.Wounded=n->Wounded;f.Shotgun=n->Class==WeaponClass::Shotgun;f.Fire=n->Intent.Fire;f.Suppress=n->Intent.Suppress;f.TriggerHeld=n->TriggerHeld;f.Reloading=n->Reloading;f.EmptyWeapon=n->Weapon && n->Weapon->IsActive() && n->Weapon->Ammo()==0;
                f.HasFlank=n->HasFlankToken;f.HasPush=n->HasPushToken;f.HasAttack=n->HasAttackToken;f.Role=(int)n->Role;f.Doing=(int)n->Doing;f.Cover=n->Cover;}
            members.push_back(f);
        }
        Scripting::NpcSquadFrame f;f.Shared=PackMemory(squad.Shared);f.Player=PackPlayer(p);f.Now=m_Now;f.Difficulty=m_Difficulty;
        f.SharedAt=squad.SharedAt;f.PlayerReloadingSeen=squad.PlayerReloadingSeen;f.PushUntil=squad.PushUntil;f.NextRoles=squad.NextRoles;f.LastDeath=squad.LastDeath;f.FlankDoneAt=squad.FlankDoneAt;f.PincerDoneAt=squad.PincerDoneAt;f.PlayerHurtPushAt=squad.PlayerHurtPushAt;f.CoverRequestAt=squad.CoverRequestAt;f.CoverFireUntil=squad.CoverFireUntil;f.NextTokens=squad.NextTokens;
        f.FlankHolder=squad.FlankHolder;f.PincerHolder=squad.PincerHolder;f.PushHolder=squad.PushHolder;f.CoverRequest=squad.CoverRequest;f.CoverFirer=squad.CoverFirer;f.AttackerCount=std::min(4,(int)squad.Attackers.size());std::copy_n(squad.Attackers.begin(),f.AttackerCount,f.Attackers);
        f.TotalCount=(int)members.size();f.MemberCount=(int)squad.Members.size();f.Members=reinterpret_cast<std::uintptr_t>(members.data());f.MemberIds=reinterpret_cast<std::uintptr_t>(squad.Members.data());
        if(!Scripting::InvokeProject("npc.squads",&f,sizeof f))throw std::runtime_error("Project squad policy unavailable");
        squad.Shared=UnpackMemory(f.Shared);squad.SharedAt=f.SharedAt;squad.PlayerReloadingSeen=f.PlayerReloadingSeen;squad.PushUntil=f.PushUntil;squad.NextRoles=f.NextRoles;squad.LastDeath=f.LastDeath;squad.FlankDoneAt=f.FlankDoneAt;squad.PincerDoneAt=f.PincerDoneAt;squad.PlayerHurtPushAt=f.PlayerHurtPushAt;squad.CoverRequestAt=f.CoverRequestAt;squad.CoverFireUntil=f.CoverFireUntil;squad.NextTokens=f.NextTokens;
        squad.FlankHolder=f.FlankHolder;squad.PincerHolder=f.PincerHolder;squad.PushHolder=f.PushHolder;squad.CoverRequest=f.CoverRequest;squad.CoverFirer=f.CoverFirer;squad.Attackers.assign(f.Attackers,f.Attackers+f.AttackerCount);m_Tactics.HurtPushes+=f.HurtPushes;m_Tactics.CoverOrders+=f.CoverOrders;
        for(size_t i=0;i<members.size();++i)if(auto& n=m_Npcs[i]) {
            const auto& view=members[i];n->Mem=UnpackMemory(view.Memory);n->ReactionLeft=view.ReactionLeft;n->CoverFireOrder=view.CoverFireOrder;n->Role=(NpcRole)view.Role;n->HasFlankToken=view.HasFlank!=0;n->HasPushToken=view.HasPush!=0;n->HasAttackToken=view.HasAttack!=0;
        }
        for(size_t i=0;i<members.size();++i)if(members[i].CallCovering && m_Npcs[i])Callout(*m_Npcs[i],CallKind::Covering);
    }
}

// A rifle-butt strike at a player in arm's reach: started here, the blow resolved MeleeHitTime later (still in reach and
// in front, it lands). The gun's thrust is drawn in LatePose.
void NpcDirector::Move(World& world, Npc& n, float dt) {
    if (n.Cct == PhysicsWorld::kNoCharacter) return;
    glm::vec3 want(0.0f);
    if (n.Intent.Move && n.Agent >= 0) want = m_Crowd.Velocity(n.Agent);
    want.y = 0.0f;
    auto life=Life(n,m_Cfg,m_Now,dt);life.Operation=1;life.Want=Pack(want);Project("npc.life",life);n.FallSpeed=life.FallSpeed;n.PushVel=Unpack(life.Push);
    const float d[3]={life.Displacement.x,life.Displacement.y,life.Displacement.z};
    const unsigned hit = PhysicsWorld::MoveNpcCharacter(n.Cct, d, dt);
    life.Grounded=(hit & PhysicsWorld::CC_DOWN)!=0;
    float feet[3], r = 0.0f, cyl = 0.0f;
    if (PhysicsWorld::GetNpcCapsule(n.Cct, feet, &r, &cyl)) {
        const glm::vec3 f(feet[0], feet[1], feet[2]);
        const glm::vec3 moved = f - n.Feet;
        n.Velocity = glm::vec3(moved.x, 0.0f, moved.z) / dt;
        n.Feet = f;
    }
    if (n.Agent >= 0) m_Crowd.Sync(n.Agent, n.Feet);
    life.Operation=2;life.Velocity=Pack(n.Velocity);life.FootIKEverywhere=FootIKEverywhere;Project("npc.life",life);n.FallSpeed=life.FallSpeed;n.BlockedTime=life.BlockedTime;
    if((life.Crouch!=0)!=n.Crouched){
        if(life.Crouch || PhysicsWorld::NpcFitsAt(n.Cct,life.StandCylinder)){
            PhysicsWorld::ResizeNpcCharacter(n.Cct,life.Crouch?life.CrouchCylinder:life.StandCylinder);n.Crouched=life.Crouch!=0;
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
    in.Aiming = life.Aim!=0;
    in.LookPoint = n.Intent.LookPoint;
    in.Crouched = n.Crouched;
    in.Sprint = life.Sprint!=0;
    in.Lean = n.Intent.Lean;
    in.Cower = n.Intent.Cower;
    in.FootIK = life.FootIK!=0; // feet on slopes and steps where they can be seen
    n.Body.Tick(world, in, dt);
}

void NpcDirector::AimAndFire(World& world,Npc& n,const PlayerSnapshot& p,float dt) {
    Scripting::NpcCombatFrame f;f.Player=PackPlayer(p);
    f.Intent.MoveTarget={n.Intent.MoveTarget.x,n.Intent.MoveTarget.y,n.Intent.MoveTarget.z};f.Intent.AimPoint={n.Intent.AimPoint.x,n.Intent.AimPoint.y,n.Intent.AimPoint.z};f.Intent.LookPoint={n.Intent.LookPoint.x,n.Intent.LookPoint.y,n.Intent.LookPoint.z};f.Intent.Move=n.Intent.Move;f.Intent.Aim=n.Intent.Aim;f.Intent.BlindFire=n.Intent.BlindFire;f.Intent.Fire=n.Intent.Fire;f.Intent.Suppress=n.Intent.Suppress;f.Intent.Reload=n.Intent.Reload;f.Intent.Pace=(int)n.Intent.Pace;
    f.Eye={n.Eye.x,n.Eye.y,n.Eye.z};f.Feet={n.Feet.x,n.Feet.y,n.Feet.z};f.Velocity={n.Velocity.x,n.Velocity.y,n.Velocity.z};f.Now=m_Now;f.Dt=dt;f.Skill=n.Skill;f.Difficulty=m_Difficulty;
    f.MeleeAt=n.MeleeAt;f.MeleeTime=m_Cfg.MeleeTime;f.MeleeHitTime=m_Cfg.MeleeHitTime;f.MeleeDamage=m_Cfg.MeleeDamage;f.BodyYaw=n.Body.Yaw();f.LookYaw=n.LookYaw;f.LookPitch=n.LookPitch;f.AimYaw=n.AimYaw;f.AimPitch=n.AimPitch;f.AimYawRate=n.AimYawRate;f.AimPitchRate=n.AimPitchRate;f.TimeOnTarget=n.TimeOnTarget;f.ReactionLeft=n.ReactionLeft;f.BlockedTime=n.BlockedTime;f.BurstPause=n.BurstPause;f.LastShot=n.LastShot;
    f.Visible=n.Mem.Visible;f.Known=n.Mem.Known;f.HoldFire=HoldFire;f.Dummy=n.Dummy;f.Wounded=n.Wounded;f.HasAttackToken=n.HasAttackToken;f.Signalling=n.Body.Signalling();f.Shotgun=n.Class==WeaponClass::Shotgun;f.TriggerHeld=n.TriggerHeld;f.FirstShot=n.FirstShot;f.FullAutoSet=n.FullAutoSet;f.AllowFullAuto=n.Gun.AllowFullAuto;f.MeleeLanded=n.MeleeLanded;
    f.AmmoSeen=n.AmmoSeen;f.ShotsFired=n.ShotsFired;f.BurstLeft=n.BurstLeft;f.WeaponActive=n.Weapon && n.Weapon->IsActive();
    if(f.WeaponActive){const auto& state=n.Weapon->CurrentState();f.WeaponState=reinterpret_cast<std::uintptr_t>(state.data());f.WeaponStateLength=(int)state.size();}
    struct Scope {NpcDirector& Director;Npc& Owner;} context{*this,n};f.Context=reinterpret_cast<std::uintptr_t>(&context);
    f.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t data,Scripting::NpcServiceFrame* r)->int {
        auto& scope=*reinterpret_cast<Scope*>(data);
        if(r->Operation==0){r->Value=std::uniform_real_distribution<float>(r->A.x,r->A.y)(scope.Director.m_Rng);return 1;}
        if(r->Operation==1){r->Value=(float)std::uniform_int_distribution<int>((int)r->A.x,(int)r->A.y)(scope.Director.m_Rng);return 1;}
        if(r->Operation==2) {
            auto& n=scope.Owner;PhysicsWorld::ScopedQueryPolicy policy(Id(n.Root),true);
            glm::vec3 origin(r->A.x,r->A.y,r->A.z),target(r->B.x,r->B.y,r->B.z),direction=glm::normalize(target-origin+glm::vec3(1e-5f));
            RaycastHit hit;QueryFilter filter;filter.HitTriggers=0;
            if(PhysicsWorld::RaycastFiltered(&origin.x,&direction.x,glm::length(target-origin)+1,filter,hit) && hit.Hit && hit.Entity!=kPlayerEntity)
                for(const auto& other:scope.Director.m_Npcs)if(other && !other->Dead && other.get()!=&n && Id(other->Root)==hit.Entity)return 1;
        }
        return 0;
    });
    auto invoke=[&](int operation){f.Operation=operation;if(!Scripting::InvokeProject("npc.combat",&f,sizeof f))throw std::runtime_error("Project NPC combat unavailable");};
    invoke(2);n.MeleeAt=f.MeleeAt;n.MeleeLanded=f.MeleeLanded!=0;n.BurstLeft=f.BurstLeft;m_Tactics.Melees+=f.Melees;m_Tactics.MeleeHits+=f.MeleeHits;
    if(f.Damage>0){DamageEvent e;e.Source=Id(n.Root);e.Target=kPlayerEntity;e.Amount=f.Damage;e.Zone=HitZone::Torso;e.Point={f.DamagePoint.x,f.DamagePoint.y,f.DamagePoint.z};e.Direction={f.DamageDirection.x,f.DamageDirection.y,f.DamageDirection.z};e.SourcePos=n.Eye;m_PlayerDamage.push_back(e);}
    if(f.CallMelee)Callout(n,CallKind::Melee);
    invoke(0);n.Reloading=f.Reloading!=0;n.Pumping=f.Pumping!=0;n.LookYaw=f.LookYaw;n.LookPitch=f.LookPitch;n.AimYaw=f.AimYaw;n.AimPitch=f.AimPitch;n.AimYawRate=f.AimYawRate;n.AimPitchRate=f.AimPitchRate;n.TimeOnTarget=f.TimeOnTarget;
    if(!f.WeaponActive)return;
    auto& w=*n.Weapon;n.WeaponCam.Position=n.Eye;n.WeaponCam.Yaw=n.AimYaw;n.WeaponCam.Pitch=n.AimPitch;n.WeaponCam.Roll=0;
    {PhysicsWorld::ScopedQueryPolicy policy(Id(n.Root),true);w.Update(world,n.WeaponCam);}
    f.Ammo=w.Ammo();f.IsFullAuto=w.IsFullAuto();f.Equipped=w.IsEquipped();f.Chambered=w.Chambered();invoke(1);
    n.FullAutoSet=f.FullAutoSet!=0;n.AmmoSeen=f.AmmoSeen;n.ShotsFired=f.ShotsFired;n.LastShot=f.LastShot;n.FirstShot=f.FirstShot!=0;n.BurstLeft=f.BurstLeft;n.BurstPause=f.BurstPause;n.BlockedTime=f.BlockedTime;n.TriggerHeld=f.TriggerHeld!=0;
    if(f.ToggleAuto)w.ToggleFireMode();if(f.GunNoise)m_Noises.push_back({n.Eye,80,1,m_Now,n.Index});w.UpdateTrigger(f.Pressed!=0,f.Held!=0);if(f.Reload)w.Reload();
    w.Tick(dt,{f.GunVelocity.x,f.GunVelocity.y,f.GunVelocity.z},f.Sprinting!=0,f.AimGun && !f.Sprinting,0,true);
    if(float feet[2];!FoleyAudio::Get().Tuning().StepsFromFeet || !n.Body.FootHeights(feet))FoleyAudio::Get().NpcWalk(world,n.Index,n.Feet,n.Velocity,f.Sprinting!=0,dt);
    if(n.WeaponAction)w.TriggerAction(n.WeaponAction);n.WeaponAction=nullptr;
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
            if (n.Dropped) n.Dropped->Update(world, dt);
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
    }
    m_LateMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    FlushCosts(false);
}

// The soldier's late pose: the spine aimed, the gun's camera hung off the shoulders, this frame's rounds fired, the
// gun seated and the hands onto it. `alive` false: its dying frame - the same pose, but no more rounds.
void NpcDirector::LatePose(World& world, Npc& n, float dt, const PlayerSnapshot& p, bool alive) {
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
    Scripting::NpcShotPoseFrame f;f.Player=PackPlayer(p);f.Intent=PackIntent(n.Intent);f.Memory=PackMemory(n.Mem);f.Feet=Pack(n.Feet);f.Eye=Pack(n.Eye);f.Velocity=Pack(n.Velocity);f.SeenPoint=Pack(n.SeenPoint);f.BlindOffset=Pack(n.BlindOffset);
    if(n.Cover>=0 && n.Cover<(int)m_Cover.Points().size() && n.PeekSide>=0 && n.PeekSide<2){const auto& cover=m_Cover.Points()[(size_t)n.Cover];f.HighCover=cover.High;f.CoverPosition=Pack(cover.Pos);f.Peek=Pack(cover.PeekPos[n.PeekSide]);}
    f.Now=m_Now;f.Dt=dt;f.AimYaw=n.AimYaw;f.AimPitch=n.AimPitch;f.BlindLift=n.BlindLift;f.MeleeAt=n.MeleeAt;f.MeleeTime=m_Cfg.MeleeTime;f.TimeOnTarget=n.TimeOnTarget;f.Suppression=n.Suppression;f.Skill=n.Skill;f.Difficulty=m_Difficulty;f.LastHurt=n.LastHurt;
    f.Alive=alive;f.TriggerHeld=n.TriggerHeld;f.FirstShot=n.FirstShot;f.Wounded=n.Wounded;f.VisiblePoints=n.VisiblePoints;f.Weapon=(int)n.Class;
    f.Context=reinterpret_cast<std::uintptr_t>(&m_Rng);f.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t ptr,Scripting::NpcServiceFrame* request)->int {request->Value=std::uniform_real_distribution<float>(0,1)(*reinterpret_cast<std::mt19937*>(ptr));return 1;});
    Project("npc.shot-pose",f);n.BlindOffset=Unpack(f.BlindOffset);n.BlindLift=f.BlindLift;n.WeaponCam.Position+=Unpack(f.CameraOffset);n.WeaponCam.Roll=f.Roll;
    const auto shot=Unpack(f.Shot);n.Weapon->SetShotTarget(f.HaveShot?&shot:nullptr);
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
        Scripting::NpcShotEffectFrame f;f.Player=PackPlayer(p);f.Origin=Pack(t.Origin);f.End=Pack(t.End);f.FirstPellet=t.FirstPellet;f.Shotgun=n.Class==WeaponClass::Shotgun;f.Tracer=n.Tracer;f.HitPlayer=t.Hit && t.Entity==kPlayerEntity;f.Reach=Fx->FlybyReach();Project("npc.shot-effects",f);n.Tracer=f.Tracer;
        if(f.Commands&1)Fx->Shot(world,f.Shotgun?CombatFx::Gun::Shotgun:CombatFx::Gun::Rifle,t.Origin+muzzleShift,t.End,false,(f.Commands&2)!=0,(std::uint32_t)n.Index+1u,n.Weapon->MuzzleEffects());
        if(f.Commands&4)Fx->Whizz(Unpack(f.Closest),f.Miss);
    }
    // The gun's own noises: a reload starting, the pump racked.
    if (Fx && n.Weapon) {
        Scripting::NpcShotEffectFrame f;f.Operation=1;f.Eye=Pack(n.Eye);f.Reloading=n.Weapon->IsReloading();f.Pumping=n.Pumping;f.WasReloading=n.FxReloading;f.WasPumping=n.FxPumping;Project("npc.shot-effects",f);
        if(f.Commands&8)Fx->Play(CombatFx::Cue::Reload,Unpack(f.ReloadPosition));if(f.Commands&16)Fx->Play(CombatFx::Cue::Pump,Unpack(f.PumpPosition));n.FxReloading=f.WasReloading!=0;n.FxPumping=f.WasPumping!=0;
    }
    for (const FirstPersonPresentation::ShotHit& hit : n.Weapon->TakeShotHits()) {
        const float dist = glm::length(hit.Point - hit.Origin);
        if (hit.Entity == kPlayerEntity) {
            if (!p.Valid || p.Dead) continue;
            const HitZone zone = ZoneFromCapsuleHeight(hit.Point.y, p.Feet.y, p.Height);
            DamageEvent e;
            e.Source = Id(n.Root);
            e.Target = kPlayerEntity;
            Scripting::NpcShotEffectFrame rule;rule.Operation=2;rule.Player=PackPlayer(p);rule.Damage=DamageForHit(n.Gun,zone,dist);rule.DamageScale=m_DamageScale;Project("npc.shot-effects",rule);if(!(rule.Commands&128))continue;
            e.Amount=rule.Damage;
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
            Scripting::NpcShotEffectFrame rule;rule.Operation=2;rule.Friendly=1;rule.Suppression=o->Suppression;rule.Damage=DamageForHit(n.Gun,HitZone::Torso,dist);Project("npc.shot-effects",rule);o->Suppression=rule.Suppression;
            if(rule.Commands&32)o->Body.Flinch(world,hit.Direction);
            if (m_FleshHits.size() < 64)
                m_FleshHits.push_back({hit.Point, hit.Direction, hit.Entity, -1, rule.Damage,
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
    for(auto& up:m_Npcs){if(!up || up->Dead)continue;auto f=Life(*up,m_Cfg,m_Now);f.Operation=3;f.Origin=Pack(origin);f.End=Pack(end);
        f.Context=reinterpret_cast<std::uintptr_t>(&m_Rng);f.Services=reinterpret_cast<std::uintptr_t>(+[](std::uintptr_t ptr,Scripting::NpcServiceFrame* request)->int {request->Value=std::uniform_real_distribution<float>(0,1)(*reinterpret_cast<std::mt19937*>(ptr));return 1;});
        Project("npc.life",f);up->Suppression=f.Suppression;up->CowerUntil=f.CowerUntil;up->Mem=UnpackMemory(f.Memory);
    }
}

void NpcDirector::ApplyDamage(World& world, Npc& n, float amount, HitZone zone, const glm::vec3& point, const glm::vec3& dir, int attacker,
                              int part) {
    Scripting::NpcDamageFrame f;
    auto pack=[](const glm::vec3& v){return Scripting::Vec3{v.x,v.y,v.z};};
    auto unpack=[](const Scripting::Vec3& v){return glm::vec3(v.x,v.y,v.z);};
    f.Now=m_Now;f.Amount=amount;f.Health=n.Health;f.MaxHealth=n.MaxHealth;f.LastHurt=n.LastHurt;f.Suppression=n.Suppression;
    f.TimeOnTarget=n.TimeOnTarget;f.Awareness=n.Mem.Awareness;f.LastSeen=n.Mem.LastSeen;f.Uncertainty=n.Mem.Uncertainty;
    f.ReactionLeft=n.ReactionLeft;f.LimpUntil=n.LimpUntil;f.StaggerUntil=n.StaggerUntil;f.NextThink=n.NextThink;
    f.LimpTime=m_Cfg.LimpTime;f.HeavyHitDamage=m_Cfg.HeavyHitDamage;f.StaggerTime=m_Cfg.StaggerTime;f.WoundChance=m_WoundChance;
    f.UseHealth=1;f.Entity=(int)Id(n.Root);f.Dead=n.Dead;f.Wounded=n.Wounded;f.Attacker=attacker;f.Zone=static_cast<int>(zone);f.Part=part;f.Hits=n.Hits;
    f.Known=n.Mem.Known;f.Visible=n.Mem.Visible;f.WoundRolled=n.WoundRolled;f.Dummy=n.Dummy;
    f.Direction=pack(dir);f.LastHurtFrom=pack(n.LastHurtFrom);f.LastKnown=pack(n.Mem.LastKnown);f.PlayerFeet=pack(m_Player.Feet);f.PushVelocity=pack(n.PushVel);
    auto invoke=[&](int operation){f.Operation=operation;if(!Scripting::InvokeProject("npc.damage",&f,sizeof f))throw std::runtime_error("Project NPC damage rules unavailable");};
    invoke(0);if(f.Ignore)return;
    if(f.NeedRandom) {f.Random=std::uniform_real_distribution<float>(0,1)(m_Rng);invoke(1);}
    n.Health=f.Health;n.LastHurt=f.LastHurt;n.LastHurtFrom=unpack(f.LastHurtFrom);n.Hits=f.Hits;n.Suppression=f.Suppression;n.TimeOnTarget=f.TimeOnTarget;
    n.Mem.Known=f.Known!=0;n.Mem.Awareness=f.Awareness;n.Mem.LastKnown=unpack(f.LastKnown);n.Mem.LastSeen=f.LastSeen;n.Mem.Uncertainty=f.Uncertainty;
    n.ReactionLeft=f.ReactionLeft;n.LimpUntil=f.LimpUntil;n.StaggerUntil=f.StaggerUntil;n.NextThink=f.NextThink;n.PushVel=unpack(f.PushVelocity);n.WoundRolled=f.WoundRolled!=0;
    if(f.Kill) {Kill(world,n,dir,point,f.Shove,part);return;}
    if(f.Flinch) {
        n.Body.Flinch(world,dir,&point,part);
        if(m_RagdollCfg.HitFlinch && part>=0)m_Flinch[n.Index].Hit(n.Body,part,dir,amount,m_Now,m_RagdollCfg);
    }
    if(f.BecomeWounded)BecomeWounded(n);
    if(f.CallWounded)Callout(n,CallKind::Wounded);
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
    n.LastCallout = m_Now;
}

void NpcDirector::Kill(World& world, Npc& n, const glm::vec3& dir, const glm::vec3& point, float shove, int part) {
    (void)world;
    if (n.Dead) return;
    n.Dead = true;
    n.Wounded = false;
    std::string healthResult;Scripting::RequestProject("health.kill",nlohmann::json{{"entity",Id(n.Root)}}.dump(),healthResult);
    n.Health = 0.0f;
    n.DiedAt = m_Now;
    n.Doing = Behaviour::Dead;
    n.TriggerHeld = false;
    m_Cover.Release(n.Index, m_Now);
    n.Hitboxes.reset(); // the ragdoll's parts take over the rounds
    m_Flinch.erase(n.Index);
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
            auto life=Life(*o,m_Cfg,m_Now);life.Operation=4;life.Origin=Pack(n.Feet);Project("npc.life",life);o->Morale=life.Morale;
            if(life.CallManDown)Callout(*o,CallKind::ManDown);
        }
    }
    if (m_Respawn) m_RespawnTimers.push_back(m_Now + m_RespawnDelay);
}

void NpcDirector::FinishDeath(World& world, Npc& n) {
    n.DeathPending = false;
    // The gun leaves his hands (copied before the weapon presentation goes) and the arm solve lets go.
    if (n.Weapon && n.Weapon->IsActive()) n.Dropped = NpcDroppedWeapon::Drop(world, n.Body, n.Weapon->WeaponEntity(), n.DeathDir * n.DeathShove);
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
        int alive = 0;
        for (auto& n : m_Npcs) if (n && !n->Dead) ++alive;
        Scripting::NpcRespawnFrame ready;ready.Player=PackPlayer(p);ready.Now=m_Now;ready.Timer=m_RespawnTimers[i];ready.Alive=alive;ready.Wanted=m_SquadSize;Project("npc.respawn",ready);
        if(!ready.Ready){++i;continue;}if(ready.Discard){m_RespawnTimers.erase(m_RespawnTimers.begin()+(long long)i);continue;}
        std::vector<Scripting::NpcRespawnPointFrame> points;points.reserve(m_Spawns.size());
        for(const auto& spawn:m_Spawns){Scripting::NpcRespawnPointFrame f;f.Position=Pack(spawn.Pos);f.Seen=p.Valid && !p.Dead && !CoverSystem::Shielded(spawn.Pos,1.6f,p.Eye);points.push_back(f);}
        Scripting::NpcRespawnFrame f;f.Player=PackPlayer(p);f.Now=m_Now;f.Timer=m_RespawnTimers[i];f.Alive=alive;f.Wanted=m_SquadSize;f.Count=(int)points.size();f.Points=reinterpret_cast<std::uintptr_t>(points.data());Project("npc.respawn",f);
        if(f.Discard || (f.Best>=0 && Spawn(world,assets,f.Best)>=0))m_RespawnTimers.erase(m_RespawnTimers.begin()+(long long)i);
        else m_RespawnTimers[i]=f.Timer;

    }
}

void NpcDirector::OnPlayerRespawned() {
    for (auto& up : m_Npcs) {
        if (!up || up->Dead) continue;
        auto life=Life(*up,m_Cfg,m_Now);life.Operation=5;Project("npc.life",life);up->Mem=UnpackMemory(life.Memory);
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
