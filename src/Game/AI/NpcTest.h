#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

#include <glm/glm.hpp>

#include <string>
#include <utility>
#include <vector>

class AssetLibrary;
class FirstPersonPresentation;
class NpcDirector;
class NavMesh;
struct Npc;
class Player;
class PlayerVitals;
class BloodFx;
class World;

// --npc-test [scenario] [--smoke-shots dir]: the training dummy, headless. The host enters Play, hands the player to
// this script each frame (Drive), and reports (After). It checks the dummy end to end and frames the Scene view on it
// for screenshots.
//   labloco  (default) LocomotionLab's lane: the player and the dummy side by side through every gait, traced and recorded
//            every frame (NpcTest_LabLoco.cpp)
//   deaths   hitboxes, hit reactions and deaths (tests/scenes/Arena.json): the dummies stand still while rays are cast at
//            every bone, then are shot dead through the head, chest, thigh and forearm (flinch, stagger, ragdoll, no pose
//            pop), and a corpse is shot
//   flame    the player's muzzle flames: the AK's and the Remington's (second slot), shots from the hip
//            then on the sights (record it: NPC_TEST_RECORD, every frame)
//   revolver the .38 in tests/scenes/WeaponLab.json (NpcTest_Revolver.cpp): hip, the sights held (the sight line logged),
//            2 shots, a partial reload, 6 from the hip, a full reload (hammer, trigger and cylinder measured per shot,
//            every ejection and the cases' sleep counted), inspect, mag check, melee, walk and sprint, then the AK and
//            the 870 fired and reloaded (one case per shot / pump), each slot's ammo kept, one scroll notch; both views
//            shot at fixed moments, close on the gun and on the floor
//   feet     foot IK: a dummy's feet measured over the ground on the flat, then across and up the Arena's ramp
//   blood    the volumetric blood (tests/scenes/BloodTest.json): a wound, a chest kill against a wall, a head kill and a
//            corpse shot, each watched from the side in shots through the spray's flight
//   ragdoll  the deaths themselves (tests/scenes/RagdollTest.json): a dozen dummies, bare and dressed, standing, walking, jogging,
//            sprinting and strafing, at a ledge, a wall, a crate, on stairs and over another body, each killed by a round from
//            a set side; every death is frozen at moments through the fall and at rest and shot from four sides (front, side,
//            high three-quarter, ground level), and checked every frame: no part of a body inside another (self-collision),
//            none inside the ground, no joint past its range, the body asleep in time; then the corpses stay while looked at and
//            go once the view turns away
//   decals   (tests/scenes/DoorsLab.json) rounds and blood put on a corridor wall and floor at fixed spots, shot from
//            fixed Scene-view cameras: the before/after of bullet holes and blood moving to the clustered decals
//            (NpcTest_Decals.cpp, docs/DECALS.md)
//   doors    (tests/scenes/DoorsLab.json, no dummies) every door opened and the player walked through each one five ways -
//            straight, grazing the hinge jamb, grazing the latch jamb, strafing, sprinting on a diagonal - each pass measured:
//            through in time, the speed kept within half a metre of the wall, time stuck, frames inside anything
//            (NpcTest_Doors.cpp)
//   npcloco  a dummy through the gaits on LocomotionLab's floor, watched from the side (NpcTest_NpcLoco.cpp)
//   npcperf  a dozen roaming dummies on LocomotionLab at every LOD tier: the director's cost per dummy and the frame time
//            (NpcTest_NpcPerf.cpp)
//   roam, stopsteps: the player's own walking (NpcTest_Roam / _StopSteps.cpp); mmplayer covers the gaits (locomotion retired, Phase 6)
//   mmview   motion matching, Phase 2 (NpcTest_MmView.cpp, LocomotionLab): the baked takes (Manny.mmanim) checked offline on
//            the Quantum rig, then a playlist played on a bare Quantum body with the generated root drawn; SKIPs when unbaked
//   mmdb     motion matching, Phase 3 (NpcTest_MmDb.cpp, LocomotionLab): the runtime database's search (time, boxes vs brute
//            force, self-match, determinism), left/right coverage per gait, the gait speed table; then a Quantum body driven
//            by MotionMatcher + TrajectoryPredictor through a scripted course, desired and matched trajectories drawn
//   mmplayer motion matching, Phase 4 (NpcTest_Testbed.cpp, AnimTestbed.json): the player's body on motion matching
//            unarmed, through the testbed's parts A and C and its own part M - strafes and
//            backwards, the crouch, turning on the spot, the run (straight, turning, strafing), jumps: the label filters, the
//            speed warping, the root on the capsule, the airborne segment, the gait phase, foot IK; SKIPs when unbaked
//   testbed  project/scenes/AnimTestbed.json (tools/scenes/build_anim_testbed.py): only what the animation overhaul fixes, re-run
//            after every phase (NpcTest_Testbed.cpp) - the player's bob reaching the screen and the gait phase on the lane, the
//            corner peek's lean with the hands on the gun, stairs and a ramp (gait phase, leg reach, the eye's overshoot), the
//            hands through holster / draw; the standing dummy's 180-degree start turn, its ragdoll's inherited velocity and a
//            flinch that keeps its planted feet. NPC_TEST_RECORD: the frames and trace.csv
class NpcTest {
public:
    explicit NpcTest(const std::string& scenario);

