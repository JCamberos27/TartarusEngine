#include "NpcDroppedWeapon.h"

#include "GameModuleAPI.h" // BodyState, QueryFilter, RaycastHit
#include "Model.h"
#include "NpcBody.h"
#include "PhysicsWorld.h"
#include "World.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kGravity = 9.81f;
// A gun's box when its model has no usable bounds (a rifle: long on the model's forward axis), metres.
const glm::vec3 kFallbackHalf(0.04f, 0.1f, 0.4f);
} // namespace

DroppedWeaponSettingsComponent NpcDroppedWeapon::SettingsIn(const World& world) {
    auto view = world.Registry.view<const DroppedWeaponSettingsComponent>();
    return view.begin() != view.end() ? view.get<const DroppedWeaponSettingsComponent>(*view.begin()) : DroppedWeaponSettingsComponent();
}

std::unique_ptr<NpcDroppedWeapon> NpcDroppedWeapon::Drop(World& world, NpcBody& body, entt::entity weapon, const glm::vec3& impulse) {
    body.ReleaseWeaponHold(); // the arms are the ragdoll's whether or not a gun falls
    return DropAt(world, weapon, body.GunVelocity(), impulse, SettingsIn(world));
}

std::unique_ptr<NpcDroppedWeapon> NpcDroppedWeapon::DropAt(World& world, entt::entity weapon, const glm::vec3& handVelocity, const glm::vec3& impulse,
                                                           const DroppedWeaponSettingsComponent& cfg) {
    auto& reg = world.Registry;
    if (!cfg.Enabled || weapon == entt::null || !reg.valid(weapon) || !reg.all_of<TransformComponent>(weapon)) return nullptr;
    if (reg.any_of<InactiveTag, DeactivatedTag>(weapon)) return nullptr; // holstered or hidden: no gun in the hands
    std::unique_ptr<NpcDroppedWeapon> d(new NpcDroppedWeapon());
    d->m_Cfg = cfg;
    const TransformComponent w = world.WorldSpaceTransform(weapon);
    const glm::vec3 scaleAbs = glm::max(glm::abs(w.Scale), glm::vec3(1e-4f));
    d->m_Entity = world.CreateEmptyEntity(w.Position, glm::vec3(0.0f), w.Scale, "[Runtime] Dropped Weapon");
    world.SetWorldPose(d->m_Entity, w.Position, w.Rotation);
    reg.get<TransformComponent>(d->m_Entity).Scale = w.Scale;
    // The box: the model's bounds (in the model's units, scaled when the body is built; the centre offset is not scaled by the
    // physics, so it is here), kept between a sliver and a rifle's length.
    glm::vec3 half = kFallbackHalf / scaleAbs, center(0.0f);
    if (const auto* src = reg.try_get<RenderableComponent>(weapon); src && src->ModelRef) {
        RenderableComponent& dst = reg.emplace<RenderableComponent>(d->m_Entity);
        dst.ModelRef = src->ModelRef; // the soldier's own weapon instance: it outlives his weapon presentation here
        dst.Materials = src->Materials;
        dst.CastShadows = RenderableComponent::ShadowCasting::On;
        dst.ReceiveShadows = true;
        const glm::vec3 lo = src->ModelRef->BoundsMin(), hi = src->ModelRef->BoundsMax();
        if (lo.x < hi.x && lo.y < hi.y && lo.z < hi.z && glm::length(hi - lo) < 1e5f) {
            half = glm::clamp((hi - lo) * 0.5f * scaleAbs, glm::vec3(0.02f), glm::vec3(1.0f)) / scaleAbs;
            center = (lo + hi) * 0.5f * scaleAbs;
        }
    }
    d->m_HalfExtents = half;
    d->m_Center = center;
    // The hands' velocity, and a share of the round's impulse (a velocity change of share x impulse / mass, capped).
    glm::vec3 v = handVelocity;
    const float il = glm::length(impulse);
    if (il > 1e-4f && cfg.ImpulseShare > 0.0f)
        v += impulse / il * std::min(cfg.MaxShotSpeed, cfg.ImpulseShare * il / std::max(cfg.Mass, 0.1f));
    d->m_Velocity = v;
    // Tumble: about the horizontal axis across the line it leaves on, more the faster it goes.
    const glm::vec3 flat(v.x, 0.0f, v.z);
    if (cfg.Spin > 0.0f && glm::length(flat) > 0.05f)
        d->m_Spin = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(flat)) * std::min(cfg.MaxSpin, cfg.Spin * glm::length(v));
    if (cfg.CollisionDelay <= 0.0f) d->Build(world);
    return d;
}

