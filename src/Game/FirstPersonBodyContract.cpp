#include "FirstPersonBodyContract.h"

#include "Components.h"

#include <algorithm>
#include <cctype>

namespace FPBody {

float LocomotionSpineWeight(const AnimatorControllerComponent& animator, float& crouch) {
    const auto ctrl = GetAnimatorController(animator.Controller);
    if (!ctrl || ctrl->Layers.empty() || animator.Layers.empty() || animator.Layers[0].Stack.empty()) return 1.0f;
    const auto& stack = animator.Layers[0].Stack;
    std::vector<float> fades;
    for (const auto& entry : stack) fades.push_back(entry.Fade);
    const auto weights = AnimatorStackWeights(fades);
    float gait = 0.0f, crouched = 0.0f;
    for (size_t s = 0; s < stack.size(); ++s) {
        const int state = stack[s].State;
        if (state < 0 || state >= (int)ctrl->Layers[0].States.size()) continue;
        const auto& st = ctrl->Layers[0].States[state];
        if (st.Name == kStateCrouchDown || st.Name == kStateCrouchUp ||
            st.Name == kStateJump || st.Name == kStateFall || st.Name == kStateLand) continue;
        if (st.Name != kStateLocomotion && st.Name != kStateCrouchLoco &&
            !st.HasTag("Locomotion") && !st.HasTag("Turn") && !st.HasTag(kTagStart) && !st.HasTag(kTagStop) &&
            !st.HasTag(kTagPivot) && !st.HasTag(kTagStep) && !st.HasTag(kTagFidget)) continue;
        gait += weights[s];
        if (st.Name == kStateCrouchLoco || st.HasTag("Crouch")) crouched += weights[s];
    }
    if (gait > 1e-5f) crouch = std::clamp(crouched / gait, 0.0f, 1.0f);
    // Preserve additive hit reactions instead of pinning their spine back to idle.
    for (size_t layer = 1; layer < animator.Layers.size() && layer < ctrl->Layers.size(); ++layer) {
        fades.clear();
        for (const auto& entry : animator.Layers[layer].Stack) fades.push_back(entry.Fade);
        const auto overlayWeights = AnimatorStackWeights(fades);
        float hit = 0.0f;
        for (size_t s = 0; s < animator.Layers[layer].Stack.size(); ++s) {
            const int state = animator.Layers[layer].Stack[s].State;
            if (state >= 0 && state < (int)ctrl->Layers[layer].States.size() && ctrl->Layers[layer].States[state].HasTag("Hit"))
                hit += overlayWeights[s];
        }
        gait *= 1.0f - std::clamp(hit, 0.0f, 1.0f);
    }
    return gait;
}

void BlendLocomotionSpine(IK::Pose& pose, const std::vector<int>& parents, const std::array<int, 5>& bones,
                         const std::vector<glm::mat4>& stand, const std::vector<glm::mat4>& crouch,
                         float crouchWeight, float stability, float crouchDrop) {
    stability = std::clamp(stability, 0.0f, 1.0f);
    if (stability <= 0.0f || pose.size() != parents.size() || stand.size() != pose.size()) return;
    crouchWeight = std::clamp(crouchWeight, 0.0f, 1.0f);
    const bool haveCrouch = crouch.size() == pose.size();
    std::vector<glm::mat4> globals;
    IK::ComputeGlobals(pose, parents, globals);
    std::vector<IK::GlobalTarget> targets;
    constexpr float retained[5] = {0.10f, 0.05f, 0.0f, 0.0f, 0.0f};
    for (size_t slot = 0; slot < bones.size(); ++slot) {
        const int i = bones[slot];
        if (i < 0 || i >= (int)pose.size()) continue;
        const auto& a = stand[i];
        const auto& b = haveCrouch ? crouch[i] : a;
        glm::vec3 neutral = glm::mix(IK::Position(a), IK::Position(b), crouchWeight);
        if (!haveCrouch) neutral.y -= crouchDrop * crouchWeight;
        const glm::quat rotation = glm::normalize(glm::slerp(IK::Rotation(a), IK::Rotation(b), crouchWeight));
        const float blend = stability * (1.0f - retained[slot]);
        // Original model-space transforms cancel inherited pelvis motion too. SetGlobals converts
        // through the corrected parents while leaving pelvis/leg local transforms untouched.
        targets.push_back({i, glm::mix(IK::Position(globals[i]), neutral, blend),
                           glm::normalize(glm::slerp(IK::Rotation(globals[i]), rotation, blend))});
    }
    std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return a.Node < b.Node; });
    IK::SetGlobals(pose, parents, globals, targets);
}

