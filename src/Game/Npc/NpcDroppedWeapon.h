#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>

#include "Components.h" // DroppedWeaponSettingsComponent

class NpcBody;
class World;

// The gun a soldier drops when he dies: a copy of the weapon's model on its own simulated body (a box from the model's bounds,
// the Dropped Weapon Settings' mass and friction), leaving the hands at the gun's velocity plus a share of the killing round's
// impulse and tumbling to rest, where it sleeps. It lies as long as the corpse does (or Lifetime): the director calls Update
// with the ragdoll's and Stop where it resets the ragdoll.
class NpcDroppedWeapon {
public:
    // The first Dropped Weapon Settings in the world; the defaults without one.
    static DroppedWeaponSettingsComponent SettingsIn(const World& world);

    // The soldier's death: lets the hold's arm solve go (body.ReleaseWeaponHold) and drops the gun at `weapon` (the weapon
    // presentation's entity, still alive: it is copied, not taken), at body.GunVelocity() and the round's `impulse` (N s).
    // Null when the settings turn it off or there is no gun to drop.
    static std::unique_ptr<NpcDroppedWeapon> Drop(World& world, NpcBody& body, entt::entity weapon, const glm::vec3& impulse);
    // The drop itself, for a given velocity of the hands (m/s).
    static std::unique_ptr<NpcDroppedWeapon> DropAt(World& world, entt::entity weapon, const glm::vec3& handVelocity, const glm::vec3& impulse,
                                                    const DroppedWeaponSettingsComponent& cfg);

    ~NpcDroppedWeapon() = default;
    NpcDroppedWeapon(const NpcDroppedWeapon&) = delete;
    NpcDroppedWeapon& operator=(const NpcDroppedWeapon&) = delete;

    void Update(World& world, float dt); // the flight before the body exists, its spin, the lifetime
    void Stop(World& world);             // the entity (and with it the body) gone

    entt::entity Entity() const { return m_Entity; }
    bool Built() const { return m_Built; } // the body exists (the collision delay has passed)
    bool Asleep() const;                   // at rest
    glm::vec3 StartVelocity() const { return m_Velocity; }

private:
    NpcDroppedWeapon() = default;
    void Build(World& world);

    entt::entity m_Entity = entt::null;
    DroppedWeaponSettingsComponent m_Cfg;
    glm::vec3 m_Velocity{0.0f}, m_Spin{0.0f}; // m/s, rad/s (world); the flight's, and the body's start
    glm::vec3 m_HalfExtents{0.0f}, m_Center{0.0f};
    float m_Age = 0.0f;
    bool m_Built = false, m_SpinApplied = false;
};
