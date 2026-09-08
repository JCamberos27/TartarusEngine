#pragma once

// PhysX 5 simulation world — created when Play mode starts, destroyed when it ends (#185).
//
// PR 1 stood up an empty PxScene. PR 2 populates it: on Create() every non-inactive
// ColliderComponent entity becomes a static PhysX actor, and Raycast() runs scene queries
// against them (exposed to gameplay through GameModuleHostAPI). The player still moves on the
// legacy AABB path (World::ResolveCollisions) until PR 3; nothing here writes back to any
// TransformComponent, so there is still no observable behaviour change in the editor.
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

} // namespace PhysicsWorld
