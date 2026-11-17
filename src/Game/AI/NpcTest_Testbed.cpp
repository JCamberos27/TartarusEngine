// --npc-test testbed: project/scenes/AnimTestbed.json (tools/scenes/build_anim_testbed.py), the animation overhaul's own
// scene. It measures only what the overhaul fixes, so the owner can re-run it after every phase:
//   A  the lane: walk, stop, jog, stop - the procedural bob reaching the screen (the camera off the body's eye), the gait
//      phase never stepping back, the gait loop's clips travelling at the capsule's speed (within 5%), the planted feet's
//      drift (the foot slide meter, logged), the eye's frame-to-frame jitter (logged)
//   A, B armed: the player's hands on the rig's (< 1 cm) and the world twin's on the world gun's grip (< 2 cm)
//   B  the cover wall's edge, aimed at: the corner peek moves the camera out sideways and both hands stay on the gun
//   C  the normal and the steep stairs up, the 15 degree ramp down: the gait phase, the legs' reach, the eye's overshoot over
//      each step edge (logged)
//   D  holster, draw, holster, draw: the body's hands on the rig's through the arms' IK fade
//   E  the standing dummy sent off 180 degrees behind it: the start turn chosen, the heading reached in time, its turn rate
//   F  the standing dummy killed by a chest shot while idle (the ragdoll's inherited part velocities), then the respawned one
//      shot in the thigh (its planted feet kept through the flinch)
// Per-check PASS / FAIL lines; a check whose value needs an accessor that isn't there yet prints SKIP (not counted) and names
// it. With NPC_TEST_RECORD set, trace.csv there: every frame's numbers behind the checks (NPC_TEST_TRACE=<dir>: the trace
// alone, no frames - much faster). TESTBED_PARTS=<letters> (e.g. C, AC) runs only those parts: the other parts' legs and
// checks are skipped, the dummies' (E, F) too unless named.
#include "NpcTest.h"
#include "Core/Env.h"
#include "BodyDebugDraw.h"
#include "Components.h"
#include "FirstPersonBodyContract.h"
#include "FirstPersonBodyMotion.h"
#include "FirstPersonPresentation.h"
#include "FirstPersonWeaponGameplay.h"
#include "IK.h"
#include "NpcDirector.h"
#include "Npc/NpcRagdoll.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {
// --- values read from the body, the ragdoll and the dummy's animator ---
bool BodyEyeOf(const BodyDebug::Snapshot& s, glm::vec3& out) { out = s.BodyEye; return true; }
bool LegReachOf(const BodyDebug::Snapshot& s, float& l, float& r) { l = s.LegReach[0]; r = s.LegReach[1]; return true; }
// At Ragdoll::Start: the most any part's inherited velocity is off the body's.
bool SpreadOf(const NpcRagdoll& r, float& out) { out = r.StartLimbSpeed(); return true; }

// --- the player's legs (Drive) ---
enum class TbPart : unsigned char { A, B, C, D, M, Watch };
struct TbLeg {
    const char* Name;
    float Seconds;
    TbPart Of;
    glm::vec2 Move = glm::vec2(0.0f); // x right, y forward
    bool Jog = false;
    bool Place = false;               // put there (feet) facing Yaw at the leg's start
    glm::vec3 At = glm::vec3(0.0f);
    float Yaw = 90.0f;                // the view (degrees; 90 = +Z, down the lane)
    float Pitch = -3.0f;
    bool Aim = false;
    int Equip = -1;                   // 0 holster, 1 draw, at the leg's start (-1: leave it)
    bool Crouch = false;              // M (mmplayer): crouched through the leg
    float ViewTurn = 0.0f;            // M: the view turns this many degrees a second through the leg (from Yaw)
    float JumpAt = -1.0f;             // M: a jump pressed this many seconds into the leg (-1: none)
    float SweepDeg = 0.0f;            // M: the view sweeps this many degrees either side of Yaw (a player looking about) ...
    float SweepHz = 0.0f;             // ... this many times a second
    bool Moving() const { return Move.x != 0.0f || Move.y != 0.0f; }
};
// Part M (--npc-test mmplayer only): motion matching's own legs, behind the lane's start (z < 0: open ground).
TbLeg TbM(const char* name, float seconds, glm::vec2 move, bool sprint, bool crouch, float yaw, float viewTurn = 0.0f, float jumpAt = -1.0f,
          bool place = false, glm::vec3 at = glm::vec3(0.0f)) {
    TbLeg l{name, seconds, TbPart::M, move, sprint, place, at, yaw};
    l.Crouch = crouch;
    l.ViewTurn = viewTurn;
    l.JumpAt = jumpAt;
    return l;
}
TbLeg TbMSweep(const char* name, float seconds, glm::vec2 move, bool sprint, float sweepDeg, float sweepHz) {
    TbLeg l = TbM(name, seconds, move, sprint, false, 90.0f);
    l.SweepDeg = sweepDeg;
    l.SweepHz = sweepHz;
    return l;
}
TbLeg TbMPitch(const char* name, float seconds, glm::vec2 move, bool sprint, float pitch) {
    TbLeg l = TbM(name, seconds, move, sprint, false, 90.0f);
    l.Pitch = pitch;
    return l;
}
const glm::vec3 kMStart(0.0f, 0.05f, -24.0f);
// Walk is the body's Run Speed (3.26 m/s), the jog its Sprint Speed (4.72).
const TbLeg kTb[] = {
    {"A settle", 1.5f, TbPart::A, {0, 0}, false, true, {0.0f, 0.05f, 0.0f}},
    {"A walk", 4.0f, TbPart::A, {0, 1}},
    {"A stop after walk", 2.0f, TbPart::A},
    {"A jog", 3.5f, TbPart::A, {0, 1}, true},
    {"A stop after jog", 2.0f, TbPart::A},
    // 0.2 m in from the wall's -X edge (x 3.5), 0.65 m short of its face (z 23.85): cover ahead, the edge within the lean.
    {"B settle at cover", 1.5f, TbPart::B, {0, 0}, false, true, {3.7f, 0.05f, 23.2f}, 90.0f, 0.0f},
    {"B aim (corner peek)", 3.0f, TbPart::B, {0, 0}, false, false, {}, 90.0f, 0.0f, true},
    {"B release", 1.5f, TbPart::B, {0, 0}, false, false, {}, 90.0f, 0.0f},
    {"C settle at normal stairs", 1.5f, TbPart::C, {0, 0}, false, true, {7.0f, 0.05f, 1.5f}},
    {"C climb normal stairs", 4.0f, TbPart::C, {0, 1}},
    {"C stop on normal platform", 1.0f, TbPart::C},
    {"C settle at steep stairs", 1.5f, TbPart::C, {0, 0}, false, true, {12.0f, 0.05f, 1.5f}},
    {"C climb steep stairs", 4.5f, TbPart::C, {0, 1}},
    {"C stop on steep platform", 1.0f, TbPart::C},
    {"C settle on ramp top", 1.5f, TbPart::C, {0, 0}, false, true, {-7.0f, 1.6f, 12.0f}, 270.0f},
    {"C walk down 15deg ramp", 2.6f, TbPart::C, {0, 1}, false, false, {}, 270.0f},
    {"C stop after ramp", 1.0f, TbPart::C, {0, 0}, false, false, {}, 270.0f},
    {"D settle", 1.0f, TbPart::D, {0, 0}, false, true, {0.0f, 0.05f, 2.0f}},
    {"D holster", 2.0f, TbPart::D, {0, 0}, false, false, {}, 90.0f, -3.0f, false, 0},
    {"D draw", 2.0f, TbPart::D, {0, 0}, false, false, {}, 90.0f, -3.0f, false, 1},
    {"D holster again", 2.0f, TbPart::D, {0, 0}, false, false, {}, 90.0f, -3.0f, false, 0},
    {"D draw again", 2.5f, TbPart::D, {0, 0}, false, false, {}, 90.0f, -3.0f, false, 1},
    // M (mmplayer): strafes and backwards, the crouch, turning on the spot, the run (straight, through a turn, strafing),
    // jumps standing and running. Down +Z from kMStart (yaw 90).
    TbM("M settle", 1.5f, {0, 0}, false, false, 90.0f, 0.0f, -1.0f, true, kMStart),
    TbM("M strafe right (walk)", 2.5f, {1, 0}, false, false, 90.0f),
    TbM("M strafe left (walk)", 2.5f, {-1, 0}, false, false, 90.0f),
    TbM("M walk back", 2.0f, {0, -1}, false, false, 90.0f),
    TbM("M stop after walk back", 1.0f, {0, 0}, false, false, 90.0f),
    TbM("M crouch walk", 2.5f, {0, 1}, false, true, 90.0f),
    TbM("M crouch strafe right", 2.0f, {1, 0}, false, true, 90.0f),
    TbM("M crouch idle", 1.5f, {0, 0}, false, true, 90.0f),
    TbM("M stand up", 1.0f, {0, 0}, false, false, 90.0f),
    TbM("M turn in place", 2.0f, {0, 0}, false, false, 90.0f, 90.0f),
    TbM("M turn settle", 1.5f, {0, 0}, false, false, 270.0f),
    TbM("M settle for run", 1.0f, {0, 0}, false, false, 90.0f, 0.0f, -1.0f, true, kMStart),
    TbM("M run", 3.0f, {0, 1}, true, false, 90.0f),
    TbM("M run turning", 3.0f, {0, 1}, true, false, 90.0f, -60.0f),
    TbM("M stop after run", 1.5f, {0, 0}, false, false, -90.0f),
    TbM("M run strafe left", 2.0f, {-1, 0}, true, false, -90.0f),
    TbM("M stop after strafe", 1.0f, {0, 0}, false, false, -90.0f),
    TbM("M settle for jumps", 1.0f, {0, 0}, false, false, 90.0f, 0.0f, -1.0f, true, kMStart),
    TbM("M jump standing", 2.0f, {0, 0}, false, false, 90.0f, 0.0f, 0.3f),
    TbM("M run and jump", 3.0f, {0, 1}, true, false, 90.0f, 0.0f, 1.5f),
    TbM("M stop after jump", 1.5f, {0, 0}, false, false, 90.0f),
    // Looking about on the move (the owner's play test, 2026-11-17: the pose lunged and jerked whenever the view turned
    // while walking or running): the view sweeps 50 degrees either side, once every 2 s.
    TbM("M settle for look-about", 1.0f, {0, 0}, false, false, 90.0f, 0.0f, -1.0f, true, kMStart),
    TbMSweep("M walk looking about", 5.0f, {0, 1}, false, 50.0f, 0.5f),
    TbM("M stop after walk look", 1.0f, {0, 0}, false, false, 90.0f),
    TbMSweep("M run looking about", 4.0f, {0, 1}, true, 50.0f, 0.5f),
    TbM("M stop after run look", 1.5f, {0, 0}, false, false, 90.0f),
    // Looking down (the owner's play test, 2026-11-17: looking down, the body folded over at the waist): standing, walking
    // and running with the view 60 degrees down.
    TbMPitch("M look down standing", 2.5f, {0, 0}, false, -60.0f),
    TbMPitch("M look straight down", 2.0f, {0, 0}, false, -85.0f),
    TbMPitch("M walk looking down", 3.0f, {0, 1}, false, -60.0f),
    TbMPitch("M run looking down", 3.0f, {0, 1}, true, -60.0f),
    TbM("M stop after look down", 1.5f, {0, 0}, false, false, 90.0f),
    // The dummies' part: the player stands 6 m off the standing dummy (its hitboxes on), looking at it.
    {"EF watch", 1e6f, TbPart::Watch, {0, 0}, false, true, {-1.5f, 0.05f, 9.0f}, 125.0f, -8.0f},
};
constexpr int kTbCount = (int)(sizeof(kTb) / sizeof(kTb[0]));
constexpr int kWatchLeg = kTbCount - 1;
static_assert(kTbCount <= 64, "per-leg arrays");
constexpr float kNpcBudget = 40.0f; // seconds the dummies' part may take

