#pragma once

// MotionBake: `--bake-motion <file.mmconfig> [--match <text>] [--out <prefix>]` (docs/MOTION_MATCHING_HANDOFF.md,
// Phase 2). Reads the untouched mocap takes through the engine's own assimp import (Model, clip-only), generates each
// take's root (RootSynth), keeps the configured core bones, trims, quantizes, and writes:
//   <output>.mmanim  the pose stream (MotionClipStream)
//   <output>.mmdb    labels and matching features (MotionFeatureSet)
// plus a report (work/mm_bake/<name>.report.json) with every take's validation numbers and the rig diff.
// It fails (exit 1) on a budget overrun (pose memory, feature memory, load time) or a take that fails to import.
//
// The .mmconfig (JSON; every path project-relative, so "../tools/..." reaches the repo's tools folder):
//   source        { "env": "TARTARUS_MM_SOURCE", "anims": "anims", "rig": "rig/SKM_Manny.fbx" }
//                 the pack's folder only ever comes from that environment variable (Core/Env.h)
//   takes         "../tools/mm/takes.csv" (tools/mm/gen_takes.py): which takes, their trims and tags
//   targetRig     the rig the stream plays on (its `root` frame and rest facing; the rig diff's other side)
//   output        "assets/Animations/MotionMatching/Manny" (no extension)
//   fps, workers, bones (the streamed core bones), staticBones (name prefixes: one relaxed pose from staticPoseFrom),
//   rootSynth { default {posSigma, yawSigma, hipsWeight}, byTag { tag: {...} } }, labels {...},
//   budgets { poseMB, featureMB, loadSeconds }, validation { facingP95Deg, yawJitter, rigLengthPct }

#include "MotionFeatures.h"
#include "RootSynth.h"

#include "Animation.h"

#include <map>
#include <string>
#include <vector>

class Model;

struct MotionBakeConfig {
    std::string Path;          // the .mmconfig itself
    std::string SourceEnv = "TARTARUS_MM_SOURCE";
    std::string SourceAnims = "anims", SourceRig = "rig/SKM_Manny.fbx";
    std::string Takes, TargetRig, Output;
    float Fps = 60.0f;
    int Workers = 4;
    std::vector<std::string> Bones;
    std::vector<std::string> StaticBonePrefixes;
    std::string StaticPoseFrom;
    RootSynthParams RootDefault;
    std::map<std::string, RootSynthParams> RootByTag;
    MotionFeatureConfig Labels;
    float BudgetPoseMB = 150.0f, BudgetFeatureMB = 40.0f, BudgetLoadSeconds = 2.0f;
    float FacingP95Deg = 20.0f, YawJitterMax = 400.0f, RigLengthPct = 2.0f, ReconstructCm = 1.0f;

    bool Load(const std::string& path, std::string& error);
    RootSynthParams ParamsFor(const std::vector<std::string>& tags) const;
};

// One row of tools/mm/takes.csv.
struct MotionTake {
    std::string Name;
    std::string Source; // the pack's take (its .fbx) when the row is another view of it (a mirror); empty: Name
    const std::string& File() const { return Source.empty() ? Name : Source; }
    bool Mirrored() const { return HasTag("mirrored"); }
    std::string Group, Pattern;
    float Seconds = 0.0f;
    bool Include = false;
    float TrimStart = 0.0f, TrimEnd = 0.0f; // seconds into the take; TrimEnd <= 0 = the take's end
    std::vector<std::string> Tags;
    bool HasTag(const std::string& t) const;
    std::string TagString() const;
};

