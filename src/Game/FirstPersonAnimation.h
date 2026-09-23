#pragma once

#include <glm/glm.hpp>

#include <string>
#include <vector>

// Authored description of a paired first-person presentation. The arms and weapon are separate
// rigs, so a semantic state owns both clip references and their shared transition settings. A
// missing weapon clip is intentional (the AK source has no weapon Idle/Walk/Draw/Regrip clips),
// and the runtime must apply its explicit fallback rather than guessing a clip name.
struct FirstPersonAnimationClip {
    std::string Name;
    std::string ArmsClip;
    // A static arms pose baked into ArmsModel (for source exports with no time samples).
    bool ArmsBindPose = false;
    std::string WeaponClip;
    bool Loop = false;
    float Fade = 0.08f;
};

struct FirstPersonAnimationSet {
    std::string ArmsModel;
    std::string WeaponModel;
    std::string DefaultState;
    // Y-X-Z Euler degrees the models themselves need to line up with the play camera, applied
    // before any per-scene View Model Rotation. This belongs to the asset, not the scene: it
    // describes the axis convention of the FBXs named above. The Manny rig comes out of Blender
    // facing model +Z while the engine's camera looks down its own -Z, so without the 180 Y this
    // set renders the arms and weapon behind the camera.
    glm::vec3 ViewRotation{0.0f};
    // The weapon rides the arms rig's gun socket rather than merely sharing the arms entity's
    // pose. `WeaponSocket` names a bone on the ARMS rig, `WeaponRoot` the bone on the WEAPON rig
    // that has to land on it, and `WeaponMountRotation` (Y-X-Z degrees) is the fixed mount between
    // the two - measured from the source FBXs as exactly (0, 90, 90) with zero translation, the
    // same for every clip's frame 0.
    //
    // This is what "the AK is parented to ik_hand_gun" means at runtime. The weapon clips never
    // move the gun: only A_W_ADS keys `root` at all, so without the socket the gun's placement
    // is frozen while the hands move. Measured socket-vs-weapon error, root space:
    // Sprint 11.5-14.5 cm, Draw 18.2 cm, Holster up to 21.7 cm, Aim 67.8 cm.
    // An empty `WeaponSocket` disables the parenting (shared pose, the old behaviour).
    std::string WeaponSocket;
    std::string WeaponRoot;
    glm::vec3 WeaponMountRotation{0.0f};
    std::vector<FirstPersonAnimationClip> Clips;

    const FirstPersonAnimationClip* Find(const std::string& state) const;

    // Parses the .fpsanim JSON payload. On failure, leaves `out` unchanged and writes a concise
    // actionable reason to error when provided.
    static bool FromJsonString(const std::string& text, FirstPersonAnimationSet& out,
                               std::string* error = nullptr);
    static bool LoadFile(const std::string& path, FirstPersonAnimationSet& out,
                         std::string* error = nullptr);
};

// The driver's fixed priority tiers for the AKS74U set's named states (below). A request only
// takes over from whatever is currently holding when it strictly outranks it, or it re-requests
// the exact same state (Fire's "tap again to restart" behavior) - see FirstPersonCanInterrupt.
// Idle/Walk/Sprint/Aim are resting states, not triggerable one-shots, and have no tier.
enum class FirstPersonActionTier { None = -1, Transition = 0, Action = 1, Committed = 2, Equip = 3 };

// Fire/Inspect/MagCheck/Regrip are freely interruptible one-shots; TacReload/EmptyReload/Melee are
// committed once started; Draw/Holster can't be interrupted at all; IdleToSprint/SprintToIdle
// are the lowest tier so any real action pre-empts a locomotion transition. Anything else
// (Idle/Walk/Sprint/Aim, or an unrecognized name) returns None.
FirstPersonActionTier FirstPersonTierOf(const std::string& state);

// True when a request for `state` at `requestedTier` may take over from whatever is currently
// holding (`current` empty / currentTier None = nothing busy).
bool FirstPersonCanInterrupt(const std::string& state, FirstPersonActionTier requestedTier,
                             const std::string& current, FirstPersonActionTier currentTier);

// Idle / Walk / Sprint / Aim from movement + input alone. The caller only consults this once
// nothing is busy holding the current pose. Sprinting (while moving) wins over aiming - there
// is no sprinting in ADS - and aiming wins over walking: the source set has no aim-walk clip,
// and the hip Walk clip would pull the sights off centre, so the driver keeps the Aim pose and
// adds a procedural walk bob on top instead.
std::string FirstPersonRestingState(float planarSpeed, bool sprinting, bool aiming);

// The transition state to play on the way from `from` to `to` ("" = crossfade directly into
// `to` via its own Fade). The source set only authored a transition pair for Idle<->Sprint;
// every other resting-state change has no transition clip.
std::string FirstPersonTransitionVia(const std::string& from, const std::string& to);

// The reload state for a magazine holding `ammo` of `capacity` rounds: EmptyReload when it is
// dry (the clip that also works the bolt), TacReload when it is part-spent, "" when it is full.
std::string FirstPersonReloadStateFor(int ammo, int capacity);

// Seconds of uninterrupted Idle before the next Regrip fidget, from a uniform [0,1] sample:
// 10-20 s, so it reads as an occasional habit rather than a loop.
float FirstPersonRegripDelay(float unit01);

// The reload key is overloaded: a tap reloads, a hold checks the magazine. A tap only resolves
// on release (until then it can't be told from the start of a hold); a hold fires MagCheck the
// moment it crosses kHoldSeconds, and its release then does nothing.
enum class FirstPersonReloadInput { None, Reload, MagCheck };
struct FirstPersonReloadButton {
    // Long enough that a deliberate tap never trips it, short enough that a hold doesn't feel
    // laggy - the same ballpark shooters use for tap/hold on one key.
    static constexpr float kHoldSeconds = 0.35f;
    bool Down = false;
    bool Fired = false;
    float Held = 0.0f;

    FirstPersonReloadInput Update(bool down, float dt);
};
