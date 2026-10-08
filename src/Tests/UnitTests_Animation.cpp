#include "UnitTestSupport.h"
#include "../Game/Components.h"
#include "../Game/Npc/NpcBody.h"
#include "World.h"
#include "SceneSerializer.h"
#include "AssetLibrary.h"
#include "../Game/FirstPersonProcedural.h"
#include "../Game/AnimatorController.h"
#include "CameraEffects.h"
#include "Camera.h"
#include "FirstPersonAnimation.h"
#include "ProjectPaths.h"
#include "Model.h"
#include <cmath>
#include <cstdio>

// Unit tests for animation poses and IK. Add a function per test and list it below.

// Every new tunable is saved to the scene and loaded back (the component registry is what the Inspector and
// the serializer both walk, so a field missing from it would silently reset on load).
void TestFirstPersonBodyNpcTunablesRoundTrip() {
    World world;
    AssetLibrary assets;
    const entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Body");
    FirstPersonBodyComponent set;
    set.ElbowEase = 0.11f;
    set.ElbowMaxRate = 321.0f;
    set.NpcTurnThreshold = 71.0f;
    set.NpcMoveEase = 0.17f;
    set.NpcFaceEase = 0.13f;
    set.NpcMaxTwist = 55.0f;
    set.NpcAimLean = 7.0f;
    set.NpcAimLeanCrouched = 17.0f;
    set.NpcReadyLeanCrouched = 31.0f;
    set.NpcCowerHunch = 27.0f;
    set.NpcHeadMaxYaw = 61.0f;
    set.NpcHeadMaxPitch = 33.0f;
    set.NpcFootIKMaxDrop = 0.41f;
    set.NpcFootIKMaxRaise = 0.43f;
    set.NpcFootIKPelvisRaise = 0.12f;
    set.NpcFootIKTiltMax = 35.0f;
    set.NpcFootOffsetEase = 0.07f;
    set.NpcFootNormalEase = 0.09f;
    set.NpcFootIKFade = 0.21f;
    world.Registry.emplace<FirstPersonBodyComponent>(e, set);

    World loaded;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(loaded, assets2, SceneSerializer::SaveToString(world, assets)));
    const auto view = loaded.Registry.view<FirstPersonBodyComponent>();
    CHECK(view.size() == 1);
    if (view.empty()) return;
    const auto& got = loaded.Registry.get<FirstPersonBodyComponent>(*view.begin());
    CHECK(got.ElbowEase == set.ElbowEase);
    CHECK(got.ElbowMaxRate == set.ElbowMaxRate);
    CHECK(got.NpcTurnThreshold == set.NpcTurnThreshold);
    CHECK(got.NpcMoveEase == set.NpcMoveEase);
    CHECK(got.NpcFaceEase == set.NpcFaceEase);
    CHECK(got.NpcMaxTwist == set.NpcMaxTwist);
    CHECK(got.NpcAimLean == set.NpcAimLean);
    CHECK(got.NpcAimLeanCrouched == set.NpcAimLeanCrouched);
    CHECK(got.NpcReadyLeanCrouched == set.NpcReadyLeanCrouched);
    CHECK(got.NpcCowerHunch == set.NpcCowerHunch);
    CHECK(got.NpcHeadMaxYaw == set.NpcHeadMaxYaw);
    CHECK(got.NpcHeadMaxPitch == set.NpcHeadMaxPitch);
    CHECK(got.NpcFootIKMaxDrop == set.NpcFootIKMaxDrop);
    CHECK(got.NpcFootIKMaxRaise == set.NpcFootIKMaxRaise);
    CHECK(got.NpcFootIKPelvisRaise == set.NpcFootIKPelvisRaise);
    CHECK(got.NpcFootIKTiltMax == set.NpcFootIKTiltMax);
    CHECK(got.NpcFootOffsetEase == set.NpcFootOffsetEase);
    CHECK(got.NpcFootNormalEase == set.NpcFootNormalEase);
    CHECK(got.NpcFootIKFade == set.NpcFootIKFade);
}

void TestNpcTurnThresholdAffectsTurning() {
    // Verify that changing NpcTurnThreshold affects NpcShouldTurn decision
    float offset = 1.3f; // ~74 degrees, should turn with default threshold

    // Default threshold is 1.15f, offset of 1.3f should trigger a turn
    CHECK(NpcShouldTurn(offset, 1.15f) == true);

    // Higher threshold (1.5f) should not trigger with same offset
    CHECK(NpcShouldTurn(offset, 1.5f) == false);

    // Smaller offset should not turn
    CHECK(NpcShouldTurn(0.8f, 1.15f) == false);
}

void TestNpcSpineTwistClamping() {
    // Verify that spine twist is properly clamped to maxTwist
    float offset = 2.0f; // 2 radians offset, should be clamped
    float maxTwist = 1.2f;

    float twisted = NpcSpineTwist(offset, maxTwist);

    // Result should be clamped to +/- maxTwist
    CHECK(std::abs(twisted) <= maxTwist + 0.01f); // small epsilon for float comparison
    CHECK(twisted > 0.0f); // positive offset should give positive twist
}

void TestFootTiltLimiting() {
    // Verify that foot tilt is properly limited by max angle
    // Simulate a steep slope (e.g., normal vector tilted 30 degrees)
    glm::vec3 normal(0.5f, 0.866f, 0.0f); // normal tilted ~30 deg from vertical
    float maxTilt = 0.5f; // radians, about 28 degrees

    float angle = std::min(std::acos(std::clamp(normal.y, -1.0f, 1.0f)), maxTilt);

    // angle should be clamped to maxTilt
    CHECK(angle <= maxTilt);
    CHECK(angle > 0.4f); // Should be substantial but under the limit
}