    // Before Player::Update: the move keys (ScriptedMove), the view, the trigger.
    void Drive(Player& player, FirstPersonPresentation& weapon, NpcDirector& npcs, PlayerVitals& vitals, float dt);
    // After everyone's late pose: checks and the log.
    void After(World& world, NpcDirector& npcs, const PlayerVitals& vitals, const Player& player);
    // The host's assets, for the scenarios that put a dummy in with NpcDirector::SpawnAt (npcloco).
    AssetLibrary* Assets = nullptr;
    // The scene has no Dummy Spawn but the scenario spawns its own: the host starts the director empty (Start's allowEmpty).
    bool StartsEmptyDirector() const { return m_Scenario == "npcloco"; }
    bool Done() const { return m_Done; }
    int Checks() const { return m_Checks; }
    int Failures() const { return m_Failures; }
    // A check the host itself failed (the outfit wait's cap): counted and logged like any other.
    void Fail(const std::string& what) { Check(false, what); }
    // Non-empty on frames to capture: the file stem.
    const std::string& ShotName() const { return m_Shot; }
    // NPC_TEST_RECORD=<dir>: every other frame of both views goes there as JPEGs (for a video).
    const std::string& RecordDir() const { return m_RecordDir; }
    // Where a scenario's trace.csv goes: NPC_TEST_TRACE=<dir> (the trace alone, no frames - fast), else NPC_TEST_RECORD's.
    const std::string& TraceDir() const { return m_TraceDir; }
    int RecordStep() const; // 1: every frame (the muzzle flame, the matcher's poses), else 2
    // Where to put the Scene view (false = leave it).
    bool SceneCamera(glm::vec3& pos, float& yaw, float& pitch) const;
    // The player's trigger this frame (the host feeds it to the weapon).
    bool Firing() const { return m_Firing; }
    bool Aiming() const { return !m_Target.empty(); }   // sights up (rounds go where the view looks)
    bool WantsReload() const { return m_Reload; }
    // A weapon action trigger this frame (Inspect, MagCheck, Melee); the host applies it after the weapon's Update.
    const std::string& Action() const { return m_Action; }
    std::string m_Action;
    // World lines the host draws over the Game view (mmview: the generated root, the hips' facing, the ankles).
    void DebugLines(std::vector<float>& out) const;
    bool RunsWithoutSquad() const; // After() even in a scene with no dummies
    bool WantsCleanViews() const;  // no collider / physics-debug overlays
    // Time stands still this frame (the host steps the game by zero): the ragdoll scenario's multi-angle shots of one moment.
    bool Frozen() const { return m_RFreeze; }
    // The player's body starts afresh this frame (the host runs FirstPersonBody Stop + Start): the testbed's leg starts
    // (TARTARUS_TESTBED_START). Reading it clears it.
    bool TakeBodyReset() { const bool r = m_BodyReset; m_BodyReset = false; return r; }
    // The blood scenario's view of the blood, filled by the host each frame.
    struct BloodView {
        bool Loaded = false;
        int SpraysSpawned = 0, ActiveSprays = 0, DecalsSpawned = 0, PoolsSpawned = 0, SplatsSpawned = 0;
        int GearSplats = 0; // splats on the player's gun / hands / body
        bool LastSprayClipped = false;
        float GpuMsAvg = 0.0f, GpuMsMax = 0.0f;
        // v2 (the Knife packs): impact puffs, exits, headshot gore
        int Puffs = 0, BigHeadshots = 0;
        bool LastExited = false, LastExitFound = false;
        int Impacts = 0, TexturedHoles = 0; // ImpactFx bursts; bullet holes drawn as PRO Effects decals
    };
    // Fires a round into the world from `from` along `dir` (the host's world-impact path: burst + hole). False: a miss.
    std::function<bool(const glm::vec3& from, const glm::vec3& dir)> ShootWorld;
    const BloodView* BloodStats = nullptr;
    // --npc-test decals: blood the host's BloodFx puts on a surface (kind 0 a wall spatter, 1 a pool).
    std::function<void(int kind, const glm::vec3& at, const glm::vec3& normal, float size)> SpawnBlood;
    // --npc-test decals: the spray can (Decals P4). Spray starts a SprayCanAction of `piece` over `seconds` from
    // `from` along `dir` for `painter`; SprayState reports on every piece going on (saving and reading the game
    // state back when `roundTrip`).
    struct SprayView {
        int Pieces = 0;          // Spray Paint components in the world
        float MinProgress = 1.0f, MaxProgress = 0.0f;
        int Sounds = 0;          // spray-can sounds started
        bool Hissing = false;    // a held hiss live now
        bool Resident = false;   // every piece's decal maps are in the decal arrays
        int Saved = -1;          // pieces back from a save + load (-1: not asked)
        float SavedElapsed = 0.0f;
        int RuntimeDecals = 0;   // the DecalSystem's runtime list (the stains and holes)
    };
    std::function<bool(const std::string& piece, const glm::vec3& from, const glm::vec3& dir, const std::string& painter, float seconds)> Spray;
    std::function<void(SprayView& out, bool roundTrip)> SprayState;
    // The revolver scenario: the cases on the ground (all, asleep), and each of the player's ejections (its model).
    std::function<void(int& count, int& sleeping)> CasingState;
    // ... the brightest muzzle light (intensity, where), and the tracer streaks in flight.
    std::function<void(float& lit, glm::vec3& at, int& tracers)> MuzzleState;
    void OnPlayerEjection(const std::string& model);
    // An NPC's can: a character stood at `feet` facing `yawDeg` (0 = +Z) sprays `piece` through
    // SprayCanAction::ForCharacter (from its hand, along its facing).
    std::function<bool(const std::string& piece, const glm::vec3& feet, float yawDeg, float seconds)> SprayNpc;

private:
    void Check(bool ok, const std::string& what);
    // The foot slide meters' totals since the last call, one line a body ("[Slide] <leg> ..."), then cleared.
    void SlideLeg(const std::string& leg);
    void PrintCosts(const NpcDirector& npcs) const;
    void Deaths(World& world, NpcDirector& npcs, float now);
    void Feet(NpcDirector& npcs, float now);
    void Blood(World& world, NpcDirector& npcs, float now);
    void Ragdoll(World& world, NpcDirector& npcs, const Player& player);
    void Decals(World& world, NpcDirector& npcs, float now); // NpcTest_Decals.cpp
    void DecalsDrive(Player& player, float& wantYaw, float& wantPitch);
    // stopsteps (NpcTest_StopSteps.cpp)
    static float StopStepsSeconds();
    void StopStepsDrive(Player& player, float dt, float& wantYaw, float& wantPitch);
    void StopSteps(const Player& player, float now);
    bool m_SSPlaced = false;
    float m_SSClock = 0.0f;
    int m_SSSteps = 0;
    int m_SSLegSteps[16] = {};
    // npcperf (NpcTest_NpcPerf.cpp): what a dozen roaming dummies cost
    static float NpcPerfSeconds();
    void NpcPerfDrive(Player& player, float& wantYaw, float& wantPitch);
    void NpcPerf(World& world, NpcDirector& npcs, float now);
    // npcloco (NpcTest_NpcLoco.cpp): a dummy through the same gaits, watched from the side
    static float NpcLocoSeconds();
    void NpcLocoDrive(Player& player, float& wantYaw, float& wantPitch);
    void NpcLoco(World& world, NpcDirector& npcs, float now);
    void NpcLocoReport();
    bool m_NLPlaced = false;
    std::string m_NLProbe;
    int m_NLLeg = 0;
    float m_NLStart = 0.0f, m_NLArrived = -1.0f, m_NLPrev = 0.0f;
    // labloco (NpcTest_LabLoco.cpp): LocomotionLab's lane, the player and the dummy side by side, traced and recorded every frame
    static float LabLocoSeconds();
    void LabLocoDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch);
    void LabLoco(World& world, NpcDirector& npcs, const Player& player, float now);
    bool m_LabPlaced = false;
    float m_LabClock = 0.0f;
    std::vector<std::string> m_SlideBodies; // the NPCs SlideLeg reports on
    std::map<std::string, float> m_SlideWorstPlayer; // SlideLeg: the player's worst plant drift per leg (m)
    std::map<std::string, float> m_SlideWorstNpc;    // SlideLeg: the worst plant drift per leg of the NPCs it reports on (m)
    std::map<std::string, float> m_SlideContactPlayer; // ... the lesser of its ankle's and its ball's (a pivot is not a slide)
    std::vector<std::string> m_SkipLegs; // legs that didn't run (TestbedLegsOff): a check naming one is skipped, not failed
    int m_LabFrame = 0;
    // mmview (NpcTest_MmView.cpp): the baked motion matching takes on a Quantum body, the generated root drawn, the bake checked
    static float MmViewSeconds();
    void MmViewDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch);
    void MmView(World& world);
    void MmViewValidate();
    struct MmViewRun;
    std::shared_ptr<MmViewRun> m_Mm;
    // mmdb (NpcTest_MmDb.cpp): the motion matching runtime - search speed and correctness, left/right coverage, the gait
    // table, then a Quantum body driven by MotionMatcher through a scripted walk/jog/run/crouch course with debug draw
    static float MmDbSeconds();
    void MmDbDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch);
    void MmDb(World& world);
    void MmDbOffline();
    void MmDbLines(std::vector<float>& out) const;
    struct MmDbRun;
    std::shared_ptr<MmDbRun> m_MmDb;
    // testbed (NpcTest_Testbed.cpp): the animation overhaul's measurements in AnimTestbed.json
    static float TestbedSeconds();
    // TARTARUS_TESTBED_LEGS: the course's legs that won't run (empty: the whole course) - Check skips a check naming one.
    static std::vector<std::string> TestbedLegsOff();
    void TestbedDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch);
    void Testbed(World& world, NpcDirector& npcs, const Player& player, float now);
    // mmplayer (NpcTest_Testbed.cpp): the testbed's parts A and C plus part M on the motion matching body
    void MmPlayerFrame(World& world, const Player& player, int leg, const float (&foot)[3]);
    void MmPlayerChecks();
    void MmStressChecks();
    struct TestbedRun;
    std::shared_ptr<TestbedRun> m_Tb;
    bool m_BodyReset = false; // TakeBodyReset
    // doors (NpcTest_Doors.cpp)
    struct DoorSpot {
        std::string Name;
        glm::vec3 Centre{0.0f};  // the opening's middle, on the floor in the wall's plane
        glm::vec3 Normal{0.0f};  // through the opening (the leaf's +Z)
        glm::vec3 Along{0.0f};   // along the wall, toward the latch side
        float Half = 0.46f;      // half the opening's width (the apartment's 0.92 m, DoorsLab's doors too)
        bool Pack = false;       // an imported pack's door: its runs stop short of the furniture either side (DoorsPassSetUp)
        float Thick = 0.0f;      // the leaf's thickness (a pack door)
        float Swing = 1.0f;      // the side of the wall it opens to: +1 along Normal, -1 back (Swing Back)
        std::uint32_t Leaf = 0xFFFFFFFFu; // the leaf's entity (the Openable)
    };
    struct DoorRun {
        glm::vec3 Start{0.0f}, Dir{0.0f}, Heading{0.0f}, Prev{0.0f};
        float Before = 0.0f, After = 0.0f; // where it starts before the wall plane and ends past it, along Dir
        float Speed = 0.0f, Budget = 0.0f, Clock = 0.0f, Time = 0.0f, MinRatio = 0.0f, Stuck = 0.0f;
        int PenFrames = 0;
        bool Placed = false, HavePrev = false, Sprint = false, Strafe = false;
    };
    void Doors(World& world, const Player& player);
    void DoorsDrive(Player& player, float dt, float& wantYaw, float& wantPitch);
    void DoorsSetUp(World& world);
    void DoorsPassSetUp(const Player& player);
    void DoorsShut(World& world);
    // roam (NpcTest_Roam.cpp): a walk through every room by the doors' waypoints, along the navigation mesh
    void Roam(World& world, const Player& player);
    void RoamDrive(Player& player, float dt, float& wantYaw, float& wantPitch);
    void RoamSetUp(World& world, const Player& player);
    std::shared_ptr<NavMesh> m_RoNav; // its own, at the player's size: a scene without dummies builds none
    std::vector<glm::vec3> m_RoPoints, m_RoPath;
    glm::vec3 m_RoFrom{0.0f}, m_RoPrev{0.0f};
    int m_RoLeg = 0, m_RoCorner = 0, m_RoOk = 0, m_RoTotal = 0, m_RoFalls = 0, m_RoStairs = 0, m_RoStairsOk = 0;
    int m_RoUnreachable = 0, m_RoOffMesh = 0, m_RoWorstAt = -1;
    float m_RoWait = 0.0f, m_RoClock = 0.0f, m_RoBudget = 0.0f, m_RoStuck = 0.0f, m_RoStuckSum = 0.0f, m_RoWorstStuck = 0.0f;
    float m_RoLow = 0.0f;
    bool m_RoReady = false;
    // After the passes: a door shut on the player standing in it, for the first few doors (m_DShut: which; -1 not yet).
    int m_DShut = -1, m_DShutOk = 0, m_DShutPen = 0;
    float m_DShutClock = 0.0f;
    bool m_DShutPlaced = false;
    std::vector<DoorSpot> m_DDoors;
    DoorRun m_DRun;
    int m_DDoor = 0, m_DPass = 0, m_DOk = 0, m_DTotal = 0, m_DPen = 0;
    float m_DDt = 0.0f, m_DWait = 0.0f, m_DWorst = 1e9f, m_DWorstStuck = 0.0f;
    bool m_DReady = false, m_DDone = false, m_DTrace = false;
    std::string m_DWorstAt;
    std::vector<std::string> m_DFailedThrough;
    // decals
    int m_KStep = 0, m_KShot = 0;
    int m_KStressFrame = 0; // TARTARUS_DECAL_STRESS: frames of rounds and blood fired so far
    float m_KClock = 0.0f;
    bool m_KMidChecked = false;
    int m_KPlacedStage = 0;
    // ragdoll
    int m_RCase = -1, m_RStage = 0, m_RMoment = 0, m_RFrame = 0;
    float m_RAt = 0.0f, m_RKilledAt = 0.0f, m_RLastDeath = 0.0f, m_RYaw = 0.0f;
    bool m_RFreeze = false;
    std::string m_RName;
    float m_RWorstSelf = 0.0f, m_RWorstGround = 0.0f, m_RWorstJoint = 0.0f;
    std::string m_RWorstSelfAt, m_RWorstGroundAt, m_RWorstJointAt;
    float m_RCaseSelf = 0.0f, m_RCaseGround = 0.0f, m_RCaseJoint = 0.0f;
    std::string m_RCaseGroundAt, m_RCaseJointAt;
    float m_RGroundNow = 0.0f; // this frame's deepest part into the ground
    int m_RLookAway = 0;           // the persistence stage: 0 looking at the bodies, 1 turned away
    float m_RLookAwayAt = 0.0f;
    float m_RViewYaw = -90.0f;     // the player's view (Drive)
    // blood
    int m_BSurface = 0, m_BImpactsBefore = 0, m_BHolesBefore = 0;
    int m_BStep = 0, m_BCase = 0, m_BShot = 0, m_BSpawnBefore = 0, m_BGearBefore = 0, m_BPuffsBefore = 0, m_BHeadshotsBefore = 0;
    bool m_BFired = false, m_BChecked = false, m_BCamSet = false, m_BOverview = false, m_BCloseUp = false;
    float m_BAt = 0.0f, m_BFiredAt = 0.0f;
    std::vector<std::string> m_BLine;
    // flame
    bool m_FlameSwitched = false;
    glm::vec2 m_RevLook{0.0f}; // the revolver scenario's look-around: yaw, pitch (degrees) on top of the default view
    int m_RevAmmoAtReload1 = -1, m_RevAmmoAfterReload1 = -1, m_RevAmmoAtEnd = -1, m_RevShots = -1;
    struct RevolverRun;
    std::shared_ptr<RevolverRun> m_Rev;
    void RevolverDrive(Player& player, FirstPersonPresentation& weapon, float dt);
    void RevolverAfter(const Player& player);
    // feet
    std::string m_FProbe;
    int m_FStage = 0, m_FMeasured = 0;
    float m_FFlat = 0.0f;
    // deaths
    int m_DStep = 0;
    float m_DAt = 0.0f;
    std::vector<std::string> m_DUsed;
    std::string m_DNpc[2];
    std::string m_DCamFor; // the dummy the Scene view was placed for
    int m_DCase = 0;
    float m_DWorstPop = 0.0f;
    int m_DDeaths = 0, m_DRagdolls = 0;
    glm::vec3 m_DCorpseBefore[11]{};
    glm::vec3 m_DBoneBefore[4]{};
    bool m_DCorpseAsleep = false;
    std::string m_Scenario;
    float m_Time = 0.0f;
    float m_Duration = 45.0f;
    bool m_Done = false;
    int m_Checks = 0, m_Failures = 0;
    std::string m_Shot;
    int m_ShotIndex = 0;
    glm::vec3 m_CamPos{0.0f};
    float m_CamYaw = 0.0f, m_CamPitch = 0.0f;
    bool m_HaveCam = false;
    void LookAt(const glm::vec3& target); // the Scene view at m_CamPos, turned to `target` (m_CamYaw / m_CamPitch, m_HaveCam)
    std::string m_RecordDir, m_TraceDir;
    bool m_Firing = false;
    float m_FireHold = 0.0f;
    bool m_Reload = false;
    std::string m_Target;
    int m_PlayerShots = 0, m_LastAmmo = -1;
};
