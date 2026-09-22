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

// Fire/Inspect/MagCheck are freely interruptible one-shots; TacReload/EmptyReload/Melee are
// committed once started; Draw/Holster can't be interrupted at all; IdleToSprint/SprintToIdle
// are the lowest tier so any real action pre-empts a locomotion transition. Anything else
// (Idle/Walk/Sprint/Aim, or an unrecognized name) returns None.
FirstPersonActionTier FirstPersonTierOf(const std::string& state);

// True when a request for `state` at `requestedTier` may take over from whatever is currently
// holding (`current` empty / currentTier None = nothing busy).
bool FirstPersonCanInterrupt(const std::string& state, FirstPersonActionTier requestedTier,
                             const std::string& current, FirstPersonActionTier currentTier);

// Idle / Walk / Sprint / Aim from movement + input alone. The caller only consults this once
// nothing is busy holding the current pose. Aim only applies at rest - the source set has no
// aim-while-moving clip, so movement always wins over the aim pose.
std::string FirstPersonRestingState(float planarSpeed, bool sprinting, bool aiming);

// The transition state to play on the way from `from` to `to` ("" = crossfade directly into
// `to` via its own Fade). The source set only authored a transition pair for Idle<->Sprint;
// every other resting-state change has no transition clip.
std::string FirstPersonTransitionVia(const std::string& from, const std::string& to);
