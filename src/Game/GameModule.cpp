#include "GameModuleAPI.h"
#include "TransformControllerSystem.h"
#include "SpinSystem.h"

namespace {

// Nothing to set up or carry across a reload yet: the systems below keep their state in World
// components. See GameModuleAPI for the OnLoad / SaveState contract when that changes.
bool OnLoad(const void* /*state*/, std::size_t /*stateSize*/) { return true; }
void OnUnload() {}

// The Transform Controller is the first gameplay system hosted in the reloadable module. Edit
// this DLL (or add future gameplay systems here), build TartarusGame, and the open editor will
// pick up the new code without restarting.
void Update(const GameModuleHostAPI& host, World& world, float deltaTime) {
    (void)host; // no host callbacks needed yet — the module→World path is exercised by the systems below
    UpdateTransformControllers(world, deltaTime);
    UpdateSpinners(world, deltaTime); // #184: first reflection-registered component's system
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
