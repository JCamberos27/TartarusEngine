#pragma once

// Motion matching debug draw (docs/MOTION_MATCHING_HANDOFF.md, Phase 3): world-space lines in the HUD line format
// (per vertex x y z r g b a, two vertices a line - NpcTest::DebugLines, drawn over the Game view by Application.cpp).
//
//   Trajectory()  three root-space trajectory samples (features 15-26: a query's or a database frame's) placed at a
//                 character: a polyline from the character through the samples, and a facing tick at each
//   Predicted()   TrajectoryPredictor states: the path, facing ticks
//   Character()   the character's root: a cross and its facing
//   Feet()        root-space foot positions (features 0-5) at a character, blue when the frame's contact flag is set
//
// Colours used by `--npc-test mmdb`: desired trajectory yellow, matched frame's trajectory magenta, the simulated path
// grey, the root green.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct TrajectoryState;

namespace MotionDebugDraw {

void Line(std::vector<float>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& c);
void Cross(std::vector<float>& out, const glm::vec3& at, float size, const glm::vec4& c);

// `traj12`: features 15-26 (raw). `charPos`/`charYaw`: where the character stands; `restYaw` the stream's.
void Trajectory(std::vector<float>& out, const glm::vec3& charPos, float charYaw, float restYaw, const float* traj12,
                const glm::vec4& c, float lift = 0.03f);
void Predicted(std::vector<float>& out, const TrajectoryState* states, int count, const glm::vec4& c, float lift = 0.03f);
void Character(std::vector<float>& out, const glm::vec3& pos, float yaw, const glm::vec4& c, float lift = 0.03f);
// `feet6`: features 0-5 (raw); `flags`: the frame's labels (MotionLabel::ContactL/R).
void Feet(std::vector<float>& out, const glm::vec3& charPos, float charYaw, float restYaw, const float* feet6, std::uint8_t flags);

} // namespace MotionDebugDraw
