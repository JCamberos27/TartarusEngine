// --npc-test stopsteps: the player on LocomotionLab's open floor walks and jogs (Shift) along the room and stops, again and
// again, and every footstep heard is counted: while moving, and in the 2.5 s after each stop (the body settling into idle).
// A stop should sound like one foot coming to rest at most, never a little shuffle of steps.
#include "NpcTest.h"
#include "Audio/FoleyAudio.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include <cstdio>

namespace {
// The legs: how long, the forward input (+1 forward / -1 back, 0 = stand), Shift (the jog).
struct Leg { float Seconds, Move; bool Sprint; const char* Name; };
const Leg kLegs[] = {
    {1.5f, 0.0f, false, "idle"},
    {2.4f, 1.0f, false, "walk"},   {2.5f, 0.0f, false, "stop after walk"},
    {2.4f, -1.0f, false, "walk back"}, {2.5f, 0.0f, false, "stop after walk back"},
    {1.1f, 1.0f, true, "jog"},  {2.5f, 0.0f, false, "stop after jog"},
    {1.1f, -1.0f, true, "jog back"}, {2.5f, 0.0f, false, "stop after jog back"},
    {1.0f, 1.0f, false, "short walk"}, {2.5f, 0.0f, false, "stop after short walk"},
};
constexpr int kLegCount = (int)(sizeof(kLegs) / sizeof(kLegs[0]));
// The course's home on tests/scenes/LocomotionLab.json's open floor (moved out of Apartment Hallway's 301 living room,
// 2026-11-18 - the owner: no testing in the apartments, a light test scene; the legs keep their 301 distances, measured from
// its south end): clear of labloco's lane (x 0..3), the ledge (-6, 8) and the stairs (8, 4..11).
constexpr glm::vec2 kCourse(-15.0f, -17.0f);
inline glm::vec3 CourseAt(float x, float z, float y = 0.0f) { return glm::vec3(kCourse.x + x, y, kCourse.y + z); }
const glm::vec3 kStart = CourseAt(0.0f, 0.0f, 0.02f); // the run goes +Z and back
} // namespace

float NpcTest::StopStepsSeconds() {
    float t = 0.0f;
    for (const Leg& l : kLegs) t += l.Seconds;
    return t + 0.5f;
}

void NpcTest::StopStepsDrive(Player& player, float dt, float& wantYaw, float& wantPitch) {
    if (!m_SSPlaced) {
        PhysicsWorld::SetCharacterFootPosition(&kStart.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = kStart + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        m_SSPlaced = true;
        m_SSSteps = FoleyAudio::Get().StepsPlayed();
    }
    wantYaw = player.Cam.Yaw = 90.0f; // facing +Z
    wantPitch = -5.0f;
    m_SSClock += dt;
    float t = m_SSClock;
    int leg = 0;
    while (leg < kLegCount && t >= kLegs[leg].Seconds) t -= kLegs[leg++].Seconds;
    if (leg >= kLegCount) return;
    player.ScriptMove = glm::vec2(0.0f, kLegs[leg].Move);
    player.ScriptSprint = kLegs[leg].Sprint;
    // Steps heard since the last frame go to the leg that is on now.
    const int now = FoleyAudio::Get().StepsPlayed();
    if (now > m_SSSteps) {
        // A foot already on its way down when the keys let go (within 0.15 s) is the gait's last stride, not the stop's.
        const int to = leg > 0 && t < 0.15f && kLegs[leg].Move == 0.0f && kLegs[leg - 1].Move != 0.0f ? leg - 1 : leg;
        m_SSLegSteps[to] += now - m_SSSteps;
        std::printf("[StopSteps] %.2f s  %-24s step (%d this leg)\n", m_SSClock, kLegs[to].Name, m_SSLegSteps[to]);
    }
    m_SSSteps = now;
}

void NpcTest::StopSteps(const Player& player, float now) {
    (void)player;
    (void)now;
    if (m_SSClock < StopStepsSeconds() || m_Done) return; // (its own clock: no dummies here to run the director's)
    m_Done = true;
    std::printf("[StopSteps] steps per leg:\n");
    for (int i = 0; i < kLegCount; ++i) std::printf("[StopSteps]   %-24s %d\n", kLegs[i].Name, m_SSLegSteps[i]);
    for (int i = 0; i < kLegCount; ++i) {
        if (kLegs[i].Move != 0.0f) Check(m_SSLegSteps[i] >= 2, std::string(kLegs[i].Name) + ": steps while moving");
        else if (i > 0) Check(m_SSLegSteps[i] <= 1, std::string(kLegs[i].Name) + ": at most one settling step");
    }
}
