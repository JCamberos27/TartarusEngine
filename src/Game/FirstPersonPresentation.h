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
struct FirstPersonControllerComponent;

// Runtime-only owner for a camera-bound pair of first-person rigs. It creates neither a second
// physics body nor persistent scene objects: the PhysX character remains Player's authority.
// The visual entities exist only between Play and Stop.
class FirstPersonPresentation {
public:
    bool Start(World& world, AssetLibrary& assets, const FirstPersonControllerComponent& config);
    void Stop(World& world);

    // Keeps the presentation in camera space. The renderer draws these rigs in its own
    // view-model sub-pass — see ViewModelFov() below — so the world camera's FOV is never
    // touched; ViewModelOffset is a residual nudge ({0,0,0} for the source set, because the
    // CameraBone anchor already puts the rig's head bone exactly on the camera).
    void Update(World& world, const Camera& camera);

    // Vertical FOV in degrees for that sub-pass, or a non-positive value when there is nothing
    // to put in it. A caller passing this straight through as RenderFrameContext::ViewModelFov
    // therefore gets the dedicated pass while Play is running with an animation set, and falls
    // back to the plain world pass (including drawing the entities normally, in the editor Scene
    // tab) when it isn't. Read-only: only Start()/Stop() ever change it.
    float ViewModelFov() const { return IsActive() ? m_ViewModelFov : -1.0f; }

    // Plays a semantic state on both rigs at the same simulation instant. A state without a
    // weapon clip intentionally restores the weapon's bind pose rather than guessing a clip.
    bool SetState(const std::string& state);

    // Per-frame gameplay drive: call once per simulated frame after Update(). While a triggered
    // one-shot (see TriggerAction) or an Idle<->Sprint transition clip is still playing, it holds
    // - locomotion/aim only resolve once it finishes (Model auto-returns a Once clip to the bind
    // pose, which is what IsBusy() below reads). `aiming` holds the Aim pose even while moving
    // (with a procedural walk bob; there is no aim-walk clip). `dt` drives the idle Regrip
    // timer, the full-auto cooldown, the ADS recoil kick and that bob.
    // Also where a finished reload refills the magazine and a finished Holster hides the rigs.
    void Tick(float dt, float planarSpeed, bool sprinting, bool aiming);

    // Fire, Inspect, MagCheck, TacReload, EmptyReload, Melee, Draw, Holster, Regrip. False is a
    // plain "not right now" (blocked by a higher-or-equal priority action already playing, or
    // Draw/Holster requested while already in that equip state) - not an error; LastError stays
    // empty. See FirstPersonTierOf/FirstPersonCanInterrupt for the priority rules. Gameplay goes
    // through Fire()/Reload()/SetEquipped() below, which add the ammo rules on top.
    bool TriggerAction(const std::string& state);

    // Spends a round. At the hip that plays the Fire clip; while settled in the Aim pose it keeps
    // the pose and applies a procedural kick instead, because the source set has no ADS fire clip
    // and the hip clip would yank the sights off centre every shot. A dry trigger pull does
    // nothing - reloading is always the player's call. False when nothing was fired.
    bool Fire();
    // The trigger, once per simulated frame. Semi-auto fires on the press only; full-auto fires
    // on the press and then every kFullAutoInterval for as long as it is held.
    void UpdateTrigger(bool pressed, bool held);
    void ToggleFireMode(); // logs the new mode to the Console (there is no weapon HUD yet)
    bool IsFullAuto() const { return m_FullAuto; }
    static constexpr float kFullAutoInterval = 60.0f / 700.0f; // AKS-74U cyclic rate, ~700 rpm
    // TacReload with rounds left, EmptyReload on an empty magazine, nothing when it is full or a
    // reload is already running. The magazine refills when the clip completes, so a reload cut
    // short by Holster leaves the count as it was.
    bool Reload();
    // Draw (true) or Holster (false). Holstered means unarmed: once the Holster clip ends both
    // rigs are hidden (no unarmed arms pose exists) until the next Draw.
    bool SetEquipped(bool equipped);

    static constexpr int kMagazineSize = 30;
    int Ammo() const { return m_Ammo; }

    bool IsActive() const { return m_Arms != entt::null; }
    bool IsBusy() const { return !m_ActionState.empty(); }
    bool IsEquipped() const { return m_Equipped; }
    const std::string& CurrentState() const { return m_CurrentState; }
    const std::string& LastError() const { return m_LastError; }

private:
    bool AttachAndValidate(AssetLibrary& assets);
    bool Play(const FirstPersonAnimationClip& clip);
    // Unconditional: always restarts the clip from time 0, even if it names the current state
    // (Fire's "tap again to restart"). SetState()/Tick() dedupe before calling this; TriggerAction
    // relies on the restart.
    bool StartAction(const std::string& state, FirstPersonActionTier tier);
    bool ActionFinished() const;
    void SetError(const std::string& message);

    FirstPersonAnimationSet m_Set;
    entt::entity m_Arms = entt::null;
    entt::entity m_Weapon = entt::null;
    std::shared_ptr<Model> m_ArmsModel;
    std::shared_ptr<Model> m_WeaponModel;
    glm::vec3 m_Offset{0.0f};
    glm::vec3 m_Rotation{0.0f};
    float m_Scale = 1.0f;
    float m_ViewModelFov = -1.0f; // authored in Start(), read by ViewModelFov()
    std::string m_CameraBone;   // rig node the play camera is pinned to (empty = root-anchored)
    bool m_CameraBoneWarned = false;
    bool m_WeaponSocketWarned = false;
    std::string m_CurrentState;
    std::string m_LastError;

    // Which rig(s) gate ActionFinished() for the currently-playing one-shot, set by StartAction
    // from the same clip fields Play() used (a bind-pose/empty-clip side never "finishes").
    bool m_ActionGateArms = false;
    bool m_ActionGateWeapon = false;
    std::string m_ActionState;               // non-empty while a one-shot/transition holds
    FirstPersonActionTier m_ActionTier = FirstPersonActionTier::None;
    bool m_Equipped = true;                  // no holstered-idle pose exists, so play starts equipped
    bool m_Hidden = false;                   // unarmed: set once Holster ends, cleared by Draw
    bool m_HiddenApplied = false;            // what Update() last pushed onto the entities

    int m_Ammo = kMagazineSize;
    bool m_RefillOnFinish = false;           // the running action is a reload that hasn't landed
    float m_RecoilTime = -1.0f;              // seconds since the last ADS shot, < 0 = at rest
    bool m_FullAuto = false;
    float m_FireCooldown = 0.0f;             // full-auto: seconds until the next round may go
    float m_AimBobWeight = 0.0f;             // 0..1, eased in while moving in the Aim pose
    float m_AimBobPhase = 0.0f;              // radians along the stride
    float m_IdleTime = 0.0f;                 // uninterrupted Idle, for the Regrip fidget
    float m_RegripDelay = 15.0f;
    std::mt19937 m_Rng{std::random_device{}()};
};
