#pragma once

// MotionFeatures: the per-frame labels and matching features of the baked takes (.mmdb), written by MotionBake next to
// the pose stream (.mmanim, MotionClipStream). Phase 3's MotionDatabase loads it and searches it.
//
// Labels (per frame, computed - docs/MOTION_MATCHING_HANDOFF.md decision 4):
//   gait     from the root's speed (Idle / Walk / Jog / Run, thresholds in the .mmconfig)
//   style    what the actor was doing (MotionLabel::Style), from the take's intent (MotionClipIntent: its tags): a take of
//            one style is that style throughout; a take holding several (a run take's walk-backs, the accelerations, the
//            transitions) takes each frame's from its stride-averaged speed, the nearest of its styles' cruise speeds
//            (StyleSpeeds, measured: docs/mm_audit.md). By speed alone the jog takes were mostly "walk" - their strafes
//            and turns move at walk speeds - and Shift sideways matched walk strafes (a speed walk).
//   Crouch   the pelvis under CrouchRatio x its standing height, slower than a jog (a run's lean drops it as low)
//   Strafe   travelling more than StrafeAngle off the root's facing (backwards included)
//   Airborne a jump take's flight: the lower ankle above AirFootHeight and the pelvis AirPelvisRise over standing,
//            AirMinFrames or more. Only in takes whose intent is a jump: the jog's own flight phases pass the same test
//            (4-12 frames, the pelvis up to 1.04 m - the pack's small hops are 5-13 frames at 1.04-1.06 m; nothing in the
//            motion tells them apart), and they are part of the stride, not a jump (docs/mm_audit.md).
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
// Storage: each feature as a 16-bit integer, x = q * qScale[d] + qOffset[d] (the dimension's range over the database in
// 65534 steps: under 0.0001 of its standard deviation), on disk and in memory - MotionDatabase searches them as they are.
//
// File layout (little endian), version 2:
//   char[4] "TMMD", u32 version, u32 frames, u32 dims, u32 clips, u32 trajFrames[3],
//   dims x { str name, f32 mean, f32 std, f32 qScale, f32 qOffset }, clips x { str name, u32 first, u32 count },
//   frames x dims x i16, frames x u8 gait, frames x u8 flags, frames x u8 style

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
    // Runtime only (MotionDatabase::Build, from the take's tags - never baked): a frame of an idle take (tag "idle"). Standing
    // still the player searches only these: over the whole database the stillest 1 s was a jump take's stance before its
    // step, matched again every 2 s (Phase 4, play test 4).
    IdleTake = 128,
};
const char* GaitName(int g);
// What the actor was doing (the take's intent). Stand: not a locomotion style (idles, turns and steps on the spot, jumps,
// repositions) - a search asking for a style doesn't charge it.
enum Style : std::uint8_t { StyleStand = 0, StyleStroll, StyleWalk, StyleJog, StyleRun, StyleCrouch, StyleCount };
const char* StyleName(int s);
int StyleFromName(const std::string& name); // -1: none
} // namespace MotionLabel

// What a take was shot for (MotionBake, from its tags): its styles and whether its flights are jumps.
struct MotionClipIntent {
    std::vector<int> Styles; // MotionLabel::Style; one: every frame; several: per frame by speed; none: Stand
    bool Jump = false;
    // From a take's ';'-separated tags (tools/mm/takes.csv): "style=walk+jog+run" names the styles; without it the group
    // (the first tag) does: stroll, walk, jog and crouch are one style each, run is walk + run (the run takes walk back
    // between their sprints), anything else Stand. "jump" makes the flights jumps.
    static MotionClipIntent FromTags(const std::string& tags);
};

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
    // m/s, the styles' cruise speeds (docs/mm_audit.md, steady forward p50; run: its dashes' plateau): a mixed take's
    // frames take the nearest of its styles'.
    float StyleSpeeds[MotionLabel::StyleCount] = {0.0f, 0.69f, 1.17f, 1.87f, 3.9f, 0.74f};
    float StyleWindowSeconds = 1.0f;           // the speed is averaged over this (a stride), so a step's sway doesn't flip it
};

// One take, as MotionBake samples it (model space, before the root is taken out).
struct MotionClipFrames {
    float Fps = 60.0f;
    float RestYaw = 0.0f;
    std::vector<glm::vec3> Pelvis, Foot[2]; // ankles (foot_l / foot_r)
};

class MotionFeatureSet {
public:
    static constexpr std::uint32_t kVersion = 2;
    static constexpr int kDims = 27;
    static const char* const* Names(); // kDims names

    struct Clip {
        std::string Name;
        std::uint32_t First = 0, Count = 0;
    };

    // Appends one take's labels and features (MotionBake): `root` from RootSynth::Generate on the same frames.
    void AddClip(const std::string& name, const MotionClipFrames& frames, const RootTrack& root,
                 const MotionFeatureConfig& cfg, const MotionClipIntent& intent);
    // The mean and standard deviation of each feature over everything added, then the features quantized (call once, after
    // the last clip). The float values are freed.
    void Finish();

    bool Save(const std::string& path, std::string* error = nullptr) const;
    bool Load(const std::string& path, std::string* error = nullptr);

    std::uint32_t FrameCount() const { return (std::uint32_t)m_Gait.size(); }
    // A frame's features (kDims floats): exact before Finish, the quantized values decoded after.
    void Decode(std::uint32_t frame, float* out) const;
    bool Quantized() const { return !m_Q.empty(); }
    const std::int16_t* Quantized(std::uint32_t frame) const { return m_Q.data() + (size_t)frame * kDims; }
    float QScale(int d) const { return m_QScale[(size_t)d]; }
    float QOffset(int d) const { return m_QOffset[(size_t)d]; }
    // The worst quantization error of each dimension over the database (Finish; empty after Load), in its own units.
    const std::vector<float>& QError() const { return m_QError; }
    std::uint8_t Gait(std::uint32_t frame) const { return m_Gait[frame]; }
    std::uint8_t Style(std::uint32_t frame) const { return m_Style[frame]; }
    std::uint8_t Flags(std::uint32_t frame) const { return m_Flags[frame]; }
    void AddFlags(std::uint32_t frame, std::uint8_t flags) { m_Flags[frame] |= flags; } // (MotionDatabase: glitches excluded)
    const std::vector<Clip>& Clips() const { return m_Clips; }
    const std::vector<float>& Mean() const { return m_Mean; }
    const std::vector<float>& Std() const { return m_Std; }
    size_t Bytes() const {
        return m_Data.size() * sizeof(float) + m_Q.size() * sizeof(std::int16_t) + m_Gait.size() + m_Flags.size() + m_Style.size();
    }
    int TrajFrames[3] = {20, 40, 60};

private:
    std::vector<Clip> m_Clips;
    std::vector<float> m_Data, m_Mean, m_Std, m_QScale, m_QOffset, m_QError;
    std::vector<std::int16_t> m_Q;
    std::vector<std::uint8_t> m_Gait, m_Flags, m_Style;
};
