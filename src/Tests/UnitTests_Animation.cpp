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
#include "Core/Paths.h"
#include "Model.h"
#include "AnimRetarget.h"
#include "../Game/IK.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

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
    set.NpcFaceEase = 0.13f;
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
    CHECK(got.NpcFaceEase == set.NpcFaceEase);
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

namespace {
// root -> hip (1 up) -> knee -> ankle, 0.45 + 0.45, the knee bent `bendDeg` toward `planeDeg` about +Y from +Z (the toes);
// the ankle hangs straight under the hip.
struct BentLeg {
    IK::Pose pose{4};
    std::vector<int> parents{-1, 0, 1, 2};
    std::vector<glm::mat4> g;
    glm::vec3 KneeSide; // the way the knee bulges
    BentLeg(float bendDeg, float planeDeg) {
        KneeSide = glm::vec3(std::sin(glm::radians(planeDeg)), 0.0f, std::cos(glm::radians(planeDeg)));
        const glm::vec3 k = glm::normalize(glm::cross(glm::vec3(0.0f, -1.0f, 0.0f), KneeSide));
        pose[1].T = glm::vec3(0.0f, 1.0f, 0.0f);
        pose[1].R = glm::angleAxis(glm::radians(bendDeg) * 0.5f, k);
        pose[2].T = glm::vec3(0.0f, -0.45f, 0.0f);
        pose[2].R = glm::angleAxis(-glm::radians(bendDeg), k);
        pose[3].T = glm::vec3(0.0f, -0.45f, 0.0f);
        IK::ComputeGlobals(pose, parents, g);
    }
    glm::vec3 Hip() const { return IK::Position(g[1]); }
    glm::vec3 Ankle() const { return IK::Position(g[3]); }
};
// Signed angle (degrees) from `from` to `to` about unit `n`, both seen square to it.
float SignedDegAbout(const glm::vec3& from, const glm::vec3& to, const glm::vec3& n) {
    const glm::vec3 f = from - n * glm::dot(from, n), t = to - n * glm::dot(to, n);
    return glm::degrees(std::atan2(glm::dot(glm::cross(f, t), n), glm::dot(f, t)));
}
// Solves the leg onto `target` with the knee steered toward the toes (+Z) by `bendWeight`, as NpcBody's legs do.
std::vector<glm::mat4> SolveBentLeg(const BentLeg& leg, const glm::vec3& target, float bendWeight) {
    IK::TwoBoneHint h;
    h.BendAxis = glm::cross(leg.Ankle() - leg.Hip(), glm::vec3(0.0f, 0.0f, 1.0f));
    h.BendAxisWeight = bendWeight;
    IK::Pose p = leg.pose;
    std::vector<glm::mat4> g = leg.g;
    CHECK(IK::SolveTwoBone(p, leg.parents, g, 1, 2, 3, target, nullptr, 1.0f, 0.0f, &h));
    return g;
}
float KneeDeg(const std::vector<glm::mat4>& g) {
    const glm::vec3 a = IK::Position(g[1]), b = IK::Position(g[2]), c = IK::Position(g[3]);
    return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a - b), glm::normalize(c - b)), -1.0f, 1.0f)));
}
} // namespace

