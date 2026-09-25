#pragma once
#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class World;
class AssetLibrary;
class Model;
struct AnimatorParam;
struct AnimatorControllerComponent;

// #175 Part B / Animator v2 - Unity's Animator Controller: layered state machines of clips.
//
// A `.controller` file (JSON) holds parameters, tracks and layers. Each layer is a state machine:
// states play a clip or a 1D blend tree (one motion per track), transitions crossfade between
// them when their conditions hold. An entity's Animator Controller component points at one; in
// Play the runtime samples every layer, blends them (override or additive, through an optional
// bone mask) and poses the entity's model. Game code drives it through the component's
// parameters (SetFloat / SetBool / SetTrigger in Components.h) and reads back the current
// state, its tags and the events it fired.
//
// Format v1 (flat states/transitions, one clip per state) still loads, as a single base layer.
struct AnimatorController {
    enum class ParamType { Float = 0, Int = 1, Bool = 2, Trigger = 3 };
    struct Parameter {
        std::string Name;
        ParamType Type = ParamType::Float;
        float Default = 0.0f; // Bool/Trigger: 0 or 1
    };

    // Greater/Less/Equals/NotEqual compare a Float/Int parameter against Threshold.
    // If/IfNot test a Bool (or Trigger: If only) parameter.
    enum class Op { Greater = 0, Less = 1, Equals = 2, NotEqual = 3, If = 4, IfNot = 5 };
    struct Condition {
        std::string Param;
        Op Mode = Op::Greater;
        float Threshold = 0.0f;
    };

    // What a state plays on one track: a clip, or a blend tree of clips - 1D along one parameter,
    // or 2D (freeform cartesian) over two, e.g. a directional walk/jog blend on MoveX / MoveY.
    // An empty motion is legal: on the base layer the track falls back to its bind pose (a weapon
    // with no clip for that state), on a higher layer it contributes nothing.
    struct BlendChild {
        std::string Clip;
        float Threshold = 0.0f;           // 1D: where on BlendParam; 2D: the X position
        float Speed = 1.0f;
        float ThresholdY = 0.0f;          // 2D only: the Y position (on BlendParamY)
    };
    struct Motion {
        std::string Clip;                 // a clip reference (AnimationSystem.h), used when no children
        std::string BlendParam;           // blend tree: the Float parameter it blends along (2D: X)
        std::string BlendParamY;          // 2D blend tree: the Y parameter; empty = a 1D tree
        std::vector<BlendChild> Children; // blend tree: sorted by Threshold when evaluated
        bool IsBlendTree() const { return !Children.empty(); }
        bool Is2D() const { return !BlendParamY.empty(); }
        bool Empty() const { return Clip.empty() && Children.empty(); }
    };

    // A named marker at a normalized time in a state (0 = entry, 1 = the end of one pass). The
    // runtime reports it through AnimatorControllerComponent::FiredEvents the frame it's crossed.
    struct Event {
        std::string Name;
        float Time = 1.0f;
    };

    struct State {
        std::string Name;
        std::vector<Motion> Motions; // one per track (missing entries = empty)
        float Speed = 1.0f;
        std::string SpeedParam;      // optional Float parameter multiplying Speed
        bool Loop = true;
        // With root motion on (the component's Root Motion), this state's travel moves the object.
        // False keeps it in the pose instead - e.g. a clip whose drift you want left as authored.
        bool RootMotion = true;
        int Priority = 0;            // see Transition::RespectPriority
        std::vector<std::string> Tags;
        std::vector<Event> Events;
        glm::vec2 Position{0.0f};    // Animator window graph position

        const Motion& MotionFor(int track) const;
        bool HasTag(const std::string& tag) const;
    };

    enum class Source { State = 0, Any = 1, Entry = 2 };
    struct Transition {
        Source FromKind = Source::State;
        std::string From;  // the source state when FromKind == State
        std::string To;    // a state name, or kExitState (go back through the layer's Entry)
        std::vector<Condition> Conditions; // all must hold
        bool HasExitTime = false;
        float ExitTime = 0.9f;   // normalized: 1 = the end of the From state's first pass
        float Duration = 0.25f;  // crossfade, seconds
        float Offset = 0.0f;     // normalized start time in the destination
        // False: once this transition starts, nothing interrupts its crossfade.
        bool Interruptible = true;
        // Any State only: fire only into a state of strictly higher Priority than the current one,
        // so e.g. a reload (priority 3) can't be cut short by firing (priority 2).
        bool RespectPriority = false;
        // Any State only: may re-enter the state that is already playing (restarting it).
        bool CanTransitionToSelf = false;
    };

    enum class Blending { Override = 0, Additive = 1 };
    struct Layer {
        std::string Name = "Base Layer";
        float Weight = 1.0f;           // the base layer always plays at 1
        Blending Mode = Blending::Override;
        // Bone mask: a node is in the layer when it or an ancestor is listed in MaskInclude (an
        // empty list includes the whole rig) and no nearer ancestor/itself is in MaskExclude.
        std::vector<std::string> MaskInclude, MaskExclude;
        std::string DefaultState;      // empty = the first state
        std::vector<State> States;
        std::vector<Transition> Transitions;
        glm::vec2 EntryPosition{-260.0f, 0.0f}, AnyPosition{-260.0f, -120.0f}, ExitPosition{520.0f, 0.0f};

