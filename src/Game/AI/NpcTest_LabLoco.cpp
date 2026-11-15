// --npc-test labloco: tests/scenes/LocomotionLab.json, the open grid floor. The player walks, jogs, back-pedals and strafes down
// the lane, stopping between each; the bare dummy soldier keeps pace 3 m to its left at the same gait (the same driver, side by
// side). Recorded every frame (NPC_TEST_RECORD): the first-person view, and the Scene view square to the player's side. With
// NPC_TEST_RECORD set it also writes trace.csv there: per frame, the gait phase, the camera's bob and roll and where the gun
// sits in the view - the numbers behind what the frames show.
#include "NpcTest.h"
#include "BodyDebugDraw.h"
#include "FirstPersonPresentation.h"
#include "NpcDirector.h"
#include "IK.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {
struct LabLeg {
    float Seconds;
    glm::vec2 Move; // x right, y forward
    bool Jog;
    bool Place;     // put back at the start of the lane in its last 0.3 s (stationary legs)
    glm::vec2 At;
    const char* Name;
    float Yaw = 90.0f; // the view (degrees; 90 = +Z, down the lane): both bodies face it - a change is a turn
    bool Aim = true;   // the dummy aims along it (else carries the gun at the hip and only looks)
};
const LabLeg kLab[] = {
    {1.5f, {0, 0}, false, true, {0.0f, -20.0f}, "idle"},
    {6.0f, {0, 1}, false, false, {}, "walk forward"},
    {2.0f, {0, 0}, false, false, {}, "stop after walk"},
    {5.0f, {0, 1}, true, false, {}, "jog forward"},
    {2.0f, {0, 0}, false, false, {}, "stop after jog"},
    {4.0f, {0, -1}, false, false, {}, "walk back"},
    {2.0f, {0, 0}, false, false, {}, "stop after walk back"},
    {3.0f, {1, 0}, false, false, {}, "strafe right"},
    {2.0f, {0, 0}, false, false, {}, "stop after strafe right"},
    {3.0f, {-1, 0}, false, false, {}, "strafe left"},
    {2.0f, {0, 0}, false, false, {}, "stop after strafe left"},
    {4.0f, {0, -1}, true, false, {}, "jog back"},
    {2.0f, {0, 0}, false, false, {}, "stop after jog back"},
    // Turns on the spot and a turning start, gun at the hip and aimed: the heading - and the gun and the arms on it - come
    // round with the turn clips.
    {2.5f, {0, 0}, false, false, {}, "turn left 90 (hip)", 180.0f, false},
    {3.0f, {0, 0}, false, false, {}, "turn round 180 (hip)", 0.0f, false},
    {2.5f, {0, 0}, false, false, {}, "turn right 90 (aiming)", 270.0f, true},
    {3.0f, {0, 1}, false, false, {}, "turning start 180 (hip)", 90.0f, false},
    {2.5f, {0, 0}, false, false, {}, "stop after turning start", 90.0f, false},
};
constexpr int kLabCount = (int)(sizeof(kLab) / sizeof(kLab[0]));
std::FILE* g_Trace = nullptr;

int LabLegAt(float t, float& into) {
    int leg = 0;
    while (leg < kLabCount && t >= kLab[leg].Seconds) t -= kLab[leg++].Seconds;
    into = t;
    return leg;
}
} // namespace

float NpcTest::LabLocoSeconds() {
    float t = 0.0f;
    for (const LabLeg& l : kLab) t += l.Seconds;
    return t + 0.5f;
}