const glm::vec3 kStandingSpawn(-5.0f, 0.0f, 14.0f);

// TESTBED_PARTS: the parts that run (all when unset).
bool TbPartOn(char part) {
    static const std::string parts = Env::IsSet(Env::Var::TestbedParts) ? Env::Str(Env::Var::TestbedParts) : std::string();
    if (part == 'M' && parts.empty()) return false; // motion matching's own legs: mmplayer (TESTBED_PARTS ACM) only
    return parts.empty() || parts.find(part) != std::string::npos || parts.find((char)(part - 'A' + 'a')) != std::string::npos;
}
bool TbDummiesOn() { return TbPartOn('E') || TbPartOn('F'); }
// A leg runs when its part does; the watch leg always does (the player's checks are made as it starts).
bool TbLegOn(int leg) { return leg == kWatchLeg || TbPartOn(kTb[leg].Of == TbPart::M ? 'M' : (char)('A' + (int)kTb[leg].Of)); }

int TbLegAt(float t, float& into) {
    int leg = 0;
    while (leg < kWatchLeg && (!TbLegOn(leg) || t >= kTb[leg].Seconds)) {
        if (TbLegOn(leg)) t -= kTb[leg].Seconds;
        ++leg;
    }
    into = t;
    return leg;
}
bool TbHas(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
float WrapDeg(float d) { return std::remainder(d, 360.0f); }
} // namespace

struct NpcTest::TestbedRun {
    // Drive -> After, this frame
    float Clock = 0.0f;
    int Leg = 0, PrevLeg = -1;
    float Into = 0.0f;
    const FirstPersonPresentation* Weapon = nullptr;
    int Frame = 0;
    std::FILE* Trace = nullptr;
    ~TestbedRun() { if (Trace) std::fclose(Trace); }

    // Per leg (the player's)
    struct LegStats {
        int Frames = 0;
        int GaitFrames = 0, GaitBack = 0;
        float GaitWorstBack = 0.0f;
        bool HavePrevGait = false;
        float PrevGait = 0.0f;
        // vertical, steady part of a moving leg (from 1 s in): the camera off the body's eye, the camera off the capsule's eye,
        // the procedural pose's camera offset
        float ResidMin = 1e9f, ResidMax = -1e9f, CamMin = 1e9f, CamMax = -1e9f, ProcMin = 1e9f, ProcMax = -1e9f;
        int ResidFrames = 0;
        float CamRelMax = -1e9f;      // the camera off the capsule's eye, the whole leg
        float LegReachMax = 0.0f, ClampPullMax = 0.0f; // reach foot IK asked for; m the reach clamp moved a foot
        bool HaveReach = false;
        // step edges (C): the capsule's feet jumping up a step, the eye's highest over the flat walk's in the 0.4 s after
        int StepEdges = 0, Overshoots = 0;
        float WorstOvershoot = -1e9f, EdgeUntil = -1.0f, EdgePeak = -1e9f, PrevFootY = 0.0f;
        bool HavePrevFoot = false, EdgeOpen = false;
        void CloseEdge() {
            if (!EdgeOpen) return;
            EdgeOpen = false;
            WorstOvershoot = std::max(WorstOvershoot, EdgePeak);
            if (EdgePeak > 0.01f) ++Overshoots;
        }
        float FootYStart = 0.0f, FootYEnd = 0.0f;
        // Armed (the arms' weight on): the player's hands off the rig's, the other pieces' (the arm solve copied onto them),
        // the world twin's off the world gun's grip - the worst of the leg.
        int HandFrames = 0, TwinFrames = 0;
        float HandGapMax = 0.0f, PieceGapMax = 0.0f, TwinGapMax = 0.0f;
        // The eye's jitter, steady part of a move: each frame's second difference (x[n] - 2 x[n-1] + x[n-2]) of the camera off
        // the body's eye (the procedural kick) and of the body's eye off the capsule's (the eye's own anchor and filters), m.
        glm::vec3 KickPrev[2]{}, EyePrev[2]{};
        int JitterSeen = 0, JitterFrames = 0;
        double KickJ2 = 0.0, EyeJ2 = 0.0;
        float KickJMax = 0.0f, EyeJMax = 0.0f;
        // The steady gait loop (A, from 1 s in, in the Locomotion state): the clips' root travel (BodyDebug RootSpeed: the
        // driver's RootMotion.DeltaPosition, world metres, over its frame's dt) and the capsule's speed, summed per frame.
        double LoopClip = 0.0, LoopCapsule = 0.0;
        int LoopFrames = 0;
    };
    LegStats Legs[64];
    // mmplayer (part M and every leg under motion matching), per leg: steady frames (from 0.5 s in, grounded), the matched
    // frames' labels, the play rate and the raw (unwarped) clip speed against the capsule's, the body's root off the capsule.
    struct MmStats {
        int Frames = 0, Steady = 0, StrafeFlag = 0, Sideways = 0, CrouchFlag = 0, GaitAtLeast[4] = {0, 0, 0, 0};
        std::vector<float> RawRatio, Speed;
        float RateMin = 9.0f, RateMax = 0.0f, YawRateMax = 0.0f, RootOffsetMax = 0.0f;
        int AirFrames = 0, AirLocked = 0, AirFlagged = 0;
        int Jumps = 0;                       // the matcher's jumps in the leg
        int Searches0 = -1, ByInput0 = 0, ByEnd0 = 0, Searches = 0, ByInput = 0, ByEnd = 0; // the schedule over the leg
        std::vector<float> PoseStep;         // the output pose's worst bone turn per frame (deg), the steady part
        float ViewOffMax = 0.0f;             // the body's heading off the view, at most (deg), the steady part
        std::vector<float> Bend, ClipBend, Pitch; // the torso's bend as drawn and as posed, the view's pitch (deg), steady
        std::vector<float> ArmAim, Arm;            // the upper arms off straight down: as the aim left them, as drawn (deg)
        bool EndAirborne = false, EndLocked = false;
        float EndYawOff = 0.0f;
        float FootHMin[2] = {9.0f, 9.0f}, FootHMax[2] = {-9.0f, -9.0f};
    };
    MmStats Mm[64];
    int MmCrouchWrong = 0, MmAirborneGround = 0, MmFrames = 0, MmPosed = 0, MmBigSteps = 0;
    bool Jumped[64] = {};
    float FlatCamMax = 0.0f; // A walk's highest camera off the capsule's eye (steady)
    bool HaveFlat = false;
    bool ResidMissing = false, ReachMissing = false;
    // B
    float LeanBase = 0.0f;
    int LeanBaseN = 0;
    float LeanPeak = 0.0f;
    float LeanGap[2] = {0.0f, 0.0f};
    int LeanGapFrames = 0;
    // D
    float FadeGap[2] = {0.0f, 0.0f};
    int FadeFrames = 0;
    float FadeWeightMin = 1.0f;
    std::string LastWeaponState;
    bool PlayerChecked = false;

    // The dummies' part (After): E then F
    int Stage = 0;
    float StageAt = 0.0f, PrevNow = -1.0f;
    std::string Standing;           // the standing dummy's name
    int StandingIndex = -1;
    // E
    float TurnFrom = 0.0f, TravelYaw = 0.0f, PrevYaw = 0.0f, ReachedAt = -1.0f, MaxYawRate = 0.0f;
    glm::vec3 TurnTarget{0.0f};
    float StartTurnSteps = 0.0f;    // the StartTurn parameter while a start-turn state played (45 degree steps, + left)
    bool SawStartTurn = false;
    std::string LastNpcState, NpcStates;
    double NpcLoopClip = 0.0, NpcLoopCapsule = 0.0; // its gait loop's clip travel and capsule speed (m/s, summed per frame; logged)
    int NpcLoopFrames = 0;
    // F
    std::string Killed;
    int KilledIndex = -1;
    std::string Respawned;
    int RespawnedIndex = -1;
    glm::vec3 FeetAt[2]{};
    float IdleDrift = 0.0f, FlinchDrift = 0.0f;
    bool SpreadMissing = false;
    int Skipped = 0;
};

