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

// The whole bake. Returns the process exit code (0 = baked within budget).
int Run(const std::string& configPath, const std::string& match, const std::string& outOverride);

} // namespace MotionBake
