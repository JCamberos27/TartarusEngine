#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp> // needs GLM_ENABLE_EXPERIMENTAL, which the engine and the game DLL both define
#include <cmath>

// TransformComponent stores orientation as a quaternion (#123), but scenes authored before
// format v4 and the Inspector both speak Y-X-Z Euler degrees. These helpers are the single
// conversion boundary between those representations.
inline glm::quat NormalizeRotation(const glm::quat& q) {
    const float len2 = glm::dot(q, q);
    if (!std::isfinite(len2) || len2 < 1e-12f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    // GLM's constructors already produce unit quaternions within float precision. Leaving an
    // already-unit value bit-identical makes save -> load -> save stable instead of changing the
    // last bit every time it is needlessly renormalized.
    if (std::fabs(len2 - 1.0f) <= 1e-6f) return q;
    return q * (1.0f / std::sqrt(len2));
}

inline glm::quat QuaternionFromEulerYXZ(const glm::vec3& eulerDeg) {
    const glm::mat4 m = glm::eulerAngleYXZ(glm::radians(eulerDeg.y),
                                           glm::radians(eulerDeg.x),
                                           glm::radians(eulerDeg.z));
    return NormalizeRotation(glm::quat_cast(m));
}

inline glm::vec3 EulerYXZFromQuaternion(const glm::quat& rotation) {
    float ey, ex, ez;
    glm::extractEulerAngleYXZ(glm::mat4_cast(NormalizeRotation(rotation)), ey, ex, ez);
    glm::vec3 out = glm::degrees(glm::vec3(ex, ey, ez));
    for (int i = 0; i < 3; ++i)
        if (std::fabs(out[i]) < 1.0e-4f) out[i] = 0.0f;
    return out;
}

// YXZ Euler angles are ambiguous: (x,y,z) and (180-x,y+180,z+180), each also +/-360,
// describe the same orientation. Pick the representation nearest the last Inspector value.
inline glm::vec3 NearestEquivalentEuler(const glm::vec3& eulerDeg, const glm::vec3& hintDeg) {
    auto wrapNear = [](float v, float hint) { return v + 360.0f * std::round((hint - v) / 360.0f); };
    glm::vec3 best(0.0f);
    float bestCost = 1e30f;
    const glm::vec3 candidates[2] = {
        eulerDeg,
        glm::vec3(180.0f - eulerDeg.x, eulerDeg.y + 180.0f, eulerDeg.z + 180.0f)
    };
    for (const glm::vec3& candidate : candidates) {
        const glm::vec3 wrapped(wrapNear(candidate.x, hintDeg.x),
                                wrapNear(candidate.y, hintDeg.y),
                                wrapNear(candidate.z, hintDeg.z));
        const glm::vec3 delta = glm::abs(wrapped - hintDeg);
        const float cost = delta.x + delta.y + delta.z;
        if (cost < bestCost - 1e-3f) { bestCost = cost; best = wrapped; }
    }
    return best;
}

inline glm::quat QuaternionFromMatrix(const glm::mat4& matrix) {
    glm::mat3 basis(matrix);
    for (int column = 0; column < 3; ++column) {
        const float length = glm::length(basis[column]);
        if (length > 1e-8f) basis[column] /= length;
    }
    // A mirrored basis contains a reflection which no quaternion can represent. Match the old
    // transform decomposition by assigning that sign to X and keeping a proper rotation basis.
    if (glm::determinant(basis) < 0.0f) basis[0] = -basis[0];
    return NormalizeRotation(glm::quat_cast(basis));
}

inline bool SameRotation(const glm::quat& a, const glm::quat& b, float epsilon = 1e-5f) {
    return std::fabs(glm::dot(NormalizeRotation(a), NormalizeRotation(b))) >= 1.0f - epsilon;
}

inline glm::quat RotateAboutLocalAxis(const glm::quat& base, const glm::vec3& axis, float angleDeg) {
    const float len = glm::length(axis);
    if (len < 1e-6f || angleDeg == 0.0f) return NormalizeRotation(base);
    return NormalizeRotation(NormalizeRotation(base) * glm::angleAxis(glm::radians(angleDeg), axis / len));
}

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
    return EulerYXZFromQuaternion(RotateAboutLocalAxis(QuaternionFromEulerYXZ(eulerDeg), axis, angleDeg));
}
