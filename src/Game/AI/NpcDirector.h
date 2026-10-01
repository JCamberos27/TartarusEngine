#pragma once

#include "AiMath.h"
#include "Combat/Damage.h"
#include "CoverSystem.h"
#include "NavMesh.h"
#include "Npc.h"
#include "ShellCasings.h" // CasingSpawn
#include "SquadVoice.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <vector>

class AssetLibrary;
class CombatFx;
class World;
struct FirstPersonControllerComponent;

// The player as the enemy squad sees them this frame (filled by the host from Player, the camera and
// the player's weapon).
struct PlayerSnapshot {
    bool Valid = false;
    glm::vec3 Eye{0.0f};
    glm::vec3 Feet{0.0f};
    glm::vec3 Velocity{0.0f};
    glm::vec3 Forward{0.0f, 0.0f, -1.0f}; // where the view looks
    float Height = 1.85f, Radius = 0.3f;  // the capsule as it stands now
    bool Crouched = false;
    bool Dead = false;
    bool Fired = false;                    // a round left the player's gun this frame
    bool Reloading = false;
    bool Sprinting = false;
    float Health = 1.0f;                   // 0..1
};

// Runs the enemy squad in Play: builds the navigation mesh and cover from the scene, spawns soldiers
// at the scene's NPC Spawns (Soldier.json bodies carrying the player's own AK / Remington on a camera
// at their eyes), and every frame lets them perceive, decide (NpcBrain.cpp), move, aim and shoot.
// Rounds the player lands on them come in through OnPlayerHit; theirs that land on the player go
// out through TakePlayerDamage. Everything it creates is runtime-only and destroyed by Stop.
//
// Frame order: Think (after the player moves, before the animators), LateUpdate (after them and the
// player's own late pose).
class NpcDirector {
public:
    NpcDirector();
    ~NpcDirector();

    // Remembers the scene's spawns and settings; the navigation mesh and the first soldiers come on the
    // first Think, once the physics world is up. False when the scene has no NPC Spawn.
    bool Start(World& world, AssetLibrary& assets, const FirstPersonControllerComponent* playerConfig);
    void Stop(World& world);
    bool Active() const { return m_Active; }

    void Think(World& world, AssetLibrary& assets, float dt, const PlayerSnapshot& player);
    void LateUpdate(World& world, float dt, const PlayerSnapshot& player);

    // One of the player's rounds struck `entity` at `point` travelling `dir` from `origin`: true when
    // that was a soldier (it takes the damage; no bullet hole). `killed` / `head` for the hitmarker.
    bool OnPlayerHit(World& world, unsigned entity, const glm::vec3& point, const glm::vec3& origin, const glm::vec3& dir,
                     const FirstPersonWeaponGameplay& weapon, bool* killed = nullptr, bool* head = nullptr);
    // Near misses: a round of the player's passed this close to soldiers' heads (suppression).
    void OnPlayerShotLine(const glm::vec3& origin, const glm::vec3& end);
    // The player respawned: the squad forgets them and resets.
    void OnPlayerRespawned();

    std::vector<DamageEvent> TakePlayerDamage();
    struct Impact { glm::vec3 Point, Normal; unsigned Entity; float Radius; };
    std::vector<Impact> TakeImpacts();
    std::vector<CasingSpawn> TakeEjections();