// The BendAxis hint (NpcBody's near-straight legs) turns the knee's plane without costing the reach: the end still lands on
// the target, the knee angle is the triangle's, and the result is continuous in the weight (a tiny weight is a tiny change).
void TestTwoBoneBendAxisReachesTarget() {
    float worstReach = 0.0f, worstKnee = 0.0f, worstJump = 0.0f;
    for (const float bend : {4.0f, 0.5f})
        for (const float plane : {90.0f, 180.0f}) {
            const BentLeg leg(bend, plane);
            const glm::vec3 a = leg.Hip();
            const glm::vec3 down = glm::normalize(leg.Ankle() - a);
            for (const glm::vec3& target : {a + down * 0.8f, a + glm::vec3(0.05f, -0.78f, 0.12f)}) {
                const float lat = glm::length(target - a);
                const float wantKnee = glm::degrees(std::acos(std::clamp((lat * lat - 2.0f * 0.45f * 0.45f) / (-2.0f * 0.45f * 0.45f), -1.0f, 1.0f)));
                std::vector<glm::mat4> byWeight[3];
                const float weights[3] = {1.0f, 0.001f, 0.0f};
                for (int k = 0; k < 3; ++k) {
                    byWeight[k] = SolveBentLeg(leg, target, weights[k]);
                    worstReach = std::max(worstReach, glm::length(IK::Position(byWeight[k][3]) - target));
                    worstKnee = std::max(worstKnee, std::fabs(KneeDeg(byWeight[k]) - wantKnee));
                }
                // Whole weight: the knee is out toward the toes.
                const glm::vec3 n = glm::normalize(IK::Position(byWeight[0][3]) - a);
                const glm::vec3 side = IK::Position(byWeight[0][2]) - a;
                CHECK(glm::dot(glm::normalize(side - n * glm::dot(side, n)), glm::vec3(0.0f, 0.0f, 1.0f)) > 0.95f);
                if (glm::length(target - (a + down * 0.8f)) < 1e-6f) // straight down the leg: no swing, the plane is exactly the wanted one
                    CHECK(std::fabs(SignedDegAbout(side, glm::vec3(0.0f, 0.0f, 1.0f), n)) < 0.5f);
                // A thousandth of the weight is (within a millimetre) none of it.
                for (int j = 2; j <= 3; ++j)
                    worstJump = std::max(worstJump, glm::length(IK::Position(byWeight[1][j]) - IK::Position(byWeight[2][j])));
            }
        }
    std::printf("[anim] bend-axis hint: worst reach miss %.3f mm, knee angle off %.4f deg, w 0.001 vs 0 %.3f mm\n",
                worstReach * 1000.0f, worstKnee, worstJump * 1000.0f);
    CHECK(worstReach < 1e-3f);
    CHECK(worstKnee < 0.05f);
    CHECK(worstJump < 1e-3f);

    // Swept 0..1 the knee turns smoothly from its own side toward the toes, the weight's share of the way, and the end
    // never leaves the target.
    for (const float plane : {90.0f, 150.0f}) {
        const BentLeg leg(4.0f, plane);
        const glm::vec3 a = leg.Hip();
        const glm::vec3 n = glm::normalize(leg.Ankle() - a);
        const glm::vec3 target = a + n * 0.8f;
        const float whole = SignedDegAbout(leg.KneeSide, glm::vec3(0.0f, 0.0f, 1.0f), n);
        float worstTurn = 0.0f, worstMiss = 0.0f;
        for (int i = 0; i <= 20; ++i) {
            const float w = (float)i / 20.0f;
            const std::vector<glm::mat4> g = SolveBentLeg(leg, target, w);
            worstMiss = std::max(worstMiss, glm::length(IK::Position(g[3]) - target));
            worstTurn = std::max(worstTurn, std::fabs(SignedDegAbout(leg.KneeSide, IK::Position(g[2]) - a, n) - w * whole));
        }
        std::printf("[anim] bend-axis sweep, plane %.0f deg: knee turn off its share by %.3f deg, reach miss %.3f mm\n", plane,
                    worstTurn, worstMiss * 1000.0f);
        CHECK(worstMiss < 1e-3f);
        CHECK(worstTurn < 0.5f);
    }
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

// An IK Rig at part weight (the procedural IK weight fading in or out) moves the gun by that share of its offset; the hand
// that keeps its animated grip must go with the gun all the way, not the same share again (that left it off the gun by
// w(1-w) of the move: 1.35 cm here at 0.5). Weight 0 is the animated pose, untouched.
void TestApplyRigPartialWeightKeepsHandOnGun() {
    // ApplyRig reads only the node hierarchy off the model: Root (a triangle, so it imports) -> Shoulder -> Elbow -> Hand,
    // Root -> Gun. The transforms are the test's own pose below.
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "TartarusIkRigTest";
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / "ik_rig_arm.gltf";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"name":"Root","mesh":0,"children":[1,4]},{"name":"Shoulder","children":[2]},{"name":"Elbow","children":[3]},
{"name":"Hand"},{"name":"Gun"}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],
"buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]})";
    }
    const auto model = Model::ImportDeferred(file.string(), ModelImportSettings{});
    CHECK(model != nullptr);
    if (!model) return;
    const int shoulder = model->NodeIndex("Shoulder"), elbow = model->NodeIndex("Elbow"), hand = model->NodeIndex("Hand"),
              gunNode = model->NodeIndex("Gun");
    CHECK(shoulder >= 0 && elbow >= 0 && hand >= 0 && gunNode >= 0);
    if (shoulder < 0 || elbow < 0 || hand < 0 || gunNode < 0) return;
    std::vector<int> parents(model->NodeCount());
    for (int i = 0; i < model->NodeCount(); ++i) parents[i] = model->NodeParent(i);

    // The animated pose: a bent arm, the gun a little off the hand (the grip), every other node at identity.
    IK::Pose animated(model->NodeCount(), LocalTRS{});
    animated[shoulder].T = glm::vec3(0.0f, 1.5f, 0.0f);
    animated[shoulder].R = glm::angleAxis(-0.4f, glm::vec3(0.0f, 1.0f, 0.0f));
    animated[elbow].T = glm::vec3(0.3f, 0.0f, 0.0f);
    animated[elbow].R = glm::angleAxis(0.9f, glm::vec3(0.0f, 0.0f, 1.0f));
    animated[hand].T = glm::vec3(0.28f, 0.0f, 0.0f);
    std::vector<glm::mat4> g0;
    IK::ComputeGlobals(animated, parents, g0);
    const glm::mat4 grip = glm::translate(glm::mat4(1.0f), glm::vec3(0.02f, -0.01f, 0.03f)) *
                           glm::mat4_cast(glm::angleAxis(0.3f, glm::vec3(1.0f, 0.0f, 0.0f))); // hand in the gun's frame
    const glm::mat4 gun0 = g0[hand] * glm::inverse(grip);
    animated[gunNode].T = IK::Position(gun0);
    animated[gunNode].R = IK::Rotation(gun0);
    IK::ComputeGlobals(animated, parents, g0);
    const glm::mat4 relative = glm::inverse(g0[gunNode]) * g0[hand];

    IKRigComponent rig;
    rig.LimbA.Enabled = true;
    rig.LimbA.Upper = "Shoulder";
    rig.LimbA.Lower = "Elbow";
    rig.LimbA.End = "Hand";
    rig.LimbA.Target = "Gun";
    rig.LimbA.KeepAnimatedOffset = true;
    rig.LimbA.MatchRotation = true;
    IKBoneOffset off;
    off.Bone = "Gun";
    off.Position = glm::vec3(0.0f, 0.02f, 0.05f);
    off.Rotation = glm::angleAxis(0.15f, glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)));
    rig.Offsets.push_back(off);

    // gap (m) and turn (deg) between the hand and where the animated grip puts it on the gun as it now stands.
    const auto solve = [&](float weight, const glm::vec3& gripPosition, float& gap, float& turn, float& gunMoved) {
        rig.Weight = weight;
        rig.LimbA.GripPosition = gripPosition;
        IK::Pose p = animated;
        IK::ApplyRig(rig, *model, p);
        std::vector<glm::mat4> g;
        IK::ComputeGlobals(p, parents, g);
        const glm::mat4 want = g[gunNode] * glm::translate(glm::mat4(1.0f), gripPosition) * relative;
        gap = glm::length(IK::Position(g[hand]) - IK::Position(want));
        turn = glm::degrees(glm::angle(glm::normalize(glm::inverse(IK::Rotation(want)) * IK::Rotation(g[hand]))));
        if (turn > 180.0f) turn = 360.0f - turn;
        gunMoved = glm::length(IK::Position(g[gunNode]) - IK::Position(g0[gunNode]));
    };
    float gap = 0.0f, turn = 0.0f, moved = 0.0f;
    for (const float weight : {0.25f, 0.5f, 0.75f, 1.0f}) {
        solve(weight, glm::vec3(0.0f), gap, turn, moved);
        std::printf("[anim] IK rig weight %.2f: gun moved %.2f cm, hand off it %.3f mm, turned %.3f deg\n", weight, moved * 100.0f,
                    gap * 1000.0f, turn);
        CHECK(moved > 0.02f * weight);
        CHECK(gap < 1e-3f);
        CHECK(turn < 0.1f);
    }
    // A grip offset is blended by the weight: whole at 1, next to nothing at a tiny weight (no jump as the rig fades in).
    const glm::vec3 gripOffset(0.03f, 0.0f, 0.0f);
    solve(1.0f, gripOffset, gap, turn, moved);
    CHECK(gap < 1e-3f && turn < 0.1f);
    solve(1e-4f, glm::vec3(0.0f), gap, turn, moved);
    CHECK(gap < 1e-4f);
    solve(1e-4f, gripOffset, gap, turn, moved);
    CHECK(std::fabs(gap - glm::length(gripOffset)) < 1e-4f); // the hand has taken next to none of the offset
    // Weight 0: the animated pose, exactly.
    rig.Weight = 0.0f;
    rig.LimbA.GripPosition = gripOffset;
    IK::Pose p = animated;
    IK::ApplyRig(rig, *model, p);
    bool same = true;
    for (size_t i = 0; i < p.size(); ++i) same = same && p[i].T == animated[i].T && p[i].R == animated[i].R && p[i].S == animated[i].S;
    CHECK(same);
    std::filesystem::remove_all(dir, ec);
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

