#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <string>
#include <vector>

// What the true-first-person body shows about itself while it plays: world-space debug lines for the
// Scene viewport (the Gizmos > "Player body" toggle) and a snapshot of its state for the First Person
// Body Inspector's live readout. FirstPersonBody fills both every frame; the lines are only built while
// the toggle is on, the snapshot is a handful of floats and always kept.
namespace BodyDebug {

struct Snapshot {
    bool Valid = false;
    std::string AnimatorState;
    float StateTime = 0.0f;       // normalized time in the driver's current state
    bool Turning = false;
    float ViewOffsetDeg = 0.0f;   // the view's angle off the body's heading (+ = to the left)
    float TwistDeg = 0.0f;        // the chest's twist toward the view
    bool Still = false;
    float IdleTime = 0.0f, MoveTime = 0.0f;
    float MoveSpeed = 0.0f;       // |MoveX, MoveY|
    float RootSpeed = 0.0f;       // m/s the clips carry the capsule
    float FootWeight = 0.0f;      // 0..1: how much foot IK is on
    float FootOffset[2] = {0, 0}; // ground under each foot minus the capsule's (m)
    bool FootPlanted[2] = {false, false};
    float FootLock[2] = {0, 0};   // 0..1: how firmly each foot is pinned
    float StepOffset = 0.0f;      // m the body is off the capsule's height (stair easing)
    float ArmsWeight = 0.0f;      // 0..1: how much the body's arms follow the weapon rig
    float WorldGunShift = 0.0f;   // m the world gun sits off the first-person one (split poses)
    float EyeSlack = 0.0f;        // m the eye is off the shoulders' motion (bounded by Eye Slack)
    float Reach[2] = {0, 0};          // each hand's distance from its shoulder, of the arm's length (before any shoulder move)
    float HandGap[2] = {0, 0};        // m each of the body's hands ends up from the rig's (off the gun when > ~1 cm)
    float ShoulderGap[2] = {0, 0};    // m between each body shoulder and the arms rig's
    float RigBlade = 0.0f, ChestBlade = 0.0f; // degrees the rig's / the chest's shoulder line is turned off the view (+ = left forward)
    float ReachLean = 0.0f;           // degrees the chest leans toward an out-of-reach hand (Reach Lean Max)
    float ShrugShift[2] = {0, 0};     // m each shoulder shrugs toward an out-of-reach hand (Shrug Start / Max)
    float ArmSteadyShift[2] = {0, 0}; // m each shoulder is held off the gait's sway (Arm Steadiness, bounded by Arm Steady Max)
    // How close each upper arm (shoulder to elbow bone line) comes to the eye - the arms rig's and the body's.
    // The mesh is a few cm thicker than the bone line.
    float EyeToUpperArmRig[2] = {0, 0}, EyeToUpperArmBody[2] = {0, 0};
    float NearPlane = 0.0f;
    // FirstPersonBody's camera probe (overlay on): the piece the camera sees nearest the eye, how far ahead
    // (along the view) and the bone that mostly moves that spot; which pass draws it.
    std::string ProbePiece, ProbeBone;
    float ProbeDepth = 0.0f;
    bool ProbeViewModel = false;
    std::string LastTrigger;      // the last start / stop / crouch / jump trigger the body fired
    float LastTriggerAgo = 0.0f;  // seconds since
};

inline bool& EnabledFlag() { static bool v = false; return v; }
inline std::vector<float>& Verts() { static std::vector<float> v; return v; } // 7 floats a vertex: pos.xyz, rgba
inline Snapshot& Info() { static Snapshot s; return s; }

// The Scene viewport's overlay toggle (set every frame from the editor settings while playing).
inline void SetEnabled(bool on) { EnabledFlag() = on; if (!on) Verts().clear(); }
inline bool Enabled() { return EnabledFlag(); }
inline void Clear() { Verts().clear(); }

inline void Line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& c) {
    if (!Enabled()) return;
    auto& v = Verts();
    for (const glm::vec3* p : {&a, &b}) {
        v.push_back(p->x); v.push_back(p->y); v.push_back(p->z);
        v.push_back(c.r); v.push_back(c.g); v.push_back(c.b); v.push_back(c.a);
    }
}
inline void Cross(const glm::vec3& p, float size, const glm::vec4& c) {
    Line(p - glm::vec3(size, 0, 0), p + glm::vec3(size, 0, 0), c);
    Line(p - glm::vec3(0, size, 0), p + glm::vec3(0, size, 0), c);
    Line(p - glm::vec3(0, 0, size), p + glm::vec3(0, 0, size), c);
}
inline void Arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& c) {
    Line(from, to, c);
    const glm::vec3 d = to - from;
    const float len = glm::length(d);
    if (len < 1e-4f) return;
    const glm::vec3 dir = d / len;
    glm::vec3 side = glm::cross(dir, glm::vec3(0, 1, 0));
    side = glm::length(side) < 1e-4f ? glm::vec3(1, 0, 0) : glm::normalize(side);
    const float h = std::min(0.12f, len * 0.3f);
    Line(to, to - dir * h + side * h * 0.5f, c);
    Line(to, to - dir * h - side * h * 0.5f, c);
}

} // namespace BodyDebug
