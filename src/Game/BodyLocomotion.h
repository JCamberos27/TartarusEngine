#pragma once
#include "GameFrames.h"

// The first-person body's locomotion decisions, each frame: what the gait blend is fed, when a start, stop, pivot,
// step, turn or fidget plays, and how far along its travel each distance-matched clip should be. The capsule moves on
// the input (input-led); nothing here holds the player back - the clips are picked and paced to fit its travel.
namespace BodyLocomotion {
void Update(Gameplay::BodyMotionFrame& f);
// How far a capsule easing (time constant tau) from speed v toward speed `target` the other way still goes
// before it turns: integral of v(t) to its zero crossing.
float DistanceToTurn(float v, float target, float tau);
// The gait loop's play rate (its PlayRate parameter): the capsule's speed over what the loop's clips carry the body at
// rate 1 at the current blend (`loopTravel`, m/s - FirstPersonBodyLoopTravel), so their feet keep its pace wherever the
// blend sits: below the walk (the idle in the blend stretches the shared cycle), between the walk and the jog, off the
// eight directions, past the slower back and side clips. Faded to 1 toward a standstill (the idle plays as authored) and
// by `responsiveness` (at 0 the clips move the capsule: their travel is its speed). Within [0.5, maxRate].
float LoopPlayRate(float capsuleSpeed, float loopTravel, float responsiveness, float maxRate);
}