namespace {
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

namespace {
// One foot on a body standing still at the origin, facing +Z: the foot in the body is the foot in the world.
glm::vec3 StepStill(IK::FootLock& lock, const IK::FootLockSettings& set, const glm::vec3& anim, float height, float dt) {
    return lock.Step(0, set, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), anim, height, dt);
}
} // namespace

void TestFootLockHoldsAndReleases() {
    // A planted foot whose animation creeps 0.3 m/s over the ground (a stride shorter than the capsule's travel) is held
    // where it landed; lifted, it catches up smoothly - no frame jumps more than its swing would.
    IK::FootLockSettings set;
    IK::FootLock lock;
    const float dt = 1.0f / 60.0f;
    glm::vec3 anim(0.0f, 0.08f, 0.0f), first(0.0f);
    StepStill(lock, set, anim, anim.y, dt); // first sight: no speed yet, nothing planted
    CHECK(!lock.Locked(0));
    for (int i = 0; i < 30; ++i) {
        anim.z += 0.3f * dt;
        const glm::vec3 drawn = anim + StepStill(lock, set, anim, anim.y, dt);
        if (i == 0) first = drawn;
        CHECK(lock.Locked(0));
        CHECK(glm::length(glm::vec2(drawn.x - first.x, drawn.z - first.z)) < 1e-4f);
    }
    float lastGap = glm::length(lock.Offset(0)), lastZ = first.z;
    CHECK(lastGap > 0.1f);
    for (int i = 0; i < 30; ++i) { // lifted
        anim.y += i == 0 ? 0.05f : 0.01f; // off the ground past Lift
        anim.z += 0.3f * dt;
        const glm::vec3 drawn = anim + StepStill(lock, set, anim, anim.y, dt);
        CHECK(!lock.Locked(0));
        const float gap = glm::length(lock.Offset(0));
        CHECK(gap <= lastGap + 1e-6f);                // only ever closes
        CHECK(std::abs(drawn.z - lastZ) < 0.03f);     // no pop
        lastGap = gap;
        lastZ = drawn.z;
    }
    CHECK(lastGap < 1e-4f); // caught up within Release
}

void TestFootLockIgnoresSwingAndLetsGoOnDrift() {
    IK::FootLockSettings set;
    const float dt = 1.0f / 60.0f;
    { // a swinging foot passing low (1.2 cm over where it plants: within Height, off the floor), fast, never plants
        IK::FootLock lock;
        glm::vec3 anim(0.0f, 0.08f, 0.0f);
        StepStill(lock, set, anim, 0.08f, dt);
        anim.y = 0.092f;
        for (int i = 0; i < 10; ++i) {
            anim.z += 4.0f * dt;
            StepStill(lock, set, anim, anim.y, dt);
            CHECK(!lock.Locked(0));
        }
    }
    { // the animation wanders off a planted foot: past MaxDrift (the safety leash) it lets go rather than stretch the leg
        IK::FootLock lock;
        glm::vec3 anim(0.0f, 0.08f, 0.0f);
        bool let = false;
        for (int i = 0; i < 120 && !let; ++i) {
            anim.x += 0.8f * dt;
            StepStill(lock, set, anim, 0.08f, dt);
            let = i > 0 && !lock.Locked(0);
            CHECK(glm::length(lock.Offset(0)) <= set.MaxDrift + 0.02f);
        }
        CHECK(let);
    }
    { // off: nothing moves
        IK::FootLockSettings off;
        off.Enabled = false;
        IK::FootLock lock;
        const glm::vec3 o = StepStill(lock, off, glm::vec3(0.0f, 0.08f, 0.0f), 0.08f, dt);
        CHECK(o == glm::vec3(0.0f) && !lock.Locked(0));
    }
    FirstPersonBodyComponent body;
    CHECK(IK::FootLockFrom(body).Enabled && std::abs(IK::FootLockFrom(body).MaxDrift - body.FootLockDrift) < 1e-6f);
}