void NpcDroppedWeapon::Build(World& world) {
    auto& reg = world.Registry;
    if (m_Entity == entt::null || !reg.valid(m_Entity)) return;
    ColliderComponent col;
    col.Kind = ColliderComponent::Shape::Box;
    col.HalfExtents = m_HalfExtents;
    col.Center = m_Center;
    col.Friction = m_Cfg.Friction;
    col.StaticFriction = m_Cfg.Friction;
    col.Bounciness = m_Cfg.Bounciness;
    col.FrictionCombine = 3; // Maximum: it grips a slick floor
    reg.emplace_or_replace<ColliderComponent>(m_Entity, col);
    RigidbodyComponent rb;
    rb.Mass = m_Cfg.Mass;
    rb.LinearDamping = m_Cfg.LinearDamping;
    rb.AngularDamping = m_Cfg.AngularDamping;
    rb.InitialVelocity = m_Velocity;
    rb.ContinuousCollision = true; // a thin box thrown hard must not go through the floor
    reg.emplace_or_replace<RigidbodyComponent>(m_Entity, rb);
    m_Built = true;
}

void NpcDroppedWeapon::Update(World& world, float dt) {
    auto& reg = world.Registry;
    if (m_Entity == entt::null || !reg.valid(m_Entity)) return;
    m_Age += dt;
    if (m_Cfg.Lifetime > 0.0f && m_Age > m_Cfg.Lifetime) { Stop(world); return; }
    if (!m_Built) {
        // Before the body exists the gun flies on its own: the hands' velocity and gravity, and the spin.
        TransformComponent t = world.WorldSpaceTransform(m_Entity);
        const glm::vec3 step = m_Velocity * dt;
        bool landed = false;
        if (glm::length(step) > 1e-5f) {
            const glm::vec3 dir = glm::normalize(step);
            const float o[3] = {t.Position.x, t.Position.y, t.Position.z}, d[3] = {dir.x, dir.y, dir.z};
            RaycastHit hit;
            landed = PhysicsWorld::RaycastSolid(o, d, glm::length(step) + 0.15f, QueryFilter{}, hit) && hit.Hit; // the world ends the flight
        }
        t.Position += step;
        m_Velocity.y -= kGravity * dt;
        const float spin = glm::length(m_Spin);
        if (spin > 1e-4f) t.Rotation = glm::normalize(glm::angleAxis(spin * dt, m_Spin / spin) * t.Rotation);
        world.SetWorldPose(m_Entity, t.Position, t.Rotation);
        if (landed || m_Age >= m_Cfg.CollisionDelay) Build(world);
        return;
    }
    if (!m_SpinApplied) {
        // The body is made by the physics step after Build; once it exists the spin goes on as a velocity change.
        BodyState s;
        if (PhysicsWorld::GetBodyState((unsigned)entt::to_integral(m_Entity), s) && s.Valid) {
            const float spin[3] = {m_Spin.x, m_Spin.y, m_Spin.z};
            if (glm::length(m_Spin) > 1e-4f) PhysicsWorld::AddTorque((unsigned)entt::to_integral(m_Entity), spin, 2u /*VelocityChange*/);
            m_SpinApplied = true;
        }
    }
}

void NpcDroppedWeapon::Stop(World& world) {
    if (m_Entity != entt::null && world.Registry.valid(m_Entity)) world.DestroyEntityAndChildren(m_Entity);
    m_Entity = entt::null;
}

bool NpcDroppedWeapon::Asleep() const {
    BodyState s;
    return m_Entity != entt::null && PhysicsWorld::GetBodyState((unsigned)entt::to_integral(m_Entity), s) && s.Valid && s.Sleeping;
}
