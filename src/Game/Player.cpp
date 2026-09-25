#include "Player.h"
#include "Input.h"
#include "InputMap.h"
#include "PhysicsWorld.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

void Player::Update(float dt, World& world, GLFWwindow* window, bool readInput) {
    (void)world;  // collision now runs against PhysicsWorld's PhysX scene, not World's AABBs
    (void)window; // kept in the signature for a future direct-input path; unused today

    if (readInput) {
        const float yawBefore = Cam.Yaw;
        Cam.ProcessMouseLook((float)Input::GetMouseDeltaX(),
                             (float)Input::GetMouseDeltaY() * (InvertY ? -1.0f : 1.0f), MouseSensitivity);
        // #145 - gamepad right stick: a turn rate, not a delta. Stick Y is +down, look is +up.
        constexpr float kStickLookDegPerSec = 180.0f;
        const float lx = Input::GetGamepadAxis(GLFW_GAMEPAD_AXIS_RIGHT_X);
        const float ly = -Input::GetGamepadAxis(GLFW_GAMEPAD_AXIS_RIGHT_Y);
        if (lx != 0.0f || ly != 0.0f)
            Cam.ProcessMouseLook(lx * kStickLookDegPerSec * dt,
                                 ly * kStickLookDegPerSec * dt * (InvertY ? -1.0f : 1.0f), 1.0f);
        if (MaxYawRate > 0.0f && dt > 0.0f) {
            auto wrap = [](float d) { d = std::fmod(d, 360.0f); return d > 180.0f ? d - 360.0f : (d <= -180.0f ? d + 360.0f : d); };
            const float before = wrap(yawBefore - YawFreeCenter), after = wrap(Cam.Yaw - YawFreeCenter);
            // Further out than it was (and than the free range): only as fast as the limit.
            const float reach = std::max(std::abs(before), YawFreeRange) + MaxYawRate * dt;
            if (std::abs(after) > reach) Cam.Yaw = YawFreeCenter + std::copysign(reach, after);
        }
    }

    // Planar move input, relative to look yaw.
    glm::vec3 forward = glm::normalize(glm::vec3(Cam.Front().x, 0, Cam.Front().z));
    glm::vec3 right   = glm::normalize(glm::vec3(Cam.Right().x, 0, Cam.Right().z));

    // #145 - the Input Manager's Horizontal / Vertical / Sprint / Jump actions (Project Settings >
    // Input), so rebinding them moves the Player too. Keys give full tilt, a stick is analog.
    glm::vec3 wish{0.0f};
    if (readInput) {
        wish = right * InputMap::GetAxis("Horizontal") + forward * InputMap::GetAxis("Vertical");
        if (glm::length(wish) > 1.0f) wish = glm::normalize(wish);
    }
    // Crouch: held; standing up needs headroom. Only from the ground, so the capsule never changes
    // shape mid-jump.
    const float standCylHalf = std::max(0.05f, Size.y * 0.5f - std::max(0.05f, Size.x * 0.5f));
    const float crouchCylHalf = std::max(0.05f, CrouchHeight * 0.5f - std::max(0.05f, Size.x * 0.5f));
    if (CrouchHeight <= 0.0f) {
        if (Crouched && PhysicsWorld::HasCharacter()) PhysicsWorld::ResizeCharacter(standCylHalf);
        Crouched = false;
    } else if (PhysicsWorld::HasCharacter()) {
        const bool wantCrouch = readInput && InputMap::GetButton("Crouch");
        if (wantCrouch && !Crouched && Grounded) {
            if (PhysicsWorld::ResizeCharacter(crouchCylHalf)) Crouched = true;
        } else if (!wantCrouch && Crouched && PhysicsWorld::CharacterFitsAt(standCylHalf)) {
            PhysicsWorld::ResizeCharacter(standCylHalf);
            Crouched = false;
        }
    }
    CrouchBlend += ((Crouched ? 1.0f : 0.0f) - CrouchBlend) * std::min(1.0f, dt * 10.0f);
    // The eye follows the capsule down (a body's head bone overrides it; without one this is it).
    const float eye = EyeHeight * (1.0f - CrouchBlend * (1.0f - std::clamp(CrouchHeight / std::max(0.1f, Size.y), 0.0f, 1.0f)) * (CrouchHeight > 0.0f ? 1.0f : 0.0f));

    const bool sprint = readInput && !Crouched && InputMap::GetButton("Sprint");
    float speed = MoveSpeed * (sprint ? SprintMultiplier : 1.0f) * (Crouched ? CrouchSpeedMultiplier : 1.0f);
    wish *= speed;
    WishVelocity = wish;
    // Root motion (a first-person body) takes over the horizontal move by its weight.
    const float rm = std::clamp(RootMotionWeight, 0.0f, 1.0f);
    const glm::vec3 horizontal = wish + (glm::vec3(RootMotionVelocity.x, 0.0f, RootMotionVelocity.z) - wish) * rm;
    Velocity.x = horizontal.x;
    Velocity.z = horizontal.z;

    const bool wasGrounded = Grounded;
    Jumped = false;
    if (readInput && Grounded && !Crouched && InputMap::GetButtonDown("Jump")) {
        Velocity.y = JumpSpeed; // one impulse; the capsule move below integrates the arc
        Jumped = true;
    }

    Velocity.y += Gravity * dt;

    glm::vec3 feet = Cam.Position - glm::vec3(0, eye, 0);

    // No PhysX world (init failed) — fall back to a free-fly integrate so Play still works.
    if (!PhysicsWorld::IsActive()) {
        Cam.Position += Velocity * dt;
        Grounded = false;
        return;
    }

    const float radius  = std::max(0.05f, Size.x * 0.5f);
    const float cylHalf = std::max(0.05f, Size.y * 0.5f - radius); // total height Size.y = 2*(radius+cylHalf)
    if (!PhysicsWorld::HasCharacter()) {
        const float f[3] = {feet.x, feet.y, feet.z};
        PhysicsWorld::CreateCharacter(radius, cylHalf, f);
    }

    const glm::vec3 disp = Velocity * dt;
    const float d[3] = {disp.x, disp.y, disp.z};
    unsigned flags = PhysicsWorld::MoveCharacter(d, dt);

    Grounded = (flags & PhysicsWorld::CC_DOWN) != 0;
    // Walking down a small step or stair: the capsule would step off the edge and fall a few
    // centimetres each time (the camera bounces). Stay on the ground when there is some within a
    // step's height below - not after a jump, and not off a real drop.
    if (!Grounded && wasGrounded && !Jumped && Velocity.y <= 0.0f) {
        float before[3];
        PhysicsWorld::GetCharacterFootPosition(before);
        const float snap[3] = {0.0f, -0.3f, 0.0f};
        if (PhysicsWorld::MoveCharacter(snap, dt) & PhysicsWorld::CC_DOWN) {
            Grounded = true;
        } else {
            PhysicsWorld::SetCharacterFootPosition(before);
        }
    }
    if (Grounded && Velocity.y < 0.0f) Velocity.y = 0.0f;          // stop accumulating fall speed
    if ((flags & PhysicsWorld::CC_UP) && Velocity.y > 0.0f) Velocity.y = 0.0f; // bonk head

    // Ride a rotating platform: turn the view with it (#185 hardening). The capsule itself is
    // radial so only the look direction needs it.
    Cam.Yaw += PhysicsWorld::PlatformYawDelta();

    float out[3] = {feet.x, feet.y, feet.z};
    PhysicsWorld::GetCharacterFootPosition(out);
    Cam.Position = glm::vec3(out[0], out[1], out[2]) + glm::vec3(0, eye, 0);

    // Kill plane (#165: per scene): a fall through a gap respawns at the spawn point.
    if (Cam.Position.y < KillY) {
        const glm::vec3 resetFeet = RespawnFeet;
        const float rf[3] = {resetFeet.x, resetFeet.y, resetFeet.z};
        PhysicsWorld::SetCharacterFootPosition(rf);
        Cam.Position = resetFeet + glm::vec3(0, eye, 0);
        Velocity = glm::vec3(0.0f);
    }
}
