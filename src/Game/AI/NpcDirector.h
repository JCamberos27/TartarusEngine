#pragma once

#include "Combat/Damage.h"
#include "Components.h" // SquadSettingsComponent (Dummy Settings), RagdollSettingsComponent
#include "NavMesh.h"
#include "Npc.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

class AssetLibrary;
class CombatFx;
class World;
struct FirstPersonWeaponGameplay;

// The player this frame, as the dummies need it (filled by the host from Player and the camera).
struct PlayerSnapshot {
    bool Valid = false;
    glm::vec3 Eye{0.0f};
    glm::vec3 Feet{0.0f};
    glm::vec3 Velocity{0.0f};
    glm::vec3 Forward{0.0f, 0.0f, -1.0f}; // where the view looks
    float Height = 1.85f, Radius = 0.3f;  // the capsule as it stands now
};

// Runs the training dummies in Play: builds the navigation mesh from the scene, spawns a dummy at each of the scene's Dummy
// Spawns (Soldier.json bodies, unarmed), and every frame walks them about the mesh (Roam), poses them, and keeps their
// hitboxes on the bones. The player's rounds come in through OnPlayerHit: a dummy flinches, staggers on a heavy hit, bleeds
// (TakeFleshHits, for the blood), and dies into a ragdoll. Everything it creates is runtime-only and destroyed by Stop.
//
// Frame order: Think (after the player moves, before the animators), LateUpdate (after them and the player's own late pose).
class NpcDirector {
public:
    NpcDirector();
    ~NpcDirector();

    // Remembers the scene's spawns and settings; the navigation mesh and the first dummies come on the first Think, once
    // the physics world is up. False when the scene has no Dummy Spawn.
    // `allowEmpty`: start even with no Dummy Spawn (F9's first press): no dummies yet, the navmesh seeded from the player.
    bool Start(World& world, AssetLibrary& assets, bool allowEmpty = false);
    // Reads the scene's Dummy Settings and Ragdoll Settings (Start does; the defaults without them).
    void ApplySettings(const entt::registry& reg);
    const SquadSettingsComponent& Settings() const { return m_Cfg; }
    const RagdollSettingsComponent& RagdollSettings() const { return m_RagdollCfg; }
    void Stop(World& world);
    bool Active() const { return m_Active; }

    void Think(World& world, AssetLibrary& assets, float dt, const PlayerSnapshot& player);
    void LateUpdate(World& world, float dt, const PlayerSnapshot& player);

    // One of the player's rounds struck `entity` at `point` travelling `dir` from `origin`: true when that was a dummy
    // (living or dead; no bullet hole). `aliveWhenHit` gates hit feedback; `killed` is true only when this hit kills a
    // living dummy, and `head` identifies its hit zone.
    bool OnPlayerHit(World& world, unsigned entity, const glm::vec3& point, const glm::vec3& origin, const glm::vec3& dir,
                     const FirstPersonWeaponGameplay& weapon, bool* killed = nullptr, bool* head = nullptr,
                     bool* aliveWhenHit = nullptr);

    // Every round that went into a dummy this frame (living or dead), for the blood (Game/Combat/BloodFx). `Entity` is the
    // dummy's root; `Part` the hitbox / ragdoll part index when known (-1 otherwise).
    struct FleshHit {
        glm::vec3 Point, Direction;
        unsigned Entity;
        int Part;
        float Damage;
        bool Killed, Head, Corpse;
        int Pellets;
        glm::vec3 Origin;  // where the round came from
        bool ByPlayer;     // the player fired it
    };
    std::vector<FleshHit> TakeFleshHits();

    // --- debug / tests ---
    const std::vector<std::unique_ptr<Npc>>& Npcs() const { return m_Npcs; }
    // Dummy `index` has hit-flinch kicks still settling (for tests).
    bool FlinchActive(int index) const { const auto it = m_Flinch.find(index); return it != m_Flinch.end() && it->second.Active(m_Now); }
    const NavMesh& Nav() const { return m_Nav; }
    const NavCrowd& Crowd() const { return m_Crowd; }
    CombatFx* Fx = nullptr;    // flesh hits and body falls (optional; the host owns it)
    bool FootIKEverywhere = false;     // feet onto the ground at any distance and out of view (tests)
    float Now() const { return m_Now; }
    float LastThinkMs() const { return m_ThinkMs; }
    float LastLateMs() const { return m_LateMs; }
    // Per-sub-system CPU time (ms per frame), for the profiler and --npc-test: Think's move, LateUpdate's body / ragdolls /
    // hitboxes. Averages skip the first two seconds.
    enum Sub { SubMove, SubBody, SubRagdoll, SubHitbox, SubCount };
    static const char* SubName(int s);
    struct CostStats { double Sum = 0.0; float Max = 0.0f; int Frames = 0; float Avg() const { return Frames ? (float)(Sum / Frames) : 0.0f; } };
    const CostStats& ThinkCost() const { return m_ThinkStat; }
    const CostStats& LateCost() const { return m_LateStat; }
    const CostStats& SubCost(int s) const { return m_SubStat[s]; }
    // Lines for the AI overlay (7 floats per vertex: xyz rgba, two per line): each dummy's way to its goal.
    void DebugLines(std::vector<float>& out) const;
    Npc* Find(int index);
    int Spawn(World& world, AssetLibrary& assets, int spawnIndex); // -1 on failure
    // F9: a dummy at `pos` facing `yawDeg`, no respawn; -1 before the navmesh is up.
    int SpawnAt(World& world, AssetLibrary& assets, const glm::vec3& pos, float yawDeg, bool roam = true);
    // F12: dummy `index` and its body gone at once (alive or a corpse).
    void Remove(World& world, int index);
    bool Started() const { return m_Started; }
    // The navmesh's cache file and its input hash (after the first Think): F6 Nav shows it again outside Play.
    const std::string& NavCachePath() const { return m_NavCachePath; }
    std::uint64_t NavHash() const { return m_NavHash; }
    int ReusedBodies() const { return m_Reused; }                  // spawns that took a pooled body
    // The dummy dies this frame; its ragdoll starts after this frame's late pose (so it takes the pose it died in), the
    // shove on part `part` (-1: the part nearest `point`).
    void Kill(World& world, Npc& npc, const glm::vec3& dir, const glm::vec3& point, float shove = 40.0f, int part = -1);
    // Tests: dummy `index` heads for `target` at `pace` whatever its roam wants; EndScriptedMove hands it back. Cleared when it dies.
    void ScriptedMove(int index, const glm::vec3& target, Gait pace = Gait::Walk) { m_Scripted[index] = {target, pace}; }
    void EndScriptedMove(int index) { m_Scripted.erase(index); }
    bool TrackDeathPop = false;        // record the bones' last animated pose so the ragdoll's first frame can be compared
    bool NoLod = false;                // every dummy animates every frame (tests)
    // Tests: where the crowd has dummy `n`'s agent, and the corner it steers for.
    glm::vec3 AgentPosition(const Npc& n) const { return n.Agent >= 0 ? m_Crowd.Position(n.Agent) : glm::vec3(0.0f); }
    glm::vec3 AgentCorner(const Npc& n) const { return n.Agent >= 0 ? m_Crowd.NextCorner(n.Agent) : glm::vec3(0.0f); }

