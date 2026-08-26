#include "Player.h"
#include "World.h"
#include "Input.h"
#include <GLFW/glfw3.h>

AABB Player::BodyBounds() const {
    glm::vec3 feet = Cam.Position - glm::vec3(0, EyeHeight, 0);
    glm::vec3 center = feet + glm::vec3(0, Size.y * 0.5f, 0);
    return AABB::FromCenterSize(center, Size);
}

void Player::Update(float dt, World& world, GLFWwindow* window) {
    // Mouse look
    Cam.ProcessMouseLook((float)Input::GetMouseDeltaX(), (float)Input::GetMouseDeltaY());

    // Movement input (planar, relative to look yaw)
    glm::vec3 forward = glm::normalize(glm::vec3(Cam.Front().x, 0, Cam.Front().z));
    glm::vec3 right = glm::normalize(glm::vec3(Cam.Right().x, 0, Cam.Right().z));

    glm::vec3 wish{0.0f};
    if (Input::IsKeyDown(GLFW_KEY_W)) wish += forward;
    if (Input::IsKeyDown(GLFW_KEY_S)) wish -= forward;
    if (Input::IsKeyDown(GLFW_KEY_D)) wish += right;
    if (Input::IsKeyDown(GLFW_KEY_A)) wish -= right;

    float speed = MoveSpeed * (Input::IsKeyDown(GLFW_KEY_LEFT_SHIFT) ? SprintMultiplier : 1.0f);
    if (glm::length(wish) > 0.0001f) {
        wish = glm::normalize(wish) * speed;
    }
    Velocity.x = wish.x;
    Velocity.z = wish.z;

    if (Grounded && Input::IsKeyPressed(GLFW_KEY_SPACE)) {
        Velocity.y = JumpSpeed;
    }
    Velocity.y += Gravity * dt;

    // Integrate feet position, then resolve against world geometry.
    glm::vec3 feet = Cam.Position - glm::vec3(0, EyeHeight, 0);
    feet += Velocity * dt;

    AABB body = AABB::FromCenterSize(feet + glm::vec3(0, Size.y * 0.5f, 0), Size);
    Grounded = world.ResolveCollisions(body, Velocity);

    glm::vec3 newCenter = (body.Min + body.Max) * 0.5f;
    glm::vec3 newFeet = newCenter - glm::vec3(0, Size.y * 0.5f, 0);
    Cam.Position = newFeet + glm::vec3(0, EyeHeight, 0);

    // Simple world bound so you can't wander into the void.
    if (Cam.Position.y < -20.0f) {
        Cam.Position = glm::vec3(0, EyeHeight + 1.0f, 0);
        Velocity = glm::vec3(0);
    }
}

bool Player::Shoot(World& world, float maxDist) {
    float dist;
    int idx = world.Raycast(Cam.Position, Cam.Front(), maxDist, dist);
    if (idx > 0) { // skip index 0 (ground) as a target
        world.Boxes[idx].Alive = false;
        return true;
    }
    return false;
}
