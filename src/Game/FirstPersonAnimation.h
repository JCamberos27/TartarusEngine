#pragma once

#include "FirstPersonBodyContract.h" // FPBody::Check: the weapon setup check reuses the body's result type
#include "FirstPersonProcedural.h"
#include "CameraEffects.h"

#include <glm/glm.hpp>

#include <functional>
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
// FirstPersonAnimationClip / Clips are the New Weapon wizard's per-state clip picks, which
// BuildFirstPersonController turns into the weapon's .controller. They are never saved.
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
    FirstPersonWeaponGameplay(); // resolved defaults belong to project C#
    int Magazine{};
    float RoundsPerMinute{};       // full-auto cadence
    bool AllowFullAuto{};            // false: B does nothing (semi-only weapon)
    int BurstRounds{}; // >1: a semi trigger schedules this many rounds; release interrupts it
    float ReloadHoldSeconds{};      // R held this long checks the magazine instead of reloading
    float RegripMin{}, RegripMax{}; // seconds of settled Idle before a Fidget
    // Each round shoves the dynamic body the bore hits: ImpactImpulse N*s at the hit point
    // (so it spins as well as flies), capped at ImpactMaxSpeed m/s of velocity change per round
    // so light props don't rocket off. 0 = rounds push nothing.
    float ImpactImpulse{};
    float ImpactMaxSpeed{};
    // The holes rounds leave: radius in metres. Game-sized rather than calibre-sized (a real
    // 5.45 mm hole, 4.5 mm, was too small to see past a few metres).
    float BulletHoleRadius{}; // metres: the round's radius (the hole is ~1.15x its calibre)
    // Zeroing: rounds (and the laser) leave the muzzle aimed to cross the sight line
    // ZeroDistance metres out, like a sighted-in rifle - dead on the front post there, a little
    // low closer, a little high past it. 0 = straight down the bore as modelled.
    float ZeroDistance{};
    // The sight line in weapon-root space (the eye's position and look direction with the sights
    // up, measured in Play). Without one it's measured the first time the sights settle, and the
    // log prints the values to save here.
    bool HasSightLine{};
    glm::vec3 SightOrigin{}, SightDirection{};

    // A shotgun: each round is Pellets rays, each inside a cone of SpreadHip / SpreadAds degrees
    // (the cone's half angle; hip applies whenever the sights aren't up) about the zeroed bore.
    // ImpactImpulse and ImpactMaxSpeed are the whole round's, shared between its pellets.
    int Pellets{};
    float SpreadHip{}, SpreadAds{};
    // How a reload fills the gun. Magazine: the reload state's Refill event fills it at once.
    // PerRound (a tube): each LoadRound event adds one round; the controller loops its load state
    // until the LastRound parameter is set, and exits early when StopReload is (the trigger was
    // pulled mid-reload). See FirstPersonAnimatorContract.
    enum class ReloadMode { Magazine, PerRound };
    ReloadMode Reload{};
    // A manual action (pump, bolt, lever): after every round the Cycle trigger is set CycleDelay
    // seconds later, until a state tagged Cycling has played through; the gun can't fire in
    // between. A round loaded into an empty gun (LoadRound at 0 rounds) is chambered by its clip.
    bool CycleAfterShot{};
    float CycleDelay{};
    // What a hit on a character costs it (gameplay.damage): Damage health per round (per pellet
    // for a shotgun), times HeadMultiplier / LimbMultiplier by where it lands, scaled from 1 at
    // FalloffStart metres down to FalloffMin at FalloffEnd and beyond. See Combat/Damage.h.
    float Damage{};
    float HeadMultiplier{}, LimbMultiplier{};
    float FalloffStart{}, FalloffEnd{}, FalloffMin{};
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