// ---- lane A round 2: pole/hint, max stretch, spine distribution, grip offsets ----
namespace {
// root (uniform scale, as imported) -> upper -> lower (bent) -> end; the elbow sits below the shoulder-hand line.
struct HintArm {
    IK::Pose pose{4};
    std::vector<int> parents{-1, 0, 1, 2};
    std::vector<glm::mat4> g;
    HintArm() {
        pose[0].S = glm::vec3(0.5f);
        pose[1].T = glm::vec3(0.0f, 1.0f, 0.0f);
        pose[2].T = glm::vec3(1.0f, 0.0f, 0.0f);
        pose[2].R = glm::angleAxis(0.6f, glm::vec3(0, 0, 1)); // the hand rises from the elbow: the elbow hangs below the line
        pose[3].T = glm::vec3(1.0f, 0.0f, 0.0f);
        IK::ComputeGlobals(pose, parents, g);
    }
    float Reach() const { return glm::length(IK::Position(g[2]) - IK::Position(g[1])) + glm::length(IK::Position(g[3]) - IK::Position(g[2])); }
};
// Angle (degrees) between the elbow's side and `pole`, both seen around the shoulder-hand line.
float ElbowToPoleDeg(const std::vector<glm::mat4>& g, const glm::vec3& pole) {
    const glm::vec3 a = IK::Position(g[1]);
    const glm::vec3 n = glm::normalize(IK::Position(g[3]) - a);
    const glm::vec3 e = IK::Position(g[2]) - a, p = pole - a;
    const glm::vec3 pe = e - n * glm::dot(e, n), pp = p - n * glm::dot(p, n);
    return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(pe), glm::normalize(pp)), -1.0f, 1.0f)));
}
glm::vec3 SweepTarget(const HintArm& arm, float turn) { // the target circles the x axis: it crosses the bend plane
    const glm::vec3 a = IK::Position(arm.g[1]);
    const glm::vec3 dir = glm::angleAxis(turn, glm::vec3(1, 0, 0)) * glm::normalize(glm::vec3(0.5f, -0.9f, 0.1f));
    return a + dir * arm.Reach() * 0.75f;
}
} // namespace

void TestTwoBoneHintDefaultsKeepOldSolve() {
    const HintArm arm;
    IK::TwoBoneHint off; // HintWeight 0, no stretch
    for (int i = 0; i < 12; ++i) {
        const glm::vec3 target = SweepTarget(arm, 0.5f * (float)i);
        IK::Pose p0 = arm.pose, p1 = arm.pose;
        std::vector<glm::mat4> g0 = arm.g, g1 = arm.g;
        const glm::quat rot = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
        CHECK(IK::SolveTwoBone(p0, arm.parents, g0, 1, 2, 3, target, &rot, 0.8f, 0.2f));
        CHECK(IK::SolveTwoBone(p1, arm.parents, g1, 1, 2, 3, target, &rot, 0.8f, 0.2f, &off));
        for (int n = 1; n <= 3; ++n) {
            CHECK(glm::length(p0[n].T - p1[n].T) < 1e-6f);
            CHECK(std::fabs(glm::dot(p0[n].R, p1[n].R)) > 1.0f - 1e-6f);
        }
    }
}

void TestTwoBoneHintStopsElbowFlip() {
    const HintArm arm;
    const glm::vec3 pole = IK::Position(arm.g[2]); // the animated elbow is the pole
    float worstOff = 0.0f, worstOn = 0.0f;
    IK::TwoBoneHint hint;
    hint.HintWeight = 1.0f;
    for (int i = 0; i < 48; ++i) {
        const glm::vec3 target = SweepTarget(arm, glm::radians(7.5f * (float)i));
        IK::Pose p0 = arm.pose, p1 = arm.pose;
        std::vector<glm::mat4> g0 = arm.g, g1 = arm.g;
        CHECK(IK::SolveTwoBone(p0, arm.parents, g0, 1, 2, 3, target, nullptr, 1.0f));
        CHECK(IK::SolveTwoBone(p1, arm.parents, g1, 1, 2, 3, target, nullptr, 1.0f, 0.0f, &hint));
        CHECK(glm::length(IK::Position(g1[3]) - target) < 1e-3f); // still reaches
        worstOff = std::max(worstOff, ElbowToPoleDeg(g0, pole));
        worstOn = std::max(worstOn, ElbowToPoleDeg(g1, pole));
    }
    std::printf("[anim] elbow-to-pole worst angle over the sweep: no hint %.1f deg, hint %.2f deg\n", worstOff, worstOn);
    CHECK(worstOff > 60.0f); // the old solve swings the elbow round with the target (it flips)
    CHECK(worstOn < 1.0f);   // with the pole it stays on its side
    // A pole offset pushes the elbow: out toward +z.
    hint.HintOffset = glm::vec3(0.0f, 0.0f, 5.0f);
    IK::Pose p = arm.pose;
    std::vector<glm::mat4> g = arm.g;
    CHECK(IK::SolveTwoBone(p, arm.parents, g, 1, 2, 3, SweepTarget(arm, 0.0f), nullptr, 1.0f, 0.0f, &hint));
    CHECK(IK::Position(g[2]).z > IK::Position(arm.g[2]).z + 0.1f);
    // HintWeight scales the turn: half way is less than the whole.
    IK::TwoBoneHint half = hint;
    half.HintWeight = 0.5f;
    IK::Pose ph = arm.pose;
    std::vector<glm::mat4> gh = arm.g;
    CHECK(IK::SolveTwoBone(ph, arm.parents, gh, 1, 2, 3, SweepTarget(arm, 0.0f), nullptr, 1.0f, 0.0f, &half));
    CHECK(IK::Position(gh[2]).z > 0.0f && IK::Position(gh[2]).z < IK::Position(g[2]).z);
}

void TestTwoBoneMaxLimbScale() {
    const HintArm arm;
    const glm::vec3 a = IK::Position(arm.g[1]);
    const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, -0.2f, 0.3f));
    const glm::vec3 target = a + dir * arm.Reach() * 1.6f;
    const auto solved = [&](float scale) {
        IK::TwoBoneHint h;
        h.MaxLimbScale = scale;
        IK::Pose p = arm.pose;
        std::vector<glm::mat4> g = arm.g;
        CHECK(IK::SolveTwoBone(p, arm.parents, g, 1, 2, 3, target, nullptr, 1.0f, 0.0f, &h));
        return glm::length(IK::Position(g[3]) - a);
    };
    const float reach = arm.Reach();
    std::printf("[anim] reach for a target at 1.6x: scale 1 -> %.3fx, 1.25 -> %.3fx, 2 -> %.3fx\n", solved(1.0f) / reach, solved(1.25f) / reach, solved(2.0f) / reach);
    CHECK(std::fabs(solved(1.0f) / reach - 1.0f) < 1e-3f);          // never stretches
    CHECK(std::fabs(solved(1.25f) / reach - 1.25f) < 2e-3f);        // capped at the limit
    CHECK(std::fabs(solved(2.0f) / reach - 1.6f) < 2e-3f);          // room to spare: lands on the target
}