void RestoreAuthoredSpine(IK::Pose& pose, const std::vector<int>& bones, const std::vector<LocalTRS>& authored,
                          const std::vector<LocalTRS>& stabilized) {
    if (authored.size() != bones.size() || stabilized.size() != bones.size()) return;
    for (size_t j = 0; j < bones.size(); ++j) {
        const int i = bones[j];
        if (i < 0 || i >= (int)pose.size()) continue;
        LocalTRS& now = pose[i];
        // The later passes turn a bone about axes in its parent's frame (IK::OffsetBone: now = added * stabilized);
        // the same turn goes on the clips' bone.
        const glm::quat added = now.R * glm::inverse(stabilized[j].R);
        now.T = authored[j].T + (now.T - stabilized[j].T);
        now.R = glm::normalize(added * authored[j].R);
    }
}

std::map<std::string, std::string> ParseBoneMap(const std::string& text) {
    std::map<std::string, std::string> out;
    auto trim = [](std::string v) {
        const size_t a = v.find_first_not_of(" \t\r");
        const size_t b = v.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    };
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of(",\n", start);
        if (end == std::string::npos) end = text.size();
        const std::string entry = text.substr(start, end - start);
        if (const size_t eq = entry.find('='); eq != std::string::npos) {
            const std::string from = trim(entry.substr(0, eq)), to = trim(entry.substr(eq + 1));
            if (!from.empty() && !to.empty()) out[from] = to;
        }
        start = end + 1;
    }
    return out;
}

using PT = AnimatorController::ParamType;

const char* FeatureName(Feature f) {
    switch (f) {
        case Feature::Core: return "the body";
        case Feature::Turning: return "Turning (Turn Threshold)";
        case Feature::StartStop: return "Start / Stop clips";
        case Feature::Crouch: return "Crouch";
        case Feature::FootIK: return "Foot IK";
        case Feature::WeaponArms: return "Weapon Arms";
        case Feature::SpineAim: return "Spine Aim / Twist";
    }
    return "";
}

bool FeatureEnabled(Feature f, const FirstPersonBodyComponent& cfg) {
    switch (f) {
        case Feature::Core: return true;
        case Feature::Turning: return cfg.TurnThreshold > 0.0f;
        case Feature::StartStop: return cfg.StartStopClips;
        case Feature::Crouch: return cfg.CrouchHeight > 0.0f;
        case Feature::FootIK: return cfg.FootIK;
        case Feature::WeaponArms: return cfg.WeaponArms;
        case Feature::SpineAim: return cfg.SpineAim > 0.0f || cfg.SpineTwist > 0.0f;
    }
    return false;
}

