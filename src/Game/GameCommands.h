// The bit flags BodyLocomotion::Update sets in BodyMotionFrame::Triggers (GameFrames.h).
#pragma once
namespace Gameplay {

// BodyMotionFrame::Triggers: bit i fires the body animator trigger i, in this order.
enum BodyMotionTrigger : int {
    kBodyCrouchDown = 1,
    kBodyCrouchUp   = 2,
    kBodyStart      = 4,
    kBodyStop       = 8,
    kBodyStopRun    = 16,
    kBodyJump       = 32,
    kBodyPivot      = 64,
    kBodyStep       = 128,
    kBodyFidget     = 256,
};

} // namespace Gameplay