float NpcTest::TestbedSeconds() {
    float t = 0.0f;
    for (int i = 0; i < kWatchLeg; ++i)
        if (TbLegOn(i)) t += kTb[i].Seconds;
    return t + (TbDummiesOn() ? kNpcBudget : 1.0f);
}

void NpcTest::TestbedDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch) {
    if (!m_Tb) m_Tb = std::make_shared<TestbedRun>();
    TestbedRun& R = *m_Tb;
    R.Weapon = &weapon;
    R.Clock += dt;
    float into = 0.0f;
    const int leg = TbLegAt(R.Clock, into);
    const TbLeg& L = kTb[leg];
    const bool entered = leg != R.PrevLeg;
    IK::SetFootSlideMeterOn(true);
    if (entered && R.PrevLeg >= 0 && kTb[R.PrevLeg].Moving()) SlideLeg(kTb[R.PrevLeg].Name);
    if (entered && L.Moving()) IK::FootSlideMeterFor("player").TakeTotals(); // the leg's own plants only
    if (entered) {
        std::printf("[Testbed] %6.2f s  %s\n", R.Clock, L.Name);
        if (L.Place) {
            PhysicsWorld::SetCharacterFootPosition(&L.At.x);
            player.Velocity = glm::vec3(0.0f);
            player.Cam.Position = L.At + glm::vec3(0.0f, player.EyeHeight, 0.0f);
            player.Cam.Yaw = L.Yaw; // placed facing it (the settle lets the body turn round)
        }
        if (L.Equip == 0) weapon.SetEquipped(false);
        if (L.Equip == 1) weapon.SetEquipped(true);
        if (m_Scenario == "mmplayer") weapon.SetEquipped(false); // Phase 4 is verified unarmed
    }
    R.PrevLeg = leg;
    R.Leg = leg;
    R.Into = into;
    player.ScriptMove = L.Move;
    player.ScriptSprint = L.Jog;
    player.ScriptCrouch = L.Crouch;
    if (L.JumpAt >= 0.0f && into >= L.JumpAt && !R.Jumped[leg]) {
        player.ScriptJump = true;
        R.Jumped[leg] = true;
    }
    if (L.Aim) m_Target = "testbed"; // sights up: aiming at cover peeks round it
    else m_Target.clear();
    wantYaw = player.Cam.Yaw = L.Yaw + L.ViewTurn * into + L.SweepDeg * std::sin(2.0f * 3.14159265f * L.SweepHz * into);
    wantPitch = L.Pitch;
}

