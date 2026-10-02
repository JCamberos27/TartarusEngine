#include "UnitTestSupport.h"
#include "../Renderer/Model.h"
#include "../Game/GravityGun.h"
#include "../Game/Components.h"

// Unit tests for renderer, core and performance. Add a function per test and list it below.

static void Test_Model_HasBones() {
    // Rigged model should have bones
    auto armsModel = std::make_shared<Model>("project/assets/Characters/Quantum/FirstPerson/Quantum_Arms_FP.fbx");
    CHECK(armsModel->HasBones());

    // Static model (weapon casing) should not have bones
    auto casingModel = std::make_shared<Model>("project/assets/Weapons/AKS74U/Casing_545x39.fbx");
    CHECK(!casingModel->HasBones());
}

static void Test_GravityGunSettings_Defaults() {
    // Gravity gun settings should match FirstPersonControllerComponent defaults
    GravityGunSettings settings;
    FirstPersonControllerComponent fpc;

    CHECK(settings.GrabRange == fpc.GrabRange);
    CHECK(settings.AssistRange == fpc.AssistRange);
    CHECK(settings.ScrollTurnDeg == fpc.ScrollTurnDeg);

    // Check reasonable bounds
    CHECK(settings.GrabRange > 0.0f);
    CHECK(settings.AssistRange > 0.0f && settings.AssistRange < settings.GrabRange);
    CHECK(settings.ScrollTurnDeg > 0.0f);
}

static void Test_FirstPersonControllerComponent_CameraTunables() {
    FirstPersonControllerComponent fpc;

    // Check that camera and control tunables are present with sensible defaults
    CHECK(fpc.StickLookDegPerSec > 0.0f);      // gamepad turn rate
    CHECK(fpc.EyeRadius > 0.0f && fpc.EyeRadius < 1.0f);  // eye collision sphere

    // Verify they match the expected default values (from components removed from hardcoded)
    CHECK(fpc.StickLookDegPerSec == 180.0f);   // from Player.cpp
    CHECK(fpc.EyeRadius == 0.12f);             // from FirstPersonPresentation.cpp
}

void RegisterEngineTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Model::HasBones", Test_Model_HasBones});
    tests.push_back({"GravityGunSettings::Defaults", Test_GravityGunSettings_Defaults});
    tests.push_back({"FirstPersonControllerComponent::CameraTunables", Test_FirstPersonControllerComponent_CameraTunables});
}
