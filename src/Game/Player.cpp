#include "Player.h"
#include "Input.h"
#include "InputMap.h"
#include "PhysicsWorld.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

glm::vec3 PlayerMovementTarget(const glm::vec3& wish, const glm::vec3& rootVelocity, float rootWeight, bool sprint) {
    const glm::vec3 planarWish(wish.x,0.0f,wish.z);
    if (sprint && glm::dot(planarWish,planarWish)>1e-8f) return planarWish;
    return glm::mix(planarWish,glm::vec3(rootVelocity.x,0.0f,rootVelocity.z),std::clamp(rootWeight,0.0f,1.0f));
}

namespace {
float WrapDeg(float d) { d = std::fmod(d, 360.0f); return d > 180.0f ? d - 360.0f : d <= -180.0f ? d + 360.0f : d; }
glm::vec3 CharacterFeet() { glm::vec3 f; PhysicsWorld::GetCharacterFootPosition(&f.x); return f; }
void SetCharacterFeet(const glm::vec3& f) { PhysicsWorld::SetCharacterFootPosition(&f.x); }
unsigned MoveCharacter(const glm::vec3& d, float dt) { return PhysicsWorld::MoveCharacter(&d.x, dt); }
}

void Player::Update(float dt, World& world, GLFWwindow* window, bool readInput) {
    (void)world; (void)window;
    dt = std::max(0.0f, dt);
    glm::vec2 move(ScriptedMove ? ScriptMove.x : (readInput ? InputMap::GetAxis("Horizontal") : 0.0f),
                   ScriptedMove ? ScriptMove.y : (readInput ? InputMap::GetAxis("Vertical") : 0.0f));
    const bool sprintKey = !SprintBlocked && (ScriptedMove ? ScriptSprint : (readInput && InputMap::GetButton("Sprint")));
    const bool crouchKey = ScriptedMove ? ScriptCrouch : (readInput && InputMap::GetButton("Crouch"));
    const bool jumpDown = readInput && InputMap::GetButtonDown("Jump");
    LookDeltaInput = glm::vec2(0);
    YawDropped = 0.0f;
    if (readInput) {
        const float mousePitch = static_cast<float>(Input::GetMouseDeltaY()) * (InvertY ? -1.0f : 1.0f) * MouseSensitivity;
        const float stickPitch = -Input::GetGamepadAxis(GLFW_GAMEPAD_AXIS_RIGHT_Y) * (InvertY ? -1.0f : 1.0f) * StickLookDegPerSec * dt;
        // The camera clamps the mouse pass and the stick pass separately; these are their combined angles.
        const float lookPitch = std::clamp(std::clamp(Cam.Pitch + mousePitch, -89.0f, 89.0f) + stickPitch, -89.0f, 89.0f) - Cam.Pitch;
        const float lookYaw = static_cast<float>(Input::GetMouseDeltaX()) * MouseSensitivity +
                              Input::GetGamepadAxis(GLFW_GAMEPAD_AXIS_RIGHT_X) * StickLookDegPerSec * dt;
        LookDeltaInput = {mousePitch + stickPitch, lookYaw};
        const float before = Cam.Yaw;
        Cam.Yaw += lookYaw;
        Cam.Pitch = std::clamp(Cam.Pitch + lookPitch, -89.0f, 89.0f);
        // The body can only turn so fast: beyond the free range, view turn past MaxYawRate is dropped.
        if (MaxYawRate > 0.0f && dt > 0.0f) {
            const float reach = std::max(std::abs(WrapDeg(before - YawFreeCenter)), YawFreeRange) + MaxYawRate * dt;
            const float after = WrapDeg(Cam.Yaw - YawFreeCenter);
            if (std::abs(after) > reach) { YawDropped = std::abs(after) - reach; Cam.Yaw = YawFreeCenter + std::copysign(reach, after); }
        }
    }
    const float yaw = glm::radians(Cam.Yaw);
    const glm::vec3 forward(std::cos(yaw), 0.0f, std::sin(yaw)), right(-std::sin(yaw), 0.0f, std::cos(yaw));
    glm::vec3 wish = right * move.x + forward * move.y;
    if (glm::dot(wish, wish) > 1.0f) wish = glm::normalize(wish);
    MoveInput = {glm::dot(wish, right), glm::dot(wish, forward)};

    const float radius = std::max(0.05f, Size.x * 0.5f);
    const float stand = std::max(0.05f, Size.y * 0.5f - radius), crouch = std::max(0.05f, CrouchHeight * 0.5f - radius);
    const bool hasCharacter = PhysicsWorld::HasCharacter();
    if (CrouchHeight <= 0.0f) {
        if (Crouched && hasCharacter) PhysicsWorld::ResizeCharacter(stand);
        Crouched = false;
    } else if (hasCharacter) {
        if (crouchKey && !Crouched && Grounded) { if (PhysicsWorld::ResizeCharacter(crouch)) Crouched = true; }
        else if (!crouchKey && Crouched && PhysicsWorld::CharacterFitsAt(stand)) { PhysicsWorld::ResizeCharacter(stand); Crouched = false; }
    }
    // The eye's crouch: a critically damped spring (as quick as the old ease, ~0.3 s), so the view never jolts into
    // the move the way an exponential's first frame did.
    {
        const float w = 14.0f;
        const float goal = Crouched ? 1.0f : 0.0f, x = CrouchBlend - goal, v = CrouchBlendRate;
        const float e = std::exp(-w * dt), k = (v + w * x) * dt;
        CrouchBlend = goal + (x + k) * e;
        CrouchBlendRate = (v - w * k) * e;
        if (std::abs(CrouchBlend - goal) < 1e-4f && std::abs(CrouchBlendRate) < 1e-3f) { CrouchBlend = goal; CrouchBlendRate = 0.0f; }
    }
    const float eye = EyeHeight * (1.0f - CrouchBlend * (1.0f - std::clamp(CrouchHeight / std::max(0.1f, Size.y), 0.0f, 1.0f)) * (CrouchHeight > 0.0f ? 1.0f : 0.0f));
    const bool sprint = !Crouched && !AimHeld && sprintKey;
    wish *= MoveSpeed * (sprint ? SprintMultiplier : 1.0f) * (Crouched ? CrouchSpeedMultiplier : 1.0f);
    WishVelocity = wish;
    // A sprint clip can keep contributing fast root travel during its exit fade.
    // While reloading, walk input owns travel so that residual cannot keep us sprinting.
    const float rootWeight = SprintBlocked ? 0.0f : RootMotionWeight;
    const glm::vec3 target = PlayerMovementTarget(wish, RootMotionVelocity, rootWeight, sprint);
    const glm::vec3 horizontal = PlayerApproachVelocity(glm::vec3(Velocity.x, 0.0f, Velocity.z), target, dt,
                                                        Grounded ? GroundAccelTime : AirAccelTime, Grounded ? GroundDecelTime : AirAccelTime);
    Velocity.x = horizontal.x; Velocity.z = horizontal.z;
    const bool wasGrounded = Grounded;
    Jumped = false;
    m_SinceGrounded = wasGrounded ? 0.0f : m_SinceGrounded + dt;
    m_JumpBuffer = readInput && jumpDown ? JumpBufferTime : std::max(0.0f, m_JumpBuffer - dt);
    if (readInput && m_JumpBuffer > 0.0f && (wasGrounded || m_SinceGrounded < CoyoteTime) && Velocity.y <= 0.1f) {
        const bool canStand = !Crouched || (hasCharacter && PhysicsWorld::CharacterFitsAt(stand));
        if (canStand) {
            if (Crouched) { PhysicsWorld::ResizeCharacter(stand); Crouched = false; }
            Velocity.y = JumpSpeed; Jumped = true; m_JumpBuffer = 0.0f; m_SinceGrounded = 1000.0f;
        }
    }
    Velocity.y += Gravity * dt;
    const glm::vec3 feet = Cam.Position - glm::vec3(0.0f, eye, 0.0f);
    if (!PhysicsWorld::IsActive()) { Cam.Position += Velocity * dt; Grounded = false; return; }
    if (!hasCharacter) PhysicsWorld::CreateCharacter(radius, stand, &feet.x);
    const unsigned flags = MoveCharacter(Velocity * dt, dt);
    // Blocked by a wall: the part of the velocity into it goes, the part along it stays. (Cutting it to the speed actually
    // travelled took the slide along the wall too: a jamb grazed in a doorway left the player crawling until the
    // acceleration caught up.)
    if ((GroundAccelTime > 0.0f || GroundDecelTime > 0.0f || AirAccelTime > 0.0f) && dt > 0.0f) {
        float n[8][3];
        glm::vec3 walls[8];
        const int count = PhysicsWorld::LastMoveWallNormals(n, 8);
        for (int i = 0; i < count; ++i) walls[i] = glm::vec3(n[i][0], 0.0f, n[i][2]);
        const glm::vec3 v = PlayerClipVelocity(glm::vec3(Velocity.x, 0.0f, Velocity.z), walls, count);
        Velocity.x = v.x; Velocity.z = v.z;
    }
    Grounded = (flags & PhysicsWorld::CC_DOWN) != 0;
    // Walking off a step down: stay on the ground rather than fall the last few centimetres.
    if (!Grounded && wasGrounded && !Jumped && Velocity.y <= 0.0f) {
        const glm::vec3 before = CharacterFeet();
        if (MoveCharacter(glm::vec3(0.0f, -0.3f, 0.0f), dt) & PhysicsWorld::CC_DOWN) Grounded = true;
        else SetCharacterFeet(before);
    }
    if ((Grounded && Velocity.y < 0.0f) || ((flags & PhysicsWorld::CC_UP) && Velocity.y > 0.0f)) Velocity.y = 0.0f;
    Cam.Yaw += PhysicsWorld::PlatformYawDelta();
    Cam.Position = CharacterFeet() + glm::vec3(0.0f, eye, 0.0f);
    if (Cam.Position.y < KillY) {
        SetCharacterFeet(RespawnFeet);
        Cam.Position = RespawnFeet + glm::vec3(0.0f, eye, 0.0f);
        Velocity = glm::vec3(0.0f);
    }
}