const std::vector<ParamSpec>& Params() {
    static const std::vector<ParamSpec> t = {
        {kMoveX, PT::Float, Feature::Core, "The input sideways in the body's frame (m/s, smoothed): the locomotion blend tree's X."},
        {kMoveY, PT::Float, Feature::Core, "The input forward in the body's frame (m/s, smoothed): the blend tree's Y."},
        {kSpeed, PT::Float, Feature::Core, "How fast the body is asked to move (m/s)."},
        {kSprint, PT::Bool, Feature::Core, "True while sprinting."},
        {kGrounded, PT::Bool, Feature::Core, "True on the ground."},
        {kAirborne, PT::Bool, Feature::Core, "True once off the ground for 0.15 s: goes to Fall."},
        {kJump, PT::Trigger, Feature::Core, "Set the frame a jump starts."},
        {kTurning, PT::Bool, Feature::Turning, "True while the body turns on the spot."},
        {kTurnAngle, PT::Float, Feature::Turning, "How far (degrees, signed) the view is off the body's heading: the Turn blend tree's input."},
        {kMoving, PT::Bool, Feature::StartStop, "True while the input asks to move (ends a Start clip early on a tap)."},
        {kStart, PT::Trigger, Feature::StartStop, "Set when a push-off should play."},
        {kStop, PT::Trigger, Feature::StartStop, "Set when a braking step should play."},
        {kStopRun, PT::Trigger, Feature::StartStop, "Set when a braking step should play after a sprint."},
        {kStartX, PT::Float, Feature::StartStop, "The push-off direction (its blend tree's X)."},
        {kStartY, PT::Float, Feature::StartStop, "The push-off direction (its blend tree's Y)."},
        {kStopX, PT::Float, Feature::StartStop, "The braking direction (its blend tree's X)."},
        {kStopY, PT::Float, Feature::StartStop, "The braking direction (its blend tree's Y)."},
        {kCrouched, PT::Bool, Feature::Crouch, "True while crouched."},
        {kCrouchDown, PT::Trigger, Feature::Crouch, "Set when standing still and crouching."},
        {kCrouchUp, PT::Trigger, Feature::Crouch, "Set when crouching still and standing."},
    };
    return t;
}

const std::vector<StateSpec>& States() {
    static const std::vector<StateSpec> t = {
        {kStateLocomotion, Feature::Core, "The standing idle / walk / jog blend. Start and Stop clips are only started from it.", true},
        {kStateJump, Feature::Core, "The take-off clip (entered on the Jump trigger).", false},
        {kStateFall, Feature::Core, "Airborne loop (entered on Airborne).", false},
        {kStateLand, Feature::Core, "The landing clip. While it plays the input steers the capsule instead of the clip's travel.", false},
        {kStateTurn, Feature::Turning, "Turn on the spot: the body reads this state's yaw travel to turn.", true},
        {kStateStart, Feature::StartStop, "The push-off clip.", true},
        {kStateStop, Feature::StartStop, "The braking clip.", true},
        {kStateStopRun, Feature::StartStop, "The braking clip after a sprint.", false},
        {kStateCrouchLoco, Feature::Crouch, "The crouched idle / walk blend.", true},
        {kStateCrouchDown, Feature::Crouch, "Stand to crouch.", false},
        {kStateCrouchUp, Feature::Crouch, "Crouch to stand.", false},
        {kStateCrouchTurn, Feature::Crouch, "Turn on the spot while crouched (with Turning).", false},
    };
    return t;
}

const std::vector<BoneSpec>& Bones() {
    static const std::vector<BoneSpec> t = {
        {kBonePelvis, Feature::FootIK, "The hips drop to the lower foot."},
        {kBoneThigh[0], Feature::FootIK, "Leg IK chain (left)."},
        {kBoneCalf[0], Feature::FootIK, "Leg IK chain (left)."},
        {kBoneFoot[0], Feature::FootIK, "Leg IK chain (left); also the ground ray's origin."},
        {kBoneThigh[1], Feature::FootIK, "Leg IK chain (right)."},
        {kBoneCalf[1], Feature::FootIK, "Leg IK chain (right)."},
        {kBoneFoot[1], Feature::FootIK, "Leg IK chain (right); also the ground ray's origin."},
        {kBoneSpine[0], Feature::SpineAim, "The spine the view's pitch / twist tilts."},
        {kBoneSpine[1], Feature::SpineAim, "The spine the view's pitch / twist tilts."},
        {kBoneSpine[2], Feature::SpineAim, "The spine the view's pitch / twist tilts."},
        {kBoneSpine[3], Feature::SpineAim, "The spine the view's pitch / twist tilts."},
        {kBoneSpine[4], Feature::SpineAim, "The spine the view's pitch / twist tilts."},
        {kBoneClavicle[0], Feature::WeaponArms, "Shoulder shrug toward the gun (left)."},
        {kBoneUpperArm[0], Feature::WeaponArms, "Arm IK chain and the camera's shoulder anchor (left)."},
        {kBoneLowerArm[0], Feature::WeaponArms, "Arm IK chain (left)."},
        {kBoneHand[0], Feature::WeaponArms, "The hand IK'd onto the gun (left)."},
        {kBoneClavicle[1], Feature::WeaponArms, "Shoulder shrug toward the gun (right)."},
        {kBoneUpperArm[1], Feature::WeaponArms, "Arm IK chain and the camera's shoulder anchor (right)."},
        {kBoneLowerArm[1], Feature::WeaponArms, "Arm IK chain (right)."},
        {kBoneHand[1], Feature::WeaponArms, "The hand IK'd onto the gun (right)."},
    };
    return t;
}

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::vector<std::string> SplitCsv(const std::string& csv) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= csv.size()) {
        size_t j = csv.find(',', i);
        if (j == std::string::npos) j = csv.size();
        std::string t = csv.substr(i, j - i);
        const size_t a = t.find_first_not_of(" \t"), b = t.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(t.substr(a, b - a + 1));
        i = j + 1;
    }
    return out;
}

