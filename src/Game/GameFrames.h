// The frames the native gameplay rules read and write: WeaponRules (weapon, loadout).
#pragma once
#include <cstdint>
namespace Gameplay {
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
constexpr std::uint32_t kFramesRevision = 3; // 2: BodyMotionFrame gone (Phase 6); 3: WeaponFrame::UnityRecoil gone
constexpr std::uint32_t FramesLayout() {
    return kFramesRevision * 0x9E3779B1u ^ (static_cast<std::uint32_t>(sizeof(WeaponFrame)) * 0x100u +
                                             static_cast<std::uint32_t>(sizeof(WeaponLoadoutFrame)));
}
}
