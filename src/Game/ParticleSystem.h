#pragma once

class World;

// #177 - steps every ParticleSystemComponent: emits new particles from the entity's world
// transform, integrates velocity and gravity, and retires expired ones. Inactive entities drop
// their particles. `dt` is game time in Play and real time while editing.
void UpdateParticleSystems(World& world, float dt);
