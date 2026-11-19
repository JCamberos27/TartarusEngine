#pragma once

#include "Npc/NpcBody.h"
#include "Npc/NpcHitboxes.h"
#include "Npc/NpcRagdoll.h"
#include "PhysicsWorld.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

// One training dummy: its body, its capsule and crowd agent, where it wanders and what has hit it. NpcDirector owns these
// and runs them.

enum class Gait : unsigned char { Still, Walk, Jog, Run };

struct Npc {
    int Index = -1;
    float PaceSpeed = 0.0f; // m/s the gait asks (the crowd's max speed): the standing pace (NpcDirector::Move)
    bool OnFloor = true;    // the capsule touched the floor this frame
    entt::entity Root = entt::null;
    PhysicsWorld::CharacterId Cct = PhysicsWorld::kNoCharacter;
    int Agent = -1;                // crowd agent
    int SpawnIndex = -1;
    bool DevSpawned = false;        // put there by F9: never respawns
    bool Roam = true;               // wanders the navigation mesh round its post (Dummy Spawn's Roam)
    std::string Name;

    NpcBody Body;
    std::unique_ptr<NpcRagdoll> Ragdoll;   // once dead
    std::unique_ptr<NpcHitboxes> Hitboxes; // while alive and near the player: the bones the player's rounds hit
    int HitboxTries = 0;

    // Physical state.
    glm::vec3 Feet{0.0f};
    glm::vec3 Velocity{0.0f};
    float FallSpeed = 0.0f;
    glm::vec3 Eye{0.0f};            // the head bone

    // Health.
    float Health = 100.0f, MaxHealth = 100.0f;
    bool Dead = false;
    float DiedAt = 0.0f;
    float UnseenSince = -1.0f;      // a corpse: since when the player hasn't seen it (-1: in sight now)
    float NextSightCheck = 0.0f;    // a corpse: when its line of sight is next checked
    float LastHurt = -1e9f;
    float StaggerUntil = 0.0f;      // a heavy hit: slowed until then ...
    glm::vec3 PushVel{0.0f};        // ... and shoved back along the round (m/s, decays)
    bool DiedFlinching = false;     // (tests) a hit flinch was still on the pose when it was killed
    // Animation level of detail (NpcDirector::UpdateLod): the pose passes run one frame in every Lod.
    int Lod = 1;
    bool Animate = true;            // this frame runs the animators and the late pose
    float LateDt = 0.0f;            // time since the late pose last ran (frames skipped by the LOD)
    // The last late pose's finished pose (foot IK, look, flinch: as drawn), LateDt old; a kill's ragdoll takes its limbs' velocity
    // from it (NpcDirector::Kill). Valid false until the first late pose.
    NpcRagdoll::BoneSnapshot LastPose;
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
    float FallSoundAt = -1.0f;      // the body hits the ground (seconds, director time)

    // Where it goes (NpcDirector::Roam, or a test's ScriptedMove).
    bool WantMove = false;
    glm::vec3 MoveTarget{0.0f};
    Gait Pace = Gait::Walk;
    glm::vec3 LookPoint{0.0f};      // what the head looks at
    glm::vec3 Goal{0.0f};           // the crowd's current target
    bool HasGoal = false;
    glm::vec3 PostPos{0.0f};        // where it spawned: it wanders round this
    int RoamPhase = 0;              // 0 standing about, 1 walking to MoveTarget
    float IdleUntil = 0.0f;         // standing about until then
    float RoamSince = 0.0f;         // set off for MoveTarget then
    float BlockedTime = 0.0f;       // moving but not getting anywhere
    bool TurningFirst = false;      // setting off behind itself: held while the body turns on the spot (NpcDirector::Move)
    float TurnFirstTime = 0.0f;     // ... for this long

    std::optional<NpcFlinch> Flinch;                    // the hit-flinch kicks still settling
    std::optional<std::pair<glm::vec3, Gait>> Scripted; // a test's order (NpcDirector::ScriptedMove): where, at what pace
    struct DeathCapture { NpcRagdoll::BoneSnapshot Bones; float Dt = 0.0f; };
    std::optional<DeathCapture> DeathBones;             // from the killing hit to the ragdoll's start: the pose it died in, its age
};

// Dummies are named "Dummy <n>": n is the number in the kill feed.
inline int UnitNumber(const Npc& n) {
    const size_t sp = n.Name.find(' ');
    return sp == std::string::npos ? n.Index + 1 : std::atoi(n.Name.c_str() + sp + 1);
}
