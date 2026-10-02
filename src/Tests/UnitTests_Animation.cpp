#include "UnitTestSupport.h"
#include "../Game/Components.h"
#include "../Game/Npc/NpcBody.h"
#include "World.h"
#include "SceneSerializer.h"
#include "AssetLibrary.h"
#include "../Game/FirstPersonProcedural.h"
#include "../Game/AnimatorController.h"
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

void RegisterAnimationTests(UnitTestSupport::TestList& tests) {
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
}