void TestSpineDistributionDefaultsEven() {
    IK::SpineDistribution d;
    float div[5];
    for (int n = 1; n <= 5; ++n) {
        IK::ChainDivisors(d.Weight, n, div);
        for (int k = 0; k < n; ++k) CHECK(div[k] == (float)n); // exactly today's 1/n, bit for bit
    }
    // A changed weight changes the distribution, and the shares still sum to the whole turn.
    d.Weight[0] = 0.0f;
    d.Weight[4] = 3.0f;
    IK::ChainDivisors(d.Weight, 5, div);
    float sum = 0.0f;
    for (int k = 0; k < 5; ++k) sum += 1.0f / div[k];
    CHECK(std::fabs(sum - 1.0f) < 1e-5f);
    CHECK(std::isinf(div[0]));                    // takes nothing
    CHECK(1.0f / div[4] > 1.0f / div[1] * 2.9f);  // three times the others
    // Per-bone limit.
    const glm::quat big = glm::angleAxis(glm::radians(40.0f), glm::vec3(0, 1, 0));
    const glm::quat capped = IK::ClampStepAngle(big, 10.0f);
    CHECK(std::fabs(glm::degrees(glm::angle(capped)) - 10.0f) < 0.01f);
    CHECK(IK::ClampStepAngle(big, 0.0f) == big);                                      // 0 = no limit
    CHECK(std::fabs(glm::degrees(glm::angle(IK::ClampStepAngle(big, 90.0f))) - 40.0f) < 0.01f);
}

void TestLimbGripOffsetMovesHand() {
    const glm::mat4 gun = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3)) * glm::mat4_cast(glm::angleAxis(0.8f, glm::normalize(glm::vec3(0, 1, 1))));
    const glm::mat4 rel = glm::translate(glm::mat4(1.0f), glm::vec3(0.1f, -0.05f, 0.2f));
    const glm::mat4 id(1.0f);
    const glm::mat4 none = IK::LimbGoal(gun, rel, true, id, glm::vec3(0.0f), glm::vec3(0.0f));
    CHECK(glm::length(glm::vec3(none[3]) - glm::vec3((gun * rel)[3])) < 1e-6f); // zero offset = as animated
    const glm::vec3 off(0.03f, 0.0f, -0.02f);
    const glm::mat4 moved = IK::LimbGoal(gun, rel, true, id, off, glm::vec3(0.0f));
    const glm::vec3 delta = glm::vec3(moved[3]) - glm::vec3(none[3]);
    CHECK(glm::length(delta - glm::vec3(gun * glm::vec4(off, 0.0f))) < 1e-6f); // moved by the offset, in the gun's frame
    const glm::mat4 turned = IK::LimbGoal(gun, rel, true, id, glm::vec3(0.0f), glm::vec3(0.0f, 30.0f, 0.0f));
    const glm::quat q = glm::quat_cast(glm::mat3(glm::inverse(gun) * turned * glm::inverse(rel)));
    CHECK(std::fabs(glm::degrees(glm::angle(q)) - 30.0f) < 0.1f);
    // The hand really lands on the offset goal.
    const HintArm arm;
    const glm::vec3 a = IK::Position(arm.g[1]);
    const glm::mat4 target = glm::translate(glm::mat4(1.0f), a + glm::vec3(0.8f, -0.4f, 0.1f));
    const glm::mat4 goal = IK::LimbGoal(target, id, false, id, glm::vec3(0.0f, 0.1f, 0.0f), glm::vec3(0.0f));
    IK::Pose p = arm.pose;
    std::vector<glm::mat4> g = arm.g;
    CHECK(IK::SolveTwoBone(p, arm.parents, g, 1, 2, 3, IK::Position(goal), nullptr, 1.0f));
    CHECK(glm::length(IK::Position(g[3]) - (glm::vec3(target[3]) + glm::vec3(0.0f, 0.1f, 0.0f))) < 1e-3f);
}

void TestWeaponIkHandOffsetsRoundTrip() {
    WeaponProceduralSettings s = WeaponProceduralSettings::Defaults();
    CHECK(s.IK.RightHandPosition == glm::vec3(0.0f) && s.IK.LeftHandRotation == glm::vec3(0.0f)); // default zero
    s.IK.RightHandPosition = glm::vec3(0.01f, -0.02f, 0.03f);
    s.IK.LeftHandRotation = glm::vec3(5.0f, -6.0f, 7.0f);
    WeaponProceduralSettings back;
    std::string err;
    CHECK(WeaponProceduralSettings::FromJson(s.ToJson(), back, &err));
    CHECK(back.IK.RightHandPosition == s.IK.RightHandPosition);
    CHECK(back.IK.LeftHandRotation == s.IK.LeftHandRotation);
}

// ---- lane A round 3 ----
// A weight curve authored on a state is read back at the state's phase, blended by the crossfade; no curve = 1.
void TestClipWeightCurveSamplingAndLayerScaling() {
    AnimatorController c;
    AnimatorController::State a, b;
    a.Name = "A";
    b.Name = "B";
    a.Curves.push_back({"IK_LeftHand", {{0.0f, 1.0f}, {0.5f, 0.0f}, {1.0f, 0.0f}}});
    a.Curves.push_back({"IK", {{0.0f, 0.5f}}});
    c.Layers[0].States = {a, b};
    // JSON round trip keeps the curves.
    AnimatorController back;
    CHECK(AnimatorController::FromJsonString(c.ToJsonString(), back));
    CHECK(back.Layers[0].States[0].Curves.size() == 2 && back.Layers[0].States[0].Curves[0].Keys.size() == 3);
    CHECK(back.Layers[0].States[1].Curves.empty());

    AnimatorControllerComponent ac;
    ac.Layers.resize(1);
    ac.Layers[0].Stack.push_back({0, 0.25f, 1.0f, 0.0f, -1.0f});
    CHECK(std::fabs(AnimatorSampleCurve(c, ac, 0, "IK_LeftHand") - 0.5f) < 1e-5f);
    CHECK(AnimatorSampleCurve(c, ac, 0, "Look") == 1.0f);          // no curve = 1
    ac.Layers[0].Stack[0].Phase = 1.75f;                           // loops count past 1: 0.75 -> 0
    CHECK(std::fabs(AnimatorSampleCurve(c, ac, 0, "IK_LeftHand")) < 1e-5f);
    // Crossfading into B (no curve): halfway the eased weight is 0.5 -> halfway between 0 and 1.
    ac.Layers[0].Stack.push_back({1, 0.0f, 0.5f, 0.2f, -1.0f});
    CHECK(std::fabs(AnimatorSampleCurve(c, ac, 0, "IK_LeftHand") - 0.5f) < 1e-5f);
    CHECK(std::fabs(AnimatorSampleCurve(c, ac, 0, "Missing", 0.7f) - 0.7f) < 1e-5f);

    // The weapon IK weights: left hand off while the curve says so, the others untouched; UseClipCurves off = all 1.
    ac.Layers[0].Stack.assign(1, {0, 0.75f, 1.0f, 0.0f, -1.0f});
    WeaponIKSettings ik;
    IKCurveWeights w = SampleIKCurves(ik, c, ac);
    CHECK(w.LeftHand == 0.0f && w.RightHand == 1.0f && w.Look == 1.0f && std::fabs(w.All - 0.5f) < 1e-5f);
    ik.UseClipCurves = false;
    w = SampleIKCurves(ik, c, ac);
    CHECK(w.LeftHand == 1.0f && w.All == 1.0f);
}

