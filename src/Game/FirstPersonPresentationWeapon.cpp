#include "FirstPersonPresentation.h"
#include "FirstPersonPresentationInternal.h"
#include "FirstPersonMovementInput.h"
#include "GameRules.h"
#include "LiveReload.h"
#include "WeaponRules.h"
#include "WeaponPrefab.h"

#include "AnimationSystem.h"
#include "Audio/WeaponAudio.h"
#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "Camera.h"
#include "Components.h"
#include "IK.h"
#include "Log.h"
#include "GameModuleAPI.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "ParticleSystem.h"
#include "SceneSerializer.h"
#include "Core/Paths.h"
#include "RotationMath.h"
#include "World.h"
#include "TimeService.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <unordered_map>

// FirstPersonPresentation: the weapon rules' side - firing, shots and ejected cases, reloads and triggers, and the
// weapon slots (loadout and swaps).

using namespace FirstPersonPresentationDetail;
namespace K = FirstPersonAnimatorContract;

namespace {

// The shared gear rattle of putting a weapon up / away (snd.foley.weapon.equip / unequip) - only for a gun without draw /
// holster takes of its own: those play from its Draw / Holster clips, and with both a weapon switch stacked four sounds
// (rattle, holster, rattle, draw).
void GearSound(const std::filesystem::path& setFile, bool equip) {
    WeaponAudio& wa = WeaponAudio::Get();
    if (!wa.Active() || wa.HasEventFiles(WeaponAudioGunId(setFile.string()), equip ? "draw" : "holster")) return;
    WeaponSound(setFile, equip ? "equip" : "unequip");
}

} // namespace

