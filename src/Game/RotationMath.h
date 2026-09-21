#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp> // needs GLM_ENABLE_EXPERIMENTAL, which the engine and the game DLL both define

// #123 - the orientation `eulerDeg` (YXZ Euler degrees, the order ComposeTransform builds with)
// turned by `angleDeg` about `axis`, a direction in the object's own (local) frame, returned as
// YXZ Euler degrees again.
//
// Adding `axis * angle` to the three Euler components only spins about `axis` when it is a
// principal axis; a diagonal like (1,1,0) instead adds equal pitch and yaw, which is a different
// rotation. A zero axis or zero angle returns `eulerDeg` untouched.
//
// Header-only on purpose: TartarusGame.dll (SpinSystem, TransformControllerSystem, ...) doesn't
// link World.cpp, so it can't call anything defined there.
inline glm::vec3 RotateEulerAboutLocalAxis(const glm::vec3& eulerDeg, const glm::vec3& axis, float angleDeg) {
    const float len = glm::length(axis);
    if (len < 1e-6f || angleDeg == 0.0f) return eulerDeg;
    const glm::mat4 base = glm::eulerAngleYXZ(glm::radians(eulerDeg.y), glm::radians(eulerDeg.x), glm::radians(eulerDeg.z));
    const glm::mat4 turn = glm::rotate(glm::mat4(1.0f), glm::radians(angleDeg), axis / len);
    float ey, ex, ez;
    glm::extractEulerAngleYXZ(base * turn, ey, ex, ez);
    return glm::degrees(glm::vec3(ex, ey, ez));
}
