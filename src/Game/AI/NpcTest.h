#pragma once
#include <cstdint>
#include <functional>
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
class SpiderSystem;
class SpiderDamage;
class BloodFx;
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
//           two are wounded - one by the leg (drops the gun, collapses on its belly, then is ordered to crawl 4 m off to its
//           side, turning as it goes: watched from its side and measured every frame - distance, the planted hands' and knees'
//           slide, the pelvis's pops, the turn rate, the dragged leg straight - then dies on the next hit), one by the chest
//           (drops the gun, stands hurt, walks a few steps, drops and lies there till it bleeds out: the states' order and
//           the body flat at the end) - ("Unit down")
//   tactics the player pins a soldier behind low cover with bursts over its head (blind fire), the squad bounds under
//           covering fire, then the player steps up to a soldier and is struck with its rifle butt
//   reload  the reloads and the regrip: an AK and a Remington soldier aimed, at the hip, empty, and idle; the butt
//           against the shoulder and the hands logged through each (as --stock-probe logs the player's body), the AK shot
//   flame   the player's muzzle flames: the AK's and the Remington's (second slot), shots from the hip
//           then on the sights (record it: NPC_TEST_RECORD, every frame)
//   feet    foot IK: a soldier's feet measured over the ground on the flat, then across and up the Arena's ramp
//   blood   the volumetric blood (scenes/BloodTest.json): a wound, a chest kill against a wall, a head kill and a
//           corpse shot, each watched from the side in shots through the spray's flight
//   ragdoll the deaths themselves (scenes/RagdollTest.json): a dozen soldiers, bare and dressed, standing, walking, jogging,
//           sprinting and strafing, at a ledge, a wall, a crate, on stairs and over another body, each killed by a round from
//           a set side; every death is frozen at moments through the fall and at rest and shot from four sides (front, side,
//           high three-quarter, ground level), and checked every frame: no part of a body inside another (self-collision),
//           none inside the ground, no joint past its range, the gun out of the hand and lying on the floor clear of the body,
//           the body asleep in time; then the corpses and guns stay while looked at and go once the view turns away
//   aptfight the squad as fighters in Apartment Hallway (scenes/Apartment Hallway.json): 301's two dummies spawn as soldiers
//           with AKs and real brains; the player (untouchable) holds the corridor outside 301's front door, then shoots
//           back - a heavy round to stagger one, a thigh to put the other down crawling, then his own rifle - and last
//           steps into 301's entry, by the arch. Measured every frame: awareness, fire and hits on the player, nobody stuck,
//           nobody through a wall or doorframe (capsule, a ray along each step, the navmesh, the gun's muzzle), the doorways,
//           the reloads, no pose pops or NaNs, the deaths and corpses in the small rooms, and the frame cost
//   decals  (scenes/Apartment Hallway.json) rounds and blood put on a corridor wall and floor at fixed spots, shot from
//           fixed Scene-view cameras: the before/after of bullet holes and blood moving to the clustered decals
//           (NpcTest_Decals.cpp, docs/DECALS.md)
//   doors   (scenes/Apartment Hallway.json, no squad) every door opened and the player walked through each one five ways -
//           straight, grazing the hinge jamb, grazing the latch jamb, strafing, sprinting on a diagonal - each pass measured:
//           through in time, the speed kept within half a metre of the wall, time stuck, frames inside anything
//           (NpcTest_Doors.cpp)
//   pose    the weapon hold, close up: an AK and a Remington soldier, the AI frozen, put through aim level /
//           up / down / to the side, at the hip, crouched, strafing, reloading and sprinting; four views of each
//           and the gun's / elbows' / hands' clearances (NpcBody::MeasureHold) logged and checked
class NpcTest {
public:
    explicit NpcTest(const std::string& scenario);

