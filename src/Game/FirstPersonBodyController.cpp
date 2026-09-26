// GENERATED from project/animations/fps_body_locomotion.controller by scratchpad/gen_body.py, then kept by hand:
// the unit test TestFirstPersonBodyController compares the result with that file, so edit both together.
#include "FirstPersonBodyContract.h"
#include "AnimatorController.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace FPBody {
namespace {

using AC = AnimatorController;

struct Child { const char* Role; float X, Y; };
struct StateDef {
    const char* Name;
    bool Loop;
    float Speed;
    float PosX, PosY;
    std::vector<const char*> Tags;
    const char* Clip;                 // a single clip's role, or null for a blend tree
    const char* ParamX;               // blend tree parameters
    const char* ParamY;               // null = 1D
    std::vector<Child> Children;
};
struct TransitionDef {
    const char* From;
    const char* To;
    float Duration;
    bool HasExitTime;
    float ExitTime;
    float Offset;
    std::vector<AC::Condition> Conditions;
};

const std::vector<StateDef>& StateDefs() {
    static const std::vector<StateDef> s = {
        {"Locomotion", true, 1.0f, 0.0f, 0.0f, {"Locomotion"}, nullptr, "MoveX", "MoveY", {{"Stand_Idle_01", 0.0f, 0.0f}, {"Loco_Walk_Fwd", 0.0f, 1.53f}, {"Loco_Walk_Fwd_Left", -1.082f, 1.082f}, {"Loco_Walk_Fwd_Right", 1.082f, 1.082f}, {"Loco_Walk_Left", -1.346f, 0.0f}, {"Loco_Walk_Right", 1.586f, 0.0f}, {"Loco_Walk_Bwd", 0.0f, -1.195f}, {"Loco_Walk_Bwd_Left", -0.845f, -0.845f}, {"Loco_Walk_Bwd_Right", 0.845f, -0.845f}, {"Loco_Jog_Fwd", 0.0f, 3.264f}, {"Loco_Jog_Fwd_Left", -2.308f, 2.308f}, {"Loco_Jog_Fwd_Right", 2.308f, 2.308f}, {"Loco_Jog_Left", -2.3f, 0.0f}, {"Loco_Jog_Right", 2.945f, 0.0f}, {"Loco_Jog_Bwd", 0.0f, -2.261f}, {"Loco_Jog_Bwd_Left", -1.599f, -1.599f}, {"Loco_Jog_Bwd_Right", 1.599f, -1.599f}, {"Loco_Run_Fwd", 0.0f, 4.736f}}},
        {"Jump", false, 1.0f, 260.0f, -140.0f, {"Airborne"}, "Jump", nullptr, nullptr, {}},
        {"Fall", true, 1.0f, 520.0f, -140.0f, {"Airborne"}, "Jump_Fall_Loop", nullptr, nullptr, {}},
        {"Land", false, 1.4f, 260.0f, 140.0f, {}, "Jump_Land_Recovery", nullptr, nullptr, {}},
        {"Turn", false, 1.0f, 0.0f, 140.0f, {"Turn"}, nullptr, "TurnAngle", nullptr, {{"Stand_Idle_Turn_R180", -180.0f, 0.0f}, {"Stand_Idle_Turn_R135", -135.0f, 0.0f}, {"Stand_Idle_Turn_R090", -90.0f, 0.0f}, {"Stand_Idle_Turn_R045", -45.0f, 0.0f}, {"Stand_Idle_Turn_L045", 45.0f, 0.0f}, {"Stand_Idle_Turn_L090", 90.0f, 0.0f}, {"Stand_Idle_Turn_L135", 135.0f, 0.0f}, {"Stand_Idle_Turn_L180", 180.0f, 0.0f}}},
        {"Start", false, 1.0f, 260.0f, 140.0f, {"Start"}, nullptr, "StartX", "StartY", {{"Loco_Jog_Fwd_Start", 0.0f, 1.0f}, {"Loco_Jog_Bwd_Start", 0.0f, -1.0f}, {"Loco_Jog_Left_Start", -1.0f, 0.0f}, {"Loco_Jog_Right_Start", 1.0f, 0.0f}}},
        {"Stop", false, 1.0f, 520.0f, 140.0f, {"Stop"}, nullptr, "StopX", "StopY", {{"Loco_Jog_Fwd_Stop", 0.0f, 1.0f}, {"Loco_Jog_Bwd_Stop", 0.0f, -1.0f}, {"Loco_Jog_Left_Stop", -1.0f, 0.0f}, {"Loco_Jog_Right_Stop", 1.0f, 0.0f}}},
        {"StopRun", false, 1.0f, 520.0f, 260.0f, {"Stop"}, "Loco_Run_Fwd_Stop", nullptr, nullptr, {}},
        {"CrouchLoco", true, 1.5f, 0.0f, 400.0f, {"Crouch"}, nullptr, "MoveX", "MoveY", {{"Crouch_Idle_01", 0.0f, 0.0f}, {"Crouch_Loco_Walk_Fwd", 0.0f, 1.355f}, {"Crouch_Loco_Walk_Fwd_Left", -0.957f, 0.957f}, {"Crouch_Loco_Walk_Fwd_Right", 0.957f, 0.957f}, {"Crouch_Loco_Walk_Left", -1.155f, 0.0f}, {"Crouch_Loco_Walk_Right", 1.133f, 0.0f}, {"Crouch_Loco_Walk_Bwd", 0.0f, -1.123f}, {"Crouch_Loco_Walk_Bwd_Left", -0.794f, -0.794f}, {"Crouch_Loco_Walk_Bwd_Right", 0.794f, -0.794f}}},
        {"CrouchTurn", false, 1.0f, 260.0f, 400.0f, {"Crouch", "Turn"}, nullptr, "TurnAngle", nullptr, {{"Crouch_Idle_Turn_R180", -180.0f, 0.0f}, {"Crouch_Idle_Turn_R135", -135.0f, 0.0f}, {"Crouch_Idle_Turn_R090", -90.0f, 0.0f}, {"Crouch_Idle_Turn_R045", -45.0f, 0.0f}, {"Crouch_Idle_Turn_L045", 45.0f, 0.0f}, {"Crouch_Idle_Turn_L090", 90.0f, 0.0f}, {"Crouch_Idle_Turn_L135", 135.0f, 0.0f}, {"Crouch_Idle_Turn_L180", 180.0f, 0.0f}}},
        {"CrouchDown", false, 1.5f, 520.0f, 400.0f, {"Crouch"}, "Stand_Idle_Trans_Crouch_02", nullptr, nullptr, {}},
        {"CrouchUp", false, 1.5f, 780.0f, 400.0f, {"Crouch"}, "Crouch_Idle_Trans_Stand_01", nullptr, nullptr, {}},
        {"CrouchStart", false, 1.5f, 260.0f, 540.0f, {"Crouch", "Start"}, nullptr, "StartX", "StartY", {{"Crouch_Loco_Walk_Fwd_Start", 0.0f, 1.0f}, {"Crouch_Loco_Walk_Bwd_Start", 0.0f, -1.0f}, {"Crouch_Loco_Walk_Left_Start", -1.0f, 0.0f}, {"Crouch_Loco_Walk_Right_Start", 1.0f, 0.0f}}},
        {"CrouchStop", false, 1.5f, 520.0f, 540.0f, {"Crouch", "Stop"}, nullptr, "StopX", "StopY", {{"Crouch_Loco_Walk_Fwd_Stop", 0.0f, 1.0f}, {"Crouch_Loco_Walk_Bwd_Stop", 0.0f, -1.0f}, {"Crouch_Loco_Walk_Left_Stop", -1.0f, 0.0f}, {"Crouch_Loco_Walk_Right_Stop", 1.0f, 0.0f}}},
    };
    return s;
}

const std::vector<TransitionDef>& TransitionDefs() {
    static const std::vector<TransitionDef> t = {
        {"Locomotion", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"Locomotion", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
        {"Jump", "Fall", 0.2f, true, 0.85f, 0.0f, {}},
        {"Jump", "Land", 0.1f, true, 0.3f, 0.0f, {{"Grounded", AC::Op::If, 0.0f}}},
        {"Fall", "Land", 0.1f, false, 1.0f, 0.0f, {{"Grounded", AC::Op::If, 0.0f}}},
        {"Land", "Locomotion", 0.2f, true, 0.15f, 0.0f, {{"Speed", AC::Op::Greater, 0.5f}}},
        {"Land", "Locomotion", 0.3f, true, 0.5f, 0.0f, {}},
        {"Locomotion", "Turn", 0.15f, false, 1.0f, 0.0f, {{"Turning", AC::Op::If, 0.0f}}},
        {"Turn", "Locomotion", 0.25f, false, 1.0f, 0.0f, {{"Turning", AC::Op::IfNot, 0.0f}}},
        {"Locomotion", "Start", 0.1f, false, 1.0f, 0.0f, {{"Start", AC::Op::If, 0.0f}}},
        {"Locomotion", "Stop", 0.15f, false, 1.0f, 0.36f, {{"Stop", AC::Op::If, 0.0f}}},
        {"Locomotion", "StopRun", 0.15f, false, 1.0f, 0.42f, {{"StopRun", AC::Op::If, 0.0f}}},
        {"Start", "Locomotion", 0.15f, false, 1.0f, 0.0f, {{"Moving", AC::Op::IfNot, 0.0f}}},
        {"Start", "Locomotion", 0.2f, true, 0.45f, 0.0f, {}},
        {"Start", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"Start", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
        {"Stop", "Locomotion", 0.15f, false, 1.0f, 0.0f, {{"Moving", AC::Op::If, 0.0f}}},
        {"Stop", "Locomotion", 0.25f, true, 0.88f, 0.0f, {}},
        {"Stop", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"Stop", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
        {"StopRun", "Locomotion", 0.15f, false, 1.0f, 0.0f, {{"Moving", AC::Op::If, 0.0f}}},
        {"StopRun", "Locomotion", 0.25f, true, 0.88f, 0.0f, {}},
        {"StopRun", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"StopRun", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
        {"Locomotion", "CrouchDown", 0.2f, false, 1.0f, 0.3f, {{"CrouchDown", AC::Op::If, 0.0f}}},
        {"CrouchDown", "Locomotion", 0.2f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::IfNot, 0.0f}}},
        {"Turn", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"CrouchLoco", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"CrouchTurn", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"CrouchStart", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"CrouchStop", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"CrouchDown", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"CrouchUp", "Jump", 0.08f, false, 1.0f, 0.15f, {{"Jump", AC::Op::If, 0.0f}}},
        {"Turn", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
        {"CrouchDown", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
        {"CrouchUp", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
        {"CrouchDown", "CrouchLoco", 0.2f, false, 1.0f, 0.0f, {{"Moving", AC::Op::If, 0.0f}}},
        {"CrouchDown", "CrouchLoco", 0.25f, true, 0.92f, 0.0f, {}},
        {"CrouchLoco", "CrouchUp", 0.2f, false, 1.0f, 0.0f, {{"CrouchUp", AC::Op::If, 0.0f}}},
        {"CrouchUp", "CrouchLoco", 0.2f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::If, 0.0f}}},
        {"CrouchUp", "Locomotion", 0.2f, false, 1.0f, 0.0f, {{"Moving", AC::Op::If, 0.0f}}},
        {"CrouchUp", "Locomotion", 0.3f, true, 0.62f, 0.0f, {}},
        {"CrouchLoco", "CrouchStart", 0.1f, false, 1.0f, 0.0f, {{"Start", AC::Op::If, 0.0f}}},
        {"CrouchLoco", "CrouchStop", 0.15f, false, 1.0f, 0.75f, {{"Stop", AC::Op::If, 0.0f}}},
        {"CrouchStart", "Locomotion", 0.2f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::IfNot, 0.0f}}},
        {"CrouchStart", "CrouchLoco", 0.15f, false, 1.0f, 0.0f, {{"Moving", AC::Op::IfNot, 0.0f}}},
        {"CrouchStart", "CrouchLoco", 0.2f, true, 0.42f, 0.0f, {}},
        {"CrouchStart", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
        {"CrouchStop", "Locomotion", 0.2f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::IfNot, 0.0f}}},
        {"CrouchStop", "CrouchLoco", 0.15f, false, 1.0f, 0.0f, {{"Moving", AC::Op::If, 0.0f}}},
        {"CrouchStop", "CrouchLoco", 0.2f, true, 0.92f, 0.0f, {}},
        {"CrouchStop", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
        {"Locomotion", "CrouchLoco", 0.25f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::If, 0.0f}}},
        {"Turn", "CrouchLoco", 0.25f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::If, 0.0f}}},
        {"Start", "CrouchLoco", 0.25f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::If, 0.0f}}},
        {"Stop", "CrouchLoco", 0.25f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::If, 0.0f}}},
        {"StopRun", "CrouchLoco", 0.25f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::If, 0.0f}}},
        {"CrouchLoco", "Locomotion", 0.25f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::IfNot, 0.0f}}},
        {"CrouchLoco", "CrouchTurn", 0.15f, false, 1.0f, 0.0f, {{"Turning", AC::Op::If, 0.0f}}},
        {"CrouchTurn", "CrouchLoco", 0.25f, false, 1.0f, 0.0f, {{"Turning", AC::Op::IfNot, 0.0f}}},
        {"CrouchTurn", "Locomotion", 0.25f, false, 1.0f, 0.0f, {{"Crouched", AC::Op::IfNot, 0.0f}}},
        {"CrouchLoco", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
        {"CrouchTurn", "Fall", 0.25f, false, 1.0f, 0.0f, {{"Airborne", AC::Op::If, 0.0f}}},
    };
    return t;
}

} // namespace