// The IK curve scales the procedural IK weight; the IKOff tag still takes it to 0; no curve = old behaviour.
void TestClipCurveAndIKOffTagCombine() {
    const WeaponProceduralSettings s = WeaponProceduralSettings::Defaults();
    auto settle = [&](bool off, float curve) {
        WeaponProceduralState st;
        WeaponProceduralInput in;
        in.Dt = 1.0f / 60.0f;
        in.IKOff = off;
        in.IKCurve = curve;
        WeaponProceduralPose p;
        for (int i = 0; i < 60; ++i) p = st.Update(s, in);
        return p.IKWeight;
    };
    CHECK(settle(false, 1.0f) == 1.0f);
    CHECK(std::fabs(settle(false, 0.4f) - 0.4f) < 1e-5f);
    CHECK(settle(true, 1.0f) == 0.0f);
    CHECK(settle(true, 0.4f) == 0.0f);
    // The new settings round-trip.
    WeaponProceduralSettings k = s, back;
    k.IK.CurveLeftHand = "Left";
    k.IK.UseClipCurves = false;
    CHECK(WeaponProceduralSettings::FromJson(k.ToJson(), back, nullptr));
    CHECK(back.IK.CurveLeftHand == "Left" && !back.IK.UseClipCurves && back.IK.CurveAll == "IK");
}

void TestFreeAimDeadZoneMath() {
    const glm::vec2 zone(4.0f, 2.0f);
    glm::vec2 o(0.0f);
    o = StepFreeAim(o, glm::vec2(3.0f, 0.0f), zone, 30.0f, false, 0.016f);
    CHECK(o == glm::vec2(3.0f, 0.0f));                  // inside the zone: the view turned, the gun held
    o = StepFreeAim(o, glm::vec2(5.0f, 5.0f), zone, 30.0f, false, 0.016f);
    CHECK(o == zone);                                   // past it the offset stops at the edge: the gun follows
    o = StepFreeAim(o, glm::vec2(-1.0f, 0.0f), zone, 30.0f, true, 0.016f);
    CHECK(glm::length(o) < glm::length(zone) - 0.4f);   // stopped: returns at ~30 deg/s
    for (int i = 0; i < 20; ++i) o = StepFreeAim(o, glm::vec2(0.0f), zone, 30.0f, true, 0.016f);
    CHECK(o == glm::vec2(0.0f));
    CHECK(StepFreeAim(glm::vec2(1.0f), glm::vec2(9.0f), glm::vec2(0.0f), 30.0f, false, 0.016f) == glm::vec2(0.0f)); // off

    // In the stack: the zone fully taken up by a steady turn adds its degrees of lag to the gun's yaw.
    auto pose = [&](glm::vec2 zoneSetting) {
        WeaponProceduralSettings s = WeaponProceduralSettings::Defaults();
        s.Sway.FreeAimZone = zoneSetting;
        s.Sway.UnityPort = false;
        WeaponProceduralState st;
        WeaponProceduralInput in;
        in.Dt = 1.0f / 60.0f;
        in.LookRate = glm::vec2(20.0f, 0.0f);
        WeaponProceduralPose p;
        for (int i = 0; i < 30; ++i) p = st.Update(s, in);
        return p;
    };
    const WeaponProceduralPose off = pose(glm::vec2(0.0f)), on = pose(glm::vec2(3.0f, 3.0f));
    CHECK(std::fabs((on.Rotation.y - off.Rotation.y) - 3.0f) < 0.05f);
    WeaponProceduralSettings k = WeaponProceduralSettings::Defaults(), back;
    k.Sway.FreeAimZone = glm::vec2(2.0f, 1.0f);
    k.Sway.FreeAimReturn = 12.0f;
    k.Sway.UnityPort = false;
    CHECK(WeaponProceduralSettings::FromJson(k.ToJson(), back, nullptr));
    CHECK(back.Sway.FreeAimZone == k.Sway.FreeAimZone && back.Sway.FreeAimReturn == 12.0f);
}