void NpcTest::LabLocoDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch) {
    auto place = [&](glm::vec2 at) {
        const glm::vec3 feet(at.x, 0.05f, at.y);
        PhysicsWorld::SetCharacterFootPosition(&feet.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = feet + glm::vec3(0.0f, player.EyeHeight, 0.0f);
    };
    if (!m_LabPlaced) {
        place(kLab[0].At);
        m_LabPlaced = true;
    }
    {
        float into = 0.0f;
        const int leg = LabLegAt(m_LabClock, into);
        wantYaw = player.Cam.Yaw = leg < kLabCount ? kLab[leg].Yaw : 90.0f; // 90: +Z, down the lane
    }
    wantPitch = -2.0f;
    const float before = m_LabClock;
    m_LabClock += dt;
    float into = 0.0f, intoBefore = 0.0f;
    const int leg = LabLegAt(m_LabClock, into);
    const int legBefore = LabLegAt(before, intoBefore);
    IK::SetFootSlideMeterOn(true);
    if (legBefore != leg && legBefore < kLabCount) SlideLeg(kLab[legBefore].Name);
    if (leg >= kLabCount) {
        if (g_Trace) { std::fclose(g_Trace); g_Trace = nullptr; }
        return;
    }
    const LabLeg& L = kLab[leg];
    player.ScriptMove = L.Move;
    player.ScriptSprint = L.Jog;
    if (L.Place && into >= L.Seconds - 0.3f && (legBefore != leg || intoBefore < L.Seconds - 0.3f)) place(L.At);

    // The trace: what the eye and the gun do, frame by frame.
    if (!g_Trace && !RecordDir().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(RecordDir(), ec);
        if (fopen_s(&g_Trace, (std::filesystem::path(RecordDir()) / "trace.csv").string().c_str(), "w") != 0) g_Trace = nullptr;
        if (g_Trace)
            std::fprintf(g_Trace, "frame,t,leg,speed,gait_ok,gait,eye_y,cam_off_y,cam_roll,gun_x,gun_y,gun_z,gun_pitch,gun_yaw,gun_roll,"
                                  "muzzle_x,muzzle_y,muzzle_z,eye_x,eye_y2,eye_z,hand_gap_l,hand_gap_r,state\n");
    }
    if (g_Trace) {
        const WeaponProceduralPose& pose = weapon.ProceduralPose();
        glm::vec3 muzzle(0.0f), dir(0.0f), local(0.0f);
        if (weapon.MuzzleRay(muzzle, dir)) {
            const glm::vec3 f = player.Cam.Front(), r = glm::normalize(glm::cross(f, glm::vec3(0, 1, 0))), u = glm::cross(r, f);
            const glm::vec3 d = muzzle - player.Cam.Position;
            local = glm::vec3(glm::dot(d, r), glm::dot(d, u), glm::dot(d, f));
        }
        const BodyDebug::Snapshot& bd = BodyDebug::Info();
        const glm::vec3 f = player.Cam.Front(), r = glm::normalize(glm::cross(f, glm::vec3(0, 1, 0))), u = glm::cross(r, f);
        const glm::vec3 eye(glm::dot(bd.EyeApplied, r), glm::dot(bd.EyeApplied, u), glm::dot(bd.EyeApplied, f));
        std::fprintf(g_Trace, "%d,%.4f,%d,%.3f,%d,%.4f,%.4f,%.4f,%.3f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%s\n", m_LabFrame++,
                     m_LabClock, leg, glm::length(glm::vec2(player.Velocity.x, player.Velocity.z)), (int)weapon.HaveGaitPhase(),
                     weapon.GaitPhaseValue(), player.Cam.Position.y, pose.CameraOffset.y, pose.CameraRoll, pose.Position.x, pose.Position.y,
                     pose.Position.z, pose.Rotation.x, pose.Rotation.y, pose.Rotation.z, local.x, local.y, local.z, eye.x, eye.y, eye.z,
                     bd.HandGap[0], bd.HandGap[1], weapon.CurrentState().c_str());
    }

    // The Scene view: off the pair's front quarter (the player and the dummy 3 m to its side both in shot, neither hiding the
    // other), following them down the lane at chest height.
    const glm::vec3 feet = player.Cam.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f);
    const glm::vec3 mid = feet + glm::vec3(1.5f, 0.0f, 0.0f);
    const glm::vec3 want(mid.x - 2.8f, feet.y + 1.3f, mid.z + 2.8f);
    m_CamPos = m_HaveCam ? m_CamPos + (want - m_CamPos) * std::min(1.0f, dt * 5.0f) : want;
    const glm::vec3 look = glm::normalize(mid + glm::vec3(0.0f, 0.8f, 0.0f) - m_CamPos);
    m_CamYaw = glm::degrees(std::atan2(look.z, look.x));
    m_CamPitch = glm::degrees(std::asin(std::clamp(look.y, -1.0f, 1.0f)));
    m_HaveCam = true;
}

void NpcTest::LabLoco(World& world, NpcDirector& npcs, const Player& player, float now) {
    (void)world;
    (void)now;
    npcs.Frozen = true;
    npcs.HoldFire = true;
    npcs.FootIKEverywhere = true; // the dummy is off the player's screen, but the Scene view watches its feet
    // The dummy keeps pace 3 m to the player's left (-X; the lane faces +Z), at the player's gait.
    float into = 0.0f;
    const int leg = LabLegAt(m_LabClock, into);
    for (const auto& up : npcs.Npcs()) {
        if (!up || up->Dead) continue;
        Npc& n = *up;
        if (std::find(m_SlideBodies.begin(), m_SlideBodies.end(), n.Name) == m_SlideBodies.end()) m_SlideBodies.push_back(n.Name);
        const glm::vec3 feet = player.Cam.Position - glm::vec3(0.0f, player.EyeHeight, 0.0f);
        const glm::vec3 there(feet.x + 3.0f, n.Feet.y, feet.z);
        NpcIntent in;
        const bool moving = leg < kLabCount && (kLab[leg].Move.x != 0.0f || kLab[leg].Move.y != 0.0f);
        in.Move = glm::length(glm::vec2(there.x - n.Feet.x, there.z - n.Feet.z)) > (moving ? 0.1f : 0.6f);
        // Ahead of it along the player's travel, so it keeps up rather than trails.
        const glm::vec3 lead = moving ? glm::vec3(player.Velocity.x, 0.0f, player.Velocity.z) * 0.6f : glm::vec3(0.0f);
        in.MoveTarget = there + lead;
        in.Pace = leg < kLabCount && kLab[leg].Jog ? Gait::Jog : Gait::Walk;
        const float yaw = glm::radians(leg < kLabCount ? kLab[leg].Yaw : 90.0f);
        const glm::vec3 view(std::cos(yaw), 0.0f, std::sin(yaw));
        in.Aim = leg >= kLabCount || kLab[leg].Aim;
        in.FaceAim = true;
        in.AimPoint = n.Feet + glm::vec3(0.0f, 1.45f, 0.0f) + view * 20.0f;
        in.LookPoint = in.AimPoint;
        n.Intent = in;
    }
    if (m_LabClock >= LabLocoSeconds() && !m_Done) {
        m_Done = true;
        Check(m_LabFrame > 100, "labloco: the run went through (" + std::to_string(m_LabFrame) + " frames traced)");
    }
}
