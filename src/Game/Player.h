#pragma once
#include <glm/glm.hpp>
#include "Camera.h"

class World;

// Basic first-person playtest controller: WASD + mouse look, gravity/jump. Just a way to walk
// around and check collision while testing a scene in Play mode — no gameplay of its own.
//
// Since #185 PR 3 the body is a PxCapsuleController living in PhysicsWorld's PhysX scene (built
// from the scene's colliders). Player::Update turns input into a displacement, sweeps the
// capsule through it, and reads the resolved foot position back into Cam. The old AABB
// push-out (World::ResolveCollisions) is gone.
class Player {
public:
    Camera Cam;
    glm::vec3 Velocity{0.0f};

    // Capsule dims: radius = Size.x/2, total height = Size.y (cylinder part = Size.y - Size.x).
    // Size.z is unused (kept for scene/pref compatibility). Eye height offset applied separately.
    glm::vec3 Size{0.6f, 1.8f, 0.6f};
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
};
