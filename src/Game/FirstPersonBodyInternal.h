#pragma once

// What the FirstPersonBody*.cpp files share among themselves (not the body's interface: FirstPersonBody.h).

#include "FirstPersonBody.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace FirstPersonBodyDetail {

// Model space: the Quantum / UE mannequin family faces +Z with +Y up, so its right is -X.
constexpr glm::vec3 kForward{0.0f, 0.0f, 1.0f};
constexpr glm::vec3 kRight{-1.0f, 0.0f, 0.0f};

inline glm::quat YawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }

// Frame-rate independent approach toward a target over `seconds` (0 = snap).
inline float Follow(float dt, float seconds) { return FirstPersonBodyFollow(dt, seconds); }

} // namespace FirstPersonBodyDetail
