#pragma once
// Skeletal animation, animator controllers, root motion and IK rigs.
// Part of the component set; Components.h includes every part.

#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <glm/glm.hpp>
#include <entt/entt.hpp>
#include "Animation.h" // LocalTRS
#include "IK.h"
#include "SwayModifier.h"
#include <glm/gtc/quaternion.hpp>

class Model;

// Root motion (RootMotion.h): what a component does with the travel its clips author on the root
// bone. Off plays the clips as authored (a walk drags the mesh away from the object and snaps
// back each loop). Apply takes the travel out of the pose and moves the object by it. In Place
// takes it out of the pose too but leaves the object where it is: game code reads the motion
// (RootMotionDeltaPosition / Yaw) and moves the object itself, or a second rig follows one that
// applies it.
enum class RootMotionMode : int { Off = 0, Apply = 1, InPlace = 2 };

struct RootMotionOptions {
    int  Mode = 0;              // RootMotionMode
    std::string Bone;           // the root bone; empty = auto ("root", else the hips / pelvis)
    bool Rotation = true;       // turns in the clip turn the object (off: they stay in the pose)
    bool Vertical = false;      // height changes move the object (off: jumps, crouches stay in the pose)

    // --- runtime (not serialized): the last update's motion ---
    glm::vec3 DeltaPosition{0.0f}; // world-space metres (the parent's space when parented)
    float DeltaYaw = 0.0f;         // degrees about +Y
    float Speed = 0.0f;            // horizontal m/s, for readouts and game code
    float TurnRate = 0.0f;         // degrees / s
    int ResolvedBone = -1;         // node index on the model, -1 = not found
};

// #175 / #113 — Unity's (legacy) Animation component: plays one of the entity's model clips in
// Play mode. Serialized fields are the authored setup; game code drives it at runtime by
// changing Clip (crossfades to it) or IsPlaying. SkeletalAnimationSystem applies it each frame.
struct SkeletalAnimationComponent {
    std::string Clip;                // clip name; empty = the model's first clip
    bool PlayAutomatically = true;   // start when Play begins
    int  WrapMode = 1;               // AnimationWrapMode: 0 Once, 1 Loop, 2 PingPong, 3 ClampForever
    float Speed = 1.0f;              // playback rate; negative plays backwards
    float CrossFade = 0.25f;         // seconds to blend when the clip changes
    RootMotionOptions RootMotion;

    // --- runtime (not serialized) ---
    bool Started = false;            // PlayAutomatically has been applied this run
    bool IsPlaying = false;          // set by the system at start; game code may set it
    std::string PlayingClip;         // what the system last asked the model for
};

// #175 Part B - one Animator Controller parameter's live value (Type matches
// AnimatorController::ParamType: 0 Float, 1 Int, 2 Bool, 3 Trigger; Bool/Trigger are 0 or 1).
struct AnimatorParam {
    std::string Name;
    int Type = 0;
    float Value = 0.0f;
};

// One layer's live playback (Animator v2). Stack[0] is the oldest pose still contributing;
// the last entry is the current state. Every entry above the first fades in over the one below
// it, so interrupting a crossfade keeps the pose it had reached instead of popping.
struct AnimatorLayerRuntime {
    struct Item {
        int   State = -1;
        float Phase = 0.0f;        // normalized time in the state: 1 = one pass, >1 counts loops
        float Fade = 1.0f;         // 0..1 blend-in weight over the entries below
        float FadeDuration = 0.0f; // seconds for Fade 0 -> 1 (0 = instant)
        float PrevPhase = -1.0f;   // Phase before the last advance (root motion); -1 = just entered
        bool Matched = false;      // distance matching has placed it at least once
        int   Via = -1;            // the transition that brought it in (its TrackDurations), or -1
        float Age = 0.0f;          // seconds since it came in
        float Hold = 0.0f;         // seconds it stays in the stack for its slowest track (TrackDurations)
        float TrackScale = 1.0f;   // the transition's TrackDurations scaled as its Duration was (TransitionDuration:*)
    };
    std::vector<Item> Stack;
    int  Transition = -1;          // the transition whose crossfade is running, or -1
    bool Interruptible = true;     // false while a non-interruptible transition fades
};

// One transition the controller took, for the Animator window's History tab.
struct AnimatorTransitionLog {
    float Time = 0.0f;       // seconds since the controller started
    int   Layer = 0;
    int   Transition = -1;   // index in the layer's transition list
    std::string From, To;    // state names
    std::string Why;         // the transition's conditions with the parameter values at that moment
};

