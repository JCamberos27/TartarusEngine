#pragma once
#include <functional>
#include <vector>

// PhysX 5 simulation world — created when Play mode starts, destroyed when it ends (#185).
//
// PR 1 stood up an empty PxScene. PR 2 populated it with a static actor per ColliderComponent
// and added Raycast() scene queries. PR 3 added the PxCapsuleController the Play-mode Player
// sweeps through it. PR 4 added dynamic bodies (RigidbodyComponent -> PxRigidDynamic, with
// pose write-back). PR 5 adds trigger dispatch: a collider with Is Trigger set reports
// enter/stay/exit for anything overlapping it (Step() collects them from PhysX's onTrigger
// plus a capsule overlap for the Player), surfaced through GameModuleHostAPI. Edit mode is
// still untouched; Play -> Stop restores the authored scene from its JSON snapshot as always.
//
// This header deliberately pulls in NO PhysX headers. Every PhysX type lives behind the pimpl
// in PhysicsWorld.cpp so the SDK's include surface (and its /MD, exception, and alignment
// requirements) stays confined to that one translation unit. All PhysX-touching code is
// host-only (TartarusEngine.exe) — never the reloadable TartarusGame / TartarusEditor DLLs.

class World;
struct RaycastHit;   // GameModuleAPI.h — POD, shared with the gameplay-module ABI
struct QueryFilter;  // GameModuleAPI.h (#170)
struct TriggerEvent; // GameModuleAPI.h — POD, shared with the gameplay-module ABI
struct ContactEvent; // GameModuleAPI.h — POD, shared with the gameplay-module ABI
struct BodyState;    // GameModuleAPI.h — POD, shared with the gameplay-module ABI

