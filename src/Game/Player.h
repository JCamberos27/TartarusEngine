#pragma once
#include <glm/glm.hpp>
#include "Camera.h"

class World;

// Native state and input adapter for project/assets/Scripts/PlayerController.cs.
// The managed controller owns movement, look, gravity, jump, crouch and respawn decisions;
// physics services own the capsule and the editor can continue inspecting these values.
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
    float StickLookDegPerSec = 180.0f; // gamepad right stick turn rate
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
    float YawDropped = 0.0f; // out, per Update: the view turn (degrees) MaxYawRate dropped
    glm::vec2 LookDeltaInput{0.0f}; // pitch/yaw degrees before camera clamps, mouse + stick
    glm::vec2 MoveInput{0.0f}; // right/forward action axes, before speed, acceleration and collisions
    // Crouching (the Input Manager's "Crouch", held): the capsule shrinks to CrouchHeight metres and
    // the move slows to CrouchSpeedMultiplier of MoveSpeed (no sprint, no jump). 0 = no crouching -
    // set by whatever wants it (a first-person body). Standing up waits for headroom.
    float CrouchHeight = 0.0f;
    float CrouchSpeedMultiplier = 0.45f;
    bool Crouched = false;
    // A jump pressed this long before landing still happens on landing, and one pressed this long
    // after stepping off an edge still counts (seconds).
    float JumpBufferTime = 0.12f;
    float CoyoteTime = 0.10f;
    // How quickly the move reaches the input's speed on the ground (speeding up / slowing down) and in
    // the air: the time constant (seconds) of an exponential approach, so about 2.3x this reaches 90%.
    // 0 = instantly (the move is the input).
    float GroundAccelTime = 0.0f;
    float GroundDecelTime = 0.0f;
    float AirAccelTime = 0.0f;
    float m_SinceGrounded = 0.0f, m_JumpBuffer = 0.0f;
    float CrouchBlend = 0.0f; // 0 standing .. 1 crouched: eases the eye height
    float CrouchBlendRate = 0.0f; // ... its rate (per second): the ease is a spring, so the view starts and stops moving smoothly
    float YawFreeCenter = 0.0f;
    float YawFreeRange = 0.0f;
    // Out, per Update: the input as a horizontal velocity (m/s, world - what the player asked
    // for, before root motion), and whether a jump started this frame.
    glm::vec3 WishVelocity{0.0f};
    bool Jumped = false;
    // A script standing in for the move keys (--stock-probe): with ScriptedMove on, the move is
    // ScriptMove (x right, y forward, -1..1) and ScriptSprint, whatever the input. Look is untouched.
    bool ScriptedMove = false;
    glm::vec2 ScriptMove{0.0f};
    bool ScriptSprint = false;
    bool AimHeld = false; // weapon aim intent blocks sprint without consuming the held sprint input
    bool SprintBlocked = false; // reload / mag check / inspect / melee suspension; the held/scripted sprint intent remains available
    bool ScriptCrouch = false; // ... and Crouch

    // readInput == false keeps the body simulating (gravity, collision, resting on geometry)
    // but ignores mouse-look / WASD / jump — used while the game runs inside the docked Game
    // panel and the player hasn't clicked in to take control yet (Esc hands control back).
    void Update(float dt, World& world, struct GLFWwindow* window, bool readInput = true);
};

// The horizontal move one step closer to the target: an exponential approach with time constant
// accelTime (speeding up) or decelTime (slowing down or turning away), so it is the same at any frame
// rate. A time of 0 snaps to the target.
glm::vec3 PlayerApproachVelocity(const glm::vec3& current, const glm::vec3& target, float dt, float accelTime, float decelTime);

// Sprint travel follows input speed in every direction, independent of clip root distance.
glm::vec3 PlayerMovementTarget(const glm::vec3& wish, const glm::vec3& rootVelocity, float rootWeight, bool sprint);
