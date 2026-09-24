#include "FirstPersonAdsCarry.h"

#include "AnimationSystem.h"
#include "AnimatorController.h"
#include "AssetLibrary.h"
#include "Components.h"
#include "FirstPersonAnimation.h"
#include "IK.h"
#include "Model.h"
#include "RotationMath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>

namespace K = FirstPersonAnimatorContract;

AdsCarryResult BuildAdsCarry(const AdsCarryInputs& in) {
    AdsCarryResult result;
    AdsCarryReport& report = result.Report;
    report.UsesIK = in.Rig && (int)in.Rig->Offsets.size() > std::max(in.AdsOffset, in.ProceduralOffset);
    if (!in.Arms || !in.Assets || !in.Controller || !in.Settings || in.Controller->Layers.empty()) return result;
    Model& arms = *in.Arms;
    const FirstPersonAdsSettings& settings = *in.Settings;
    const auto& L = in.Controller->Layers[0];
    const int armsTrack = in.Controller->TrackIndex(in.ArmsTrack);
    const int socket = arms.NodeIndex(in.GunBone);
    const int head = in.CameraBone.empty() ? -1 : arms.NodeIndex(in.CameraBone);

    std::vector<int> carried;
    for (int i = 0; i < (int)L.States.size(); ++i)
        if (L.States[i].HasTag(settings.CarryTag)) carried.push_back(i);
    if (carried.empty()) return result; // nothing to carry: not a problem, just nothing to do
    if (socket < 0) {
        report.Warnings.push_back("The arms rig has no '" + in.GunBone + "' bone (the weapon socket), so nothing can be carried.");
        return result;
    }

    // The reference: the named state, or the first one tagged ADS.
    int reference = settings.ReferenceState.empty() ? -1 : L.FindState(settings.ReferenceState);
    if (reference < 0 && settings.ReferenceState.empty())
        for (int i = 0; i < (int)L.States.size() && reference < 0; ++i)
            if (L.States[i].HasTag(K::kTagAds)) reference = i;
    if (reference < 0) {
        report.Warnings.push_back(settings.ReferenceState.empty()
                                      ? std::string("No state is tagged ADS to measure against.")
                                      : "There is no state named '" + settings.ReferenceState + "' to measure against.");
        return result;
    }
    report.Reference = L.States[reference].Name;
    if (!report.UsesIK)
        report.Warnings.push_back("No arm IK (switched off, or the rig lacks its bones): the whole view model is "
                                  "carried instead of just the gun, so the shoulders move too.");

    std::vector<int> parents(arms.NodeCount());
    for (int i = 0; i < (int)parents.size(); ++i) parents[i] = arms.NodeParent(i);
    // A state's first frame, and its socket relative to the camera bone (which sits on the eye).
    auto firstFrame = [&](int state, std::vector<LocalTRS>& pose, glm::mat4& socketFromEye, std::string& problem) {
        const std::string& path = L.States[state].MotionFor(armsTrack).Clip;
        const int clip = path.empty() ? -1 : ResolveAnimationClip(arms, path, *in.Assets);
        if (clip < 0) {
            problem = path.empty() ? "has no arms clip" : "its arms clip '" + path + "' didn't load";
            return false;
        }
        std::vector<glm::mat4> globals;
        arms.SampleLocalPose(clip, 0.0f, AnimationWrapMode::ClampForever, pose);
        IK::ComputeGlobals(pose, parents, globals);
        const glm::vec3 eye = head >= 0 ? IK::Position(globals[head]) : glm::vec3(0.0f);
        socketFromEye = glm::translate(glm::mat4(1.0f), -eye) * globals[socket];
        return true;
    };

    std::vector<LocalTRS> aimPose;
    glm::mat4 aim;
    std::string problem;
    if (!firstFrame(reference, aimPose, aim, problem)) {
        report.Warnings.push_back("The reference state '" + report.Reference + "' " + problem + ".");
        return result;
    }
    std::vector<glm::mat4> aimGlobals;
    IK::ComputeGlobals(aimPose, parents, aimGlobals);

    for (int state : carried) {
        AdsCarryReport::Entry entry;
        entry.State = L.States[state].Name;
        if (state == reference) {
            entry.Problem = "is the reference itself";
            report.Entries.push_back(entry);
            continue;
        }
        std::vector<LocalTRS> hipPose;
        glm::mat4 hip;
        if (!firstFrame(state, hipPose, hip, entry.Problem)) {
            report.Entries.push_back(entry);
            continue;
        }
        const glm::mat4 c = aim * glm::inverse(hip);
        AdsCarryAction a;
        a.State = state;
        a.StateName = entry.State;
        a.R = QuaternionFromMatrix(c);
        a.T = glm::vec3(c[3]);
        entry.GunOffsetCm = glm::length(a.T) * 100.0f;
        entry.GunTurnDeg = glm::degrees(glm::angle(a.R));

        if (report.UsesIK && (settings.MatchElbows || settings.MatchTwist)) {
            // Solve the first frame with its correction, as Play will.
            IKRigComponent probe = *in.Rig;
            probe.Weight = 1.0f;
            probe.Offsets[in.AdsOffset].Rotation = a.R;
            probe.Offsets[in.AdsOffset].Position = a.T;
            probe.Offsets[in.ProceduralOffset] = IKBoneOffset{in.Rig->Offsets[in.ProceduralOffset].Bone};
            probe.LimbA.Swivel = probe.LimbB.Swivel = 0.0f;
            probe.LocalRotations.clear();
            const IKLimb* limbs[2] = {&probe.LimbA, &probe.LimbB};
            if (settings.MatchElbows) {
                // How far each elbow has to swing about its shoulder->hand line to land on the
                // reference's.
                std::vector<LocalTRS> solved = hipPose;
                std::vector<glm::mat4> g;
                IK::ApplyRig(probe, arms, solved);
                IK::ComputeGlobals(solved, parents, g);
                for (int arm = 0; arm < 2; ++arm) {
                    const int up = arms.NodeIndex(limbs[arm]->Upper), lo = arms.NodeIndex(limbs[arm]->Lower),
                              end = arms.NodeIndex(limbs[arm]->End);
                    if (!limbs[arm]->Enabled || up < 0 || lo < 0 || end < 0) continue;
                    const glm::vec3 s = IK::Position(g[up]);
                    const glm::vec3 n = glm::normalize(IK::Position(g[end]) - s);
                    glm::vec3 from = IK::Position(g[lo]) - s, to = IK::Position(aimGlobals[lo]) - IK::Position(aimGlobals[up]);
                    from -= n * glm::dot(from, n);
                    to -= n * glm::dot(to, n);
                    if (glm::length(from) < 1e-6f || glm::length(to) < 1e-6f) continue;
                    a.Swivel[arm] = std::atan2(glm::dot(n, glm::cross(from, to)), glm::dot(from, to));
                    entry.SwivelDeg[arm] = glm::degrees(a.Swivel[arm]);
                }
                probe.LimbA.Swivel = a.Swivel[0];
                probe.LimbB.Swivel = a.Swivel[1];
            }
            if (settings.MatchTwist) {
                // With the swivel on the solved chain is the reference's; what's left are the
                // helper bones hanging directly off it that the clips key but IK doesn't solve
                // (the twists spreading the wrist's roll along the skin).
                std::vector<LocalTRS> solved = hipPose;
                IK::ApplyRig(probe, arms, solved);
                for (const IKLimb* limb : limbs) {
                    const int up = arms.NodeIndex(limb->Upper), lo = arms.NodeIndex(limb->Lower), end = arms.NodeIndex(limb->End);
                    if (!limb->Enabled || up < 0 || lo < 0 || end < 0) continue;
                    for (int i = 0; i < arms.NodeCount(); ++i) {
                        const int p = parents[i];
                        if (i == lo || i == end || (p != up && p != lo)) continue;
                        const glm::quat d = glm::normalize(aimPose[i].R * glm::inverse(glm::normalize(solved[i].R)));
                        if (std::fabs(d.w) < 0.99999f) a.Locals.push_back({arms.NodeName(i), d});
                    }
                }
                entry.MatchedBones = (int)a.Locals.size();
            }
        }
        result.Actions.push_back(std::move(a));
        report.Entries.push_back(entry);
    }
    return result;
}