void TestAdsBlendPieces() {
    // Additive 1 = the old behaviour: pose + offset * ads.
    const glm::vec3 pose(1.0f, 2.0f, 3.0f), off(0.1f, 0.0f, -0.2f);
    CHECK(BlendAdsChannel(pose, off, 0.5f, 1.0f) == pose + off * 0.5f);
    CHECK(BlendAdsChannel(pose, off, 0.0f, 0.0f) == pose);       // sights down: nothing, whatever the mode
    CHECK(BlendAdsChannel(pose, off, 1.0f, 0.0f) == off);        // fully absolute, sights up: the offset alone

    auto settle = [&](WeaponProceduralSettings s, float crouch) {
        s.Aim.BlendTime = 0.01f;
        WeaponProceduralState st;
        WeaponProceduralInput in;
        in.Dt = 1.0f / 60.0f;
        in.Ads = true;
        in.Crouch = crouch;
        WeaponProceduralPose p;
        for (int i = 0; i < 90; ++i) p = st.Update(s, in);
        return p;
    };
    WeaponProceduralSettings s = WeaponProceduralSettings::Defaults();
    s.Aim.Position = glm::vec3(0.0f, -0.02f, 0.0f);
    const WeaponProceduralPose base = settle(s, 0.0f);
    // Defaults add nothing: the crouch input and a camera share of 0 change no number.
    CHECK(settle(s, 1.0f).Position == base.Position && settle(s, 1.0f).CameraOffset == base.CameraOffset);
    WeaponProceduralSettings c = s;
    c.Aim.CrouchPosition = glm::vec3(0.0f, -0.01f, 0.0f);
    CHECK(std::fabs(settle(c, 1.0f).Position.y - (base.Position.y - 0.01f)) < 1e-4f);
    CHECK(settle(c, 0.0f).Position == base.Position);            // standing: unchanged
    // Camera share: the gun takes (1 - share) of the offset, the view the rest; their difference is unchanged.
    WeaponProceduralSettings v = s;
    v.Aim.CameraShare = 0.5f;
    const WeaponProceduralPose p = settle(v, 0.0f);
    CHECK(std::fabs((p.Position.y - p.CameraOffset.y) - (base.Position.y - base.CameraOffset.y)) < 1e-4f);
    CHECK(std::fabs(p.CameraOffset.y - base.CameraOffset.y - 0.01f) < 1e-4f);
    WeaponProceduralSettings k = s, back;
    k.Aim.PositionAdditive = 0.25f;
    k.Aim.CrouchRotation = glm::vec3(1.0f, 2.0f, 3.0f);
    k.Aim.CameraShare = 0.3f;
    CHECK(WeaponProceduralSettings::FromJson(k.ToJson(), back, nullptr));
    CHECK(back.Aim.PositionAdditive == 0.25f && back.Aim.CrouchRotation == k.Aim.CrouchRotation && back.Aim.CameraShare == 0.3f);
    CHECK(WeaponProceduralSettings::Defaults().Aim.PositionAdditive == 1.0f && WeaponProceduralSettings::Defaults().Aim.RotationAdditive == 1.0f);
}
// ---- end lane A round 3 ----

// The cached arm-shape links (FirstPersonBodyArmShapeLinksFrom) pick exactly the nodes the per-frame name walk did:
// every node under a clavicle (itself included), in order, flagged when it is the clavicle, with the rig's counterpart.
static void TestArmShapeLinksMatchNameWalk() {
    // A spine with two clavicle branches (each a chain of 5 with a finger fork) and a leg branch, parents first.
    std::vector<int> parents = {-1, 0, 1, 2};      // root, pelvis, spine, chest
    const int clavL = (int)parents.size(); parents.push_back(3);
    for (int k = 0; k < 5; ++k) parents.push_back((int)parents.size() - 1);
    parents.push_back(clavL + 2); // fork off the chain
    const int clavR = (int)parents.size(); parents.push_back(3);
    for (int k = 0; k < 5; ++k) parents.push_back((int)parents.size() - 1);
    const int leg = (int)parents.size(); parents.push_back(1);
    parents.push_back(leg);
    const int count = (int)parents.size();
    const auto rigOf = [&](int i) { return i % 7 == 3 ? -1 : i + 10; }; // some nodes the rig lacks
    for (const bool haveR : {true, false}) {
        const int clavicles[2] = {clavL, haveR ? clavR : -1};
        std::vector<FirstPersonArmShapeLink> links;
        FirstPersonBodyArmShapeLinksFrom(count, parents, clavicles, rigOf, links);
        std::vector<FirstPersonArmShapeLink> want;
        for (int i = 0; i < count; ++i) {
            bool under = false;
            for (int n = i; n >= 0 && !under; n = parents[(size_t)n]) under = n == clavicles[0] || n == clavicles[1];
            if (!under || rigOf(i) < 0) continue;
            want.push_back({i, rigOf(i), i == clavicles[0] || i == clavicles[1]});
        }
        CHECK(links.size() == want.size() && !links.empty());
        for (size_t i = 0; i < std::min(links.size(), want.size()); ++i)
            CHECK(links[i].Body == want[i].Body && links[i].Rig == want[i].Rig && links[i].Clavicle == want[i].Clavicle);
    }
}

// ---- foot slide correction (stride warp + foot pin) ----
namespace {
// A treadmill gait: each foot plants for 0.4 s sliding back at `clipSpeed` in the capsule's frame, then swings forward; the capsule
// travels at `groundSpeed`. Returns the mean planted-foot drift (m) over the plants after a 2 s warm-up; `last` = the final output.
float FootSlideSim(const IK::FootSlideSettings& set, float clipSpeed, float groundSpeed, IK::FootSlideStats* stats = nullptr, IK::FootSlideOutput* last = nullptr, float ankle = 0.0f) {
    IK::FootSlide fs;
    const float dt = 1.0f / 120.0f;
    const float stance = 0.4f, half = 0.8f * 0.5f;
    float z = 0.0f;
    for (int f = 0; f < 120 * 8; ++f) {
        const float t = f * dt;
        if (t >= 2.0f && f == (int)(2.0f / dt)) fs.Stats.Clear();
        z += groundSpeed * dt;
        IK::FootSlideInput in;
        in.Feet = glm::vec3(0.0f, 0.0f, z);
        in.Velocity = glm::vec3(0.0f, 0.0f, groundSpeed);
        in.Dt = dt;
        in.Pelvis = in.Feet + glm::vec3(0.0f, 1.0f, 0.0f);
        for (int s = 0; s < 2; ++s) {
            const float ph = std::fmod(t + s * half, 2.0f * half);
            float rel, h;
            if (ph < stance) { rel = 0.5f * clipSpeed * stance - clipSpeed * ph; h = 0.0f; }
            else { const float u = (ph - stance) / (2.0f * half - stance); rel = -0.5f * clipSpeed * stance + clipSpeed * stance * u; h = 0.15f * std::sqrt(std::sin(3.14159265f * u)); } // lifts off quickly, like a heel-off
            h += ankle; // the ankle bone rides a few cm over the sole
            in.Foot[s] = in.Feet + glm::vec3(0.0f, h, rel);
            in.Height[s] = h;
        }
        const IK::FootSlideOutput o = fs.Step(set, in, !set.Active());
        if (last) *last = o;
    }
    if (stats) *stats = fs.Stats;
    return fs.Stats.Plants > 0 ? fs.Stats.SumSlide / fs.Stats.Plants : -1.0f;
}
// ---- lane A round 6 ----
// The clip files a controller reaches are what the editor prefetches: each file once, no "#clip" suffix, no
// own clips, blend-tree children included (a missing one is the 1 s first-Play-frame hitch).
void TestControllerClipFilesForPrefetch() {
    AnimatorController c;
    AnimatorController::State a, b, d;
    a.Name = "A"; a.Motions.resize(1); a.Motions[0].Clip = "anims/Walk.FBX#Walk";
    b.Name = "B"; b.Motions.resize(1); b.Motions[0].Clip = "Idle"; // a clip the model owns
    d.Name = "D";
    AnimatorController::BlendChild k0, k1, k2;
    k0.Clip = "anims/Walk.FBX#Run";
    k1.Clip = "anims/Strafe.glb";
    k2.Clip = "";
    d.Motions.resize(1); d.Motions[0].Children = {k0, k1, k2};
    c.Layers[0].States = {a, b, d};
    const auto files = AnimatorControllerClipFiles(c, 0);
    CHECK(files.size() == 2);
    CHECK(files.size() == 2 && files[0] == "anims/Walk.FBX" && files[1] == "anims/Strafe.glb");
}
// ---- end lane A round 6 ----

} // namespace

