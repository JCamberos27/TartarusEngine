#pragma once

#include "Animation.h"

#include <vector>

// Pose retargeting between humanoid skeletons that share bone names but not proportions, rest poses or
// spine counts - a UE4 mannequin pack (spine_01..03, neck_01) onto a UE5 Manny rig like Quantum's
// (spine_01..05, neck_01..02). Model::AttachClip copies local rotations by name, which only holds when
// both files share a rest pose; this bakes the clip onto the target skeleton instead, frame by frame:
//
//   - Each matched bone takes the source's world-space turn from its rest, applied to the target's rest
//     after the target bone is swung to point where the source's rest bone points (so A-pose vs T-pose
//     and differing bone axes don't matter).
//   - Target bones with no source counterpart between matched ones (spine_02 / spine_04, neck_02, the
//     metacarpals) take a share of the turn of the matched bones either side of them.
//   - Only root and pelvis carry the source's translation, scaled by the ratio of leg lengths; every
//     other bone keeps the target's bone lengths.
//   - The two files' world frames (up axis, facing, unit scale) are aligned from the rest skeletons.

// Per target node, the source node it takes its motion from (-1 none). By name, with the UE4 -> UE5 spine
// map when the source is a UE4 mannequin and the target a UE5 one. ik_* bones are never matched.
std::vector<int> RetargetMatchNodes(const std::vector<AnimNode>& source, const std::vector<AnimNode>& target);

// Whether `source` is a UE4 mannequin skeleton (spine_03, no spine_04) and `target` a UE5 one (spine_05):
// the case AttachClip bakes rather than copying locals.
bool RetargetIsUe4ToUe5(const std::vector<AnimNode>& source, const std::vector<AnimNode>& target);

// `clip` (animating `source`) baked onto `target`: `out` gets a channel per driven target node, keyed at
// the source's key times, and `nodeChannel` (per target node) its channel or -1. False when fewer than
// a handful of bones match.
bool RetargetBakeClip(const std::vector<AnimNode>& source, const AnimationClip& clip, const std::vector<AnimNode>& target,
                      AnimationClip& out, std::vector<int>& nodeChannel);
