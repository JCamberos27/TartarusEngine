#pragma once

#include "FirstPersonAdsCarry.h"
#include "Scripting/GameFrames.h"
#include "Scripting/WeaponAttachments.h"
#include "FirstPersonAnimation.h"
#include "FirstPersonBody.h" // FirstPersonStockLockInput
#include "ThirdPersonWeapon.h"
#include "ShellCasings.h"      // CasingSpawn
#include "MuzzleEffects.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

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
// native presentation adapter for C# gameplay. It exchanges ammo, fire-mode and timer state
// with the script and talks to the animator through its parameters, tags and events.
//
// Procedural motion (recoil, sway, bob, breathing, aim, state offsets, lean) runs through
// WeaponProceduralState and lands on the arms rig's gun bone as an IK Rig offset, with two-bone
// IK keeping the hands on the gun. The weapon rides that bone, so it follows. The view punch
// and lean go on the play camera (ApplyViewKick / RemoveViewKick).
class FirstPersonPresentation {
public:
    // How the rigs are used. The defaults are the player's. An NPC (Npc/NpcWeapon.h) carries the
    // same weapon on a virtual camera at its eyes: OwnerView off draws the gun as ordinary world
    // geometry and the arms rig not at all (the NPC's body takes its hands), HotReload off skips
    // polling the .fpsanim, CornerPeek off leaves leaning to the AI.
    struct Options {
        bool OwnerView = true;
        bool HotReload = true;
        bool CornerPeek = true;
    };
    bool Start(World& world, AssetLibrary& assets, const FirstPersonControllerComponent& config);
    bool Start(World& world, AssetLibrary& assets, const FirstPersonControllerComponent& config, const Options& options);
    // The spent cases' mesh and material of `animationSet` (a .fpsanim) into the asset cache. Every slot's are warmed
    // when it starts; left to the first round fired (ShellCasings::Spawn), the FBX import stalls that frame for up to a second.
    static void WarmEjectAssets(AssetLibrary& assets, const std::string& animationSet);
    static void WarmEjectAssets(AssetLibrary& assets, const FirstPersonAnimationSet& set);
    void PreloadClips(AssetLibrary& assets); // every slot's clip files, imported clip-only in parallel (Play start)
    // Where rounds go instead of down the zeroed bore: from the muzzle toward `point` (pellets
    // spread about that line). Null = the bore, as the player's do. Copied.
    void SetShotTarget(const glm::vec3* point) {
        m_HasShotTarget = point != nullptr;
        if (point) m_ShotTarget = *point;
    }
    void Stop(World& world);

    // Keeps the presentation in camera space, and takes in what the animator reported last
    // frame (fired events, the current state's tags). Call once per simulated frame, before the
    // input calls below and Tick(). The renderer draws these rigs in its own view-model
    // sub-pass - see ViewModelFov(); the world camera's FOV only changes through WorldFov().
    // Also applies this frame's view punch and lean to `camera` (see RemoveViewKick).
    void Update(World& world, Camera& camera);
    // Re-places the arms and weapon from this frame's finished pose (clips + IK). Call after
    // UpdateAnimatorControllers, so the gun sits in the hands the frame renders with.
    void LateUpdate(World& world, Camera& camera);
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
    void SetPlayerLookInput(glm::vec2 pitchYaw) { m_RawLookInput=pitchYaw; m_HaveRawLookInput=true; }
    void SetPlayerMoveInput(glm::vec2 rightForward) { m_SwayMoveInput=rightForward; m_HaveSwayMoveInput=true; }
    // The R key: tap = Reload, hold = MagCheck (see FirstPersonReloadButton).
    void UpdateReloadKey(bool down, float dt);
    void ResetReloadKey() { m_ManagedReloadSeconds=0; m_ManagedReloadDown=false; m_ManagedReloadFired=false; }
    // Sets the Reload trigger when the magazine isn't full and no reload is running; the
    // magazine refills on the controller's Refill event (so a reload cut short doesn't count).
    bool Reload();
    // Sets a one-frame trigger (MagCheck, Inspect, Melee, ...) on the controller.
    bool TriggerAction(const std::string& trigger);
    // Wants the weapon in hand (true) or holstered (false). The controller plays Draw/Holster;
    // while it is in a state tagged Hidden both rigs are hidden. Holstering cancels a pending swap.
    void SetEquipped(bool equipped);