glm::vec3 PlayerClipVelocity(const glm::vec3& v, const glm::vec3* walls, int count) {
    constexpr float kEps = 1e-5f;
    glm::vec3 out = v;
    for (int i = 0; i < count; ++i) {
        const float li = glm::length(walls[i]);
        if (li < 1e-4f) continue;
        const glm::vec3 ni = walls[i] / li;
        if (glm::dot(out, ni) >= -kEps) continue;
        glm::vec3 slid = out - ni * glm::dot(out, ni);
        for (int j = 0; j < count; ++j) {
            const float lj = glm::length(walls[j]);
            if (j == i || lj < 1e-4f || glm::dot(slid, walls[j] / lj) >= -kEps) continue;
            // Into a second wall too: along the corner between them, or stopped where they meet head on.
            glm::vec3 crease = glm::cross(ni, walls[j] / lj);
            const float cl = glm::length(crease);
            slid = cl < 1e-4f ? glm::vec3(0.0f) : crease / cl * glm::dot(out, crease / cl);
            break;
        }
        out = slid;
    }
    return glm::dot(out, v) > 0.0f ? out : glm::vec3(0.0f);
}

glm::vec3 PlayerApproachVelocity(const glm::vec3& current, const glm::vec3& target, float dt, float accelTime, float decelTime) {
    // Speeding up toward the target uses the acceleration time; slowing down (letting go, or turning
    // to a slower move) the deceleration time.
    const float t = glm::length(target) >= glm::length(current) ? accelTime : decelTime;
    if (!(t > 0.0f) || !(dt > 0.0f)) return target;
    const glm::vec3 next = current + (target - current) * (1.0f - std::exp(-dt / t));
    return glm::length(target - next) < 0.01f ? target : next; // arrive, rather than creep for ever
}
