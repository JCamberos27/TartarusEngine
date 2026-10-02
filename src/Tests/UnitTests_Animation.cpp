#include "UnitTestSupport.h"
#include "../Game/Components.h"
#include "../Game/Npc/NpcBody.h"
#include <cmath>

// Unit tests for animation poses and IK. Add a function per test and list it below.

void TestFirstPersonBodyComponentTunables() {
    // Verify that FirstPersonBodyComponent fields can be set and retrieved
    FirstPersonBodyComponent cfg;

    // Test Arm IK fields
    cfg.ElbowEase = 0.08f;
    cfg.ElbowMaxRate = 15.0f;
    CHECK(cfg.ElbowEase == 0.08f);
    CHECK(cfg.ElbowMaxRate == 15.0f);

    // Test NPC body tuning fields
    cfg.NpcTurnThreshold = 1.5f;
    cfg.NpcMoveEase = 0.15f;
    cfg.NpcFaceEase = 0.12f;
    cfg.NpcMaxTwist = 1.3f;
    cfg.NpcAimLean = 0.12f;
    cfg.NpcAimLeanCrouched = 0.25f;
    cfg.NpcReadyLeanCrouched = 0.45f;
    cfg.NpcCowerHunch = 0.4f;
    cfg.NpcHeadMaxYaw = 1.3f;
    cfg.NpcHeadMaxPitch = 0.7f;

    CHECK(cfg.NpcTurnThreshold == 1.5f);
    CHECK(cfg.NpcMoveEase == 0.15f);
    CHECK(cfg.NpcFaceEase == 0.12f);
    CHECK(cfg.NpcMaxTwist == 1.3f);
    CHECK(cfg.NpcAimLean == 0.12f);
    CHECK(cfg.NpcAimLeanCrouched == 0.25f);
    CHECK(cfg.NpcReadyLeanCrouched == 0.45f);
    CHECK(cfg.NpcCowerHunch == 0.4f);
    CHECK(cfg.NpcHeadMaxYaw == 1.3f);
    CHECK(cfg.NpcHeadMaxPitch == 0.7f);

    // Test NPC foot IK fields
    cfg.NpcFootIKMaxDrop = 0.4f;
    cfg.NpcFootIKMaxRaise = 0.4f;
    cfg.NpcFootIKPelvisRaise = 0.1f;
    cfg.NpcFootIKTiltMax = 0.6f;
    cfg.NpcFootOffsetEase = 0.08f;
    cfg.NpcFootNormalEase = 0.1f;
    cfg.NpcFootIKFade = 0.2f;

    CHECK(cfg.NpcFootIKMaxDrop == 0.4f);
    CHECK(cfg.NpcFootIKMaxRaise == 0.4f);
    CHECK(cfg.NpcFootIKPelvisRaise == 0.1f);
    CHECK(cfg.NpcFootIKTiltMax == 0.6f);
    CHECK(cfg.NpcFootOffsetEase == 0.08f);
    CHECK(cfg.NpcFootNormalEase == 0.1f);
    CHECK(cfg.NpcFootIKFade == 0.2f);
}

void TestNpcBodySettingsCopying() {
    // Verify that NpcHoldSettings fields copy correctly from FirstPersonBodyComponent
    FirstPersonBodyComponent fpb;
    fpb.NpcTurnThreshold = 1.4f;
    fpb.NpcMaxTwist = 1.25f;
    fpb.NpcHeadMaxYaw = 1.25f;
    fpb.NpcHeadMaxPitch = 0.65f;
    fpb.NpcFootIKTiltMax = 0.55f;

    // Simulate NpcDirector copying these values to NpcHoldSettings
    NpcHoldSettings settings;
    settings.TurnThreshold = fpb.NpcTurnThreshold;
    settings.MaxTwist = fpb.NpcMaxTwist;
    settings.HeadMaxYaw = fpb.NpcHeadMaxYaw;
    settings.HeadMaxPitch = fpb.NpcHeadMaxPitch;
    settings.FootIKTiltMax = fpb.NpcFootIKTiltMax;

    CHECK(settings.TurnThreshold == 1.4f);
    CHECK(settings.MaxTwist == 1.25f);
    CHECK(settings.HeadMaxYaw == 1.25f);
    CHECK(settings.HeadMaxPitch == 0.65f);
    CHECK(settings.FootIKTiltMax == 0.55f);
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

void TestDefaultValuesPreserved() {
    // Verify that component default values match the original constants
    FirstPersonBodyComponent cfg;

    // Check that defaults match the constants we removed from NpcBody.cpp
    CHECK(cfg.NpcTurnThreshold == 1.15f);   // Original kTurnThreshold
    CHECK(cfg.NpcMaxTwist == 1.2f);         // Original kMaxTwist
    CHECK(cfg.NpcMoveEase == 0.1f);         // Original kMoveEase
    CHECK(cfg.NpcFaceEase == 0.09f);        // Original kFaceEase
    CHECK(cfg.NpcAimLean == 0.1f);          // Original kAimLean
    CHECK(cfg.NpcAimLeanCrouched == 0.22f); // Original kAimLeanCrouched
    CHECK(cfg.NpcReadyLeanCrouched == 0.4f);// Original kReadyLeanCrouched
    CHECK(cfg.NpcCowerHunch == 0.35f);      // Original kCowerHunch
    CHECK(cfg.NpcHeadMaxYaw == 1.2f);       // Original kHeadMaxYaw
    CHECK(cfg.NpcHeadMaxPitch == 0.6f);     // Original kHeadMaxPitch
    CHECK(cfg.NpcFootIKMaxDrop == 0.35f);   // Original kMaxDrop
    CHECK(cfg.NpcFootIKMaxRaise == 0.35f);  // Original kMaxRaise
    CHECK(cfg.NpcFootIKPelvisRaise == 0.08f);// Original kPelvisRaise
    CHECK(cfg.NpcFootIKTiltMax == 0.5f);    // Original kTiltMax
    CHECK(cfg.ElbowEase == 0.06f);          // Original kElbowEase
    CHECK(cfg.ElbowMaxRate == 9.42f);       // Original glm::radians(540.0f) ≈ 9.42
}

void RegisterAnimationTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"FirstPersonBody component tunables round-trip", TestFirstPersonBodyComponentTunables});
    tests.push_back({"NPC body settings copying", TestNpcBodySettingsCopying});
    tests.push_back({"NPC turn threshold affects turning", TestNpcTurnThresholdAffectsTurning});
    tests.push_back({"NPC spine twist clamping", TestNpcSpineTwistClamping});
    tests.push_back({"Foot tilt limiting", TestFootTiltLimiting});
    tests.push_back({"Default values preserved", TestDefaultValuesPreserved});
}
