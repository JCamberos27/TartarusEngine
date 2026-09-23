#pragma once

#include "FirstPersonAnimation.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <random>
#include <string>

class AssetLibrary;
class Camera;
class Model;
class World;
struct AnimatorControllerComponent;
struct FirstPersonControllerComponent;

// Runtime-only owner for a camera-bound pair of first-person rigs. It creates neither a second
// physics body nor persistent scene objects: the PhysX character remains Player's authority.
// The visual entities exist only between Play and Stop.
//
// Animation is the weapon's Animator Controller: the arms entity runs it (track "arms") and the
// weapon entity follows it in lockstep (track "weapon", Driver = arms). This class is the
// gameplay on top - ammo, fire modes, the reload key, the idle fidget timer - and talks to the
// controller only through FirstPersonAnimatorContract's parameters, tags and events.
class FirstPersonPresentation {
public:
    bool Start(World& world, AssetLibrary& assets, const FirstPersonControllerComponent& config);
    void Stop(World& world);

    // Keeps the presentation in camera space, and takes in what the animator reported last
    // frame (fired events, the current state's tags). Call once per simulated frame, before the
    // input calls below and Tick(). The renderer draws these rigs in its own view-model
    // sub-pass - see ViewModelFov() - so the world camera's FOV is never touched.
    void Update(World& world, const Camera& camera);

    // Vertical FOV in degrees for that sub-pass, or a non-positive value when there is nothing
    // to put in it (no animation set is running). Only Start()/Stop() change it.
    float ViewModelFov() const { return IsActive() ? m_ViewModelFov : -1.0f; }

    // Per-frame locomotion + timers. Sets the controller's Speed/Sprint/Aim/Equipped/Ammo, runs
    // the Fidget timer, and advances the ADS recoil and walk bob.
    void Tick(float dt, float planarSpeed, bool sprinting, bool aiming);

    // Input. Each is a plain "not right now" false when the weapon isn't in hand (or has nothing
    // to do); the controller decides what can interrupt what.
    //
    // Fire: in a state tagged ADS it keeps the sight picture and kicks the view model
    // procedurally (the source set has no ADS fire clip; the hip clip would pull the sights off
    // centre). Otherwise it sets the Fire trigger, and the round is spent when the controller's
    // Shot event fires. A dry trigger does nothing - reloading is always the player's call.
    bool Fire();
    // The trigger, once per simulated frame. Semi-auto fires on the press only; full-auto on the
    // press and then every 60/rpm seconds while held.
    void UpdateTrigger(bool pressed, bool held);
    void ToggleFireMode(); // logs the new mode to the Console (there is no weapon HUD yet)
    bool IsFullAuto() const { return m_FullAuto; }
    // The R key: tap = Reload, hold = MagCheck (see FirstPersonReloadButton).
    void UpdateReloadKey(bool down, float dt);
    void ResetReloadKey() { m_ReloadKey = {}; m_ReloadKey.HoldSeconds = m_Set.Gameplay.ReloadHoldSeconds; }
    // Sets the Reload trigger when the magazine isn't full and no reload is running; the
    // magazine refills on the controller's Refill event (so a reload cut short doesn't count).
    bool Reload();
    // Sets a one-frame trigger (MagCheck, Inspect, Melee, ...) on the controller.
    bool TriggerAction(const std::string& trigger);
    // Wants the weapon in hand (true) or holstered (false). The controller plays Draw/Holster;
    // while it is in a state tagged Hidden both rigs are hidden.
    void SetEquipped(bool equipped);

    int Ammo() const { return m_Ammo; }
    int MagazineSize() const { return m_Set.Gameplay.Magazine; }
    bool IsActive() const { return m_Arms != entt::null; }
    bool IsEquipped() const { return m_Equipped; }
    const std::string& CurrentState() const;
    const std::string& LastError() const { return m_LastError; }

private:
    bool AttachAndValidate(AssetLibrary& assets, const AnimatorController& ctrl);
    AnimatorControllerComponent* Animator() const;
    bool HasTag(const char* tag) const;
    void SetError(const std::string& message);

    World* m_World = nullptr;
    FirstPersonAnimationSet m_Set;
    std::string m_ControllerPath;
    entt::entity m_Arms = entt::null;
    entt::entity m_Weapon = entt::null;
    std::shared_ptr<Model> m_ArmsModel;
    std::shared_ptr<Model> m_WeaponModel;
    glm::vec3 m_Offset{0.0f};
    glm::vec3 m_Rotation{0.0f};
    float m_Scale = 1.0f;
    float m_ViewModelFov = -1.0f; // authored in Start(), read by ViewModelFov()
    std::string m_CameraBone;     // rig node the play camera is pinned to (empty = root-anchored)
    bool m_CameraBoneWarned = false;
    bool m_WeaponSocketWarned = false;
    std::string m_LastError;

    bool m_Equipped = true;       // what the player asked for (the controller catches up)
    bool m_HiddenApplied = false; // what Update() last pushed onto the entities
    int m_Ammo = 30;
    float m_RecoilTime = -1.0f;   // seconds since the last ADS shot, < 0 = at rest
    bool m_FullAuto = false;
    float m_FireCooldown = 0.0f;  // full-auto: seconds until the next round may go
    float m_AimBobWeight = 0.0f;  // 0..1, eased in while moving in an ADS state
    float m_AimBobPhase = 0.0f;   // radians along the stride
    float m_IdleTime = 0.0f;      // settled Idle, for the Fidget
    float m_RegripDelay = 15.0f;
    FirstPersonReloadButton m_ReloadKey;
    std::mt19937 m_Rng{std::random_device{}()};
};
