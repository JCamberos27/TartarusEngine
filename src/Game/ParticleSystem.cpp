#include "ParticleSystem.h"
#include "Components.h"
#include "ProjectSettings.h"
#include "World.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace {

// xorshift32 per system: deterministic, cheap, and no shared global state.
float Rand01(std::uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (float)(s & 0xFFFFFFu) / (float)0x1000000u;
}

// A unit vector inside a cone of half-angle `halfAngleRad` around +Y (uniform over the cap).
glm::vec3 RandomInCone(std::uint32_t& s, float halfAngleRad) {
    const float cosMax = std::cos(halfAngleRad);
    const float z = 1.0f - Rand01(s) * (1.0f - cosMax); // cos(theta) in [cosMax, 1]
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const float phi = Rand01(s) * glm::two_pi<float>();
    return glm::vec3(r * std::cos(phi), z, r * std::sin(phi));
}

} // namespace

void UpdateParticleSystems(World& world, float dt) {
    if (dt <= 0.0f) return;
    dt = std::min(dt, 0.1f); // a long hitch shouldn't dump a burst of particles at once
    const float gravityY = ProjectSettings::Physics().Gravity.y;

    for (auto [e, ps] : world.Registry.view<ParticleSystemComponent>().each()) {
        if (world.Registry.all_of<InactiveTag>(e) || !world.Registry.all_of<TransformComponent>(e)) {
            ps.Live.clear();
            ps.EmitAccumulator = 0.0f;
            continue;
        }

        // Age and move what's alive; swap-remove the expired.
        const float g = gravityY * ps.GravityModifier;
        for (size_t i = 0; i < ps.Live.size();) {
            auto& p = ps.Live[i];
            p.Age += dt;
            if (p.Age >= p.Life) {
                p = ps.Live.back();
                ps.Live.pop_back();
                continue;
            }
            p.Vel.y += g * dt;
            p.Pos += p.Vel * dt;
            ++i;
        }

        if (!ps.Emitting || ps.Rate <= 0.0f || ps.Lifetime <= 0.0f) {
            ps.EmitAccumulator = 0.0f;
            continue;
        }
        const int cap = std::clamp(ps.MaxParticles, 0, 100000);
        ps.EmitAccumulator += ps.Rate * dt;
        if (ps.EmitAccumulator < 1.0f) continue;

        const glm::mat4 m = world.ComposeWorldTransform(e);
        const glm::vec3 origin(m[3]);
        const glm::mat3 basis(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])),
                              glm::normalize(glm::vec3(m[2])));
        const float halfAngle = glm::radians(std::clamp(ps.Spread, 0.0f, 180.0f));
        while (ps.EmitAccumulator >= 1.0f) {
            ps.EmitAccumulator -= 1.0f;
            if ((int)ps.Live.size() >= cap) continue; // at the cap: this emission is dropped
            ParticleSystemComponent::Particle p;
            p.Pos = origin;
            p.Vel = basis * RandomInCone(ps.Rng, halfAngle) * ps.StartSpeed;
            p.Life = ps.Lifetime * (0.85f + 0.3f * Rand01(ps.Rng)); // slight variation reads as natural
            // Spread emissions across the frame so a high rate doesn't come out in visible shells.
            const float sub = Rand01(ps.Rng) * dt;
            p.Age = sub;
            p.Pos += p.Vel * sub;
            ps.Live.push_back(p);
        }
    }
}