const char* TypeName(PT t) {
    switch (t) {
        case PT::Float: return "Float";
        case PT::Int: return "Int";
        case PT::Bool: return "Bool";
        case PT::Trigger: return "Trigger";
    }
    return "";
}

void Add(std::vector<Check>& out, Severity s, std::string msg, std::string hint = {}) {
    out.push_back({s, std::move(msg), std::move(hint)});
}

} // namespace

Severity Worst(const std::vector<Check>& checks) {
    Severity w = Severity::Ok;
    for (const Check& c : checks)
        if ((int)c.Level > (int)w) w = c.Level;
    return w;
}

std::vector<Check> Validate(const ValidationInput& in) {
    std::vector<Check> out;
    if (!in.Config) return out;
    const FirstPersonBodyComponent& cfg = *in.Config;

    if (in.BodyCount > 1)
        Add(out, Severity::Warning, "There is more than one First Person Body in the scene.",
            "Only the first one is used. Remove the others.");
    if (!in.HasPieces)
        Add(out, Severity::Error, "No rigged body pieces.",
            "Give this object a rigged model, or add child objects with rigged models (head, torso, legs...).");
    else if (!in.HasDriverPiece)
        Add(out, Severity::Error, "No piece has an Animator Controller.",
            "Add an Animator Controller to one piece (e.g. assets/Animations/Controllers/fps_body_locomotion.controller). The others follow it.");
    else if (!in.ControllerSet)
        Add(out, Severity::Error, "The driving piece's Animator Controller has no controller file.",
            "Set its Controller to a locomotion controller (e.g. assets/Animations/Controllers/fps_body_locomotion.controller).");
    else if (!in.Controller)
        Add(out, Severity::Error, "The driving piece's controller file could not be loaded.",
            "Check the path and that the file is valid (open it in the Animator window).");

    if (in.Controller) {
        const AnimatorController& c = *in.Controller;
        for (const ParamSpec& p : Params()) {
            if (!FeatureEnabled(p.Needed, cfg)) continue;
            const AnimatorController::Parameter* found = c.FindParameter(p.Name);
            if (!found) {
                Add(out, Severity::Warning,
                    std::string("The controller has no parameter '") + p.Name + "' (" + TypeName(p.Type) + "). " + p.What,
                    std::string("Add it in the Animator window's Parameters tab; ") + FeatureName(p.Needed) + " won't work without it.");
                continue;
            }
            const bool ok = (p.Type == PT::Float && (found->Type == PT::Float || found->Type == PT::Int)) ||
                            (p.Type == PT::Bool && (found->Type == PT::Bool || found->Type == PT::Trigger)) ||
                            (p.Type == PT::Trigger && found->Type == PT::Trigger);
            if (!ok)
                Add(out, Severity::Warning,
                    std::string("The controller's '") + p.Name + "' is a " + TypeName(found->Type) + " but the body sets it as a " + TypeName(p.Type) + ".",
                    "Change its type in the Parameters tab.");
        }
        auto hasState = [&](const char* name) {
            for (const auto& L : c.Layers)
                if (L.FindState(name) >= 0) return true;
            return false;
        };
        for (const StateSpec& s : States()) {
            if (!FeatureEnabled(s.Needed, cfg) || hasState(s.Name)) continue;
            // CrouchTurn only matters when the body also turns.
            if (std::string(s.Name) == kStateCrouchTurn && !FeatureEnabled(Feature::Turning, cfg)) continue;
            Add(out, s.Required ? Severity::Warning : Severity::Info,
                std::string("The controller has no state '") + s.Name + "'. " + s.What,
                s.Required ? std::string(FeatureName(s.Needed)) + " won't work without it. Add the state in the Animator window."
                           : "Optional: the body works, but this part of the motion is missing. Add the state to use it.");
        }
        if (cfg.FootIK) {
            bool tagged = false;
            for (const auto& L : c.Layers)
                for (const auto& st : L.States)
                    if (st.HasTag(kTagAirborne)) tagged = true;
            if (!tagged)
                Add(out, Severity::Warning, std::string("No state has the '") + kTagAirborne + "' tag.",
                    "Tag the Jump and Fall states 'Airborne' so Foot IK lets go of the ground while the feet are in the air.");
        }
    }

    if (in.HasBone) {
        const auto boneMap = ParseBoneMap(cfg.BoneMap);
        auto need = [&](const std::string& standard, Feature f, const char* what) {
            const std::string& bone = MappedBone(boneMap, standard);
            if (!FeatureEnabled(f, cfg) || in.HasBone(bone)) return;
            Add(out, Severity::Warning, "The body has no bone '" + bone + "'. " + what,
                std::string(FeatureName(f)) + " is skipped (or partly skipped) without it. Use the UE5 mannequin's bone names, or map them in Bone Map.");
        };
        if (!cfg.HeadBone.empty() && !in.HasBone(cfg.HeadBone))
            Add(out, Severity::Warning, "The body has no bone '" + cfg.HeadBone + "' (Head Bone).",
                "The camera stays at the eye height instead of riding the head. Set Head Bone to the head's bone.");
        for (const BoneSpec& b : Bones()) need(b.Name, b.Needed, b.What);
        for (const std::string& hb : SplitCsv(cfg.HiddenBones))
            if (!in.HasBone(hb))
                Add(out, Severity::Warning, "Hidden Bones names '" + hb + "', which the body doesn't have.", "Fix the name or remove it.");
    }

    if (!in.PieceNames.empty()) {
        if (cfg.WeaponArms && !cfg.ArmsPiece.empty()) {
            bool any = false;
            for (const std::string& n : in.PieceNames)
                if (Lower(n).find(Lower(cfg.ArmsPiece)) != std::string::npos) any = true;
            if (!any)
                Add(out, Severity::Warning, "No body piece is named like '" + cfg.ArmsPiece + "' (Arms Piece).",
                    "Weapon Arms needs the piece that is the arms: rename it or change Arms Piece.");
        }
        for (const std::string& part : SplitCsv(cfg.HiddenParts)) {
            bool any = false;
            for (const std::string& n : in.PieceNames)
                if (Lower(n).find(Lower(part)) != std::string::npos) any = true;
            if (!any)
                Add(out, Severity::Info, "Hidden Parts names '" + part + "', which no piece matches.",
                    "The camera is in the head: with no head piece hidden you may see inside it.");
        }
    }

    if (cfg.MaxTurnRate > 0.0f && cfg.TurnThreshold <= 0.0f)
        Add(out, Severity::Info, "Max Turn Rate does nothing while Turn Threshold is 0.", "Set a Turn Threshold (e.g. 55) to turn on the spot.");
    if (cfg.SpineTwist > 0.0f && cfg.TurnThreshold <= 0.0f)
        Add(out, Severity::Info, "Spine Twist does nothing while Turn Threshold is 0.", "Set a Turn Threshold to let the chest lead the feet.");
    if (in.HasDriverPiece)
        Add(out, Severity::Info, "Root motion on the driving piece is forced while playing: In Place, Rotation on, Vertical off.",
            "Its own Root Motion settings are ignored; the body hands the clips' travel to the player.");

    std::stable_sort(out.begin(), out.end(), [](const Check& a, const Check& b) { return (int)a.Level > (int)b.Level; });
    return out;
}

} // namespace FPBody
