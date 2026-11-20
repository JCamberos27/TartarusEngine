#include "Player.h"
#include "Core/Env.h"
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
unsigned MoveCharacter(const glm::vec3& d, float dt, bool snap = false) { return PhysicsWorld::MoveCharacter(&d.x, dt, snap); }
}

void Player::Update(float dt, bool readInput) {
    dt = std::max(0.0f, dt);
    glm::vec2 move(ScriptedMove ? ScriptMove.x : (readInput ? InputMap::GetAxis("Horizontal") : 0.0f),
                   ScriptedMove ? ScriptMove.y : (readInput ? InputMap::GetAxis("Vertical") : 0.0f));
    // The jog key (Sprint button). Reloads, inspects and melee no longer hold it back: the gun stays up at a jog.
    const bool sprintKey = ScriptedMove ? ScriptSprint : (readInput && InputMap::GetButton("Sprint"));
    // The sprint rides the same key (the owner, 2026-11-17: Left Shift): a press while moving starts it, and it holds while the
    // key does (SprintHeld); a press standing still is the jog. (A script asks with ScriptRun, held.)
    const bool sprintPressed = !ScriptedMove && readInput && InputMap::GetButtonDown("Sprint");
    const bool crouchKey = ScriptedMove ? ScriptCrouch : (readInput && InputMap::GetButton("Crouch"));
    const bool walkKey = ScriptedMove ? ScriptWalk : (readInput && InputMap::GetButton("Walk"));
    const bool jumpDown = ScriptedMove ? ScriptJump : (readInput && InputMap::GetButtonDown("Jump"));
    LookDeltaInput = glm::vec2(0);
    YawDropped = 0.0f;
    if (readInput) {
        const float mousePitch = static_cast<float>(Input::GetMouseDeltaY()) * (InvertY ? -1.0f : 1.0f) * MouseSensitivity;
        const float stickPitch = -Input::GetGamepadAxis(GLFW_GAMEPAD_AXIS_RIGHT_Y) * (InvertY ? -1.0f : 1.0f) * StickLookDegPerSec * dt;
        // The camera clamps the mouse pass and the stick pass separately; these are their combined angles.
        const float lookPitch = std::clamp(std::clamp(Cam.Pitch + mousePitch, -89.0f, 89.0f) + stickPitch, -89.0f, 89.0f) - Cam.Pitch;
        // TARTARUS_PERF_TURN: a steady turn, so --perf-bench measures a turning view (the worst case for anything
        // cached across frames) rather than a still one.
        static const float s_PerfTurn = Env::Float(Env::Var::PerfTurn, 0.0f);
        const float lookYaw = static_cast<float>(Input::GetMouseDeltaX()) * MouseSensitivity +
                              Input::GetGamepadAxis(GLFW_GAMEPAD_AXIS_RIGHT_X) * StickLookDegPerSec * dt + s_PerfTurn * dt;
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
    // A body's look-down limit, whatever turned the view (the mouse, the stick, a script).
    Cam.Pitch = std::max(Cam.Pitch, std::max(MinPitch, -89.0f));
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
    // Shift (the Sprint button) is the jog: the gun stays up, so aiming doesn't cancel it.
    const bool sprint = !Crouched && sprintKey;
    Sprinting = sprint;
    Gait = Crouched ? 2 : sprint ? 1 : 0;
    if (GaitSpeeds.Active) {
        // The stick picks the gait (see DirectionalSpeeds): the stroll under StrollStick or with Walk held, then the gait's
        // own speed in the stick's direction.
        const float stick = glm::length(MoveInput);
        if (Gait == 0 && GaitSpeeds.Forward[3] > 0.0f && stick > 1e-4f && (walkKey || stick < StrollStick)) Gait = 3;
        SprintHeld = !Crouched && (ScriptedMove ? ScriptRun : sprintKey && stick > 1e-4f && (sprintPressed || SprintHeld));
        if (SprintHeld && GaitSpeeds.Forward[4] > 0.0f) Gait = 4;
        if (stick > 1e-4f) wish /= stick;
        wish *= PlayerGaitSpeed(GaitSpeeds, MoveInput, Gait);
    } else {
        SprintHeld = false;
        wish *= MoveSpeed * (sprint ? SprintMultiplier : 1.0f) * (Crouched ? CrouchSpeedMultiplier : 1.0f);
    }
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
    m_JumpBuffer = jumpDown ? JumpBufferTime : std::max(0.0f, m_JumpBuffer - dt);
    if (m_JumpBuffer > 0.0f && (wasGrounded || m_SinceGrounded < CoyoteTime) && Velocity.y <= 0.1f) {
        const bool canStand = !Crouched || (hasCharacter && PhysicsWorld::CharacterFitsAt(stand));
        if (canStand) {
            if (Crouched) { PhysicsWorld::ResizeCharacter(stand); Crouched = false; }
            Velocity.y = JumpSpeed; Jumped = true; m_JumpBuffer = 0.0f; m_SinceGrounded = 1000.0f;
        }
    }
    Velocity.y += Gravity * dt;
    const glm::vec3 feet = Cam.Position - glm::vec3(0.0f, eye, 0.0f);
    if (!PhysicsWorld::IsActive()) {
        // No physics: nothing to stand on, so the fall ends at KillY like the mover's (it used to fall forever, ever faster).
        Cam.Position += Velocity * dt;
        Grounded = false;
        if (Cam.Position.y < KillY) {
            Cam.Position = RespawnFeet + glm::vec3(0.0f, eye, 0.0f);
            Velocity = glm::vec3(0.0f);
        }
        return;
    }
    if (!hasCharacter) PhysicsWorld::CreateCharacter(radius, stand, &feet.x);
    PhysicsWorld::SetCharacterDoorwayAssist(DoorwayAssist, DoorwayAssistMaxAngle);
    const unsigned flags = MoveCharacter(Velocity * dt, dt, wasGrounded && !Jumped && Velocity.y <= 0.0f);
    float moverVelocity[3], stepDelta = 0.0f;
    bool moverGrounded = false;
    // The mover slid the velocity along what it hit and followed the floor down itself.
    const bool mover = PhysicsWorld::LastMoverResult(moverVelocity, &moverGrounded, &stepDelta);
    if (mover && (GroundAccelTime > 0.0f || GroundDecelTime > 0.0f || AirAccelTime > 0.0f)) { Velocity.x = moverVelocity[0]; Velocity.z = moverVelocity[2]; }
    Grounded = mover && moverGrounded;
    if ((Grounded && Velocity.y < 0.0f) || ((flags & PhysicsWorld::CC_UP) && Velocity.y > 0.0f)) Velocity.y = 0.0f;
    Cam.Yaw += PhysicsWorld::PlatformYawDelta();
    // A step up or down: the eye eases over it (a critically damped spring, ~0.15 s) rather than jumping with the feet.
    {
        const float w = 20.0f;
        const float x = std::clamp(StepEase - stepDelta, -0.4f, 0.4f), v = StepEaseRate;
        const float e = std::exp(-w * dt), k = (v + w * x) * dt;
        StepEase = (x + k) * e;
        StepEaseRate = (v - w * k) * e;
        if (std::abs(StepEase) < 1e-4f && std::abs(StepEaseRate) < 1e-3f) StepEase = StepEaseRate = 0.0f;
    }
    Cam.Position = CharacterFeet() + glm::vec3(0.0f, eye + StepEase, 0.0f);
    if (Cam.Position.y < KillY) {
        SetCharacterFeet(RespawnFeet);
        Cam.Position = RespawnFeet + glm::vec3(0.0f, eye, 0.0f);
        Velocity = glm::vec3(0.0f);
        StepEase = StepEaseRate = 0.0f;
    }
}

float PlayerGaitSpeed(const Player::DirectionalSpeeds& s, const glm::vec2& local, int gait) {
    gait = std::clamp(gait, 0, 4);
    const float len = glm::length(local);
    if (len < 1e-5f) return s.Forward[gait];
    const float off = glm::degrees(std::acos(std::clamp(local.y / len, -1.0f, 1.0f))); // 0 ahead .. 180 behind
    const float t = std::clamp(off / std::max(s.StrafeAngle, 1.0f), 0.0f, 1.0f);
    const float w = t * t * (3.0f - 2.0f * t);
    const float side = s.Forward[gait] + (s.Strafe[gait] - s.Forward[gait]) * w;
    const float back = s.Back[gait] > 0.0f ? s.Back[gait] : s.Strafe[gait];
    const float u = std::clamp((off - 90.0f) / 90.0f, 0.0f, 1.0f);
    return side + (back - s.Strafe[gait]) * u * u * (3.0f - 2.0f * u);
}

glm::vec3 PlayerApproachVelocity(const glm::vec3& current, const glm::vec3& target, float dt, float accelTime, float decelTime) {
    // Speeding up toward the target uses the acceleration time; slowing down (letting go, or turning
    // to a slower move) the deceleration time.
    const float t = glm::length(target) >= glm::length(current) ? accelTime : decelTime;
    if (!(t > 0.0f) || !(dt > 0.0f)) return target;
    const glm::vec3 next = current + (target - current) * (1.0f - std::exp(-dt / t));
    return glm::length(target - next) < 0.01f ? target : next; // arrive, rather than creep for ever
}
