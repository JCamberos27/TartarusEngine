#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Animation.h"       // LocalTRS
#include "FirstPersonBody.h" // FirstPersonArmShapeLink

class Camera;
class Model;
class World;
struct FirstPersonWorldGunInput;

// How well a soldier holds its gun this frame (NpcBody::MeasureHold; --npc-test pose). Gaps are metres
// (negative = not measured): the gun (butt to muzzle) to the drawn head / neck / hood and to the drawn
// torso, each elbow to the torso, each hand to the arms rig's (off the gun when it can't reach).
struct NpcHoldReport {
    float GunHead = -1.0f, GunTorso = -1.0f;
    float Elbow[2] = {-1.0f, -1.0f};
    float Hand[2] = {-1.0f, -1.0f};
    float Shift = 0.0f;          // how far the gun was moved off where the rig holds it
    float CheekTilt = 0.0f;      // degrees the head tilted onto the stock
    glm::vec3 EyeFromHead{0.0f}; // the weapon's eye off the head bone (body frame: x left, y up, z forward), m
};

// An enemy soldier's body: the Quantum outfit pieces under one root (assets/AI/Soldier.json), on the
// npc_soldier controller (the player body's locomotion plus a flinch layer). Capsule-led: the AI moves
// the capsule and this turns that into the controller's parameters, stands the body on the capsule's
// feet, turns it on the spot with the turn clips, and afterwards bends the finished pose - the spine
// toward what it aims at, the head toward what it looks at, the hands onto the weapon rig's.
//
// Frame order:
//   Tick         before the animators: heading, parameters, triggers
//   LateUpdate   after them: spine aim and lean, head look; the eye for the weapon's camera
//   WeaponEye    before the weapon places its rig: where its camera goes (off the shoulders)
//   HoldWeapon   after it: the gun clear of the body, arms onto its hands, the pose onto every piece

// The weapon hold's tuning: the player's First Person Body numbers (the same body, the same rigs), copied
// from the scene's player by NpcDirector; these defaults are the Arena player's.
struct NpcHoldSettings {
    float ClavicleFollow = 0.35f;    // how much of the rig's collarbone swing the body takes
    float ShoulderMaxAngle = 25.0f;  // degrees a collarbone may turn (shrug)
    float ShrugStart = 0.98f;        // a hand further than this share of the arm's length: the shoulder shrugs
    float ShrugMax = 0.12f;          // ... by at most this (m)
    float ReachLeanMax = 15.0f;      // degrees the chest leans for a hand still out of reach
    float ElbowClearance = 0.06f;    // m: elbows swing out of the drawn torso by this
    float ReachSlack = 0.04f;        // m: the shoulders a little nearer the gun than the rig's
    float ShoulderLineMatch = 1.0f;  // the chest takes the rig's bladed stance
    float SpineAim = 0.6f, SpineAimDown = 0.9f; // the share of the aim's pitch the chest takes (up / down)
    float SpineStability = 1.0f; // player's lower-body/spine blend, copied at Play
    // The weapon's eye off the shoulders, as the player's camera is (FirstPersonBody::LateUpdate).
    glm::vec3 ArmedEyeOffset{0.0f, 0.10f, 0.04f}; // the eye lifted off the rig's (model: right, up, forward), looking level
    float HeadBob = 0.5f;            // the share of the shoulders' motion about their slow average the eye follows
    float CameraSmoothing = 0.06f;   // seconds
    float EyeSlack = 0.035f;         // m the eye may trail the shoulders by
    float LookDownPush = 0.0f, LookDownStart = 0.0f; // looking down, the eye comes forward over the chest
    // NPC body tuning (Tick, LateUpdate, FootPass)
    float TurnThreshold = 1.15f;     // radians: a still body further off than this turns on the spot
    float MoveEase = 0.1f;           // seconds: blend tree parameter easing
    float FaceEase = 0.09f;          // seconds: heading easing while moving
    IK::SpineDistribution Spine;     // how the spine's turns are shared over spine_01..05 (default: even)
    float MaxTwist = 1.2f;           // radians: spine twists toward aim
    float AimLean = 0.1f;            // radians: torso forward lean aiming, standing
    float AimLeanCrouched = 0.22f;   // radians: torso lean aiming, crouched (~13 deg)
    float ReadyLeanCrouched = 0.4f;  // radians: torso lean at low ready, crouched (~23 deg)
    float CowerHunch = 0.35f;        // radians: spine curls forward ducking (~20 deg)
    float HeadMaxYaw = 1.2f;         // radians: head turns past chest (~70 deg)
    float HeadMaxPitch = 0.6f;       // radians: head nods up/down (~35 deg)
    // NPC foot IK
    float FootIKMaxDrop = 0.35f;     // m: pelvis drops to lower foot
    float FootIKMaxRaise = 0.35f;    // m: pelvis rises to higher foot
    float FootIKPelvisRaise = 0.08f; // m: pelvis height adjustment limit
    float FootIKTiltMax = 0.5f;      // radians: max foot angle to ground normal
    float FootOffsetEase = 0.05f;    // seconds: vertical foot adjustment easing
    float FootNormalEase = 0.08f;    // seconds: ground normal easing
    float FootIKFade = 0.15f;        // seconds: foot IK enable/disable easing
    IK::FootSlideSettings FootSlide; // foot pinning + stride warping (off by default; the player body's settings)
};

