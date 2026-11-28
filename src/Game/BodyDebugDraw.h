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
    // The torso's forward bend (deg, + = forward): pelvis to spine_05 off the vertical, in the body's frame - as the clips
    // (or motion matching) pose it, and as drawn after the spine's aim at the view; and the view's pitch (deg, + = up).
    float ClipBendDeg = 0.0f, TorsoBendDeg = 0.0f, ViewPitchDeg = 0.0f;
    // The same bend on the world twins (what every view but the player's camera shows) after their capped spine aim; the
    // pieces' bend when there are no twins.
    float TwinBendDeg = 0.0f;
    // The upper arms off straight down (deg, the worse of the two, body frame): as the spine's pitch at the view leaves them,
    // and as drawn (unarmed, Arm Hang turns them back to hang).
    float ArmDownAimDeg = 0.0f, ArmDownDeg = 0.0f;
    // Posture (Phase 4, the owner's play test 2), from the bones' orientations against the bind pose, body frame:
    // the chest's pitch (spine_05's up off the vertical, deg, + = forward) as the clip has it, before the aim at
    // the view (the "Stable" value) and as drawn; its roll (+ = toward the body's left); the head's pitch against
    // the chest (+ = chin down, - = thrown back).
    float ChestPitchClipDeg = 0.0f, ChestPitchStableDeg = 0.0f, ChestPitchDeg = 0.0f, ChestRollDeg = 0.0f, HeadPitchDeg = 0.0f;
    float HeadWorldPitchDeg = 0.0f; // the head's own pitch off the bind pose, body frame (+ = chin down)
    // The trunk's motion (deg, body frame, off the bind pose; FirstPersonBody::TrunkAngles): the pelvis's yaw / roll / pitch,
    // the chest's (spine_05), and the chest's against the pelvis - as the clip poses it (before the aim and layers) and as drawn.
    float TrunkClipDeg[9] = {}, TrunkDeg[9] = {};
    // Under motion matching, per stream bone: how far the drawn local rotation is off the matcher's (deg) - what everything
    // after the pose (the aim, the arms' layers, foot IK) did to it. Names in LayerBones (the stream's).
    std::vector<float> LayerDeg;
    const std::vector<std::string>* LayerBones = nullptr;
    // Each hand (l, r): its height over its shoulder (upper arm bone, m, + = above), its distance to the camera (m), and
    // whether it is in the view (within 35 deg of the camera's forward and 0.8 m).
    float HandOverShoulder[2] = {0, 0}, HandToEye[2] = {0, 0};
    float HandEyePush[2] = {0, 0}; // m: how far KeepHandsFromEye moved each hand out from the eye this frame (0: it didn't)
    bool HandInView[2] = {false, false};
    float FeetGap = 0.0f;       // m: left foot minus right along the body's left (< 0: the legs are crossed)
    float BodyYawRateDeg = 0.0f; // the body's heading's turn rate (deg/s)
    float MmAimFade = 1.0f;     // motion matching: the share of Spine Aim kept at this speed (FirstPersonBodyMmAimFade)
    float PelvisHeight = 0.0f, PelvisHeightClip = 0.0f; // m over the body's base: as drawn, and as the clip had it
    float FootWeight = 0.0f;      // 0..1: how much foot IK is on
    float FootOffset[2] = {0, 0}; // ground under each foot minus the capsule's (m)
    bool FootPlanted[2] = {false, false};
    float FootLock[2] = {0, 0};   // 0..1: how firmly each foot is pinned
    float StepOffset = 0.0f;      // m the body is off the capsule's height (stair easing)
    float ArmsWeight = 0.0f;      // 0..1: how much the body's arms follow the weapon rig
    float HeadHeight = 0.0f;      // m the clip's head bone over the body's base (before the chest's aim)
    float EyeSlack = 0.0f;        // m the eye is off the shoulders' motion (bounded by Eye Slack)
    glm::vec3 EyeApplied{0.0f};   // m the body moved the camera this frame, world (head height, bob, sway)
    glm::vec3 BodyEye{0.0f};      // world: the body's eye, before the procedural camera offsets (bob, lean, landing)
    float LegReach[2] = {0, 0};   // each leg's hip-to-foot distance foot IK asks for (before the reach clamp), of the leg's length
    float ClampPull[2] = {0, 0};  // m the reach clamp moved each foot off where the lock / ground wanted it
    float ReachFree[2] = {0, 0};  // LegReach without the foot lock's shift (the clip's foot on its ground only)
    float LockShift[2] = {0, 0};  // m the foot lock moves each foot sideways off its animation
    float AnimHeight[2] = {0, 0}; // m each animated foot stands over the body's base
    float BodyYawDeg = 0.0f;      // the body's heading (degrees)
    glm::vec3 DrawnFoot[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // world: each foot as drawn after foot IK
    float Reach[2] = {0, 0};          // each hand's distance from its shoulder, of the arm's length (before any shoulder move)
    float HandGap[2] = {0, 0};        // m each of the body's hands ends up from the rig's (off the gun when > ~1 cm)
    float PieceHandGap = 0.0f;        // m the worst hand of the other pieces (the arms' solve copied onto them) from the rig's
    float TwinHandGap[2] = {-1, -1};  // m each world twin's hand from the world gun's grip; -1 = no twins solved this frame
    float ShoulderGap[2] = {0, 0};    // m between each body shoulder and the arms rig's
    float RigBlade = 0.0f, ChestBlade = 0.0f; // degrees the rig's / the chest's shoulder line is turned off the view (+ = left forward)
    float ReachLean = 0.0f;           // degrees the chest leans toward an out-of-reach hand (Reach Lean Max)
    float ShrugShift[2] = {0, 0};     // m each shoulder shrugs toward an out-of-reach hand (Shrug Start / Max)
    float ArmSteadyShift[2] = {0, 0}; // m each shoulder is held off the gait's sway (Arm Steadiness: in full to Arm Steady Max, eased toward twice it)
    // How close each upper arm (shoulder to elbow bone line) comes to the eye - the arms rig's and the body's.
    // The mesh is a few cm thicker than the bone line.
    float EyeToUpperArmRig[2] = {0, 0}, EyeToUpperArmBody[2] = {0, 0};
    float NearPlane = 0.0f;
    float EyeBodyPush = 0.0f;
    int NeckRimPoints = 0;        // the torso's neck opening's rim vertices EyeBodyPush keeps out of the view
    float LookLimitDeg = -89.0f;  // the look-down limit (Player::MinPitch under motion matching)
    float CameraRide = 0.0f;      // FirstPersonBody::CameraRide (the procedural camera bob is off by this share)
    // FirstPersonBody's camera probe (overlay on): the piece the camera sees nearest the eye, how far ahead
    // (along the view) and the bone that mostly moves that spot; which pass draws it.
    std::string ProbePiece, ProbeBone;
    float ProbeDepth = 0.0f;
    bool ProbeViewModel = false;
    // ... and the camera pass's hides that fall inside the view (vertices past the near plane, in the frustum): each is a
    // cut in the body you can see into - the Near Hide (body / clothing), the collar mask, a Camera Hidden Bone's collapse
    // point, the hidden head (Head Bones, or a Hidden Part's vertices).
    int ProbeCutNear = 0, ProbeCutCollar = 0, ProbeCutBones = 0, ProbeCutHead = 0;
    // ... and (ProbeRequested only) a grid of view rays cast at the drawn body's triangles: how many there are, how many hit
    // the body, and how many hit a back face first - the inside of the body, seen through an opening ("see through my
    // body"); the piece and bone most of those hit. -1: not measured this frame.
    int ProbeRays = -1, ProbeRayHits = 0, ProbeInsideRays = 0;
    std::string ProbeInsideWhat;
    glm::vec2 ProbeInsideAt{0.0f}; // ... and where on the screen, on average (-1..1, + right / up)
    std::string LastTrigger;      // the last start / stop / crouch / jump trigger the body fired
    float LastTriggerAgo = 0.0f;  // seconds since
};

inline bool& EnabledFlag() { static bool v = false; return v; }
inline std::vector<float>& Verts() { static std::vector<float> v; return v; } // 7 floats a vertex: pos.xyz, rgba
inline Snapshot& Info() { static Snapshot s; return s; }

// The Scene viewport's overlay toggle (set every frame from the editor settings while playing).
inline void SetEnabled(bool on) { EnabledFlag() = on; if (!on) Verts().clear(); }
inline bool Enabled() { return EnabledFlag(); }
// The camera probe alone, without the overlay (--npc-test measures with it): costs ~4.5 ms a frame while on.
inline bool& ProbeRequested() { static bool v = false; return v; }
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
