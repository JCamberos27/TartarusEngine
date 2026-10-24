#pragma once

#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include <glm/gtc/quaternion.hpp>

class World;

// Doors, drawers and lids the player opens (OpenableComponent). Each motion is worked out from the model's bounds in
// its own space, front facing local +Z, and applied to the entity's local transform around its shut pose.
namespace Openable {

// How far the player reaches with Interact, metres from the eye.
constexpr float kReach = 2.2f;

// The motion's usual amount when the component leaves it at 0: degrees for a hinge (100 for a door, 95 for a lid, 85
// for an oven door, 90 for a flap); metres for a slide (a drawer most of its parent's depth, `parentDepth`, or 0.3 m
// without one; a sliding door most of its own width).
float DefaultAmount(int motion, const glm::vec3& lo, const glm::vec3& hi, float parentDepth);

// The local transform at `t` (0 shut .. 1 open, eased) from the shut pose (`basePos`, `baseRot`), for a model whose
// bounds are lo..hi in its own space, drawn at `scale`. `back`: a hinge or flap swings toward local -Z (Swing Back).
void Pose(int motion, float amount, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& scale,
          const glm::vec3& basePos, const glm::quat& baseRot, float t, glm::vec3& outPos, glm::quat& outRot,
          bool back = false);

// Distance along the ray (origin + d * dir, dir unit length) to the box lo..hi placed by `xform`; negative for a miss.
float RayBox(const glm::mat4& xform, const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& origin,
             const glm::vec3& dir);

// One Play frame: with `interact` (the Interact button went down), the nearest openable under the crosshair within
// kReach, and not behind a wall, opens or shuts; then every openable moves toward its target.
void Update(World& world, const glm::vec3& eye, const glm::vec3& forward, bool interact, float dt);

// Opens `e`'s openable as Interact does (its open sound, the same swing): what a creature pushing on it uses. False
// when it isn't one, hasn't started (its first Play frame) or is open or opening already.
bool Open(World& world, entt::entity e);

// A hinged door's leaf as it hangs shut, flat on the ground plane: the hinge's foot, the unit direction from the hinge
// to the latch edge, the leaf's width and height, and the unit direction it swings toward. False for anything but a
// started Hinge Left / Right with a box collider.
bool Leaf(World& world, entt::entity e, glm::vec3& hinge, glm::vec3& toLatch, float& width, glm::vec3& swingsToward,
          float* height = nullptr);

// The part's box where it stands now (its Progress): centre, half-extents along its own axes, rotation. False for one
// without a box collider or not started.
bool LeafBox(World& world, entt::entity e, glm::vec3& centre, glm::vec3& half, glm::quat& rot);

// Every openable straight to open (t = 1) or shut (0), outside Play too: --smoke-shots with TARTARUS_OPEN_ALL=1 shows
// them all open, to check each hinge.
void SetAll(World& world, float t);

} // namespace Openable