bool FirstPersonPresentation::LaserBeam(Laser& out) const {
    const auto* ac = Animator();
    if (!m_Set.Laser.Enabled || !m_AimPointValid || !ac || !m_Equipped || ac->HasTag(K::kTagHidden)) return false;
    // A draw starts with the gun low: its beam would curl across the bottom of the view.
    if (m_SinceUnhidden < 0.35f) return false;
    out.BeamColor = m_Set.Laser.Color * m_Set.Laser.BeamBrightness;
    out.SpotColor = m_Set.Laser.Color * m_Set.Laser.SpotBrightness;
    out.From = m_Muzzle;
    out.To = m_AimPoint;
    out.Normal = m_AimNormal;
    out.Hit = m_AimHit;
    return true;
}
std::vector<FirstPersonPresentation::ShotTrace> FirstPersonPresentation::TakeShotTraces() {
    std::vector<ShotTrace> t;
    t.swap(m_ShotTraces);
    return t;
}
std::vector<FirstPersonPresentation::ShotHit> FirstPersonPresentation::TakeShotHits() {
    std::vector<ShotHit> hits;
    hits.swap(m_ShotHits);
    return hits;
}
std::vector<CasingSpawn> FirstPersonPresentation::TakeEjections() {
    std::vector<CasingSpawn> out;
    out.swap(m_Ejections);
    return out;
}
void FirstPersonPresentation::Eject() {
    const FirstPersonEjectSettings& ej = m_Set.Eject;
    if (!ej.Enabled || ej.Model.empty() || !m_RootSeenValid) return;
    // From the port as the player sees it, out along the eject direction inside a small cone, at the
    // player's own speed plus the throw. The case leaves lying down the bore, base first, tumbling end
    // over end about the axis across the bore and the throw.
    const glm::mat4& R = m_RootSeen;
    const int slot = m_EjectIndex++;
    const glm::vec3 local = ej.Origins.empty() ? ej.Origin : ej.Origins[(size_t)slot % ej.Origins.size()];
    const glm::vec3 port = glm::vec3(R * glm::vec4(local, 1.0f));
    const glm::vec3 out = glm::normalize(glm::mat3(R) * ej.Direction);
    const glm::vec3 bore = glm::normalize(glm::mat3(R) * (m_HaveMuzzle ? m_BoreLocal : glm::vec3(0.0f, 0.0f, -1.0f)));
    std::uniform_real_distribution<float> u(0.0f, 1.0f), s(-1.0f, 1.0f);
    const glm::vec3 dir = FirstPersonPelletDirection(out, ej.Spread, u(m_Rng), u(m_Rng));
    CasingSpawn c;
    c.Model = slot < m_LiveEjects && !ej.LiveModel.empty() ? ej.LiveModel : ej.Model;
    c.Material = ej.Material;
    c.Position = port;
    c.Velocity = m_PlayerVelocity + dir * ej.Speed * (1.0f + ej.SpeedJitter * s(m_Rng));
    // The mesh's long axis is its local +Y (tools/weapons/extract_casings.py).
    glm::vec3 across = glm::cross(bore, dir);
    if (glm::dot(across, across) < 1e-6f) across = glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 a = glm::normalize(across);
    const glm::vec3 y = -bore, x = glm::normalize(glm::cross(y, a)), z = glm::cross(x, y);
    c.Rotation = glm::quat_cast(glm::mat3(x, y, z));
    c.AngularVelocity = a * ej.Spin * (0.75f + 0.5f * u(m_Rng)) + glm::vec3(s(m_Rng), s(m_Rng), s(m_Rng)) * ej.Spin * 0.2f;
    m_Ejections.push_back(c);
}
Gameplay::WeaponFrame FirstPersonPresentation::RunGameplay(int operation,float dt,bool pressed,bool held,int events) {
    Gameplay::WeaponFrame f;
    const auto& g=m_Set.Gameplay;
    f.Operation=operation; f.Dt=dt; f.Pressed=pressed; f.Held=held; f.Events=events;
    auto* ac=Animator();
    if(!ac) return f;
    f.InTransition=ac->InTransition; f.WallBlocked=WallBlocked();
    f.Random=std::uniform_real_distribution<float>(0.0f,1.0f)(m_Rng);
    f.Ammo=m_Ammo;
    f.Cooldown=m_FireCooldown;
    f.CycleWait=m_CycleWait;
    f.IdleTime=m_IdleTime;
    f.RegripDelay=m_RegripDelay;
    f.SinceShot=m_SinceShot;
    f.SinceUnhidden=m_SinceUnhidden;
    f.Equipped=m_Equipped;
    f.Chambered=m_Chambered;
    f.CycleSeen=m_CycleSeen;
    f.StopReload=m_StopReload;
    f.FullAuto=m_FullAuto;
    f.BurstRemaining=m_BurstRemaining;
    f.WaitingShot=m_WaitingAnimatedShot;
    f.ReloadHeldSeconds=m_ReloadSeconds;
    f.ReloadWasDown=m_ReloadDown;
    f.ReloadHoldFired=m_ReloadFired;
    f.Rpm=g.RoundsPerMinute;
    f.CycleDelay=g.CycleDelay;
    f.HoldSeconds=g.ReloadHoldSeconds;
    f.RegripMin=g.RegripMin;
    f.RegripMax=g.RegripMax;
    f.Magazine=g.Magazine;
    f.BurstRounds=g.BurstRounds;
    f.AllowFullAuto=g.AllowFullAuto;
    f.PerRound=g.Reload==FirstPersonWeaponGameplay::ReloadMode::PerRound;
    f.CycleAfterShot=g.CycleAfterShot;
    f.HipProcedural=m_Set.Procedural.Recoil.HipProcedural;
    f.RecoilProfile=!m_Set.RecoilProfile.empty() && !m_Set.HipFireClip; // hipFireClip: the profile only shapes sights-up shots
    if(ac->HasTag(K::kTagHidden)) f.Tags|=1;
    if(ac->HasTag(K::kTagReload)) f.Tags|=2;
    if(ac->HasTag(K::kTagBusy)) f.Tags|=4;
    if(ac->HasTag(K::kTagCycling)) f.Tags|=8;
    if(ac->HasTag(K::kTagAds)) f.Tags|=16;
    if(ac->HasTag(K::kTagIdle)) f.Tags|=32;
    if(ac->HasTag(K::kTagReady)) f.Tags|=64;
    if(ac->HasTag(K::kTagIKOff)) f.Tags|=128;
    GameRules::UpdateWeapon(f);
    m_Ammo=f.Ammo;
    m_FireCooldown=f.Cooldown;
    m_CycleWait=f.CycleWait;
    m_IdleTime=f.IdleTime;
    m_RegripDelay=f.RegripDelay;
    m_SinceShot=f.SinceShot;
    m_SinceUnhidden=f.SinceUnhidden;
    m_Equipped=f.Equipped!=0;
    m_Chambered=f.Chambered!=0;
    m_CycleSeen=f.CycleSeen!=0;
    m_StopReload=f.StopReload!=0;
    m_FullAuto=f.FullAuto!=0;
    m_BurstRemaining=f.BurstRemaining;
    m_WaitingAnimatedShot=f.WaitingShot!=0;
    m_ReloadSeconds=f.ReloadHeldSeconds;
    m_ReloadDown=f.ReloadWasDown!=0;
    m_ReloadFired=f.ReloadHoldFired!=0;
    if(f.Commands&8) { m_Procedural.EndBurst(); m_Procedural.SetFiring(false); }
    if(f.Commands&1) WeaponSound(m_SetFile, "dry_fire");
    if(f.Commands&4) ac->SetTrigger(K::kFire);
    if(f.Commands&16) ac->SetTrigger(K::kCycle);
    if(f.Commands&32) ac->SetTrigger(K::kFidget);
    if(f.Commands&64) { ac->SetBool(K::kStopReload,false); ac->SetInt(K::kAmmo,m_Ammo); ac->SetTrigger(K::kReload); }
    if(f.Commands&128) TriggerAction(K::kMagCheck);
    return f;
}
bool FirstPersonPresentation::Fire() {
    const auto f=RunGameplay(1);
    if(f.Commands&2) CommitShot((f.Tags&16)!=0,true);
    if(f.Result) m_JogHold=0.5f; // shooting from a jog: the arms settle to the ready pose for a moment
    return f.Result!=0;
}
void FirstPersonPresentation::CommitShot(bool ads, bool cycleBolt) {
    const auto mode = m_FullAuto ? RecoilFireMode::Auto :
        m_Set.Gameplay.BurstRounds > 1 ? RecoilFireMode::Burst : RecoilFireMode::Semi;
    m_Procedural.OnShot(m_Set.Procedural, ads, cycleBolt, m_Set.Gameplay.RoundsPerMinute, mode,
                        Time::UnscaledTimeSinceStartup(), Time::DeltaTime());
    if(!m_Set.CameraShakeProfile.empty()) m_CameraShake.Trigger(m_Set.CameraShake,ads);
    RunGameplay(2);
    ShotImpact();

}
void FirstPersonPresentation::FireShot() {
    const auto& g = m_Set.Gameplay;
    if (!m_World || !m_AimPointValid) return;
    if (const auto* fx = MuzzleEffects(); fx && fx->Enabled)
        for (const auto* emitters : {&m_WeaponEmitters, &m_WorldWeaponEmitters})
            for (auto e : *emitters)
                if ((emitters == &m_WeaponEmitters ? m_Attachments : m_WorldAttachments).OwnsEmitter(*m_World, e))
                if (const auto* ps = m_World->Registry.try_get<ParticleSystemComponent>(e))
                    EmitParticleBurst(*m_World, e, ps->BurstCount);
    glm::vec3 line = m_BoreDir;
    // A shotgun's round is several pellets, each down its own line inside the spread cone; the
    // round's shove is shared between them.
    const float spread = HasTag(K::kTagAds) ? g.SpreadAds : g.SpreadHip;
    const int pellets = std::max(1, g.Pellets);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (int p = 0; p < pellets; ++p) {
        const glm::vec3 dir = pellets > 1 || spread > 0.0f ? FirstPersonPelletDirection(line, spread, unit(m_Rng), unit(m_Rng))
                                                           : line;
        const float o[3] = {m_Muzzle.x, m_Muzzle.y, m_Muzzle.z}, d[3] = {dir.x, dir.y, dir.z};
        RaycastHit hit;
        QueryFilter filter;
        filter.HitTriggers = 0;
        const bool recording = PhysicsWorld::GetQueryRecording();
        PhysicsWorld::SetQueryRecording(false);
        const bool struck = PhysicsWorld::RaycastFiltered(o, d, 300.0f, filter, hit) && hit.Hit;
        PhysicsWorld::SetQueryRecording(recording);
        if (m_ShotTraces.size() < 256)
            m_ShotTraces.push_back({m_Muzzle, struck ? glm::vec3(hit.Point[0], hit.Point[1], hit.Point[2]) : m_Muzzle + dir * 300.0f,
                                    struck, p == 0, struck ? hit.Entity : 0xFFFFFFFFu});
        if (!struck) continue;
        // Bounded, in case nothing drains it (no renderer this session).
        if (m_ShotHits.size() < 256)
            m_ShotHits.push_back({glm::vec3(hit.Point[0], hit.Point[1], hit.Point[2]),
                                  glm::vec3(hit.Normal[0], hit.Normal[1], hit.Normal[2]), hit.Entity, g.BulletHoleRadius,
                                  m_Muzzle, dir, pellets});
        if (g.ImpactImpulse <= 0.0f) continue;
        const auto e = static_cast<entt::entity>(hit.Entity);
        const auto* rb = m_World->Registry.valid(e) ? m_World->Registry.try_get<RigidbodyComponent>(e) : nullptr;
        if (!rb || rb->IsKinematic) continue;
        // Light props: capped at ImpactMaxSpeed of velocity change; heavy ones just get the impulse.
        float impulse = g.ImpactImpulse;
        if (g.ImpactMaxSpeed > 0.0f) impulse = std::min(impulse, g.ImpactMaxSpeed * std::max(rb->Mass, 0.01f));
        const glm::vec3 j = dir * (impulse / (float)pellets);
        const float jv[3] = {j.x, j.y, j.z};
        PhysicsWorld::AddForceAtPosition(hit.Entity, jv, hit.Point, ForceMode::Impulse);
    }
}
Gameplay::WeaponLoadoutFrame FirstPersonPresentation::RunLoadout(int operation,int value){
    Gameplay::WeaponLoadoutFrame f;f.Operation=operation;f.Value=value;f.Count=(int)m_SlotAmmo.size();f.Slot=m_Slot;f.Pending=m_PendingSlot;f.Ammo=m_Ammo;f.Magazine=m_Set.Gameplay.Magazine;f.Burst=m_BurstRemaining;f.CycleWait=m_CycleWait;f.Active=IsActive();f.Equipped=m_Equipped;f.Requested=value!=0;f.Chambered=m_Chambered;f.SlotAmmo=reinterpret_cast<std::uintptr_t>(m_SlotAmmo.data());
    if(auto* ac=Animator()){f.Hidden=ac->HasTag(K::kTagHidden);f.InTransition=ac->InTransition;}
    GameRules::UpdateWeaponLoadout(f);
    m_PendingSlot=f.Pending;m_Ammo=f.Ammo;m_Chambered=f.Chambered!=0;m_CycleWait=f.CycleWait;m_BurstRemaining=f.Burst;
    if(f.Commands&2){m_Procedural.EndBurst();m_Procedural.SetFiring(false);}
    if(f.Commands&8)GearSound(m_SetFile, f.Equipped!=0);
    m_Equipped=f.Equipped!=0;
    if(f.Commands&4)if(auto* ac=Animator()){ac->SetBool(K::kEquipped,m_Equipped);if(operation==3)ac->SetInt(K::kAmmo,m_Ammo);}
    return f;
}
void FirstPersonPresentation::RefillAmmo(){RunLoadout(3);}
int FirstPersonPresentation::SlotForKey(int configured) const {
    for (size_t i = 0; i < m_SlotConfigured.size(); ++i)
        if (m_SlotConfigured[i] == configured) return (int)i;
    return -1;
}
void FirstPersonPresentation::SelectSlot(int slot){const auto f=RunLoadout(0,slot);if(f.Commands&16)SetEquipped(true);}
void FirstPersonPresentation::CycleSlot(int step){const auto f=RunLoadout(1,step);if(f.Commands&32)SelectSlot(f.Value);}
void FirstPersonPresentation::SwapToPendingSlot() {
    if (m_PendingSlot < 0 || !m_World || !m_SlotAssets || !m_Config) return;
    World& world = *m_World;
    const int slot = m_PendingSlot, previous = m_Slot;
    m_PendingSlot = -1;
    // Each gun sits in the shoulder its own way: this one's hold kept for a switch back, the next one's (if it has been in hand)
    // brought out - so its draw settles into the lock with the clip, not with a slide of its own once Idle starts.
    if (previous >= 0 && previous < (int)m_SlotShoulderedButt.size()) {
        m_SlotShoulderedButt[previous] = m_ShoulderedButt;
        m_SlotHaveShoulderedButt[previous] = m_HaveShoulderedButt;
    }
    m_HaveShoulderedButt = slot < (int)m_SlotHaveShoulderedButt.size() && m_SlotHaveShoulderedButt[slot];
    if (m_HaveShoulderedButt) m_ShoulderedButt = m_SlotShoulderedButt[slot];
    m_SlotAmmo[previous] = m_Ammo;
    const float walk = m_WalkSpeed, sprint = m_SprintSpeed;
    if (!StartSet(world, *m_SlotAssets, slot, true)) {
        // Keep the weapon that was in hand rather than leaving the player with nothing.
        Log::Error("First-person presentation: couldn't switch to '" + m_SlotSets[slot] + "'; keeping '" +
                   m_SlotSets[previous] + "'.");
        if (!StartSet(world, *m_SlotAssets, previous, true)) return;
    }
    m_WalkSpeed = walk;
    m_SprintSpeed = sprint;
    GearSound(m_SetFile, true); // (StartSet has the gun in hand already: SetEquipped sees no change)
    SetEquipped(true);
}
void FirstPersonPresentation::ToggleFireMode() {
    if(!IsActive()||!m_Equipped) return;
    const auto f=RunGameplay(6);
    if(f.Result) WeaponSound(m_SetFile, "firemode");
    Log::Info(std::string("Fire mode: ")+(m_FullAuto?"Full-Auto":"Semi-Auto"));
}
void FirstPersonPresentation::UpdateTrigger(bool pressed, bool held) {
    // The trigger down lets a fidget go (the controller's Trigger exit; cleared after each controller update). A press
    // the hands-off fidget refused gets one more try on the next call, once the controller has let the gun go: the
    // shot goes a frame late instead of being lost. Draw / holster stay hands-off, so a press there is still dropped.
    auto* ac=Animator();
    if(ac && (pressed||held)) ac->SetBool(K::kTrigger,true);
    const bool retry=m_TriggerRetry;
    m_TriggerRetry=false;
    const auto request=RunGameplay(3,0.0f,pressed||retry,held);
    if(!request.Result) return;
    const bool fired=Fire();
    if(!fired && pressed && ac && ac->HasTag(K::kTagIKOff)) m_TriggerRetry=true;
    const auto result=RunGameplay(4,0.0f,fired,held);
    m_Procedural.SetFiring(result.Result!=0);
}
void FirstPersonPresentation::UpdateReloadKey(bool down, float dt) {
    const auto f=RunGameplay(9,dt,false,down);
    if(f.Result) Reload();
}
bool FirstPersonPresentation::Reload() {
    return RunGameplay(5).Result!=0;
}
bool FirstPersonPresentation::TriggerAction(const std::string& trigger) {
    auto* ac = Animator();
    if (!ac || !m_Equipped || ac->HasTag(K::kTagHidden)) return false;
    ac->SetTrigger(trigger);
    m_BurstRemaining = 0;
    m_Procedural.EndBurst();
    m_Procedural.SetFiring(false);
    return true;
}
void FirstPersonPresentation::SetEquipped(bool equipped) {
    RunLoadout(2,equipped?1:0);
}