void NpcTest::Testbed(World& world, NpcDirector& npcs, const Player& player, float now) {
    if (!m_Tb) return; // (Drive hasn't run yet)
    TestbedRun& R = *m_Tb;
    npcs.NoLod = true;
    npcs.FootIKEverywhere = true;
    const float dt = R.PrevNow >= 0.0f ? std::max(0.0f, now - R.PrevNow) : 0.0f;
    R.PrevNow = now;
    const int leg = R.Leg;
    const TbLeg& L = kTb[leg];
    const BodyDebug::Snapshot& bd = BodyDebug::Info();

    // Every dummy stands still unless this scenario moves it (the roaming one is out of the way, and out of the numbers).
    auto find = [&](const std::string& name) -> Npc* {
        for (const auto& up : npcs.Npcs())
            if (up && up->Name == name) return up.get();
        return nullptr;
    };
    for (const auto& up : npcs.Npcs())
        if (up && !up->Dead) { up->Roam = false; if (up->Name != R.Standing || R.Stage != 2) up->WantMove = false; }

    // --- the player: this frame's numbers ---
    float foot[3] = {0.0f, 0.0f, 0.0f}, radius = 0.3f, cyl = 0.6f;
    PhysicsWorld::GetCharacterCapsule(foot, &radius, &cyl);
    const glm::vec3 capsuleEye(foot[0], foot[1] + player.EyeHeight, foot[2]);
    const glm::vec3 cam = player.Cam.Position;
    glm::vec3 flatFront = player.Cam.Front();
    flatFront.y = 0.0f;
    flatFront = glm::length(flatFront) > 1e-5f ? glm::normalize(flatFront) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 flatRight = glm::normalize(glm::cross(flatFront, glm::vec3(0.0f, 1.0f, 0.0f)));
    const float camRelY = cam.y - capsuleEye.y;
    const float camSide = glm::dot(cam - capsuleEye, flatRight);
    glm::vec3 bodyEye(0.0f);
    const bool haveBodyEye = BodyEyeOf(bd, bodyEye);
    const float residY = haveBodyEye ? cam.y - bodyEye.y : 0.0f;
    float reachL = 0.0f, reachR = 0.0f;
    const bool haveReach = LegReachOf(bd, reachL, reachR);
    const FirstPersonPresentation* weapon = R.Weapon;
    const bool gaitOk = weapon && weapon->HaveGaitPhase();
    const float gait = weapon ? weapon->GaitPhaseValue() : 0.0f;
    const float procY = weapon ? weapon->ProceduralPose().CameraOffset.y : 0.0f;
    const std::string weaponState = weapon ? weapon->CurrentState() : std::string();

    if (L.Of != TbPart::Watch) {
        TestbedRun::LegStats& s = R.Legs[leg];
        if (s.Frames == 0) s.FootYStart = foot[1];
        s.FootYEnd = foot[1];
        ++s.Frames;
        if (m_Scenario == "mmplayer") MmPlayerFrame(world, player, leg, foot);
        // The gait phase never steps back (wrapped; a hair of noise allowed).
        if (L.Moving() && gaitOk) {
            if (s.HavePrevGait) {
                const float d = std::remainder(gait - s.PrevGait, 1.0f);
                ++s.GaitFrames;
                if (d < -0.002f) { ++s.GaitBack; s.GaitWorstBack = std::min(s.GaitWorstBack, d); }
            }
            s.PrevGait = gait;
            s.HavePrevGait = true;
        } else {
            s.HavePrevGait = false;
        }
        // The steady part of a move: the bob.
        if (L.Moving() && R.Into >= 1.0f) {
            s.CamMin = std::min(s.CamMin, camRelY);
            s.CamMax = std::max(s.CamMax, camRelY);
            s.ProcMin = std::min(s.ProcMin, procY);
            s.ProcMax = std::max(s.ProcMax, procY);
            if (haveBodyEye) { s.ResidMin = std::min(s.ResidMin, residY); s.ResidMax = std::max(s.ResidMax, residY); ++s.ResidFrames; }
            if (haveBodyEye) {
                const glm::vec3 kick = cam - bodyEye, eye = bodyEye - capsuleEye;
                if (s.JitterSeen >= 2) {
                    const float k2 = glm::length(kick - 2.0f * s.KickPrev[0] + s.KickPrev[1]);
                    const float e2 = glm::length(eye - 2.0f * s.EyePrev[0] + s.EyePrev[1]);
                    s.KickJ2 += (double)k2 * k2;
                    s.EyeJ2 += (double)e2 * e2;
                    s.KickJMax = std::max(s.KickJMax, k2);
                    s.EyeJMax = std::max(s.EyeJMax, e2);
                    ++s.JitterFrames;
                }
                s.KickPrev[1] = s.KickPrev[0];
                s.KickPrev[0] = kick;
                s.EyePrev[1] = s.EyePrev[0];
                s.EyePrev[0] = eye;
                ++s.JitterSeen;
            }
            // The clips' travel against the capsule's, in the loop only (not a start still playing out or a fade). Under motion
            // matching: the matched frame's root speed at the play rate it is warped to (BodyDebug RootSpeed).
            if (L.Of == TbPart::A && bd.Valid && (bd.AnimatorState == FPBody::kStateLocomotion ||
                                                  (bd.AnimatorState == FPBody::kStateMotionMatching && player.Grounded))) {
                s.LoopClip += bd.RootSpeed;
                s.LoopCapsule += glm::length(glm::vec2(player.Velocity.x, player.Velocity.z));
                ++s.LoopFrames;
            }
        }
        s.CamRelMax = std::max(s.CamRelMax, camRelY);
        // Armed through the walk, the jog and the corner peek: both bodies' hands on their guns.
        if (((L.Of == TbPart::A && L.Moving()) || L.Aim) && bd.ArmsWeight > 0.99f) {
            ++s.HandFrames;
            s.HandGapMax = std::max({s.HandGapMax, bd.HandGap[0], bd.HandGap[1]});
            s.PieceGapMax = std::max(s.PieceGapMax, bd.PieceHandGap);
            if (bd.TwinHandGap[0] >= 0.0f || bd.TwinHandGap[1] >= 0.0f) {
                ++s.TwinFrames;
                s.TwinGapMax = std::max({s.TwinGapMax, bd.TwinHandGap[0], bd.TwinHandGap[1]});
            }
        }
        if (haveReach && (L.Of == TbPart::C || L.Of == TbPart::A) && L.Moving()) {
            s.HaveReach = true;
            s.LegReachMax = std::max({s.LegReachMax, reachL, reachR});
            s.ClampPullMax = std::max({s.ClampPullMax, bd.ClampPull[0], bd.ClampPull[1]});
        }
        // Step edges: the capsule's feet up a step in a frame; the eye's highest over the flat walk's for the next 0.4 s.
        if (L.Of == TbPart::C && L.Moving()) {
            if (s.HavePrevFoot && foot[1] - s.PrevFootY > 0.03f) {
                s.CloseEdge();
                ++s.StepEdges;
                s.EdgeUntil = R.Into + 0.4f;
                s.EdgePeak = -1e9f;
                s.EdgeOpen = R.HaveFlat;
            }
            if (s.EdgeOpen) s.EdgePeak = std::max(s.EdgePeak, camRelY - R.FlatCamMax);
            if (s.EdgeOpen && R.Into > s.EdgeUntil) s.CloseEdge();
            s.PrevFootY = foot[1];
            s.HavePrevFoot = true;
        }
        // The flat walk's highest eye, the reference for the stairs.
        if (std::string(L.Name) == "A walk" && R.Into >= 1.0f) { R.FlatCamMax = s.CamMax; R.HaveFlat = true; }
        // B: the lean, sideways off the capsule's eye; the hands while leaned out.
        if (std::string(L.Name) == "B settle at cover" && R.Into >= L.Seconds - 0.5f) {
            R.LeanBase = (R.LeanBase * (float)R.LeanBaseN + camSide) / (float)(R.LeanBaseN + 1);
            ++R.LeanBaseN;
        }
        if (L.Aim) {
            const float lean = std::fabs(camSide - R.LeanBase);
            R.LeanPeak = std::max(R.LeanPeak, lean);
            if (lean > 0.10f) {
                ++R.LeanGapFrames;
                R.LeanGap[0] = std::max(R.LeanGap[0], bd.HandGap[0]);
                R.LeanGap[1] = std::max(R.LeanGap[1], bd.HandGap[1]);
            }
        }
        // D: the hands through the arms' IK fade (the weight between off and on, or a Draw / Holster clip with the arms on).
        if (L.Of == TbPart::D && L.Equip >= 0) {
            const float w = bd.ArmsWeight;
            const bool fading = w > 0.02f && w < 0.98f;
            const bool clip = w > 0.02f && (TbHas(weaponState, "Draw") || TbHas(weaponState, "Holster"));
            R.FadeWeightMin = std::min(R.FadeWeightMin, w);
            if (fading || clip) {
                ++R.FadeFrames;
                R.FadeGap[0] = std::max(R.FadeGap[0], bd.HandGap[0]);
                R.FadeGap[1] = std::max(R.FadeGap[1], bd.HandGap[1]);
            }
            if (weaponState != R.LastWeaponState) {
                std::printf("[Testbed] %6.2f s  weapon %s -> %s (arms weight %.2f)\n", R.Clock, R.LastWeaponState.c_str(), weaponState.c_str(), w);
                R.LastWeaponState = weaponState;
            }
        }
        if (!haveBodyEye) R.ResidMissing = true;
        if (!haveReach) R.ReachMissing = true;
    }

    // --- the player's checks, once its legs are through ---
    if (L.Of == TbPart::Watch && !R.PlayerChecked) {
        R.PlayerChecked = true;
        auto legIndex = [](const char* name) {
            for (int i = 0; i < kTbCount; ++i)
                if (std::string(kTb[i].Name) == name) return i;
            return 0;
        };
        auto skip = [&](const std::string& what, const char* accessor) {
            ++R.Skipped;
            std::printf("[NpcTest] SKIP %s (needs %s)\n", what.c_str(), accessor);
        };
        const bool partA = TbPartOn('A'), partB = TbPartOn('B'), partC = TbPartOn('C'), partD = TbPartOn('D');
        if (partA) {
        // A: the bob on the screen.
        for (const char* name : {"A walk", "A jog"}) {
            const TestbedRun::LegStats& s = R.Legs[legIndex(name)];
            std::printf("[Testbed] %-6s bob (half peak-to-peak, steady): camera off the capsule's eye %.1f mm, procedural camera offset %.1f mm, "
                        "camera off the body's eye %s\n", name, (s.CamMax - s.CamMin) * 500.0f, (s.ProcMax - s.ProcMin) * 500.0f,
                        s.ResidFrames ? (std::to_string((s.ResidMax - s.ResidMin) * 500.0f) + " mm").c_str() : "n/a");
        }
        {
            const TestbedRun::LegStats& s = R.Legs[legIndex("A walk")];
            if (s.ResidFrames > 0) {
                const float amp = (s.ResidMax - s.ResidMin) * 0.5f;
                Check(amp > 0.005f, "A: the procedural bob reaches the screen - walking, the camera bobs " + std::to_string(amp * 1000.0f) +
                                        " mm off the body's eye (> 5 mm)");
            } else {
                skip("A: the procedural bob reaches the screen (camera off the body's eye > 5 mm at walk)", "BodyDebug::Snapshot::BodyEye");
            }
        }
        }
        auto gaitCheck = [&](const char* name) {
            const TestbedRun::LegStats& s = R.Legs[legIndex(name)];
            std::printf("[Testbed] %-26s gait phase: %d frames read, %d back, worst %.4f cycles\n", name, s.GaitFrames, s.GaitBack, s.GaitWorstBack);
            Check(s.GaitFrames > 20 && s.GaitBack == 0, std::string(name) + ": the gait phase never steps back (" + std::to_string(s.GaitBack) +
                                                            " of " + std::to_string(s.GaitFrames) + " frames, worst " + std::to_string(s.GaitWorstBack) + ")");
        };
        if (partA) {
        gaitCheck("A walk");
        gaitCheck("A jog");
        // A: the gait loop's clips travel at the capsule's speed - PlayRate alone matching them (no closed-loop trim).
        for (const char* name : {"A walk", "A jog"}) {
            const TestbedRun::LegStats& s = R.Legs[legIndex(name)];
            const double ratio = s.LoopCapsule > 1e-3 ? s.LoopClip / s.LoopCapsule : 0.0;
            std::printf("[Testbed] %-6s loop travel: clips %.3f m/s, capsule %.3f m/s (ratio %.3f, %d frames)\n", name,
                        s.LoopFrames ? s.LoopClip / s.LoopFrames : 0.0, s.LoopFrames ? s.LoopCapsule / s.LoopFrames : 0.0, ratio, s.LoopFrames);
            Check(s.LoopFrames > 30 && std::fabs(ratio - 1.0) <= 0.05,
                  std::string(name) + ": the gait loop's clips travel at the capsule's speed (ratio " + std::to_string(ratio) + " over " +
                      std::to_string(s.LoopFrames) + " steady frames, within 5%)");
        }
        // A: the eye's jitter at a steady walk and jog - logged until a baseline sets its threshold.
        for (const char* name : {"A walk", "A jog"}) {
            const TestbedRun::LegStats& s = R.Legs[legIndex(name)];
            const double n = s.JitterFrames > 0 ? (double)s.JitterFrames : 1.0;
            std::printf("[Testbed] %-6s eye jitter (frame-to-frame second difference, steady, %d frames): camera off the body's eye rms %.3f mm, "
                        "max %.3f mm; body's eye off the capsule's rms %.3f mm, max %.3f mm (logged, not checked)\n", name, s.JitterFrames,
                        std::sqrt(s.KickJ2 / n) * 1000.0, s.KickJMax * 1000.0f, std::sqrt(s.EyeJ2 / n) * 1000.0, s.EyeJMax * 1000.0f);
        }
        }
        // A, B: armed, the player's hands on the rig's (< 1 cm) and the world twin's on the world gun's grip (< 2 cm).
        for (const char* name : {"A walk", "A jog", "B aim (corner peek)"}) {
            if (!TbPartOn(name[0])) continue;
            const TestbedRun::LegStats& s = R.Legs[legIndex(name)];
            std::printf("[Testbed] %-20s hands (%d armed frames): player %.2f mm, other pieces %.2f mm, world twin %s at worst\n", name,
                        s.HandFrames, s.HandGapMax * 1000.0f, s.PieceGapMax * 1000.0f,
                        s.TwinFrames ? (std::to_string(s.TwinGapMax * 1000.0f) + " mm").c_str() : "n/a");
            if (s.HandFrames == 0) {
                skip(std::string(name) + ": the hands on the gun (no armed frames)", "the weapon drawn in this leg");
                continue;
            }
            Check(s.HandGapMax < 0.01f, std::string(name) + ": the player's hands stay on the rig's (" + std::to_string(s.HandGapMax * 1000.0f) +
                                            " mm at worst, < 10 mm)");
            if (s.TwinFrames > 0)
                Check(s.TwinGapMax < 0.02f, std::string(name) + ": the world twin's hands stay on the world gun (" +
                                                std::to_string(s.TwinGapMax * 1000.0f) + " mm at worst, < 20 mm)");
            else
                skip(std::string(name) + ": the world twin's hands on the world gun", "split poses (Weapon Arms' world twins)");
        }
        // B: the corner peek.
        if (partB) {
        std::printf("[Testbed] B lean: camera %.3f m sideways at most (base %.3f m); hands while out (%d frames) L %.1f mm, R %.1f mm\n", R.LeanPeak,
                    R.LeanBase, R.LeanGapFrames, R.LeanGap[0] * 1000.0f, R.LeanGap[1] * 1000.0f);
        Check(R.LeanPeak > 0.15f, "B: aiming at the cover's edge peeks round it - the camera " + std::to_string(R.LeanPeak) + " m sideways (> 0.15 m)");
        Check(R.LeanGapFrames > 0 && R.LeanGap[0] < 0.01f && R.LeanGap[1] < 0.01f,
              "B: leaned out, both hands stay on the gun (L " + std::to_string(R.LeanGap[0] * 1000.0f) + " mm, R " + std::to_string(R.LeanGap[1] * 1000.0f) +
                  " mm, < 10 mm)");
        }
        // C: stairs and the ramp.
        for (const char* name : {"C climb normal stairs", "C climb steep stairs", "C walk down 15deg ramp"}) {
            if (!partC) break;
            TestbedRun::LegStats& s = R.Legs[legIndex(name)];
            s.CloseEdge();
            gaitCheck(name);
            std::printf("[Testbed] %-26s feet %.2f -> %.2f m; %d step edges, eye over the flat walk's highest by %.1f cm at worst, %d edge(s) over 1 cm "
                        "(logged, not checked)\n", name, s.FootYStart, s.FootYEnd, s.StepEdges, s.StepEdges ? s.WorstOvershoot * 100.0f : 0.0f, s.Overshoots);
        }
        // The legs: foot IK never asks for much more than the leg has (the lock lets go by reach), so the reach clamp
        // never has to drag a foot visibly off where it should be.
        for (const char* name : {"A walk", "A jog", "C climb normal stairs", "C climb steep stairs", "C walk down 15deg ramp"}) {
            if (!TbPartOn(name[0])) continue;
            const TestbedRun::LegStats& s = R.Legs[legIndex(name)];
            if (!s.HaveReach) { skip(std::string(name) + ": the reach clamp moves no foot more than 1 cm", "BodyDebug::Snapshot::ClampPull"); continue; }
            std::printf("[Testbed] %-26s legs: reach asked %.3f at most, clamp pulled a foot %.1f mm at most\n", name, s.LegReachMax, s.ClampPullMax * 1000.0f);
            Check(s.ClampPullMax < 0.01f, std::string(name) + ": the reach clamp moves no foot more than 1 cm (" + std::to_string(s.ClampPullMax * 1000.0f) + " mm)");
        }
        if (partC)
        Check(R.Legs[legIndex("C stop on normal platform")].FootYEnd > 1.3f && R.Legs[legIndex("C stop on steep platform")].FootYEnd > 2.3f,
              "C setup: the player went up both flights (normal " + std::to_string(R.Legs[legIndex("C stop on normal platform")].FootYEnd) + " m, steep " +
                  std::to_string(R.Legs[legIndex("C stop on steep platform")].FootYEnd) + " m)");
        // D: holster and draw.
        if (partD)
        std::printf("[Testbed] D fade: %d frames (arms weight down to %.2f); hands off the rig's L %.2f mm, R %.2f mm at worst\n", R.FadeFrames,
                    R.FadeWeightMin, R.FadeGap[0] * 1000.0f, R.FadeGap[1] * 1000.0f);
        // (Logged only: holstered, the body's arms let go of the rig on purpose. The rig's own hands-on-gun through its IK fade is
        // the unit test "IK rig at part weight keeps the hand on the gun".)
        if (m_Scenario == "mmplayer") MmPlayerChecks();
        R.Stage = TbDummiesOn() ? 1 : 7; // (the dummies' part left out: done)
        R.StageAt = now;
    }

    // --- the dummies (E, F) ---
    Npc* n = R.Standing.empty() ? nullptr : find(R.Standing);
    auto rootId = [](const Npc& d) { return (unsigned)entt::to_integral(d.Root); };
    auto shoot = [&](Npc& d, int part, float damage) {
        FirstPersonWeaponGameplay w;
        w.Damage = damage;
        w.HeadMultiplier = 2.0f;
        w.LimbMultiplier = 0.75f;
        w.FalloffStart = 1000.0f;
        w.FalloffEnd = 2000.0f;
        w.FalloffMin = 1.0f;
        const float yaw = d.Body.Yaw();
        const glm::vec3 dir(std::sin(yaw), 0.0f, std::cos(yaw)); // from behind, along its facing (deaths' rounds)
        const glm::vec3 c = d.Hitboxes->Centre(d.Body, part);
        bool killed = false, head = false;
        npcs.OnPlayerHit(world, rootId(d), c, c - dir * 3.0f, dir, w, &killed, &head);
        return killed;
    };
    auto feetDrift = [&](const Npc& d) {
        glm::vec3 f[2];
        if (!d.Body.FootWorld(f)) return -1.0f;
        return std::max(glm::length(f[0] - R.FeetAt[0]), glm::length(f[1] - R.FeetAt[1]));
    };
    auto standingNear = [&](const std::string& notThis) -> Npc* {
        Npc* best = nullptr;
        float bestD = 2.5f;
        for (const auto& up : npcs.Npcs()) {
            if (!up || up->Dead || up->Name == notThis || !up->Hitboxes || !up->Hitboxes->Active()) continue;
            const float d = glm::length(glm::vec2(up->Feet.x - kStandingSpawn.x, up->Feet.z - kStandingSpawn.z));
            if (d < bestD) { bestD = d; best = up.get(); }
        }
        return best;
    };
    const float inStage = now - R.StageAt;
    auto next = [&](int stage) { R.Stage = stage; R.StageAt = now; };
    switch (R.Stage) {
    case 1: { // the standing dummy, stood a while, the player watching
        if (inStage < 1.5f) break;
        Npc* s = standingNear("");
        if (!s) { if (inStage > 10.0f) { Check(false, "E setup: the standing dummy, with hitboxes"); next(7); } break; }
        R.Standing = s->Name;
        R.StandingIndex = s->Index;
        R.TurnFrom = glm::degrees(s->Body.Yaw());
        const glm::vec3 fwd(std::sin(s->Body.Yaw()), 0.0f, std::cos(s->Body.Yaw()));
        R.TurnTarget = s->Feet - fwd * 3.0f; // straight behind it (from the spawn facing -Z: 3 m toward the ledge, 1 m short of it)
        R.TravelYaw = glm::degrees(NpcYawOf(-fwd, 0.0f));
        R.PrevYaw = R.TurnFrom;
        std::printf("[Testbed] E: %s at (%.2f, %.2f) facing %.0f deg, sent to (%.2f, %.2f) - travel %.0f deg\n", s->Name.c_str(), s->Feet.x, s->Feet.z,
                    R.TurnFrom, R.TurnTarget.x, R.TurnTarget.z, R.TravelYaw);
        next(2);
        break;
    }
    case 2: { // E: off 180 degrees behind it
        if (!n || n->Dead) { Check(false, "E setup: the standing dummy alive through its turn"); next(7); break; }
        const glm::vec3 to(R.TurnTarget.x, n->Feet.y, R.TurnTarget.z);
        const bool there = glm::length(glm::vec2(n->Feet.x - to.x, n->Feet.z - to.z)) < 0.35f;
        if (!there) npcs.ScriptedMove(n->Index, to, Gait::Walk);
        else npcs.EndScriptedMove(n->Index);
        n->LookPoint = n->Feet + glm::vec3(0.0f, 1.6f, 0.0f) + (to - n->Feet) * 2.0f + glm::vec3(0.0f, 0.0f, 0.01f);
        const float yaw = glm::degrees(n->Body.Yaw());
        if (dt > 1e-4f) {
            const float rate = std::fabs(WrapDeg(yaw - R.PrevYaw)) / dt;
            R.MaxYawRate = std::max(R.MaxYawRate, rate);
        }
        R.PrevYaw = yaw;
        if (R.ReachedAt < 0.0f && std::fabs(WrapDeg(yaw - R.TravelYaw)) < 15.0f) R.ReachedAt = inStage;
        if (const auto* ac = world.Registry.try_get<AnimatorControllerComponent>(n->Body.Driver())) {
            if (ac->HasTag(FPBody::kTagStartTurn)) {
                R.SawStartTurn = true;
                const float steps = ac->GetFloat(FPBody::kStartTurn);
                if (std::fabs(steps) > std::fabs(R.StartTurnSteps)) R.StartTurnSteps = steps;
            }
            // Its loop's clip travel against its capsule (the frames with root motion: an LOD hold has none).
            const float rmDt = n->Body.RootMotionDt();
            if (ac->CurrentState() == FPBody::kStateLocomotion && !ac->InTransition && rmDt > 1e-5f) {
                R.NpcLoopClip += glm::length(glm::vec2(ac->RootMotion.DeltaPosition.x, ac->RootMotion.DeltaPosition.z)) / rmDt;
                R.NpcLoopCapsule += glm::length(glm::vec2(n->Velocity.x, n->Velocity.z));
                ++R.NpcLoopFrames;
            }
            if (ac->CurrentState() != R.LastNpcState) {
                std::printf("[Testbed] %6.2f s  E %s -> %s  (yaw %.0f, %.2f m/s)\n", inStage, R.LastNpcState.c_str(), ac->CurrentState().c_str(), yaw,
                            glm::length(glm::vec2(n->Velocity.x, n->Velocity.z)));
                R.LastNpcState = ac->CurrentState();
                R.NpcStates += (R.NpcStates.empty() ? "" : " > ") + R.LastNpcState;
            }
        }
        if ((there && inStage > 1.5f) || inStage > 5.0f) {
            npcs.EndScriptedMove(n->Index);
            std::printf("[Testbed] E: states %s; StartTurn %.2f (%.0f deg); heading reached at %.2f s; body yaw rate %.0f deg/s at most%s\n",
                        R.NpcStates.c_str(), R.StartTurnSteps, R.StartTurnSteps * 45.0f, R.ReachedAt, R.MaxYawRate,
                        "");
            std::printf("[Testbed] E: its gait loop's clips %.3f m/s, capsule %.3f m/s (ratio %.3f, %d frames; logged, not checked)\n",
                        R.NpcLoopFrames ? R.NpcLoopClip / R.NpcLoopFrames : 0.0, R.NpcLoopFrames ? R.NpcLoopCapsule / R.NpcLoopFrames : 0.0,
                        R.NpcLoopCapsule > 1e-3 ? R.NpcLoopClip / R.NpcLoopCapsule : 0.0, R.NpcLoopFrames);
            Check(R.SawStartTurn && std::fabs(R.StartTurnSteps) * 45.0f >= 157.5f,
                  "E: setting off 180 degrees behind it, the start turn chosen covers ~180 degrees (" + std::to_string(R.StartTurnSteps * 45.0f) + ")");
            Check(R.ReachedAt >= 0.0f && R.ReachedAt <= 1.2f,
                  "E: its heading reaches the travel (within 15 degrees) in 1.2 s (" + std::to_string(R.ReachedAt) + " s)");
            // No snap: the turn clips peak near 480 deg/s (8 deg a frame at 60 Hz); a heading snapped by a clamp is a single frame's jump.
            Check(R.MaxYawRate <= 600.0f, "E: the heading turns smoothly, never snapping (" + std::to_string(R.MaxYawRate) + " deg/s at most, <= 600)");
            next(3);
        }
        break;
    }
    case 3: { // F: stood idle again, killed by a round through the chest
        if (!n || n->Dead || !n->Hitboxes || !n->Hitboxes->Active()) { if (inStage > 5.0f) { Check(false, "F setup: the standing dummy to kill"); next(7); } break; }
        if (inStage < 2.0f) break; // its stop played out: idle
        R.Killed = n->Name;
        R.KilledIndex = n->Index;
        const bool killed = shoot(*n, 1, 400.0f);
        Check(killed, "F setup: one round through the chest kills the idle dummy");
        next(4);
        break;
    }
    case 4: { // F: the ragdoll's first velocities
        Npc* k = find(R.Killed);
        if (!k || !k->Ragdoll) { if (inStage > 1.0f) { Check(false, "F setup: a ragdoll took the body"); next(5); } break; }
        float spread = 0.0f;
        if (SpreadOf(*k->Ragdoll, spread))
            Check(spread < 0.3f, "F: the ragdoll's parts start with the idle body's velocity (worst part " + std::to_string(spread) + " m/s off, < 0.3)");
        else {
            ++R.Skipped;
            R.SpreadMissing = true;
            std::printf("[NpcTest] SKIP F: the ragdoll's parts start with the idle body's velocity (< 0.3 m/s) (needs NpcRagdoll::InheritedVelocitySpread)\n");
        }
        next(5);
        break;
    }
    case 5: { // F: the respawned dummy, idle a while; its feet's own drift idling measured first
        Npc* s = find(R.Respawned);
        if (!s) {
            s = standingNear(R.Killed);
            if (!s) { if (inStage > 10.0f) { Check(false, "F setup: the standing dummy respawned"); next(7); } break; }
            R.Respawned = s->Name;
            R.RespawnedIndex = s->Index;
            R.StageAt = now;
            break;
        }
        if (s->Dead) { next(7); break; }
        if (inStage >= 1.5f && R.IdleDrift == 0.0f) { // (once: the baseline)
            s->Body.FootWorld(R.FeetAt);
            R.IdleDrift = -1.0f;
        }
        if (inStage >= 2.5f) {
            if (!s->Hitboxes || !s->Hitboxes->Active()) { if (inStage > 6.0f) { Check(false, "F setup: the respawned dummy's hitboxes"); next(7); } break; }
            R.IdleDrift = feetDrift(*s);
            s->Body.FootWorld(R.FeetAt);
            const bool killed = shoot(*s, 7, 20.0f); // thigh_l, light: no stagger (Heavy Hit Damage 40), not a kill
            Check(!killed && !s->Dead, "F setup: a light round in the thigh doesn't kill");
            next(6);
        }
        break;
    }
    case 6: { // F: the flinch, the planted feet held
        Npc* s = find(R.Respawned);
        if (!s || s->Dead) { next(7); break; }
        R.FlinchDrift = std::max(R.FlinchDrift, feetDrift(*s));
        if (inStage >= 1.0f) {
            std::printf("[Testbed] F: planted feet drift %.1f mm through the flinch (%.1f mm over a second idling before it)\n", R.FlinchDrift * 1000.0f,
                        R.IdleDrift * 1000.0f);
            Check(R.FlinchDrift < 0.01f, "F: the thigh shot's flinch leaves the planted feet where they are (" + std::to_string(R.FlinchDrift * 1000.0f) +
                                             " mm, < 10 mm)");
            next(7);
        }
        break;
    }
    case 7:
        if (!m_Done) {
            m_Done = true;
            std::printf("[Testbed] %d check(s) skipped for accessors not there yet\n", R.Skipped);
        }
        break;
    default:
        break;
    }
    if (now >= m_Duration && !m_Done) {
        m_Done = true;
        Check(false, "testbed: every part ran (stopped at stage " + std::to_string(R.Stage) + ")");
    }

    // --- trace.csv ---
    if (!R.Trace && !TraceDir().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(TraceDir(), ec);
        if (fopen_s(&R.Trace, (std::filesystem::path(TraceDir()) / "trace.csv").string().c_str(), "w") != 0) R.Trace = nullptr;
        if (R.Trace)
            std::fprintf(R.Trace, "frame,t,leg,leg_t,speed,gait_ok,gait,cam_x,cam_y,cam_z,foot_y,cam_rel_y,cam_side,body_eye_ok,cam_off_body_eye_y,"
                                  "proc_cam_off_y,hand_gap_l,hand_gap_r,arms_weight,leg_reach_ok,leg_reach_l,leg_reach_r,weapon_state,"
                                  "npc_stage,npc_yaw,npc_speed,npc_state,piece_hand_gap,twin_hand_gap_l,twin_hand_gap_r,body_eye_x,body_eye_y,body_eye_z,"
                                  "foot_off_l,foot_off_r,step_off,planted_l,planted_r,lock_shift_l,lock_shift_r,reach_free_l,reach_free_r,"
                                  "clamp_pull_l,clamp_pull_r,anim_h_l,anim_h_r,body_yaw,drawn_lx,drawn_ly,drawn_lz,drawn_rx,drawn_ry,drawn_rz\n");
    }
    if (R.Trace) {
        float npcYaw = 0.0f, npcSpeed = 0.0f;
        std::string npcState;
        if (n && !n->Dead) {
            npcYaw = glm::degrees(n->Body.Yaw());
            npcSpeed = glm::length(glm::vec2(n->Velocity.x, n->Velocity.z));
            npcState = n->Body.StateName(world);
        }
        std::fprintf(R.Trace, "%d,%.4f,%d,%.4f,%.3f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.5f,%.5f,%.5f,%.5f,%.3f,%d,%.4f,%.4f,%s,%d,%.2f,%.3f,%s,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,", R.Frame++,
                     now, leg, R.Into, glm::length(glm::vec2(player.Velocity.x, player.Velocity.z)), (int)gaitOk, gait, cam.x, cam.y, cam.z, foot[1], camRelY,
                     camSide, (int)haveBodyEye, residY, procY, bd.HandGap[0], bd.HandGap[1], bd.ArmsWeight, (int)haveReach, reachL, reachR, weaponState.c_str(),
                     R.Stage, npcYaw, npcSpeed, npcState.c_str(), bd.PieceHandGap, bd.TwinHandGap[0], bd.TwinHandGap[1], bodyEye.x, bodyEye.y,
                     bodyEye.z);
        // The foot IK behind the leg checks: each foot's ground off the body's base, the body's off the capsule's, the lock
        // (planted, how far it moves the foot), the reach asked without the lock, the clamp's pull, the clip's foot height.
        std::fprintf(R.Trace, "%.5f,%.5f,%.5f,%d,%d,%.5f,%.5f,%.4f,%.4f,%.5f,%.5f,%.4f,%.4f,%.2f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", bd.FootOffset[0], bd.FootOffset[1], bd.StepOffset,
                     (int)bd.FootPlanted[0], (int)bd.FootPlanted[1], bd.LockShift[0], bd.LockShift[1], bd.ReachFree[0], bd.ReachFree[1],
                     bd.ClampPull[0], bd.ClampPull[1], bd.AnimHeight[0], bd.AnimHeight[1], bd.BodyYawDeg,
                     bd.DrawnFoot[0].x, bd.DrawnFoot[0].y, bd.DrawnFoot[0].z, bd.DrawnFoot[1].x, bd.DrawnFoot[1].y, bd.DrawnFoot[1].z);
    }

    // --- the Scene view: off the player's front quarter on its legs; side on to the standing dummy for E and F ---
    glm::vec3 want, at;
    if (L.Of != TbPart::Watch) {
        const glm::vec3 feet(foot[0], foot[1], foot[2]);
        want = feet + flatRight * 2.6f + flatFront * 2.2f + glm::vec3(0.0f, 1.3f, 0.0f);
        at = feet + glm::vec3(0.0f, 0.9f, 0.0f);
    } else {
        Npc* who = find(R.Stage >= 5 ? R.Respawned : R.Standing);
        const glm::vec3 c = who ? who->Feet : kStandingSpawn;
        want = c + glm::vec3(3.2f, 1.1f, 0.0f);
        at = c + glm::vec3(0.0f, 0.8f, 0.0f);
    }
    m_CamPos = m_HaveCam ? m_CamPos + (want - m_CamPos) * std::min(1.0f, dt * 5.0f) : want;
    if (glm::length(want - m_CamPos) > 6.0f) m_CamPos = want; // a placement: cut, don't fly
    const glm::vec3 look = glm::normalize(at - m_CamPos);
    m_CamYaw = glm::degrees(std::atan2(look.z, look.x));
    m_CamPitch = glm::degrees(std::asin(std::clamp(look.y, -1.0f, 1.0f)));
    m_HaveCam = true;
}

