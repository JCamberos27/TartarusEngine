#pragma once

#include "AiMath.h"
#include "Camera.h"
#include "FirstPersonAnimation.h" // FirstPersonWeaponGameplay
#include "Npc/NpcBody.h"
#include "Npc/NpcRagdoll.h"
#include "PhysicsWorld.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <string>

class FirstPersonPresentation;

// One enemy soldier: its body, its weapon, its capsule and crowd agent, what it knows and what it
// is doing. NpcDirector owns these and runs them; NpcBrain.cpp decides what they do.

enum class Gait : unsigned char { Still, Walk, Jog, Run };

enum class NpcRole : unsigned char { Anchor, Suppressor, Flanker };

enum class Behaviour : unsigned char {
    Idle,        // unaware: patrols near its post, looks about
    Investigate, // something was off: goes to look, weapon up
    Engage,      // fighting in the open: strafes and shoots
    TakeCover,   // getting to cover
    CoverFight,  // in cover: hides, peeks, shoots, reloads, suppresses
    Flank,       // working round to the player's side
    Push,        // closing in while the player is busy
    Search,      // lost the player: hunting for them
    Retreat,     // hurt: falling back to cover further off
    Dead,
};
const char* BehaviourName(Behaviour b);
const char* RoleName(NpcRole r);

// What the brain wants this frame; the director turns it into movement, stance, aim and trigger.
struct NpcIntent {
    bool Move = false;
    glm::vec3 MoveTarget{0.0f};
    Gait Pace = Gait::Jog;
    bool Crouch = false;
    bool Aim = false;              // weapon up (ADS) and the spine aiming at AimPoint
    glm::vec3 AimPoint{0.0f};
    glm::vec3 LookPoint{0.0f};
    bool FaceAim = false;          // face the aim while moving (strafe) instead of the travel
    bool Fire = false;             // the brain is happy to shoot (the fire control still decides)
    bool Suppress = false;         // shooting at where the player was, not at them
    bool Reload = false;
    float Lean = 0.0f;
    float Cower = 0.0f;            // 0..1: duck from rounds cracking past (the body hunches, the head goes down)
};

struct Npc {
    int Index = -1;
    entt::entity Root = entt::null;
    PhysicsWorld::CharacterId Cct = PhysicsWorld::kNoCharacter;
    int Agent = -1;                // crowd agent
    int SpawnIndex = -1;
    int Squad = 0;
    float Skill = 0.5f;
    std::string Name;

    NpcBody Body;
    std::unique_ptr<NpcRagdoll> Ragdoll; // once dead
    std::unique_ptr<FirstPersonPresentation> Weapon;
    FirstPersonWeaponGameplay Gun;  // a copy of the weapon's numbers (damage, rpm)
    WeaponClass Class = WeaponClass::Rifle;
    Camera WeaponCam;

    // Physical state.
    glm::vec3 Feet{0.0f};
    glm::vec3 Velocity{0.0f};
    float FallSpeed = 0.0f;
    bool Crouched = false;
    glm::vec3 Eye{0.0f};            // the head bone, where the weapon's camera sits
    glm::vec3 SightEye{0.0f};       // between the eyes, what sight rays leave from

    // Health.
    float Health = 100.0f, MaxHealth = 100.0f;
    bool Dead = false;
    float DiedAt = 0.0f;
    float LastHurt = -1e9f;
    glm::vec3 LastHurtFrom{0.0f};
    int Hits = 0;

    // What it knows.
    TargetMemory Mem;
    float NextLook = 0.0f;          // next sight check (time-sliced)
    int VisiblePoints = 0;
    glm::vec3 SeenPoint{0.0f};      // the player's body point it can see best (chest, else head ...)
    float Suppression = 0.0f;       // 0..1
    float CowerUntil = -1e9f;       // ducking from a near miss until then
    glm::vec3 GlanceAt{0.0f};       // a squadmate who just called out: a look their way ...
    float GlanceUntil = -1e9f;      // ... until then
    float ReactionLeft = 0.0f;      // hold fire this long after (re)acquiring
    bool HadSight = false;
    float LastOwnSight = -1e9f;     // when it last saw the player itself (not told by the squad)
    int EmptyPeeks = 0;             // peeks in a row that found nothing to shoot

    // Aim and fire control.
    float AimYaw = -90.0f, AimPitch = 0.0f; // the weapon camera's (degrees, Camera convention)
    float AimYawRate = 0.0f, AimPitchRate = 0.0f;
    float LookYaw = -90.0f, LookPitch = 0.0f; // where the eyes look (degrees, Camera convention)
    float TimeOnTarget = 0.0f;
    bool FirstShot = true;          // the opening round of an engagement goes wide
    int BurstLeft = 0;
    float BurstPause = 0.0f;
    bool TriggerHeld = false;
    int AmmoSeen = -1;
    int ShotsFired = 0;
    int Tracer = 0;                 // every third round is a tracer
    bool FxReloading = false, FxPumping = false; // for the reload / pump sounds' rising edges
    bool Reloading = false;         // the weapon is in a reload state (read once a frame in AimAndFire)
    float FallSoundAt = -1.0f;      // the body hits the ground (seconds, director time)
    float LastShot = -1e9f;
    bool FullAutoSet = false;

    // The brain.
    Behaviour Doing = Behaviour::Idle;
    float DoingSince = 0.0f;
    float NextThink = 0.0f;
    NpcRole Role = NpcRole::Anchor;
    int Cover = -1;                 // claimed cover point
    int Phase = 0;                  // the behaviour's own state machine
    float PhaseUntil = 0.0f;
    int PeekSide = -1;              // high cover: 0 left, 1 right, -1 over (low cover)
    glm::vec3 Goal{0.0f};           // where the behaviour is heading
    bool HasGoal = false;
    float GoalSetAt = 0.0f;
    float Morale = 1.0f;
    float LastCallout = -1e9f;
    glm::vec3 PostPos{0.0f};        // where it started (patrols round it)
    float IdleUntil = 0.0f;
    int SearchStep = 0;
    bool HasAttackToken = false;
    bool HasFlankToken = false;
    bool HasPushToken = false;
    float BlockedTime = 0.0f;       // moving but not getting anywhere
    bool Retreated = false;         // fell back once already this life
    float NoCoverUntil = 0.0f;      // no usable cover was found: don't look again till then
    float CoverCheckAt = 0.0f;      // when the claimed cover's protection was last tested
    bool CoverGood = false;         // ... and whether it still hides from the threat
    float PhaseStart = 0.0f;
    int ShotsAtPhase = 0;           // ShotsFired when the phase began
    float Scores[10] = {};          // the last decision's behaviour scores, for the overlay
    NpcIntent Intent;
    std::string Callout;            // the last thing it shouted, for the overlay
    float CalloutAt = -1e9f;
    std::string Why;                // the last decision's reason, for the overlay
};