    // --- debug / tests ---
    const std::vector<std::unique_ptr<Npc>>& Npcs() const { return m_Npcs; }
    const NavMesh& Nav() const { return m_Nav; }
    const CoverSystem& Cover() const { return m_Cover; }
    CombatFx* Fx = nullptr;    // gun reports, flashes, tracers, whizzes (optional; the host owns it)
    SquadVoice& Voice() { return m_Voice; }
    const SquadVoice& Voice() const { return m_Voice; }
    void SetDifficulty(float d) { m_Difficulty = std::clamp(d, 0.25f, 3.0f); }
    float Difficulty() const { return m_Difficulty; }
    int SquadSize() const { return m_SquadSize; }
    bool Frozen = false;       // the AI stops deciding (bodies and weapons still run)
    bool HoldFire = false;     // nobody shoots
    bool MeshChecksEverywhere = false; // the weapon hold checks the drawn body at any distance (tests)
    bool FootIKEverywhere = false;     // feet onto the ground at any distance and out of view (tests)
    int ShootersNow() const { return m_ShootersNow; }
    // What the squad's tactics did (--npc-test).
    struct TacticStats {
        int Bounds = 0;          // bounds that had to wait for the player's attention ...
        int CoveredBounds = 0;   // ... and went with covering fire on
        int CoverOrders = 0;     // squadmates told to cover a bound
        int BlindFires = 0;
        int Melees = 0, MeleeHits = 0;
        int Flanks = 0, FlankFails = 0; // flanks started / given up for want of cover round the side
        int Pincers = 0;         // second flankers sent round the other side
        int HurtPushes = 0;      // pushes called on a badly hurt player
        int Startles = 0;
        int Backpedals = 0;      // a rifle too close: backing off while shooting
    };
    const TacticStats& Tactics() const { return m_Tactics; }
    int MaxShootersSeen() const { return m_MaxShooters; }
    float Now() const { return m_Now; }
    float LastThinkMs() const { return m_ThinkMs; }
    float LastLateMs() const { return m_LateMs; }
    // Per-sub-system CPU time (ms per frame), for the profiler and --npc-test: Think's perceive / brain / squads /
    // move / aim+fire, LateUpdate's body / weapon rig / weapon hold / ragdolls / hitboxes. Averages skip the first two seconds.
    enum Sub { SubPerceive, SubBrain, SubSquads, SubMove, SubAimFire, SubBody, SubWeapon, SubHold, SubRagdoll, SubHitbox, SubCount };
    static const char* SubName(int s);
    struct CostStats { double Sum = 0.0; float Max = 0.0f; int Frames = 0; float Avg() const { return Frames ? (float)(Sum / Frames) : 0.0f; } };
    const CostStats& ThinkCost() const { return m_ThinkStat; }
    const CostStats& LateCost() const { return m_LateStat; }
    const CostStats& SubCost(int s) const { return m_SubStat[s]; }
    // Lines for the AI overlay (7 floats per vertex: xyz rgba, two per line).
    void DebugLines(std::vector<float>& out) const;
    // A scripted test can put a soldier somewhere and give it a goal.
    Npc* Find(int index);
    int Spawn(World& world, AssetLibrary& assets, int spawnIndex); // -1 on failure
    // The soldier dies this frame; its ragdoll starts after this frame's late pose (so it takes the pose it died in),
    // the shove on part `part` (-1: the part nearest `point`).
    void Kill(World& world, Npc& npc, const glm::vec3& dir, const glm::vec3& point, float shove = 40.0f, int part = -1);
    // Tests: the chance a soldier left under 20% health by a leg or torso hit goes down wounded (default 0.35).
    void SetWoundChance(float chance) { m_WoundChance = chance; }
    bool TrackDeathPop = false;        // record the bones' last animated pose so the ragdoll's first frame can be compared
    bool NoLod = false;                // every soldier animates every frame (tests)

    struct SpawnPoint {
        glm::vec3 Pos{0.0f};
        float Yaw = 0.0f;
        int Weapon = 0, Squad = 0, Brain = 0, OutfitSeed = 0;
        float Skill = 0.5f;
    };
    const std::vector<SpawnPoint>& SpawnPoints() const { return m_Spawns; }

private:
    friend class NpcBrain;
    struct Noise { glm::vec3 Pos; float Radius, Loudness, Time; int Source; };
    struct Squad {
        std::vector<int> Members;
        float NextRoles = 0.0f;
        float NextTokens = 0.0f;
        int FlankHolder = -1, PushHolder = -1;
        int PincerHolder = -1;        // a second flanker, for the other side, while the first is on its way
        float PincerDoneAt = -1e9f;
        float PlayerHurtPushAt = -1e9f;
        // A flank token handed back (left the flank, or arrived).
        void DropFlank(int index, float now) {
            if (FlankHolder == index) { FlankHolder = -1; FlankDoneAt = now; }
            if (PincerHolder == index) { PincerHolder = -1; PincerDoneAt = now; }
        }
        std::vector<int> Attackers;
        TargetMemory Shared;          // what the squad knows, merged from its members
        float SharedAt = -1e9f;
        float PushUntil = 0.0f;       // a coordinated push is on until then
        float PlayerReloadingSeen = -1e9f;
        float LastDeath = -1e9f;
        float FlankDoneAt = -1e9f;    // the last flank reached its cover (the next waits a while)
        // Fire and maneuver: a soldier waiting to bound asks for covering fire; a squadmate shooting is it.
        int CoverRequest = -1;        // who is waiting (-1: nobody)
        float CoverRequestAt = -1e9f; // refreshed every frame it waits
        int CoverFirer = -1;          // the squadmate told to give it
        float CoverFireUntil = -1e9f; // someone other than the requester is shooting: covered until then
    };

