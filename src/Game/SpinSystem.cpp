#include "SpinSystem.h"
#include "World.h"
#include "Components.h"

#include <glm/geometric.hpp>

void UpdateSpinners(World& world, float dt) {
    auto view = world.Registry.view<SpinComponent, TransformComponent>();
    for (auto entity : view) {
        const auto& spin = view.get<SpinComponent>(entity);
        float len = glm::length(spin.Axis);
        if (len < 1e-6f) continue; // a zero axis has no meaningful rotation
        glm::vec3 axis = spin.Axis / len;
        view.get<TransformComponent>(entity).RotationEuler += axis * spin.Speed * dt;
    }
}
