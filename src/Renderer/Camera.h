#pragma once
#include <glm/glm.hpp>

// First-person camera: position + yaw/pitch, derives view matrix and basis vectors.
class Camera {
public:
    glm::vec3 Position{0.0f, 1.7f, 0.0f};
    float Yaw = -90.0f;   // degrees, facing -Z by default
    float Pitch = 0.0f;
    float Fov = 75.0f;

    glm::vec3 Front() const;
    glm::vec3 Right() const;
    glm::vec3 Up() const;

    void ProcessMouseLook(float dx, float dy, float sensitivity = 0.1f);

    glm::mat4 ViewMatrix() const;
    glm::mat4 ProjectionMatrix(float aspect, float nearPlane = 0.05f, float farPlane = 500.0f) const;
};