    // Weapon slots: the controller's Animation Set is slot 0, its Secondary Animation Set slot 1.
    // Selecting the slot in hand just draws it; another one holsters this weapon, then swaps the
    // rigs over and draws that one (see Tick). Each slot keeps its own ammo.
    int SlotCount() const { return (int)m_SlotSets.size(); }
    int Slot() const { return m_Slot; }
    void SelectSlot(int slot);
    // The scroll wheel wraps through weapon slots (`step` +1 / -1).
    void CycleSlot(int step);
    void CycleAttachment(Scripting::AttachmentKind kind);
    int AttachmentCount(Scripting::AttachmentKind kind) const { return m_Attachments.Count(kind); }

    int Ammo() const { return m_Ammo; }
    void SetAmmo(int rounds) { m_Ammo = rounds < 0 ? 0 : rounds; } // tests: a part-empty magazine to reload
    // Every slot's magazine full again, the one in hand chambered (a respawn).
    void RefillAmmo();
    // In a reload state (the controller's Reload tag).
    bool IsReloading() const { return HasTag("Reload"); }
    // Accepted reload input and every reload still in the base-layer fade suspend sprint.
    // The caller keeps reading sprint intent, so it resumes when this becomes false.
    bool ReloadBlocksSprint() const;
    // False while the action still has to be worked after a round (gameplay.cycle).
    bool Chambered() const { return m_Chambered; }
    int MagazineSize() const { return m_Set.Gameplay.Magazine; }
    bool IsActive() const { return m_Arms != entt::null; }
    // The runtime arms rig's entity (null when inactive) - the body's arms take their hands from it.
    entt::entity ArmsEntity() const { return m_Arms; }
    entt::entity WeaponEntity() const { return m_Weapon; }
    // The arms rig node the play camera is pinned to (empty = the rig's root sits on the camera).
    const std::string& CameraBone() const { return m_CameraBone; }
    // Mid-swap counts as equipped: the weapon is only being traded for another, so e.g. the gravity
    // gun mustn't take the mouse in between.
    bool IsEquipped() const { return m_Equipped || m_PendingSlot >= 0; }
    // The speeds (m/s) the walk and sprint clips play at: the controller's Move Speed / Sprint Multiplier, or - with a
    // First Person Body - the body's Run / Sprint Speed (what the player really moves at). Call after Start.
    void SetLocomotionSpeeds(float walk, float sprint) { m_WalkSpeed = walk; m_SprintSpeed = sprint; }
    // 0..1 how crouched the player is, for WeaponAimSettings::Crouch* (call before Tick; default 0).
    void SetCrouch(float crouch) { m_Crouch = crouch; }
    // Where the barrel points (world space): down the bore from the muzzle to the first surface.
    // False while the gun isn't simply held at the hip (ADS, sprinting, reloading, holstered).
    bool BarrelAimPoint(glm::vec3& out) const;
    // The gun's laser, straight down the bore: from the muzzle to the first surface it meets
    // (`hit` false = nothing within range; `to` is then the end of the range). On whenever the
    // gun is in hand and not put away, whatever it's doing - it swings with the reloads.
    // The colours are the weapon's laser settings (linear HDR): the beam's and the dot's.
    struct Laser {
        glm::vec3 From{0.0f}, To{0.0f}, Normal{0.0f, 1.0f, 0.0f};
        bool Hit = false;
        glm::vec3 BeamColor{1.1f, 0.025f, 0.015f}, SpotColor{9.0f, 0.2f, 0.12f};
    };
    bool LaserBeam(Laser& out) const;
    // Where the rounds fired since the last call struck (down the bore from the muzzle), oldest
    // first. The caller leaves the holes; the list empties.
    struct ShotHit {
        glm::vec3 Point{0.0f}, Normal{0.0f, 1.0f, 0.0f};
        unsigned Entity = 0xFFFFFFFFu;
        float HoleRadius = 0.0027f; // the weapon's bullet hole, metres (a 5.45 mm round)
        glm::vec3 Origin{0.0f}, Direction{0.0f, 0.0f, -1.0f}; // the ray it came down (muzzle, unit)
        int Pellets = 1;            // rays in the round it belongs to (a shotgun's pellets)
    };
    std::vector<ShotHit> TakeShotHits();
    // Every pellet's line since the last call, hit or not (muzzle flash, tracers, near misses).
    struct ShotTrace {
        glm::vec3 Origin{0.0f}, End{0.0f};
        bool Hit = false;
        bool FirstPellet = true; // the first ray of its round (one flash and one report per round)
        unsigned Entity = 0xFFFFFFFFu;
    };
    std::vector<ShotTrace> TakeShotTraces();
    // The spent cases thrown out of the port since the last call (FirstPersonEjectSettings), for
    // ShellCasings. The list empties.
    std::vector<CasingSpawn> TakeEjections();
    // Cases ejected this Play, and where the last one left (world space) - for --weapon-test.
    int EjectedTotal() const { return m_EjectedTotal; }
    glm::vec3 LastEjectPoint() const { return m_LastEjectPoint; }
    glm::vec3 LastEjectThrow() const { return m_LastEjectThrow; } // its velocity off the player's, m/s
    const std::string& CurrentState() const;
    const std::string& LastError() const { return m_LastError; }
    const WeaponProceduralPose& ProceduralPose() const { return m_Procedural.Pose(); }
    // The weapon definition this is running, and whether IK carries the procedural motion (the
    // arms rig has the gun bone and both arm chains) or the whole view model does.
    const FirstPersonAnimationSet& Set() const { return m_Set; }
    const MuzzleEffectSettings* MuzzleEffects() const {
        return m_Slot>=0 && m_Slot<(int)m_SlotPrefabs.size() && !m_SlotPrefabs[m_Slot].empty()
            ? &m_SlotMuzzles[m_Slot] : nullptr;
    }
    bool UsesIK() const { return m_UsesIK; }
    // What the ADS carry measured at Start (and on live edits): per carried state, the gun move
    // onto the sights and the arm matching - for the weapon Inspector.
    const AdsCarryReport& AdsReport() const { return m_AdsCarry.Report; }
    // How far the ADS hand anchor holds the free hand to the body this frame (0 = on the gun).
    float HandAnchorWeight() const { return m_AnchorWeight; }
    // Where an arms-rig node was drawn last frame, in the camera's view space (-Z ahead; metres).
    bool ArmsNodeInView(const std::string& node, glm::vec3& out) const;
    bool WeaponNodeInView(const std::string& node, glm::vec3& out) const;
    // Diagnostics (--stock-probe): the gun's butt - its mesh's rearmost vertices along the bore,
    // skinned as posed - and the bore's forward, world space, as last drawn.
    bool StockWorld(glm::vec3& butt, glm::vec3& forward) const;
    // Split poses: this frame's first-person gun and how the world copy is placed off it, for
    // FirstPersonBody::ArmsLateUpdate. False with no gun in hand.
    bool WorldGunInput(FirstPersonWorldGunInput& out) const;
    // The first-person gun, rig hands and camera this frame, and what the gun is doing, for the third-person hold
    // (ThirdPersonWeapon). False with no gun in hand.
    bool ThirdPersonFrame(ThirdPersonWeaponFrame& out) const;
    // Split poses: the world copy of the gun (every view but the player's camera, and the shadow) at the
    // first-person gun moved by `worldFromFirstPerson` (rigid, world); the first-person gun is then the player's
    // camera's only. Off (no body to split for): the one gun shows everywhere, as before.
    void PlaceWorldWeapon(World& world, bool split, const glm::mat4& worldFromFirstPerson);
    // The barrel and sight line found this Play (the muzzle, and the sights' measurement while aiming).
    const FirstPersonBarrelReport& BarrelReport() const { return m_Barrel; }
    // Where rounds leave from this frame, world space: the muzzle and the (zeroed) bore.
    float PlanarSpeed() const { return m_PlanarSpeed; } // the player's, from the last Tick (m/s)
    bool MuzzleRay(glm::vec3& origin, glm::vec3& direction) const {
        origin = m_Muzzle;
        direction = m_BoreDir;
        return m_AimPointValid;
    }
    // The muzzle where the gun really is, world space, from the last PlaceRigs (MuzzleRay's is where it's
    // SEEN, stretched by the view-model FOV): the first-person gun's (drawn in the view-model pass), the
    // world copy's every other view sees (PlaceWorldWeapon; the same as the first when poses aren't
    // split), and the bore as modelled.
    bool MuzzleFrames(glm::vec3& firstPerson, glm::vec3& worldCopy, glm::vec3& bore) const;

private:
    // One weapon's rigs up or down; Start / Stop add the slot list around them.
    bool StartSet(World& world, AssetLibrary& assets, int slot, bool holstered);
    void StopSet(World& world);
    void SwapToPendingSlot();
    // A round just left: a manual action now has to be worked before the next (gameplay.cycle).
    bool AttachAndValidate(AssetLibrary& assets, const AnimatorController& ctrl);
    AnimatorControllerComponent* Animator() const;
    bool HasTag(const char* tag) const;
    void SetError(const std::string& message);
    bool SetupIK();
    void SetupSway();
    void WriteIK();
    void PlaceRigs(World& world, const Camera& camera,const Camera* placementCamera=nullptr);
    void ApplyHidden(World& world);
    void WriteAdsHold(IKRigComponent& rig, const AdsCarrySample& carry);
    void SetupBolt(AssetLibrary& assets, const AnimatorController& ctrl);
    void SetupMuzzle(int bolt);
    void SetupAdsCarry();
    // The ADS hand anchor (FirstPersonAdsSettings::Anchor): found with the carry, written with it.
    void SetupHandAnchor();
    void WriteHandAnchor(IKRigComponent& rig, const AdsCarrySample& carry, const glm::quat& adsR, const glm::vec3& adsT);
    AdsCarrySample SampleAdsCarry(float dt) const;
    // A round leaves the bore. Queued, and fired in LateUpdate from the gun as the frame renders it
    // (after the body has put the camera in its head), so it goes where the laser points.
    void ShotImpact() {
        ++m_PendingShots;
        if (m_Set.Eject.Enabled && m_Set.Eject.When == FirstPersonEjectSettings::Trigger::Shot) ++m_PendingEjects;
    }
    void FireShot(); // one queued round: note where it hits and shove that
    int m_PendingShots = 0;
    // A spent case leaves the port: on the shot, or on the controller's Eject event (a pump).
    // Queued like a shot and thrown in LateUpdate from the port as the gun is drawn.
    void Eject();
    int m_PendingEjects = 0;
    int m_EjectedTotal = 0;
    int m_ShotsTotal = 0; // rounds fired this Play (ThirdPersonFrame)
    glm::vec3 m_LastEjectPoint{0.0f}, m_LastEjectThrow{0.0f};
    std::vector<CasingSpawn> m_Ejections;
    // The weapon root as SEEN this frame (world space, through the view-model FOV stretch): where
    // the port is on screen. Valid once PlaceRigs found the root.
    glm::mat4 m_RootSeen{1.0f};
    glm::mat4 m_RootWorld{1.0f}; // the same without the stretch
    bool m_RootSeenValid = false;
    glm::vec3 m_PlayerVelocity{0.0f}; // from the last Tick
    void ReloadIfChanged(float dt);
    // Hides the spare magazine bones unless the left hand holds them (FirstPersonAnimationSet).
    void UpdateSpareMagazine(const glm::vec3& armsPos, const glm::quat& armsRot, const glm::vec3& weaponPos,
                             const glm::quat& weaponRot);
    std::vector<int> m_SpareMagHidden; // weapon nodes currently collapsed

