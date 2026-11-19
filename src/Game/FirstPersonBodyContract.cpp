#include "FirstPersonBodyContract.h"

#include "Components.h"

#include <algorithm>
#include <cctype>

namespace FPBody {

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

const char* FeatureName(Feature f) {
    switch (f) {
        case Feature::Core: return "the body";
        case Feature::FootIK: return "Foot IK";
        case Feature::WeaponArms: return "Weapon Arms";
        case Feature::SpineAim: return "Spine Aim / Twist";
    }
    return "";
}

bool FeatureEnabled(Feature f, const FirstPersonBodyComponent& cfg) {
    switch (f) {
        case Feature::Core: return true;
        case Feature::FootIK: return cfg.FootIK;
        case Feature::WeaponArms: return cfg.WeaponArms;
        case Feature::SpineAim: return cfg.SpineAim > 0.0f || cfg.SpineTwist > 0.0f;
    }
    return false;
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

void Add(std::vector<Check>& out, Severity s, std::string msg, std::string hint = {}) {
    out.push_back({s, std::move(msg), std::move(hint)});
}

} // namespace

bool IsArmsPiece(const FirstPersonBodyComponent& cfg, const std::string& pieceName) {
    const std::string name = Lower(pieceName);
    for (const std::string* want : {&cfg.ArmsPiece, &cfg.HandsPiece})
        if (!want->empty() && name.find(Lower(*want)) != std::string::npos) return true;
    return false;
}

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
        Add(out, Severity::Error, "No piece has the whole skeleton.",
            "One piece (the torso) needs the pelvis, the head and the hands: motion matching poses it, and the others follow it.");

    if (in.HasBone) {
        const auto boneMap = ParseBoneMap(cfg.BoneMap);
        auto need = [&](const std::string& standard, Feature f, const char* what) {
            const std::string& bone = MappedBone(boneMap, standard);
            if (!FeatureEnabled(f, cfg) || in.HasBone(bone)) return;
            Add(out, Severity::Warning, "The body has no bone '" + bone + "'. " + what,
                std::string(FeatureName(f)) + " is skipped (or partly skipped) without it. Use the UE5 mannequin's bone names, or map them in Bone Map.");
        };
        if (const std::string& head = MappedBone(boneMap, cfg.HeadBone); !cfg.HeadBone.empty() && !in.HasBone(head))
            Add(out, Severity::Warning, "The body has no bone '" + head + "' (Head Bone).",
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
                if (IsArmsPiece(cfg, n)) any = true;
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
    std::stable_sort(out.begin(), out.end(), [](const Check& a, const Check& b) { return (int)a.Level > (int)b.Level; });
    return out;
}

} // namespace FPBody
