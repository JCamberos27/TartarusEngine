#pragma once

#include "FirstPersonProcedural.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

struct AnimatorController;

// A weapon definition (.fpsanim): the camera-bound arms + weapon rigs, the Animator Controller
// that animates them, how the weapon mounts in the hands, and the weapon's gameplay numbers.
//
// Everything about WHICH animation plays WHEN lives in the controller (`controller`), edited in
// the Animator window like any other. The first-person driver (FirstPersonPresentation) only
// sets its parameters and reacts to its tags and events - see FirstPersonAnimatorContract below.
//
// Format v1 files carried a flat `clips` list instead of a controller. They still load: the
// driver builds the standard first-person graph from those clips in memory
// (BuildFirstPersonController), and the same function turns them into a .controller file.
struct FirstPersonAnimationClip {
    std::string Name;
    std::string ArmsClip;
    // A static arms pose baked into ArmsModel (for source exports with no time samples).
    bool ArmsBindPose = false;
    std::string WeaponClip;
    bool Loop = false;
    float Fade = 0.08f;
};

// Per-weapon gameplay numbers. Defaults are the AKS-74U's.
struct FirstPersonWeaponGameplay {
    int Magazine = 30;
    float RoundsPerMinute = 700.0f;       // full-auto cadence
    bool AllowFullAuto = true;            // false: B does nothing (semi-only weapon)
    float ReloadHoldSeconds = 0.35f;      // R held this long checks the magazine instead of reloading
    float RegripMin = 10.0f, RegripMax = 20.0f; // seconds of settled Idle before a Fidget
};

struct FirstPersonAnimationSet {
    std::string ArmsModel;
    std::string WeaponModel;
    // The Animator Controller (.controller) driving both rigs: its "arms" track plays on the arms
    // model, its "weapon" track on the weapon model. Empty = a v1 file; see Clips.
    std::string Controller;
    std::string DefaultState;             // v1 only
    // Y-X-Z Euler degrees the models themselves need to line up with the play camera, applied
    // before any per-scene View Model Rotation. This belongs to the asset, not the scene: it
    // describes the axis convention of the FBXs named above. The Manny rig comes out of Blender
    // facing model +Z while the engine's camera looks down its own -Z, so without the 180 Y this
    // set renders the arms and weapon behind the camera.
    glm::vec3 ViewRotation{0.0f};
    // The weapon rides the arms rig's gun socket rather than merely sharing the arms entity's
    // pose. `WeaponSocket` names a bone on the ARMS rig, `WeaponRoot` the bone on the WEAPON rig
    // that has to land on it, and `WeaponMountRotation` (Y-X-Z degrees) is the fixed mount between
    // the two - measured from the source FBXs as exactly (0, 90, 90) with zero translation, the
    // same for every clip's frame 0.
    //
    // The weapon clips never move the gun: only A_W_ADS keys `root` at all, so without the socket
    // the gun's placement is frozen while the hands move. Measured socket-vs-weapon error, root
    // space: Sprint 11.5-14.5 cm, Draw 18.2 cm, Holster up to 21.7 cm, Aim 67.8 cm.
    // An empty `WeaponSocket` disables the parenting (shared pose, the old behaviour).
    std::string WeaponSocket;
    std::string WeaponRoot;
    glm::vec3 WeaponMountRotation{0.0f};
    // Optional .mat overrides, keyed by the source FBX's material name ("aks74u" -> a .mat path).
    // Every submesh using that material draws with the .mat; unlisted materials keep the import.
    std::vector<std::pair<std::string, std::string>> ArmsMaterials;
    std::vector<std::pair<std::string, std::string>> WeaponMaterials;
    FirstPersonWeaponGameplay Gameplay;
    // Recoil, sway, bob, breathing, aim, per-state offsets, lean and IK (FirstPersonProcedural.h).
    // Files from before it existed load their old gameplay.recoil / adsBob numbers into it.
    WeaponProceduralSettings Procedural = WeaponProceduralSettings::Defaults();
    std::vector<FirstPersonAnimationClip> Clips; // v1 only

