#include "SpinSystem.h"
#include "World.h"
#include "Components.h"

#include <cmath>
#include <glm/geometric.hpp>

void UpdateSpinners(World& world, float dt) {
    auto view = world.Registry.view<SpinComponent, TransformComponent>();
    for (auto entity : view) {
        const auto& spin = view.get<SpinComponent>(entity);
        float len = glm::length(spin.Axis);
        if (len < 1e-6f) continue; // a zero axis has no meaningful rotation
        glm::vec3 axis = spin.Axis / len;
        glm::vec3& rot = view.get<TransformComponent>(entity).RotationEuler;
        rot += axis * spin.Speed * dt;

        // Wrap the axes this spinner actually drives back into [0,360) every frame (audit
        // CPP-205 / #375): left running, this was unbounded accumulation into RotationEuler —
        // fine for the first few minutes, but a spinner left playing for hours (a looping
        // showcase build, an idle level) grows the stored degrees into the millions, losing
        // float precision exactly like #375's write-up on this component describes. Wrapping is
        // exact here (unlike a general Euler-angle re-derivation elsewhere in CPP-205) because
        // ComposeTransform's eulerAngleYXZ is periodic in each axis — mod-360 doesn't change the
        // resulting rotation, only how big the stored number gets. Same technique
        // AnimationSystem already uses for its own spin/orbit offsets (AnimationSystem.cpp).
        for (int k = 0; k < 3; ++k) {
            if (axis[k] != 0.0f)
                rot[k] = std::fmod(std::fmod(rot[k], 360.0f) + 360.0f, 360.0f);
        }
    }
}
