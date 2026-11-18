// --npc-test npcloco: a dummy (spawned here, roaming off) on LocomotionLab's open floor goes through the gaits - walk
// forward, back and both sideways (it faces its travel), the jog out and back - stopping between each (ScriptedMove).
// The Scene view watches it from the side (record with NPC_TEST_RECORD; NPC_TEST_BARE undresses it). Checked per leg: the
// top speed, the start and stop clips, and that the body doesn't chatter between states (a start or a stop retriggered
// while it's going one way).
#include "NpcTest.h"
#include "NpcDirector.h"
#include "Player.h"
#include "IK.h"
#include "PhysicsWorld.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

namespace {
// The course's home on tests/scenes/LocomotionLab.json's open floor (moved out of Apartment Hallway's 301 living room,
// 2026-11-18 - the owner: no testing in the apartments, a light test scene; the legs keep their 301 distances, measured from
// its south end): clear of labloco's lane (x 0..3), the ledge (-6, 8) and the stairs (8, 4..11).
constexpr glm::vec2 kCourse(-15.0f, -17.0f);
inline glm::vec3 CourseAt(float x, float z, float y = 0.0f) { return glm::vec3(kCourse.x + x, y, kCourse.y + z); }
const glm::vec3 kHome = CourseAt(0.0f, 0.0f); // the legs go +Z and across (X)
struct NLeg {
    const char* Name;
    glm::vec3 To;  // where to (world, flat); the home for a stand
    Gait Pace;
    float Hold;    // seconds stood once there
};
const NLeg kNLegs[] = {
    {"go home", kHome, Gait::Walk, 2.5f},
    {"walk forward", CourseAt(0.0f, 4.2f), Gait::Walk, 2.0f},
    {"walk back", kHome, Gait::Walk, 2.0f},
    {"walk right", CourseAt(-1.9f, 0.0f), Gait::Walk, 2.0f},
    {"walk left", CourseAt(1.8f, 0.0f), Gait::Walk, 2.0f},
    {"walk to home", kHome, Gait::Walk, 2.0f},
    {"jog forward", CourseAt(0.0f, 4.3f), Gait::Jog, 2.0f},
    {"jog back (turns)", kHome, Gait::Jog, 2.0f},
    {"stand", CourseAt(0.0f, 3.9f), Gait::Walk, 2.0f},
};
constexpr int kNLegCount = (int)(sizeof(kNLegs) / sizeof(kNLegs[0]));
static_assert(kNLegCount <= 16, "per-leg arrays");

struct NLegStats {
    float SpeedSum = 0.0f;
    int SpeedN = 0;
    int Starts = 0, Stops = 0; // entries into a start / stop state
    std::set<std::string> States;
    // Motion matching (the body's matcher steps, NpcBody::MmDebug): on the move (capsule over 0.3 m/s, not yet there) the jumps
    // (a search that changed the clip), the pose steps and the time; cruising (at 90 % of the pace or more) the frames playing
    // the style asked for; and the matched frame's gait and clip at the leg's end.
    float Pace = 0.0f;
    int Asked = -1;
    int MoveJumps = 0, MoveSteps = 0;
    float MoveTime = 0.0f;
    std::vector<float> PoseSteps;
    int Cruise = 0, CruiseStyle = 0;
    // ... and the jumps while cruising (at 90 % of the pace, 0.5 s on from setting off: the start's and the stop's searches left
    // out - a 1 m leg is little else), what the reference (a long steady walk) measures.
    float MoveStart = -1.0f, CruiseTime = 0.0f;
    int CruiseJumps = 0;
    int EndGait = -1;
    std::string EndClip;
};
NLegStats g_N[kNLegCount];
std::string g_LastState;
bool g_Mm = false;
int g_MmUpdates = 0;
float P99(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[(size_t)(0.99f * (float)(v.size() - 1))];
}

bool IsStartState(const std::string& s) { return s.rfind("Start", 0) == 0 || s.rfind("CrouchStart", 0) == 0; }
bool IsStopState(const std::string& s) { return s.rfind("Stop", 0) == 0 || s == "CrouchStop"; }
float Flat(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }
} // namespace

