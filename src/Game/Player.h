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
    // #165 - were hard-coded.
    float MouseSensitivity = 0.1f;
    bool InvertY = false;
    float KillY = -20.0f;
    glm::vec3 RespawnFeet{0.0f, 1.0f, 0.0f};

    // A first-person body (FirstPersonBody.h) steering the capsule by root motion: the clips'
    // horizontal velocity (m/s, world) and how much of it replaces the input's (0 = input only,
    // 1 = root motion only). Set before each Update; zero weight is the plain controller.
    glm::vec3 RootMotionVelocity{0.0f};
    float RootMotionWeight = 0.0f;
    // The most the view may turn (yaw, degrees / second) beyond YawFreeRange degrees either side
    // of YawFreeCenter (Camera::Yaw degrees) - what a body turning on the spot can keep up with.
    // Inside the range, and back toward it, the view is free. 0 = unlimited. Set before each Update;
    // the turn beyond the limit is dropped.
    float MaxYawRate = 0.0f;
    // Crouching (the Input Manager's "Crouch", held): the capsule shrinks to CrouchHeight metres and
    // the move slows to CrouchSpeedMultiplier of MoveSpeed (no sprint, no jump). 0 = no crouching -
    // set by whatever wants it (a first-person body). Standing up waits for headroom.
    float CrouchHeight = 0.0f;
    float CrouchSpeedMultiplier = 0.45f;
    bool Crouched = false;
    float CrouchBlend = 0.0f; // 0 standing .. 1 crouched: eases the eye height
    float YawFreeCenter = 0.0f;
    float YawFreeRange = 0.0f;
    // Out, per Update: the input as a horizontal velocity (m/s, world - what the player asked
    // for, before root motion), and whether a jump started this frame.
    glm::vec3 WishVelocity{0.0f};
    bool Jumped = false;

    // readInput == false keeps the body simulating (gravity, collision, resting on geometry)
    // but ignores mouse-look / WASD / jump — used while the game runs inside the docked Game
    // panel and the player hasn't clicked in to take control yet (Esc hands control back).
    void Update(float dt, World& world, struct GLFWwindow* window, bool readInput = true);
};