    bool LateStart(World& world, AssetLibrary& assets);
    void Perceive(World& world, Npc& n, const PlayerSnapshot& p, float dt);
    void UpdateSquads(const PlayerSnapshot& p, float dt);
    void Move(World& world, Npc& n, float dt);
    void AimAndFire(World& world, Npc& n, const PlayerSnapshot& p, float dt);
    void HandleShots(World& world, Npc& n, const PlayerSnapshot& p, const glm::vec3& muzzleShift);
    // `part`: the hitbox part struck (NpcRagdoll's order), -1 when unknown (the capsule: only the zone is known).
    void ApplyDamage(World& world, Npc& n, float amount, HitZone zone, const glm::vec3& point, const glm::vec3& dir, int attacker,
                     int part = -1);
    void BecomeWounded(Npc& n);
    void FinishDeath(World& world, Npc& n);
    void LatePose(World& world, Npc& n, float dt, const PlayerSnapshot& p, bool alive);
    void UpdateHitboxes(Npc& n, const PlayerSnapshot& p, bool posed);
    void UpdateLod(World& world, Npc& n, const PlayerSnapshot& p);
    void Despawn(World& world, Npc& n);
    // A radio bark (SquadVoice picks the line, the channel and the cooldown). NpcDirectorVoice.cpp.
    void Callout(Npc& n, Bark ev);
    void UpdateVoice(const PlayerSnapshot& p);
    void Respawns(World& world, AssetLibrary& assets, const PlayerSnapshot& p);
    bool CanSee(const Npc& n, const glm::vec3& point) const; // a solid-world sight line
    void BuildNav(World& world);

    bool m_Active = false, m_Started = false;
    float m_Now = 0.0f;
    std::vector<SpawnPoint> m_Spawns;
    int m_SquadSize = 4;
    float m_RespawnDelay = 8.0f, m_Difficulty = 1.0f, m_DamageScale = 0.45f;
    bool m_Respawn = true;
    std::shared_ptr<FirstPersonControllerComponent> m_ViewConfig;
    NpcHoldSettings m_HoldSettings;        // the scene player's First Person Body numbers
    std::string m_SoldierJson;
    std::vector<std::unique_ptr<Npc>> m_Npcs;
    std::vector<Squad> m_Squads;
    std::vector<float> m_RespawnTimers;     // per pending replacement
    NavMesh m_Nav;
    NavCrowd m_Crowd;
    int m_PlayerAgent = -1;
    CoverSystem m_Cover;
    std::vector<Noise> m_Noises;
    std::vector<DamageEvent> m_PlayerDamage;
    std::vector<Impact> m_Impacts;
    std::vector<CasingSpawn> m_Ejections;
    std::mt19937 m_Rng{0x5eedu};
    PlayerSnapshot m_Player;
    float m_PlayerUnseen = 0.0f;           // since any soldier last saw the player
    glm::vec3 m_PlayerPost{0.0f};          // where the player has been holding ...
    float m_PlayerStill = 0.0f;            // ... and for how long (within 2.5 m): a camper gets flanked
    glm::vec3 m_PrevPlayerFeet{0.0f};
    float m_FootstepTimer = 0.0f;
    int m_ShootersNow = 0, m_MaxShooters = 0;
    float m_ThinkMs = 0.0f, m_LateMs = 0.0f;
    int m_NextName = 1;
    float m_WoundChance = 0.35f;
    int m_LookCursor = 0;
    // One cover search (raycasts + paths, the AI's priciest call) per frame across the squad: a second
    // soldier wanting one waits a frame, so a volley that sends everyone to cover is no spike.
    int m_Frame = 0, m_CoverSearchFrame = -1;
    SquadVoice m_Voice;
    bool m_PlayerWasDead = false;
    TacticStats m_Tactics;
    // Where soldiers fell lately (a ring): cover near them is avoided for a while (DangerScale).
    static constexpr int kDeathMemory = 8;
    glm::vec3 m_DeathPos[kDeathMemory]{};
    float m_DeathTime[kDeathMemory]{};
    int m_DeathCount = 0, m_DeathNext = 0;
    void UpdateCoverFire(Squad& s);
    void UpdateMelee(Npc& n, const PlayerSnapshot& p);
    // Cost accounting (see Sub).
    struct SubTimer;
    float m_SubFrame[SubCount] = {};
    CostStats m_SubStat[SubCount], m_ThinkStat, m_LateStat;
    void FlushCosts(bool think);
};
