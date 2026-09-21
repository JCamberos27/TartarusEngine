#include "SpinSystem.h"
#include "World.h"
#include "RotationMath.h"
#include "Components.h"

#include <cmath>
#include <glm/geometric.hpp>

void UpdateSpinners(World& world, float dt) {
    auto view = world.Registry.view<SpinComponent, TransformComponent>(entt::exclude<InactiveTag>); // #199 - an inactive object is paused, not just hidden
    for (auto entity : view) {
        auto& spin = view.get<SpinComponent>(entity);
        float len = glm::length(spin.Axis);
        if (len < 1e-6f) continue; // a zero axis has no meaningful rotation
        auto& transform = view.get<TransformComponent>(entity);

        // #123 - turn about the axis itself. Adding axis * angle to the Euler components was only
        // right for a principal axis; a diagonal one added equal pitch and yaw, which spins about
        // some other line.
        //
        // The orientation is always BaseRotation turned by Angle, never the previous frame's
        // output turned a little more, so a spinner left running for hours has no error to build
        // up, and the wrapped Angle keeps the numbers small (audit CPP-205 / #375). If anything
        // else moved the rotation since we last wrote it (the Inspector during Play, physics,
        // the gravity gun) or the axis was edited, restart from where the object is now.
        if (!spin.Initialized || !SameRotation(transform.Rotation, spin.LastRotation) || spin.Axis != spin.LastAxis) {
            spin.BaseRotation = transform.Rotation;
            spin.LastAxis = spin.Axis;
            spin.Angle = 0.0f;
            spin.Initialized = true;
        }
        spin.Angle = std::fmod(spin.Angle + spin.Speed * dt, 360.0f);
        const glm::quat rotation = RotateAboutLocalAxis(spin.BaseRotation, spin.Axis, spin.Angle);
        transform.SetRotationQuaternion(rotation);
        spin.LastRotation = rotation;
    }
}
