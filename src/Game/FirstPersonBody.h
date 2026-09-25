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
    void LateUpdate(World& world, Camera& camera, float dt);
    // Phase 2, after FirstPersonPresentation::LateUpdate has seated the arms rig and gun: the
    // body's arms take that rig's arm pose and reach their hands onto its hands (Weapon Arms).
    // `weaponArms` is the presentation's arms entity (null = none), `viewModelFov` its sub-pass
    // FOV in degrees. The arms rig stops being drawn while this holds; the gun still is.
    void ArmsLateUpdate(World& world, const Camera& camera, entt::entity weaponArms, float viewModelFov, float dt);

    // The controller parameters of the last Tick (body frame: x right, y forward, m/s).
    glm::vec2 Move() const { return m_Move; }
    // The root motion's horizontal velocity (world, m/s) from the last LateUpdate.
    glm::vec3 RootVelocity() const { return m_RootVelocity; }

private:
    void Fail(const std::string& message);
    void ApplySpineAim(const Camera& camera, float amount);

    entt::entity m_Body = entt::null;   // the root: placed at the feet, its pieces ride along
    entt::entity m_Driver = entt::null; // the piece whose Animator Controller runs the body
    std::vector<std::shared_ptr<Model>> m_Models; // every piece's model (Stop un-hides their bones)
    std::vector<entt::entity> m_Pieces;           // ... and its entity, in step with m_Models
    float m_ArmsWeight = 0.0f;                    // 0..1: how much the arms follow the weapon's
    std::string m_LastError;
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
// A world velocity in the frame of a body at heading `yaw`: x = to its right, y = forward.
glm::vec2 FirstPersonBodyLocalMove(const glm::vec3& worldVelocity, float yaw);
// The eye in model space: the head's standing position, plus `bob` of the head's motion away
// from it, plus `offset` given in the body's frame (x right, y up, z forward).
glm::vec3 FirstPersonBodyEye(const glm::vec3& restHead, const glm::vec3& head, float bob, const glm::vec3& offset);
// A point in camera space (x right, y up, z forward) as the world pass must draw it to land where
// the view-model pass puts it: the two projections differ only in FOV, so scaling x and y by the
// ratio of their tangents keeps the screen position. Depth is untouched. Identity when either
// FOV is not positive.
glm::vec3 FirstPersonBodyViewModelToWorldFov(const glm::vec3& cameraSpace, float worldFovDeg, float viewModelFovDeg);
