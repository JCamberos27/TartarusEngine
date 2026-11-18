#pragma once

// MotionDatabase: the motion matching runtime's view of one bake (docs/MOTION_MATCHING_HANDOFF.md, Phase 3) - the pose
// stream (.mmanim, MotionClipStream) and the features and labels (.mmdb, MotionFeatureSet), loaded together, checked
// against each other, and prepared for MotionSearch:
//   - the 27 features normalized per group: (x - mean[d]) / groupStd, where groupStd is the mean of the group's
//     per-feature standard deviations (a group's dimensions keep their relative scale, and no one group outweighs
//     another just by having more dimensions or bigger units). The group weights are applied at search time.
//     The features stay as the file's 16-bit integers (MotionFeatureSet): normalized = q * QA(d) + QB(d), one affine map
//     per dimension, so nothing is re-quantized and only the 16-bit copy is held. MotionSearch searches the integers.
//   - the AABBs of the features (16-bit) over blocks of 16 and of 64 frames (Holden's "bounding volume hierarchy" for
//     motion matching), with each block's OR and AND of the frame flags, its gait range and its styles, so a search skips
//     a whole block when its lower bound can't beat the best so far or no frame in it can pass the label filter.
//   - which clip each frame is in.
// Frames flagged Excluded (prone) or NoSearch (the last 60 frames of a take) are never matched (MotionSearch).
//
// Pure: no Model, no GL. Load() reads the two files; Adopt() takes them already built (the unit tests' synthetic takes).

#include "MotionClipStream.h"
#include "MotionFeatures.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace MotionFeature {
// Feature groups, in MotionFeatureSet's layout (MotionFeatures.h).
enum Group : int { FootPos = 0, FootVel, PelvisVel, TrajPos, TrajDir, GroupCount };
inline constexpr int kGroupFirst[GroupCount + 1] = {0, 6, 12, 15, 21, 27};
const char* GroupName(int g);
inline int GroupOf(int dim) {
    for (int g = 0; g < GroupCount; ++g)
        if (dim < kGroupFirst[g + 1]) return g;
    return GroupCount - 1;
}
} // namespace MotionFeature

// One pose of the stream: each stream bone's local rotation and the pelvis translation (source scale, under the root), and
// the finger bones' (MotionClipStream::Statics order: the clip's track, or the relaxed pose).
struct MotionPose {
    std::vector<glm::quat> Rot;
    std::vector<glm::quat> Fingers;
    glm::vec3 Pelvis{0.0f};
};

class MotionDatabase {
public:
    static constexpr int kDims = MotionFeatureSet::kDims;
    static constexpr int kSmallBox = 16;
    static constexpr int kLargeBox = 64;

    struct Box {
        std::int16_t Min[kDims], Max[kDims]; // the features' 16-bit values (MotionFeatureSet::Quantized)
        std::uint8_t FlagOr = 0, FlagAnd = 0xFF;
        std::uint8_t GaitMin = 255, GaitMax = 0;
        std::uint8_t Styles = 0; // bit (1 << MotionLabel::Style) per style in the block
    };

    // Loads `<base>.mmanim` and `<base>.mmdb`. False (with `error`) when either is missing or corrupt, or they disagree.
    bool Load(const std::string& base, std::string* error = nullptr);
    // Takes a stream and a feature set already in memory (the unit tests); the same checks as Load.
    bool Adopt(MotionClipStream&& stream, MotionFeatureSet&& features, std::string* error = nullptr);
    bool Loaded() const { return m_Frames > 0; }
    double LoadSeconds() const { return m_LoadSeconds; }

