#include "Player.h"
#include "World.h"
#include "Input.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

AABB Player::BodyBounds() const {
    glm::vec3 feet = Cam.Position - glm::vec3(0, EyeHeight, 0);
    glm::vec3 center = feet + glm::vec3(0, Size.y * 0.5f, 0);
    return AABB::FromCenterSize(center, Size);
}

void Player::Update(float dt, World& world, GLFWwindow* window, bool readInput) {
    (void)window; // kept in the signature for a future direct-input path; unused today
    // Mouse look — skipped when input is handed to the editor (see the header): the body still
    // falls/collides below, it just doesn't turn or walk.
    if (readInput) {
        Cam.ProcessMouseLook((float)Input::GetMouseDeltaX(), (float)Input::GetMouseDeltaY());
    }

    // Movement input (planar, relative to look yaw)
    glm::vec3 forward = glm::normalize(glm::vec3(Cam.Front().x, 0, Cam.Front().z));
    glm::vec3 right = glm::normalize(glm::vec3(Cam.Right().x, 0, Cam.Right().z));

    glm::vec3 wish{0.0f};
    if (readInput) {
        if (Input::IsKeyDown(GLFW_KEY_W)) wish += forward;
        if (Input::IsKeyDown(GLFW_KEY_S)) wish -= forward;
        if (Input::IsKeyDown(GLFW_KEY_D)) wish += right;
        if (Input::IsKeyDown(GLFW_KEY_A)) wish -= right;
    }

    float speed = MoveSpeed * ((readInput && Input::IsKeyDown(GLFW_KEY_LEFT_SHIFT)) ? SprintMultiplier : 1.0f);
    if (glm::length(wish) > 0.0001f) {
        wish = glm::normalize(wish) * speed;
    }
    Velocity.x = wish.x;
    Velocity.z = wish.z;

    if (readInput && Grounded && Input::IsKeyPressed(GLFW_KEY_SPACE)) {
        Velocity.y = JumpSpeed; // impulse, applied once — the fall/collide loop below is per-substep
    }

    // Integrate gravity + movement and resolve against world geometry in small fixed substeps.
    // A single large step — low framerate (in-panel play also renders the Scene view + full
    // editor every frame now), a debugger pause, or just a fast fall onto a thin floor collider
    // — moves the body so far in one go that it ends up more than halfway through the geometry.
    // AABB::MTV then pushes out along the now-smallest overlap, which points the WRONG way once
    // the body's center is past the obstacle's center, and the player is shoved straight
    // through. Capping each step keeps every penetration shallow, so MTV always resolves toward
    // the face the body actually crossed.
    float remaining = std::min(dt, 0.1f); // Clock already clamps to 0.1; belt-and-braces
    const float kMaxStep = 1.0f / 120.0f;
    while (remaining > 1e-5f) {
        // Also cap by distance so a very fast fall (great height) can't skip a thin collider
        // between substeps either — no step moves the body more than ~5 cm.
        float speed3D = glm::length(Velocity);
        float stepByDist = speed3D > 1e-4f ? 0.05f / speed3D : remaining;
        float h = std::min({remaining, kMaxStep, stepByDist});
        h = std::max(h, 1.0f / 1000.0f); // never stall the loop, however absurd the speed
        remaining -= h;

        Velocity.y += Gravity * h;

        glm::vec3 feet = Cam.Position - glm::vec3(0, EyeHeight, 0);
        feet += Velocity * h;

        AABB body = AABB::FromCenterSize(feet + glm::vec3(0, Size.y * 0.5f, 0), Size);
        Grounded = world.ResolveCollisions(body, Velocity);

        glm::vec3 newCenter = (body.Min + body.Max) * 0.5f;
        glm::vec3 newFeet = newCenter - glm::vec3(0, Size.y * 0.5f, 0);
        Cam.Position = newFeet + glm::vec3(0, EyeHeight, 0);

        // Simple world bound so you can't wander into the void.
        if (Cam.Position.y < -20.0f) {
            Cam.Position = glm::vec3(0, EyeHeight + 1.0f, 0);
            Velocity = glm::vec3(0);
            break;
        }
    }
}