float NpcTest::NpcLocoSeconds() { return 75.0f; }

void NpcTest::NpcLocoDrive(Player& player, float& wantYaw, float& wantPitch) {
    // The player stands off the course, out of the way, looking at it.
    if (!m_NLPlaced) {
        const glm::vec3 feet = CourseAt(1.2f, 7.2f, 0.02f); // (where 301's entry was): out of the Scene view's way
        PhysicsWorld::SetCharacterFootPosition(&feet.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = feet + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        m_NLPlaced = true;
    }
    wantYaw = player.Cam.Yaw = -100.0f;
    wantPitch = -10.0f;
}

void NpcTest::NpcLoco(World& world, NpcDirector& npcs, float now) {
    const float dt = std::max(0.0f, now - m_NLPrev);
    m_NLPrev = now;
    npcs.FootIKEverywhere = true; // the Scene view watches his feet wherever the player looks
    // The dummy: one, put on the course by SpawnAt once the director's navigation mesh is up (roaming off).
    if (m_NLProbe.empty() && now > 0.5f && Assets) {
        const int index = npcs.SpawnAt(world, *Assets, CourseAt(0.0f, 1.9f), 0.0f, false);
        if (Npc* spawned = index >= 0 ? npcs.Find(index) : nullptr) m_NLProbe = spawned->Name;
    }
    Npc* probe = nullptr;
    for (const auto& up : npcs.Npcs())
        if (up && !up->Dead && !m_NLProbe.empty() && up->Name == m_NLProbe) probe = up.get();
    if (!probe || now < 1.0f || m_NLLeg >= kNLegCount) {
        if (m_NLLeg >= kNLegCount && !m_Done) NpcLocoReport();
        return;
    }
    Npc& n = *probe;
    IK::SetFootSlideMeterOn(true);
    if (m_SlideBodies.empty()) m_SlideBodies.push_back(n.Name);
    const NLeg& L = kNLegs[m_NLLeg];
    const glm::vec3 to(L.To.x, n.Feet.y, L.To.z);
    const bool there = Flat(n.Feet, to) < 0.35f;
    if (there && m_NLArrived < 0.0f) m_NLArrived = now;
    n.Roam = false;
    if (!there || m_NLArrived < 0.0f) npcs.ScriptedMove(n.Index, to, L.Pace);
    else npcs.EndScriptedMove(n.Index);
    n.LookPoint = n.Feet + glm::vec3(0.0f, 1.6f, 0.0f) + (to - n.Feet) * 2.0f + glm::vec3(0.0f, 0.0f, 0.01f);

    NLegStats& s = g_N[m_NLLeg];
    const float legTime = now - m_NLStart;
    const float speed = glm::length(glm::vec2(n.Velocity.x, n.Velocity.z));
    if (m_NLArrived < 0.0f) { s.SpeedSum = std::max(s.SpeedSum, speed); s.SpeedN = 1; } // the leg's top speed
    g_Mm = n.Body.MotionMatching();
    const bool moving = m_NLArrived < 0.0f && speed > 0.3f;
    if (moving && s.MoveStart < 0.0f) s.MoveStart = now;
    const bool cruising = moving && s.Pace > 0.0f && speed >= 0.9f * s.Pace && now - s.MoveStart >= 0.5f;
    if (g_Mm && moving) s.MoveTime += dt; // (every frame: the LOD may hold some matcher steps)
    if (g_Mm && cruising) s.CruiseTime += dt;
    if (g_Mm && n.Body.MmUpdates() != g_MmUpdates) { // a matcher step this frame
        g_MmUpdates = n.Body.MmUpdates();
        const MmPlayerDebug::Snapshot& mm = n.Body.MmDebug();
        if (n.PaceSpeed > 0.0f) s.Pace = n.PaceSpeed;
        s.Asked = L.Pace == Gait::Jog ? (int)MotionLabel::StyleJog : L.Pace == Gait::Run ? (int)MotionLabel::StyleSprint : (int)MotionLabel::StyleWalk;
        if (mm.Jumped) // (every jump, for the diagnosis: why the search ran - forced by the input, the clip's end, an excluded frame)
            std::printf("[NpcLocoJump] %6.2f s %-18s %s -> %s f%u  cost %.2f (playing %.2f)  forced input %d end %d excluded %d  %.2f m/s yaw %.0f "
                        "vel %.0f%s\n", now, L.Name, s.EndClip.c_str(), mm.Clip.c_str(), mm.Frame, mm.LastCost, mm.PlayingCost, mm.ForcedByInput,
                        mm.ForcedByEnd, mm.ForcedByExcluded, speed, glm::degrees(mm.Yaw), glm::degrees(NpcYawOf(n.Velocity, 0.0f)),
                        m_NLArrived < 0.0f && speed > 0.3f ? " (moving)" : "");
        if (cruising) s.CruiseJumps += mm.Jumped ? 1 : 0;
        if (moving) {
            s.MoveJumps += mm.Jumped ? 1 : 0;
            ++s.MoveSteps;
            s.PoseSteps.push_back(mm.PoseStepDeg);
            if (s.Pace > 0.0f && speed >= 0.9f * s.Pace) {
                ++s.Cruise;
                s.CruiseStyle += mm.Style == s.Asked ? 1 : 0;
            }
        }
        s.EndGait = mm.Gait;
        s.EndClip = mm.Clip;
    }
    const std::string state = n.Body.StateName(world);
    if (!state.empty() && state != g_LastState) {
        s.States.insert(state);
        if (IsStartState(state)) ++s.Starts;
        if (IsStopState(state)) ++s.Stops;
        std::printf("[NpcLoco] %6.2f s  %-28s %s -> %s  (%.2f m/s)\n", now, L.Name, g_LastState.c_str(), state.c_str(), speed);
        g_LastState = state;
    }
    if ((m_NLArrived >= 0.0f && now - m_NLArrived >= L.Hold) || legTime > 12.0f) {
        SlideLeg(L.Name);
        ++m_NLLeg;
        m_NLStart = now;
        m_NLArrived = -1.0f;
    }

    // The Scene view: from the side of the course it isn't on, at chest height, following it.
    const float side = n.Feet.x < kCourse.x ? kCourse.x + 2.35f : kCourse.x - 2.35f;
    const glm::vec3 want(side, n.Feet.y + 1.05f, n.Feet.z);
    if (!m_HaveCam || std::abs(m_CamPos.x - want.x) > 1.0f) m_CamPos = want;
    else m_CamPos += (want - m_CamPos) * std::min(1.0f, dt * 4.0f);
    const glm::vec3 look = glm::normalize(n.Feet + glm::vec3(0.0f, 0.85f, 0.0f) - m_CamPos);
    m_CamYaw = glm::degrees(std::atan2(look.z, look.x));
    m_CamPitch = glm::degrees(std::asin(std::clamp(look.y, -1.0f, 1.0f)));
    m_HaveCam = true;
}

void NpcTest::NpcLocoReport() {
    m_Done = true;
    m_Duration = m_Time; // done: end the run
    auto speed = [](int i) { return g_N[i].SpeedN ? g_N[i].SpeedSum / (float)g_N[i].SpeedN : 0.0f; };
    for (int i = 0; i < kNLegCount; ++i) {
        std::string states;
        for (const std::string& st : g_N[i].States) states += (states.empty() ? "" : ", ") + st;
        std::printf("[NpcLoco] %-28s speed %.2f m/s  starts %d  stops %d  states: %s\n", kNLegs[i].Name, speed(i), g_N[i].Starts,
                    g_N[i].Stops, states.c_str());
    }
    Check(m_NLLeg >= kNLegCount, "the dummy went through every leg");
    if (g_Mm) {
        // Motion matching (Phase 5): the clip checks become the matcher's. The limits are mmplayer's, against the player's own walk
        // on the same database (`A walk`, docs/MOTION_MATCHING_HANDOFF.md: 1.2 jumps a second, pose step p99 7.75 deg): jumps while
        // cruising within 1.5x + 0.5 (rated on a leg with 1 s of it or more), pose steps on the move within 1.5x + 1; the style asked for on >= 80 % of the cruising frames; the capsule at the
        // database's gait speed (within 15 %: the director's pace); and each leg ends on a standing idle frame (the stop played).
        constexpr float kRefJumps = 1.2f, kRefStep = 7.75f;
        for (int i = 1; i < kNLegCount; ++i) {
            const NLeg& L = kNLegs[i];
            if (Flat(L.To, kNLegs[i - 1].To) <= 0.5f) continue;
            const NLegStats& s = g_N[i];
            const float jumps = s.MoveTime > 0.0f ? (float)s.MoveJumps / s.MoveTime : 0.0f, p99 = P99(s.PoseSteps);
            const float cruiseJumps = s.CruiseTime > 0.0f ? (float)s.CruiseJumps / s.CruiseTime : 0.0f;
            const float style = s.Cruise ? 100.0f * (float)s.CruiseStyle / (float)s.Cruise : 0.0f;
            std::printf("[NpcLoco] MM %-22s pace %.2f m/s top %.2f; moving %.1f s, %d jumps (%.2f/s), pose step p99 %.2f deg; style %.0f %% of %d "
                        "cruising (%.1f s: %d jumps, %.2f/s); ends on %s (gait %d)\n", L.Name, s.Pace, speed(i), s.MoveTime, s.MoveJumps, jumps, p99,
                        style, s.Cruise, s.CruiseTime, s.CruiseJumps, cruiseJumps, s.EndClip.c_str(), s.EndGait);
            Check(s.Pace > 0.0f && speed(i) > 0.85f * s.Pace && speed(i) < 1.15f * s.Pace,
                  std::string(L.Name) + ": top speed " + std::to_string(speed(i)) + " m/s, the gait's " + std::to_string(s.Pace) + " (within 15 %)");
            if (s.CruiseTime >= 1.0f)
                Check(cruiseJumps <= 1.5f * kRefJumps + 0.5f, std::string(L.Name) + ": " + std::to_string(cruiseJumps) +
                                                                  " jumps a second cruising (<= " + std::to_string(1.5f * kRefJumps + 0.5f) + ")");
            else
                std::printf("[NpcLoco] %s: %.1f s cruising - too short to rate its jumps (logged above)\n", L.Name, s.CruiseTime);
            Check(p99 <= 1.5f * kRefStep + 1.0f, std::string(L.Name) + ": pose step p99 " + std::to_string(p99) + " deg (<= " +
                                                     std::to_string(1.5f * kRefStep + 1.0f) + ")");
            Check(s.Cruise > 0 && style >= 80.0f, std::string(L.Name) + ": cruising plays the style asked for (" + std::to_string(style) + " %)");
            Check(s.EndGait == MotionLabel::Idle, std::string(L.Name) + ": stops on a standing idle frame (" + s.EndClip + ")");
        }
        return;
    }
    for (int i = 1; i < kNLegCount; ++i) {
        const NLeg& L = kNLegs[i];
        const bool moves = Flat(L.To, kNLegs[i - 1].To) > 0.5f;
        if (!moves) continue;
        const float lo = L.Pace == Gait::Jog ? 2.7f : 1.2f, hi = L.Pace == Gait::Jog ? 3.7f : 1.9f;
        Check(speed(i) > lo && speed(i) < hi, std::string(L.Name) + ": top speed " + std::to_string(speed(i)) + " m/s");
        Check(g_N[i].Starts <= 1, std::string(L.Name) + ": one start at most (" + std::to_string(g_N[i].Starts) + ")");
        Check(g_N[i].Stops <= 1, std::string(L.Name) + ": one stop at most (" + std::to_string(g_N[i].Stops) + ")");
        Check(g_N[i].Stops == 1, std::string(L.Name) + ": stops with a stop clip");
    }
}
