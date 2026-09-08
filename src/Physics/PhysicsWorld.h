#pragma once

// PhysX 5 simulation world — created when Play mode starts, destroyed when it ends (#185).
//
// PR 1 scope: this owns a PxScene and steps it at a fixed timestep with ZERO actors. No
// colliders, no character controller, no gameplay queries yet — those are PR 2-6 (see #185).
// The point of landing it empty first is to isolate the PhysX build/link/lifetime plumbing
// from any behaviour change: with no actors the scene does nothing observable.
//
// This header deliberately pulls in NO PhysX headers. Every PhysX type lives behind the pimpl
// in PhysicsWorld.cpp so the SDK's include surface (and its /MD, exception, and alignment
// requirements) stays confined to that one translation unit. All PhysX-touching code is
// host-only (TartarusEngine.exe) — never the reloadable TartarusGame / TartarusEditor DLLs.

namespace PhysicsWorld {

// Stand up PxFoundation / PxPhysics / one PxScene / a CPU dispatcher / a shared material.
// Idempotent: a second Create() without an intervening Destroy() is a no-op. Called from
// EditorLayer::OnEnterPlayMode.
void Create();

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

} // namespace PhysicsWorld