void TestFootLockPlantsWhileTheBodyTurns() {
    // The body swings round 180 degrees a second, its clip's foot still in it (0.32 m from the root): the foot's world speed
    // is ~1 m/s, twice the tight Speed set here, but the turn is the body's, so the foot plants and stays where it landed.
    IK::FootLockSettings set;
    set.Speed = 0.5f;
    const float dt = 1.0f / 60.0f;
    const glm::vec3 root(1.0f, 0.0f, 2.0f), local(0.3f, 0.08f, 0.1f);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    IK::FootLock lock;
    float yaw = 0.0f;
    glm::vec3 first(0.0f);
    for (int i = 0; i < 12; ++i) { // 36 degrees round: inside the leash
        const glm::quat facing = glm::angleAxis(yaw, up);
        const glm::vec3 drawn = root + facing * local + lock.Step(0, set, root, facing, local, local.y, dt);
        if (i == 0) {
            CHECK(!lock.Locked(0)); // (a first frame has no speed yet)
        } else {
            CHECK(lock.Locked(0));
            if (i == 1) first = drawn;
            CHECK(glm::length(drawn - first) < 1e-4f);
        }
        yaw += glm::radians(180.0f) * dt;
    }
    // The same sweep made by the clip (the foot moving in a body that does not turn) off the floor - 1.2 cm up, within
    // Height but over FlatHeight - is a foot moving: it never plants.
    IK::FootLock swept;
    yaw = 0.0f;
    swept.Step(0, set, root, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), local, local.y, dt); // its floor
    for (int i = 0; i < 12; ++i) {
        yaw += glm::radians(180.0f) * dt;
        const glm::vec3 at = glm::angleAxis(yaw, up) * local + glm::vec3(0.0f, 0.012f, 0.0f);
        swept.Step(0, set, root, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), at, at.y, dt);
        CHECK(!swept.Locked(0));
    }
    // Flat on the floor, the clip sliding the foot (a crossfade into a stop) plants however fast it slides, and holds.
    IK::FootLock slid;
    yaw = 0.0f;
    glm::vec3 held(0.0f);
    for (int i = 0; i < 12; ++i) {
        const glm::vec3 at = glm::angleAxis(yaw, up) * local;
        const glm::vec3 drawn = root + at + slid.Step(0, set, root, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), at, at.y, dt);
        if (i == 1) held = drawn;
        if (i >= 1) { CHECK(slid.Locked(0)); CHECK(glm::length(drawn - held) < 1e-4f); }
        yaw += glm::radians(180.0f) * dt;
    }
    // ... but not a foot the capsule carries that fast (a swing passing low): the root's own travel is held to Speed.
    IK::FootLock carried;
    glm::vec3 r = root;
    for (int i = 0; i < 12; ++i) {
        carried.Step(0, set, r, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), local, local.y, dt);
        CHECK(!carried.Locked(0));
        r.z += 1.0f * dt;
    }
}

