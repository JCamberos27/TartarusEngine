#include "Camera.h"
#include <cmath> // #202 isfinite
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

void Camera::RefreshBasisIfNeeded() const {
    if (Yaw == m_CachedYaw && Pitch == m_CachedPitch && Roll == m_CachedRoll) return;

    glm::vec3 f;
    f.x = cos(glm::radians(Yaw)) * cos(glm::radians(Pitch));
    f.y = sin(glm::radians(Pitch));
    f.z = sin(glm::radians(Yaw)) * cos(glm::radians(Pitch));
    m_CachedFront = glm::normalize(f);
    m_CachedRight = glm::normalize(glm::cross(m_CachedFront, glm::vec3(0, 1, 0)));
    m_CachedUp = glm::normalize(glm::cross(m_CachedRight, m_CachedFront));
    if (Roll != 0.0f) {
        const float c = cos(glm::radians(Roll)), s = sin(glm::radians(Roll));
        const glm::vec3 r = m_CachedRight, u = m_CachedUp;
        m_CachedRight = c * r + s * u;
        m_CachedUp    = c * u - s * r;
    }
    m_CachedYaw = Yaw;
    m_CachedPitch = Pitch;
    m_CachedRoll = Roll;
}

glm::vec3 Camera::Front() const {
    RefreshBasisIfNeeded();
    return m_CachedFront;
}

glm::vec3 Camera::Right() const {
    RefreshBasisIfNeeded();
    return m_CachedRight;
}

glm::vec3 Camera::Up() const {
    RefreshBasisIfNeeded();
    return m_CachedUp;
}

void Camera::ProcessMouseLook(float dx, float dy, float sensitivity) {
    Yaw += dx * sensitivity;
    Pitch += dy * sensitivity;
    Pitch = std::clamp(Pitch, -89.0f, 89.0f);
}

glm::mat4 Camera::ViewMatrix() const {
    return glm::lookAt(Position, Position + Front(), Up());
}

// #202 - see the header. Shared by Camera::ProjectionMatrix and every direct call site.
glm::mat4 MakePerspective(float fovDegrees, float aspect, float nearPlane, float farPlane) {
    if (!std::isfinite(aspect) || aspect <= 0.0f) aspect = 1.0f;  // inf from a zero-height viewport, NaN from 0x0
    if (!std::isfinite(fovDegrees) || fovDegrees <= 0.0f || fovDegrees >= 180.0f) fovDegrees = 60.0f;
    if (!std::isfinite(nearPlane) || nearPlane <= 0.0f) nearPlane = 0.01f;
    if (!std::isfinite(farPlane) || farPlane <= nearPlane) farPlane = nearPlane + 1.0f;
    return glm::perspective(glm::radians(fovDegrees), aspect, nearPlane, farPlane);
}

glm::mat4 Camera::ProjectionMatrix(float aspect, float nearOverride, float farOverride) const {
    float nearPlane = nearOverride >= 0.0f ? nearOverride : NearPlane;
    float farPlane  = farOverride  >= 0.0f ? farOverride  : FarPlane;

    // #202 - a degenerate frustum must never reach glm. far == near divides by zero and a
    // non-positive near makes the perspective divide meaningless: either way the matrix comes
    // back full of inf/NaN, which then spreads into every world position derived from it and
    // the view simply goes black - with nothing in the log to say why. The values can arrive
    // from a hand-edited scene, an older file, or a component written at runtime, so the guard
    // belongs here at the single point of use rather than at each of those call sites.
    if (!std::isfinite(aspect) || aspect <= 0.0f) aspect = 1.0f;  // inf from a zero-height viewport, NaN from 0x0
    if (!std::isfinite(nearPlane) || nearPlane <= 0.0f) nearPlane = 0.01f;
    if (!std::isfinite(farPlane) || farPlane <= nearPlane) farPlane = nearPlane + 1.0f;
    if (Orthographic) {
        // #202 - same reasoning as the clip planes above, for the axis this path divides by.
        // glm::ortho computes 2/(right-left) and 2/(top-bottom), so a zero (or NaN, or negative)
        // half-height gives an inf/NaN matrix. The interactive zoom clamps to [0.25, 250], but
        // nothing else does - a view transition derives it from distance * tan(halfFov), which
        // is zero when the camera sits on its pivot.
        float halfH = OrthoHalfHeight;
        if (!std::isfinite(halfH) || halfH <= 0.0f) halfH = 0.25f;
        const float halfW = halfH * aspect;
        return glm::ortho(-halfW, halfW, -halfH, halfH, nearPlane, farPlane);
    }
    return MakePerspective(Fov, aspect, nearPlane, farPlane);
}
