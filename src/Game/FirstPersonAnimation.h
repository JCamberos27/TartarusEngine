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
    // Each round shoves the dynamic body the bore hits: ImpactImpulse N*s at the hit point
    // (so it spins as well as flies), capped at ImpactMaxSpeed m/s of velocity change per round
    // so light props don't rocket off. 0 = rounds push nothing.
    float ImpactImpulse = 0.0f;
    float ImpactMaxSpeed = 8.0f;
    // The holes rounds leave, metres across the bore (a 5.45 mm round's, a touch torn).
    float BulletHoleRadius = 0.0045f;
    // Zeroing: rounds (and the laser) leave the muzzle aimed to cross the sight line
    // ZeroDistance metres out, like a sighted-in rifle - dead on the front post there, a little
    // low closer, a little high past it. 0 = straight down the bore as modelled.
    float ZeroDistance = 25.0f;
    // The sight line in weapon-root space (the eye's position and look direction with the sights
    // up, measured in Play). Without one it's measured the first time the sights settle, and the
    // log prints the values to save here.
    bool HasSightLine = false;
    glm::vec3 SightOrigin{0.0f}, SightDirection{0.0f, 0.0f, -1.0f};
};

// Where rounds (and the laser) leave the gun. Auto finds the barrel from the procedural bolt's
// stroke (recoil.boltBone, measured from the Fire clip): the bolt rides the bore, so its travel
// is the barrel's axis, and the muzzle is the mesh's front face along it. A weapon whose bolt
// doesn't run down the bore (a pistol slide, a pump, a revolver) or that has no bolt sets the
// muzzle by hand instead: Origin and Direction in the weapon root's space (model units), which
// the Inspector can fill from what Auto detected.
struct FirstPersonMuzzleSettings {
    bool Auto = true;
    glm::vec3 Origin{0.0f};
    glm::vec3 Direction{0.0f, 0.0f, -1.0f};
};

// The weapon's laser: a beam from the muzzle down the (zeroed) bore and the dot where it lands.
// Color is the hue (linear); the beam draws at BeamBrightness times it and the dot, which a
// camera sees washing out towards white, at SpotBrightness.
struct FirstPersonLaserSettings {
    bool Enabled = true;
    glm::vec3 Color{1.0f, 0.0227f, 0.0136f};
    float BeamBrightness = 1.1f;
    float SpotBrightness = 9.0f;
};

// What Play found for a weapon's barrel and sights, kept per weapon definition (by file path)
// after Play stops so the weapon Inspector can show it and save it.
struct FirstPersonBarrelReport {
    bool Detected = false;            // Auto found a muzzle from the bolt
    glm::vec3 DetectedOrigin{0.0f}, DetectedDirection{0.0f, 0.0f, -1.0f}; // weapon-root space
    bool HasMuzzle = false;           // rounds and the laser have a muzzle to leave from
    std::string Problem;              // why there's no muzzle (empty = there is one)
    bool SightMeasured = false;       // Play measured the sight line (no saved one)
    glm::vec3 SightOrigin{0.0f}, SightDirection{0.0f, 0.0f, -1.0f};
};
void PublishBarrelReport(const std::string& weaponPath, const FirstPersonBarrelReport& report);
const FirstPersonBarrelReport* FindBarrelReport(const std::string& weaponPath);