    World* m_World = nullptr;
    FirstPersonAnimationSet m_Set;
    std::string m_ControllerPath;
    AssetLibrary* m_Assets = nullptr;                       // for re-measuring on live edits
    std::shared_ptr<const AnimatorController> m_Controller;
    float m_Crouch = 0.0f;
    IKCurveWeights m_IKCurves;                // the clip weight curves this frame (1 = none)
    entt::entity m_Arms = entt::null;
    entt::entity m_Weapon = entt::null;
    entt::entity m_WeaponParticles = entt::null, m_WorldWeaponParticles = entt::null;
    std::vector<entt::entity> m_WeaponEmitters, m_WorldWeaponEmitters;
    std::vector<entt::entity> m_WeaponAttachmentEntities;
    glm::mat4 m_ParticleBindInverse{1.0f};
    Scripting::WeaponAttachments m_Attachments, m_WorldAttachments;
    std::vector<std::array<int, 3>> m_SlotAttachmentSelections;
    glm::vec3 m_OpticOffset{0.0f}, m_OpticFromOffset{0.0f};
    glm::quat m_OpticRotation{1, 0, 0, 0}, m_OpticFromRotation{1, 0, 0, 0};
    float m_OpticBlendElapsed = 1.0f, m_OpticBlendTime = .2f;
    void ApplyMuzzleAttachment();
    void SetupWeaponParticles();
    void SetupWorldWeaponParticles();
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
    // Manual action (gameplay.cycle): a round is chambered; if not, the Cycle trigger goes on once
    // m_CycleWait runs out and stays on until a Cycling state has been seen to play through.
    bool m_Chambered = true;
    float m_CycleWait = 0.0f;
    bool m_CycleSeen = false;
    // Per-round reload: the trigger was pulled mid-reload (StopReload until the reload ends).
    bool m_StopReload = false;
    // Weapon slots (SelectSlot): the .fpsanim of each, the ammo each was left with (-1 = full),
    // the one in hand and the one to swap to once this one is holstered (-1 = none).
    std::vector<std::string> m_SlotSets;
    std::vector<std::string> m_SlotPrefabs;
    std::vector<MuzzleEffectSettings> m_SlotMuzzles;
    std::vector<std::filesystem::file_time_type> m_SlotPrefabTimes;
    std::vector<int> m_SlotAmmo;
    int m_Slot = 0;
    int m_PendingSlot = -1;
    AssetLibrary* m_SlotAssets = nullptr;
    std::shared_ptr<FirstPersonControllerComponent> m_Config; // the controller's settings, for a swap
    bool m_FullAuto = false;
    int m_BurstRemaining = 0;
    bool m_WaitingAnimatedShot = false;
    void CommitShot(bool ads, bool cycleBolt);
    float m_FireCooldown = 0.0f;  // full-auto: seconds until the next round may go
    float m_IdleTime = 0.0f;      // settled Idle, for the Fidget

