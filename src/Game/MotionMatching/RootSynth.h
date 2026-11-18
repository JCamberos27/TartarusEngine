#pragma once

// RootSynth: generates the root trajectory a mocap take never recorded (the Quantum Stage pack's `root` bone stays at
// the origin; all travel is on the pelvis - docs/MOCAP_EXPORT_FINDINGS.md section 3).
//
//   position: the pelvis projected to the ground (y = 0), Gaussian-smoothed (PosSigma, default 0.10 s)
//   facing:   HipsWeight x the hips' forward + (1 - HipsWeight) x the shoulder line's forward, flattened to the ground,
//             as an unwrapped yaw, Gaussian-smoothed (YawSigma, default 0.25 s)
//   edges:    mirror-padded about the edge's trend line (MotionMath::MirrorPad): a take cut mid-stride keeps its speed
//             and turn rate to the edge, and its first and last frames are smoothed like the rest
//
// Everything is in the target model space: metres, Y up. "Right" vectors are left-to-right (thigh_l -> thigh_r,
// clavicle_l -> clavicle_r); forward = up x right. Yaw 0 faces +Z (MotionMath::YawOf).
//
// Validate() measures a generated root: yaw jitter, the root's facing against the hips', and foot slide.
// Pure math: no Model, no files (unit-tested on synthetic clips, UnitTests_MotionMatching.cpp).

#include <glm/glm.hpp>

#include <vector>

struct RootSynthParams {
    float PosSigma = 0.10f;   // seconds
    float YawSigma = 0.25f;   // seconds
    float HipsWeight = 0.5f;  // 0..1, the rest goes to the shoulder line
};

struct RootSynthInput {
    float Fps = 60.0f;
    std::vector<glm::vec3> Pelvis;        // per frame, model space
    std::vector<glm::vec3> HipsRight;     // thigh_r - thigh_l (any length)
    std::vector<glm::vec3> ShoulderRight; // clavicle_r - clavicle_l (any length); empty = hips only
};

struct RootTrack {
    std::vector<glm::vec3> Position; // on the ground (y = 0)
    std::vector<float> Yaw;          // unwrapped radians, 0 = +Z
    size_t Size() const { return Position.size(); }
};

struct RootValidation {
    float YawJitter = 0.0f;       // RMS of the root's yaw acceleration (deg/s^2) after removing the turn itself (see .cpp)
    float YawRateMax = 0.0f;      // fastest turn (deg/s)
    float FacingP95 = 0.0f;       // 95th percentile of |root facing - hips facing| (deg), frames moving > 0.5 m/s
    float FacingMax = 0.0f;
    float SpeedMax = 0.0f;        // fastest root speed (m/s)
    float StepMax = 0.0f;         // largest frame-to-frame root move (m): a teleport shows here
    float PathWobble = 0.0f;      // RMS of the root path's stride band (0.08 s minus 0.3 s Gaussian, cm): the sway left in it
    float FootSlide[2] = {0, 0};  // mean horizontal foot speed while in contact (cm/s), left / right
    int ContactFrames[2] = {0, 0};
};

namespace RootSynth {

// Forward on the ground from a left-to-right vector (up x right, flattened). Zero when `right` is vertical.
glm::vec3 ForwardFromRight(const glm::vec3& right);

// The raw (unsmoothed) facing per frame: the weighted, flattened blend of the hips' and shoulders' forward.
std::vector<float> RawYaw(const RootSynthInput& in, float hipsWeight);

RootTrack Generate(const RootSynthInput& in, const RootSynthParams& p);

// The root's model-space transform at frame i: translate(Position) * rotateY(Yaw - restYaw). `restYaw` is the facing of
// the rig's rest pose, so a body re-expressed under this root faces the way it does at rest.
glm::mat4 RootMatrix(const glm::vec3& position, float yaw, float restYaw);

// Foot contacts: a foot is planted while its height is under `maxHeight` and its horizontal speed under `maxSpeed`
// (m/s), with flicker of up to 2 frames removed.
std::vector<char> Contacts(const std::vector<glm::vec3>& foot, float fps, float maxHeight, float maxSpeed);

// `hipsRight`: thigh_l -> thigh_r per frame (for the facing check). `feet[k]` (optional, model space): each foot's
// position per frame, with `contacts[k]` its planted frames, for the slide.
RootValidation Validate(const RootTrack& root, float fps, const std::vector<glm::vec3>& hipsRight,
                        const std::vector<glm::vec3>* feet = nullptr, const std::vector<char>* contacts = nullptr);

} // namespace RootSynth
