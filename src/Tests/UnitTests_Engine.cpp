#include "UnitTestSupport.h"
#include "../Renderer/Model.h"

// Unit tests for renderer, core and performance. Add a function per test and list it below.

static void Test_Model_HasBones() {
    // Rigged model should have bones
    auto armsModel = std::make_shared<Model>("project/assets/Characters/Quantum/FirstPerson/Quantum_Arms_FP.fbx");
    CHECK(armsModel->HasBones());

    // Static model (weapon casing) should not have bones
    auto casingModel = std::make_shared<Model>("project/assets/Weapons/AKS74U/Casing_545x39.fbx");
    CHECK(!casingModel->HasBones());
}

void RegisterEngineTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Model::HasBones", Test_Model_HasBones});
}