// Spent casings thrown out of the ejection port (ShellCasings owns them once they leave the gun).
// Origin and Direction are in the weapon root's space (metres), like the hand-set muzzle. Trigger
// "shot" ejects as each round is fired (a self-loading gun); "event" waits for the controller's
// Eject event (a pump or bolt action: the hull comes out when the action is worked).
struct FirstPersonEjectSettings {
    bool Enabled = false;
    std::string Model;       // the casing mesh (.fbx)
    std::string Material;    // .mat applied to every submesh; empty keeps the import / .meta remap
    glm::vec3 Origin{0.0f};
    glm::vec3 Direction{1.0f, 0.0f, 0.0f};
    float Speed = 3.0f;      // m/s along Direction, on top of the player's own velocity
    float SpeedJitter = 0.2f;// +- fraction of Speed
    float Spread = 12.0f;    // degrees of random cone around Direction
    float Spin = 20.0f;      // rad/s of tumble
    enum class Trigger { Shot, Event } When = Trigger::Shot;
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

// The world gun (true first person, split poses - FirstPersonBody::ArmsLateUpdate): what every view
// but the player's own camera shows is the first-person gun moved so its butt sits in the body's right
// shoulder pocket while shouldered (Enabled + Tags), and always clear of the neck and head; the body's
// hands hold it there and its head tilts over the stock. The first-person view is the animations' own.
// A first-person rig holds the stock in by the chin and the gun high across the chest sprinting: shown
// on the body as-is, it went through the neck and the hood.
struct FirstPersonStockLockSettings {
    bool Enabled = false;    // the shoulder-pocket lock (the keep-outs always apply)
    // Shouldered: a state with any of these tags, or anything carried on the sights.
    std::vector<std::string> Tags{"Idle", "Ready", "ADS", "Cycling"};
    // The pocket, from the right upper arm's joint in the chest's frame (x right, y up, z forward), metres.
    glm::vec3 Pocket{-0.045f, 0.03f, 0.05f};
    float MaxShift = 0.3f;     // the most the world gun is moved off the first-person one (metres)
    float HeadTilt = 25.0f;    // the most the neck tilts the world head over the stock (degrees)
    float BlendTime = 0.2f;    // seconds in and out
    float NeckRadius = 0.09f;  // keep-outs: the gun stays this far from the neck bone ...
    float HeadRadius = 0.14f;  // ... and from the head (a hood on it), metres
    float GunLength = 0.45f;   // how much of the gun, from the butt forward, is kept clear (metres)
    // ... and the drawn head, neck and hood themselves: the gun's bore line stays this far from every
    // vertex the body's pieces skin to the neck or head, whatever the outfit (metres, 0 = the spheres only).
    float MeshClearance = 0.05f;
    // Looking down, the pocket lets go: all of it down to ReleaseStart degrees of pitch, none from ReleaseEnd (a stock
    // held in the shoulder with the barrel at the feet lies down the chest). Let go, the gun keeps MeshClearance
    // from the drawn torso too. Start <= End disables the release.
    float ReleaseStart = -40.0f;
    float ReleaseEnd = -70.0f;
};

// What Play found for a weapon's barrel and sights, kept per weapon definition (by file path)
// after Play stops so the weapon Inspector can show it and save it.
struct FirstPersonBarrelReport {
    glm::vec3 BoltTravel{0.0f}; // measured or explicit weapon-model stroke, available for authoring
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
    // The free hand off the gun. Its clip keys it relative to the gun, so with the gun held on the
    // sights a spot the hand reaches for away from it (a shell on the belt, a mag in a pouch) swings
    // along with the gun - in front of the face. Anchored, a hand more than Far metres from the gun
    // (its mesh's box) goes where it is relative to the eye at the hip instead, and within Near
    // follows the gun, easing between; Bones (weapon-rig bones it carries off the gun: the shell)
    // move with it. Off = the whole action relative to the gun.
    struct HandAnchor {
        bool Enabled = false;
        float Near = 0.05f;
        float Far = 0.15f;
        std::vector<std::string> Bones;
    } Anchor;
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
    // model, its "weapon" track on the weapon model. Required.
    std::string Controller;
    std::string DefaultState;             // wizard only: the generated graph's start state
    // Y-X-Z Euler degrees the models themselves need to line up with the play camera, applied
    // before any per-scene View Model Rotation. This belongs to the asset, not the scene: it
    // describes the axis convention of the FBXs named above. The Manny rig comes out of Blender
    // facing model +Z while the engine's camera looks down its own -Z, so without the 180 Y this
    // set renders the arms and weapon behind the camera.
    glm::vec3 ViewRotation{0.0f};
    // Camera-space metres applied to the owner's entire view rig after animation and IK.
    // Persists through ADS and actions; world presentations retain their authored mount.
    glm::vec3 ViewPosition{0.0f};
    float AdsLocomotionScale=1.0f;
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
    // The mount's translation, in the socket's frame (model metres), for a rig whose weapon root
    // doesn't sit exactly on the socket - socket_probe prints it as the mount's `t`. The
    // Remington 870's Main rides 1.6 cm off its CB_Gun. Zero for the AKS-74U.
    glm::vec3 WeaponMountOffset{0.0f};
    // Spare magazine: weapon bones whose mesh only shows while the arms' left hand is within
    // `SpareMagazineGrabDistance` (model metres) of them. The AKS-74U parks `mag2` ~0.63 m off
    // the gun (the pouch) outside the tactical reload; the hand is ~0.19 m from it while held.
    std::vector<std::string> SpareMagazineBones{"mag2"};
    float SpareMagazineGrabDistance = 0.21f;
    // Optional .mat overrides, keyed by the source FBX's material name ("aks74u" -> a .mat path).
    // Every submesh using that material draws with the .mat; unlisted materials keep the import.
    std::vector<std::pair<std::string, std::string>> ArmsMaterials;
    std::vector<std::pair<std::string, std::string>> WeaponMaterials;
    FirstPersonWeaponGameplay Gameplay;
    bool HasLegacyGameplay = true; // write compatibility only; migrated assets omit the gameplay block
    FirstPersonAdsSettings Ads;
    FirstPersonMuzzleSettings Muzzle;
    FirstPersonEjectSettings Eject;
    FirstPersonLaserSettings Laser;
    FirstPersonStockLockSettings StockLock;
    // Recoil, sway, bob, breathing, aim, per-state offsets, lean and IK (FirstPersonProcedural.h).
    // Files from before it existed load their old gameplay.recoil / adsBob numbers into it.
    WeaponProceduralSettings Procedural = WeaponProceduralSettings::Defaults();
    std::string RecoilProfile; // project-relative .recoil asset, or empty for embedded legacy data
    std::string RecoilProfileGuid;
    ActionCameraSettings ActionCamera;
    std::string CameraShakeProfile, CameraShakeProfileGuid;
    CameraShakeAsset CameraShake; // resolved shared asset; no shake when the path is empty
    std::vector<FirstPersonAnimationClip> Clips; // wizard only, never saved

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
inline constexpr const char* kCycle = "Cycle";       // gameplay.cycle: work the action after a round (until a Cycling state plays)
// Bools the driver sets every frame for a per-round (tube) reload: gameplay.reload = perRound.
inline constexpr const char* kLastRound = "LastRound";   // one round short of full: load the last one and finish
inline constexpr const char* kStopReload = "StopReload"; // the trigger was pulled mid-reload: finish the current round and stop
// State tags the driver reads.
inline constexpr const char* kTagAds = "ADS";        // sights up: fire is a procedural kick, aim offset on
inline constexpr const char* kTagAdsCarry = "ADSCarry"; // hip clip carried onto the sights while aiming
inline constexpr const char* kTagReload = "Reload";  // a reload is running (R does nothing, no firing)
inline constexpr const char* kTagBusy = "Busy";      // hands busy (mag check, inspect, melee): no firing
inline constexpr const char* kTagHidden = "Hidden";  // unarmed: both rigs hidden
inline constexpr const char* kTagIdle = "Idle";      // settled idle: counts toward the Fidget
inline constexpr const char* kTagReady = "Ready";    // gun simply held (walk, hip fire): like Idle, minus the Fidget
inline constexpr const char* kTagIKOff = "IKOff";    // the default procedural IK off tag: plays as authored
inline constexpr const char* kTagCycling = "Cycling";// the action is being worked (pump / bolt): no firing until it's done

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
    {kTagCycling, "The action is being worked (a pump or bolt, gameplay.cycle): the gun can't fire until this state "
                  "has played through and the next round is chambered."},
};
const char* KnownTagDescription(const std::string& tag); // nullptr for a custom tag
// Events the driver reacts to.
inline constexpr const char* kEventShot = "Shot";    // a round leaves the gun (hip fire)
inline constexpr const char* kEventRefill = "Refill";// the magazine is full again
inline constexpr const char* kEventLoadRound = "LoadRound"; // one round goes in (a per-round reload)
inline constexpr const char* kEventEject = "Eject";  // the spent case leaves the port (eject.trigger "event")
} // namespace FirstPersonAnimatorContract