void TestStrideScaleMath() {
    CHECK(std::abs(IK::StrideScale(3.0f, 2.0f, 0.75f, 1.35f) - 1.35f) < 1e-5f);  // clamped high
    CHECK(std::abs(IK::StrideScale(1.0f, 2.0f, 0.75f, 1.35f) - 0.75f) < 1e-5f);  // clamped low
    CHECK(std::abs(IK::StrideScale(2.2f, 2.0f, 0.75f, 1.35f) - 1.1f) < 1e-5f);   // in range: the ratio
    CHECK(IK::StrideScale(0.1f, 2.0f, 0.75f, 1.35f) == 1.0f && IK::StrideScale(2.0f, 0.0f, 0.75f, 1.35f) == 1.0f); // standing / no estimate
    CHECK(IK::StrideWarpPelvisDrop(0.9f, 0.4f, 1.0f, 1.0f) == 0.0f && IK::StrideWarpPelvisDrop(0.9f, 0.4f, 0.8f, 1.0f) == 0.0f);
    const float drop = IK::StrideWarpPelvisDrop(0.9f, 0.4f, 1.3f, 1.0f);
    CHECK(drop > 0.005f && drop <= 0.15f);
    CHECK(IK::StrideWarpPelvisDrop(0.9f, 0.4f, 1.3f, 0.0f) == 0.0f);
}

void TestFootSlideDefaultsOff() {
    IK::FootSlideSettings off;
    CHECK(!off.Active());
    FirstPersonBodyComponent body;
    CHECK(!body.FootPinEnabled && !body.StrideWarpEnabled && !IK::FootSlideFrom(body).Active());
    IK::FootSlideOutput o;
    FootSlideSim(off, 2.0f, 3.0f, nullptr, &o);
    CHECK(o.Shift[0] == glm::vec3(0.0f) && o.Shift[1] == glm::vec3(0.0f) && o.PelvisDrop == 0.0f && o.Scale == 1.0f); // the pose is the clips'
}

void TestFootSlidePinHoldsPlantedFeet() {
    IK::FootSlideOutput previous;
    previous.Shift[0]=glm::vec3(0.2f,0,0);
    previous.PelvisDrop=0.05f;
    const auto release=IK::SmoothFootSlideOutput(previous,{},1.0f/60.0f,0.06f);
    CHECK(release.Shift[0].x>0.1f && release.Shift[0].x<0.2f);
    CHECK(release.PelvisDrop>0 && release.PelvisDrop<previous.PelvisDrop);
    auto split=IK::SmoothFootSlideOutput(previous,{},1.0f/120.0f,0.06f);
    split=IK::SmoothFootSlideOutput(split,{},1.0f/120.0f,0.06f);
    CHECK(glm::length(split.Shift[0]-release.Shift[0])<1e-6f);
    IK::FootSlide idle;
    IK::FootSlideSettings active;
    active.PinEnabled=active.StrideEnabled=true;
    IK::FootSlideInput input;
    input.Dt=1.0f/60.0f;
    input.Velocity=glm::vec3(0,0,2);
    input.Foot[0]=glm::vec3(-0.1f,0,0);
    input.Foot[1]=glm::vec3(0.1f,0,0);
    idle.Step(active,input);
    input.Velocity=glm::vec3(0);
    input.Foot[0].z+=0.2f;
    input.Foot[1].z+=0.2f;
    const auto stopped=idle.Step(active,input);
    CHECK(stopped.Shift[0]==glm::vec3(0) && stopped.Shift[1]==glm::vec3(0));
    CHECK(stopped.PelvisDrop==0 && stopped.Scale==1);
    IK::FootSlideStats base, pin, warp, both;
    IK::FootSlideSettings set;
    const float slideOff = FootSlideSim(set, 2.0f, 2.4f, &base);
    CHECK(base.Plants >= 6 && slideOff > 0.12f);          // plants are detected; the clips' feet slide ~16 cm a plant
    set.PinEnabled = true;
    const float slidePin = FootSlideSim(set, 2.0f, 2.4f, &pin);
    CHECK(slidePin < 0.03f && pin.Plants == base.Plants); // pinned within the leash: held
    IK::FootSlideSettings w;
    w.StrideEnabled = true;
    const float slideWarp = FootSlideSim(w, 2.0f, 2.4f, &warp);
    CHECK(slideWarp < slideOff * 0.4f);                   // the stride matched to the ground speed: most of it gone
    w.PinEnabled = true;
    const float slideBoth = FootSlideSim(w, 2.0f, 2.4f, &both);
    std::printf("[FootSlide] unit sim (clip 2.0, ground 2.4 m/s) drift per plant: clips %.1f cm, pin %.1f cm (%d/%d plants), warp %.1f cm, both %.1f cm\n", slideOff * 100, slidePin * 100, pin.Plants, base.Plants, slideWarp * 100, slideBoth * 100);
    CHECK(slideBoth < 0.02f);
    // A mismatch beyond the leash: the pin is dragged along, never further behind than Pin Max Drift.
    IK::FootSlideSettings leash;
    leash.PinEnabled = true;
    leash.PinMaxDrift = 0.1f;
    const float slideLeash = FootSlideSim(leash, 2.0f, 4.0f);
    CHECK(slideLeash > 0.05f && slideLeash < FootSlideSim(IK::FootSlideSettings{}, 2.0f, 4.0f));
    // The ankle bone rides 7 cm over the ground even planted: plants are still found, judged against each foot's own lowest height.
    IK::FootSlideStats ankleStats;
    IK::FootSlideSettings ap;
    ap.PinEnabled = true;
    CHECK(FootSlideSim(ap, 2.0f, 2.4f, &ankleStats, nullptr, 0.07f) < 0.03f && ankleStats.Plants == base.Plants);
    // Weight 0 = the clips' feet.
    IK::FootSlideSettings z;
    z.PinEnabled = true;
    z.PinWeight = 0.0f;
    CHECK(std::abs(FootSlideSim(z, 2.0f, 2.4f) - slideOff) < 0.005f);
}