    const MotionClipStream& Stream() const { return m_Stream; }
    const MotionFeatureSet& Features() const { return m_Feat; }
    std::uint32_t FrameCount() const { return m_Frames; }
    float Fps() const { return m_Stream.Fps(); }
    int ClipOf(std::uint32_t frame) const { return m_ClipOf[frame]; }
    const MotionClipStream::Clip& Clip(int c) const { return m_Stream.Clips()[(size_t)c]; }
    // The last frame of `frame`'s clip.
    std::uint32_t ClipEnd(std::uint32_t frame) const;
    std::uint8_t Flags(std::uint32_t frame) const { return m_Feat.Flags(frame); }
    std::uint8_t Gait(std::uint32_t frame) const { return m_Feat.Gait(frame); }
    std::uint8_t Style(std::uint32_t frame) const { return m_Feat.Style(frame); }
    // How many frames from `frame` on (itself included, at most 65535) its take keeps the labels a filter can refuse
    // (Crouch, Airborne, Excluded, NoSearch) as they are here: a candidate with a short runway is a dead end - it plays
    // into a refused frame at once, forcing another search (MotionSearchParams::MinRunway).
    static constexpr std::uint8_t kRunwayMask = 1 | 4 | 8 | 16; // MotionLabel::Crouch | Airborne | Excluded | NoSearch
    std::uint16_t Runway(std::uint32_t frame) const { return m_Runway[frame]; }
    // Glitches in the capture: a frame where a stream bone turns more than kGlitchDegrees to the next (at 60 fps; the pack's
    // fastest real motion, its runs' p99.9, is ~30) - twist flips up to 180 degrees on the thighs and feet, a forearm
    // swinging 60 degrees in a frame. Build marks them and kGlitchPad frames either side Excluded: never matched, and
    // playing into one forces a search before it is shown.
    static constexpr float kGlitchDegrees = 35.0f;
    static constexpr int kGlitchPad = 3;
    // Marks more frames Excluded (a host's own pose rules, e.g. the player's hands-up frames) and rebuilds the runway and
    // the boxes. Returns how many frames it newly excluded.
    std::uint32_t ExcludeFrames(const std::vector<char>& bad);
    std::uint32_t GlitchFrames() const { return m_Glitches; } // frames Build excluded for glitches (the pads included)
    // The way down to prone and back up: the baked Excluded (prone) frames are padded by this many seconds either side.
    // Unpadded, a crouched frame on its way down to prone won the crouch search, and its pose (feet 0.7 m behind, the
    // pelvis dropping) kept matching more of the same until the body was stuck there (Phase 4, the crouch legs).
    static constexpr float kProneApproachSeconds = 1.0f;
    std::uint32_t ProneApproachFrames() const { return m_ProneApproach; }

    // A frame's normalized features (kDims floats) and its raw ones, decoded from the 16-bit values; the normalization.
    std::array<float, kDims> Normalized(std::uint32_t frame) const;
    std::array<float, kDims> Raw(std::uint32_t frame) const;
    void Normalize(const float* raw, float* out) const;
    void Denormalize(const float* norm, float* out) const;
    float Mean(int d) const { return m_Mean[(size_t)d]; }
    float Scale(int d) const { return m_Scale[(size_t)d]; } // the group's std that dimension d is divided by
    // The 16-bit features (kDims per frame) and their map to the normalized space: normalized = q * QA(d) + QB(d).
    const std::int16_t* Quantized(std::uint32_t frame) const { return m_Feat.Quantized(frame); }
    float QA(int d) const { return m_QA[(size_t)d]; }
    float QB(int d) const { return m_QB[(size_t)d]; }

    const std::vector<Box>& SmallBoxes() const { return m_Small; }
    const std::vector<Box>& LargeBoxes() const { return m_Large; }

    // Seconds into the clip of a global frame, and back.
    float SecondsInClip(std::uint32_t frame) const;
    // The pose at global frame `frame` plus `frac` (0..1) of the way to the next frame of the same clip (nlerp).
    void SamplePose(std::uint32_t frame, float frac, MotionPose& out) const;

    // The trajectory times (seconds) the features sample: TrajFrames / fps (0.33, 0.67, 1.0 s on the bake).
    std::array<float, 3> TrajSeconds() const;

    // Bytes the runtime holds: the pose stream, and the features (normalized values, labels, boxes, clip map).
    size_t Bytes() const { return PoseBytes() + FeatureBytes(); }
    size_t PoseBytes() const { return m_Stream.PoseBytes(); }
    size_t FeatureBytes() const;

private:
    bool Build(std::string* error);
    void Index(); // the runway and the boxes, from the flags

    MotionClipStream m_Stream;
    MotionFeatureSet m_Feat;
    std::uint32_t m_Frames = 0;
    std::vector<int> m_ClipOf;
    std::vector<std::uint16_t> m_Runway;
    std::uint32_t m_Glitches = 0, m_ProneApproach = 0;
    std::vector<float> m_Mean, m_Scale, m_QA, m_QB;
    std::vector<Box> m_Small, m_Large;
    double m_LoadSeconds = 0.0;
};
