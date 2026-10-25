#pragma once

#include "AiMath.h"
#include "Camera.h"
#include "FirstPersonAnimation.h" // FirstPersonWeaponGameplay
#include "Npc/NpcBody.h"
#include "Npc/NpcDroppedWeapon.h"
#include "Npc/NpcHitboxes.h"
#include "Npc/NpcRagdoll.h"
#include "PhysicsWorld.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdlib>
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
    Wounded,     // out of the fight until it bleeds out or is finished (after Dead so the indices hold): shot down by a leg, on
                 // its belly crawling for cover, dragging the leg; gut-shot, a few staggering steps, then down
};
constexpr int kBehaviourCount = 11;
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
    bool BlindFire = false;        // pinned behind cover: the gun held up over it, fired without looking (Suppress, head down)
};

struct Npc {
    int Index = -1;
    entt::entity Root = entt::null;
    PhysicsWorld::CharacterId Cct = PhysicsWorld::kNoCharacter;
    int Agent = -1;                // crowd agent
    int SpawnIndex = -1;
    int Squad = 0;
    const char* WeaponAction = nullptr; // a weapon trigger to pull with this frame's weapon tick (tests: "Fidget", "Inspect")
    bool Passive = false;           // F9 Passive: ignores the player until hurt
    bool DevSpawned = false;        // put there by F9: never respawns
    bool Dummy = false;             // a training dummy (NPC Spawn Brain 1): stands where it spawned, never shoots or calls out, takes hits
    float Skill = 0.5f;
    std::string Name;

