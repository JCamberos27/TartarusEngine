// The frames the native gameplay rules read and write: WeaponRules (weapon, loadout) and BodyLocomotion.
#pragma once
#include <cstdint>
namespace Gameplay {
struct Vec3 { float x=0, y=0, z=0; };
struct BodyMotionFrame {
    Vec3 Velocity{};
    Vec3 WishVelocity{};
    Vec3 Move{};
    Vec3 LastDir{};
    Vec3 StartDir{};
    Vec3 StopDir{};
    Vec3 PivotDir{};
    Vec3 StepDir{};
    std::int32_t Grounded{};
    std::int32_t Crouched{};
    std::int32_t WasCrouched{};
    std::int32_t StartStopClips{};
    std::int32_t Turning{};
    std::int32_t IsTurn{};
    std::int32_t IsCrouchTurn{};
    std::int32_t IsLocomotion{};
    std::int32_t IsCrouchLoco{};
    std::int32_t Jumped{};
    std::int32_t Still{};
    std::int32_t SetTurnAngle{};
    std::int32_t LastSprint{};
    std::int32_t Moving{};
    std::int32_t Sprint{};
    std::int32_t Airborne{};
    std::int32_t Armed{};
    std::int32_t Busy{};
    std::int32_t IsStart{};
    std::int32_t IsStartTurn{};
    std::int32_t IsStop{};
    std::int32_t IsPivot{};
    std::int32_t IsStep{};
    std::int32_t IsFidget{};
    std::int32_t IsSprint{};
    std::int32_t PivotReversed{};
    float Dt{};
    float ViewYaw{};
    float Yaw{};
    float Twist{};
    float AirTime{};
    float IdleTime{};
    float MoveTime{};
    float TurnTime{};
    float TurnDone{};
    float RootYaw{};
    float StateTime{};
    float PlayerRunSpeed{};
    float PlayerSprintSpeed{};
    float RunSpeed{};
    float ClipSprint{};
    float Responsiveness{};
    float MaxPlayRate{};
    float ParamSmoothing{};
    float TurnThreshold{};
    float TurnMinTime{};
    float TurnEndAngle{};
    float TurnTimeout{};
    float TurnMoveEase{};
    float TurnLagMargin{};
    float TurnLagFloor{};
    float StopDebounce{};
    float StartIdleTime{};
    float StartMaxMove{};
    float StopMinRunTimeCrouched{};
    float StopMinRunTime{};
    float StopMinSpeedCrouched{};
    float StopMinSpeed{};
    float StopRunForward{};
    float AirborneDelay{};
    float PlayRate{};
    float TurnAngle{};
    float AccelTime{};
    float DecelTime{};
    float Random{};
    float StartGait{};
    float StopGait{};
    float PivotGait{};
    float StartTurn{};
    float StartTurnAmount{};
    float StartDistance{};
    float StopDistance{};
    float PivotDistance{};
    float PivotTravel{};
    float FidgetTime{};
    float FidgetNext{};
    float FidgetIndex{};
    float SprintRate{};
    float SprintClip{};
    float MoveDistance{};
    std::int32_t Triggers{};
};
struct WeaponFrame {
    float Dt{};
    float Rpm{};
    float CycleDelay{};
    float HoldSeconds{};
    float RegripMin{};
    float RegripMax{};
    float Cooldown{};
    float CycleWait{};
    float IdleTime{};
    float RegripDelay{};
    float SinceShot{};
    float SinceUnhidden{};
    float ReloadHeldSeconds{};
    float Random{};
    std::int32_t Operation{};
    std::int32_t Ammo{};
    std::int32_t Magazine{};
    std::int32_t BurstRounds{};
    std::int32_t AllowFullAuto{};
    std::int32_t PerRound{};
    std::int32_t CycleAfterShot{};
    std::int32_t HipProcedural{};
    std::int32_t RecoilProfile{};
    std::int32_t UnityRecoil{};
    std::int32_t Equipped{};
    std::int32_t Chambered{};
    std::int32_t CycleSeen{};
    std::int32_t StopReload{};
    std::int32_t FullAuto{};
    std::int32_t BurstRemaining{};
    std::int32_t WaitingShot{};
    std::int32_t Tags{};
    std::int32_t InTransition{};
    std::int32_t WallBlocked{};
    std::int32_t Pressed{};
    std::int32_t Held{};
    std::int32_t ReloadWasDown{};
    std::int32_t ReloadHoldFired{};
    std::int32_t Events{};
    std::int32_t Commands{};
    std::int32_t Result{};
};
struct WeaponLoadoutFrame {
    std::int32_t Active{};
    std::int32_t Equipped{};
    std::int32_t Requested{};
    std::int32_t Chambered{};
    std::int32_t Hidden{};
    std::int32_t InTransition{};
    float CycleWait{};
    std::int32_t Operation{};
    std::int32_t Value{};
    std::int32_t Count{};
    std::int32_t Slot{};
    std::int32_t Pending{};
    std::int32_t Ammo{};
    std::int32_t Magazine{};
    std::int32_t Burst{};
    std::int32_t Commands{};
    std::uintptr_t SlotAmmo{};
};

// Identifies these structs to a hot-reloaded game module (GameModuleAPI::FramesLayout): the host
// only calls a module's rules when the module was built against the same frames. Bump
// kFramesRevision whenever a field is added, removed, retyped or reordered.
constexpr std::uint32_t kFramesRevision = 1;
constexpr std::uint32_t FramesLayout() {
    return kFramesRevision * 0x9E3779B1u ^ (static_cast<std::uint32_t>(sizeof(BodyMotionFrame)) * 0x10000u +
                                             static_cast<std::uint32_t>(sizeof(WeaponFrame)) * 0x100u +
                                             static_cast<std::uint32_t>(sizeof(WeaponLoadoutFrame)));
}
}
