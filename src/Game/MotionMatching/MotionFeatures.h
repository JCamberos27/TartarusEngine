#pragma once

// MotionFeatures: the per-frame labels and matching features of the baked takes (.mmdb), written by MotionBake next to
// the pose stream (.mmanim, MotionClipStream). Phase 3's MotionDatabase loads it and searches it.
//
// Labels (per frame, computed - docs/MOTION_MATCHING_HANDOFF.md decision 4):
//   gait     from the root's speed (Idle / Walk / Jog / Run, thresholds in the .mmconfig)
//   Crouch   the pelvis under CrouchRatio x its standing height, slower than a jog (a run's lean drops it as low)
//   Strafe   travelling more than StrafeAngle off the root's facing (backwards included)
//   Airborne the lower ankle above AirFootHeight and the pelvis AirPelvisRise over standing, AirMinFrames or more
//   Excluded prone (the pelvis under ProneHeight): never matched
//   NoSearch the last TrajFrames[2] frames of a take (the future trajectory runs off its end)
//   ContactL / ContactR the foot is planted (RootSynth::Contacts)
//
// Features (27 floats, in the frame's own root space - RootSynth::RootMatrix's local frame, so the body faces the rig's
// rest facing - metres and m/s):
//   0-2 left foot position, 3-5 right foot position, 6-8 left foot velocity, 9-11 right foot velocity,
//   12-14 pelvis velocity, 15-20 the root's position at +TrajFrames[0..2] frames (x, z each),
//   21-26 the root's facing at +TrajFrames[0..2] frames (x, z each, unit).
// The file also stores each feature's mean and standard deviation over the database (for normalized search).
//
// File layout (little endian), version 1:
//   char[4] "TMMD", u32 version, u32 frames, u32 dims, u32 clips, u32 trajFrames[3],
//   dims x { str name, f32 mean, f32 std }, clips x { str name, u32 first, u32 count },
//   frames x dims x f32, frames x u8 gait, frames x u8 flags

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct RootTrack;

namespace MotionLabel {
enum Gait : std::uint8_t { Idle = 0, Walk = 1, Jog = 2, Run = 3 };
enum Flag : std::uint8_t {
    Crouch = 1,
    Strafe = 2,
    Airborne = 4,
    Excluded = 8,
    NoSearch = 16,
    ContactL = 32,
    ContactR = 64,
};
const char* GaitName(int g);
} // namespace MotionLabel

struct MotionFeatureConfig {
    int TrajFrames[3] = {20, 40, 60};
    float GaitSpeeds[3] = {0.3f, 1.6f, 2.9f}; // m/s: walk from, jog from, run from (the pack: walk ~1.25, jog ~2.0, run 4-5)
    float CrouchRatio = 0.84f;                 // of the standing pelvis height
    float ProneHeight = 0.20f;                 // m (the deepest crouches sit at 0.30)
    float StrafeAngleDeg = 50.0f;
    float StrafeMinSpeed = 0.5f;               // m/s
    float AirFootHeight = 0.18f;               // m, the lower foot (ankle)
    float AirPelvisRise = 0.02f;               // m over StandingPelvis
    int AirMinFrames = 4;
    float ContactHeight = 0.15f;               // m, ankle height while planted
    float ContactSpeed = 0.6f;                 // m/s
    float StandingPelvis = 0.96f;              // m, the source rig's pelvis at rest
};

// One take, as MotionBake samples it (model space, before the root is taken out).
struct MotionClipFrames {
    float Fps = 60.0f;
    float RestYaw = 0.0f;
    std::vector<glm::vec3> Pelvis, Foot[2]; // ankles (foot_l / foot_r)
};

class MotionFeatureSet {
public:
    static constexpr std::uint32_t kVersion = 1;
    static constexpr int kDims = 27;
    static const char* const* Names(); // kDims names

    struct Clip {
        std::string Name;
        std::uint32_t First = 0, Count = 0;
    };

    // Appends one take's labels and features (MotionBake): `root` from RootSynth::Generate on the same frames.
    void AddClip(const std::string& name, const MotionClipFrames& frames, const RootTrack& root,
                 const MotionFeatureConfig& cfg);
    // The mean and standard deviation of each feature over everything added (call once, after the last clip).
    void Finish();

    bool Save(const std::string& path, std::string* error = nullptr) const;
    bool Load(const std::string& path, std::string* error = nullptr);

    std::uint32_t FrameCount() const { return (std::uint32_t)m_Gait.size(); }
    const float* Features(std::uint32_t frame) const { return m_Data.data() + (size_t)frame * kDims; }
    std::uint8_t Gait(std::uint32_t frame) const { return m_Gait[frame]; }
    std::uint8_t Flags(std::uint32_t frame) const { return m_Flags[frame]; }
    const std::vector<Clip>& Clips() const { return m_Clips; }
    const std::vector<float>& Mean() const { return m_Mean; }
    const std::vector<float>& Std() const { return m_Std; }
    size_t Bytes() const { return m_Data.size() * sizeof(float) + m_Gait.size() + m_Flags.size(); }
    // Frees the feature values (labels, clips and statistics stay): MotionDatabase keeps its own normalized copy, so the
    // runtime holds the features once (the budget). Features() must not be called after.
    void ReleaseFeatureData() { std::vector<float>().swap(m_Data); }
    int TrajFrames[3] = {20, 40, 60};

private:
    std::vector<Clip> m_Clips;
    std::vector<float> m_Data, m_Mean, m_Std;
    std::vector<std::uint8_t> m_Gait, m_Flags;
};
