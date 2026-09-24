#pragma once

#include "FirstPersonAdsCarry.h"
#include "FirstPersonAnimation.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <filesystem>
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
//
// Procedural motion (recoil, sway, bob, breathing, aim, state offsets, lean) runs through
// WeaponProceduralState and lands on the arms rig's gun bone as an IK Rig offset, with two-bone
// IK keeping the hands on the gun. The weapon rides that bone, so it follows. The view punch
// and lean go on the play camera (ApplyViewKick / RemoveViewKick).
class FirstPersonPresentation {
public:
    bool Start(World& world, AssetLibrary& assets, const FirstPersonControllerComponent& config);
    void Stop(World& world);

    // Keeps the presentation in camera space, and takes in what the animator reported last
    // frame (fired events, the current state's tags). Call once per simulated frame, before the
    // input calls below and Tick(). The renderer draws these rigs in its own view-model
    // sub-pass - see ViewModelFov(); the world camera's FOV only changes through WorldFov().
    // Also applies this frame's view punch and lean to `camera` (see RemoveViewKick).
    void Update(World& world, Camera& camera);
    // Re-places the arms and weapon from this frame's finished pose (clips + IK). Call after
    // UpdateAnimatorControllers, so the gun sits in the hands the frame renders with.
    void LateUpdate(World& world, const Camera& camera);
    // Takes the view punch / lean back off the camera. Call before anything reads or integrates
    // the camera as the player's own (Player::Update), and when Play stops.
    void RemoveViewKick(Camera& camera);

    // Vertical FOV in degrees for that sub-pass, or a non-positive value when there is nothing
    // to put in it (no animation set is running). Only Start()/Stop() change it.
    // Narrowed by the ADS zoom while the sights are up.
    float ViewModelFov() const;
    // The world camera's vertical FOV for `baseFov` under the ADS zoom, and the matching mouse
    // look scale (the ratio of the two view widths, so the sights track the same per pixel).
    float WorldFov(float baseFov) const;
    float LookScale(float baseFov) const;

    // Per-frame locomotion + timers. Sets the controller's Speed/Sprint/Aim/Equipped/Ammo, runs
    // the Fidget timer, and advances the procedural stack. `velocity` is the player's world
    // velocity; `lean` is -1 (left) .. +1 (right).
    void Tick(float dt, const glm::vec3& velocity, bool sprinting, bool aiming, float lean = 0.0f, bool grounded = true);

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
    // Where the barrel points (world space): down the bore from the muzzle to the first surface.
    // False while the gun isn't simply held at the hip (ADS, sprinting, reloading, holstered).
    bool BarrelAimPoint(glm::vec3& out) const;
    const std::string& CurrentState() const;
    const std::string& LastError() const { return m_LastError; }
    const WeaponProceduralPose& ProceduralPose() const { return m_Procedural.Pose(); }
    // The weapon definition this is running, and whether IK carries the procedural motion (the
    // arms rig has the gun bone and both arm chains) or the whole view model does.
    const FirstPersonAnimationSet& Set() const { return m_Set; }
    bool UsesIK() const { return m_UsesIK; }
    // What the ADS carry measured at Start (and on live edits): per carried state, the gun move
    // onto the sights and the arm matching - for the weapon Inspector.
    const AdsCarryReport& AdsReport() const { return m_AdsCarry.Report; }

private:
    bool AttachAndValidate(AssetLibrary& assets, const AnimatorController& ctrl);
    AnimatorControllerComponent* Animator() const;
    bool HasTag(const char* tag) const;
    void SetError(const std::string& message);
    bool SetupIK();
    void WriteIK();
    void PlaceRigs(World& world, const Camera& camera);
    void ApplyHidden(World& world);
    void WriteAdsHold(IKRigComponent& rig, const AdsCarrySample& carry);
    void SetupBolt(AssetLibrary& assets, const AnimatorController& ctrl);
    void SetupMuzzle(int bolt, const std::vector<int>& parents);
    void SetupAdsCarry();
    AdsCarrySample SampleAdsCarry(float dt) const;
    void ShotImpact(); // a round leaves the bore: shove whatever it hits
    void ReloadIfChanged(float dt);

