#include "GameModuleAPI.h"
#include "BodyLocomotion.h"
#include "WeaponRules.h"
#include "TransformControllerSystem.h"
#include "LightFlickerSystem.h"

namespace {

// Nothing to set up or carry across a reload yet: the systems below keep their state in World
// components. See GameModuleAPI for the OnLoad / SaveState contract when that changes.
bool OnLoad(const void* /*state*/, std::size_t /*stateSize*/) { return true; }
void OnUnload() {}

// Gameplay systems hosted in the reloadable module. Edit one (or add another here), save it in
// the Script IDE or build TartarusGame, and the open editor picks the new code up without
// restarting.
void Update(const GameModuleHostAPI&, World& world, float deltaTime) {
    UpdateTransformControllers(world, deltaTime);
    UpdateLightFlickers(world, deltaTime);
}

const GameModuleAPI kAPI{
    kGameModuleAPIVersion,
    &OnLoad,
    &OnUnload,
    &Update,
    /*SaveState=*/nullptr,
    /*FixedUpdate=*/nullptr,
    // The first-person rules: the engine calls these instead of its built-in copies, so editing
    // WeaponRules.cpp or BodyLocomotion.cpp reloads like the systems above.
    Gameplay::FramesLayout(),
    &WeaponRules::Update,
    &WeaponRules::UpdateLoadout,
    &BodyLocomotion::Update,
};

} // namespace

extern "C" __declspec(dllexport) const GameModuleAPI* TartarusGetGameModuleAPI() {
    return &kAPI;
}
