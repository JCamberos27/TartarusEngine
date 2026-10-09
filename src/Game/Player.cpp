#include "Player.h"
#include "Scripting/ScriptRuntime.h"
#include "Scripting/GameFrames.h"
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

void Player::Update(float dt, World& world, GLFWwindow* window, bool readInput) {
    (void)world; (void)window;
    Scripting::PlayerFrame f;
    f.Dt=std::max(0.0f,dt); f.Yaw=Cam.Yaw; f.Pitch=Cam.Pitch;
    f.SizeX=Size.x; f.SizeY=Size.y; f.ReadInput=readInput;
    f.MoveX=ScriptedMove?ScriptMove.x:(readInput?InputMap::GetAxis("Horizontal"):0.0f);
    f.MoveY=ScriptedMove?ScriptMove.y:(readInput?InputMap::GetAxis("Vertical"):0.0f);
    f.Sprint=!SprintBlocked && (ScriptedMove?ScriptSprint:(readInput&&InputMap::GetButton("Sprint")));
    f.Crouch=ScriptedMove?ScriptCrouch:(readInput&&InputMap::GetButton("Crouch"));
    f.JumpDown=readInput&&InputMap::GetButtonDown("Jump"); f.AimHeld=AimHeld;
    LookDeltaInput=glm::vec2(0);
    if(readInput) {
        const float mousePitch=static_cast<float>(Input::GetMouseDeltaY())*(InvertY?-1.0f:1.0f)*MouseSensitivity;
        const float stickPitch=-Input::GetGamepadAxis(GLFW_GAMEPAD_AXIS_RIGHT_Y)*(InvertY?-1.0f:1.0f)*StickLookDegPerSec*dt;
        f.LookPitch=std::clamp(std::clamp(Cam.Pitch+mousePitch,-89.0f,89.0f)+stickPitch,-89.0f,89.0f)-Cam.Pitch;
        f.LookYaw=static_cast<float>(Input::GetMouseDeltaX())*MouseSensitivity+Input::GetGamepadAxis(GLFW_GAMEPAD_AXIS_RIGHT_X)*StickLookDegPerSec*dt;
        LookDeltaInput={mousePitch+stickPitch,f.LookYaw};
    }
    f.EyeHeight=EyeHeight;
    f.MoveSpeed=MoveSpeed;
    f.SprintMultiplier=SprintMultiplier;
    f.JumpSpeed=JumpSpeed;
    f.Gravity=Gravity;
    f.MouseSensitivity=MouseSensitivity;
    f.StickLookDegPerSec=StickLookDegPerSec;
    f.KillY=KillY;
    // A sprint clip can keep contributing fast root travel during its exit fade.
    // While reloading, walk input owns travel so that residual cannot keep us sprinting.
    f.RootMotionWeight=SprintBlocked?0.0f:RootMotionWeight;
    f.MaxYawRate=MaxYawRate;
    f.YawFreeCenter=YawFreeCenter;
    f.YawFreeRange=YawFreeRange;
    f.CrouchHeight=CrouchHeight;
    f.CrouchSpeedMultiplier=CrouchSpeedMultiplier;
    f.JumpBufferTime=JumpBufferTime;
    f.CoyoteTime=CoyoteTime;
    f.GroundAccelTime=GroundAccelTime;
    f.GroundDecelTime=GroundDecelTime;
    f.AirAccelTime=AirAccelTime;
    f.CrouchBlend=CrouchBlend;
    f.CrouchBlendRate=CrouchBlendRate;
    f.YawDropped=YawDropped;
    f.Position={Cam.Position.x,Cam.Position.y,Cam.Position.z};
    f.Velocity={Velocity.x,Velocity.y,Velocity.z};
    f.RespawnFeet={RespawnFeet.x,RespawnFeet.y,RespawnFeet.z};
    f.RootMotionVelocity={RootMotionVelocity.x,RootMotionVelocity.y,RootMotionVelocity.z};
    f.WishVelocity={WishVelocity.x,WishVelocity.y,WishVelocity.z};
    f.SinceGrounded=m_SinceGrounded; f.JumpBuffer=m_JumpBuffer;
    f.Grounded=Grounded; f.Crouched=Crouched; f.Jumped=Jumped;
    if(!Scripting::InvokeProject("player",&f,sizeof f)) return;
    Cam.Yaw=f.Yaw; Cam.Pitch=f.Pitch; MoveInput={f.MoveX,f.MoveY};
    m_SinceGrounded=f.SinceGrounded; m_JumpBuffer=f.JumpBuffer;
    Grounded=f.Grounded!=0; Crouched=f.Crouched!=0; Jumped=f.Jumped!=0;
    CrouchBlend=f.CrouchBlend; CrouchBlendRate=f.CrouchBlendRate; YawDropped=f.YawDropped;
    Cam.Position={f.Position.x,f.Position.y,f.Position.z};
    Velocity={f.Velocity.x,f.Velocity.y,f.Velocity.z};
    WishVelocity={f.WishVelocity.x,f.WishVelocity.y,f.WishVelocity.z};
}

glm::vec3 PlayerApproachVelocity(const glm::vec3& current, const glm::vec3& target, float dt, float accelTime, float decelTime) {
    // Speeding up toward the target uses the acceleration time; slowing down (letting go, or turning
    // to a slower move) the deceleration time.
    const float t = glm::length(target) >= glm::length(current) ? accelTime : decelTime;
    if (!(t > 0.0f) || !(dt > 0.0f)) return target;
    const glm::vec3 next = current + (target - current) * (1.0f - std::exp(-dt / t));
    return glm::length(target - next) < 0.01f ? target : next; // arrive, rather than creep for ever
}