    // Before Player::Update: the move keys (ScriptedMove), the view, the trigger.
    void Drive(Player& player, FirstPersonPresentation& weapon, NpcDirector& npcs, PlayerVitals& vitals, float dt);
    // After everyone's late pose: checks and the log.
    void After(World& world, NpcDirector& npcs, const PlayerVitals& vitals, const Player& player);
    // On Play, before the squad starts: the Apartment Hallway scenarios bring their own two soldiers (301's living
    // room) and squad settings, which the scene itself no longer carries.
    void AddSceneSquad(World& world, AssetLibrary& assets) const;
    bool Done() const { return m_Done; }
    int Checks() const { return m_Checks; }
    int Failures() const { return m_Failures; }
    // Non-empty on frames to capture: the file stem.
    const std::string& ShotName() const { return m_Shot; }
    // NPC_TEST_RECORD=<dir>: every other frame of both views goes there as JPEGs (for a video).
    const std::string& RecordDir() const { return m_RecordDir; }
    int RecordStep() const { return m_Scenario == "flame" ? 1 : 2; } // every frame for the muzzle flame
    // Where to put the Scene view (false = leave it).
    bool SceneCamera(glm::vec3& pos, float& yaw, float& pitch) const;
    // The player's trigger this frame (the host feeds it to the weapon).
    bool Firing() const { return m_Firing; }
    bool Aiming() const { return !m_Target.empty(); }   // sights up (rounds go where the view looks)
    bool WantsReload() const { return m_Reload; }
    bool WantsAiOverlay() const { return m_Scenario == "sandbox"; } // the AI overlay in the sandbox shots
    bool RunsWithoutSquad() const { return m_Scenario == "decals" || m_Scenario == "doors" || m_Scenario == "roam" || m_Scenario == "spider" || m_Scenario == "spiderdeath" || m_Scenario == "spiderhunt" || m_Scenario == "spiderplay" || m_Scenario == "spiderdoor"; } // After() even in a scene with no squad
    bool WantsCleanViews() const { return m_Scenario == "blood" || m_Scenario == "ragdoll" || m_Scenario == "decals" || m_Scenario == "doors" || m_Scenario == "roam" || m_Scenario == "spider" || m_Scenario == "spiderdeath" || m_Scenario == "spiderhunt" || m_Scenario == "spiderplay" || m_Scenario == "spiderdoor"; } // no collider / physics-debug overlays
    // Time stands still this frame (the host steps the game by zero): the ragdoll scenario's multi-angle shots of one moment.
    bool Frozen() const { return m_RFreeze; }
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
    // An NPC's can: a character stood at `feet` facing `yawDeg` (0 = +Z) sprays `piece` through
    // SprayCanAction::ForCharacter (from its hand, along its facing).
    std::function<bool(const std::string& piece, const glm::vec3& feet, float yawDeg, float seconds)> SprayNpc;
    // --npc-test spider: a spider put in the scene at `pos` facing `yawDeg` (its root entity; 0xFFFFFFFF when it won't
    // build), and the system running it (NpcTest_Spider.cpp).
    std::function<std::uint32_t(const glm::vec3& pos, float yawDeg)> SpawnSpider;
    SpiderSystem* Spiders = nullptr;
    // --npc-test spiderhunt: the same, but a hunter (its own AI) rather than a scripted spider.
    std::function<std::uint32_t(const glm::vec3& pos, float yawDeg)> SpawnHunter;
    // --npc-test spiderdeath: a round of `damage` (x `pellets`) fired from `from` along `dir` down the host's player-round
    // path (what it strikes: a spider's hitboxes take the damage and throw the ichor; anything else, a hole). Returns the
    // entity struck (0xFFFFFFFF: a miss) and, for a spider, whether it was claimed as one.
    std::function<std::uint32_t(const glm::vec3& from, const glm::vec3& dir, float damage, int pellets, bool& spider)> ShootSpider;
    const SpiderDamage* SpiderHurt = nullptr;
    const BloodFx* BloodFxView = nullptr;

private:
    void Check(bool ok, const std::string& what);
    void PrintCosts(const NpcDirector& npcs) const;
    void Pose(World& world, NpcDirector& npcs, float now);
    void Deaths(World& world, NpcDirector& npcs, float now);
    void Feet(NpcDirector& npcs, float now);
    void Reload(World& world, NpcDirector& npcs, float now);
    void Blood(World& world, NpcDirector& npcs, float now);
    void Ragdoll(World& world, NpcDirector& npcs, const Player& player);
    void Escape(World& world, NpcDirector& npcs, float now);
    void AptWounds(World& world, NpcDirector& npcs, float now);
    void AptFight(World& world, NpcDirector& npcs, const PlayerVitals& vitals, const Player& player, float now);
    // deaths: a wounded soldier, frame by frame - its body states as they begin and (with `motion`) how it moves
    struct WoundTrack {
        bool On = false, HavePrev = false;
        float From = 0.0f, Seconds = 0.0f;           // when the motion tracking began (director time), and for how long
        glm::vec3 Start{0.0f}, PrevFeet{0.0f}, PrevPelvis{0.0f};
        float PrevYaw = 0.0f;
        float Path = 0.0f, TopSpeed = 0.0f;          // m along the floor, m/s
        float MaxPelvis = 0.0f, MaxPop = 0.0f;       // the pelvis's height off the feet, its worst one-frame vertical jump (m)
        float MaxOffset = 0.0f, MaxLeap = 0.0f;      // crawling: the pelvis's distance from the feet over the floor, and its worst
                                                     // one-frame move over the floor beyond the feet's own (m)
        float MaxYawRate = 0.0f;                     // deg/s
        // The hands and knees (hand_l, hand_r, calf_l, calf_r) while planted: slide speeds frame by frame, the current and
        // the longest unbroken slide (m).
        glm::vec3 PrevContact[4]{};
        bool PrevPlanted[4]{};
        std::vector<float> PlantedSpeed[4];
        float Slide[4]{}, MaxSlide[4]{};
        float Straight[2]{};                         // prone: each knee's straightness summed over the frames it crawls ...
        int StraightFrames = 0;                      // ... and how many
        float LyingPelvis = -1.0f;                   // the pelvis's highest once InjuredLying has held 0.5 s (-1: never)
        std::vector<std::pair<std::string, float>> States; // each body state as it began (s after the hit)
    };
    void TrackWound(World& world, const Npc& n, WoundTrack& w, float now, float dt, bool motion);
    // spider (NpcTest_Spider.cpp): one spider sent round Apartment 301's living room - floor, wall, ceiling, wall,
    // floor - each leg of the route measured
    struct SpiderLeg {
        std::string Name;
        glm::vec3 Goal{0.0f};
        bool Run = false, Arrived = false, Fell = false;
        float Clock = 0.0f, WorstGap = 0.0f, WorstTip = 0.0f, WorstSlide = 0.0f, SumTip = 0.0f;
        int Frames = 0, StepsAtStart = 0;
    };
    void SpiderRoute(World& world);
    // spider / spiderdeath: the player stood in 301's doorway, looking at what the Scene view is on (m_WatchAt)
    void SpiderWatchDrive(Player& player, float& wantYaw, float& wantPitch);
    glm::vec3 m_WatchAt{0.0f};
    bool m_HaveWatch = false, m_WatchPlaced = false;
    // spiderplay (NpcTest_SpiderPlay.cpp): editor Play - hunters left alone go up unaware; the player comes in and shoots
    void SpiderPlay(World& world);
    void SpiderPlayDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch);
    std::vector<std::uint32_t> m_PRoots;
    float m_PClock = 0.0f, m_PShotAt = -1.0f, m_PFirstAttackAt = -1.0f, m_PRoomShotAt = -1.0f, m_PRoomAttackAt = -1.0f;
    int m_PStage = 0, m_PAwareEarly = 0, m_PUnder = 0, m_PAmmoStart = 0, m_PAmmoSpent = 0, m_PRoomAmmo = 0, m_PRoomSpent = 0;
    int m_PAttacksBefore = -1, m_PReachable = 0, m_PVoid = 0, m_PAbove = 0, m_POutside = 0, m_PBarriers = 0;
    bool m_PReached[3] = {false, false, false};
    bool m_POutSaid[3] = {false, false, false};
    bool m_PPlaced = false, m_PEntered = false, m_PHaveAim = false, m_PPerchedAll = false, m_PNavSame = false, m_PDone = false;
    bool m_PDown[3] = {false, false, false};
    glm::vec3 m_PAim{0.0f};
    // spiderdoor (NpcTest_SpiderDoor.cpp): a hunter behind 301's shut front door pushes it open to reach the player
    void SpiderDoor(World& world);
    void SpiderDoorDrive(Player& player, FirstPersonPresentation& weapon, float& wantYaw, float& wantPitch);
    std::uint32_t m_SDDoor = 0xFFFFFFFFu, m_SDRoot = 0xFFFFFFFFu;
    float m_SDClock = 0.0f, m_SDWidth = 0.0f, m_SDOpenedAt = -1.0f, m_SDReachedAt = -1.0f, m_SDWorstStuck = 0.0f;
    int m_SDStage = 0, m_SDAmmoStart = -1, m_SDAmmoSpent = 0, m_SDSwept = 0, m_SDOutside = 0, m_SDPushes = 0, m_SDAttacks = 0;
    bool m_SDReady = false, m_SDPlaced = false, m_SDShutAtStart = false, m_SDDone = false;
    glm::vec3 m_SDHinge{0.0f}, m_SDToLatch{0.0f}, m_SDSwing{0.0f}, m_SDStand{0.0f};
    std::string m_PNavWhy;
    // spiderhunt (NpcTest_SpiderHunt.cpp): hunters let loose on the player in 301's living room
    void SpiderHunt(World& world);
    void SpiderHuntDrive(Player& player, PlayerVitals& vitals, float& wantYaw, float& wantPitch);
    std::vector<std::uint32_t> m_HRoots;
    float m_HClock = 0.0f, m_HMaxDrift = 0.0f; // m_HMaxDrift: the farthest the hits moved the player in any one second
    bool m_HEverAware = false;
    std::vector<glm::vec2> m_HTrail;           // the player's last second of positions (over the floor)
    int m_HCeilFrames[4] = {0, 0, 0, 0};       // frames each hunter spent up on the ceiling                            // the corner of the room the player walks to next
    int m_HStage = 0, m_HMaxAttackers = 0, m_HFellThrough = 0;
    bool m_HPlaced = false, m_HDone = false, m_HDropShot = false, m_HPounceShot = false, m_HBiteShot = false;
    void SpiderSetUp(World& world);
    // Every spider scenario's jank report and bar (pops, snaps, stutter, clipping, legs, decisions, physics, cost).
    void SpiderJank(const char* test);
    // spiderdeath (NpcTest_SpiderDeath.cpp): spiders shot in 301's living room - a leg, the abdomen, the head; one killed
    // on the ceiling, one knocked off a wall
    struct SpiderDeathRun;
    std::shared_ptr<SpiderDeathRun> m_SD;
    void SpiderDeath(World& world);
    std::uint32_t m_SRoot = 0xFFFFFFFFu;
    std::vector<SpiderLeg> m_SLegs;
    int m_SLeg = 0, m_SWorstRays = 0;
    float m_SClock = 0.0f, m_SSettle = 0.0f;
    bool m_SReady = false, m_SDone = false, m_SShotTilt = false;
    void Decals(World& world, NpcDirector& npcs, float now); // NpcTest_Decals.cpp
    void DecalsDrive(Player& player, float& wantYaw, float& wantPitch);
    // doors (NpcTest_Doors.cpp)
    struct DoorSpot {
        std::string Name;
        glm::vec3 Centre{0.0f};  // the opening's middle, on the floor in the wall's plane
        glm::vec3 Normal{0.0f};  // through the opening (the leaf's +Z)
        glm::vec3 Along{0.0f};   // along the wall, toward the latch side
        float Half = 0.46f;      // half the opening's width (Apartment Hallway's 0.92 m)
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
    std::shared_ptr<NavMesh> m_RoNav; // its own, at the player's size: a scene without a squad builds none
    std::vector<glm::vec3> m_RoPoints, m_RoPath;
    glm::vec3 m_RoStart{0.0f}, m_RoFrom{0.0f}, m_RoPrev{0.0f};
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
    float m_KAt = 0.0f, m_KClock = 0.0f;
    bool m_KMidChecked = false;
    int m_KPlacedStage = 0;
    // ragdoll
    int m_RCase = -1, m_RStage = 0, m_RMoment = 0, m_RFrame = 0, m_RFrames = 0;
    float m_RAt = 0.0f, m_RKilledAt = 0.0f, m_RLastDeath = 0.0f, m_RYaw = 0.0f;
    bool m_RFreeze = false, m_RKilled = false;
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
    // reload
    unsigned m_Frame = 0, m_Reloads = 0, m_Fidgets = 0;
    // flame
    bool m_FlameSwitched = false;
    // feet
    std::string m_FProbe;
    int m_FStage = 0, m_FMeasured = 0;
    float m_FFlat = 0.0f;
    // tactics
    std::string m_TVictim;
    int m_TBlindShots = 0, m_TMeleeShots = 0;
    float m_TShotAt = -1e9f, m_TNextCrack = 0.0f;
    // escape (Apartment Hallway), the unarmed dummy in 301's living room shot in the thigh: first the player stands by the
    // arch (the way out behind him: the dummy crawls away, deeper in), then walks off to the window end - and the dummy has
    // to turn and crawl out through the arch and the front door into the corridor, without stopping
    std::string m_EName;
    float m_EShotAt = -1.0f, m_ECrawlFrom = -1.0f, m_EOutOfRoom = -1.0f, m_EInHall = -1.0f, m_EStill = 0.0f, m_EPrevNow = 0.0f;
    float m_EPath = 0.0f, m_EStillRun = 0.0f, m_ELongestStill = 0.0f, m_EAwayPath = 0.0f;
    static constexpr float kEMoveAt = 14.0f; // s after the shot the player walks off to the window
    glm::vec3 m_EPrevFeet{0.0f};
    bool m_EDoorsOpen = false, m_EDone = false;
    float m_EAtShutDoor = 0.0f;    // escapeshut: seconds he lay still by the shut front door ...
    float m_EThroughShut = -1e9f;  // ... and the furthest past it any place he crawled for lay (z; none: -1e9)
    std::vector<std::pair<std::string, float>> m_EStates;
    // aptwounds (Apartment Hallway, 301's living room, the player by the arch): every wound and death in a tight room, the
    // bodies kept out of the walls - an arm's flinch, a gut shot's stagger and collapse, a leg shot's fall and crawl past that
    // corpse, the crawler finished off flat, both dummies back again
    int m_WStep = 0;
    float m_WAt = 0.0f, m_WPrevNow = 0.0f;
    std::string m_WA, m_WB;          // the naked one with the AK (the gut shot), the unarmed one (the leg shot)
    float m_WWorstWall = 0.0f;       // the deepest any bone or ragdoll part went into a wall (m) ...
    std::string m_WWorstWallAt;      // ... which, when
    float m_WCorpseNear = 1e9f;      // the crawler's pelvis nearest the gut-shot corpse's (m)
    float m_WCrawlPath = 0.0f, m_WLift = 0.0f, m_WGrounded = -1.0f;
    bool m_WLegFromFront = false; // the leg shot came from in front of him, as he stood
    glm::vec3 m_WPrevFeet{0.0f};
    std::vector<std::pair<std::string, float>> m_WStates;
    // aptfight: per soldier (by name) what the last frame saw, and the run's findings
    struct AptTrack {
        std::string Name;
        bool Have = false, HaveRel = false, Dead = false, Wounded = false;
        glm::vec3 PrevFeet{0.0f}, PrevRel[5]{};
        float PrevYaw = 0.0f; // the body's facing last frame (degrees), for a pop's report ...
        std::string PrevAnim; // ... its animator (NpcBody::AnimDebug) ...
        std::string PrevGun;  // ... and its gun's tuck and camera
        float StuckFrom = -1.0f;                  // asked to move and not getting anywhere since then ...
        glm::vec3 StuckAt{0.0f};                  // ... from here
        float DoorStill = 0.0f;                   // standing in a doorway's opening this long
        int Room = -1;                            // the room it was last wholly in (kAptRooms)
        int ReloadAmmo = -1;                      // a reload began with this many rounds ...
        float ReloadFrom = -1.0f;                 // ... then
        float ReloadedAt = -1.0f;                 // the last reload finished then ...
        int ShotsAtReload = 0;                    // ... with this many rounds fired in all
        float GunInWall = 0.0f;                   // seconds its muzzle was through a wall
        bool GunIn = false, CapsuleIn = false, OffNav = false, Reloading = false; // last frame's (each counted once as it starts)
        float Path = 0.0f;
    };
    std::vector<AptTrack> m_ATrack;
    AptTrack& ATrack(const std::string& name);
    bool m_ADoorsOpen = false, m_AFightOn = false, m_ADoorPaths = false;
    std::string m_AStaggered, m_ACrawler, m_ACrawlerSpared;
    float m_AStaggerAt = -1.0f, m_ACrawlAt = -1.0f, m_ACrawlStartDist = 0.0f, m_ACrawlBestDist = 0.0f;
    bool m_AStaggerSeen = false;
    int m_AStaggerShotsBefore = -1, m_AStaggerShotsAfter = -1; // the staggered one's rounds when hit, and after it recovered
    int m_AOtherShotsAtCrawl = -1, m_AOtherShotsAfterCrawl = 0;  // the other soldier's rounds while the crawler crawls
    std::string m_AOther;
    int m_AStuck = 0, m_AWallSteps = 0, m_ACapsuleInWall = 0, m_AOffNav = 0, m_APops = 0, m_ADoorPasses = 0, m_ADoorLingers = 0;
    int m_AReloads = 0, m_AReloadsDone = 0, m_AReloadsBad = 0, m_ARefireAfterReload = 0, m_AHitsOnPlayer = 0, m_AIndicators = 0;
    int m_AGunInWallEvents = 0, m_ADeaths = 0, m_ACorpseInWall = 0, m_ADummiesCulled = 0;
    float m_AWorstOffNav = 0.0f, m_AWorstPop = 0.0f, m_AWorstDoorStill = 0.0f, m_AWorstDeathPop = 0.0f, m_AWorstGunInWall = 0.0f;
    float m_AWorstReload = 0.0f;
    std::string m_AWorstPopAt;
    float m_AYoungest = 1e9f;                    // the youngest hit indicator's age last frame
    int m_ACrawlsOnBack = 0;                      // leg-shot soldiers who landed on their backs (they lie there: another is shot)
    int m_AFindingsLogged = 0;                    // capped: the first of each kind are printed
    std::vector<std::string> m_ACorpsesChecked;
    // deaths
    int m_DStep = 0;
    float m_DAt = 0.0f;
    std::vector<std::string> m_DUsed;
    std::string m_DNpc[2];
    std::string m_DCamFor; // the soldier the Scene view was placed for
    int m_DCase = 0;
    float m_DWorstPop = 0.0f;
    float m_DPrevNow = 0.0f;
    // the wounded (step 5): the crawl ordered at the end of the collapse (< 0: not yet), the side view's subject
    // (0 the crawler, 1 the dying one), the dying one's shot waiting for the view and the crawl shots taken
    float m_DCrawlAt = -1.0f, m_DCamSince = 0.0f;
    int m_DCamOn = 0, m_DCrawlShots = 0;
    bool m_DCrawlDone = false;
    float m_DProneKillAt = -1.0f; // the crawler finished off on the floor (deaths' clock), for how its body comes to rest
    float m_DProneWorstLift = 0.0f;
    std::string m_DWantShot;
    std::vector<std::string> m_DDyingShots;
    WoundTrack m_DCrawl, m_DDying;
    int m_DDeaths = 0, m_DRagdolls = 0;
    glm::vec3 m_DCorpseBefore[11]{};
    glm::vec3 m_DBoneBefore[4]{};
    bool m_DCorpseAsleep = false, m_DKneelChecked = false, m_DProneChecked = false, m_DDropShot = false;
    // The leg-shot one's wound fall: down checked, the handover's jumps checked, and (landed on his back) killed where he lies.
    bool m_DDownChecked = false, m_DHandoverChecked = false, m_DFaceUpKilled = false;
    const void* m_DFallRagdoll = nullptr; // the ragdoll he fell as (a death on his back must reuse it)
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
    std::string m_Room = "301";      // aptwounds: the room it runs in (NPC_TEST_ROOM) ...
    glm::vec3 m_RoomShift{0.0f};      // ... as the offset from 301's living room (the dummies, the player and his shots)
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
