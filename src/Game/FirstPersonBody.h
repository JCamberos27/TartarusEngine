#pragma once

#include "Audio/FoleyAudio.h" // FootContactDetector (the gait phase)
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "FirstPersonBodyContract.h"
#include "FirstPersonBodyMotion.h" // the motion matching pose source (Locomotion Source)
#include "IK.h" // IK::SpineDistribution
#include "ModelVertex.h" // MAX_BONE_INFLUENCE
#include "Player.h" // Player::DirectionalSpeeds

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class AssetLibrary;
class Camera;
class Model;
class Player;
class World;
struct FirstPersonBodyComponent;
struct AnimatorControllerComponent;
struct LocalTRS;

// One body node's arm-shape link to the arms rig (FirstPersonBodyArmShapeLinks).
struct FirstPersonArmShapeLink { int Body, Rig; bool Clavicle; };

// The world gun this frame (FirstPersonPresentation::WorldGunInput): the first-person gun's butt and
// bore (world), and how the world copy is placed off it - its butt into the body's right shoulder
// pocket while shouldered, and always clear of the neck and head (see ArmsLateUpdate).
struct FirstPersonWorldGunInput {
    glm::vec3 ButtWorld{0.0f};
    glm::vec3 ForwardWorld{0.0f, 0.0f, -1.0f};
    float Shouldered = 0.0f;       // 0..1 (eased): the pocket lock's weight
    glm::vec3 Pocket{0.0f};        // from the right upper arm, chest frame (x right, y up, z forward), m
    float MaxShift = 0.3f;         // the most the world gun is moved off the first-person one, m
    float HeadTiltDegrees = 0.0f;  // the world head's tilt over the stock, at most
    float NeckRadius = 0.09f;      // keep-outs around the neck and head bones, m
    float HeadRadius = 0.14f;
    float GunLength = 0.45f;       // how much of the gun, from the butt forward, is kept clear, m
    float MeshClearance = 0.05f;   // ... and how far from the drawn head / neck / hood vertices, m (0 = off)
    float TorsoKeepOut = 0.0f;     // 0..1: how much the gun also keeps MeshClearance from the drawn torso (1 - Shouldered)
    float CheekWeld = 0.0f;        // 0..1 (eased): the head's tilt over the stock - on the sights only
    float Action = 0.0f;           // 0..1: a reload / inspect / mag check / melee off the sights - the world gun takes less of the view pitch
};

// The weapon's hand anchor (FirstPersonPresentation::HandAnchor), for the world body: the free hand's trip off the gun
// - the pouch, the shells - goes where it is on the body rather than with the gun.
struct FirstPersonHandAnchor {
    std::string Hand, Socket;              // the free hand and the gun socket it is measured from (arms rig nodes)
    glm::mat4 Mount{1.0f};                 // the weapon root on the socket
    glm::vec3 BoxMin{0.0f}, BoxMax{0.0f};  // the gun's box, in the weapon root's space
    float Near = 0.05f, Far = 0.15f;       // off the box: with the gun within Near, on the body from Far
    bool Active = false;                   // a state whose free hand reaches off the gun (FirstPersonPresentation::HoldsOnBody)
    float Off = 1.0f;                      // ... and how far off its grip the clips' free hand is now (0 on it, 1 well off)
};

// True first person (#405, phase 1): the player's own body, drawn in the world under the play
// camera and walked by its clips' root motion.
//
// The scene authors the body as ordinary objects: a root with the First Person Body component
// (Components.h) and its pieces as children - head, torso, legs, feet, later clothing - rigged
// models on one skeleton, the first with an Animator Controller driving the rest. In Play this class
// takes it over: it stands the body at the capsule's feet facing the view, feeds the controller
// the player's movement, runs its root motion In Place and hands that travel to the Player (which
// sweeps the capsule with it, blended with the input by Responsiveness), and puts the camera in
// the head. Like FirstPersonPresentation it owns nothing persistent: everything it changes on
// the scene's objects is play-time state the Stop snapshot restores.
//
// Frame order, per simulated step:
//   BeforePlayerMove  camera back to the player's own eye; last step's root motion to the Player
//   Player::Update    look, capsule sweep
//   Tick              body to the feet / view yaw, controller parameters     (before the animators)
//   LateUpdate        this step's root motion, camera into the head          (after the animators)
// The arms' twist bones after an arm solve (the player's body, its world twins, the soldiers): IK re-rolls a hand and
// the twist bones spread that roll along the forearm, and undo the upper arm's own toward the shoulder (IK::SpreadTwist).
class FirstPersonArmTwist {
public:
    // On `m`'s applied pose. `roll` / `residual` (optional, [2]): each hand's roll about its forearm and what is left at
    // the wrist, degrees. False when `m` has no arms with twist bones.
    bool Apply(Model& m, float* roll = nullptr, float* residual = nullptr);
private:
    struct Rig {
        int Nodes = 0;
        IK::Pose Bind;
        int Upper[2] = {-1, -1}, Lower[2] = {-1, -1}, Hand[2] = {-1, -1};
        std::vector<IK::TwistBone> UpperTwist[2], LowerTwist[2];
    };
    std::map<const Model*, Rig> m_Rigs;
};

