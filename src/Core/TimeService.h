#pragma once
#include <cstdint>

// Frame timing for the editor and the game - the engine's counterpart of Unity's Time (#144).
// Replaces the old Clock, whose float total lost precision after a few hours, whose 0.1 s dt
// clamp was hard-coded and hid hitches, and whose time scale only reached physics (#169).
//
// Three deltas, all in seconds, all set once per frame by BeginFrame():
//   RealDeltaTime      wall-clock frame time, never clamped. Profiler / FPS readouts.
//   UnscaledDeltaTime  RealDeltaTime clamped to MaximumDeltaTime, so a hitch (breakpoint, asset
//                      load) becomes one long-but-bounded step instead of a simulation spiral.
//                      Editor camera, UI animation, anything that must ignore the time scale.
//   DeltaTime          UnscaledDeltaTime * the effective time scale. Everything that is part of
//                      the running game: physics, Player, animators, the game module.
//
// The effective time scale is TimeScale (the game's own, Unity's Time.timeScale: seeded from
// Project Settings when Play starts, settable by the game module) times DebugTimeScale (the
// editor's Physics-panel slow-mo slider, a session-only testing aid).
//
// Main thread only.
namespace Time {

// Advances the frame. Call exactly once at the top of every frame.
void BeginFrame();

float DeltaTime();
float UnscaledDeltaTime();
float RealDeltaTime();

// Scaled seconds accumulated from DeltaTime (Unity's Time.time), unscaled seconds from
// UnscaledDeltaTime, and wall-clock seconds since the engine started. Doubles: a float total
// visibly loses precision in shader time / oscillators after a few hours.
double TimeSinceStartup();
double UnscaledTimeSinceStartup();
double RealtimeSinceStartup();

// Frames begun since startup.
std::uint64_t FrameCount();

// The game's time scale: 0 freezes, 1 is real time. Clamped to [0, 100].
float TimeScale();
void  SetTimeScale(float scale);

// Editor-only multiplier on top of TimeScale (Physics panel slow-mo). Clamped to [0, 2].
void  SetDebugTimeScale(float scale);
float DebugTimeScale();
float EffectiveTimeScale();

// Upper bound on UnscaledDeltaTime. Clamped to [0.01, 1] (Unity: Maximum Allowed Timestep).
float MaximumDeltaTime();
void  SetMaximumDeltaTime(float seconds);

// Project Settings > Physics > Fixed Timestep (the physics / FixedUpdate step), guarded against
// zero or negative values.
float FixedDeltaTime();

// Waits until this frame has lasted at least 1/targetFps seconds, measured from BeginFrame().
// No-op when targetFps <= 0. Sleeps on a high-resolution waitable timer where Windows has one
// (1803+) and only spins through the final fraction of a millisecond, instead of burning a core
// for the last 1.5 ms of every frame.
void LimitFps(int targetFps);

} // namespace Time