        int FindState(const std::string& name) const;
        int DefaultStateIndex() const;
    };

    static constexpr const char* kAnyState = "Any";   // v1 files: "from": "Any"
    static constexpr const char* kEntryState = "Entry";
    static constexpr const char* kExitState = "Exit";
    static bool IsReservedName(const std::string& name);

    std::vector<Parameter> Parameters;
    std::vector<std::string> Tracks{"main"};
    std::vector<Layer> Layers{Layer{}};

    int TrackIndex(const std::string& name) const; // empty/unknown = 0
    const Parameter* FindParameter(const std::string& name) const;

    // The transition to take in `layer` from `state` this frame, or -1. `normalizedTime` is the
    // time in the state as a fraction of one pass. Conditions read `params` (matched by name); the
    // triggers a firing transition tests are reset in `params`. Any-state transitions are checked
    // first, then the state's own, each in file order.
    int PickTransition(int layer, int state, float normalizedTime, std::vector<AnimatorParam>& params) const;
    // The state an Entry (layer start, or a transition to Exit) resolves to: the first Entry
    // transition whose conditions hold (consuming its triggers), else the default state.
    int PickEntry(int layer, std::vector<AnimatorParam>& params) const;

    // JSON round trip. Load returns false (and leaves `out` alone) on a missing/unparseable file.
    static bool LoadFile(const std::string& path, AnimatorController& out, std::string* error = nullptr);
    static bool FromJsonString(const std::string& text, AnimatorController& out, std::string* error = nullptr);
    std::string ToJsonString() const;
    bool SaveFile(const std::string& path) const;
};

// Weights of a 1D blend tree's children for `value`: at most two non-zero, summing to 1.
// `children` need not be sorted. Exposed for tests and the Animator window preview.
// How much a crossfading state shows at fade progress `fade` (0..1, linear in time): eased in
// and out, so a pose settles into the next instead of sliding over at a constant rate and
// stopping dead. Anything weighting by a crossfade (e.g. the first-person ADS correction)
// should use the same curve.
inline float AnimatorCrossfadeWeight(float fade) {
    fade = fade < 0.0f ? 0.0f : (fade > 1.0f ? 1.0f : fade);
    return fade * fade * (3.0f - 2.0f * fade);
}

std::vector<float> AnimatorBlendWeights(const std::vector<AnimatorController::BlendChild>& children, float value);
// Weights of a 2D (freeform cartesian) blend tree's children at (x, y), each child sitting at
// (Threshold, ThresholdY): gradient-band interpolation, as Unity's Freeform Cartesian. A child's
// weight is 1 on its own position and fades out toward every other child; they sum to 1.
std::vector<float> AnimatorBlendWeights2D(const std::vector<AnimatorController::BlendChild>& children, float x, float y);
// The weights of `m`'s children for the current parameter values (1D or 2D).
std::vector<float> AnimatorMotionWeights(const AnimatorController::Motion& m, const std::vector<AnimatorParam>& params);

// How much each entry of a crossfade stack shows in the final pose, given each entry's Fade
// (bottom first): the entries blend in order, each over everything below it, eased by
// AnimatorCrossfadeWeight. Sums to 1. Root motion mixes the entries' travel by these.
std::vector<float> AnimatorStackWeights(const std::vector<float>& fades);

// Per node of a rig described by `parent` (parents first) and `name`: 1 when the node is inside
// `layer`'s bone mask, else 0.
std::vector<float> AnimatorMaskWeights(const AnimatorController::Layer& layer,
                                       const std::vector<std::string>& names, const std::vector<int>& parents);

// Loads (and caches, reloading when the file changes on disk) the controller at `path`, which
// may be project-relative. Null when it can't be read.
std::shared_ptr<const AnimatorController> GetAnimatorController(const std::string& path);

// Makes an in-memory controller reachable through GetAnimatorController under `key` (use a
// "memory:" prefix so it can never collide with a file) - e.g. one generated from a v1 weapon
// definition. Registering the same key again replaces it.
void RegisterAnimatorController(const std::string& key, std::shared_ptr<const AnimatorController> ctrl);

// Project-relative paths of every .controller file under the project, sorted.
std::vector<std::string> FindAnimatorControllers();

// Advances one component's state machines by `dt` without touching any model: transitions,
// crossfade stacks, phases, events and the base-layer state fields. `stateLength(layer, state)`
// returns a state's length in seconds for the current parameters (<= 0 counts as 1 s). Split
// out of UpdateAnimatorControllers so the logic is testable without a GPU or model.
void AdvanceAnimator(const AnimatorController& ctrl, AnimatorControllerComponent& ac, float dt,
                     const std::function<float(int, int)>& stateLength);

// Runs every Animator Controller component for one Play frame: advances the state machines,
// then samples and blends every layer and poses each entity's model. Followers (Driver set)
// mirror their driver. Entities with one skip the plain Animation component. `dt` is the game
// step (0 while paused).
void UpdateAnimatorControllers(World& world, AssetLibrary& assets, float dt);
