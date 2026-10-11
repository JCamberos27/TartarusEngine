#include "NpcDroppedWeapon.h"

#include "GameModuleAPI.h" // BodyState
#include "Model.h"
#include "NpcBody.h"
#include "NpcRagdoll.h"
#include "PhysicsWorld.h"
#include "World.h"

#include <algorithm>
#include <cmath>

namespace {
// A gun's box when its model has no usable bounds (a rifle: long on the model's forward axis), metres.
const glm::vec3 kFallbackHalf(0.04f, 0.1f, 0.4f);
// The parts a gun always passes while it is held and just after: the hands and forearms.
constexpr unsigned kHandsMask = (1u << 4) | (1u << 6) | (1u << NpcRagdoll::kHandL) | (1u << NpcRagdoll::kHandR);
unsigned Hash(unsigned x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
} // namespace

DroppedWeaponSettingsComponent NpcDroppedWeapon::SettingsIn(const World& world) {
    auto view = world.Registry.view<const DroppedWeaponSettingsComponent>();
    return view.begin() != view.end() ? view.get<const DroppedWeaponSettingsComponent>(*view.begin()) : DroppedWeaponSettingsComponent();
}

std::unique_ptr<NpcDroppedWeapon> NpcDroppedWeapon::Drop(World& world, NpcBody& body, entt::entity weapon, const glm::vec3& impulse) {
    body.ReleaseWeaponHold(); // the arms are the ragdoll's whether or not a gun falls
    return DropAt(world, weapon, body.GunVelocity(), impulse, SettingsIn(world), (unsigned)entt::to_integral(weapon));
}

std::unique_ptr<NpcDroppedWeapon> NpcDroppedWeapon::DropAt(World& world, entt::entity weapon, const glm::vec3& handVelocity, const glm::vec3& impulse,
                                                           const DroppedWeaponSettingsComponent& cfg, unsigned seed) {
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
    // The fallback box: the model's bounds (in the model's units, scaled when the body is built; the centre offset is not scaled by
    // the physics, so it is here), kept between a sliver and a rifle's length.
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
    // Tumble on release: about the horizontal axis across the line it leaves on, more the faster it goes.
    const glm::vec3 flat(v.x, 0.0f, v.z);
    if (cfg.Spin > 0.0f && glm::length(flat) > 0.05f)
        d->m_Spin = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(flat)) * std::min(cfg.MaxSpin, cfg.Spin * glm::length(v));
    const float lo = std::max(cfg.ReleaseDelayMin, 0.0f), hi = std::max(cfg.ReleaseDelayMax, lo);
    d->m_ReleaseDelay = lo + (hi - lo) * (float)(Hash(seed * 2654435761u + 1u) & 0xFFFFu) / 65535.0f;
    d->Build(world);
    return d;
}

void NpcDroppedWeapon::Build(World& world) {
    auto& reg = world.Registry;
    if (m_Entity == entt::null || !reg.valid(m_Entity)) return;
    ColliderComponent col;
    // Its own shape: a hull of the mesh lies on its side as the gun does (the box stands it on the magazine's corner).
    const bool hull = m_Cfg.HullCollider && reg.all_of<RenderableComponent>(m_Entity);
    col.Kind = hull ? ColliderComponent::Shape::ConvexHull : ColliderComponent::Shape::Box;
    col.HalfExtents = hull ? glm::vec3(0.0f) : m_HalfExtents;
    col.Center = hull ? glm::vec3(0.0f) : m_Center;
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
    rb.ContinuousCollision = true; // a thin gun thrown hard must not go through the floor
    reg.emplace_or_replace<RigidbodyComponent>(m_Entity, rb);
    m_Built = true;
}

void NpcDroppedWeapon::SetOwner(int ragdoll, int handPart) {
    m_Ragdoll = ragdoll;
    m_Hand = handPart;
    m_Bound = false;
}

void NpcDroppedWeapon::ForgetOwner() {
    if (m_Grip >= 0 && m_Ragdoll >= 0) PhysicsWorld::DetachFromRagdoll(m_Ragdoll, m_Grip);
    m_Grip = -1;
    if (m_Entity != entt::null) PhysicsWorld::IgnoreRagdollParts((unsigned)entt::to_integral(m_Entity), -1, 0u);
    m_Ignore = 0u;
    m_Ragdoll = -1;
    if (m_ReleasedAt < 0.0f) m_ReleasedAt = m_Age;
}

void NpcDroppedWeapon::Release(bool spin) {
    if (m_Grip >= 0) PhysicsWorld::DetachFromRagdoll(m_Ragdoll, m_Grip);
    m_Grip = -1;
    m_ReleasedAt = m_Age;
    // The fingers open: the gun goes on with what the arm gave it, and turns over as it leaves.
    if (spin && glm::length(m_Spin) > 1e-4f) {
        const float s[3] = {m_Spin.x, m_Spin.y, m_Spin.z};
        PhysicsWorld::AddTorque((unsigned)entt::to_integral(m_Entity), s, 2u /*VelocityChange*/);
    }
}