AnimatorController BuildLocomotionController(const std::function<std::string(const std::string&)>& clipForRole) {
    AC c;
    c.Tracks = {"main"};
    c.Parameters = {
        {"MoveX", AC::ParamType::Float, 0.0f},
        {"MoveY", AC::ParamType::Float, 0.0f},
        {"Speed", AC::ParamType::Float, 0.0f},
        {"Sprint", AC::ParamType::Bool, 0.0f},
        {"Grounded", AC::ParamType::Bool, 1.0f},
        {"Airborne", AC::ParamType::Bool, 0.0f},
        {"Jump", AC::ParamType::Trigger, 0.0f},
        {"Turning", AC::ParamType::Bool, 0.0f},
        {"TurnAngle", AC::ParamType::Float, 0.0f},
        {"Moving", AC::ParamType::Bool, 0.0f},
        {"Start", AC::ParamType::Trigger, 0.0f},
        {"Stop", AC::ParamType::Trigger, 0.0f},
        {"StopRun", AC::ParamType::Trigger, 0.0f},
        {"StartX", AC::ParamType::Float, 0.0f},
        {"StartY", AC::ParamType::Float, 1.0f},
        {"StopX", AC::ParamType::Float, 0.0f},
        {"StopY", AC::ParamType::Float, 1.0f},
        {"Crouched", AC::ParamType::Bool, 0.0f},
        {"CrouchDown", AC::ParamType::Trigger, 0.0f},
        {"CrouchUp", AC::ParamType::Trigger, 0.0f},
    };
    AC::Layer& L = c.Layers[0];
    L.Name = "Base Layer";
    L.DefaultState = "Locomotion";
    L.EntryPosition = {-260.0f, 0.0f};
    L.AnyPosition = {-260.0f, -140.0f};
    L.ExitPosition = {760.0f, 0.0f};
    auto clip = [&](const char* role) { return clipForRole ? clipForRole(role) : std::string(); };
    for (const StateDef& d : StateDefs()) {
        AC::State s;
        s.Name = d.Name;
        s.Loop = d.Loop;
        s.Speed = d.Speed;
        s.Position = {d.PosX, d.PosY};
        for (const char* t : d.Tags) s.Tags.push_back(t);
        s.Motions.resize(1);
        AC::Motion& m = s.Motions[0];
        if (d.Clip) {
            m.Clip = clip(d.Clip);
        } else {
            m.BlendParam = d.ParamX;
            if (d.ParamY) m.BlendParamY = d.ParamY;
            for (const Child& ch : d.Children) m.Children.push_back({clip(ch.Role), ch.X, 1.0f, ch.Y});
        }
        L.States.push_back(std::move(s));
    }
    for (const TransitionDef& d : TransitionDefs()) {
        AC::Transition t;
        t.From = d.From;
        t.To = d.To;
        t.Duration = d.Duration;
        t.HasExitTime = d.HasExitTime;
        t.ExitTime = d.ExitTime;
        t.Offset = d.Offset;
        t.Conditions = d.Conditions;
        L.Transitions.push_back(std::move(t));
    }
    return c;
}

std::vector<std::string> LocomotionRoles() {
    std::vector<std::string> out;
    auto add = [&](const char* r) {
        for (const auto& x : out) if (x == r) return;
        out.push_back(r);
    };
    for (const StateDef& d : StateDefs()) {
        if (d.Clip) add(d.Clip);
        for (const Child& ch : d.Children) add(ch.Role);
    }
    return out;
}

std::string PickLocomotionClip(const std::string& role, const std::vector<std::string>& files) {
    auto lower = [](std::string x) {
        for (char& ch : x) ch = (char)std::tolower((unsigned char)ch);
        return x;
    };
    const std::string want = lower(role);
    std::string best;
    for (const std::string& f : files) {
        const size_t slash = f.find_last_of("/\\");
        std::string stem = lower(f.substr(slash == std::string::npos ? 0 : slash + 1));
        if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
        if (stem.rfind("am_", 0) == 0) stem.erase(0, 3);
        if (stem == want) return f; // the exact name wins
    }
    (void)best;
    return {};
}

} // namespace FPBody