    const FirstPersonAnimationClip* Find(const std::string& state) const;

    // Parses the .fpsanim JSON payload. On failure, leaves `out` unchanged and writes a concise
    // actionable reason to error when provided.
    static bool FromJsonString(const std::string& text, FirstPersonAnimationSet& out,
                               std::string* error = nullptr);
    static bool LoadFile(const std::string& path, FirstPersonAnimationSet& out,
                         std::string* error = nullptr);
    // Writes the v2 form (controller path, no clip list).
    std::string ToJsonString() const;
    bool SaveFile(const std::string& path) const;
};

// The names the first-person driver and a weapon's controller agree on. A controller built for
// a new weapon only has to use these; everything else in it is free-form.
namespace FirstPersonAnimatorContract {
// Parameters the driver sets every frame.
inline constexpr const char* kSpeed = "Speed";       // Float, planar m/s
inline constexpr const char* kSprint = "Sprint";     // Bool, sprint held
inline constexpr const char* kAim = "Aim";           // Bool, aim held
inline constexpr const char* kEquipped = "Equipped"; // Bool, weapon wanted in hand
inline constexpr const char* kAmmo = "Ammo";         // Int, rounds in the magazine
// Float clip-rate multipliers that follow the player's speed (procedural.locomotion): use them
// as the Walk / Sprint states' speed parameter.
inline constexpr const char* kWalkRate = "WalkRate";
inline constexpr const char* kSprintRate = "SprintRate";
// Triggers the driver sets for one frame on input (dropped if nothing takes them).
inline constexpr const char* kFire = "Fire";
inline constexpr const char* kReload = "Reload";
inline constexpr const char* kMagCheck = "MagCheck";
inline constexpr const char* kInspect = "Inspect";
inline constexpr const char* kMelee = "Melee";
inline constexpr const char* kFidget = "Fidget";     // after RegripMin..Max s in a state tagged Idle
// State tags the driver reads.
inline constexpr const char* kTagAds = "ADS";        // sights up: fire is a procedural kick, aim offset on
inline constexpr const char* kTagReload = "Reload";  // a reload is running (R does nothing)
inline constexpr const char* kTagHidden = "Hidden";  // unarmed: both rigs hidden
inline constexpr const char* kTagIdle = "Idle";      // settled idle: counts toward the Fidget
// Events the driver reacts to.
inline constexpr const char* kEventShot = "Shot";    // a round leaves the gun (hip fire)
inline constexpr const char* kEventRefill = "Refill";// the magazine is full again
} // namespace FirstPersonAnimatorContract

// The standard first-person graph for a v1 clip list (the AKS-74U's 15 states): locomotion with
// Idle<->Sprint transition clips, ADS, one-shots returning through Exit, reloads/melee that
// firing can't interrupt, Draw/Holster that nothing interrupts, and a hidden Holstered state.
// Used to run v1 files and to write their .controller once.
AnimatorController BuildFirstPersonController(const FirstPersonAnimationSet& set);

// Seconds of settled Idle before the next Fidget, from a uniform [0,1] sample.
float FirstPersonRegripDelay(float unit01, float minSeconds = 10.0f, float maxSeconds = 20.0f);

// The reload key is overloaded: a tap reloads, a hold checks the magazine. A tap only resolves
// on release (until then it can't be told from the start of a hold); a hold fires MagCheck the
// moment it crosses HoldSeconds, and its release then does nothing.
enum class FirstPersonReloadInput { None, Reload, MagCheck };
struct FirstPersonReloadButton {
    // Long enough that a deliberate tap never trips it, short enough that a hold doesn't feel
    // laggy - the same ballpark shooters use for tap/hold on one key.
    static constexpr float kHoldSeconds = 0.35f;
    float HoldSeconds = kHoldSeconds;
    bool Down = false;
    bool Fired = false;
    float Held = 0.0f;

    FirstPersonReloadInput Update(bool down, float dt);
};
