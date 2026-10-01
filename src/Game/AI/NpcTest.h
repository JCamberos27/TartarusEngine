#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

class FirstPersonPresentation;
class NpcDirector;
class Player;
class PlayerVitals;
class World;

// --npc-test [scenario] [--smoke-shots dir]: the enemy squad, headless, in scenes/Arena.json. The host
// enters Play, hands the player to this script each frame (Drive), and reports (After). It logs what
// every soldier is doing, checks the fight works end to end, and frames the Scene view on the soldiers
// for screenshots.
//   watch   (default) the player stands at the spawn, untouchable: the squad notices, takes cover, shoots
//   fight   the player shoots back at whoever it can see; the squad takes losses and replaces them
//   die     the player stands in the open and can be killed: death and respawn
//   deaths  hitboxes, hit reactions and deaths: the soldiers stand still while rays are cast at every bone, then are shot
//           dead through the head, chest, thigh and forearm (limp, stagger, ragdoll, no pose pop), a corpse is shot, and
//           two are wounded (crawl, "Unit down", dies on the next hit / bleeds out)
//   tactics the player pins a soldier behind low cover with bursts over its head (blind fire), the squad bounds under
//           covering fire, then the player steps up to a soldier and is struck with its rifle butt
//   pose    the weapon hold, close up: an AK and a Remington soldier, the AI frozen, put through aim level /
//           up / down / to the side, low ready, crouched, strafing, reloading and sprinting; four views of each
//           and the gun's / elbows' / hands' clearances (NpcBody::MeasureHold) logged and checked
class NpcTest {
public:
    explicit NpcTest(const std::string& scenario);

    // Before Player::Update: the move keys (ScriptedMove), the view, the trigger.
    void Drive(Player& player, FirstPersonPresentation& weapon, NpcDirector& npcs, PlayerVitals& vitals, float dt);
    // After everyone's late pose: checks and the log.
    void After(World& world, NpcDirector& npcs, const PlayerVitals& vitals, const Player& player);
    bool Done() const { return m_Done; }
    int Checks() const { return m_Checks; }
    int Failures() const { return m_Failures; }
    // Non-empty on frames to capture: the file stem.
    const std::string& ShotName() const { return m_Shot; }
    // NPC_TEST_RECORD=<dir>: every other frame of both views goes there as JPEGs (for a video).
    const std::string& RecordDir() const { return m_RecordDir; }
    // Where to put the Scene view (false = leave it).
    bool SceneCamera(glm::vec3& pos, float& yaw, float& pitch) const;
    // The player's trigger this frame (the host feeds it to the weapon).
    bool Firing() const { return m_Firing; }
    bool Aiming() const { return !m_Target.empty(); }   // sights up (rounds go where the view looks)
    bool WantsReload() const { return m_Reload; }
    bool WantsAiOverlay() const { return m_Scenario == "sandbox"; } // the AI overlay in the sandbox shots

private:
    void Check(bool ok, const std::string& what);
    void CheckRadio(NpcDirector& npcs);
    void PrintCosts(const NpcDirector& npcs) const;
    void Pose(World& world, NpcDirector& npcs, float now);
    void Deaths(World& world, NpcDirector& npcs, float now);
    // tactics
    std::string m_TVictim;
    int m_TBlindShots = 0, m_TMeleeShots = 0;
    float m_TShotAt = -1e9f, m_TNextCrack = 0.0f;
    // deaths
    int m_DStep = 0;
    float m_DAt = 0.0f;
    std::vector<std::string> m_DUsed;
    std::string m_DNpc[2];
    int m_DCase = 0;
    float m_DWorstPop = 0.0f, m_DWoundSpeed = 0.0f;
    int m_DDeaths = 0, m_DRagdolls = 0;
    glm::vec3 m_DCorpseBefore[11]{};
    glm::vec3 m_DBoneBefore[4]{};
    bool m_DCorpseAsleep = false, m_DKneelChecked = false;
    std::string m_Scenario;
    float m_Time = 0.0f;
    float m_Duration = 45.0f;
    bool m_Done = false;
    int m_Checks = 0, m_Failures = 0;
    std::string m_Shot;
    float m_NextLog = 0.0f;
    float m_NextShot = 0.0f;
    int m_ShotIndex = 0;
    int m_Focus = 0;          // which soldier the Scene view follows
    glm::vec3 m_CamPos{0.0f};
    float m_CamYaw = 0.0f, m_CamPitch = 0.0f;
    bool m_HaveCam = false;
    // What happened.
    float m_FirstKnown = -1.0f, m_FirstDamage = -1.0f, m_FirstCover = -1.0f, m_FirstShot = -1.0f;
    int m_MaxAlive = 0, m_Kills = 0, m_Spawned = 0;
    bool m_PlayerDied = false, m_PlayerRespawned = false;
    bool m_DevKilled = false;
    int m_DevKilledCount = 0, m_AliveAfterKill = -1;
    float m_HealthSeen = 0.0f;
    std::vector<std::string> m_BehavioursSeen;
    std::vector<std::string> m_Dead;
    std::string m_LastKill;
    std::string m_RecordDir;
    float m_LastKillAt = -1e9f;
    int m_KillShots = 0;
    bool m_Firing = false;
    float m_FireHold = 0.0f;
    float m_AimErr = -1.0f;
    bool m_Reload = false;
    std::string m_Target;
    int m_PlayerShots = 0, m_LastAmmo = -1;
    // pose
    std::string m_Probe[2];
    int m_PosePhase = -1;
    float m_PhaseStart = 0.0f;
    int m_PoseShots = 0;
    glm::vec3 m_ProbeHome[2]{};
    float m_WorstHead = 1e9f, m_WorstTorso = 1e9f, m_WorstElbow = 1e9f, m_WorstHand = 0.0f;
    std::string m_WorstHeadAt, m_WorstTorsoAt, m_WorstElbowAt, m_WorstHandAt;
};
