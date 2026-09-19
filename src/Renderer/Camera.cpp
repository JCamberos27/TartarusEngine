#include "Camera.h"
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

glm::mat4 Camera::ProjectionMatrix(float aspect, float nearOverride, float farOverride) const {
    const float nearPlane = nearOverride >= 0.0f ? nearOverride : NearPlane;
    const float farPlane  = farOverride  >= 0.0f ? farOverride  : FarPlane;
    if (Orthographic) {
        float halfW = OrthoHalfHeight * aspect;
        return glm::ortho(-halfW, halfW, -OrthoHalfHeight, OrthoHalfHeight, nearPlane, farPlane);
    }
    return glm::perspective(glm::radians(Fov), aspect, nearPlane, farPlane);
}