namespace {
std::string ReportKey(const std::string& path) {
    std::string key = std::filesystem::u8path(path).lexically_normal().generic_u8string();
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return key;
}
std::map<std::string, AdsCarryReport>& Reports() {
    static std::map<std::string, AdsCarryReport> reports;
    return reports;
}
} // namespace

void PublishAdsCarryReport(const std::string& weaponPath, const AdsCarryReport& report) {
    Reports()[ReportKey(weaponPath)] = report;
}

const AdsCarryReport* FindAdsCarryReport(const std::string& weaponPath) {
    const auto it = Reports().find(ReportKey(weaponPath));
    return it == Reports().end() ? nullptr : &it->second;
}

AdsCarrySample EvaluateAdsCarry(const AnimatorLayerRuntime& layer, const std::vector<AdsCarryAction>& actions,
                                float dt, float hold) {
    AdsCarrySample out;
    if (hold <= 0.0f || actions.empty()) return out;
    // Each crossfade entry fades in over everything beneath it; walk down from the top.
    const auto& stack = layer.Stack;
    float remaining = 1.0f, total = 0.0f, best = 0.0f;
    for (int k = (int)stack.size() - 1; k >= 0 && remaining > 0.0f; --k) {
        float fade = stack[k].Fade;
        if (dt > 0.0f && fade < 1.0f)
            fade = stack[k].FadeDuration > 0.0f ? std::min(1.0f, fade + dt / stack[k].FadeDuration) : 1.0f;
        const float w = k == 0 ? remaining : remaining * AnimatorCrossfadeWeight(fade);
        remaining -= w;
        for (const AdsCarryAction& a : actions)
            if (a.State == stack[k].State) {
                total += w;
                if (w > best) { best = w; out.Action = &a; }
            }
    }
    if (!out.Action) return out;
    out.Weight = std::clamp(total * std::min(hold, 1.0f), 0.0f, 1.0f);
    out.R = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), out.Action->R, out.Weight);
    out.T = out.Action->T * out.Weight;
    out.Swivel[0] = out.Action->Swivel[0] * out.Weight;
    out.Swivel[1] = out.Action->Swivel[1] * out.Weight;
    return out;
}
