#pragma once
#include <memory>
#include <string>
#include <vector>

class World;
class AssetLibrary;
struct AnimatorParam;

// #175 Part B - Unity's Animator Controller: a state machine of animation clips.
//
// A `.controller` file (JSON) holds parameters, states and transitions. An entity's Animator
// Controller component points at one; in Play the controller picks the entity model's clip each
// frame and crossfades between states when a transition's conditions hold. Game code drives it
// through the component's parameters (SetFloat / SetBool / SetTrigger in Components.h).
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

    struct State {
        std::string Name;
        std::string Clip;  // a clip reference, as the Animation component uses (AnimationSystem.h)
        float Speed = 1.0f;
        bool Loop = true;
    };

    struct Transition {
        std::string From;  // a state name, or "Any" (from every state except To itself)
        std::string To;
        std::vector<Condition> Conditions; // all must hold
        bool HasExitTime = false;
        float ExitTime = 0.9f;  // normalized: 1 = the end of the From clip's first play
        float Duration = 0.25f; // crossfade, seconds
    };

    static constexpr const char* kAnyState = "Any";

    std::vector<Parameter> Parameters;
    std::vector<State> States;
    std::vector<Transition> Transitions;
    std::string DefaultState; // empty = the first state

    int FindState(const std::string& name) const;
    int DefaultStateIndex() const;

    // The transition to take from `state` this frame, or -1. `normalizedTime` is the time in
    // the state divided by its clip's length. Conditions read `params` (matched by name); the
    // triggers a firing transition tests are reset in `params`. Any-state transitions are
    // checked first, then the state's own, each in file order.
    int PickTransition(int state, float normalizedTime, std::vector<AnimatorParam>& params) const;

    // JSON round trip. Load returns false (and leaves `out` alone) on a missing/unparseable file.
    static bool LoadFile(const std::string& path, AnimatorController& out, std::string* error = nullptr);
    static bool FromJsonString(const std::string& text, AnimatorController& out, std::string* error = nullptr);
    std::string ToJsonString() const;
    bool SaveFile(const std::string& path) const;
};

// Loads (and caches, reloading when the file changes on disk) the controller at `path`, which
// may be project-relative. Null when it can't be read.
std::shared_ptr<const AnimatorController> GetAnimatorController(const std::string& path);

// Project-relative paths of every .controller file under the project, sorted.
std::vector<std::string> FindAnimatorControllers();

// Runs every Animator Controller component for one Play frame: starts the default state,
// evaluates transitions, and drives the entity's model clip. Entities with one skip the plain
// Animation component. `dt` is the game step (0 while paused).
void UpdateAnimatorControllers(World& world, AssetLibrary& assets, float dt);