// A weapon setup checked against the contract above and its own definition, worst first (the Weapon
// Inspector shows them, Play start logs the warnings): the controller must carry the parameters,
// triggers, tags and events the driver uses, and the rigs the bones the IK and mount name. A check
// whose input is missing is skipped. Reuses the body's result type.
struct FirstPersonWeaponCheckInput {
    const FirstPersonAnimationSet* Set = nullptr;
    const AnimatorController* Controller = nullptr;        // null = none loaded (a bad path)
    std::function<bool(const std::string&)> HasArmsBone;   // on the arms rig (empty = skip)
    std::function<bool(const std::string&)> HasWeaponBone; // on the weapon model (empty = skip)
};
std::vector<FPBody::Check> FirstPersonWeaponValidate(const FirstPersonWeaponCheckInput& in);

// The standard first-person graph for a clip list (the AKS-74U's 15 states): locomotion with
// Idle<->Sprint transition clips, ADS, one-shots returning through Exit, reloads/melee that
// firing can't interrupt, Draw/Holster that nothing interrupts, and a hidden Holstered state.
// The New Weapon wizard writes a new weapon's .controller with it.
AnimatorController BuildFirstPersonController(const FirstPersonAnimationSet& set);

// Seconds of settled Idle before the next Fidget, from a uniform [0,1] sample.
float FirstPersonRegripDelay(float unit01, float minSeconds = 10.0f, float maxSeconds = 20.0f);

// One pellet's direction: `dir` (unit) turned by up to `halfAngleDegrees`, spread evenly over the
// cone's disc by two uniform [0,1] samples (u1: how far out, u2: which way round).
glm::vec3 FirstPersonPelletDirection(const glm::vec3& dir, float halfAngleDegrees, float u1, float u2);

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
