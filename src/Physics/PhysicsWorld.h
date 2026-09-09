#pragma once

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
struct TriggerEvent; // GameModuleAPI.h — POD, shared with the gameplay-module ABI
struct ContactEvent; // GameModuleAPI.h — POD, shared with the gameplay-module ABI
struct BodyState;    // GameModuleAPI.h — POD, shared with the gameplay-module ABI

namespace PhysicsWorld {

// Stand up PxFoundation / PxPhysics / one PxScene / a CPU dispatcher / a shared material, then
// build a static actor for every collider in `world`. Idempotent: a second Create() without an
// intervening Destroy() is a no-op. Called from EditorLayer::OnEnterPlayMode.
void Create(const World& world);

// Tear everything down in reverse order. Idempotent — safe to call when nothing is active,
// which is why the exit-while-playing path (main.cpp) can call it unconditionally via
// OnExitPlayMode. Called from EditorLayer::OnExitPlayMode.
void Destroy();

// True between a successful Create() and the next Destroy().
bool IsActive();

// Advance the simulation by real-frame `dt` seconds, accumulated into fixed 1/60 s sub-steps
// (at most 4 per call, so a long hitch doesn't spiral). Before stepping, kinematic bodies are
// pushed from `world`'s TransformComponents; after, dynamic bodies' simulated poses are written
// back into them. No-op when the world isn't active. Called once per simulated frame.
void Step(float dt, World& world);

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

// Fill outXYZRGBA (interleaved pos.xyz + colour.rgba — 7 floats/vertex, 2 vertices/line) with
// this frame's debug lines; returns the line count written (capped at maxLines). Additive-blend
// friendly: alpha carries the fade.
int CopyDebugLines(float* outXYZRGBA, int maxLines);

// --- Slow-mo / step ------------------------------------------------------------------
void  SetSimTimeScale(float scale);     // clamped [0, 2]; 0 = frozen. Persists across sessions.
float GetSimTimeScale();
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
void SetActorPose(unsigned entity, const float posXYZ[3], const float rotEulerDeg[3], bool zeroVelocity);

// --- Gravity gun (#185 hardening) ---------------------------------------------------
// Drives the physics-playground grab/throw harness in main.cpp. GrabBody latches a dynamic
// body and suspends its gravity; UpdateGrab servos it toward `target` (world space) each
// frame and kills its spin; ReleaseBody restores it, applying `impulse` as a velocity change
// when `launch` is true. All no-ops outside Play or on a non-dynamic entity.
void GrabBody(unsigned entity);
void UpdateGrab(const float target[3]);
void ReleaseBody(bool launch, const float impulse[3]);
bool IsGrabbing();

// --- Debug (#185 PR 12) -------------------------------------------------------------
// Copy up to `maxPoints` world-space contact points from this frame's events (3 floats each)
// into `outXYZ`; returns the count written. For the collider gizmo's contact markers.
int CopyContactPoints(float* outXYZ, int maxPoints);

} // namespace PhysicsWorld
