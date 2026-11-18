#pragma once

// Inertialization by dead blending (Holden, "Dead Blending", 2023; docs/MOTION_MATCHING_HANDOFF.md, Phase 3).
//
// When the motion matcher jumps to a new frame, the pose that was on screen doesn't snap: at the jump, the outgoing pose
// and its velocity (per bone angular velocity, the pelvis's linear velocity, measured from the last two output poses)
// are extrapolated forward with the velocity decaying (a critically damped decay to zero, DecayHalflife), and the output
// cross-fades from that extrapolated pose to the new animation over BlendTime with a smoothstep weight. Unlike offset
// inertialization, nothing is added to the new animation, so the result can't overshoot past either pose.
//
//   - a jump mid-blend starts from what's on screen (the last output), so chained jumps stay continuous
//   - the extrapolated angular velocity is clamped (MaxAngularVelocity), so a noisy last frame can't spin a bone
//
// Pure math on MotionPose (stream-bone rotations, the fingers', the pelvis translation): unit-tested on synthetic poses. The
// fingers blend like the other bones - a jump into a clip without finger data (the relaxed pose) eases into it too.

#include "MotionDatabase.h"

struct InertializationParams {
    float BlendTime = 0.2f;         // s, the cross-fade from the extrapolated pose to the new animation
    float DecayHalflife = 0.05f;    // s, the extrapolated velocity's decay
    float MaxAngularVelocity = 4.0f * 3.14159265f; // rad/s
    float MaxLinearVelocity = 6.0f;                // m/s
};

class Inertialization {
public:
    InertializationParams Params;

    // Start a blend: the output continues from what was shown (`last`, and `previous` one `dt` before it).
    void Transition(const MotionPose& previous, const MotionPose& last, float dt);
    // Advance by dt and blend into `target` (the new animation's pose this frame). Writes the output into `out`
    // (`target` itself when no blend is running).
    void Update(float dt, const MotionPose& target, MotionPose& out);
    bool Blending() const { return m_Active; }
    float BlendWeight() const; // 0 = all extrapolated source, 1 = all target
    void Reset() { m_Active = false; }

private:
    bool m_Active = false;
    float m_Time = 0.0f;
    // The stream bones then the fingers (MotionPose::Rot, then MotionPose::Fingers).
    std::vector<glm::quat> m_Rot;
    std::vector<glm::vec3> m_AngVel; // scaled angle-axis per second, in the bone's parent space
    size_t m_Bones = 0;
    glm::vec3 m_Pelvis{0.0f}, m_PelvisVel{0.0f};
};

namespace InertializationMath {
// Rotation <-> scaled angle-axis (the rotation vector: axis * angle).
glm::quat FromScaledAngleAxis(const glm::vec3& v);
glm::vec3 ToScaledAngleAxis(const glm::quat& q);
// The angular velocity (parent space) taking `a` to `b` in `dt`: b = FromScaledAngleAxis(w * dt) * a.
glm::vec3 AngularVelocity(const glm::quat& a, const glm::quat& b, float dt);
float Smoothstep(float x);
} // namespace InertializationMath
