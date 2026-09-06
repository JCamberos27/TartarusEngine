#include "GameModuleAPI.h"
#include "TransformControllerSystem.h"
#include "SpinSystem.h"

namespace {

void OnLoad() {}
void OnUnload() {}

// The Transform Controller is the first gameplay system hosted in the reloadable module. Edit
// this DLL (or add future gameplay systems here), build TartarusGame, and the open editor will
// pick up the new code without restarting.
void Update(const GameModuleHostAPI& host, World& world, float deltaTime) {
    // A visible cross-DLL smoke test: this request is implemented by the host, while deciding
    // to request it lives in the reloadable game module. The host side is idempotent, so module
    // reloads never duplicate the four donuts.
    if (host.EnsureRoomDonutTestSet) host.EnsureRoomDonutTestSet(world);
    UpdateTransformControllers(world, deltaTime);
    UpdateSpinners(world, deltaTime); // #184: first reflection-registered component's system

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