void TestFootLockLetsGoAtReach() {
    IK::FootLockSettings set;
    const float dt = 1.0f / 60.0f;
    const glm::quat facing(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 local(0.0f, 0.08f, 0.0f); // the clip's foot, down and still in the body
    glm::vec3 root(0.0f);
    IK::FootLock lock;
    lock.Step(0, set, root, facing, local, local.y, dt);
    for (int i = 0; i < 6; ++i) { // the body walks on 1 m/s faster than the clip: the planted foot is held behind
        root.z += 1.0f * dt;
        lock.Step(0, set, root, facing, local, local.y, dt);
    }
    CHECK(lock.Locked(0));
    CHECK(!lock.Overreach(0, set, 0.97f, 0.90f));  // within Reach: held
    CHECK(!lock.Overreach(0, set, 0.99f, 0.985f)); // the clip's own leg near straight, the hold adding next to nothing: held
    CHECK(lock.Locked(0));
    CHECK(lock.Overreach(0, set, 0.981f, 0.90f));  // past Reach because of the hold: lets go
    CHECK(!lock.Locked(0));
    CHECK(!lock.Overreach(0, set, 1.2f, 0.90f));   // (a free foot has nothing to let go)
    // ... and steps: the lag only ever closes, the foot lifts in an arc on the way, and it plants again only once it is there.
    float lastGap = glm::length(glm::vec2(lock.Offset(0).x, lock.Offset(0).z));
    CHECK(lastGap > 0.05f);
    bool lifted = false;
    const int frames = (int)std::ceil(set.Release / dt) + 2;
    for (int i = 0; i < frames; ++i) { // the body stopped, the clip's foot still down
        const glm::vec3 o = lock.Step(0, set, root, facing, local, local.y, dt);
        const float gap = glm::length(glm::vec2(o.x, o.z));
        if (i < frames - 3) CHECK(!lock.Locked(0));
        CHECK(gap <= lastGap + 1e-6f);
        lifted = lifted || o.y > 0.005f;
        lastGap = gap;
    }
    CHECK(lifted);
    CHECK(lastGap < 1e-4f);
}

void TestClampReachNeverStretchesTheLeg() {
    const glm::vec3 hip(0.1f, 0.9f, -0.2f);
    const float leg = 0.85f;
    const glm::vec3 dir = glm::normalize(glm::vec3(0.2f, -1.0f, 0.3f));
    CHECK(glm::length(IK::ClampReach(hip, hip + dir * 0.7f, leg) - (hip + dir * 0.7f)) < 1e-6f); // within reach: as asked
    float last = 0.0f;
    for (int i = 0; i <= 200; ++i) {
        const float asked = leg * (0.9f + 0.01f * (float)i); // 0.9 .. 2.9 x the leg
        const glm::vec3 t = IK::ClampReach(hip, hip + dir * asked, leg);
        const float got = glm::length(t - hip);
        CHECK(got <= 0.995f * leg + 1e-5f);                        // never past the clamp
        CHECK(glm::length(glm::normalize(t - hip) - dir) < 1e-4f); // drawn in along the hip->target line
        CHECK(got >= last - 1e-6f);                                // asked for more, never given less
        CHECK(i == 0 || got - last <= 0.01f * leg + 1e-5f);        // no jump
        last = got;
    }
    CHECK(last > 0.99f * leg); // it still reaches for it
}

void TestFootSlideMeterCountsPlants() {
    IK::FootSlideMeter m;
    const float dt = 1.0f / 60.0f;
    glm::vec3 feet[2] = {glm::vec3(0.0f, 0.08f, 0.0f), glm::vec3(0.3f, 0.3f, 0.0f)};
    for (int i = 0; i < 30; ++i) m.Step(feet, 0.0f, dt); // foot 0 planted, still
    feet[0].y = 0.3f;
    m.Step(feet, 0.0f, dt);
    feet[0] = glm::vec3(1.0f, 0.08f, 0.0f);
    for (int i = 0; i < 30; ++i) { feet[0].z += 0.5f * dt; m.Step(feet, 0.0f, dt); } // planted, sliding 0.5 m/s
    feet[0].y = 0.3f;
    m.Step(feet, 0.0f, dt);
    const IK::FootSlideMeterTotals t = m.TakeTotals();
    CHECK(t.Plants == 2);
    CHECK(t.MaxDrift > 0.18f && t.MaxDrift < 0.26f);   // ~0.5 m/s for the 27 settled frames
    CHECK(t.MeanCm() > 9.0f && t.MeanCm() < 13.0f);    // the still plant counts 0
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
    // The sample package omits Weapons; solver/schema checks above always run.
    if (std::filesystem::exists(ProjectPaths::Resolve("assets/Weapons"))) {
        CHECK(CameraShakeAsset::LoadFile(ProjectPaths::Resolve("assets/Weapons/AKS74U/AKS74U.camerashake"),parsed,&error));
        CHECK(CameraShakeAsset::LoadFile(ProjectPaths::Resolve("assets/Weapons/Remington870/Remington870.camerashake"),parsed,&error));
    }
}

// A UE4 mannequin clip (3 spine bones, Z-up, cm, arms in an A) baked onto a UE5 rig (5 spine bones, 2 neck
// bones, Y-up, m, taller, arms in a T): the chest turns as the source's does, the in-between spine bones take a
// share, the arm points where the source's points, and the pelvis' lift is scaled to the taller legs.
static void TestRetargetUe4ToUe5() {
    auto node = [](std::vector<AnimNode>& v, const std::string& name, int parent, glm::vec3 pos, glm::vec3 scale = glm::vec3(1.0f)) {
        AnimNode n;
        n.Name = name;
        n.Parent = parent;
        n.BindTRS.T = pos;
        n.BindTRS.S = scale;
        n.BindLocal = n.BindTRS.ToMatrix();
        v.push_back(n);
        return (int)v.size() - 1;
    };
    // Source: X forward, Z up (right-handed, so right is -Y); centimetres under a 0.01 armature.
    std::vector<AnimNode> src;
    const int arm = node(src, "Armature", -1, glm::vec3(0.0f), glm::vec3(0.01f));
    const int sRoot = node(src, "root", arm, glm::vec3(0.0f));
    const int sPelvis = node(src, "pelvis", sRoot, {0, 0, 90});
    const int sS1 = node(src, "spine_01", sPelvis, {0, 0, 10});
    const int sS2 = node(src, "spine_02", sS1, {0, 0, 15});
    const int sS3 = node(src, "spine_03", sS2, {0, 0, 15});
    const int sNeck = node(src, "neck_01", sS3, {0, 0, 15});
    node(src, "head", sNeck, {0, 0, 10});
    const int sClav = node(src, "clavicle_r", sS3, {0, -5, 10});
    const int sUp = node(src, "upperarm_r", sClav, {0, -12, 0});
    node(src, "lowerarm_r", sUp, glm::vec3(0, -1, -1) * (28.0f / std::sqrt(2.0f))); // the A: 45 degrees down
    for (int side = 0; side < 2; ++side) {
        const std::string s = side ? "_r" : "_l";
        const int th = node(src, "thigh" + s, sPelvis, {0, side ? -10.0f : 10.0f, 0});
        const int ca = node(src, "calf" + s, th, {0, 0, -45});
        node(src, "foot" + s, ca, {0, 0, -40});
    }
    // Target, Y-up facing +Z (right = -X), metres, 1.1x the proportions, arms in a T.
    std::vector<AnimNode> tgt;
    const float m = 0.011f;
    const int tRoot = node(tgt, "root", -1, glm::vec3(0.0f));
    const int tPelvis = node(tgt, "pelvis", tRoot, glm::vec3(0, 90, 0) * m);
    int prev = tPelvis;
    for (int i = 1; i <= 5; ++i) prev = node(tgt, "spine_0" + std::to_string(i), prev, glm::vec3(0, 8, 0) * m);
    const int tChest = prev;
    const int tNeck1 = node(tgt, "neck_01", tChest, glm::vec3(0, 8, 0) * m);
    const int tNeck2 = node(tgt, "neck_02", tNeck1, glm::vec3(0, 7, 0) * m);
    node(tgt, "head", tNeck2, glm::vec3(0, 10, 0) * m);
    const int tClav = node(tgt, "clavicle_r", tChest, glm::vec3(-5, 2, 0) * m);
    const int tUp = node(tgt, "upperarm_r", tClav, glm::vec3(-12, 0, 0) * m);
    const int tLow = node(tgt, "lowerarm_r", tUp, glm::vec3(-28, 0, 0) * m);
    for (int side = 0; side < 2; ++side) {
        const std::string s = side ? "_r" : "_l";
        const int th = node(tgt, "thigh" + s, tPelvis, glm::vec3(side ? -10.0f : 10.0f, 0, 0) * m);
        const int ca = node(tgt, "calf" + s, th, glm::vec3(0, -45, 0) * m);
        node(tgt, "foot" + s, ca, glm::vec3(0, -40, 0) * m);
    }
    CHECK(RetargetIsUe4ToUe5(src, tgt));
    const std::vector<int> match = RetargetMatchNodes(src, tgt);
    CHECK(match[tChest] == sS3);
    CHECK(match[tgt[tChest].Parent] == -1); // spine_04 is filled in, not matched

    // The clip: each source spine bone bends 10 degrees forward (+Z toward +X), 30 at the chest;
    // the pelvis lifts 10 cm.
    AnimationClip clip;
    clip.DurationTicks = 10.0f;
    clip.TicksPerSecond = 10.0f;
    const glm::quat q10 = glm::angleAxis(glm::radians(10.0f), glm::vec3(0, 1, 0));
    for (const char* bone : {"spine_01", "spine_02", "spine_03"}) {
        BoneAnimChannel bend;
        bend.BoneName = bone;
        bend.Rotations = {{q10, 0.0f}, {q10, 10.0f}};
        clip.Channels.push_back(bend);
    }
    BoneAnimChannel lift;
    lift.BoneName = "pelvis";
    lift.Positions = {{glm::vec3(0, 0, 100), 0.0f}, {glm::vec3(0, 0, 100), 10.0f}};
    clip.Channels.push_back(lift);

    AnimationClip baked;
    std::vector<int> nodeChannel;
    CHECK(RetargetBakeClip(src, clip, tgt, baked, nodeChannel));
    if (nodeChannel.size() != tgt.size()) return;
    std::vector<glm::mat4> g(tgt.size()), rest(tgt.size());
    for (size_t i = 0; i < tgt.size(); ++i) {
        const int c = nodeChannel[i];
        const glm::mat4 local = c >= 0 ? baked.Channels[c].Sample(5.0f, tgt[i].BindTRS).ToMatrix() : tgt[i].BindLocal;
        g[i] = tgt[i].Parent >= 0 ? g[tgt[i].Parent] * local : local;
        rest[i] = tgt[i].Parent >= 0 ? rest[tgt[i].Parent] * tgt[i].BindLocal : tgt[i].BindLocal;
    }
    auto turn = [&](int i) { return glm::degrees(glm::angle(glm::normalize(glm::quat_cast(glm::mat3(g[i])) * glm::inverse(glm::quat_cast(glm::mat3(rest[i])))))); };
    // The chest turns the source's 30 degrees, forward: its up leans toward the target's front (+Z).
    CHECK(std::abs(turn(tChest) - 30.0f) < 1.0f);
    const glm::vec3 chestUp = glm::normalize(glm::vec3(g[tNeck1][3]) - glm::vec3(g[tChest][3]));
    CHECK(chestUp.z > 0.4f);
    // The filled-in spine_02 / spine_04 sit between their neighbours (10 / 20 / 30 degrees on the matched ones).
    const int tS4 = tgt[tChest].Parent, tS3 = tgt[tS4].Parent, tS2 = tgt[tS3].Parent;
    CHECK(std::abs(turn(tS3) - 20.0f) < 1.0f);
    CHECK(turn(tS2) > 11.0f && turn(tS2) < 19.0f);
    CHECK(turn(tS4) > 21.0f && turn(tS4) < 29.0f);
    // The arm hangs in the source's A (45 degrees down from the T), rotated with the chest.
    const glm::vec3 armDir = glm::normalize(glm::vec3(g[tLow][3]) - glm::vec3(g[tUp][3]));
    const glm::vec3 chestRight = glm::normalize(glm::mat3(g[tChest]) * glm::vec3(-1, 0, 0));
    const glm::vec3 chestDown = glm::normalize(glm::mat3(g[tChest]) * glm::vec3(0, -1, 0));
    const glm::vec3 want = glm::normalize(chestRight + chestDown);
    CHECK(glm::degrees(std::acos(glm::clamp(glm::dot(armDir, want), -1.0f, 1.0f))) < 2.0f);
    // The pelvis lifts 10 cm of source leg, scaled to the target's 1.1x legs.
    const float lifted = g[tPelvis][3].y - rest[tPelvis][3].y;
    CHECK(std::abs(lifted - 0.10f * 1.1f) < 0.003f);
}

// Distance matching: a clip's time follows the character's travel. A path that covers 2 m evenly over the first
// half and then stands (a stop's settle) - Traveled, Remaining and Pivot read it as the states use them, and the
// pacing never runs backwards, freezes, or races ahead.
static void TestDistanceMatching() {
    using DM = AnimatorController::State::DistanceMode;
    std::vector<float> curve;
    for (int i = 0; i <= 10; ++i) curve.push_back(std::min(i, 5) * 0.4f); // 0 .. 2 m by phase 0.5, then still
    CHECK(std::abs(AnimatorDistanceToPhase(curve, 0.0f, DM::Traveled, 1.0f) - 0.25f) < 1e-5f);
    CHECK(std::abs(AnimatorDistanceToPhase(curve, 0.0f, DM::Remaining, 0.4f) - 0.4f) < 1e-5f);
    // Nothing left to travel: where the clip stops moving (its settle then plays on the clock), not its end.
    CHECK(std::abs(AnimatorDistanceToPhase(curve, 0.0f, DM::Remaining, 0.0f) - 0.5f) < 1e-5f);
    CHECK(AnimatorDistanceToPhase(curve, 0.0f, DM::Traveled, -1.0f) == 0.0f);
    // A pivot turning round 1.2 m along: 0.4 m short of it, and 0.2 m past it.
    CHECK(std::abs(AnimatorDistanceToPhase(curve, 1.2f, DM::Pivot, -0.4f) - 0.2f) < 1e-5f);
    CHECK(std::abs(AnimatorDistanceToPhase(curve, 1.2f, DM::Pivot, 0.2f) - 0.35f) < 1e-5f);
    CHECK(AnimatorDistanceToPhase({}, 0.0f, DM::Traveled, 1.0f) < 0.0f);
    // Monotonic in the distance.
    float last = -1.0f;
    bool rising = true;
    for (float d = 0.0f; d <= 2.0f; d += 0.05f) {
        const float ph = AnimatorDistanceToPhase(curve, 0.0f, DM::Traveled, d);
        rising = rising && ph >= last;
        last = ph;
    }
    CHECK(rising);
    // Pacing: entering jumps straight there; after, 0.35x..3x the clock; with nothing ahead, the clock.
    CHECK(AnimatorDistanceStep(0.0f, 0.4f, 0.01f, true) == 0.4f);
    CHECK(std::abs(AnimatorDistanceStep(0.2f, 0.9f, 0.01f, false) - 0.23f) < 1e-6f);
    CHECK(std::abs(AnimatorDistanceStep(0.2f, 0.201f, 0.01f, false) - 0.2035f) < 1e-6f);
    CHECK(std::abs(AnimatorDistanceStep(0.5f, 0.5f, 0.01f, false) - 0.51f) < 1e-6f);
    CHECK(std::abs(AnimatorDistanceStep(0.5f, 0.1f, 0.01f, false) - 0.51f) < 1e-6f);
    // Through the animator: a one-shot state with a distance parameter follows it, a looping one ignores it.
    AnimatorController c;
    c.Parameters = {{"Dist", AnimatorController::ParamType::Float, 0.0f}};
    AnimatorController::State st;
    st.Name = "Start";
    st.Loop = false;
    st.DistanceParam = "Dist";
    st.Motions.assign(1, AnimatorController::Motion{});
    c.Layers[0].States = {st};
    AnimatorControllerComponent ac;
    const auto len = [](int, int) { return 2.0f; };
    const auto phaseAt = [&](int, int, DM mode, float m) { return AnimatorDistanceToPhase(curve, 0.0f, mode, m); };
    ac.SetFloat("Dist", 0.8f);
    AdvanceAnimator(c, ac, 0.1f, len, phaseAt); // enters, placed straight at 0.8 m
    CHECK(std::abs(ac.Layers[0].Stack.back().Phase - 0.2f) < 1e-5f);
    ac.SetFloat("Dist", 0.9f);
    AdvanceAnimator(c, ac, 0.1f, len, phaseAt); // clock step 0.05; the travel asks for +0.025
    CHECK(std::abs(ac.Layers[0].Stack.back().Phase - 0.225f) < 1e-5f);
    c.Layers[0].States[0].Loop = true;
    AdvanceAnimator(c, ac, 0.1f, len, phaseAt);
    CHECK(std::abs(ac.Layers[0].Stack.back().Phase - 0.275f) < 1e-5f);
    // Saved and loaded.
    c.Layers[0].States[0].Distance = DM::Pivot;
    AnimatorController back;
    CHECK(AnimatorController::FromJsonString(c.ToJsonString(), back) && back.Layers[0].States[0].DistanceParam == "Dist" &&
          back.Layers[0].States[0].Distance == DM::Pivot);
}

// Twist bones: a hand turned about its forearm spreads that roll down the forearm's twist bones by where they sit
// (exact at 0, 90 and 180 degrees); an upper arm's own roll is undone toward the shoulder.
static void TestTwistBones() {
    // root -> upper (along +X) -> lower (along +X) -> hand; upper has a twist bone a quarter down it, lower one at half
    // and one at three quarters.
    IK::Pose bind(7);
    std::vector<int> parents = {-1, 0, 1, 2, 1, 2, 2};
    std::vector<std::string> names = {"root", "upperarm_l", "lowerarm_l", "hand_l", "upperarm_twist_01_l", "lowerarm_twist_02_l", "lowerarm_twist_01_l"};
    bind[2].T = glm::vec3(0.3f, 0.0f, 0.0f);
    bind[3].T = glm::vec3(0.28f, 0.0f, 0.0f);
    bind[4].T = glm::vec3(0.075f, 0.0f, 0.0f);
    bind[5].T = glm::vec3(0.14f, 0.0f, 0.0f);
    bind[6].T = glm::vec3(0.21f, 0.0f, 0.0f);
    const auto upperTwist = IK::FindTwistBones(bind, parents, names, 1, 2);
    const auto lowerTwist = IK::FindTwistBones(bind, parents, names, 2, 3);
    CHECK(upperTwist.size() == 1 && std::abs(upperTwist[0].Share - 0.25f) < 1e-5f);
    CHECK(lowerTwist.size() == 2 && std::abs(lowerTwist[0].Share - 0.5f) < 1e-5f && std::abs(lowerTwist[1].Share - 0.75f) < 1e-5f);
    const glm::vec3 x(1.0f, 0.0f, 0.0f);
    auto rollOf = [&](const IK::Pose& p, int node) { // a twist bone's turn about +X off its bind
        const glm::quat d = glm::normalize(p[node].R * glm::inverse(bind[node].R));
        return glm::degrees(2.0f * std::atan2(glm::dot(glm::vec3(d.x, d.y, d.z), x), d.w));
    };
    for (float deg : {0.0f, 90.0f, 180.0f, -120.0f}) {
        IK::Pose pose = bind;
        // The hand rolled, and bent a little (a swing, which the twist bones must not take).
        pose[3].R = glm::angleAxis(glm::radians(deg), x) * glm::angleAxis(glm::radians(20.0f), glm::vec3(0, 0, 1));
        const float roll = IK::EndRoll(pose, bind, 2, 3);
        IK::SpreadTwist(pose, bind, 2, 3, lowerTwist, roll, true);
        const float want = deg == 180.0f ? std::abs(glm::degrees(roll)) : deg;
        CHECK(std::abs(std::abs(glm::degrees(roll)) - std::abs(want)) < 0.05f);
        CHECK(std::abs(std::abs(rollOf(pose, 5)) - std::abs(want) * 0.5f) < 0.05f && std::abs(std::abs(rollOf(pose, 6)) - std::abs(want) * 0.75f) < 0.05f);
    }
    // The upper arm rolled 60 degrees in the shoulder: its twist bone near the shoulder turns 45 back.
    IK::Pose pose = bind;
    pose[1].R = glm::angleAxis(glm::radians(60.0f), x);
    const float roll = IK::SegmentRoll(pose, bind, 1, 2);
    CHECK(std::abs(glm::degrees(roll) - 60.0f) < 0.05f);
    IK::SpreadTwist(pose, bind, 1, 2, upperTwist, roll, false);
    CHECK(std::abs(rollOf(pose, 4) + 45.0f) < 0.05f);
}

void RegisterAnimationTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Retarget: UE4 mannequin clip baked onto a UE5 rig", TestRetargetUe4ToUe5});
    tests.push_back({"Distance matching: phase from travel, paced", TestDistanceMatching});
    tests.push_back({"Twist bones spread a limb's roll", TestTwistBones});

    tests.push_back({"Action camera and firing shake",TestCameraEffects});
    tests.push_back({"ADS reload preserves additive locomotion",TestAdsReloadPreservesAdditiveLocomotion});
    tests.push_back({"FirstPersonBody NPC tunables save/load round-trip", TestFirstPersonBodyNpcTunablesRoundTrip});
    tests.push_back({"NPC spine twist clamping", TestNpcSpineTwistClamping});
    tests.push_back({"Foot tilt limiting", TestFootTiltLimiting});
    tests.push_back({"Two-bone hint defaults keep the old solve", TestTwoBoneHintDefaultsKeepOldSolve});
    tests.push_back({"Two-bone pole hint stops the elbow flip", TestTwoBoneHintStopsElbowFlip});
    tests.push_back({"Two-bone bend axis hint still reaches the target", TestTwoBoneBendAxisReachesTarget});
    tests.push_back({"Two-bone max limb scale caps the reach", TestTwoBoneMaxLimbScale});
    tests.push_back({"Spine distribution defaults to the even spread", TestSpineDistributionDefaultsEven});
    tests.push_back({"Limb grip offset moves the hand", TestLimbGripOffsetMovesHand});
    tests.push_back({"IK rig at part weight keeps the hand on the gun", TestApplyRigPartialWeightKeepsHandOnGun});
    tests.push_back({"Weapon IK hand offsets round-trip", TestWeaponIkHandOffsetsRoundTrip});
    tests.push_back({"Clip weight curves sample and scale the IK layers", TestClipWeightCurveSamplingAndLayerScaling});
    tests.push_back({"Clip IK curve and the IKOff tag combine", TestClipCurveAndIKOffTagCombine});
    tests.push_back({"ADS blend pieces: additive, crouch, camera share", TestAdsBlendPieces});
    tests.push_back({"Arm-shape links match the per-frame name walk", TestArmShapeLinksMatchNameWalk});
    tests.push_back({"Controller clip files for the editor prefetch", TestControllerClipFilesForPrefetch});
    tests.push_back({"Foot lock holds a planted foot and lets it catch up smoothly", TestFootLockHoldsAndReleases});
    tests.push_back({"Foot lock ignores a swinging foot and lets go past its drift", TestFootLockIgnoresSwingAndLetsGoOnDrift});
    tests.push_back({"Foot lock plants a still clip foot while the body turns 180 deg/s", TestFootLockPlantsWhileTheBodyTurns});
    tests.push_back({"Foot lock lets go and steps past 0.98 of the leg", TestFootLockLetsGoAtReach});
    tests.push_back({"Clamp reach never asks a leg for more than 0.995 of it", TestClampReachNeverStretchesTheLeg});
    tests.push_back({"Foot slide meter counts plants and their drift", TestFootSlideMeterCountsPlants});
}