    World* m_World = nullptr;
    FirstPersonAnimationSet m_Set;
    std::string m_ControllerPath;
    AssetLibrary* m_Assets = nullptr;                       // for re-measuring on live edits
    std::shared_ptr<const AnimatorController> m_Controller;
    entt::entity m_Arms = entt::null;
    entt::entity m_Weapon = entt::null;
    std::shared_ptr<Model> m_ArmsModel;
    std::shared_ptr<Model> m_WeaponModel;
    glm::vec3 m_Offset{0.0f};
    glm::vec3 m_Rotation{0.0f};
    float m_Scale = 1.0f;
    float m_WalkSpeed = 0.0f;     // the controller's full walk / sprint speeds (m/s)
    float m_SprintSpeed = 0.0f;
    float m_ViewModelFov = -1.0f; // authored in Start(), read by ViewModelFov()
    std::string m_CameraBone;     // rig node the play camera is pinned to (empty = root-anchored)
    bool m_CameraBoneWarned = false;
    bool m_WeaponSocketWarned = false;
    std::string m_LastError;

    bool m_Equipped = true;       // what the player asked for (the controller catches up)
    bool m_HiddenApplied = false; // what Update() last pushed onto the entities
    float m_WallDistance = -1.0f; // Update: metres to what's in front of the gun (< 0 = nothing near)
    bool m_WallFacesUp = false;
    // Corner peek: -1 (out left) .. 1 (out right), from UpdateCornerPeek; the side picked when
    // the aim started (0 = none yet), and whether the player was aiming last Tick.
    float m_PeekLean = 0.0f;
    int m_PeekSide = 0;
    glm::vec3 m_PeekFrom{0.0f}; // where the eye was when the peek side was picked
    bool m_Aiming = false;
    void UpdateCornerPeek(const Camera& camera);
    float m_WallSide = 0.0f;      // its surface normal along the view's right (-1..1)
    // Tucked far enough off a wall that the gun can't fire or aim.
    bool WallBlocked() const {
        const float at = m_Set.Procedural.Obstruction.BlockAt;
        return at > 0.0f && m_Procedural.Pose().Obstruction >= at;
    }
    bool m_AdsHolding = false;    // WriteAdsHold: the aim pose is holding the rig
    float m_AdsAimTime = 0.0f;    // ... how far into the aim clip it is
    float m_AdsHoldLast = 0.0f;   // ... its weight while the action played
    float m_AdsReleaseT = 0.0f;   // ... and seconds since the action faded out
    int m_Ammo = 30;
    bool m_FullAuto = false;
    float m_FireCooldown = 0.0f;  // full-auto: seconds until the next round may go
    float m_IdleTime = 0.0f;      // settled Idle, for the Fidget

    // Procedural stack. Look rate comes from the camera's yaw/pitch between Updates, velocity
    // is taken into the camera's flat frame there too.
    WeaponProceduralState m_Procedural;
    bool m_UsesIK = false;
    glm::vec3 m_BoltStroke{0.0f}; // weapon model space, from the Fire clip (SetupBolt)
    bool m_HaveMuzzle = false;
    glm::vec3 m_MuzzleLocal{0.0f}, m_BoreLocal{0.0f, 0.0f, -1.0f}; // weapon root space
    glm::vec3 m_AimPoint{0.0f};
    glm::vec3 m_Muzzle{0.0f}, m_BoreDir{0.0f, 0.0f, -1.0f}; // world, from the last PlaceRigs
    // Actions carried onto the sights while aiming (FirstPersonAdsCarry.h).
    AdsCarryResult m_AdsCarry;
    float m_AdsHold = 0.0f;       // 0..1, eased toward "aim held" over Ads.AimHoldTime
    float m_TickDt = 0.0f;        // the last Tick's dt: the step the animators take next
    // IK rig offset slots on the arms: the ADS-action gun correction, then the procedural pose.
    static constexpr int kAdsOffset = 0, kProceduralOffset = 1;
    float m_Zoom = 0.0f, m_ZoomRate = 0.0f; // ADS zoom 0..1, critically damped spring
    bool m_AimPointValid = false;
    float m_LookYaw = 0.0f, m_LookPitch = 0.0f, m_PrevLookYaw = 0.0f, m_PrevLookPitch = 0.0f;
    bool m_HaveLook = false;
    glm::vec3 m_FlatRight{1.0f, 0.0f, 0.0f}, m_FlatForward{0.0f, 0.0f, -1.0f};
    // The view kick currently on the camera (so it can come off exactly).
    bool m_KickApplied = false;
    glm::vec3 m_KickAngles{0.0f}; // pitch, yaw, roll degrees
    glm::vec3 m_KickOffset{0.0f}; // world
    // Live retuning: the .fpsanim is re-read when it changes on disk (the Inspector saves it).
    std::filesystem::path m_SetFile;
    std::filesystem::file_time_type m_SetFileTime{};
    float m_ReloadPoll = 0.0f;
    float m_RegripDelay = 15.0f;
    FirstPersonReloadButton m_ReloadKey;
    std::mt19937 m_Rng{std::random_device{}()};
};