// A gait's phase read off the feet, every frame: 0 as the left foot reaches its furthest ahead (its heel strike), 0.5 the right.
// Over a stride the feet's spread along the travel and their height difference trace a loop a quarter cycle apart - the
// angle round it is the phase. Nothing to detect or miss (counting plants, a missed or doubled one halved or doubled the
// rate for steps on end), it runs at the legs' own pace whatever clip they play, and it turns back the right way walking
// backwards or sideways (the spread is measured along the travel). Followed by a smoothed rate and an eased catch-up, so
// whatever follows it never jerks.
struct FirstPersonGaitPhase {
    float Phase = 0.0f;
    float Rate = 0.0f;       // cycles a second
    bool Locked = false;     // moving, with a steady reading
    float SpreadAmp = 0.1f;  // the running size of each signal (m), to read them as a circle
    float LiftAmp = 0.05f;
    float PrevAngle = -1.0f;
    float Still = 0.0f;      // seconds not moving
    float Reading = 0.0f;    // seconds of readings since moving off (the first are taken quickly)
    // `spread`: (left foot - right foot) along the travel (m); `lift`: left foot height - right's (m).
    void Update(float spread, float lift, bool moving, float dt);
};

class FirstPersonBody {
public:
    // Finds the scene's First Person Body and takes it over. False (and every call below a
    // no-op) when there is none or it can't run - see LastError. Sets the player's move speeds
    // to the body's Run / Sprint speeds, so the input asks the blend tree for what it has.
    bool Start(World& world, Player& player);
    // The rig's name for a standard bone (the Bone Map; the standard name when unmapped).
    const std::string& Bone(const std::string& standard) const { return FPBody::MappedBone(m_BoneMap, standard); }
    void Stop(World& world);
    bool IsActive() const { return m_Body != entt::null; }
    // The body's Character Outfit changed its pieces since Start (Stop + Start again to take them).
    bool OutfitChanged(const World& world) const;
    const std::string& LastError() const { return m_LastError; }

    void BeforePlayerMove(Player& player, Camera& camera);
    void Tick(World& world, const Player& player, const Camera& camera, float dt);
    // `weaponArms` / `rigCameraBone`: the presentation's arms rig entity and the node its camera
    // is pinned to (see ArmsLateUpdate) - with Weapon Arms the eye is put where the rig's is
    // relative to the body's shoulders, so the rig's hands are within the body's reach.
    void LateUpdate(World& world, Camera& camera, float dt, entt::entity weaponArms = entt::null,
                    const std::string& rigCameraBone = std::string());
    // Phase 2, after FirstPersonPresentation::LateUpdate has seated the arms rig and gun: the
    // body's arms take that rig's arm pose and reach their hands onto its hands (Weapon Arms).
    // `weaponArms` is the presentation's arms entity (null = none), `viewModelFov` its sub-pass
    // FOV in degrees. The arms rig stops being drawn while this holds; the gun still is.
    // `camera` (optional) is the view it's drawn from: the Scene overlay's frustum and the eye distances.
    //
    // Split poses (Weapon Arms): the body's pieces are what the player's own camera shows. Each piece has a world
    // twin - the same model, its own pose - which every other view and every shadow shows. The twins are the same body
    // (the pieces' pose as LateUpdate left it) and take the same arm solve; only the targets differ: the world gun's hold.
    // That is the first-person gun moved by `thirdPersonGun` (the rig's model space; FirstPersonPresentation::
    // ThirdPersonGunCorrection: the 3P clips' gun against the 1P clips'), and with `thirdPersonRig` (the 3P clips' pose,
    // FirstPersonPresentation::ThirdPersonArms) its grip on the gun at `gunSocket` - the hands, fingers and elbows -
    // instead of the first-person rig's. `gun` and `handAnchor` are no longer read (the shoulder lock, cheek weld and
    // hand anchor are gone); kept so the caller needn't change.
    void ArmsLateUpdate(World& world, entt::entity weaponArms, float viewModelFov, float dt, const Camera* camera = nullptr,
                        const glm::mat4& thirdPersonGun = glm::mat4(1.0f), const FirstPersonWorldGunInput* gun = nullptr,
                        const Model* thirdPersonRig = nullptr, const std::string& gunSocket = {},
                        const FirstPersonHandAnchor* handAnchor = nullptr);
    bool SplitPoses() const { return !m_Twins.empty(); }
    // The hand anchor's move (FirstPersonPresentation::PlaceWorldWeapon): none - the world hand carries what it holds
    // straight off the gun, and PlaceWorldWeapon's `holdingHand` puts a part in it on the solved world hand.
    glm::mat4 CarriedMove() const { return glm::mat4(1.0f); }
    float TwinAnchorWeight() const { return 0.0f; }
    glm::vec3 CarriedHand() const { return glm::vec3(0.0f); }
    // Where the world gun is off the first-person one this frame (rigid, world): the third-person hold. Identity with no
    // split.
    const glm::mat4& WorldGunDelta() const { return m_WorldGunDelta; }
    // What the hands are doing, for the idle fidgets: a gun held (the ready stance's own fidgets) and busy with it
    // (aiming, or just fired: no fidget). Set each frame before Tick.
    void SetHands(bool armed, bool busy) { m_Armed = armed; m_Busy = busy; }
    // Diagnostics: where the body stands (world; the capsule's feet with a stair step eased in).
    const glm::vec3& Feet() const { return m_Feet; }
    float TwinHandGap(int side) const { return m_TwinHandGap[side & 1]; }

