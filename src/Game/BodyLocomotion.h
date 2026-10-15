#pragma once
#include "Scripting/GameFrames.h"

// The first-person body's locomotion decisions, each frame: what the gait blend is fed, when a start, stop, pivot,
// step, turn or fidget plays, and how far along its travel each distance-matched clip should be. The capsule moves on
// the input (input-led); nothing here holds the player back - the clips are picked and paced to fit its travel.
namespace BodyLocomotion {
void Update(Scripting::BodyMotionFrame& f);
// How far a capsule easing (time constant tau) from speed v toward speed `target` the other way still goes
// before it turns: integral of v(t) to its zero crossing.
float DistanceToTurn(float v, float target, float tau);
}
