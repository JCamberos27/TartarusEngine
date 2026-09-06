#include "ComponentRegistry.h"

#include "Components.h"

#include <IconsFontAwesome6.h>

namespace ComponentRegistry {

namespace {
std::vector<RegisteredComponent>& Storage() {
    static std::vector<RegisteredComponent> registry;
    return registry;
}
} // namespace

const std::vector<RegisteredComponent>& All() { return Storage(); }

void Add(RegisteredComponent entry) { Storage().push_back(std::move(entry)); }

void RegisterEngineComponents() {
    using T = ReflectFieldType;

    Register<SpinComponent>({
        "Spin", ICON_FA_ARROWS_SPIN,
        "Spins the object around a local axis while playing (SpinSystem, in TartarusGame.dll).",
        {
            { "Axis",  T::Vec3,  offsetof(SpinComponent, Axis),  0.01f, "Local axis to rotate around." },
            { "Speed", T::Float, offsetof(SpinComponent, Speed), 1.0f,  "Degrees per second." },
        },
    });
}

// Runs once, before main(). It only appends to All()'s function-local static, so there is no
// static-init-order dependency.
namespace {
struct AutoRegister {
    AutoRegister() { RegisterEngineComponents(); }
} g_autoRegister;
} // namespace

} // namespace ComponentRegistry
