#pragma once

// PhysX 5 simulation world — created when Play mode starts, destroyed when it ends (#185).
//
// PR 1 stood up an empty PxScene. PR 2 populated it with a static actor per ColliderComponent
// and added Raycast() scene queries. PR 3 adds the character controller: the Play-mode Player
// now sweep-moves a PxCapsuleController through this scene instead of the old AABB push-out
// (World::ResolveCollisions, deleted). Nothing writes back to a TransformComponent, so edit
// mode is unchanged; Play-mode movement now has real step/slope handling.
//
// This header deliberately pulls in NO PhysX headers. Every PhysX type lives behind the pimpl
// in PhysicsWorld.cpp so the SDK's include surface (and its /MD, exception, and alignment
// requirements) stays confined to that one translation unit. All PhysX-touching code is
// host-only (TartarusEngine.exe) — never the reloadable TartarusGame / TartarusEditor DLLs.

class World;
struct RaycastHit; // GameModuleAPI.h — POD, shared with the gameplay-module ABI

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
// (at most 4 per call, so a long hitch doesn't spiral). No-op when the world isn't active.
// Called once per simulated frame from the main loop.
void Step(float dt);

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

// Sweep-move the capsule by `disp` (world units, already scaled by dt at the call site).
// Returns a mask of CharacterCollision bits; 0 when there is no character or world.
unsigned MoveCharacter(const float disp[3], float dt);

} // namespace PhysicsWorld
