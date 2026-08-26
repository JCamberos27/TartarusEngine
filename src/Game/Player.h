#pragma once
#include <glm/glm.hpp>
#include "Camera.h"
#include "AABB.h"

class World;

// FPS player: capsule-ish AABB body, WASD + mouse look, gravity/jump, and a shoot raycast.
class Player {
public:
    Camera Cam;
    glm::vec3 Velocity{0.0f};

    glm::vec3 Size{0.6f, 1.8f, 0.6f}; // body AABB (eye height offset applied separately)
    float EyeHeight = 1.6f;
    float MoveSpeed = 6.0f;
    float SprintMultiplier = 1.6f;
    float JumpSpeed = 5.5f;
    float Gravity = -18.0f;
    bool Grounded = false;

    void Update(float dt, World& world, struct GLFWwindow* window);

    // Fires a hitscan shot from the camera; marks the hit target (if any) dead.
    // Returns true if something was hit.
    bool Shoot(World& world, float maxDist = 100.0f);

private:
    AABB BodyBounds() const;
};
