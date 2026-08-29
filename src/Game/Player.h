#pragma once
#include <glm/glm.hpp>
#include "Camera.h"
#include "AABB.h"

class World;

// Basic first-person playtest controller: capsule-ish AABB body, WASD + mouse look,
// gravity/jump. Just a way to walk around and check collision while testing a scene in Play
// mode — no gameplay of its own.
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

    // readInput == false keeps the body simulating (gravity, collision, resting on geometry)
    // but ignores mouse-look / WASD / jump — used while the game runs inside the docked Game
    // panel and the player hasn't clicked in to take control yet (Esc hands control back).
    void Update(float dt, World& world, struct GLFWwindow* window, bool readInput = true);

private:
    AABB BodyBounds() const;
};