// #175 Part B - drives this entity's model from an Animator Controller asset (.controller):
// clips (or 1D blend trees) as states on one or more layers, crossfaded transitions on parameter
// conditions. While playing it takes over from an Animation component on the same entity. Game
// code steers it with the setters below and reads back state, tags and fired events.
struct AnimatorControllerComponent {
    std::string Controller;  // project-relative .controller path
    float Speed = 1.0f;      // multiplies every state's speed
    // Which of the controller's tracks this entity plays (empty = the first). A controller can
    // hold several clip sets per state - e.g. "arms" and "weapon" - for rigs animated together.
    std::string Track;
    RootMotionOptions RootMotion; // from the base layer; a state can opt out (State::RootMotion)

    void  SetFloat(const std::string& name, float v)  { Param(name, 0).Value = v; }
    void  SetInt(const std::string& name, int v)      { Param(name, 1).Value = (float)v; }
    void  SetBool(const std::string& name, bool v)    { Param(name, 2).Value = v ? 1.0f : 0.0f; }
    void  SetTrigger(const std::string& name)         { Param(name, 3).Value = 1.0f; }
    void  ResetTrigger(const std::string& name)       { Param(name, 3).Value = 0.0f; }
    float GetFloat(const std::string& name) const {
        for (const auto& p : Params) if (p.Name == name) return p.Value;
        return 0.0f;
    }
    const std::string& CurrentState() const { return StateName; }
    // Base-layer queries, valid after the controller's first update.
    bool InState(const std::string& name) const { return StateName == name; }
    bool HasTag(const std::string& tag) const {
        for (const auto& t : StateTags) if (t == tag) return true;
        return false;
    }
    // Events whose time was crossed during the last update (cleared at the start of each one).
    bool EventFired(const std::string& name) const {
        for (const auto& e : FiredEvents) if (e == name) return true;
        return false;
    }

    // --- runtime (not serialized) ---
    std::vector<AnimatorParam> Params; // seeded from the controller's defaults at start
    bool  Started = false;
    int   State = -1;                  // base layer: current state index
    std::string StateName;             // base layer: current state name
    std::vector<std::string> StateTags;// base layer: current state's tags
    float StateTime = 0.0f;            // base layer: normalized time in the current state
    bool  InTransition = false;        // base layer: a crossfade is still running
    std::vector<std::string> FiredEvents;
    std::vector<AnimatorLayerRuntime> Layers;
    std::vector<AnimatorTransitionLog> History; // newest last, capped
    float Clock = 0.0f;                         // seconds since the controller started
    // Follower mode: when set, this entity mirrors the driver's layers/states/times exactly and
    // evaluates no transitions of its own - the way a weapon rig stays locked to the arms.
    entt::entity Driver = entt::null;
    // Follower only (runtime, not saved): take the driver's finished local pose, matched by node
    // name, instead of sampling every clip again - for modular pieces cut from one skeleton (an
    // NPC's outfit). Ignored when either rig has an IK Rig.
    bool CopyDriverPose = false;
    // Runtime, not saved: skip this entity's update this frame (the enemy squad's animation LOD). The time still
    // counts - the next update advances by all of it - and the pose holds meanwhile.
    bool SkipUpdate = false;
    float SkippedTime = 0.0f;
    // Keep updating while the entity is inactive (hidden rigs that must still leave a state).
    bool UpdateWhenInactive = false;

    // A parameter by name, created (with `type`) if the controller hasn't declared it yet -
    // game code may set values before the first frame.
    AnimatorParam& Param(const std::string& name, int type) {
        for (auto& p : Params) if (p.Name == name) return p;
        Params.push_back({name, type, 0.0f});
        return Params.back();
    }
};

// One two-bone chain of an IK Rig (shoulder-elbow-hand, hip-knee-foot).
struct IKLimb {
    bool Enabled = false;
    std::string Upper, Lower, End; // node names on this entity's model
    std::string Target;            // node the end reaches for
    // Reach for where the end sat relative to Target in the ANIMATED pose, rather than for Target
    // itself: a hand keeps its authored grip while the gun it holds is moved procedurally.
    bool KeepAnimatedOffset = true;
    bool MatchRotation = true;     // also turn the end to the goal's rotation
    float Weight = 1.0f;
    // Elbow/knee hint (IK.h TwoBoneHint). HintWeight 0 keeps the animated bend plane, as before.
    std::string PoleBone;          // node whose position the elbow points toward (empty = the animated elbow)
    float HintWeight = 0.0f;       // 0..1
    glm::vec3 HintOffset{0.0f};    // model-space move of the pole point
    float MaxLimbScale = 1.0f;     // longest the limb may stretch toward an out-of-reach target (1 = never)
    float CurveWeight = 1.0f;      // runtime: clip weight curve scale on Weight (game code writes it; 1 = none)
    // The end's grip on the Target, moved in the Target bone's own frame (model units, degrees pitch/yaw/roll);
    // zero = the grip as animated. First-person weapon IK writes the hand-vs-gun offsets here.
    glm::vec3 GripPosition{0.0f};
    glm::vec3 GripRotation{0.0f};
    // Runtime: radians to swing the solved limb about its root->end line (the elbow's "door"),
    // after the solve - the end stays put. Game code writes it (first-person ADS actions do).
    float Swivel = 0.0f;
    // Runtime: a model-space move applied to the end's goal after it is found (identity = none).
    // First-person ADS writes it to keep a hand that has left the gun where the body is.
    glm::mat4 GoalMove{1.0f};
};