struct NpcBodyInput {
    glm::vec3 Feet{0.0f};          // the capsule's foot position
    glm::vec3 Velocity{0.0f};      // how the capsule moved (m/s, world)
    float FacingYaw = 0.0f;        // the heading the body should have (radians, model +Z = sin/cos)
    bool HoldFacing = false;       // face FacingYaw even while moving (strafing); else face the travel
    glm::vec3 AimPoint{0.0f};      // what the spine and gun aim at (world)
    bool Aiming = false;           // false: the spine doesn't aim (running, at the hip)
    glm::vec3 LookPoint{0.0f};     // what the head looks at
    bool Crouched = false;
    bool Sprint = false;
    float Lean = 0.0f;             // -1 (left) .. 1 (right), a peek round cover
    float Cower = 0.0f;            // 0..1: ducking from rounds cracking past (hunched, head down)
    bool FootIK = false;           // feet onto uneven ground (near the player: two rays and a leg solve a frame)
};

class NpcBody {
public:
    bool Start(World& world, entt::entity root);
    void Stop();
    bool IsActive() const { return m_Root != entt::null; }
    entt::entity Root() const { return m_Root; }

    void Tick(World& world, const NpcBodyInput& in, float dt);
    void SetHoldSettings(const NpcHoldSettings& s) { m_Set = s; }
    // The drawn surfaces' lookup tables (shared by every soldier wearing a piece) built now, with the rest of a spawn's
    // work, rather than on the first close look in the middle of a fight.
    void WarmHoldTables(const World& world);
    // `weaponCam` (optional): the weapon's camera with this frame's aim (its position is set later, by
    // WeaponEye). Armed, the chest takes the arms rig's stance against it (its shoulder line).
    void LateUpdate(World& world, float dt, const Camera* weaponCam = nullptr);

    // Holding the weapon - the player's world body's solve (FirstPersonBody::ArmsLateUpdate), for a soldier:
    //
    // WeaponEye, before the weapon places its rig: where the weapon's camera goes. Hung off the body's
    // shoulders as the rig's camera is off the rig's (the player's shoulder lock), so the rig's hands land
    // within the body's reach - not at the head bone with the whole rig dragged into the shoulder after.
    glm::vec3 WeaponEye(const World& world, entt::entity armsRig, const std::string& cameraBone, const Camera& cam, float dt);
    // HoldWeapon, after it: the gun (rig and weapon together) into the right shoulder pocket while
    // shouldered and pushed clear of the neck, head and (unshouldered) torso - spheres, and with
    // `meshChecks` the drawn surfaces; then every piece's arms onto the rig's hands (the rig's arm shapes,
    // collarbone shrug, chest lean for a far hand, elbows in the rig's bend plane and swung clear of the
    // torso, fingers); then the head onto the stock on the sights. Returns the gun's shift (world).
    // `gun` null (holstered, no weapon): the arms go back to the clips' pose.
    glm::vec3 HoldWeapon(World& world, entt::entity armsRig, entt::entity weapon, const FirstPersonWorldGunInput* gun, const Camera& cam,
                         float dt, bool meshChecks);
    NpcHoldReport MeasureHold(const World& world, entt::entity armsRig, const glm::vec3& butt, const glm::vec3& muzzle) const;
    const NpcHoldReport& LastHold() const { return m_Hold; }
    glm::vec3 GunShift() const { return m_GunShift; } // the gun off where the rig holds it (world)
    // Each foot's height above the capsule's feet as last posed (m; [0] left, [1] right), for the footsteps. False when
    // the driver has no foot bones.
    bool FootHeights(float (&out)[2]) const;
    bool Sprinting() const { return m_In.Sprint; } // this frame's gait (NpcBodyInput::Sprint)