void TestStrideWarpMatchesGroundSpeed() {
    IK::FootSlideSettings w;
    w.StrideEnabled = true;
    IK::FootSlideOutput o;
    FootSlideSim(w, 2.0f, 2.6f, nullptr, &o);
    CHECK(o.Scale > 1.25f && o.Scale <= 1.35f + 1e-4f);   // ~1.3 for 2.6 over 2.0
    CHECK(o.PelvisDrop > 0.0f);
    FootSlideSim(w, 2.0f, 5.0f, nullptr, &o);
    CHECK(o.Scale <= 1.35f + 1e-4f);                      // clamped
    w.StrideMax = 1.1f;
    FootSlideSim(w, 2.0f, 5.0f, nullptr, &o);
    CHECK(o.Scale <= 1.1f + 1e-4f);                       // the limit is tunable
    w.StrideMax = 1.35f;
    w.StrideWeight = 0.0f;
    FootSlideSim(w, 2.0f, 2.6f, nullptr, &o);
    CHECK(std::abs(o.Scale - 1.0f) < 1e-4f);
}

void TestAdsReloadPreservesAdditiveLocomotion() {
    IK::Pose reload(2),aim(2),reference(2),walk(2);
    reload[0].T=glm::vec3(0,0,0.4f);
    reload[1].T=glm::vec3(0.3f,0.2f,0);
    aim[0].T=glm::vec3(0,0,-0.2f);
    aim[1].T=glm::vec3(-1);
    walk[0].T=glm::vec3(0.02f,0.03f,0);
    walk[0].R=glm::angleAxis(0.1f,glm::vec3(0,1,0));
    walk[1].T=glm::vec3(0.01f,0,0);
    for(float strength : {0.0f,0.31f,1.0f}) {
        auto result=reload;
        IK::ApplyHeldPose(result,aim,{1,0});
        IK::ApplyAdditivePose(result,walk,reference,{},strength);
        CHECK(glm::length(result[0].T-(aim[0].T+walk[0].T*strength))<1e-6f);
        CHECK(glm::length(result[1].T-(reload[1].T+walk[1].T*strength))<1e-6f); // reload hand stays free
        CHECK(std::abs(glm::dot(result[0].R,glm::angleAxis(0.1f*strength,glm::vec3(0,1,0))))>0.99999f);
    }
    auto partial=reload;
    IK::ApplyHeldPose(partial,aim,{0.5f,0});
    IK::ApplyAdditivePose(partial,walk,reference,{},1);
    CHECK(glm::length(partial[0].T-(glm::mix(reload[0].T,aim[0].T,0.5f)+walk[0].T))<1e-6f);
}