// Aim-down-sights. Two ways a weapon's actions (reloads, mag check, ...) can play with the
// sights up, and a weapon can mix them per action:
//  - Authored: the controller has a real ADS clip for the action, in a state tagged ADS that
//    it reaches while Aim is held. It plays exactly as animated.
//  - Carried: the action only has a hip clip, in a state tagged CarryTag. While aiming, the
//    driver carries the GUN from where that clip holds it onto ReferenceState's sight line and
//    the arm IK follows, so the body stays put. With MatchElbows / MatchTwist the arms are also
//    matched to the reference pose at the clip's ends (elbow swing, forearm/upper-arm twist
//    bones), so the action starts and ends exactly in the aim pose and moves like the hip clip
//    in between.
struct FirstPersonAdsSettings {
    // The view: the world magnifies by Zoom (1 = none) and the gun by ViewModelZoom, eased in
    // and out over about ZoomTime seconds. Mouse look scales with the world zoom.
    float Zoom = 1.0f;
    float ViewModelZoom = 1.0f;
    float ZoomTime = 0.2f;
    // The pose carried actions are measured against: the aiming state (empty = the first state
    // tagged ADS).
    std::string ReferenceState = "Aim";
    std::string CarryTag = "ADSCarry";
    bool MatchElbows = true;
    bool MatchTwist = true;
    // Seconds for a carried action to rise onto / drop off the sights when aim is pressed or
    // released partway through it.
    float AimHoldTime = 0.15f;
    // The bones a carried action plays on with the sights up (each with everything under it):
    // the rest of the rig - the gun and the other arm - keeps playing the reference (aim) state,
    // and the action's hands keep their grip relative to the gun, so the left hand reloads while
    // the gun stays on the sights. Empty = the whole action is carried onto the sights instead
    // (the gun moves as in the hip clip).
    std::vector<std::string> ActionBones{"clavicle_l"};
    // Per carried action, how much of its clip's own gun motion plays on top of that anyway: the
    // share of its turn (the mag check tipping the mag into view) and of its movement, turned
    // about the rear sight (SightPivot metres ahead of the eye) so the sights stay near the
    // centre. Unlisted = none.
    struct GunMotion {
        std::string State;
        float Rotation = 0.0f;
        float Position = 0.0f;
    };
    // The defaults: the reloads sway a little with their clip, the mag check tips the mag into view.
    std::vector<GunMotion> GunMotions{{"TacReload", 0.18f, 0.25f}, {"EmptyReload", 0.18f, 0.25f}, {"MagCheck", 0.6f, 0.2f}};
    float SightPivot = 0.25f;
    const GunMotion* GunMotionFor(const std::string& state) const {
        for (const GunMotion& m : GunMotions)
            if (m.State == state) return &m;
        return nullptr;
    }
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
    FirstPersonAdsSettings Ads;
    FirstPersonMuzzleSettings Muzzle;
    FirstPersonLaserSettings Laser;
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
inline constexpr const char* kTagAdsCarry = "ADSCarry"; // hip clip carried onto the sights while aiming
inline constexpr const char* kTagReload = "Reload";  // a reload is running (R does nothing, no firing)
inline constexpr const char* kTagBusy = "Busy";      // hands busy (mag check, inspect, melee): no firing
inline constexpr const char* kTagHidden = "Hidden";  // unarmed: both rigs hidden
inline constexpr const char* kTagIdle = "Idle";      // settled idle: counts toward the Fidget
inline constexpr const char* kTagReady = "Ready";    // gun simply held (walk, hip fire): like Idle, minus the Fidget
inline constexpr const char* kTagIKOff = "IKOff";    // the default procedural IK off tag: plays as authored

// Every tag above, with what it does - for the editors' tag pickers and tooltips.
struct KnownTag { const char* Name; const char* Description; };
inline constexpr KnownTag kKnownTags[] = {
    {kTagAds, "Sights up. The ADS zoom, sight alignment and ADS recoil apply; firing is a procedural kick. "
              "Tag the aiming state, and any real ADS clip (an ADS reload, say)."},
    {kTagAdsCarry, "While aiming, this state's hip clip is carried onto the sights: the gun moves to the aim "
                   "pose's sight line and the arms follow by IK. For actions that have no ADS clip."},
    {kTagReload, "A reload is running: R does nothing and the gun can't fire."},
    {kTagBusy, "The hands are busy (mag check, inspect, melee): the gun can't fire."},
    {kTagIdle, "A settled idle: standing in it long enough plays the Fidget."},
    {kTagReady, "The gun is simply being held (walking, hip fire): hip rounds kick procedurally (recoil.hipProcedural) "
                "and the barrel's aim point shows, as in Idle, but no Fidget."},
    {kTagHidden, "Unarmed: both rigs are hidden."},
    {kTagIKOff, "Plays exactly as animated: the hand IK and procedural motion fade out (the weapon's IK Off Tag)."},
};
const char* KnownTagDescription(const std::string& tag); // nullptr for a custom tag
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