    struct SpawnPoint {
        glm::vec3 Pos{0.0f};
        float Yaw = 0.0f;
        int OutfitSeed = 0;
        bool Bare = false; // dressed in its own skin, no clothing
        bool Roam = true;
    };
    // Whether the player could see `point` now: in front of the view (a cone a little wider than the screen), near enough and
    // not behind solid geometry. Corpses only ever go while unseen.
    static bool PlayerSees(const PlayerSnapshot& p, const glm::vec3& point);
    const std::vector<SpawnPoint>& SpawnPoints() const { return m_Spawns; }

private:
    bool LateStart(World& world, AssetLibrary& assets);
    void Roam(Npc& n);
    void Move(World& world, Npc& n, float dt);
    // `part`: the hitbox part struck (NpcRagdoll's order), -1 when unknown (the capsule: only the zone is known).
    void ApplyDamage(World& world, Npc& n, float amount, HitZone zone, const glm::vec3& point, const glm::vec3& dir, int part = -1);
    void FinishDeath(World& world, Npc& n);
    void LatePose(World& world, Npc& n, float dt);
    void UpdateHitboxes(Npc& n, const PlayerSnapshot& p, bool posed);
    void UpdateLod(World& world, Npc& n, const PlayerSnapshot& p);
    // Gone: its capsule, agent and physics. `keepBody`: its entity tree goes into the pool for the next spawn (hidden), else
    // it is destroyed.
    void Despawn(World& world, Npc& n, bool keepBody = false);
    void Respawns(World& world, AssetLibrary& assets);
    void BuildNav(World& world);
    void UpdatePersistence(World& world, const PlayerSnapshot& p);

    bool m_Active = false, m_Started = false;
    float m_Now = 0.0f;
    std::vector<SpawnPoint> m_Spawns;
    static constexpr int kDevSpawn = -2; // Spawn's index for m_DevPoint (SpawnAt)
    SpawnPoint m_DevPoint;
    std::string m_NavCachePath;
    std::uint64_t m_NavHash = 0;
    SquadSettingsComponent m_Cfg;            // the scene's Dummy Settings (defaults without one)
    RagdollSettingsComponent m_RagdollCfg;   // ... and Ragdoll Settings
    struct DeathCapture { NpcRagdoll::BoneSnapshot Bones; float Dt = 0.0f; };
    std::unordered_map<int, DeathCapture> m_DeathBones; // by dummy index, from the hit to the ragdoll's start
    std::unordered_map<int, NpcFlinch> m_Flinch;       // by dummy index: the hit-flinch kicks still settling
    NpcHoldSettings m_HoldSettings;        // the scene player's First Person Body numbers
    std::string m_BodyJson;
    std::vector<std::unique_ptr<Npc>> m_Npcs;
    // A dummy's built body - its entity tree, and its animators as they came from the prefab - kept from its corpse going to
    // the next spawn instead of destroyed and built again.
    struct DummyBody;
    std::unordered_map<entt::entity, std::shared_ptr<DummyBody>> m_Bodies; // per dummy, living or lying, by root
    std::vector<std::shared_ptr<DummyBody>> m_Pool;                        // hidden, ready for the next spawn
    int m_Reused = 0;
    std::shared_ptr<DummyBody> BuildBody(World& world, AssetLibrary& assets); // from the body prefab; null on failure
    struct PendingRespawn { float At; int SpawnIndex; };
    std::vector<PendingRespawn> m_RespawnTimers;
    NavMesh m_Nav;
    NavCrowd m_Crowd;
    int m_PlayerAgent = -1;
    std::vector<FleshHit> m_FleshHits;
    std::mt19937 m_Rng{0x5eedu};
    PlayerSnapshot m_Player;
    float m_ThinkMs = 0.0f, m_LateMs = 0.0f;
    int m_NextName = 1;
    int m_Frame = 0;
    std::unordered_map<int, std::pair<glm::vec3, Gait>> m_Scripted; // ScriptedMove, by dummy index
    // Cost accounting (see Sub).
    struct SubTimer;
    float m_SubFrame[SubCount] = {};
    CostStats m_SubStat[SubCount], m_ThinkStat, m_LateStat;
    void FlushCosts(bool think);
};