    // A hit: the upper body flinches away along `dirWorld` (the round's travel).
    // `point` / `part` (a hitbox part, see NpcRagdoll) steer it by where the round struck; both optional.
    void Flinch(World& world, const glm::vec3& dirWorld, const glm::vec3* point = nullptr, int part = -1);
    // A hand signal with an order: the support hand leaves the gun and points along `dirWorld` for `seconds`.
    void Signal(const glm::vec3& dirWorld, float seconds = 0.9f) { m_SignalDir = dirWorld; m_SignalLeft = seconds; }
    void CancelSignal() { m_SignalLeft = 0.0f; }
    bool Signalling() const { return m_SignalWeight > 0.05f; }

    float Yaw() const { return m_Yaw; }
    bool Turning() const { return m_Turning; }
    glm::vec3 Eye() const { return m_Eye; }               // world, after LateUpdate
    glm::vec3 Feet() const { return m_Feet; }
    // A standard bone's world position as last posed; false when there is no such bone.
    bool BoneWorld(const World& world, const std::string& bone, glm::vec3& out) const;
    bool BoneWorld(const std::string& bone, glm::vec3& out) const;
    const std::vector<std::shared_ptr<Model>>& Models() const { return m_Models; }
    // The driver's skeleton (the one whose pose is solved), for the hitboxes.
    const Model* DriverModel() const { return m_DriverModel.get(); }
    glm::mat4 RootMatrix() const { return RootWorld(); }
    // The entity of every piece (for hit tests, the ragdoll).
    const std::vector<entt::entity>& Pieces() const { return m_Pieces; }
    entt::entity Driver() const { return m_Driver; }
    // The driver's current state name and the animator's root speed, for the debug overlay.
    std::string StateName(const World& world) const;
    // The pose is the ragdoll's from now on: Tick / LateUpdate stop touching the pose.
    void SetPoseOwnedElsewhere(bool owned) { m_PoseExternal = owned; }
    // The weapon leaves the hands (NpcDroppedWeapon): the hold's solve stops for good - the arms are the ragdoll's.
    void ReleaseWeaponHold();
    bool WeaponReleased() const { return m_WeaponReleased; }
    // The gun's world velocity (m/s), from the position HoldWeapon was given each frame (smoothed over a few frames): a
    // dropped gun leaves the hand at it. TrackGun is HoldWeapon's per-frame step, exposed for tests.
    void TrackGun(const glm::vec3& position, float dt);
    glm::vec3 GunVelocity() const { return m_GunVelocity; }

private:
    glm::mat4 RootWorld() const;

    entt::entity m_Root = entt::null;
    entt::entity m_Driver = entt::null;
    std::vector<entt::entity> m_Pieces;
    std::vector<std::shared_ptr<Model>> m_Models;       // per piece (index-matched)
    std::shared_ptr<Model> m_DriverModel;
    bool m_PoseExternal = false;
    bool m_WeaponReleased = false;                           // the gun has been let go (ReleaseWeaponHold)
    glm::vec3 m_GunPrev{0.0f}, m_GunVelocity{0.0f};          // the gun's last position (world) and its smoothed velocity
    bool m_HaveGunPrev = false;

