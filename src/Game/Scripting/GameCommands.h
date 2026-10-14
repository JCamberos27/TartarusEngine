// The bit flags the project gameplay scripts set in a frame's Commands / Triggers (GameFrames.h). The values are ABI:
// project/assets/Scripts/GameplayContracts.cs holds the same numbers (SessionCommand, BodyMotionTrigger).
#pragma once
namespace Scripting {

// GameSessionFrame::Commands. Operation 0 = input, 1 = cheats, 2 = death and respawn, 3 = a round's hit feedback.
enum SessionCommand : int {
    kSessionSlot1        = 1,      // 0: the AK
    kSessionSlot2        = 2,      // 0: the Remington
    kSessionUnarmed      = 4,      // 0: holster (key 3)
    kSessionCycleSlot    = 8,      // 0: the wheel, by SlotDirection
    kSessionHolster      = 16,     // 0: toggle the weapon (asks operation 4 for the result)
    kSessionWeaponInput  = 32,     // 0: the weapon owns the trigger and reload keys this frame
    kSessionFireMode     = 64,     // 0
    kSessionInspect      = 128,    // 0
    kSessionMelee        = 256,    // 0
    kSessionResetReload  = 512,    // 0: the weapon doesn't have the input; drop a held reload
    kSessionRefillAmmo   = 1024,   // 1: infinite ammo, Ammo = Magazine
    kSessionDied         = 2048,   // 2: put the weapon away, drop what's grabbed
    kSessionRespawn      = 4096,   // 2: back at Position / Velocity / Camera
    kSessionDeadCamera   = 8192,   // 2: the death camera's sink and roll
    kSessionHitmarker    = 16384,  // 3
    kSessionKillFeed     = 32768,  // 3
    kSessionWorldImpact  = 65536,  // 3: the round hit the world, not a character
    kSessionFlashlight   = 131072, // 0
};

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
