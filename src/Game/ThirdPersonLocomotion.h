#pragma once

#include "IK.h"

#include <glm/glm.hpp>

#include <array>
#include <map>
#include <string>
#include <vector>

class AssetLibrary;
class Model;

// The third-person body's legs and hips: the MoCap Online Mobility set (MOB1, a UE4 pack baked onto the rig by
// AnimRetarget), played from the character's own movement rather than driving it - the player's movement and
// first-person body stay on their own clips; this is what every other view, the shadows and the enemy soldiers show.
//
//   - Eight directions (forward, the diagonals, the strafes, the backpedals) blended by the travel's heading.
//   - Walk, jog and run crossfaded by speed, each played at the rate that keeps its feet planted (the clips' own
//     ground speed is measured off their planted foot once, at bind); one shared phase keeps the footfalls in step.
//   - Standing, crouched, turning on the spot, in the air.
// The upper body is ThirdPersonWeapon's when a gun is held.
struct ThirdPersonMove {
    glm::vec2 Velocity{0.0f}; // body frame: x right, y forward, m/s
    float Crouch = 0.0f;      // 0..1
    bool Grounded = true;
    float TurnRate = 0.0f;    // the body's heading, radians/s, + left
    float ModelScale = 1.0f;  // world metres per model unit (the clips' measured speeds are in model units)
};

class ThirdPersonLocomotion {
public:
    static const char* const kFolder;
    // The idles the weapon layer's spine is layered against.
    static std::string StandIdle();
    static std::string CrouchIdle();
    bool Bind(Model& m, AssetLibrary& assets);
    // Overwrites `pose` (every node the clips drive, the root left as it is) with this frame's locomotion.
    bool Apply(const Model& m, IK::Pose& pose, const ThirdPersonMove& move, float dt);
    void Reset();
    // Diagnostics: the clips' measured ground speeds (forward walk, jog, run, crouch walk), model units/s.
    float GaitSpeed(int gait) const { return m_Speed[gait & 3]; }
    float PlayRate() const { return m_Rate; }

private:
    enum Gait { Walk, Jog, Run, Crouch, GaitCount };
    struct Rig {
        int Clips[GaitCount][8];    // per gait, per direction (F, FR, R, BR, B, BL, L, FL); -1 = none (falls back to Jog)
        int Idle = -1, CrouchIdle = -1, TurnL = -1, TurnR = -1, CrouchTurnL = -1, CrouchTurnR = -1, Air = -1;
        float Length[GaitCount][8] = {};
        int Root = -1;
        bool Ok = false;
    };
    std::map<const Model*, Rig> m_Rigs;
    float m_Speed[GaitCount] = {1.4f, 3.2f, 5.0f, 1.0f}; // measured at the first bind
    bool m_Measured = false;
    IK::Pose m_A, m_B, m_Acc;

    // Per character.
    float m_Phase = 0.0f;
    float m_Heading = 0.0f;  // eased travel heading, radians (0 forward, + right)
    float m_Moving = 0.0f, m_Air = 0.0f, m_Turn = 0.0f, m_CrouchW = 0.0f, m_GaitPos = 0.0f;
    float m_TurnTime = 0.0f, m_IdleTime = 0.0f;
    float m_Rate = 1.0f;
    bool m_Fresh = true;
};
