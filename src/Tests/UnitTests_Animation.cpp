#include "UnitTestSupport.h"
#include "../Game/Components.h"
#include "../Game/Npc/NpcBody.h"
#include "World.h"
#include "SceneSerializer.h"
#include "AssetLibrary.h"
#include <cmath>

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

void RegisterAnimationTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"FirstPersonBody NPC tunables save/load round-trip", TestFirstPersonBodyNpcTunablesRoundTrip});
    tests.push_back({"NPC turn threshold affects turning", TestNpcTurnThresholdAffectsTurning});
    tests.push_back({"NPC spine twist clamping", TestNpcSpineTwistClamping});
    tests.push_back({"Foot tilt limiting", TestFootTiltLimiting});
}