    // A standard bone's world position as last posed - from the arms
    // piece when it has the bone (the arms after ArmsLateUpdate), else the driver. The world twins' (what every
    // other view shows) when poses are split, unless `worldTwins` is false: then the pieces' (the player's own view).
    bool BoneWorld(const World& world, const std::string& standard, glm::vec3& out, bool worldTwins = true) const;
    // ... and its whole world matrix (the world twins' when poses are split).
    bool BoneWorldMatrix(const World& world, const std::string& standard, glm::mat4& out, bool worldTwins = true) const;
    // Each foot's height above the body's feet as last posed (m; [0] left, [1] right), for the footsteps
    // (FoleyAudio). False with no body or no foot bones.
    bool FootHeights(const World& world, float (&out)[2]) const;
    bool FootPoints(const World& world, glm::vec3 (&out)[2]) const; // the feet in the driver's model space (x right, y up, z forward)
    // The gait's phase from the body's own foot plants (0 = the left foot down, 0.5 = the right), what the weapon's walk and
    // run sway and the view bob follow so they dip on the footfalls. False until the feet have planted.
    bool GaitPhase(float& phase) const { phase = m_Gait.Phase; return IsActive() && m_Gait.Locked; }
    float Yaw() const { return m_Yaw; }     // body heading, radians about +Y (model +Z faces the view)
    float Twist() const { return m_Twist; } // view heading minus body heading, radians

    // The controller parameters of the last Tick (body frame: x right, y forward, m/s).
    glm::vec2 Move() const { return m_Move; }
    // The root motion's horizontal velocity (world, m/s) from the last LateUpdate.
    glm::vec3 RootVelocity() const { return m_RootVelocity; }

    // Diagnostics of the last LateUpdate's camera (world). The camera ends at BodyEye() + ViewKick(): the body's eye
    // (head / shoulders, no procedural motion), plus what the presentation's view punch put on it this frame (walk bob,
    // landing dip, lean, ADS camera share - FirstPersonPresentation::ViewKickOffset), kept rather than overwritten.
    glm::vec3 BodyEye() const { return m_BodyEye; }
    glm::vec3 ViewKick() const { return m_ViewKick; }
    // The lean: the kick's sideways part (m, + = the view's right) and how far the spine's lean carried the
    // shoulders that way (m, world) - the two match within the spine's limits, so the hands stay on the gun.
    float LeanWanted() const { return m_LeanWanted; }
    float LeanShoulders() const { return m_LeanShoulders; }
    // Weapon Arms' weight: now, and as LateUpdate's eye used it (the same frame's: equal but for a holster's first frame).
    float ArmsWeight() const { return m_ArmsWeight; }
    float EyeArmsWeight() const { return m_EyeArmsWeight; }
    // The clips' feet before foot IK (driver model space), which the gait phase is read off. False until posed.
    bool GaitFeet(glm::vec3 (&out)[2]) const { out[0] = m_GaitFeet[0]; out[1] = m_GaitFeet[1]; return m_HaveGaitFeet; }
    float GaitRate() const { return m_Gait.Rate; } // the gait phase's cycles a second
    // Locomotion Source = Motion Matching took (the database loaded and bound): the pose comes from FirstPersonBodyMotion.
    bool MotionMatching() const { return m_Mm.Active(); }
    const FirstPersonBodyMotion& Motion() const { return m_Mm; }
    // The source a body component asks for, after TARTARUS_LOCOMOTION_SOURCE (legacy | mm).
    static LocomotionSource SourceFor(const FirstPersonBodyComponent& cfg);

private:
    // Motion matching's Tick (after the body's feet and the view's heading are worked out).
    void TickMotionMatching(World& world, const Player& player, const FirstPersonBodyComponent& cfg, float dt);
    void UpdateGaitPhase(const Player& player, float dt);
    FirstPersonBodyMotion m_Mm;
    Player::DirectionalSpeeds m_MmSpeeds; // the gait table's speeds, for the capsule (BeforePlayerMove)
    float m_MmViewYaw = 0.0f, m_MmViewRate = 0.0f; // the view's heading last Tick and its turn rate (smoothed, rad/s)
    bool m_HaveMmView = false;
    float m_MmRunScale = 1.0f;    // the run slowed in turns (of the run's speed)
    float m_MmSpineWeight = 1.0f; // Spine Stability's share under motion matching: eased out through the airborne segment
    void Fail(const std::string& message);
    void ApplySpineStability(float weight, const AnimatorControllerComponent& animator);
    std::string m_SpineIdleClip[2]; // standing / crouched reference from the locomotion controller
    float m_SpineCrouch = 0.0f, m_SpineCrouchDrop = 0.0f;
    struct SpineReference {
        std::vector<int> Parents, Bones;
        std::array<int, 5> SpineNodes = {-1, -1, -1, -1, -1};
        std::vector<glm::mat4> Globals[2];
        int Clips[2] = {-2, -2}; // rebuilt if an idle clip becomes available after startup
    };
    std::map<const Model*, SpineReference> m_SpineReferences;
    void ApplySpineAim(const Camera& camera, float amount, float twist);
    // Arm Hang (unarmed): the spine's pitch at the view carried the arms round with the chest - looking down they swung
    // back, stiff, behind a body bent over. Each piece's upper arms' model-space rotations are taken before the pitch and
    // turned back `weight` of the way after it, so the arms keep hanging as the chest bends.
    struct ArmHangPose { glm::quat Rot[2]; bool Have[2] = {false, false}; };
    void CaptureArms(std::vector<ArmHangPose>& out) const;
    void HangArms(const std::vector<ArmHangPose>& saved, float weight);
    float ArmDownDeg() const; // the driver's upper arms off straight down (deg, the worse; BodyDebug)
    // Turns the chest by `modelDelta` (model space), spread evenly down the spine bones, on every piece.
    void ApplySpineRotation(const glm::quat& modelDelta);
    // The spine's per-bone turn, applied bone by bone on every piece. `stepFor(divisor)` gives the turn a bone takes:
    // the whole turn divided by `divisor` (the bone count for the even spread; see IK::ChainDivisors).
    void RotateSpine(const std::function<glm::quat(float)>& stepFor);
    // The same over any chain of (standard-named) bones, root first - on `models` (default: the pieces). With
    // `dist` (spine chains) the bones take weighted shares, per-bone limits and a pelvis share; null = even.
    void RotateChain(const std::vector<std::string>& bones, const std::function<glm::quat(float)>& stepFor,
                     const std::vector<std::shared_ptr<Model>>* models = nullptr, const IK::SpineDistribution* dist = nullptr);
    IK::SpineDistribution m_Spine; // the body component's Spine fields, as of the last update
    void MakeTwins(World& world);
    // Each twin onto its piece: transform, and the pose as posed so far (one body: the twins differ only in the arms).
    void SyncTwins(World& world);
    // Rigid head wear's twins (a balaclava, glasses) onto the head twin's head bone, once the twins are posed.
    void PlaceHeadAttachedTwins(World& world);
    void ApplyFootIK(World& world, const FirstPersonBodyComponent& cfg, float dt);