void NpcDroppedWeapon::Update(World& world, float dt) {
    auto& reg = world.Registry;
    if (m_Entity == entt::null || !reg.valid(m_Entity)) return;
    m_Age += dt;
    // The body is made by the physics step after Build.
    BodyState bs;
    if (!PhysicsWorld::GetBodyState((unsigned)entt::to_integral(m_Entity), bs) || !bs.Valid) return;
    const unsigned id = (unsigned)entt::to_integral(m_Entity);
    if (!m_Bound && m_Ragdoll >= 0) {
        m_Bound = true;
        // Inside the hands, and whatever else of the body it starts in (an arm across the chest, the stock at the shoulder):
        // passed until clear. Everything else of the body it hits from the start.
        m_Ignore = kHandsMask | PhysicsWorld::RagdollPartsOverlapping(id, m_Ragdoll, 0xFFFFu, 0.02f);
        PhysicsWorld::IgnoreRagdollParts(id, m_Ragdoll, m_Ignore);
        if (m_ReleaseDelay > 0.0f && m_Hand >= 0) {
            // Held from the forearm, not the hand: the gun's weight on a half-kilo hand would wrench the wrist past its range in the
            // first frames. The wrist stays free, and the gun still rides the arm.
            const int holder = m_Hand == NpcRagdoll::kHandL ? 4 : m_Hand == NpcRagdoll::kHandR ? 6 : m_Hand;
            m_Grip = PhysicsWorld::AttachToRagdollPart(id, m_Ragdoll, holder, m_Cfg.GripBreakForce);
            m_HeldFrom = m_Age;
        }
        if (m_Grip < 0) Release(true);
    } else if (!m_Bound && m_ReleasedAt < 0.0f && m_Age > 0.25f) {
        Release(true); // no body to hold it (the ragdoll never started): it simply falls
    }
    if (m_Grip >= 0 && (m_Age - m_HeldFrom >= m_ReleaseDelay || !PhysicsWorld::RagdollAttachmentHeld(m_Ragdoll, m_Grip))) Release(true);
    // After the release: let go of each part once the gun is clear of it (the hands not before Owner Ignore Time).
    if (m_Ignore != 0u && m_Grip < 0 && m_Ragdoll >= 0 && m_ReleasedAt >= 0.0f) {
        unsigned keep = PhysicsWorld::RagdollPartsOverlapping(id, m_Ragdoll, m_Ignore, 0.005f);
        if (m_Age - m_ReleasedAt < m_Cfg.OwnerIgnoreTime) keep |= m_Ignore & kHandsMask;
        // Not for long, though: a body that comes down on the gun while it still passes a part would leave it inside. Past this the
        // two collide again and the contact pushes the (light) gun out.
        if (m_Age - m_ReleasedAt > m_Cfg.OwnerIgnoreTime + 0.5f) keep = 0u;
        if (keep != m_Ignore) {
            // Parts it is still inside of (a body come down on it while it passed them) it is moved out of first: the contact would
            // only fight it there, the gun shaking inside the body for good.
            const unsigned stillIn = PhysicsWorld::RagdollPartsOverlapping(id, m_Ragdoll, m_Ignore & ~keep, 0.0f);
            if (stillIn) PhysicsWorld::PushOutOfRagdoll(id, m_Ragdoll, stillIn | (0xFFFFu & ~keep & ~m_Ignore));
            m_Ignore = keep;
            PhysicsWorld::IgnoreRagdollParts(id, m_Ragdoll, m_Ignore);
        }
    }
}

glm::vec3 NpcDroppedWeapon::Position(const World& world) const {
    if (m_Entity == entt::null || !world.Registry.valid(m_Entity)) return glm::vec3(0.0f);
    return world.WorldSpaceTransform(m_Entity).Position;
}

void NpcDroppedWeapon::Stop(World& world) {
    if (m_Grip >= 0 && m_Ragdoll >= 0) PhysicsWorld::DetachFromRagdoll(m_Ragdoll, m_Grip);
    m_Grip = -1;
    if (m_Entity != entt::null && world.Registry.valid(m_Entity)) world.DestroyEntityAndChildren(m_Entity);
    m_Entity = entt::null;
}

bool NpcDroppedWeapon::Asleep() const {
    BodyState s;
    return m_Entity != entt::null && PhysicsWorld::GetBodyState((unsigned)entt::to_integral(m_Entity), s) && s.Valid && s.Sleeping;
}