    NpcBody Body;
    std::unique_ptr<NpcRagdoll> Ragdoll; // once dead (or alive and going down: the wound fall, see FallPending)
    std::unique_ptr<NpcDroppedWeapon> Dropped; // the gun he dropped (handed to the director's loose guns if it outlasts the corpse)
    std::unique_ptr<NpcHitboxes> Hitboxes; // while alive and near the player: the bones the player's rounds hit
    int HitboxTries = 0;
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
    float UnseenSince = -1.0f;      // a corpse: since when the player hasn't seen it (-1: in sight now)
    float LastHurt = -1e9f;
    glm::vec3 LastHurtFrom{0.0f};
    int Hits = 0;
    // Hit reactions.
    float StaggerUntil = 0.0f;      // a heavy hit: aim paused, movement slowed until then
    float LimpUntil = -1e9f;        // a leg hit: slower, no pushing or flanking until then
    glm::vec3 PushVel{0.0f};        // a stagger's shove (m/s, decays)
    bool Wounded = false;           // down, crawling (Behaviour::Wounded)
    float WoundedAt = 0.0f;
    bool Prone = false;             // ... shot down by a leg: the gun dropped, on its belly, dragging HurtLeg
    int HurtLeg = -1;               // 0 left, 1 right
    glm::vec3 CrawlTo{0.0f};        // ... where he is dragging himself now, away from the threat (NpcBrain::PickCrawlGoal) ...
    bool HasCrawlTo = false;
    float CrawlRestUntil = 0.0f;    // ... looked at again then
    glm::vec3 CrawlSteer{0.0f};     // ... (the way the crawl steered last frame: tests)
    float CrawlStuck = 0.0f;        // ... seconds his hands have been at a wall (he turns where he has to, then) ...
    float CrawlSwing = 0.0f;        // ... the way he swings off it (+1 / -1 yaw; 0 none), kept till he's clear (it would dither) ...
    float CrawlSwingYaw = 0.0f;     // ... to this heading (a set turn from where he stuck, not a spin)
    bool Dying = false;             // ... gut-shot: the gun dropped, doubled over, a few staggering steps, then down
    bool Collapsed = false;         // ... and down now, lying there
    glm::vec3 StaggerTo{0.0f};      // where he staggers to (picked once, entering Wounded)
    bool HasStaggerTo = false;
    bool WoundRolled = false;       // the 35% chance has been taken this life
    // Shot down by a leg, he goes down as a powered ragdoll (NpcDirector::StartWoundFall, after this frame's late pose) while alive:
    // Ragdoll is set and he is not Dead. Once down and slow he takes the animated prone pose back (RecoverFromFall) - or, landed on
    // his back, lies there as he fell.
    bool FallPending = false;
    glm::vec3 FallHitDir{0.0f}, FallHitPoint{0.0f};
    float FallHitShove = 0.0f;
    int FallHitPart = -1;
    float FallStartedAt = -1.0f;
    float DownAt = -1.0f;           // down: the fall came to rest then (-1: not yet) ...
    bool Recovered = false;         // ... and the body has blended back into the animated prone pose (on his back: as soon as he is
                                    // down) - the brain waits for this before it crawls
    bool CanCrawl = false;          // ... on his belly, and able to crawl (false: on his back, he lies where he fell)
    float FallTime = -1.0f;         // seconds from the hit to down (tests)
    int RollTries = 0;              // on his back after the fall: the heaves made to roll onto his belly ...
    float NextRollAt = 0.0f;        // ... and when the next may come
    float RollHeaveUntil = -1.0f;   // ... the one under way pushes until then (an effort, not a kick) ...
    glm::vec3 RollSide{0.0f};       // ... over toward this side (world, flat) ...
    bool RollOntoLeft = false;      // ... his left (else his right): the good leg's side, unless a wall is there
    bool TurnedOver = false;        // the heaves came to nothing: the animated pose turned him over instead (Wound Roll Blend Time)
    float FallFrontUp = 0.0f;       // how far the chest's and pelvis's front faced up when he came to rest (-1 face down .. 1 face up)
    bool DiedFlinching = false;     // (tests) a hit flinch was still on the pose when he was killed
    // The drawn bones' jumps over the fall's handover (tests): the largest one-frame move of the tracked bones in the frame the
    // animated pose takes over and over the blend after it, and those bones last frame.
    float HandoverJump = -1.0f, BlendJump = -1.0f;
    glm::vec3 JumpBones[6]{};
    bool HaveJumpBones = false;
    // Animation level of detail (NpcDirector::Think): the pose passes run one frame in every Lod.
    int Lod = 1;
    bool Animate = true;            // this frame runs the animators and the late pose
    float LateDt = 0.0f;            // time since the late pose last ran (frames skipped by the LOD)
    float SpawnedAt = 0.0f;
    float PlayerDist = 0.0f;
    bool OnScreen = true;
    // Dying: the ragdoll starts after this frame's late pose (so it takes the pose the body died in).
    bool DeathPending = false;
    glm::vec3 DeathDir{0.0f, 0.0f, 1.0f}, DeathPoint{0.0f};
    float DeathShove = 40.0f;
    int DeathPart = -1;
    float DeathPop = -1.0f;         // metres the tracked bones jumped between the last animated frame and the first ragdoll one
    glm::vec3 LastBones[3]{};       // head, left hand, left foot as last posed (only tracked when asked)
    bool HaveLastBones = false;

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
    float ReloadHold = 0.0f;                // 0..1 (eased): reloading, the gun's camera levels (FirstPersonBody's reload body hold)
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
    bool Reloading = false;         // the weapon is in a reload state (read once a frame in AimAndFire) ...
    bool Pumping = false;           // ... or racking the pump
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
    float Scores[kBehaviourCount] = {};          // the last decision's behaviour scores, for the overlay
    NpcIntent Intent;
    std::string Why;                // the last decision's reason, for the overlay
    // Squad callouts: the edges already called, and the next idle chatter.
    bool VcReloading = false, VcCovering = false, VcSuspicious = false;
    float NextChatter = 0.0f;
    // Tactics.
    float BoundWaitFrom = -1.0f;    // about to bound across open ground: waiting since then for covering fire (< 0: not waiting)
    float CoverFireOrder = -1e9f;   // told to give a bounding squadmate covering fire until then
    float PinnedSince = -1.0f;      // heavy suppression since then (< 0: not pinned)
    float LastBlindFire = -1e9f;
    float GunTuck = 0.0f;           // 0..1: the gun drawn up and in off a wall it would go through (eased) ...
    float GunReach = 0.8f;          // ... the eye to the muzzle, untucked (m) ...
    float GunDrop = 0.0f;           // ... and how far the hold points the muzzle below the camera's line (the unit vectors' y) ...
    float GunSide = 0.0f;           // ... and off to its side (along the camera's right) ...
    float TuckYaw = 0.0f;           // ... the facing (degrees) along the wall the tuck swings it to
    float BlindLift = 0.0f;         // 0..1: how far the gun is out for blind fire (eased) ...
    glm::vec3 BlindOffset{0.0f};    // ... and where to: up over low cover, out past the edge of high cover
    float MeleeAt = -1e9f;          // a rifle-butt strike started then (the gun thrusts, the blow lands kMeleeHitTime in)
    bool MeleeLanded = true;        // ... and whether its blow has been resolved
};

// Soldiers are named "Soldier <n>": n is the unit number on the radio and in the kill feed.
inline int UnitNumber(const Npc& n) {
    const size_t sp = n.Name.find(' ');
    return sp == std::string::npos ? n.Index + 1 : std::atoi(n.Name.c_str() + sp + 1);
}
