#pragma once
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Root motion: the travel a clip authors on its root bone, taken out of the pose and handed to
// the object instead, so a walk moves the character rather than dragging its mesh away from it.
//
// Everything here works in the model's space (what the entity's transform multiplies), on the
// root bone's model-space transform X(t) at a clip time t:
//   F(t)   the root's "motion frame": its ground-plane position (plus height when Vertical is
//          on) and its heading (when Rotation is on).
//   MT(t)  = F(t) * inverse(F(0)): how far the clip has carried the character since its start.
//   In place: X'(t) = inverse(MT(t)) * X(t) - the root stays where the clip's first frame had it,
//          still bobbing / leaning / swaying on whatever the motion frame leaves out.
//   Delta over [t0, t1] = inverse(MT(t0)) * MT(t1): what to apply to the object so that
//          object * in-place pose == the clip as authored.

struct RootMotionSettings {
    bool Rotation = true;   // heading (yaw about +Y) moves the object; off = the turn stays in the pose
    bool Vertical = false;  // height moves the object too; off = hops and crouches stay in the pose
};

// A rigid motion in model space: turn by Yaw (radians, about +Y) and translate. Applied to an
// object as object * Matrix(): the translation is in the object's own (scaled) frame.
struct RootMotionDelta {
    glm::vec3 Translation{0.0f};
    float Yaw = 0.0f;

    glm::mat4 Matrix() const;
    // This motion followed by `next` (next expressed in the frame this one ends in).
    RootMotionDelta Then(const RootMotionDelta& next) const;
    bool IsZero() const { return Yaw == 0.0f && Translation == glm::vec3(0.0f); }
};

// Weighted mix of several deltas (the entries of a crossfade or blend tree). Weights need not sum
// to 1; they are normalised. An empty or all-zero set gives no motion.
struct RootMotionMix {
    glm::vec3 Translation{0.0f};
    float Yaw = 0.0f, Weight = 0.0f;
    void Add(const RootMotionDelta& d, float w);
    RootMotionDelta Result() const;
};

// Heading of a rotation about +Y (swing-twist), radians.
float RootMotionYaw(const glm::quat& q);

// The motion frame F of a model-space root transform.
glm::mat4 RootMotionFrame(const glm::mat4& rootModel, const RootMotionSettings& s);

// X'(t): the root at `rootAtT` with the motion since `rootAtStart` (the clip's time 0) removed.
glm::mat4 RootMotionInPlace(const glm::mat4& rootAtT, const glm::mat4& rootAtStart, const RootMotionSettings& s);

// The delta over clip time [t0, t1] seconds. `sampleRoot(t)` returns the root's model-space
// transform at clip time t in [0, length]. Looping clips wrap (each pass carries on from where
// the last ended, so a looping walk keeps walking); others clamp to [0, length]. t1 < t0 (a clip
// played backwards) gives the reverse motion.
RootMotionDelta RootMotionBetween(const std::function<glm::mat4(float)>& sampleRoot, float t0, float t1,
                                  float length, bool loop, const RootMotionSettings& s);
