#include "TransformControllerSystem.h"
#include "World.h"
#include "Components.h"

#include <cmath>

void UpdateTransformControllers(World& world, float dt) {
    auto view = world.Registry.view<TransformControllerComponent, TransformComponent>();
    for (auto entity : view) {
        auto& controller = view.get<TransformControllerComponent>(entity);
        auto& transform = view.get<TransformComponent>(entity);
        if (!controller.Enabled) continue;

        if (!controller.Initialized) {
            controller.BasePosition = transform.Position;
            controller.BaseRotation = transform.RotationEuler;
            controller.BaseScale = transform.Scale;
            controller.Elapsed = 0.0f;
            controller.Initialized = true;
        }

        controller.Elapsed += dt;
        const float elapsed = controller.Elapsed;
        transform.Position = controller.BasePosition + controller.TranslationUnitsPerSec * elapsed;
        transform.RotationEuler = controller.BaseRotation + controller.RotationDegPerSec * elapsed;

        if (controller.ScalePulseAmplitude != 0.0f && controller.ScalePulseFrequencyHz != 0.0f) {
            float wave = std::sin(elapsed * controller.ScalePulseFrequencyHz * 6.28318530718f);
            float multiplier = std::max(0.01f, 1.0f + controller.ScalePulseAmplitude * wave);
            transform.Scale = controller.BaseScale * multiplier;
        }
    }
}
