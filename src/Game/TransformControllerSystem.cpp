#include "TransformControllerSystem.h"
#include "World.h"
#include "RotationMath.h"
#include "Components.h"

#include <cmath>

void UpdateTransformControllers(World& world, float dt) {
    auto view = world.Registry.view<TransformControllerComponent, TransformComponent>(entt::exclude<InactiveTag>); // #199 - an inactive object is paused, not just hidden
    for (auto entity : view) {
        auto& controller = view.get<TransformControllerComponent>(entity);
        auto& transform = view.get<TransformComponent>(entity);
        if (!controller.Enabled) continue;

        if (!controller.Initialized) {
            controller.BasePosition = transform.Position;
            controller.BaseRotation = transform.Rotation;
            controller.BaseScale = transform.Scale;
            controller.Elapsed = 0.0f;
            controller.Initialized = true;
        }

        controller.Elapsed += dt;
        const float elapsed = controller.Elapsed;
        transform.Position = controller.BasePosition + controller.TranslationUnitsPerSec * elapsed;
        // #123 - RotationDegPerSec is an angular velocity: turn the base by |w| * t about w's own
        // direction. Adding it to the Euler components only matched that for one principal axis.
        const float spinRate = glm::length(controller.RotationDegPerSec);
        transform.SetRotationQuaternion(spinRate > 0.0f
            ? RotateAboutLocalAxis(controller.BaseRotation, controller.RotationDegPerSec, std::fmod(spinRate * elapsed, 360.0f))
            : controller.BaseRotation);

        if (controller.ScalePulseAmplitude != 0.0f && controller.ScalePulseFrequencyHz != 0.0f) {
            float wave = std::sin(elapsed * controller.ScalePulseFrequencyHz * 6.28318530718f);
            float multiplier = std::max(0.01f, 1.0f + controller.ScalePulseAmplitude * wave);
            transform.Scale = controller.BaseScale * multiplier;
        }
    }
}
