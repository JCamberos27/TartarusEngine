#include "PhysicsInternal.h"

namespace PhysicsWorld {
namespace {
// --- NPC hitboxes ----------------------------------------------------------------------

PxTransform HitboxPose(const HitCapsule& c) {
    return PxTransform(PxVec3(c.Position[0], c.Position[1], c.Position[2]), ToQuat(c.Rotation));
}
// One active set more (or fewer) shadowing its capsule: the capsule is out of queries while any set still stands in for it.
void ShadowCapsule(PhysicsState::Hitboxes& h, bool on) {
    if (!g_State || !h.Capsule) return;
    auto& shadowed = g_State->hitboxedCapsules;
    if (on) {
        ++shadowed[h.Capsule];
    } else if (auto it = shadowed.find(h.Capsule); it != shadowed.end() && --it->second <= 0) {
        shadowed.erase(it);
    }
}

// The live set `id` names (its slot's current generation, not freed), or null.
PhysicsState::Hitboxes* HitboxesAt(int id) {
    PhysicsState::Hitboxes* h = g_State ? HandleTarget(g_State->hitboxes, id) : nullptr;
    return h && !h->Bodies.empty() ? h : nullptr;
}

// Kinematic query-only capsules reporting `entity`; `capsule` (may be null) is the character capsule they stand in for.
int MakeHitboxes(PxRigidActor* capsule, std::uint32_t entity, const HitCapsule* parts, int count) {
    PhysicsState& s = *g_State;
    const int id = AcquireSlot(s.hitboxes, [](const PhysicsState::Hitboxes& x) { return x.Bodies.empty(); });
    if (id < 0) { Log::Error("PhysX: out of hitbox slots."); return -1; }
    PhysicsState::Hitboxes& h = s.hitboxes[(size_t)id];
    h.Gen = NextGen(h.Gen);
    h.Capsule = capsule;
    for (int i = 0; i < count; ++i) {
        PxRigidDynamic* b = s.physics->createRigidDynamic(HitboxPose(parts[i]));
        PxShape* sh = b && s.defaultMaterial ? PxRigidActorExt::createExclusiveShape(
            *b, PxCapsuleGeometry(std::max(parts[i].Radius, 0.01f), std::max(parts[i].HalfLength, 0.005f)), *s.defaultMaterial) : nullptr;
        if (!sh) { // fail cleanly: take back the ones already made
            Log::Error("PhysX: creating a hitbox capsule failed.");
            if (b) b->release();
            for (PxRigidDynamic* made : h.Bodies) {
                s.hitboxActors.erase(made);
                s.bodyHitboxActors.erase(made);
                made->release();
            }
            h.Bodies.clear();
            h.Capsule = nullptr;
            return -1;
        }
        sh->setFlag(PxShapeFlag::eSIMULATION_SHAPE, false); // query only
        sh->setFlag(PxShapeFlag::eSCENE_QUERY_SHAPE, true);
        sh->setSimulationFilterData(PxFilterData(1u, 0u, 0u, 0u)); // the Default layer, as the capsule
        sh->setQueryFilterData(PxFilterData(1u, 0u, 0u, 0u));
        sh->userData = BodyPartTag(kBodyPartHitbox, (unsigned)i);
        b->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
        b->userData = reinterpret_cast<void*>(static_cast<std::uintptr_t>(entity));
        s.scene->addActor(*b);
        h.Bodies.push_back(b);
        s.hitboxActors.insert(b);
        if (!capsule) s.bodyHitboxActors.insert(b);
    }
    h.Active = true;
    ShadowCapsule(h, true);
    return MakeHandle((size_t)id, h.Gen);
}
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld::detail {
void ForgetHitboxCapsule(PhysicsState& s, const PxRigidActor* capsule) {
    if (!capsule) return;
    s.hitboxedCapsules.erase(capsule);
    for (auto& h : s.hitboxes)
        if (h.Capsule == capsule) h.Capsule = nullptr; // (a later actor at the same address isn't shadowed by these)
}
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
int CreateNpcHitboxes(CharacterId owner, const HitCapsule* parts, int count) {
    PxRigidActor* capsule = NpcActor(owner);
    if (!g_State || !g_State->scene || !capsule || !parts || count <= 0) return -1;
    return MakeHitboxes(capsule, UserDataToEntity(capsule->userData), parts, count);
}

int CreateBodyHitboxes(unsigned entity, const HitCapsule* parts, int count) {
    if (!g_State || !g_State->scene || !parts || count <= 0) return -1;
    return MakeHitboxes(nullptr, entity, parts, count);
}

void SetNpcHitboxPoses(int id, const HitCapsule* parts, int count) {
    PhysicsState::Hitboxes* hp = HitboxesAt(id);
    if (!hp || !parts) return;
    auto& h = *hp;
    for (int i = 0; i < count && i < (int)h.Bodies.size(); ++i) {
        if (!FiniteVec3(parts[i].Position) || !std::isfinite(parts[i].Rotation[0]) || !std::isfinite(parts[i].Rotation[1]) ||
            !std::isfinite(parts[i].Rotation[2]) || !std::isfinite(parts[i].Rotation[3]))
            continue; // keep this hitbox where it was
        h.Bodies[(size_t)i]->setGlobalPose(HitboxPose(parts[i]), false);
    }
}

void SetNpcHitboxesActive(int id, bool active) {
    PhysicsState::Hitboxes* hp = HitboxesAt(id);
    if (!hp || hp->Active == active) return;
    auto& h = *hp;
    h.Active = active;
    for (PxRigidDynamic* b : h.Bodies) {
        PxShape* sh = nullptr;
        if (b->getShapes(&sh, 1) == 1 && sh) sh->setFlag(PxShapeFlag::eSCENE_QUERY_SHAPE, active);
    }
    ShadowCapsule(h, active);
}

void HitboxDebugLines(std::vector<float>& out) {
    if (!g_State) return;
    auto line = [&](const PxVec3& a, const PxVec3& b, const PxVec3& c) {
        const float v[14] = {a.x, a.y, a.z, c.x, c.y, c.z, 0.9f, b.x, b.y, b.z, c.x, c.y, c.z, 0.9f};
        out.insert(out.end(), v, v + 14);
    };
    for (const auto& h : g_State->hitboxes) {
        if (!h.Active) continue;
        const PxVec3 col = h.Capsule ? PxVec3(1.0f, 0.55f, 0.15f) : PxVec3(1.0f, 0.25f, 0.85f);
        for (const PxRigidDynamic* b : h.Bodies) {
            PxShape* sh = nullptr;
            if (!b || b->getShapes(&sh, 1) != 1 || !sh || sh->getGeometry().getType() != PxGeometryType::eCAPSULE) continue;
            const auto& g = static_cast<const PxCapsuleGeometry&>(sh->getGeometry());
            const PxTransform t = b->getGlobalPose() * sh->getLocalPose();
            const PxVec3 ax = t.q.rotate(PxVec3(1, 0, 0)), u = t.q.rotate(PxVec3(0, 1, 0)), w = t.q.rotate(PxVec3(0, 0, 1));
            const PxVec3 e0 = t.p - ax * g.halfHeight, e1 = t.p + ax * g.halfHeight;
            line(e0 - ax * g.radius, e1 + ax * g.radius, col);
            constexpr int kSeg = 8;
            for (int k = 0; k < kSeg; ++k) {
                const float a0 = 6.2831853f * k / kSeg, a1 = 6.2831853f * (k + 1) / kSeg;
                const PxVec3 r0 = (u * std::cos(a0) + w * std::sin(a0)) * g.radius, r1 = (u * std::cos(a1) + w * std::sin(a1)) * g.radius;
                line(e0 + r0, e0 + r1, col);
                line(e1 + r0, e1 + r1, col);
                if (k % 2 == 0) line(e0 + r0, e1 + r0, col);
            }
        }
    }
}

void DestroyNpcHitboxes(int id) {
    PhysicsState::Hitboxes* hp = HitboxesAt(id);
    if (!hp) return;
    auto& h = *hp;
    if (h.Active) ShadowCapsule(h, false);
    for (PxRigidDynamic* b : h.Bodies)
        if (b) {
            if (g_State->playerGround == b) g_State->playerGround = nullptr;
            g_State->hitboxActors.erase(b);
            g_State->bodyHitboxActors.erase(b);
            b->release();
        }
    h.Bodies.clear();
    h.Capsule = nullptr;
    h.Active = false;
}

bool RaycastBodyParts(const float origin[3], const float dir[3], float maxDistance, BodyPartHit& out) {
    out = BodyPartHit{};
    PxVec3 d;
    if (!g_State || !g_State->scene || maxDistance <= 0.0f || !NormalizedDir(dir, d)) return false;
    struct PartsOnly : PxQueryFilterCallback {
        PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor*, PxHitFlags&) override {
            return shape && shape->userData ? PxQueryHitType::eBLOCK : PxQueryHitType::eNONE;
        }
        PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override {
            return PxQueryHitType::eBLOCK;
        }
    } filter;
    // (Static too: a frozen corpse's parts, see FreezeRagdoll. Only tagged shapes pass the filter.)
    PxQueryFilterData fd(PxQueryFlag::eDYNAMIC | PxQueryFlag::eSTATIC | PxQueryFlag::ePREFILTER);
    PxRaycastBuffer buf;
    if (!g_State->scene->raycast(PxVec3(origin[0], origin[1], origin[2]), d, maxDistance, buf, PxHitFlag::eDEFAULT, fd, &filter) ||
        !buf.hasBlock || !buf.block.shape)
        return false;
    const unsigned tag = (unsigned)reinterpret_cast<std::uintptr_t>(buf.block.shape->userData);
    out.Kind = (int)(tag >> 16);
    out.Part = (int)(tag & 0xFFFFu);
    out.Entity = buf.block.actor ? UserDataToEntity(buf.block.actor->userData) : 0xFFFFFFFFu;
    out.Distance = buf.block.distance;
    out.Point[0] = buf.block.position.x; out.Point[1] = buf.block.position.y; out.Point[2] = buf.block.position.z;
    return true;
}
} // namespace PhysicsWorld
