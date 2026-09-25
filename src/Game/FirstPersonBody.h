#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

class Camera;
class Model;
class Player;
class World;
struct FirstPersonBodyComponent;

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
class FirstPersonBody {
public:
    // Finds the scene's First Person Body and takes it over. False (and every call below a
    // no-op) when there is none or it can't run - see LastError. Sets the player's move speeds
    // to the body's Run / Sprint speeds, so the input asks the blend tree for what it has.
    bool Start(World& world, Player& player);
    void Stop(World& world);
    bool IsActive() const { return m_Body != entt::null; }
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
    void ArmsLateUpdate(World& world, entt::entity weaponArms, float viewModelFov, float dt);

    // The controller parameters of the last Tick (body frame: x right, y forward, m/s).
    glm::vec2 Move() const { return m_Move; }
    // The root motion's horizontal velocity (world, m/s) from the last LateUpdate.
    glm::vec3 RootVelocity() const { return m_RootVelocity; }

private:
    void Fail(const std::string& message);
    void ApplySpineAim(const Camera& camera, float amount, float twist);
    void ApplyFootIK(World& world, const FirstPersonBodyComponent& cfg, float dt);

    entt::entity m_Body = entt::null;   // the root: placed at the feet, its pieces ride along
    entt::entity m_Driver = entt::null; // the piece whose Animator Controller runs the body
    std::vector<std::shared_ptr<Model>> m_Models; // every piece's model (Stop un-hides their bones)
    std::vector<entt::entity> m_Pieces;           // ... and its entity, in step with m_Models
    float m_ArmsWeight = 0.0f;                    // 0..1: how much the arms follow the weapon's
    int m_ShoulderNode[2] = {-1, -1};             // upperarm_l / upperarm_r on the driver
    glm::vec3 m_ShouldersSlow{0.0f};              // their midpoint in model space, a slow average (the standing height)
    glm::vec3 m_Shoulders{0.0f};                  // ... the part of the clips' motion the eye follows, smoothed
    bool m_HaveShoulders = false;
    glm::vec3 m_RigEyeToShoulders{0.0f};          // rig: camera bone to its shoulders, in the camera's frame (smoothed)
    bool m_HaveRigOffset = false;
    std::vector<entt::entity> m_ArmsTagged;       // pieces given the ViewModelTag
    std::string m_LastError;
    float m_ViewYaw = 0.0f;        // the view's heading, radians (the body's turns aim at it)
    bool m_HaveHeading = false;
    float m_CrouchHeight = 0.0f, m_CrouchSpeed = 0.42f; // the component's, from Start
    float m_IdleTime = 1.0f;       // seconds with no move input (a start clip needs a real pause)
    glm::vec2 m_LastDir{0.0f, 1.0f}; // the last move direction (body frame) and whether it was a sprint
    bool m_LastSprint = false;
    bool m_Grounded = false;       // at the last Tick
    bool m_ArmsEasedOut = false;   // the arms piece is hidden while it eases off a holstered gun
    int m_ArmsShadow = 1;          // its RenderableComponent::CastShadows before that
    bool m_InLand = false;         // the animator is in the landing state (the input drives the capsule)
    float m_FootWeight = 0.0f;     // 0..1: how much foot IK is on (eases at the edges of grounded)
    bool m_HaveFoot = false;
    float m_StepOffset = 0.0f;     // metres the body is off the capsule's height: a stair's pop, eased out
    float m_LastCapsuleY = 0.0f;
    bool m_HaveCapsule = false, m_LastGrounded = false;
    bool m_FootPlanted[2] = {false, false};     // foot lock: pinned in the world while planted
    glm::vec3 m_FootLock[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    float m_FootLockWeight[2] = {0.0f, 0.0f};
    float m_FootYaw = 0.0f;
    float m_FootOffset[2] = {0.0f, 0.0f};                 // ground under each foot minus the capsule's, smoothed
    glm::vec3 m_FootNormal[2] = {glm::vec3(0, 1, 0), glm::vec3(0, 1, 0)};
    float m_MoveTime = 0.0f;       // seconds of move input in a row (a tap is not a run to stop from)
    bool m_WasCrouched = false;    // for the stand<->crouch edge
    float m_StopDistance = 0.0f, m_StartDistance = 0.0f; // metres the start / stop clip has carried the body
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
    glm::vec3 m_CameraApplied{0.0f}; // what LateUpdate added to the camera (BeforePlayerMove takes it off)
    glm::vec3 m_Feet{0.0f};
    float m_Yaw = 0.0f;           // body heading, radians about +Y (model +Z faces the view)
    glm::vec2 m_Move{0.0f};
    glm::vec3 m_RootVelocity{0.0f};
    float m_AirTime = 0.0f;
    float m_RunSpeed = 0.0f;
    float m_Responsiveness = 0.0f; // the component's, from the last Tick
};

// --- The maths, exposed for tests ------------------------------------------------------------

// The heading (radians about +Y) that turns a model facing +Z to look along `front`'s flat
// direction. 0 when `front` is (near) vertical.
float FirstPersonBodyYaw(const glm::vec3& front, float fallback = 0.0f);
// An angle wrapped to (-pi, pi].
float FirstPersonBodyWrapAngle(float radians);
// Whether a body standing still, `offset` radians off the view, starts turning on the spot.
bool FirstPersonBodyShouldTurn(float offset, float thresholdDegrees);
// A world velocity in the frame of a body at heading `yaw`: x = to its right, y = forward.
glm::vec2 FirstPersonBodyLocalMove(const glm::vec3& worldVelocity, float yaw);
// The eye in model space: the head's standing position, plus `bob` of the head's motion away
// from it, plus `offset` given in the body's frame (x right, y up, z forward).
// How far the pelvis moves (metres, + up) to put the feet on ground `offL` / `offR` above the
// capsule's: down to the lower foot (at most `maxDrop`), up a little (`maxRaise`) when both are higher.
float FirstPersonBodyFootPelvis(float offL, float offR, float maxDrop, float maxRaise);
glm::vec3 FirstPersonBodyEye(const glm::vec3& restHead, const glm::vec3& head, float bob, const glm::vec3& offset);