namespace PhysicsWorld {

// Stand up PxFoundation / PxPhysics / one PxScene / a CPU dispatcher / a shared material, then
// build a static actor for every collider in `world`. Idempotent: a second Create() without an
// intervening Destroy() is a no-op. Called from EditorLayer::OnEnterPlayMode.
void Create(const World& world);

// Tear down the Play session's scene, actors, joints and character. Idempotent — safe to call
// when nothing is active, which is why the exit-while-playing path (main.cpp) can call it
// unconditionally via OnExitPlayMode. Called from EditorLayer::OnExitPlayMode. PxPhysics, the
// dispatcher, materials and cooked meshes stay up for the next Play (#167).
void Destroy();

// Releases everything, including what Destroy() keeps for the next Play. Call once at exit.
void Shutdown();

// True between a successful Create() and the next Destroy().
bool IsActive();

// Advance the simulation by `dt` game seconds (already time-scaled, see Core/Time.h),
// accumulated into fixed Time::FixedDeltaTime() sub-steps (at most 4 per call, so a long hitch
// doesn't spiral). Before stepping, kinematic bodies are pushed from `world`'s
// TransformComponents; after, dynamic bodies' simulated poses are written back into them.
// `onFixedStep` (optional) runs before every sub-step with the fixed step length - the
// game module's FixedUpdate (#144). No-op when the world isn't active.
void Step(float dt, World& world, const std::function<void(float fixedDt)>& onFixedStep = {});

// Closest hit of the ray `origin` + t*`dir` (dir need not be normalised) within `maxDistance`.
// Returns true and fills `outHit` on a hit; false (with `outHit` left at defaults) on a miss
// or when no world is active. Backs GameModuleHostAPI::Raycast.
bool Raycast(const float origin[3], const float dir[3], float maxDistance, RaycastHit& outHit);

// --- Character controller (#185 PR 3) ---------------------------------------------------
// One kinematic capsule for the Play-mode Player. The Player owns its dimensions and spawn,
// so the controller is created lazily on the first CreateCharacter() call and released with
// the world. All positions are FOOT positions (the bottom of the capsule), world space.

// Bit flags returned by MoveCharacter — which side(s) the capsule touched something this move.
enum CharacterCollision { CC_SIDES = 1, CC_UP = 2, CC_DOWN = 4 };

bool HasCharacter();

// radius + cylinderHalfHeight define the capsule (total height = 2*(radius+cylinderHalfHeight));
// footPos places its bottom. No-op if a character already exists or no world is active.
void CreateCharacter(float radius, float cylinderHalfHeight, const float footPos[3]);

// Teleport (no sweep) — used for the spawn drop and the void-fall reset.
void SetCharacterFootPosition(const float footPos[3]);
void GetCharacterFootPosition(float outFootPos[3]);

// The Player capsule in world space while playing, for the collider overlay (the Player isn't
// an ECS entity with a ColliderComponent, so nothing else draws it). Foot position, capsule
// radius, and the half-height of the cylindrical section. False when there's no character.
bool GetCharacterCapsule(float outFootPos[3], float* outRadius, float* outCylHalfHeight);

// Sweep-move the capsule by `disp` (world units, already scaled by dt at the call site).
// Returns a mask of CharacterCollision bits; 0 when there is no character or world.
unsigned MoveCharacter(const float disp[3], float dt);

// Change the capsule's cylinder half-height (crouching), keeping the feet where they are.
// False when there is no character.
bool ResizeCharacter(float cylinderHalfHeight);
// Whether the capsule, at that cylinder half-height and the current foot position, is clear of
// solid geometry (triggers and the character itself ignored): standing up from a crouch.
bool CharacterFitsAt(float cylinderHalfHeight);

// --- NPC characters ------------------------------------------------------------------
// More kinematic capsules, one per AI character, beside the Player's. Each is tagged with its
// owner entity (so shots that hit it report that entity) and sits on the Default layer. They
// don't ride platforms or fire the Player's trigger overlap. Ids are small ints, -1 = none;
// all released with the world.
using CharacterId = int;
constexpr CharacterId kNoCharacter = -1;
CharacterId CreateNpcCharacter(unsigned entity, float radius, float cylinderHalfHeight, const float footPos[3]);
void     DestroyNpcCharacter(CharacterId id);
unsigned MoveNpcCharacter(CharacterId id, const float disp[3], float dt);
void     SetNpcFootPosition(CharacterId id, const float footPos[3]);
bool     GetNpcCapsule(CharacterId id, float outFootPos[3], float* outRadius, float* outCylHalfHeight);
bool     ResizeNpcCharacter(CharacterId id, float cylinderHalfHeight);
bool     NpcFitsAt(CharacterId id, float cylinderHalfHeight);
// The NPC character owned by `entity`, or kNoCharacter.
CharacterId NpcCharacterOf(unsigned entity);

// --- Ragdolls ------------------------------------------------------------------------
// A dead character's body as jointed capsules (Npc/NpcRagdoll.h). Each part is a capsule along its
// local +X, posed in world space; part 0 is the root, every other names its parent, and a D6 joint at
// `Anchor` (world) lets it swing `SwingDeg` and twist `TwistDeg` about the parent. The parts don't
// collide with each other (only the world), and report `entity` when queries or contacts hit them.
struct RagdollPart {
    int Parent = -1;
    float Position[3] = {0, 0, 0};
    float Rotation[4] = {0, 0, 0, 1}; // xyzw
    float HalfLength = 0.1f, Radius = 0.05f, Mass = 1.0f;
    float Anchor[3] = {0, 0, 0};
    float SwingDeg = 45.0f, TwistDeg = 20.0f;
    float Velocity[3] = {0, 0, 0};
};
int  CreateRagdoll(unsigned entity, const RagdollPart* parts, int count); // -1 on failure
void DestroyRagdoll(int ragdoll);
void RagdollImpulse(int ragdoll, int part, const float impulse[3], const float point[3]);
bool GetRagdollPart(int ragdoll, int part, float outPos[3], float outRotXYZW[4]);
bool RagdollAsleep(int ragdoll);
// The joints' slerp drives: each part held toward its target orientation (identity: the pose the ragdoll was built in,
// see SetRagdollDriveTarget) as a spring of `stiffness` / `damping` (acceleration: mass independent). 0 = limp.
void SetRagdollDrive(int ragdoll, float stiffness, float damping);
// Where part `part`'s drive wants it, as its rotation (xyzw) relative to its parent from the built pose.
void SetRagdollDriveTarget(int ragdoll, int part, const float rotXYZW[4]);

// --- Hitboxes ---------------------------------------------------------------------------
// Per-bone hitboxes for a living character: kinematic, query-only capsules (along local +X, posed in world space)
// that report the owner entity. While they are active, an unscoped query (the player's shots, grabs, picks) hits them
// and skips the character's capsule; a ScopedQueryPolicy query (an NPC's own, its sight) is the other way round, and
// the *Solid queries see neither. Ids are small ints, -1 = none; released with the world.
struct HitCapsule {
    float Position[3] = {0, 0, 0};
    float Rotation[4] = {0, 0, 0, 1}; // xyzw
    float HalfLength = 0.1f, Radius = 0.05f;
};
int  CreateNpcHitboxes(CharacterId owner, const HitCapsule* parts, int count);
void SetNpcHitboxPoses(int id, const HitCapsule* parts, int count); // poses only (Position / Rotation)
void SetNpcHitboxesActive(int id, bool active);                      // false: the capsule answers queries again
void DestroyNpcHitboxes(int id);

// A ray against body parts only - the hitboxes of living characters and the parts of ragdolls - for which part of
// a body a round struck. `kind` 1 = hitbox, 2 = ragdoll part; `part` is the index in the soldier's part order.
struct BodyPartHit {
    int Kind = 0;
    unsigned Entity = 0xFFFFFFFFu;
    int Part = -1;
    float Distance = 0.0f;
    float Point[3] = {0, 0, 0};
};
bool RaycastBodyParts(const float origin[3], const float dir[3], float maxDistance, BodyPartHit& out);

// Who scene queries may hit, for the calls made while one of these is alive (innermost wins,
// per thread). By default a query never hits the Player capsule; an NPC's own queries set
// `ignoreEntity` to itself (its capsule and hitboxes) and `hitPlayer` so its shots and sight
// rays can reach the Player. `hitCharacters` false skips every NPC capsule. The *Solid queries
// always skip NPC capsules - a character isn't cover or ground.
class ScopedQueryPolicy {
public:
    ScopedQueryPolicy(unsigned ignoreEntity, bool hitPlayer, bool hitCharacters = true);
    ~ScopedQueryPolicy();
    ScopedQueryPolicy(const ScopedQueryPolicy&) = delete;
    ScopedQueryPolicy& operator=(const ScopedQueryPolicy&) = delete;
};

// Every solid, unmoving shape in the scene (static actors, plus kinematic ones when asked) as
// world-space triangles, for building a navigation mesh: boxes, convex and triangle meshes and
// heightfields exactly, spheres and capsules as their bounding boxes. Triggers, characters and
// simulated bodies are left out. Appends to `verts` (xyz) / `tris` (indices); returns the triangles added.
int CollectStaticGeometry(std::vector<float>& verts, std::vector<int>& tris, bool includeKinematic);

// --- Triggers (#185 PR 5) --------------------------------------------------------------
// Step() rebuilds this frame's enter/stay/exit list from PhysX's onTrigger callback (dynamic
// and kinematic bodies) plus a capsule overlap for the Player. Backs
// GameModuleHostAPI::GetTriggerEvents.

// Copy up to `maxEvents` of this frame's transitions into `out`; return the total count.
int GetTriggerEvents(TriggerEvent* out, int maxEvents);

// True while something is inside the given trigger entity's volume — for the editor gizmo.
bool IsTriggerOccupied(unsigned triggerEntity);

// --- Forces & read-back (#185 PR 7) ---------------------------------------------------
// `mode` is a GameModuleAPI ForceMode. All no-ops on an unknown / non-dynamic / kinematic
// entity or outside Play.
void AddForce(unsigned entity, const float force[3], unsigned mode);
void AddTorque(unsigned entity, const float torque[3], unsigned mode);
void AddForceAtPosition(unsigned entity, const float force[3], const float worldPos[3], unsigned mode);
void AddExplosionForce(const float center[3], float radius, float strength, float upwardBias);
void SetLinearVelocity(unsigned entity, const float v[3]);

// Fills `out`; returns false (out.Valid == false) for anything but a live dynamic body.
bool GetBodyState(unsigned entity, BodyState& out);

// This frame's solid-contact transitions; same copy/return convention as GetTriggerEvents.
int GetContactEvents(ContactEvent* out, int maxEvents);

// --- Shape queries (#185 PR 9) --------------------------------------------------------
bool SphereCast(const float origin[3], const float dir[3], float radius, float maxDistance,
                RaycastHit& outHit);
int  OverlapSphere(const float center[3], float radius, unsigned* out, int maxEntities);

// #170 - layer-masked / trigger-aware versions and the remaining Unity shapes. See
// GameModuleHostAPI for the parameter conventions (rotation = quaternion xyzw, null = identity).
bool RaycastFiltered(const float origin[3], const float dir[3], float maxDistance, const QueryFilter& f,
                     RaycastHit& outHit);
int  RaycastAll(const float origin[3], const float dir[3], float maxDistance, const QueryFilter& f,
                RaycastHit* out, int maxHits);
bool SphereCastFiltered(const float origin[3], const float dir[3], float radius, float maxDistance,
                        const QueryFilter& f, RaycastHit& outHit);
// SphereCastFiltered that only hits the solid world: statics and kinematic bodies (doors,
// platforms), not simulated props - for the weapon's wall and corner probes, so a rolling ball
// isn't cover. Engine-side only (not in the gameplay-module API).
// RaycastFiltered that only hits the solid world (statics and kinematic bodies), not simulated props:
// for the foot IK's ground probe, so a rolling ball is not ground. Engine-side only.
bool RaycastSolid(const float origin[3], const float dir[3], float maxDistance, const QueryFilter& f, RaycastHit& outHit);
bool SphereCastSolid(const float origin[3], const float dir[3], float radius, float maxDistance,
                     const QueryFilter& f, RaycastHit& outHit);
bool BoxCast(const float center[3], const float halfExtents[3], const float rotation[4], const float dir[3],
             float maxDistance, const QueryFilter& f, RaycastHit& outHit);
bool CapsuleCast(const float point1[3], const float point2[3], float radius, const float dir[3],
                 float maxDistance, const QueryFilter& f, RaycastHit& outHit);
int  OverlapSphereFiltered(const float center[3], float radius, const QueryFilter& f, unsigned* out, int maxEntities);
int  OverlapBox(const float center[3], const float halfExtents[3], const float rotation[4], const QueryFilter& f,
                unsigned* out, int maxEntities);

// (#185 PR 10: pushing dynamic bodies and riding moving platforms is handled inside
// MoveCharacter via the CCT hit report.) Yaw the ground platform turned through this frame,
// in degrees — Player::Update adds it to the camera so a spinning platform carries the view.
// Zero when not standing on a rotating kinematic body.
float PlatformYawDelta();

// --- Debug tooling (#185) ==========================================================
// A small, general-use visual debugger: shape wireframes, contacts/impacts, raycasts, and
// velocity arrows in the Scene viewport, plus a slow-mo control. Everything here is a no-op /
// empty result when no world is active. None of it touches the game-module ABI.

struct PhysicsDebugStats {
    bool  Active = false;
    int   Substeps = 0;              // fixed substeps run in the last Step()
    float StepMillis = 0.0f;         // wall time inside simulate()+fetchResults() last Step()
    float TimeScale = 1.0f;
    int   DynamicBodies = 0, KinematicBodies = 0, StaticActors = 0;
    int   AwakeBodies = 0, AsleepBodies = 0;
    int   ContactsThisFrame = 0;
    int   TriggerOverlaps = 0;
    int   Joints = 0, BrokenJoints = 0;
    float Gravity[3] = {0, 0, 0};
    float FixedStep = 1.0f / 60.0f;
    unsigned FrameIndex = 0;
};
PhysicsDebugStats GetDebugStats();

// The four engine-drawn overlay channels (collision-shape wireframes stay on their own
// EditorSettings::ShowColliders toggle). Bitmask persists across Play sessions.
enum PhysicsDebugDrawFlag : unsigned {
    PDD_Contacts = 1u << 0,   // impact / resting contact points: fading spark + impulse-scaled normal arrow
    PDD_Raycasts = 1u << 1,   // recent raycasts & sweeps: ray line, hit burst, hit normal — fading
    PDD_Velocity = 1u << 2,   // per-dynamic-body velocity arrow from the centre of mass
    PDD_Sleep    = 1u << 3,   // dim "z" marker above each sleeping body
};
void     SetDebugDrawFlags(unsigned flags);
unsigned GetDebugDrawFlags();

// #169 - whether trigger enter/exit, solid hits and joint breaks are written to the Console.
// Off by default: in physics-heavy Play they flooded the log and cost frame time.
void SetEventLogging(bool enabled);

// Fill outXYZRGBA (interleaved pos.xyz + colour.rgba — 7 floats/vertex, 2 vertices/line) with
// this frame's debug lines; returns the line count written (capped at maxLines). Additive-blend
// friendly: alpha carries the fade.
int CopyDebugLines(float* outXYZRGBA, int maxLines);

// --- Slow-mo / step ------------------------------------------------------------------
// (The slow-mo scale is Time::DebugTimeScale now and reaches Step() through its dt, #169.)
void  StepOneSubstep(World& world);     // run exactly one fixed substep regardless of accumulator

// --- Query recorder ---------------------------------------------------------------
// Enabled automatically whenever the Raycasts channel is on. Keeps the last 64 queries.
void SetQueryRecording(bool on);
bool GetQueryRecording();

// --- Live editing while playing ------------------------------------------------------
// Teleport a live actor to a world-space pose — lets the editor transform gizmo move a
// simulated body during Play instead of the sim immediately overwriting the drag. Dynamic:
// setGlobalPose + (optionally) zero velocities + wake. Kinematic: next kinematic target.
// Static: setGlobalPose. No-op outside Play or for an unknown entity.
// Rotation is a normalized quaternion in x,y,z,w order.
void SetActorPose(unsigned entity, const float posXYZ[3], const float rotationXYZW[4], bool zeroVelocity);

// --- Gravity gun (#185 hardening) ---------------------------------------------------
// Drives the gravity gun (GravityGun.cpp). GrabBody latches a dynamic body and suspends its
// gravity; UpdateGrab sets the hold point (world space), how fast that point is moving (the
// player walking / turning - fed forward so the body doesn't trail behind) and optionally the
// orientation to hold it at (quaternion xyzw; null = no spin). The body is servoed toward them
// every physics substep, so it moves smoothly whatever the frame rate. ReleaseBody restores it,
// applying `impulse` as a velocity change when `launch` is true. All no-ops outside Play or on a
// non-dynamic entity.
void GrabBody(unsigned entity);
void UpdateGrab(const float target[3], const float targetVelocity[3], const float targetRotation[4]);
// `backspinRadPerSec` > 0 also spins a round (sphere-collider) body backwards about the axis
// across the throw, the way a shot basketball leaves the hand.
void ReleaseBody(bool launch, const float impulse[3], float backspinRadPerSec = 0.0f);
bool IsGrabbing();
// The entity the gravity gun holds, or 0xFFFFFFFF.
unsigned GrabbedEntity();
// World position of the PhysX actor built for `entity` (dynamic, kinematic or static, triggers
// included). False outside Play or for an entity without one.
bool GetActorPosition(unsigned entity, float out[3]);
// World rotation (quaternion xyzw) of the same actor; false outside Play / unknown entity.
bool GetActorRotation(unsigned entity, float outXYZW[4]);
// A dynamic body's linear damping (0 when it has none / isn't dynamic) - for throw prediction.
float GetLinearDamping(unsigned entity);
// Sweeps `entity`'s own collision shape (at its current rotation) from `from` along `dir` for
// `distance`, ignoring the body itself, triggers and the Player. On a hit, fills outHit (Point,
// Normal, Distance = how far the body's origin travelled) and the bounciness / dynamic friction
// PhysX would use for that contact (both materials combined by their combine modes). For the
// gravity gun's throw prediction.
bool SweepBody(unsigned entity, const float from[3], const float dir[3], float distance, RaycastHit& outHit,
               float& outBounciness, float& outFriction);
// The bounding radius of `entity`'s collision shape about its origin (0 if unknown).
float BodyRadius(unsigned entity);

// --- Debug (#185 PR 12) -------------------------------------------------------------
// Copy up to `maxPoints` world-space contact points from this frame's events (3 floats each)
// into `outXYZ`; returns the count written. For the collider gizmo's contact markers.
int CopyContactPoints(float* outXYZ, int maxPoints);

} // namespace PhysicsWorld
