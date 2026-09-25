#pragma once

#include "RootMotion.h"

#include <string>
#include <vector>

class Model;

// What a clip does, measured on a rig: the numbers that used to come from headless Blender scripts
// (travel speed, foot contacts, stride length, loop seam) so blend thresholds, movement speeds and
// stop / start transition offsets can be set from the editor.
namespace ClipAnalysis {

inline constexpr int kSamples = 64; // evenly spaced over [0, length]

struct Foot {
    std::string Bone;
    bool Found = false;
    // Contact: the foot is near its lowest height in the clip. Normalised start times (0..1) of each
    // contact, wrapping the loop.
    std::vector<float> ContactStarts;
    float ContactFraction = 0.0f; // share of the clip the foot spends planted
    float Stride = 0.0f;          // distance the character covers between two plants of this foot (0 = unknown)
};

struct Result {
    bool Valid = false;
    float Length = 0.0f;          // seconds
    float Distance = 0.0f;        // ground travel over one pass (m)
    float Speed = 0.0f;           // Distance / Length
    float YawDegrees = 0.0f;      // heading change over one pass
    float VerticalDelta = 0.0f;   // net root height change over one pass
    // How far the last frame is from the first, relative to the root, for feet / hands / head: the
    // worst one, in metres. Large = a visible pop when the clip loops.
    float LoopSeam = 0.0f;
    std::string LoopSeamBone;
    Foot Feet[2];
    std::vector<float> SpeedProfile; // ground speed at each sample (m/s), for a plot
};

// `footBones` / `seamBones` are node names. Returns Valid = false when the clip or root node is missing.
Result Analyze(const Model& model, int clip, int rootNode, const RootMotionSettings& rm,
               const std::string footBones[2], const std::vector<std::string>& seamBones);

} // namespace ClipAnalysis
