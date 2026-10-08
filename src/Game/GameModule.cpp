#include "GameModuleAPI.h"
#include "TransformControllerSystem.h"

namespace {

// Nothing to set up or carry across a reload yet: the systems below keep their state in World
// components. See GameModuleAPI for the OnLoad / SaveState contract when that changes.
bool OnLoad(const void* /*state*/, std::size_t /*stateSize*/) { return true; }
void OnUnload() {}

// The Transform Controller is the first gameplay system hosted in the reloadable module. Edit
// this DLL (or add future gameplay systems here), build TartarusGame, and the open editor will
// pick up the new code without restarting.
void Update(const GameModuleHostAPI&, World& world, float deltaTime) {
    UpdateTransformControllers(world, deltaTime);
}

const GameModuleAPI kAPI{
    kGameModuleAPIVersion,
    &OnLoad,
    &OnUnload,
    &Update,
    /*SaveState=*/nullptr,
    /*FixedUpdate=*/nullptr,
};

} // namespace

extern "C" __declspec(dllexport) const GameModuleAPI* TartarusGetGameModuleAPI() {
    return &kAPI;
}