    // Procedural stack. Look rate comes from the camera's yaw/pitch between Updates, velocity
    // is taken into the camera's flat frame there too.
    WeaponProceduralState m_Procedural;
    bool m_UsesIK = false;
    glm::vec3 m_BoltStroke{0.0f}; // weapon model space, from the Fire clip (SetupBolt)
    bool m_HaveMuzzle = false;
    glm::vec3 m_MuzzleLocal{0.0f}, m_BoreLocal{0.0f, 0.0f, -1.0f}; // weapon root space
    FirstPersonBarrelReport m_Barrel; // what SetupMuzzle and the sight measuring found, for the Inspector
    glm::vec3 m_AimPoint{0.0f};
    glm::vec3 m_AimNormal{0.0f, 1.0f, 0.0f};
    bool m_AimHit = false;        // the bore ray met a surface within range
    std::vector<ShotHit> m_ShotHits;
    std::vector<ShotTrace> m_ShotTraces;
    Options m_Options;
    bool m_HasShotTarget = false;
    glm::vec3 m_ShotTarget{0.0f};
    // The sight line measured in Play (weapon root space), for a weapon without a saved one.
    bool m_SightMeasured = false;
    float m_SightSettled = 0.0f;  // seconds the sights have been steady for measuring
    bool m_SightLogged = false;
    glm::vec3 m_SightOrigin{0.0f}, m_SightDirection{0.0f, 0.0f, -1.0f};
    float m_SinceShot = 1e3f;     // seconds since a round left
    float m_PlanarSpeed = 0.0f;   // the player's, from the last Tick
    glm::vec3 m_Muzzle{0.0f}, m_BoreDir{0.0f, 0.0f, -1.0f}; // world, from the last PlaceRigs
    // Actions carried onto the sights while aiming (FirstPersonAdsCarry.h).
    AdsCarryResult m_AdsCarry;
    // The hand anchor: the free (not held) limb (0 = LimbA, 1 = LimbB, -1 = none), the gun mesh's
    // box in its root's space, and the weapon nodes it carries (weapon rig offsets from
    // kWeaponAnchorOffset on; the bolt has slot 0).
    int m_AnchorLimb = -1;
    glm::vec3 m_GunBoxMin{0.0f}, m_GunBoxMax{0.0f};
    std::vector<int> m_AnchorBones;
    float m_AnchorWeight = 0.0f; // this frame's (for the weapon test)
    glm::mat4 m_ArmsWorld{1.0f}, m_WeaponWorld{1.0f}, m_View{1.0f}; // PlaceRigs': the arms entity's pose and the camera's view
    entt::entity m_WorldWeapon = entt::null; // split poses: the gun every other view sees
    glm::mat4 m_WorldWeaponDelta{1.0f};      // ... placed this far off the first-person one
    mutable std::vector<std::pair<int, int>> m_StockVerts; // StockWorld's butt vertices (mesh, vertex), found once per model
    mutable const Model* m_StockModel = nullptr;
    mutable glm::vec3 m_StockBore{0.0f};                     // ... along this bore (root space)
    static constexpr int kWeaponAnchorOffset = 1;
    float m_AdsHold = 0.0f;       // 0..1, eased toward "aim held" over Ads.AimHoldTime
    float m_SinceUnhidden = 0.0f; // seconds the weapon has been out of its Hidden state (the laser waits for the gun to be up)
    float m_TickDt = 0.0f;        // the last Tick's dt: the step the animators take next
    // IK rig offset slots on the arms: the ADS-action gun correction, then the procedural pose.
    static constexpr int kAdsOffset = 0, kProceduralOffset = 1;
    float m_Zoom = 0.0f, m_ZoomRate = 0.0f; // ADS zoom 0..1, critically damped spring
    float m_StockLockWeight = 0.0f;           // 0..1: how shouldered the gun is (the stock lock's weight)
    bool m_AimPointValid = false;
    float m_LookYaw = 0.0f, m_LookPitch = 0.0f, m_PrevLookYaw = 0.0f, m_PrevLookPitch = 0.0f;
    bool m_HaveLook = false;
    glm::vec3 m_FlatRight{1.0f, 0.0f, 0.0f}, m_FlatForward{0.0f, 0.0f, -1.0f};
    // The view kick currently on the camera (so it can come off exactly).
    bool m_KickApplied = false;
    glm::vec3 m_KickAngles{0.0f}; // pitch, yaw, roll degrees
    glm::vec3 m_KickOffset{0.0f}; // world
    float m_EyeRadius = 0.12f;   // camera lean collision: sphere radius for wall detection
    // Live retuning: the .fpsanim is re-read when it changes on disk (the Inspector saves it).
    std::filesystem::path m_SetFile;
    std::filesystem::file_time_type m_SetFileTime{};
    std::filesystem::file_time_type m_RecoilFileTime{};
    std::filesystem::file_time_type m_ShakeFileTime{};
    CameraShakeState m_CameraShake;
    CameraEffectPose m_ShakePose;
    glm::vec2 m_PlayerLookBaseline{0.0f}, m_PlayerLookDelta{0.0f}; // pitch, yaw
    glm::vec2 m_RawLookInput{0.0f};
    glm::vec2 m_SwayLookInput{0.0f}, m_SwayMoveInput{0.0f};
    bool m_HaveSwayMoveInput=false;
    SwayModifierState m_Sway;
    bool m_HaveRawLookInput=false;
    bool m_HavePlayerLookBaseline = false;
    float m_ReloadPoll = 0.0f;
    float m_RegripDelay = 15.0f;
    float m_ManagedReloadSeconds=0;
    bool m_ManagedReloadDown=false, m_ManagedReloadFired=false;
    Scripting::WeaponLoadoutFrame RunLoadout(int operation,int value=0);
    Scripting::WeaponFrame RunGameplay(int operation,float dt=0.0f,bool pressed=false,bool held=false,int events=0);
    std::mt19937 m_Rng{std::random_device{}()};
};