    glm::vec3 m_Feet{0.0f};
    float m_Yaw = 0.0f;
    bool m_HaveYaw = false;
    glm::vec2 m_Move{0.0f};
    bool m_Turning = false;
    float m_TurnTime = 0.0f;
    float m_StillTime = 0.0f;
    bool m_WasCrouched = false;
    // Aim, sprung: the spine's pitch and twist (radians) with their rates, and the lean (eased).
    float m_AimPitch = 0.0f, m_AimTwist = 0.0f, m_Lean = 0.0f, m_AimWeight = 0.0f;
    float m_AimPitchRate = 0.0f, m_AimTwistRate = 0.0f;
    // The head's own look (radians, relative to the chest), sprung, and the cower (eased).
    float m_HeadYaw = 0.0f, m_HeadPitch = 0.0f, m_HeadYawRate = 0.0f, m_HeadPitchRate = 0.0f;
    float m_Cower = 0.0f;
    int m_DriverNeck = -1, m_DriverNeck2 = -1, m_DriverHead = -1;
    float m_Straighten = 0.0f;   // radians the aiming torso is brought up from the clips' lean (eased)
    NpcBodyInput m_In;
    glm::vec3 m_Eye{0.0f};

    // The weapon hold (NpcWeaponHold.cpp).
    NpcHoldSettings m_Set;
    void RotateSpine(const glm::quat& modelDelta); // spread down the driver's spine (SyncPieces passes it on)
    // The pose passes (spine aim, shoulder line, arms onto the gun, cheek weld) solve on the driver alone; this
    // gives every other piece that draws the upper body the driver's rotations and stabilized spine offsets (one skeleton: the pieces'
    // own solves came out the same, at several times the cost). Called whenever a piece is read or drawn next.
    void SyncPieces();
    // `stepFor(divisor)` = the turn one spine bone takes (the whole turn / divisor); see IK::ChainDivisors.
    void OffsetSpine(const std::function<glm::quat(float)>& stepFor); // m_Pose / m_Globals already hold the driver's pose and its globals
    void ApplySpineStability(const World& world);
    int m_SpineIdleClips[2] = {-2, -2};
    std::vector<glm::mat4> m_SpineIdleGlobals[2];
    // Feet onto uneven ground (LateUpdate, first): the pelvis drops to the lower foot's ground, the legs reach theirs.
    void FootPass(float dt);
    void SyncLower(); // the driver's pelvis and legs onto every other piece
    std::vector<std::vector<std::pair<int, int>>> m_LowerMap; // per piece: (piece node, driver node), pelvis and legs
    int m_DriverPelvis = -1, m_DriverLeg[2][3] = {{-1, -1, -1}, {-1, -1, -1}}; // thigh, calf, foot
    IK::FootSlide m_Slide;
    float m_FootWeight = 0.0f, m_FootOffset[2] = {0.0f, 0.0f};
    glm::vec3 m_FootNormal[2] = {glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    bool m_HaveFootGround = false;
    bool m_PiecesStale = false;
    std::vector<std::vector<std::pair<int, int>>> m_UpperMap; // per piece: (piece node, driver node), spine_01 and below
    int m_DriverIndex = -1;
    std::vector<int> m_DriverParents;                          // the driver's node parents
    int m_DriverSpine[5] = {-1, -1, -1, -1, -1}, m_DriverSpineCount = 0;
    int m_DriverSpineSlot[5] = {0, 1, 2, 3, 4}; // the chain slot (spine_0N - 1) of each found bone, for the Spine distribution
    // Scratch.
    std::vector<glm::mat4> m_Globals;
    std::vector<LocalTRS> m_Pose, m_BindScratch;
    // What the hold reads every frame, looked up once per rig (by name, they cost a hash each): the rig's finger nodes and
    // the driver's matching ones, the driver / rig node pairs for the arm shapes, and the driver's arm and chest bones.
    struct FingerNode { int Node; int Hand; std::string Name; int Driver; };
    std::vector<FingerNode> m_FingerList;
    const Model* m_FingerRig = nullptr;
    int m_FingerNodes = 0;
    std::vector<FirstPersonArmShapeLink> m_ArmLinks;
    struct ArmBones { int Upper[2] = {-1, -1}, Lower[2] = {-1, -1}, Hand[2] = {-1, -1}, Clav[2] = {-1, -1}, Chest = -1; };
    ArmBones m_DriverArm;
    std::vector<int> m_PieceParents; // scratch: a piece other than the driver's parents
    enum class Region { Head, Torso };
    struct RegionPoint { glm::vec3 Pos; int Count; std::uint16_t Bone[4]; float Weight[4]; };
    struct RegionSkin { std::vector<int> Bones; std::vector<RegionPoint> Points; };
    struct SkinTables { RegionSkin Head, Torso; };
    // Per piece. The tables depend only on the mesh data, which every soldier wearing the piece shares: built once.
    struct PieceSkin { const Model* M = nullptr; std::shared_ptr<const SkinTables> T; bool Built = false; };
    mutable std::vector<PieceSkin> m_Skins;                 // per piece
    // The drawn neck / head / hood, or pelvis / spine (a hoodie), skinned to world as posed now.
    void SkinnedRegion(const World& world, Region region, std::vector<glm::vec3>& out) const;
    std::vector<glm::vec3> m_HeadPoints, m_TorsoPoints, m_TorsoScratch;
    int m_ArmsIndex = -1;
    // Per piece: what of the skeleton it draws - anything the spine moves (kSkinsSpine), the arms (kSkinsArms), the
    // neck and head (kSkinsNeck). A piece that draws none of what a pass moves is left out of it (legs, shoes):
    // the driver always takes every pass (its bones are what is read back).
    enum : unsigned { kSkinsSpine = 1u, kSkinsArms = 2u, kSkinsNeck = 4u };
    std::vector<unsigned> m_PieceSkins;
    bool PieceTakes(size_t k, unsigned what) const {
        return k >= m_PieceSkins.size() || m_Pieces[k] == m_Driver || (m_PieceSkins[k] & what) != 0u;
    }
    int ArmsPiece() const;                                   // the "Arms" piece (index), else the driver's
    glm::vec3 m_RigEyeToShoulders{0.0f};                     // the rig's shoulders off its camera (camera frame), slow
    bool m_HaveRigOffset = false;
    // The shoulders' midpoint (model space) as the clips have them (before LateUpdate's spine passes), and the eased
    // copy the eye hangs off - the player's camera's (Head Bob of the motion about a slow average, Eye Slack at most).
    glm::vec3 m_ShouldersAnimated{0.0f}, m_ShouldersSlow{0.0f}, m_Shoulders{0.0f};
    bool m_HaveShouldersAnimated = false, m_HaveShoulders = false;
    glm::vec3 m_RigShoulderLine{0.0f};                       // the rig's left-from-right upper arm (camera frame)
    bool m_HaveRigLine = false;
    glm::vec3 m_GunShift{0.0f};                              // eased
    float m_ElbowClear[2] = {0.0f, 0.0f};                    // eased elbow swivels (radians)
    glm::vec3 m_ElbowAim[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // where each elbow heads (camera frame), eased
    bool m_HaveElbowAim[2] = {false, false};
    float m_ArmsWeight = 0.0f;
    float m_CheekWeld = 0.0f;
    glm::vec3 m_SignalDir{0.0f};                              // world
    float m_SignalLeft = 0.0f, m_SignalWeight = 0.0f;
    unsigned m_HoldFrame = 0;
    int m_HoldStagger = 0;                                   // which of every three frames this one checks the mesh
    glm::vec3 m_MeshPush{0.0f};                              // the gun's push out of the drawn head / torso, last checked
    float m_ElbowWant[2] = {0.0f, 0.0f};                     // the elbows' clearing swivels, last checked
    float m_WeldShare = 1.0f;                                // how much of the cheek weld clears the gun, last checked
    NpcHoldReport m_Hold;
};

// --- the maths, exposed for tests ---------------------------------------------------------------
// Heading (radians, model +Z) of a flat direction; `fallback` when it has no length.
float NpcYawOf(const glm::vec3& dir, float fallback);
// Where to put the spine's twist (radians, + = toward model +X): `offset` (aim heading minus body
// heading, wrapped) clamped to +-maxTwist.
float NpcSpineTwist(float offset, float maxTwist);
// Whether a still body `offset` radians off where it should face turns on the spot.
bool NpcShouldTurn(float offset, float thresholdRadians);