// --- mmplayer: the player's body on motion matching (docs/MOTION_MATCHING_HANDOFF.md, Phase 4) ------------------------
// The testbed's parts A and C with Locomotion Source = Motion Matching, plus part M (strafes, the crouch, turning on the
// spot, the run, jumps), unarmed (TESTBED_PARTS=ACM, TARTARUS_LOCOMOTION_SOURCE=mm: set by the NpcTest constructor). Every
// frame: the matched frame's labels against the filters, the speed warping, the root against the capsule; checked once
// the player's legs are through (MmPlayerChecks).
void NpcTest::MmPlayerFrame(World& world, const Player& player, int leg, const float (&foot)[3]) {
    TestbedRun& R = *m_Tb;
    const MmPlayerDebug::Snapshot& mm = MmPlayerDebug::Info();
    const BodyDebug::Snapshot& bd = BodyDebug::Info();
    TestbedRun::MmStats& q = R.Mm[leg];
    ++q.Frames;
    ++R.MmFrames;
    if (bd.Valid && bd.AnimatorState == FPBody::kStateMotionMatching) ++R.MmPosed;
    // The torso's bend (any locomotion source: TARTARUS_LOCOMOTION_SOURCE=legacy runs the same course on the controller).
    if (R.Into >= 0.5f && player.Grounded && bd.Valid) {
        q.Bend.push_back(bd.TorsoBendDeg);
        q.ClipBend.push_back(bd.ClipBendDeg);
        q.Pitch.push_back(bd.ViewPitchDeg);
        q.ArmAim.push_back(bd.ArmDownAimDeg);
        q.Arm.push_back(bd.ArmDownDeg);
    }
    if (!mm.Valid) return;
    const bool crouchFlag = (mm.Flags & MotionLabel::Crouch) != 0;
    if (!mm.Locked && crouchFlag != mm.Crouch) ++R.MmCrouchWrong;
    if (!mm.Locked && (mm.Flags & MotionLabel::Airborne)) ++R.MmAirborneGround;
    if (mm.Airborne) {
        ++q.AirFrames;
        if (mm.Locked) ++q.AirLocked;
        if (mm.Flags & MotionLabel::Airborne) ++q.AirFlagged;
    }
    q.EndAirborne = mm.Airborne;
    q.EndLocked = mm.Locked;
    q.EndYawOff = std::fabs(bd.ViewOffsetDeg);
    q.YawRateMax = std::max(q.YawRateMax, std::fabs(mm.YawRate));
    if (mm.Jumped) ++q.Jumps;
    if (q.Searches0 < 0) { q.Searches0 = mm.Searches; q.ByInput0 = mm.ForcedByInput; q.ByEnd0 = mm.ForcedByEnd; }
    q.Searches = mm.Searches - q.Searches0;
    q.ByInput = mm.ForcedByInput - q.ByInput0;
    q.ByEnd = mm.ForcedByEnd - q.ByEnd0;
    // The first jumps of each leg, for the log: where to, why (a search forced by the input or by the frame running out).
    if (mm.Jumped && q.Jumps <= 4 && std::string(kTb[leg].Name).find("crouch") != std::string::npos) {
        auto row = [](const float* f) {
            char s[256];
            std::snprintf(s, sizeof s, "feet %.2f %.2f %.2f / %.2f %.2f %.2f  pelvis v %.2f %.2f %.2f  traj %.2f,%.2f %.2f,%.2f %.2f,%.2f  dir %.2f,%.2f %.2f,%.2f %.2f,%.2f",
                          f[0], f[1], f[2], f[3], f[4], f[5], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23], f[24], f[25], f[26]);
            return std::string(s);
        };
        std::printf("[MmPlayer]     query   %s\n[MmPlayer]     matched %s\n", row(mm.Query).c_str(), row(mm.Matched).c_str());
    }
    if (mm.Jumped && q.Jumps <= 4)
        std::printf("[MmPlayer] %6.2f s %-24s jump to %s @ frame %u (flags %02x, gait %d), cost %.2f vs playing %.2f; searches %d, by input %d, "
                    "by end %d\n", R.Clock, kTb[leg].Name, mm.Clip.c_str(), mm.Frame, mm.Flags, mm.Gait, mm.LastCost, mm.PlayingCost, q.Searches,
                    q.ByInput, q.ByEnd);
    // The root clamped to the capsule: the body's root (its entity) on the capsule's feet, horizontally.
    for (auto e : world.Registry.view<FirstPersonBodyComponent>()) {
        const glm::vec3 at(world.ComposeWorldTransform(e)[3]);
        q.RootOffsetMax = std::max(q.RootOffsetMax, std::hypot(at.x - foot[0], at.z - foot[2]));
        break;
    }
    if (R.Into >= 0.5f && player.Grounded && !mm.Airborne) {
        ++q.Steady;
        if (mm.Flags & MotionLabel::Strafe) ++q.StrafeFlag;
        q.PoseStep.push_back(mm.PoseStepDeg);
        if (mm.PoseStepDeg > 30.0f && ++R.MmBigSteps <= 12)
            std::printf("[MmPlayer] %6.2f s %-24s pose step %.1f deg on %s, playing %s @ frame %u (rate %.2f%s)\n", R.Clock, kTb[leg].Name,
                        mm.PoseStepDeg, mm.PoseStepBone.c_str(), mm.Clip.c_str(), mm.Frame, mm.PlayRate, mm.Jumped ? ", a jump" : "");
        q.ViewOffMax = std::max(q.ViewOffMax, std::fabs(bd.ViewOffsetDeg));
        if (mm.MatchedTravel > 0.2f && mm.MatchedTravelDeg > 50.0f) ++q.Sideways;
        if (crouchFlag) ++q.CrouchFlag;
        for (int g = 0; g <= std::min<int>(mm.Gait, 3); ++g) ++q.GaitAtLeast[g];
        if (mm.CapsuleSpeed > 0.3f && mm.AnimSpeed > 0.05f) q.RawRatio.push_back(mm.AnimSpeed / mm.CapsuleSpeed);
        q.Speed.push_back(mm.CapsuleSpeed);
        q.RateMin = std::min(q.RateMin, mm.PlayRate);
        q.RateMax = std::max(q.RateMax, mm.PlayRate);
        for (int s = 0; s < 2; ++s) {
            q.FootHMin[s] = std::min(q.FootHMin[s], bd.AnimHeight[s]);
            q.FootHMax[s] = std::max(q.FootHMax[s], bd.AnimHeight[s]);
        }
    }
}

