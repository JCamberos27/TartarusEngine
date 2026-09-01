#include "AnimationSystem.h"
#include "World.h"
#include "Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>

namespace {

// h in [0,1), s and v in [0,1]. Standard HSV->RGB.
glm::vec3 HsvToRgb(float h, float s, float v) {
    h = h - std::floor(h);
    float i = std::floor(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch (static_cast<int>(i) % 6) {
        case 0:  return {v, t, p};
        case 1:  return {q, v, p};
        case 2:  return {p, v, t};
        case 3:  return {p, q, v};
        case 4:  return {t, p, v};
        default: return {v, p, q};
    }
}

// An orthonormal pair spanning the plane perpendicular to `axis` (assumed non-zero).
void PerpBasis(const glm::vec3& axis, glm::vec3& u, glm::vec3& v) {
    glm::vec3 a = glm::normalize(axis);
    glm::vec3 ref = (std::abs(a.y) < 0.99f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    u = glm::normalize(glm::cross(a, ref));
    v = glm::cross(a, u);
}

} // namespace

void UpdateAnimators(World& world, float dt) {
    auto view = world.Registry.view<AnimatorComponent, TransformComponent>();
    for (auto entity : view) {
        auto& anim = view.get<AnimatorComponent>(entity);
        auto& transform = view.get<TransformComponent>(entity);
        auto* light = world.Registry.try_get<LightComponent>(entity);

        if (!anim.Initialized) {
            anim.BasePosition = transform.Position;
            anim.BaseColor = light ? light->Color : glm::vec3(1.0f);
            anim.Elapsed = 0.0f;
            anim.Initialized = true;
        }
        anim.Elapsed += dt;
        const float t = anim.Elapsed;

        // Spin: accumulate into the authored rotation.
        transform.RotationEuler += anim.SpinDegPerSec * dt;

        // Position = authored base + orbit + bob.
        glm::vec3 pos = anim.BasePosition;
        if (anim.OrbitRadius != 0.0f && glm::dot(anim.OrbitAxis, anim.OrbitAxis) > 1e-8f) {
            glm::vec3 u, v;
            PerpBasis(anim.OrbitAxis, u, v);
            float ang = glm::radians(anim.OrbitDegPerSec * t);
            pos += (std::cos(ang) * u + std::sin(ang) * v) * anim.OrbitRadius;
        }
        if (anim.BobAmplitude != 0.0f && anim.BobFreqHz != 0.0f) {
            pos.y += std::sin(glm::two_pi<float>() * anim.BobFreqHz * t) * anim.BobAmplitude;
        }
        transform.Position = pos;

        // Light hue cycle: keep the authored brightness, sweep the hue.
        if (light && anim.ColorCycleHzPerSec != 0.0f) {
            float brightness = glm::max(glm::max(anim.BaseColor.r, anim.BaseColor.g),
                                       glm::max(anim.BaseColor.b, 0.35f));
            light->Color = HsvToRgb(anim.ColorCycleHzPerSec * t, 0.85f, brightness);
        }
    }
}
