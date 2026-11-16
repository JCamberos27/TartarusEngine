// --npc-test npcloco: a dummy (spawned here, roaming off) in Apartment Hallway's 301 living room goes through the gaits - walk
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

namespace {
const glm::vec3 kHome(-8.1f, 0.0f, -8.9f); // 301's south end; the legs go north (+Z) and across (X)
struct NLeg {
    const char* Name;
    glm::vec3 To;  // where to (world, flat); the home for a stand
    Gait Pace;
    float Hold;    // seconds stood once there
};
const NLeg kNLegs[] = {
    {"go home", kHome, Gait::Walk, 2.5f},
    {"walk forward", {-8.1f, 0.0f, -4.7f}, Gait::Walk, 2.0f},
    {"walk back", kHome, Gait::Walk, 2.0f},
    {"walk right", {-10.0f, 0.0f, -8.9f}, Gait::Walk, 2.0f},
    {"walk left", {-6.3f, 0.0f, -8.9f}, Gait::Walk, 2.0f},
    {"walk to home", kHome, Gait::Walk, 2.0f},
    {"jog forward", {-8.1f, 0.0f, -4.6f}, Gait::Jog, 2.0f},
    {"jog back (turns)", kHome, Gait::Jog, 2.0f},
    {"stand", {-8.1f, 0.0f, -5.0f}, Gait::Walk, 2.0f},
};
constexpr int kNLegCount = (int)(sizeof(kNLegs) / sizeof(kNLegs[0]));
static_assert(kNLegCount <= 16, "per-leg arrays");

struct NLegStats {
    float SpeedSum = 0.0f;
    int SpeedN = 0;
    int Starts = 0, Stops = 0; // entries into a start / stop state
    std::set<std::string> States;
};
NLegStats g_N[kNLegCount];
std::string g_LastState;

bool IsStartState(const std::string& s) { return s.rfind("Start", 0) == 0 || s.rfind("CrouchStart", 0) == 0; }
bool IsStopState(const std::string& s) { return s.rfind("Stop", 0) == 0 || s == "CrouchStop"; }
float Flat(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }
} // namespace

float NpcTest::NpcLocoSeconds() { return 75.0f; }

void NpcTest::NpcLocoDrive(Player& player, float& wantYaw, float& wantPitch) {
    // The player stands in the entry, out of the way, looking in.
    if (!m_NLPlaced) {
        const glm::vec3 feet(-6.9f, 0.02f, -1.7f); // 301's entry, by the front door: out of the Scene view's way
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
    // The dummy: one, put in 301's living room by SpawnAt once the director's navigation mesh is up (roaming off).
    if (m_NLProbe.empty() && now > 0.5f && Assets) {
        const int index = npcs.SpawnAt(world, *Assets, glm::vec3(-8.1f, 0.0f, -7.0f), 0.0f, false);
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

    // The Scene view: from the side of the room it isn't on, at chest height, following it.
    const float side = n.Feet.x < -8.1f ? -5.75f : -10.45f;
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