// A procedural rigid move of one bone and everything under it, in model space. Runtime only:
// game code writes these each frame (the first-person recoil/sway stack does).
struct IKBoneOffset {
    std::string Bone;
    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Pivot{0.0f};         // rotation centre, relative to the bone's own position
    // When set, the rotation centre is this bone's position (plus Pivot) in the same pose,
    // instead of the offset bone's own.
    std::string PivotBone;
};

// Post-process IK on an Animator Controller's output pose (IK.h): procedural bone offsets, up to
// two two-bone limbs and a look-at, run after the layers blend and before the pose is applied.
struct IKRigComponent {
    bool Enabled = true;
    float Weight = 1.0f;           // master blend, offsets included (0 = the animated pose)
    IKLimb LimbA;
    IKLimb LimbB;
    bool LookAtEnabled = false;
    std::string LookAtBone;
    std::string LookAtTarget;
    glm::vec3 LookAtAxis{0.0f, 0.0f, 1.0f}; // the bone's local axis that should face the target
    float LookAtMaxAngle = 60.0f;
    float LookAtWeight = 1.0f;
    float LookCurveWeight = 1.0f;   // runtime: clip weight curve scale on LookAtWeight (lane A; 1 = none)

    // --- runtime (not serialized) ---
    std::vector<IKBoneOffset> Offsets;
    // Runtime-only source sway job, inserted before the general procedural/ADS aim offset.
    bool SwayEnabled = false;
    SwayModifierSettings SwaySettings;
    SwayModifierPose SwayPose;
    int SwayBeforeOffset = 1;
    glm::quat SwayModelToComponent{1,0,0,0}, SwayModelToWorld{1,0,0,0};
    float SwayMetresPerUnit = 1;
    // Extra local rotations (pre-multiplied onto a bone's animated local rotation, scaled by
    // Weight), applied before everything else - e.g. twist bones the limbs don't solve.
    std::vector<std::pair<std::string, glm::quat>> LocalRotations;
    // A pose the bones are pulled toward first, each by its HoldWeights entry (node order; empty
    // = none) - e.g. the aim pose keeping the gun and right arm on the sights while a reload
    // plays on the left arm. A limb whose hand is held keeps the held pose's grip on its target;
    // one that isn't keeps its animated grip, so it plays its motion relative to the target.
    std::vector<LocalTRS> HoldPose;
    std::vector<float> HoldWeights;
    // Keep a contact action's authored hand-vs-target grip before upper layers add
    // locomotion. The finished target still moves; the hand and held object move with it.
    bool PreserveBaseGrip = false;
};

// Procedural runtime animation: spin, orbit, bob, and (for a LightComponent entity) hue
// cycling. Applied only while the scene is playing — edit mode always shows the authored
// pose. All parameters are authored/serialized; the Base* fields and Initialized are runtime
// scratch, captured from the entity's authored transform the first frame play runs and never
// written to the scene file, so Play -> Stop restores everything cleanly.
// Registered via ComponentRegistry (#184) rather than hand-coded serializer/Inspector code — the
// third such migration. Reflected fields are addressed by pointer-to-member
// (ComponentReflection.h); the runtime-scratch tail (Initialized, Base*, Elapsed) is left out of
// the reflected field list, same as it was left out of the old hand-written serializer. The flat
// reflected Inspector list loses the old section's "Spin"/"Orbit"/"Bob"/"Light Color Cycle"
// sub-headers, so field names below are qualified (e.g. "Orbit Speed", not "Speed") to stay
// unambiguous without them.
struct AnimatorComponent {
    glm::vec3 SpinDegPerSec{0.0f};   // continuous local rotation: angular velocity, degrees/second, about this vector's own direction

    glm::vec3 OrbitAxis{0.0f, 1.0f, 0.0f};
    float OrbitDegPerSec = 0.0f;     // revolve around Base position on this axis
    float OrbitRadius = 0.0f;

    float BobAmplitude = 0.0f;       // vertical sine offset from Base position, world units
    float BobFreqHz = 0.0f;

    float ColorCycleHzPerSec = 0.0f; // LightComponent hue revolutions/second (0 = leave color alone)

    // --- runtime scratch (not serialized) ---
    bool Initialized = false;
    glm::vec3 BasePosition{0.0f};
    glm::quat BaseRotation{1.0f, 0.0f, 0.0f, 0.0f}; // authored orientation, so spin is reversible like orbit/bob (#109)
    glm::vec3 BaseColor{1.0f};
    float Elapsed = 0.0f;
};
