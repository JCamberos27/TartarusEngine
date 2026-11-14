// --npc-test locomotion: the player's own first-person body in Apartment Hallway's 301 living room goes through every
// gait - walk (the default) forward, back and both strafes, the jog (Shift) forward, back and on a diagonal, a hop on
// the spot, a hop on the move, a drop from height, the crouch and a crouched walk - stopping in between. The Scene view
// watches the body from the side (record it with NPC_TEST_RECORD; NPC_TEST_BARE undresses it to see the mesh).
// Checked: each gait's steady speed, the gun never lowered at a jog, a hop lands light (no recovery clip) and the drop
// heavy (the recovery), and the crouch plays its own states.
#include "NpcTest.h"
#include "BodyDebugDraw.h"
#include "FirstPersonPresentation.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace {
// A leg: how long; the move input (x right, y forward); Shift; crouch; a jump at Jump seconds in (< 0 = none);
// dropped from Drop metres up at its start (0 = no); placed at At (x, z) in its last 0.3 s (stationary legs only).
struct Leg {
    float Seconds;
    glm::vec2 Move;
    bool Jog, Crouch;
    float Jump, Drop;
    bool Place;
    glm::vec2 At;
    const char* Name;
};
constexpr glm::vec2 kNo(0.0f);
const float kD = 0.70710678f;
const Leg kLegs[] = {
    {1.5f, {0, 0}, false, false, -1, 0, true, {-8.1f, -9.0f}, "idle"},
    {3.0f, {0, 1}, false, false, -1, 0, false, kNo, "walk forward"},
    {2.0f, {0, 0}, false, false, -1, 0, false, kNo, "stop after walk"},
    {3.0f, {0, -1}, false, false, -1, 0, false, kNo, "walk back"},
    {2.0f, {0, 0}, false, false, -1, 0, true, {-6.3f, -6.7f}, "stop after walk back"},
    {2.0f, {1, 0}, false, false, -1, 0, false, kNo, "walk strafe right"},
    {2.0f, {0, 0}, false, false, -1, 0, false, kNo, "stop after strafe right"},
    {2.0f, {-1, 0}, false, false, -1, 0, false, kNo, "walk strafe left"},
    {2.0f, {0, 0}, false, false, -1, 0, true, {-8.1f, -9.1f}, "stop after strafe left"},
    {1.3f, {0, 1}, true, false, -1, 0, false, kNo, "jog forward"},
    {2.0f, {0, 0}, false, false, -1, 0, false, kNo, "stop after jog"},
    {1.3f, {0, -1}, true, false, -1, 0, false, kNo, "jog back"},
    {2.0f, {0, 0}, false, false, -1, 0, true, {-6.1f, -9.0f}, "stop after jog back"},
    {1.4f, {kD, kD}, true, false, -1, 0, false, kNo, "jog diagonal"},
    {2.0f, {0, 0}, false, false, -1, 0, true, {-8.1f, -8.8f}, "stop after diagonal"},
    {1.8f, {0, 0}, false, false, 0.2f, 0, false, kNo, "hop on the spot"},
    {2.2f, {0, 1}, false, false, 0.8f, 0, false, kNo, "walk and hop"},
    {2.0f, {0, 0}, false, false, -1, 0, true, {-8.1f, -7.0f}, "stop after hop"},
    {2.6f, {0, 0}, false, false, -1, 0.7f, true, {-8.1f, -9.0f}, "drop"},
    {2.2f, {0, 0}, false, true, -1, 0, false, kNo, "crouch"},
    {3.0f, {0, 1}, false, true, -1, 0, false, kNo, "crouch walk"},
    {2.0f, {0, 0}, false, true, -1, 0, false, kNo, "crouch stop"},
    {2.0f, {0, 0}, false, false, -1, 0, false, kNo, "stand up"},
};
constexpr int kLegCount = (int)(sizeof(kLegs) / sizeof(kLegs[0]));
static_assert(kLegCount <= 32, "NpcTest's per-leg arrays");

struct LegStats {
    float SpeedSum = 0.0f;
    int SpeedN = 0;
    float PeakFall = 0.0f;
    bool ArmsSprint = false;
    std::set<std::string> States;
};
LegStats g_Stats[kLegCount];

int LegAt(float t, float& into) {
    int leg = 0;
    while (leg < kLegCount && t >= kLegs[leg].Seconds) t -= kLegs[leg++].Seconds;
    into = t;
    return leg;
}
} // namespace

float NpcTest::LocomotionSeconds() {
    float t = 0.0f;
    for (const Leg& l : kLegs) t += l.Seconds;
    return t + 0.5f;
}

