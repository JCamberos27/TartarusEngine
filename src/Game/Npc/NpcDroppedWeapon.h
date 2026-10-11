#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>

#include "Components.h" // DroppedWeaponSettingsComponent

class NpcBody;
class World;

// The gun a soldier drops when he dies: a copy of the weapon's model on its own simulated body (a convex hull of its mesh, the
// Dropped Weapon Settings' mass and friction). It is a body from the first frame, at the hands' velocity plus a share of the
// killing round's impulse. Once the ragdoll exists (SetOwner) the dead hand keeps its grip a moment (a breakable joint to the
// hand), the gun riding the falling arm, then lets go and the gun tumbles to rest and sleeps. It never passes through its owner
// and is never knocked out of the body by it: it ignores only the parts it starts inside of until it is clear of each (and the
// hands for Owner Ignore Time after the release), and collides with every other part from the start. The director keeps it as
// long as its rules say (Lifetime, never while the player can see it), longer than the corpse if need be.
class NpcDroppedWeapon {
public:
    // The first Dropped Weapon Settings in the world; the defaults without one.
    static DroppedWeaponSettingsComponent SettingsIn(const World& world);

    // The soldier's death: lets the hold's arm solve go (body.ReleaseWeaponHold) and drops the gun at `weapon` (the weapon
    // presentation's entity, still alive: it is copied, not taken), at body.GunVelocity() and the round's `impulse` (N s).
    // Null when the settings turn it off or there is no gun to drop.
    static std::unique_ptr<NpcDroppedWeapon> Drop(World& world, NpcBody& body, entt::entity weapon, const glm::vec3& impulse);
    // The drop itself, for a given velocity of the hands (m/s). `seed` picks the release delay.
    static std::unique_ptr<NpcDroppedWeapon> DropAt(World& world, entt::entity weapon, const glm::vec3& handVelocity, const glm::vec3& impulse,
                                                    const DroppedWeaponSettingsComponent& cfg, unsigned seed = 0u);

    ~NpcDroppedWeapon() = default;
    NpcDroppedWeapon(const NpcDroppedWeapon&) = delete;
    NpcDroppedWeapon& operator=(const NpcDroppedWeapon&) = delete;

    // The dead body's ragdoll and the hand holding the gun (a ragdoll part): the grip and the owner's parts to pass are worked
    // out once the gun's body exists.
    void SetOwner(int ragdoll, int handPart);
    // The ragdoll is going away: the grip lets go and nothing of it is ignored any more.
    void ForgetOwner();
    void Update(World& world, float dt); // the grip, the release, the parts it is clear of
    void Stop(World& world);             // the entity (and with it the body) gone

    entt::entity Entity() const { return m_Entity; }
    const DroppedWeaponSettingsComponent& Settings() const { return m_Cfg; }
    glm::vec3 Position(const World& world) const; // the gun's origin (world)
    bool Built() const { return m_Built; }        // the body's components are on (the body exists from the next physics step)
    bool Asleep() const;                          // at rest
    bool Held() const { return m_Grip >= 0; }     // still in the dead hand
    bool Released() const { return m_ReleasedAt >= 0.0f; }
    float ReleaseDelay() const { return m_ReleaseDelay; }
    float Age() const { return m_Age; }
    unsigned IgnoredParts() const { return m_Ignore; } // the owner's parts it passes through now (bit per part)
    glm::vec3 StartVelocity() const { return m_Velocity; }

private:
    NpcDroppedWeapon() = default;
    void Build(World& world);
    void Release(bool spin);

    entt::entity m_Entity = entt::null;
    DroppedWeaponSettingsComponent m_Cfg;
    glm::vec3 m_Velocity{0.0f}, m_Spin{0.0f}; // m/s, rad/s (world): the body's start, and the tumble it gets on release
    glm::vec3 m_HalfExtents{0.0f}, m_Center{0.0f};
    float m_Age = 0.0f, m_ReleaseDelay = 0.0f, m_HeldFrom = -1.0f, m_ReleasedAt = -1.0f;
    int m_Ragdoll = -1, m_Hand = -1, m_Grip = -1;
    unsigned m_Ignore = 0u;
    bool m_Built = false, m_Bound = false; // m_Bound: the grip and the ignore set have been worked out
};
