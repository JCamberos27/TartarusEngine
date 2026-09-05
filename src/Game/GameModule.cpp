#include "GameModuleAPI.h"
#include "TransformControllerSystem.h"

namespace {

void OnLoad() {}
void OnUnload() {}

// The Transform Controller is the first gameplay system hosted in the reloadable module. Edit
// this DLL (or add future gameplay systems here), build TartarusGame, and the open editor will
// pick up the new code without restarting.
void Update(World& world, float deltaTime) {
    UpdateTransformControllers(world, deltaTime);
}

const GameModuleAPI kAPI{
    kGameModuleAPIVersion,
    &OnLoad,
    &OnUnload,
    &Update,
};

} // namespace

extern "C" __declspec(dllexport) const GameModuleAPI* TartarusGetGameModuleAPI() {
    return &kAPI;
}