void NpcTest::LocomotionDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch) {
    auto place = [&](glm::vec2 at, float up) {
        const glm::vec3 feet(at.x, 0.02f + up, at.y);
        PhysicsWorld::SetCharacterFootPosition(&feet.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = feet + glm::vec3(0.0f, player.EyeHeight, 0.0f);
    };
    if (!m_LPlaced) {
        place(kLegs[0].At, 0.0f);
        m_LPlaced = true;
    }
    wantYaw = player.Cam.Yaw = 90.0f; // facing +Z, down the room
    wantPitch = -5.0f;
    const float before = m_LClock;
    m_LClock += dt;
    float into = 0.0f, intoBefore = 0.0f;
    const int leg = LegAt(m_LClock, into);
    const int legBefore = LegAt(before, intoBefore);
    if (leg >= kLegCount) return;
    const Leg& L = kLegs[leg];
    player.ScriptMove = L.Move;
    player.ScriptSprint = L.Jog;
    player.ScriptCrouch = L.Crouch;
    player.ScriptJump = L.Jump >= 0.0f && (legBefore != leg ? into >= L.Jump : intoBefore < L.Jump && into >= L.Jump);
    // A fall from ~2.5 m: 301's ceiling leaves under a metre to fall, so for its first 0.4 s the body is held up there,
    // already falling at ledge speed (the airtime a real drop has), then let go.
    if (L.Drop > 0.0f && into < 0.4f) {
        if (legBefore != leg) m_LDropAt = glm::vec2(player.Cam.Position.x, player.Cam.Position.z);
        place(m_LDropAt, L.Drop);
        player.Grounded = false; // or the mover snaps it straight back down to the floor
        player.Velocity.y = -6.5f;
    }
    if (L.Place && into >= L.Seconds - 0.3f && (legBefore != leg || intoBefore < L.Seconds - 0.3f)) place(L.At, 0.0f);

    // What this leg does: the steady speed over its second half, the fall, the body's states, the arms' state.
    LegStats& s = g_Stats[leg];
    if ((L.Move.x != 0.0f || L.Move.y != 0.0f) && into > L.Seconds * 0.5f && player.Grounded) {
        s.SpeedSum += glm::length(glm::vec2(player.Velocity.x, player.Velocity.z));
        ++s.SpeedN;
    }
    if (!player.Grounded) s.PeakFall = std::max(s.PeakFall, -player.Velocity.y);
    const BodyDebug::Snapshot& d = BodyDebug::Info();
    if (d.Valid && !d.AnimatorState.empty() && s.States.insert(d.AnimatorState).second)
        std::printf("[Locomotion] %6.2f s  %-24s body -> %s\n", m_LClock, L.Name, d.AnimatorState.c_str());
    if (weapon.IsActive() && weapon.CurrentState() == "Sprint") s.ArmsSprint = true;

    // The Scene view: square to the body's side, a couple of metres off (the room is 5 m across), following it down
    // the room, at chest height.
    const glm::vec3 feet = player.Cam.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f);
    const float side = feet.x < -8.1f ? -5.75f : -10.45f;
    const glm::vec3 want(side, feet.y + 1.05f, feet.z);
    if (!m_HaveCam || std::abs(m_CamPos.x - want.x) > 1.0f) m_CamPos = want;
    else m_CamPos += (want - m_CamPos) * std::min(1.0f, dt * 4.0f);
    const glm::vec3 look = glm::normalize(feet + glm::vec3(0.0f, 0.85f, 0.0f) - m_CamPos);
    m_CamYaw = glm::degrees(std::atan2(look.z, look.x));
    m_CamPitch = glm::degrees(std::asin(std::clamp(look.y, -1.0f, 1.0f)));
    m_HaveCam = true;
}

void NpcTest::Locomotion(const Player& player, float now) {
    (void)player;
    (void)now;
    if (m_LClock < LocomotionSeconds() || m_Done) return;
    m_Done = true;
    for (int i = 0; i < kLegCount; ++i) {
        const LegStats& s = g_Stats[i];
        std::string states;
        for (const std::string& n : s.States) states += (states.empty() ? "" : ", ") + n;
        std::printf("[Locomotion] %-24s speed %.2f m/s  fall %.1f m/s  arms sprint %d  states: %s\n", kLegs[i].Name,
                    s.SpeedN ? s.SpeedSum / (float)s.SpeedN : 0.0f, s.PeakFall, (int)s.ArmsSprint, states.c_str());
    }
    auto speed = [&](int i) { return g_Stats[i].SpeedN ? g_Stats[i].SpeedSum / (float)g_Stats[i].SpeedN : 0.0f; };
    auto visited = [&](int i, const char* state) { return g_Stats[i].States.count(state) > 0; };
    bool armsDown = false;
    for (const LegStats& s : g_Stats) armsDown = armsDown || s.ArmsSprint;
    Check(speed(1) > 1.2f && speed(1) < 1.9f, "the default gait is the walk (" + std::to_string(speed(1)) + " m/s forward)");
    Check(speed(5) > 1.0f && speed(5) < 1.9f, "the walk strafes (" + std::to_string(speed(5)) + " m/s)");
    Check(speed(9) > 2.7f && speed(9) < 3.8f, "Shift is the jog (" + std::to_string(speed(9)) + " m/s forward)");
    Check(speed(13) > 2.5f && speed(13) < 3.8f, "the jog on a diagonal (" + std::to_string(speed(13)) + " m/s)");
    Check(!armsDown, "the gun stays up at a jog (the arms never sprint)");
    bool sprintState = false;
    for (const LegStats& s : g_Stats) sprintState = sprintState || s.States.count("Sprint");
    Check(!sprintState, "the body never sprints");
    Check(visited(9, "Start") || visited(9, "StartRun") || visited(9, "StartWalk"), "the jog starts with a start clip");
    Check(visited(10, "Stop") || visited(10, "StopWalk"), "the jog stops with a stop clip");
    Check(visited(2, "StopWalk") || visited(2, "Stop"), "the walk stops with a stop clip");
    Check(visited(15, "Jump") && !visited(15, "Land"), "a hop on the spot lands light (no recovery clip)");
    Check(!visited(16, "Land"), "a hop on the move lands light");
    Check(g_Stats[18].PeakFall > 7.0f && visited(18, "Land"), "a drop lands heavy (" + std::to_string(g_Stats[18].PeakFall) + " m/s)");
    Check(visited(19, "CrouchDown") || visited(19, "CrouchLoco"), "the crouch plays its own states");
    Check(speed(20) > 0.5f && speed(20) < 1.6f, "the crouched walk (" + std::to_string(speed(20)) + " m/s)");
}