    entt::entity m_Body = entt::null;   // the root: placed at the feet, its pieces ride along
    entt::entity m_Driver = entt::null; // the piece whose Animator Controller runs the body
    std::vector<std::shared_ptr<Model>> m_Models; // every piece's model (Stop un-hides their bones)
    std::vector<entt::entity> m_Pieces;           // ... and its entity, in step with m_Models
    float m_ArmsWeight = 0.0f;                   // 0..1: how much the arms follow the weapon's
    std::map<std::string, std::string> m_BoneMap; // the Bone Map, parsed at Start
    int m_ShoulderNode[2] = {-1, -1};             // upperarm_l / upperarm_r on the driver
    glm::vec3 m_Shoulders{0.0f};                  // their midpoint in model space as the eye follows it (FirstPersonBodyFollowShoulders)
    bool m_HaveShoulders = false;
    glm::vec3 m_ShoulderAnchor[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // Arm Steadiness: each upper arm in m_ChestView's frame (slow)
    bool m_HaveShoulderAnchor[2] = {false, false};
    glm::vec3 m_ElbowAim[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // Weapon Arms: where each elbow heads, in m_ChestView's frame (eased)
    bool m_HaveElbowAim[2] = {false, false};
    glm::mat4 m_ChestView{1.0f};     // LateUpdate's camera (eye + view punch), facing the view's heading at the chest's pitch (columns: right, up, front, camera)
    bool m_HaveChestView = false;
    glm::vec3 m_RigEyeToShoulders{0.0f};          // rig: camera bone to its shoulders, in the camera's frame (smoothed)
    bool m_HaveRigOffset = false;
    glm::vec3 m_RigShoulderLine{1.0f, 0.0f, 0.0f}; // rig: right to left upper arm, in the camera's frame (smoothed) - its stance
    bool m_HaveRigShoulderLine = false;
    std::vector<entt::entity> m_ArmsTagged;       // pieces given the ViewModelTag
    float m_ProbeLogTimer = 0.0f, m_ProbeLogged = 1e9f; // CameraProbe's log throttle
    void CameraProbe(World& world, const Camera& camera, float viewModelFov, float dt);
    std::string m_LastError;
    int m_OutfitVersion = 0;       // CharacterOutfitComponent::Version at Start
    float m_ViewYaw = 0.0f;        // the view's heading, radians (the body's turns aim at it)
    bool m_HaveHeading = false;
    float m_CrouchHeight = 0.0f, m_CrouchSpeed = 0.42f; // the component's, from Start
    float m_IdleTime = 1.0f;       // seconds with no move input (a start clip needs a real pause)
    glm::vec2 m_LastDir{0.0f, 1.0f}; // the last move direction (body frame) and whether it was a sprint
    bool m_LastSprint = false;
    bool m_Grounded = false;       // at the last Tick
    glm::vec3 m_ArmedEyeDelta{0.0f}; // the shoulder-anchored eye minus the head's, in the body's frame (as of the last armed frame)
    bool m_HaveArmedEye = false;
    entt::entity m_PoseSource = entt::null; // the arms rig while it carries PoseSourceTag (not drawn, no shadow)
    // The arms pieces hidden while they ease off a holstered gun, each with its RenderableComponent::CastShadows before that.
    std::vector<std::pair<entt::entity, int>> m_ArmsEasedOut;
    // Whether piece `e` is part of the arms (FPBody::IsArmsPiece on its name, with this body's settings).
    bool IsArmsPiece(const entt::registry& reg, entt::entity e) const;
    bool m_InLand = false;         // the animator is in the landing state (the input drives the capsule)
    float m_FootWeight = 0.0f;     // 0..1: how much foot IK is on (eases at the edges of grounded)
    bool m_HaveFoot = false;
    // The body's own height. Stairs: the capsule climbs a step at a time; the body follows the ground's ramp under it
    // instead (and its hips the lower foot), on one critically damped spring fed the ramp's own climb - so the camera on
    // the head rides stairs as a slope, without lag or kinks.
    float m_BodyY = 0.0f, m_BodyVy = 0.0f; // the body's feet height (world) and its rate
    bool m_HaveBodyY = false;
    float m_StepOffset = 0.0f;     // ... off the capsule's (m_BodyY - the capsule's foot)
    float m_FootGround[2] = {0.0f, 0.0f}; // each foot's ground off the capsule's, smoothed (foot IK, last frame)
    bool m_HaveFootGround = false;
    float m_LastCapsuleY = 0.0f;
    bool m_HaveCapsule = false;
    glm::vec3 m_GroundVelocity{0.0f};           // the capsule's horizontal velocity
    IK::FootLock m_Lock;                        // planted feet held in the world
    std::string m_LastState;                    // the driver's state last frame
    bool m_TurnFade = false;                    // a turn on the spot fading back to its loop (a start may cut in)
    float m_FootOffset[2] = {0.0f, 0.0f};                 // ground under each foot minus the capsule's, smoothed
    glm::vec3 m_FootNormal[2] = {glm::vec3(0, 1, 0), glm::vec3(0, 1, 0)};
    float m_MoveTime = 0.0f;       // seconds of move input in a row (a tap is not a run to stop from)
    bool m_WasCrouched = false;    // for the stand<->crouch edge
    // What the locomotion script keeps between frames beyond the above: distance matching, the pivot under way,
    // the fidget timer, and the picks it last made.
    struct LocoMemory {
        float StartDistance = 0.0f, PivotDistance = 0.0f, PivotTravel = 0.0f, MoveDistance = 0.0f;
        float FidgetTime = 0.0f, FidgetNext = 0.0f, FidgetIndex = 0.0f;
        float StartGait = 1.0f, StopGait = 1.0f, PivotGait = 1.0f, StartTurn = 0.0f, StartTurnAmount = 1.0f;
        glm::vec2 PivotDir{0.0f, 1.0f}, StepDir{0.0f, 1.0f};
        bool PivotReversed = false;
    } m_Loco;
    bool m_Armed = false, m_Busy = false; // a gun in hand; aiming or just fired (SetHands)
    std::uint32_t m_Random = 0x9E3779B9u;
    bool m_Still = false;          // standing still at the last Tick (the view's turn is then limited)
    float m_MaxTurnRate = 0.0f;    // the component's, from the last Tick
    float m_TurnThreshold = 0.0f;
    bool m_Turning = false;        // a turn-in-place clip is carrying the body round
    float m_TurnTime = 0.0f;       // seconds into it
    float m_TurnDone = 0.0f;       // radians the clip has turned the body so far
    float m_Twist = 0.0f;          // view heading minus body heading, radians, wrapped
    int m_HeadNode = -1;
    glm::vec3 m_RestHead{0.0f};   // head bone, model space, in the bind pose
    glm::vec3 m_Eye{0.0f};        // the smoothed eye, model space
    bool m_HaveEye = false;
    // Split poses: each piece's world twin (m_Twins[k] for m_Pieces[k]) and its model instance.
    std::vector<entt::entity> m_Twins;
    std::vector<std::shared_ptr<Model>> m_TwinModels;
    // Per model: does it skin anything the arms' solve moves (under its top spine bone or the clavicles)?
    // Legs, feet and rigid pieces don't, so the solve leaves them be (nothing of theirs it moves is drawn).
    std::map<const Model*, bool> m_SkinsUpperBody;
    // The body / arms-rig node pairs CopyArmShape works out by name, per (piece model, rig): the same pairs every
    // frame, so found once. Keyed by model addresses and node counts; cleared at Stop.
    struct ArmShapeCache { const Model* Body; const Model* Rig; int BodyNodes, RigNodes; std::vector<FirstPersonArmShapeLink> Links; };
    std::vector<ArmShapeCache> m_ArmShapeCache;
    void CopyArmShapeCached(const Model& m, const Model& rig, float weight, std::vector<LocalTRS>& pose, const std::vector<int>& parents,
                            float clavicleWeight);
    bool SkinsUpperBody(const Model& m);
    // The same question for any set of roots (a chain's bones), cached by the chain's first name.
    std::map<std::pair<const Model*, std::string>, bool> m_SkinsUnder;
    bool SkinsUnder(const Model& m, const std::vector<int>& roots, const std::string& key);
    float m_TwinHandGap[2] = {0.0f, 0.0f}; // m each world hand ended up off the world gun's grip (the twins' solve)
    FirstPersonArmTwist m_ArmTwist;
    void ArmTwist(const std::vector<std::shared_ptr<Model>>& models);
    glm::mat4 m_WorldGunDelta{1.0f};
    // The twins' own Arm Steadiness / elbow state (the pieces' is m_ShoulderAnchor ... m_ElbowAim).
    glm::vec3 m_WorldShoulderAnchor[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    bool m_WorldHaveShoulderAnchor[2] = {false, false};
    glm::vec3 m_WorldElbowAim[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    bool m_WorldHaveElbowAim[2] = {false, false};
    glm::vec3 m_CameraApplied{0.0f}; // what LateUpdate added to the camera (BeforePlayerMove takes it off)
    glm::vec3 m_CameraBase{0.0f};    // the player's own eye at Tick (no body, no view punch): LateUpdate's kick is off it
    bool m_HaveCameraBase = false;
    glm::vec3 m_BodyEye{0.0f}, m_ViewKick{0.0f}; // LateUpdate's eye and the presentation's kick on top (diagnostics)
    float m_LeanWanted = 0.0f, m_LeanShoulders = 0.0f; // the lean's side step and the spine's share of it (diagnostics)
    glm::vec3 m_HeadEased{0.0f}; // unarmed: the clips' head as the eye follows it (FirstPersonBodyFollowShoulders)
    float m_EyeArmsWeight = 0.0f;    // m_ArmsWeight as LateUpdate's eye used it
    float m_ArmsWeightPrev = 0.0f;   // ... and before LateUpdate eased it (ArmsLateUpdate re-decides from it)
    bool m_ArmsWeightEased = false;  // LateUpdate has eased m_ArmsWeight this frame
    bool m_ViewModelFovOk = true;    // the last ArmsLateUpdate had a view-model FOV (LateUpdate's guess at the rig)
    glm::vec3 m_GaitFeet[2] = {glm::vec3(0.0f), glm::vec3(0.0f)}; // the clips' feet before foot IK (driver model space)
    bool m_HaveGaitFeet = false;
    glm::vec3 m_Feet{0.0f};
    float m_Yaw = 0.0f;           // body heading, radians about +Y (model +Z faces the view)
    glm::vec2 m_Move{0.0f};
    glm::vec3 m_RootVelocity{0.0f};
    float m_AirTime = 0.0f;
    std::string m_LastTrigger;     // the last start / stop / crouch / jump trigger fired (for the debug readout)
    float m_SinceTrigger = 1000.0f;
    float m_RunSpeed = 0.0f;       // the blend tree's (the clips') run speed
    float m_PlayerRunSpeed = 0.0f; // the capsule's run / sprint speeds (Player Run / Sprint Speed, else the clips')
    float m_PlayerSprintSpeed = 0.0f;
    float m_PlayerWalkSpeed = 0.0f; // the capsule's walk (the default gait); Shift is the jog (m_PlayerRunSpeed)
    float m_FallSpeed = 0.0f;       // m/s down at the last airborne frame: the next landing's impact
    FirstPersonGaitPhase m_Gait;
    float m_Responsiveness = 0.0f; // the component's, from the last Tick
};

// --- The maths, exposed for tests ------------------------------------------------------------

// The heading (radians about +Y) that turns a model facing +Z to look along `front`'s flat
// direction. 0 when `front` is (near) vertical.
float FirstPersonBodyYaw(const glm::vec3& front, float fallback = 0.0f);
// An angle wrapped to (-pi, pi].
float FirstPersonBodyWrapAngle(float radians);
// A world velocity in the frame of a body at heading `yaw`: x = to its right, y = forward.
glm::vec2 FirstPersonBodyLocalMove(const glm::vec3& worldVelocity, float yaw);
// m/s (world, flat) the gait loop `stateName` on the body's driver carries the body at PlayRate 1 and the controller's
// current parameters: its clips' root travel as the phase-synced blend plays them (each child covers its whole clip in a
// cycle as long as the children's weighted mean length), times the state's speed and the driver's world scale. The open-loop
// half of BodyLocomotion::LoopPlayRate. < 0 when it can't be read (no controller, state, root bone or attached clip).
float FirstPersonBodyLoopTravel(const World& world, entt::entity driver, const char* stateName);
// The eye in model space: the head's standing position, plus `bob` of the head's motion away
// from it, plus `offset` given in the body's frame (x right, y up, z forward).
// How far the pelvis moves (metres, + up) to put the feet on ground `offL` / `offR` above the
// capsule's: down to the lower foot (at most `maxDrop`), up a little (`maxRaise`) when both are higher.
// How far into the look-down push the view is: 0 until it pitches `startDegrees` below level, 1 straight
// down, rising as a sine between (fast at first, like a head pitching forward). `pitchRadians` is + up.
float FirstPersonBodyLookDown(float pitchRadians, float startDegrees);
// The turn (model space) that brings the body's shoulder line `bodyAcross` (right to left upper arm) onto
// the arms rig's `rigAcross`, `weight` of the way (0..1). Identity when either is degenerate.
glm::quat FirstPersonBodyShoulderLineTurn(const glm::vec3& bodyAcross, const glm::vec3& rigAcross, float weight);
// The rig's shoulder line `rigAcross` (model space, +Y up) with its tilt out of the horizontal taken only by
// `tilt` (0..1, the rest the body's own `bodyAcross` tilt); its heading (the blade) and length are kept.
glm::vec3 FirstPersonBodyShoulderLineTilt(const glm::vec3& bodyAcross, const glm::vec3& rigAcross, float tilt);
// How much of Armed Eye Offset applies at view pitch `pitchRadians` (+ = up): all of it within 25 degrees of
// level, none from 60 degrees up or down, eased between.
float FirstPersonBodyArmedEyeLift(float pitchRadians);
// The share of the view's pitch the spine takes: Spine Aim looking up, Spine Aim Down looking down.
float FirstPersonBodySpineAim(float pitchRadians, float spineAim, float spineAimDown);
// How far segment `a`-`b` must move along unit `dir` for every one of `points` to be at least `clearance` from it:
// the first of 0, `step`, 2 `step` ... that clears, and `maxPush` when none up to it does. 0 when already clear.
float FirstPersonBodyClearPush(const std::vector<glm::vec3>& points, const glm::vec3& a, const glm::vec3& b, const glm::vec3& dir,
                               float clearance, float maxPush, float step);
// The body a world gun is seated on (FirstPersonGunSeat), world space: the upper arms' heads, the neck and head bones,
// and - when known this frame - the chest (spine_05) and the drawn head / torso vertices.
struct FirstPersonGunSeatBody {
    glm::vec3 UpperL{0.0f}, UpperR{0.0f}, Neck{0.0f}, Head{0.0f};
    const glm::vec3* Chest = nullptr;
    const std::vector<glm::vec3>* HeadPoints = nullptr;
    const std::vector<glm::vec3>* TorsoPoints = nullptr;
};
// How far the world gun moves off `gun.ButtWorld` / `gun.ForwardWorld` to sit on `body`: its butt into the right shoulder
// pocket by Shouldered; out of the neck and hood keep-outs; out of the drawn head (and, by Torso Keep Out, the torso) by
// Mesh Clearance; at most Max Shift. The mesh part is written to `meshPush` when points were given, and `cachedMeshPush`
// stands in for it when not (a caller skinning every few frames). (No caller since the soldiers went and the player's world
// body took the 3P clips' hold as it is; kept, with its unit test, until those are retired together.)
glm::vec3 FirstPersonGunSeat(const FirstPersonWorldGunInput& gun, const FirstPersonGunSeatBody& body, glm::vec3* meshPush = nullptr,
                             const glm::vec3* cachedMeshPush = nullptr);
// The hand anchor's share for something `distance` off the gun's box: 0 within `nearDist`, 1 from `farDist`, smoothstep between.
float FirstPersonAnchorWeight(float distance, float nearDist, float farDist);
// `m` (any frame, scale kept) moved `weight` of the way by the rigid world move `move`: its origin along the straight line,
// its turn by slerp. 0 = `m`, 1 = `move` * `m`.
glm::mat4 FirstPersonPartialMove(const glm::mat4& move, const glm::mat4& m, float weight);
// The arms rig's arm shapes onto `pose` of model `m` (rotations of every node under the clavicles, the body
// keeping its bone lengths), by `weight`; the clavicles themselves by `weight * clavicleWeight`. What the
// player's arms take before reaching the rig's hands.
void FirstPersonBodyCopyArmShape(const Model& m, const Model& rig, float weight, std::vector<LocalTRS>& pose,
                                 const std::vector<int>& parents, float clavicleWeight);
// The same with the body / rig node pairs worked out once (FirstPersonBodyArmShapeLinks), for a body that holds the
// same rig frame after frame - no name lookups per frame.
void FirstPersonBodyArmShapeLinks(const Model& m, const Model& rig, const std::vector<int>& parents,
                                  std::vector<FirstPersonArmShapeLink>& out);
// Grip Inset's direction: the way `rig`'s hand on `side` (0 left, 1 right) closes, world - from its knuckle toward its
// curled middle fingertip, across the hand's length. Zero when the rig has no such bones or the hand is open.
glm::vec3 FirstPersonBodyPalmDirection(const Model& rig, const glm::mat4& rigWorld, int side);
// Finger Curl Extra: every finger joint of `m` (index / middle / ring / pinky 01-03, not the thumb) turned `degrees`
// further about the axis it has already turned on from its rest - the way the grip closes it.
void FirstPersonBodyCurlFingers(const Model& m, std::vector<LocalTRS>& pose, float degrees);
void FirstPersonBodyCopyArmShape(const std::vector<FirstPersonArmShapeLink>& links, const Model& rig, float weight,
                                 std::vector<LocalTRS>& pose, float clavicleWeight);
// The arm solve's steps (FirstPersonBody::LateUpdate), kept as free functions for reuse; where callers differ, the
// difference is a parameter.
// Frame-rate independent approach toward a target over `seconds`: the share of the way to go this step. 1 (a snap) at
// `snapBelow` seconds or less.
float FirstPersonBodyFollow(float dt, float seconds, float snapBelow = 1e-4f);
// A rotation's angle, radians (0..pi).
float FirstPersonBodyAngleOf(const glm::quat& q);
// The collarbone turned about its inner end so the upper arm heads for `want` (model space), by at most `maxAngle` -
// never slid, which stretched the skin between neck and shoulder. Returns the turn made (model space).
glm::quat FirstPersonBodyTurnClavicle(std::vector<LocalTRS>& pose, const std::vector<int>& parents, std::vector<glm::mat4>& globals,
                                      int clav, int upper, const glm::vec3& want, float maxAngle);
// Reach Lean: the turn (model space) the chest at node `chest` takes toward the hand furthest out of reach once its
// collarbone has turned all it may (`shoulderMax`), at most `leanMax`, `weight` of it. Per side the arm's nodes (-1 none),
// whether it holds the gun and the hand's target (world, `toModel` into model space). False when no hand is out of reach.
bool FirstPersonBodyReachLean(const std::vector<glm::mat4>& globals, const int (&upper)[2], const int (&lower)[2], const int (&hand)[2],
                              const int (&clav)[2], const bool (&haveHand)[2], const glm::vec3 (&handPos)[2], const glm::mat4& toModel,
                              int chest, float shoulderMax, float leanMax, float weight, glm::quat& turn);
// The arms rig's shoulders off its camera bone (the upper arms' and the camera bone's model-space nodes, the rig's world
// transform), in the view's frame (x right, y up, z forward).
glm::vec3 FirstPersonBodyRigEyeToShoulders(const glm::mat4& rigWorld, const glm::mat4& upperL, const glm::mat4& upperR,
                                           const glm::mat4& cameraBone, const Camera& cam);
// Seconds the eye's anchor (the shoulders armed, the head unarmed) takes to follow the clips': the one smoothing on it.
// The walk's sway and bob stay out of the view (the bob is the procedural camera's); a crouch, a stop's lean come through.
constexpr float kFirstPersonEyeFollowSeconds = 0.5f;
// The shoulders (or head) the eye hangs off (model space), a step on: `eased` follows the clips' `animated` over
// kFirstPersonEyeFollowSeconds and never trails them by more than `eyeSlack` - pulled back in over `slackSeconds`, or
// snapped there at 0 (the arms' reach: past it a hand comes off the gun). `have` false starts it on `animated`.
void FirstPersonBodyFollowShoulders(glm::vec3& eased, bool& have, const glm::vec3& animated, float eyeSlack, float slackSeconds, float dt);
// Armed Eye Offset (the body's frame: x right, y up, z forward) in model space, by how much of it applies at `pitchRadians`.
glm::vec3 FirstPersonBodyArmedEyeOffset(const glm::vec3& offset, float pitchRadians);
// FirstPersonBodyArmShapeLinks' core, by node index: `rigOf(i)` is body node i's counterpart in the rig (-1 none),
// `clavicles` the body's two collarbone nodes (-1 none). The nodes under a clavicle (parents come first), in order.
void FirstPersonBodyArmShapeLinksFrom(int count, const std::vector<int>& parents, const int (&clavicles)[2],
                                      const std::function<int(int)>& rigOf, std::vector<FirstPersonArmShapeLink>& out);
float FirstPersonBodyFootPelvis(float offL, float offR, float maxDrop, float maxRaise);
// One step of the body's height spring: critically damped toward `target`, fed its rate `targetRate` (so a steady
// ramp is followed without lag); `lag` seconds is how softly it takes a jump.
void FirstPersonBodyStepSpring(float& value, float& velocity, float target, float targetRate, float lag, float dt);
// The ground as a ramp: `heights` sampled evenly along a window of `span` metres (centre to centre), the travel's
// way. The mean is the ramp's height at the middle; a least-squares fit, its slope (rise per metre).
void FirstPersonBodyGroundRamp(const float* heights, int count, float span, float& height, float& slope);
glm::vec3 FirstPersonBodyEye(const glm::vec3& restHead, const glm::vec3& head, float bob, const glm::vec3& offset);
// A piece's Near Hide: the body's, and for clothing at least Clothing Near Hide.
float FirstPersonBodyPieceNearHide(float nearHide, float clothingNearHide, bool clothing);
// Whether world point `p` is inside the near-eye hide (as ModelFragment.glsl tests it): within `radius` of
// `eye`, stretched to `width` along `right` (a unit vector, the view's flat right). width <= 0: a sphere.
bool FirstPersonBodyNearHidden(const glm::vec3& p, const glm::vec3& eye, const glm::vec3& right, float radius, float width);
// A clothing piece's vertices around the neck in its bind pose (model space): at or above the shoulder
// joints less `drop`, between them (a little past, for the shoulder tops) and within 30 cm of the neck
// front to back. 1 = hide. Empty when `drop` < 0 or the shoulders are degenerate.
std::vector<std::uint8_t> FirstPersonBodyCollarVertices(const std::vector<glm::vec3>& bindPositions, const glm::vec3& neck,
                                                        const glm::vec3& shoulderL, const glm::vec3& shoulderR, float drop);
