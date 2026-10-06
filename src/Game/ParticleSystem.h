#pragma once
#include <entt/entt.hpp>
#include "Curve.h"

struct ParticleCurveCache {
    std::string Stamp;
    Curve Size = Curve::Constant(1), Alpha = Curve::Constant(1);
    Curve Speed = Curve::Constant(1), Emission = Curve::Constant(1);
};

class World;
struct ParticleSystemComponent;
void RestartParticleSystem(ParticleSystemComponent& system, bool clear = true);
void EmitParticleBurst(World& world, entt::entity entity, int count);

// #177 - steps every ParticleSystemComponent: emits new particles from the entity's world
// transform, integrates velocity and gravity, and retires expired ones. Inactive entities drop
// their particles. `dt` is game time in Play and real time while editing.
void UpdateParticleSystems(World& world, float dt);