void NpcTest::MmPlayerChecks() {
    TestbedRun& R = *m_Tb;
    const MmPlayerDebug::Snapshot& mm = MmPlayerDebug::Info();
    auto legIndex = [](const char* name) {
        for (int i = 0; i < kTbCount; ++i)
            if (std::string(kTb[i].Name) == name) return i;
        return 0;
    };
    auto median = [](std::vector<float> v) {
        if (v.empty()) return 0.0f;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    auto mean = [](const std::vector<float>& v) {
        float sum = 0.0f;
        for (float x : v) sum += x;
        return v.empty() ? 0.0f : sum / (float)v.size();
    };
    char b[400];
    std::printf("[MmPlayer] %-26s %6s %6s %7s %7s %13s %9s %9s %5s %5s\n", "leg", "frames", "steady", "strafe%", "crouch%", "gait>=w/j/r %",
                "raw ratio", "rate", "m/s", "air");
    for (int i = 0; i < kWatchLeg; ++i) {
        if (!TbLegOn(i)) continue;
        const TestbedRun::MmStats& q = R.Mm[i];
        const float n = (float)std::max(1, q.Steady);
        std::printf("[MmPlayer] %-26s %6d %6d %7.0f %7.0f %4.0f/%3.0f/%3.0f %9.2f %4.2f-%4.2f %5.2f %5d\n", kTb[i].Name, q.Frames, q.Steady,
                    100.0f * q.StrafeFlag / n, 100.0f * q.CrouchFlag / n, 100.0f * q.GaitAtLeast[1] / n, 100.0f * q.GaitAtLeast[2] / n,
                    100.0f * q.GaitAtLeast[3] / n, median(q.RawRatio), q.Steady ? q.RateMin : 0.0f, q.Steady ? q.RateMax : 0.0f, mean(q.Speed),
                    q.AirFrames);
    }
    Check(R.MmFrames > 0 && R.MmPosed == R.MmFrames, "mmplayer: motion matching poses the body every frame (" + std::to_string(R.MmPosed) + " of " +
                                                         std::to_string(R.MmFrames) + ")");
    const float rate = (float)mm.Searches / std::max(1e-3f, R.Clock);
    std::snprintf(b, sizeof b, "mmplayer: the matcher searched %d times in %.1f s (%.1f per second; none while a jump plays locked), between 7 and 20",
                  mm.Searches, R.Clock, rate);
    Check(rate >= 7.0f && rate <= 20.0f, b);
    std::snprintf(b, sizeof b, "mmplayer: searches avg %.3f ms, max %.3f ms; avg under 1 ms", mm.SearchMsAvg, mm.SearchMsMax);
    Check(mm.SearchMsAvg < 1.0, b);
    Check(R.MmCrouchWrong == 0, "mmplayer: crouch is a label filter - crouched frames only while crouched, standing ones only standing (" +
                                    std::to_string(R.MmCrouchWrong) + " wrong)");
    Check(R.MmAirborneGround == 0, "mmplayer: airborne frames only in the jump segment (" + std::to_string(R.MmAirborneGround) + " outside it)");
    float rootOff = 0.0f;
    for (int i = 0; i < kWatchLeg; ++i) rootOff = std::max(rootOff, R.Mm[i].RootOffsetMax);
    std::snprintf(b, sizeof b, "mmplayer: the root is clamped to the capsule - the body's root at most %.1f mm off the capsule's feet (< 10 mm)",
                  rootOff * 1000.0f);
    Check(rootOff < 0.01f, b);
    // Speed warping: the play rate stays in its limits, and the matched clips need only a little of it (the raw clip speed
    // over the capsule's, median of the steady part, within the warp's limits) on the gaits the table speeds come from.
    float rateMin = 9.0f, rateMax = 0.0f;
    for (int i = 0; i < kWatchLeg; ++i)
        if (R.Mm[i].Steady > 0) {
            rateMin = std::min(rateMin, R.Mm[i].RateMin);
            rateMax = std::max(rateMax, R.Mm[i].RateMax);
        }
    std::snprintf(b, sizeof b, "mmplayer: speed warping replaces LoopPlayRate - the play rate %.2f..%.2f, within 0.8..1.25", rateMin, rateMax);
    Check(rateMin >= 0.79f && rateMax <= 1.26f, b);
    for (const char* name : {"A walk", "A jog", "M crouch walk", "M run", "M strafe right (walk)", "M strafe left (walk)"}) {
        if (!TbPartOn(name[0])) continue;
        const TestbedRun::MmStats& q = R.Mm[legIndex(name)];
        const float r = median(q.RawRatio);
        std::snprintf(b, sizeof b, "%s: the matched clips move at the capsule's pace before warping (median clip / capsule speed %.2f over %zu "
                                   "frames, within 0.8..1.25)", name, r, q.RawRatio.size());
        Check(q.RawRatio.size() > 20 && r >= 0.8f && r <= 1.25f, b);
    }
    // Strafes (and walking backwards) match strafing frames: the facing held while the travel goes sideways - the matched
    // frame's own root 1 s on more than 50 degrees off its facing. (The Strafe label also wants 0.5 m/s, which the pack's
    // crouched strafes dip under between steps: logged, not checked.)
    for (const char* name : {"M strafe right (walk)", "M strafe left (walk)", "M walk back", "M crouch strafe right", "M run strafe left"}) {
        const TestbedRun::MmStats& q = R.Mm[legIndex(name)];
        const float share = q.Steady ? (float)q.Sideways / (float)q.Steady : 0.0f;
        std::snprintf(b, sizeof b, "%s: strafing frames matched - travelling over 50 deg off their facing (%.0f %% of %d steady frames, >= 50 %%; "
                                   "Strafe-labelled %.0f %%)", name, 100.0f * share, q.Steady, q.Steady ? 100.0f * q.StrafeFlag / q.Steady : 0.0f);
        Check(q.Steady > 20 && share >= 0.5f, b);
    }
    // Sprint is the run gait; the run slows through a turn.
    {
        const TestbedRun::MmStats& q = R.Mm[legIndex("M run")];
        const float share = q.Steady ? (float)q.GaitAtLeast[2] / (float)q.Steady : 0.0f;
        std::snprintf(b, sizeof b, "M run: sprinting plays jog-or-faster frames (%.0f %% of %d steady frames, >= 60 %%) at %.2f m/s", 100.0f * share,
                      q.Steady, mean(q.Speed));
        Check(q.Steady > 20 && share >= 0.6f, b);
        const TestbedRun::MmStats& t = R.Mm[legIndex("M run turning")];
        std::snprintf(b, sizeof b, "M run turning: the run slows through a 60 deg/s turn (%.2f m/s against the straight run's %.2f)", mean(t.Speed),
                      mean(q.Speed));
        Check(t.Steady > 20 && mean(t.Speed) < 0.9f * mean(q.Speed), b);
    }
    // Turning on the spot.
    {
        const TestbedRun::MmStats& t = R.Mm[legIndex("M turn in place")];
        const TestbedRun::MmStats& e = R.Mm[legIndex("M turn settle")];
        std::snprintf(b, sizeof b, "M turn in place: the body turns on the spot after the view (up to %.0f deg/s; the database's turn-in-place "
                                   "rate %.0f deg/s feeds MaxYawRate) and ends %.1f deg off the view (< 20)",
                      glm::degrees(t.YawRateMax), glm::degrees(mm.TurnInPlaceRate), e.EndYawOff);
        Check(t.YawRateMax > glm::radians(30.0f) && e.EndYawOff < 20.0f && mm.TurnInPlaceRate > glm::radians(30.0f), b);
    }
    // Looking about on the move: the pose stays as steady as walking straight - the worst bone's frame-to-frame turn
    // (p99) within 1.5x the straight leg's plus 1 degree, and the matcher jumps no more often (1.5x + 1 a second).
    auto pct = [](std::vector<float> v, float p) {
        if (v.empty()) return 0.0f;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, (size_t)std::lround(p * (float)(v.size() - 1)))];
    };
    std::printf("[MmPlayer] %-26s %6s %9s %9s %9s %11s %9s %8s %6s\n", "leg", "jumps/s", "step p50", "step p99", "step max", "view off max",
                "searches", "by input", "by end");
    for (int i = 0; i < kWatchLeg; ++i) {
        if (!TbLegOn(i) || !kTb[i].Moving()) continue;
        const TestbedRun::MmStats& q = R.Mm[i];
        std::printf("[MmPlayer] %-26s %6.1f %9.2f %9.2f %9.2f %11.1f %9d %8d %6d\n", kTb[i].Name, q.Jumps / std::max(0.1f, kTb[i].Seconds),
                    pct(q.PoseStep, 0.5f), pct(q.PoseStep, 0.99f), pct(q.PoseStep, 1.0f), q.ViewOffMax, q.Searches, q.ByInput, q.ByEnd);
    }
    for (const auto& pair : {std::make_pair("M walk looking about", TbPartOn('A') ? "A walk" : "M crouch walk"),
                             std::make_pair("M run looking about", "M run")}) {
        const TestbedRun::MmStats& q = R.Mm[legIndex(pair.first)];
        const TestbedRun::MmStats& ref = R.Mm[legIndex(pair.second)];
        const float p99 = pct(q.PoseStep, 0.99f), refP99 = pct(ref.PoseStep, 0.99f);
        const float jumps = q.Jumps / std::max(0.1f, kTb[legIndex(pair.first)].Seconds);
        const float refJumps = ref.Jumps / std::max(0.1f, kTb[legIndex(pair.second)].Seconds);
        std::snprintf(b, sizeof b, "%s: the pose stays steady while the view sweeps 50 deg either side (worst bone's turn per frame p99 %.2f deg "
                                   "against %s's %.2f: within 1.5x + 1; %.1f jumps a second against its %.1f: within 1.5x + 1)", pair.first, p99, pair.second,
                      refP99, jumps, refJumps);
        Check(!q.PoseStep.empty() && p99 <= 1.5f * refP99 + 1.0f && jumps <= 1.5f * refJumps + 1.0f, b);
    }
    // Looking down: the torso's forward bend as drawn, against the view's pitch and the pose's own (logged; checked below).
    std::printf("[MmPlayer] %-26s %9s %11s %11s %11s %13s %10s\n", "leg", "view pitch", "pose bend", "drawn bend", "drawn max",
                "arms aim/drawn", "arms max");
    for (const char* name : {"A walk", "A jog", "M look down standing", "M look straight down", "M walk looking down", "M run looking down", "M run"}) {
        if (!TbPartOn(name[0])) continue;
        const TestbedRun::MmStats& q = R.Mm[legIndex(name)];
        std::printf("[MmPlayer] %-26s %9.1f %11.1f %11.1f %11.1f %6.1f/%6.1f %10.1f\n", name, median(q.Pitch), median(q.ClipBend), median(q.Bend),
                    pct(q.Bend, 1.0f), median(q.ArmAim), median(q.Arm), pct(q.Arm, 1.0f));
    }
    // Looking 60 degrees down the torso bends no further than on the locomotion controller (measured 2026-11-17: legacy
    // median 16-27, worst 27 degrees; motion matching before Spine Stability took it, running 46 and 61).
    for (const char* name : {"M look down standing", "M walk looking down", "M run looking down"}) {
        const TestbedRun::MmStats& q = R.Mm[legIndex(name)];
        std::snprintf(b, sizeof b, "%s: the body doesn't fold at the waist - the torso bends %.1f deg (median; worst %.1f) with the view 60 deg "
                                   "down, <= 30 (worst <= 40)", name, median(q.Bend), pct(q.Bend, 1.0f));
        Check(q.Bend.size() > 20 && median(q.Bend) <= 30.0f && pct(q.Bend, 1.0f) <= 40.0f, b);
    }
    // ... and the arms hang (unarmed) rather than swinging back with the chest: looking down standing, the upper arms as far
    // off straight down as looking level (A walk's median at most, plus 10 degrees).
    for (const char* name : {"M look down standing", "M look straight down"}) {
        const TestbedRun::MmStats& q = R.Mm[legIndex(name)];
        const TestbedRun::MmStats& level = R.Mm[legIndex(TbPartOn('A') ? "A walk" : "M crouch walk")];
        std::snprintf(b, sizeof b, "%s: the arms hang - the upper arms %.1f deg off straight down (the spine's aim left them at "
                                   "%.1f; looking level %.1f): within 10 of level", name, median(q.Arm), median(q.ArmAim), median(level.Arm));
        Check(q.Arm.size() > 20 && median(q.Arm) <= median(level.Arm) + 10.0f, b);
    }
    // Jumps: the airborne segment plays locked, then matching takes over.
    for (const char* name : {"M jump standing", "M run and jump"}) {
        const TestbedRun::MmStats& q = R.Mm[legIndex(name)];
        std::snprintf(b, sizeof b, "%s: the airborne segment plays label-locked (%d airborne frames, %d locked, %d on airborne-labelled frames) "
                                   "and matching resumes after the landing (%s)", name, q.AirFrames, q.AirLocked, q.AirFlagged,
                      !q.EndAirborne && !q.EndLocked ? "yes" : "no");
        Check(q.AirFrames > 10 && q.AirLocked > 10 && q.AirFlagged > 0 && !q.EndAirborne && !q.EndLocked, b);
    }
    // The footsteps' per-foot heights are still fed (each foot lifts and plants walking).
    if (TbPartOn('A')) {
        const TestbedRun::MmStats& q = R.Mm[legIndex("A walk")];
        std::snprintf(b, sizeof b, "A walk: the per-foot heights are fed - each foot lifts and plants (left %.1f..%.1f cm, right %.1f..%.1f cm; "
                                   "> 5 cm of travel)", q.FootHMin[0] * 100.0f, q.FootHMax[0] * 100.0f, q.FootHMin[1] * 100.0f, q.FootHMax[1] * 100.0f);
        Check(q.FootHMax[0] - q.FootHMin[0] > 0.05f && q.FootHMax[1] - q.FootHMin[1] > 0.05f, b);
    }
}