void TestCameraEffects() {
    CameraShakeAsset asset,parsed;
    std::string error;
    CHECK(CameraShakeAsset::FromJsonString(asset.ToJsonString(),parsed,&error));
    CHECK(parsed.Duration==asset.Duration && parsed.Rotation[0].Keys.size()==asset.Rotation[0].Keys.size());
    const float original=parsed.Duration;
    CHECK(!CameraShakeAsset::FromJsonString("{\"duration\":-1}",parsed,&error));
    CHECK(parsed.Duration==original);
    CHECK(!CameraShakeAsset::FromJsonString("{\"rotation\":[1,2]}",parsed,&error));
    CHECK(!CameraShakeAsset::FromJsonString("{\"envelope\":[[0,1,0,0],[1,0,0,0]]}",parsed,&error));
    CHECK(!CameraShakeAsset::FromJsonString("{\"version\":2,\"rotationCurves\":[\"bad\",[],[]]}",parsed,&error));
    CHECK(CameraShakeAsset::FromJsonString("{\"version\":1,\"frequency\":20,\"rotation\":[1,2,3],\"envelope\":[[0,0],[0.5,1],[1,0]]}",parsed,&error));
    CHECK(parsed.Rotation[2].Evaluate(.5f)==3 && parsed.ToJsonString().find("frequency")==std::string::npos);
    asset.Rotation[0]=Curve::Kick(.08f);
    asset.Position[0]=Curve::Kick(.08f);
    for(auto& key:asset.Rotation[0].Keys) { key.Value*=-.6f; key.InTangent*=-.6f; key.OutTangent*=-.6f; }
    for(auto& key:asset.Position[0].Keys) { key.Value*=-.002f; key.InTangent*=-.002f; key.OutTangent*=-.002f; }
    CameraShakeState a,b,ads;
    a.Reset(); b.Reset(); ads.Reset();
    auto zero=a.Update(.01f);
    CHECK(glm::length(zero.Position)==0 && std::abs(zero.Rotation.w-1)<1e-6f);
    asset.AdsScale=0;
    a.Trigger(asset,false); b.Trigger(asset,false); ads.Trigger(asset,true);
    float motion=0;
    for(int i=0;i<10;++i) {
        const auto x=a.Update(.01f),y=b.Update(.01f),z=ads.Update(.01f);
        CHECK(glm::length(x.Position-y.Position)<1e-8f && std::abs(glm::dot(x.Rotation,y.Rotation))>.999999f);
        const float phase=(i+1)*.01f/asset.Duration;
        CHECK(std::abs(x.Position.x-asset.Position[0].Evaluate(phase))<1e-7f && x.Position.y==0 && x.Position.z==0);
        const auto expected=glm::angleAxis(glm::radians(asset.Rotation[0].Evaluate(phase)),glm::vec3(1,0,0));
        CHECK(std::abs(glm::dot(x.Rotation,expected))>.999999f);
        CHECK(glm::length(z.Position)==0 && std::abs(z.Rotation.w-1)<1e-6f);
        motion+=glm::length(x.Position)+glm::length(glm::vec3(x.Rotation.x,x.Rotation.y,x.Rotation.z));
    }
    CHECK(motion>.001f);
    a.Trigger(asset,false);
    const auto ended=a.Update(10);
    CHECK(glm::length(ended.Position)==0 && std::abs(ended.Rotation.w-1)<1e-6f);
    a.Trigger(asset,false); a.Reset();
    CHECK(glm::length(a.Update(.01f).Position)==0);

    // The constrained head rotates a child camera's orientation AND its eye offset.
    IK::Pose rest(2),posed(2);
    rest[0].T={0,1.6f,0}; rest[1].T={0,0,-.1f}; posed=rest;
    posed[0].R=glm::angleAxis(glm::radians(12.0f),glm::vec3(1,0,0));
    std::vector<glm::mat4> r,p;
    IK::ComputeGlobals(rest,{-1,0},r); IK::ComputeGlobals(posed,{-1,0},p);
    const auto effect=ActionCameraDelta(r[1],p[1],glm::quat(1,0,0,0),1,1,1);
    CHECK(glm::length(effect.Position-(glm::vec3(p[1][3])-glm::vec3(r[1][3])))<1e-6f);
    Camera camera; camera.Yaw=33; camera.Pitch=25; camera.Roll=7;
    const auto position=camera.Position;
    const glm::mat3 basis(camera.Right(),camera.Up(),-camera.Front());
    const glm::vec3 expectedForward=basis*(effect.Rotation*glm::vec3(0,0,-1));
    ApplyCameraEffect(camera,effect);
    CHECK(glm::length(camera.Front()-expectedForward)<1e-5f);
    CHECK(glm::length(camera.Position-position-basis*effect.Position)<1e-6f);
    const auto neutral=ActionCameraDelta(r[1],r[1],glm::quat(1,0,0,0),1,1,1);
    CHECK(glm::length(neutral.Position)==0 && std::abs(neutral.Rotation.w-1)<1e-6f);
    Camera identityCamera; identityCamera.Yaw=123; identityCamera.Pitch=-60; identityCamera.Roll=17;
    const auto oldFront=identityCamera.Front(),oldUp=identityCamera.Up();
    ApplyCameraEffect(identityCamera,neutral);
    CHECK(glm::length(identityCamera.Front()-oldFront)<1e-5f && glm::length(identityCamera.Up()-oldUp)<1e-5f);
    // Removing exactly the visible offset restores input before the next mouse-look frame.
    const auto before=identityCamera;
    const glm::vec3 oldAngles(before.Pitch,before.Yaw,before.Roll);
    ApplyCameraEffect(identityCamera,effect);
    const glm::vec3 angles=glm::vec3(identityCamera.Pitch,identityCamera.Yaw,identityCamera.Roll)-oldAngles;
    const auto moved=identityCamera.Position-before.Position;
    identityCamera.Pitch-=angles.x; identityCamera.Yaw-=angles.y; identityCamera.Roll-=angles.z;
    identityCamera.Position-=moved;
    CHECK(glm::length(identityCamera.Front()-before.Front())<1e-5f && glm::length(identityCamera.Position-before.Position)<1e-6f);

    FirstPersonAnimationSet set;
    set.ArmsModel="arms.fbx"; set.WeaponModel="weapon.fbx"; set.Controller="weapon.controller";
    set.ActionCamera.Enabled=true; set.ActionCamera.Node="camera_anim"; set.ActionCamera.PositionScale=1;
    set.ActionCamera.States={"Draw","Melee"}; set.CameraShakeProfile="test.camerashake";
    FirstPersonAnimationSet read;
    CHECK(FirstPersonAnimationSet::FromJsonString(set.ToJsonString(),read,&error));
    CHECK(read.ActionCamera.Enabled && read.ActionCamera.Node=="camera_anim" && read.ActionCamera.States==set.ActionCamera.States);
    CHECK(read.ActionCamera.PositionScale==1 && read.CameraShakeProfile==set.CameraShakeProfile);
    CHECK(CameraShakeAsset::LoadFile(ProjectPaths::Resolve("assets/Weapons/AKS74U/AKS74U.camerashake"),parsed,&error));
    CHECK(CameraShakeAsset::LoadFile(ProjectPaths::Resolve("assets/Weapons/Remington870/Remington870.camerashake"),parsed,&error));
}

void RegisterAnimationTests(UnitTestSupport::TestList& tests) {

    tests.push_back({"Action camera and firing shake",TestCameraEffects});
    tests.push_back({"ADS reload preserves additive locomotion",TestAdsReloadPreservesAdditiveLocomotion});
    tests.push_back({"FirstPersonBody NPC tunables save/load round-trip", TestFirstPersonBodyNpcTunablesRoundTrip});
    tests.push_back({"NPC turn threshold affects turning", TestNpcTurnThresholdAffectsTurning});
    tests.push_back({"NPC spine twist clamping", TestNpcSpineTwistClamping});
    tests.push_back({"Foot tilt limiting", TestFootTiltLimiting});
    tests.push_back({"Two-bone hint defaults keep the old solve", TestTwoBoneHintDefaultsKeepOldSolve});
    tests.push_back({"Two-bone pole hint stops the elbow flip", TestTwoBoneHintStopsElbowFlip});
    tests.push_back({"Two-bone max limb scale caps the reach", TestTwoBoneMaxLimbScale});
    tests.push_back({"Spine distribution defaults to the even spread", TestSpineDistributionDefaultsEven});
    tests.push_back({"Limb grip offset moves the hand", TestLimbGripOffsetMovesHand});
    tests.push_back({"Weapon IK hand offsets round-trip", TestWeaponIkHandOffsetsRoundTrip});
    tests.push_back({"Clip weight curves sample and scale the IK layers", TestClipWeightCurveSamplingAndLayerScaling});
    tests.push_back({"Clip IK curve and the IKOff tag combine", TestClipCurveAndIKOffTagCombine});
    tests.push_back({"Free-aim dead zone", TestFreeAimDeadZoneMath});
    tests.push_back({"ADS blend pieces: additive, crouch, camera share", TestAdsBlendPieces});
    tests.push_back({"Arm-shape links match the per-frame name walk", TestArmShapeLinksMatchNameWalk});
    tests.push_back({"Controller clip files for the editor prefetch", TestControllerClipFilesForPrefetch});
    tests.push_back({"Stride scale and pelvis drop math", TestStrideScaleMath});
    tests.push_back({"Foot slide correction defaults off (identical pose)", TestFootSlideDefaultsOff});
    tests.push_back({"Foot pin holds planted feet (slide cm per plant)", TestFootSlidePinHoldsPlantedFeet});
    tests.push_back({"Stride warp matches the ground speed within its limits", TestStrideWarpMatchesGroundSpeed});
}
