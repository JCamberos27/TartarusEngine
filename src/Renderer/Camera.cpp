#include "Camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

void Camera::RefreshBasisIfNeeded() const {
    if (Yaw == m_CachedYaw && Pitch == m_CachedPitch) return;

    glm::vec3 f;
    f.x = cos(glm::radians(Yaw)) * cos(glm::radians(Pitch));
    f.y = sin(glm::radians(Pitch));
    f.z = sin(glm::radians(Yaw)) * cos(glm::radians(Pitch));
    m_CachedFront = glm::normalize(f);
    m_CachedRight = glm::normalize(glm::cross(m_CachedFront, glm::vec3(0, 1, 0)));
    m_CachedUp = glm::normalize(glm::cross(m_CachedRight, m_CachedFront));
    m_CachedYaw = Yaw;
    m_CachedPitch = Pitch;
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
    return glm::lookAt(Position, Position + Front(), glm::vec3(0, 1, 0));
}

glm::mat4 Camera::ProjectionMatrix(float aspect, float nearPlane, float farPlane) const {
    if (Orthographic) {
        float halfW = OrthoHalfHeight * aspect;
        return glm::ortho(-halfW, halfW, -OrthoHalfHeight, OrthoHalfHeight, nearPlane, farPlane);
    }
    return glm::perspective(glm::radians(Fov), aspect, nearPlane, farPlane);
}