namespace MotionBake {

// The pack's folder (the config's source env var), or "" when unset or missing.
std::string SourceDir(const MotionBakeConfig& cfg);

bool ReadTakes(const std::string& csvPath, std::vector<MotionTake>& out, std::string& error);

// Rest pose and bone length diff of two rigs over `bones`, in each rig's `root` frame.
struct BoneDiff {
    std::string Name;
    bool InSource = false, InTarget = false;
    float LengthSource = 0.0f, LengthTarget = 0.0f; // the bone's offset from its parent (m)
    float LengthPct = 0.0f;                          // |target - source| / source, percent
    float RestAngleDeg = 0.0f;                       // the local rest rotations' difference
    float PositionCm = 0.0f;                         // the rest positions' distance in the root frame
};
std::vector<BoneDiff> RigDiff(const Model& source, const Model& target, const std::vector<std::string>& bones);

// Every node's transform relative to the `root` node (the frame both rigs share below their import's axis fix), from a
// local pose. Nodes above `root` get its inverse chain.
void RootSpaceGlobals(const Model& model, const std::vector<LocalTRS>& pose, int rootNode,
                      std::vector<glm::mat4>& out);

// Every node's transform from a local pose over a parent table (parents before children not required).
void Globals(const std::vector<int>& parents, const std::vector<LocalTRS>& pose, std::vector<glm::mat4>& out);

// --- Mirroring ----------------------------------------------------------------------------------------------------------
// A take's left-right mirror image, baked as a take of its own (tools/mm/gen_takes.py lists them: where the audit found the
// pack lopsided - docs/mm_audit.md). Each node under the rig's `root` takes its counterpart's (_l <-> _r) transform
// reflected through the rig's sagittal plane, in the root node's frame, times a rest correction: G'(i) = M G(m(i)) M K(i),
// K(i) = (M Bind(m(i)) M)^-1 Bind(i) - so the rest pose mirrors to itself even where the two sides' bone axes aren't
// mirror images (the Manny rig's right-side bones point the other way along X). Everything downstream (the root, the
// features, the contacts) is then computed from the mirrored pose as from any other.
struct MirrorRig {
    std::vector<int> Parent;           // per node (-1: none)
    std::vector<int> Mirror;           // per node: its counterpart (itself in the middle, or when the rig lacks one)
    std::vector<glm::mat4> Correction; // per node: K(i)
    std::vector<char> Below;           // per node: under the root node (the rest are left as they are)
    int Root = -1;
    glm::vec3 Normal{1.0f, 0.0f, 0.0f}; // the mirror plane's normal in the root node's frame (thigh_l -> thigh_r)
    glm::mat4 M{1.0f};
};
MirrorRig BuildMirror(const std::vector<std::string>& names, const std::vector<int>& parents, const std::vector<LocalTRS>& bind,
                      int root);
void MirrorPose(const MirrorRig& rig, std::vector<LocalTRS>& pose);

// --- Glitch repair --------------------------------------------------------------------------------------------------------
// The capture's glitches (docs/mm_audit.md: 583 bone-frames over 35 deg in a frame), repaired in the bake rather than cut:
//   - twist flips: a bone turning over kFlipDeg about its own long axis in one frame (the solver's 170-180 deg flips of the
//     thighs, calves and feet; the chain below flips with it, and the flip often stays for seconds). Unrolled: the bone is
//     turned back about its axis by the flip, its children's local rotations take the inverse (their world rotations, and
//     every joint position, are unchanged). The axis is the direction to the bone's children (only bones whose children
//     all lie along one line), or for a leaf the direction it comes in from its parent.
//   - spikes: a bone over kSpikeDeg in a frame (at 60 fps) that is back within kSpikeReturnDeg of where it was in at most
//     kSpikeMaxFrames frames - slerped across.
// What is neither stays as captured; MotionDatabase excludes it at load (kGlitchDegrees).
inline constexpr float kFlipDeg = 90.0f, kSpikeDeg = 35.0f, kSpikeReturnDeg = 10.0f;
inline constexpr int kSpikeMaxFrames = 6;
struct RepairStats {
    int Flips = 0;       // twist flips unrolled (bone events)
    int Spikes = 0;      // spikes slerped across (bone events) ...
    int SpikeFrames = 0; // ... and the bone-frames they covered
};
// `poses`: frames x nodes, local; `repair`: per node, whether to repair it (the streamed and finger bones).
RepairStats RepairGlitches(const std::vector<int>& parents, const std::vector<LocalTRS>& bind, const std::vector<char>& repair, float fps,
                           std::vector<std::vector<LocalTRS>>& poses);

// The whole bake. Returns the process exit code (0 = baked within budget).
int Run(const std::string& configPath, const std::string& match, const std::string& outOverride);

} // namespace MotionBake
