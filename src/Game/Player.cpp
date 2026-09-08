#include "Player.h"
#include "Input.h"
#include "PhysicsWorld.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

void Player::Update(float dt, World& world, GLFWwindow* window, bool readInput) {
    (void)world;  // collision now runs against PhysicsWorld's PhysX scene, not World's AABBs
    (void)window; // kept in the signature for a future direct-input path; unused today

    if (readInput)
        Cam.ProcessMouseLook((float)Input::GetMouseDeltaX(), (float)Input::GetMouseDeltaY());

    // Planar move input, relative to look yaw.
    glm::vec3 forward = glm::normalize(glm::vec3(Cam.Front().x, 0, Cam.Front().z));
    glm::vec3 right   = glm::normalize(glm::vec3(Cam.Right().x, 0, Cam.Right().z));

    glm::vec3 wish{0.0f};
    if (readInput) {
        if (Input::IsKeyDown(GLFW_KEY_W)) wish += forward;
        if (Input::IsKeyDown(GLFW_KEY_S)) wish -= forward;
        if (Input::IsKeyDown(GLFW_KEY_D)) wish += right;
        if (Input::IsKeyDown(GLFW_KEY_A)) wish -= right;
    }
    float speed = MoveSpeed * ((readInput && Input::IsKeyDown(GLFW_KEY_LEFT_SHIFT)) ? SprintMultiplier : 1.0f);
    if (glm::length(wish) > 0.0001f) wish = glm::normalize(wish) * speed;
    Velocity.x = wish.x;
    Velocity.z = wish.z;

    if (readInput && Grounded && Input::IsKeyPressed(GLFW_KEY_SPACE))
        Velocity.y = JumpSpeed; // one impulse; the capsule move below integrates the arc

    Velocity.y += Gravity * dt;

    glm::vec3 feet = Cam.Position - glm::vec3(0, EyeHeight, 0);

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
    if (Grounded && Velocity.y < 0.0f) Velocity.y = 0.0f;          // stop accumulating fall speed
    if ((flags & PhysicsWorld::CC_UP) && Velocity.y > 0.0f) Velocity.y = 0.0f; // bonk head

    float out[3] = {feet.x, feet.y, feet.z};
    PhysicsWorld::GetCharacterFootPosition(out);
    Cam.Position = glm::vec3(out[0], out[1], out[2]) + glm::vec3(0, EyeHeight, 0);

    // Simple world floor so a fall through a gap doesn't drop forever.
    if (Cam.Position.y < -20.0f) {
        const glm::vec3 resetFeet(0.0f, 1.0f, 0.0f);
        const float rf[3] = {resetFeet.x, resetFeet.y, resetFeet.z};
        PhysicsWorld::SetCharacterFootPosition(rf);
        Cam.Position = resetFeet + glm::vec3(0, EyeHeight, 0);
        Velocity = glm::vec3(0.0f);
    }
}
