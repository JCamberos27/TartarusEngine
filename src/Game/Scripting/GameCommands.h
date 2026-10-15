// The bit flags the project gameplay scripts set in a frame's Triggers (GameFrames.h). The values are ABI:
// project/assets/Scripts/GameplayContracts.cs holds the same numbers (BodyMotionTrigger).
#pragma once
namespace Scripting {

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

} // namespace Scripting
