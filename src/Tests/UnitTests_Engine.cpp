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

void RegisterEngineTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Model::HasBones", Test_Model_HasBones});
    tests.push_back({"GravityGunSettings::Defaults", Test_GravityGunSettings_Defaults});
}
