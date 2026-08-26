#include "Camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

glm::vec3 Camera::Front() const {
    glm::vec3 f;
    f.x = cos(glm::radians(Yaw)) * cos(glm::radians(Pitch));
    f.y = sin(glm::radians(Pitch));
    f.z = sin(glm::radians(Yaw)) * cos(glm::radians(Pitch));
    return glm::normalize(f);
}

glm::vec3 Camera::Right() const {
    return glm::normalize(glm::cross(Front(), glm::vec3(0, 1, 0)));
}

glm::vec3 Camera::Up() const {
    return glm::normalize(glm::cross(Right(), Front()));
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
    return glm::perspective(glm::radians(Fov), aspect, nearPlane, farPlane);
}
