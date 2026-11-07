#include "PhysicsInternal.h"

namespace PhysicsWorld {
namespace {
// ScopedQueryPolicy's stack (see the header). Empty = the default: skip the Player, hit NPCs.
struct QueryPolicy { std::uint32_t ignoreEntity = 0xFFFFFFFFu; bool hitPlayer = false; bool hitCharacters = true; };
thread_local std::vector<QueryPolicy> t_QueryPolicy;

// Whether the active policy drops `actor` from a query. `solidOnly`: the query is after the solid
// world, so characters never count.
bool PolicyRejects(const PxRigidActor* actor, bool solidOnly = false) {
    if (!actor || !g_State) return false;
    const QueryPolicy p = t_QueryPolicy.empty() ? QueryPolicy{} : t_QueryPolicy.back();
    if (actor == PlayerActor()) return !p.hitPlayer || solidOnly;
    if (p.ignoreEntity != 0xFFFFFFFFu && UserDataToEntity(actor->userData) == p.ignoreEntity) return true;
    if ((solidOnly || !p.hitCharacters) && g_State->npcActors.count(actor)) return true;
    // Hitboxes answer the unscoped queries (the player's); a scoped one (an NPC's) and the solid ones use the capsule.
    if (!g_State->hitboxActors.empty()) {
        if (g_State->hitboxActors.count(actor)) return solidOnly || !t_QueryPolicy.empty();
        if (t_QueryPolicy.empty() && !solidOnly && g_State->hitboxedCapsules.count(actor)) return true;
    }
    return false;
}

// Drop the Player capsule from scene queries — a "from the player" ray/sweep almost never
// wants to hit itself, and its shape sits right on the query origin (#185 hardening).
struct ExcludeActorFilter : PxQueryFilterCallback {
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor* actor,
                                   PxHitFlags&) override {
        if (shape && (shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE)) return PxQueryHitType::eNONE; // triggers aren't solid
        return PolicyRejects(actor) ? PxQueryHitType::eNONE : PxQueryHitType::eBLOCK;
    }
    PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*,
                                    const PxRigidActor*) override {
        return PxQueryHitType::eBLOCK;
    }
};
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld::detail {
const PxRigidActor* PlayerActor() {
    if (!g_State) return nullptr;
    return g_State->playerBody ? g_State->playerBody->Actor : nullptr;
}
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
bool Raycast(const float origin[3], const float dir[3], float maxDistance, RaycastHit& outHit) {
    outHit = RaycastHit{};
    if (!g_State || !g_State->scene) return false;

    PxVec3 d(dir[0], dir[1], dir[2]);
    const float len = d.magnitude();
    if (len < 1e-8f || maxDistance <= 0.0f) return false;
    d *= (1.0f / len);

    ExcludeActorFilter filter; // the Player is skipped through PolicyRejects (unless a policy says otherwise)
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER);
    PxRaycastBuffer buf;

    QueryRecord qr; qr.kind = 0;
    qr.origin = PxVec3(origin[0], origin[1], origin[2]);
    qr.dir = d; qr.dist = maxDistance;

    if (!g_State->scene->raycast(PxVec3(origin[0], origin[1], origin[2]), d, maxDistance, buf,
                                 PxHitFlag::eDEFAULT, fd, &filter) || !buf.hasBlock) {
        RecordQuery(qr);
        return false;
    }

    const PxRaycastHit& b = buf.block;
    outHit.Hit = true;
    outHit.Distance = b.distance;
    outHit.Point[0]  = b.position.x; outHit.Point[1]  = b.position.y; outHit.Point[2]  = b.position.z;
    outHit.Normal[0] = b.normal.x;   outHit.Normal[1] = b.normal.y;   outHit.Normal[2] = b.normal.z;
    outHit.Entity = b.actor ? UserDataToEntity(b.actor->userData) : outHit.Entity;
    qr.hit = true; qr.dist = b.distance;
    qr.hitPoint = b.position; qr.hitNormal = b.normal;
    RecordQuery(qr);
    return true;
}

// --- Shape queries (#185 PR 9) ----------------------------------------------------------

bool SphereCast(const float origin[3], const float dir[3], float radius, float maxDistance,
                RaycastHit& outHit) {
    outHit = RaycastHit{};
    if (!g_State || !g_State->scene || radius <= 0.0f || maxDistance <= 0.0f) return false;
    PxVec3 d(dir[0], dir[1], dir[2]);
    const float len = d.magnitude();
    if (len < 1e-8f) return false;
    d *= (1.0f / len);

    ExcludeActorFilter filter; // the Player is skipped through PolicyRejects (unless a policy says otherwise)
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER);
    PxSweepBuffer buf;
    const PxTransform pose(PxVec3(origin[0], origin[1], origin[2]));

    QueryRecord qr; qr.kind = 1; qr.radius = radius;
    qr.origin = PxVec3(origin[0], origin[1], origin[2]);
    qr.dir = d; qr.dist = maxDistance;

    if (!g_State->scene->sweep(PxSphereGeometry(radius), pose, d, maxDistance, buf,
                               PxHitFlag::eDEFAULT, fd, &filter) || !buf.hasBlock) {
        RecordQuery(qr);
        return false;
    }
    const PxSweepHit& b = buf.block;
    outHit.Hit = true;
    outHit.Distance = b.distance;
    outHit.Point[0]  = b.position.x; outHit.Point[1]  = b.position.y; outHit.Point[2]  = b.position.z;
    outHit.Normal[0] = b.normal.x;   outHit.Normal[1] = b.normal.y;   outHit.Normal[2] = b.normal.z;
    outHit.Entity = b.actor ? UserDataToEntity(b.actor->userData) : outHit.Entity;
    qr.hit = true; qr.dist = b.distance;
    qr.hitPoint = b.position; qr.hitNormal = b.normal;
    RecordQuery(qr);
    return true;
}

int OverlapSphere(const float center[3], float radius, unsigned* out, int maxEntities) {
    if (!g_State || !g_State->scene || radius <= 0.0f) return 0;
    static thread_local std::vector<PxOverlapHit> hits(64);
    PxOverlapBuffer buf;
    const PxTransform pose(PxVec3(center[0], center[1], center[2]));
    for (;;) { // grow until PhysX reports every overlap (it used to stop at 64)
        buf = PxOverlapBuffer(hits.data(), (PxU32)hits.size());
        if (!g_State->scene->overlap(PxSphereGeometry(radius), pose, buf)) return 0;
        if (buf.getNbTouches() >= (PxU32)hits.size() && hits.size() < 4096) { hits.resize(hits.size() * 2); continue; }
        break;
    }

    int count = 0;
    const PxRigidActor* player = PlayerActor();
    for (PxU32 i = 0; i < buf.getNbTouches(); ++i) {
        const PxOverlapHit& h = buf.getTouch(i);
        if (h.shape && (h.shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE)) continue; // solids only
        if (!h.actor || h.actor == player) continue;
        const unsigned id = UserDataToEntity(h.actor->userData);
        bool dup = false;
        for (int k = 0; k < count; ++k) if (out && k < maxEntities && out[k] == id) { dup = true; break; }
        if (dup) continue;
        if (out && count < maxEntities) out[count] = id;
        ++count;
    }
    if (g_QueryRecording) {
        QueryRecord qr; qr.kind = 2; qr.radius = radius;
        qr.origin = PxVec3(center[0], center[1], center[2]);
        qr.hit = count > 0;
        RecordQuery(qr);
    }
    return count;
}
} // namespace PhysicsWorld

namespace PhysicsWorld {
namespace {
// ==================================================================================
// #170 — filtered queries: layer mask + trigger interaction, RaycastAll, box / capsule shapes
// ==================================================================================

// Rejects the Player capsule, trigger shapes (unless asked for) and shapes whose layer bit
// (simulation filter word0, stamped at build time) isn't in the mask. `touchMode` turns every
// accepted hit into a touch, for the "all hits" queries.
struct LayerQueryFilter : PxQueryFilterCallback {
    PxU32 mask = 0xFFFFFFFFu;
    bool triggers = true;
    bool touchMode = false;
    bool skipSimulated = false; // ignore simulated (non-kinematic) dynamic bodies
    bool skipStartInside = false; // ignore a shape the ray starts inside (PhysX reports it at distance 0, normal -dir)
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor* actor,
                                   PxHitFlags&) override {
        if (PolicyRejects(actor, skipSimulated)) return PxQueryHitType::eNONE;
        if (skipSimulated && actor) {
            if (const PxRigidDynamic* dyn = actor->is<PxRigidDynamic>();
                dyn && !(dyn->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC))
                return PxQueryHitType::eNONE;
            if (actor->is<PxArticulationLink>()) return PxQueryHitType::eNONE; // a ragdoll's part
            if (shape && BodyPartKindOf(shape->userData) == kBodyPartRagdoll) return PxQueryHitType::eNONE; // a frozen one's
        }
        if (shape) {
            if (!triggers && (shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE)) return PxQueryHitType::eNONE;
            const PxU32 layerBit = shape->getSimulationFilterData().word0;
            if (layerBit && !(layerBit & mask)) return PxQueryHitType::eNONE;
        }
        return touchMode ? PxQueryHitType::eTOUCH : PxQueryHitType::eBLOCK;
    }
    PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit& hit, const PxShape*,
                                    const PxRigidActor*) override {
        if (skipStartInside && static_cast<const PxLocationHit&>(hit).hadInitialOverlap()) return PxQueryHitType::eNONE;
        return touchMode ? PxQueryHitType::eTOUCH : PxQueryHitType::eBLOCK;
    }
};

LayerQueryFilter MakeFilter(const QueryFilter& f, bool touchMode = false) {
    LayerQueryFilter lf; // the Player / self / characters are dropped by PolicyRejects
    lf.mask = f.LayerMask;
    lf.triggers = f.HitTriggers != 0;
    lf.touchMode = touchMode;
    return lf;
}
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld::detail {
bool NormalizedDir(const float dir[3], PxVec3& out) {
    out = PxVec3(dir[0], dir[1], dir[2]);
    const float len = out.magnitude();
    if (len < 1e-8f) return false;
    out *= 1.0f / len;
    return true;
}

PxQuat ToQuat(const float rotation[4]) {
    if (!rotation) return PxQuat(PxIdentity);
    PxQuat q(rotation[0], rotation[1], rotation[2], rotation[3]);
    return q.magnitudeSquared() > 1e-12f ? q.getNormalized() : PxQuat(PxIdentity);
}
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
namespace {
template <typename HitT>
void FillHit(const HitT& b, RaycastHit& outHit) {
    outHit.Hit = true;
    outHit.Distance = b.distance;
    outHit.Point[0]  = b.position.x; outHit.Point[1]  = b.position.y; outHit.Point[2]  = b.position.z;
    outHit.Normal[0] = b.normal.x;   outHit.Normal[1] = b.normal.y;   outHit.Normal[2] = b.normal.z;
    outHit.Entity = b.actor ? UserDataToEntity(b.actor->userData) : outHit.Entity;
}

bool SweepFiltered(const PxGeometry& geom, const PxTransform& pose, const float dir[3], float maxDistance,
                   const QueryFilter& f, RaycastHit& outHit, bool skipSimulated = false) {
    outHit = RaycastHit{};
    PxVec3 d;
    if (!g_State || !g_State->scene || maxDistance <= 0.0f || !NormalizedDir(dir, d)) return false;
    LayerQueryFilter lf = MakeFilter(f);
    lf.skipSimulated = skipSimulated;
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER);
    PxSweepBuffer buf;
    if (!g_State->scene->sweep(geom, pose, d, maxDistance, buf, PxHitFlag::eDEFAULT, fd, &lf) || !buf.hasBlock)
        return false;
    FillHit(buf.block, outHit);
    return true;
}
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld::detail {
int OverlapFiltered(const PxGeometry& geom, const PxTransform& pose, const QueryFilter& f,
                    unsigned* out, int maxEntities) {
    if (!g_State || !g_State->scene) return 0;
    static thread_local std::vector<PxOverlapHit> hits(256);
    PxOverlapBuffer buf;
    LayerQueryFilter lf = MakeFilter(f, /*touchMode=*/true);
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER |
                         PxQueryFlag::eNO_BLOCK);
    for (;;) { // grow until PhysX reports every overlap
        buf = PxOverlapBuffer(hits.data(), (PxU32)hits.size());
        if (!g_State->scene->overlap(geom, pose, buf, fd, &lf)) return 0;
        if (buf.getNbTouches() >= (PxU32)hits.size() && hits.size() < 4096) { hits.resize(hits.size() * 2); continue; }
        break;
    }
    int count = 0;
    std::vector<unsigned> seen;
    for (PxU32 i = 0; i < buf.getNbTouches(); ++i) {
        const PxOverlapHit& h = buf.getTouch(i);
        if (!h.actor) continue;
        const unsigned id = UserDataToEntity(h.actor->userData);
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue; // one entry per entity
        seen.push_back(id);
        if (out && count < maxEntities) out[count] = id;
        ++count;
    }
    return count;
}
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
bool RaycastFiltered(const float origin[3], const float dir[3], float maxDistance, const QueryFilter& f,
                     RaycastHit& outHit) {
    outHit = RaycastHit{};
    PxVec3 d;
    if (!g_State || !g_State->scene || maxDistance <= 0.0f || !NormalizedDir(dir, d)) return false;
    LayerQueryFilter lf = MakeFilter(f);
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER);
    PxRaycastBuffer buf;
    const PxVec3 o(origin[0], origin[1], origin[2]);
    QueryRecord qr; qr.kind = 0; qr.origin = o; qr.dir = d; qr.dist = maxDistance;
    if (!g_State->scene->raycast(o, d, maxDistance, buf, PxHitFlag::eDEFAULT, fd, &lf) || !buf.hasBlock) {
        RecordQuery(qr);
        return false;
    }
    FillHit(buf.block, outHit);
    qr.hit = true; qr.dist = buf.block.distance; qr.hitPoint = buf.block.position; qr.hitNormal = buf.block.normal;
    RecordQuery(qr);
    return true;
}

bool RaycastSolid(const float origin[3], const float dir[3], float maxDistance, const QueryFilter& f,
                  RaycastHit& outHit, bool fromOutside) {
    outHit = RaycastHit{};
    PxVec3 d;
    if (!g_State || !g_State->scene || maxDistance <= 0.0f || !NormalizedDir(dir, d)) return false;
    LayerQueryFilter lf = MakeFilter(f);
    lf.skipSimulated = true;
    lf.skipStartInside = fromOutside;
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER |
                         (fromOutside ? PxQueryFlags(PxQueryFlag::ePOSTFILTER) : PxQueryFlags()));
    PxRaycastBuffer buf;
    if (!g_State->scene->raycast(PxVec3(origin[0], origin[1], origin[2]), d, maxDistance, buf, PxHitFlag::eDEFAULT, fd, &lf) ||
        !buf.hasBlock)
        return false;
    FillHit(buf.block, outHit);
    return true;
}

bool RaycastStatic(const float origin[3], const float dir[3], float maxDistance, const QueryFilter& f, RaycastHit& outHit) {
    outHit = RaycastHit{};
    PxVec3 d;
    if (!g_State || !g_State->scene || maxDistance <= 0.0f || !NormalizedDir(dir, d)) return false;
    LayerQueryFilter lf = MakeFilter(f);
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::ePREFILTER);
    PxRaycastBuffer buf;
    if (!g_State->scene->raycast(PxVec3(origin[0], origin[1], origin[2]), d, maxDistance, buf, PxHitFlag::eDEFAULT, fd, &lf) ||
        !buf.hasBlock)
        return false;
    FillHit(buf.block, outHit);
    return true;
}

int RaycastAll(const float origin[3], const float dir[3], float maxDistance, const QueryFilter& f,
               RaycastHit* out, int maxHits) {
    PxVec3 d;
    if (!g_State || !g_State->scene || maxDistance <= 0.0f || !NormalizedDir(dir, d)) return 0;
    static thread_local std::vector<PxRaycastHit> hits(256);
    PxRaycastBuffer buf;
    LayerQueryFilter lf = MakeFilter(f, /*touchMode=*/true);
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER |
                         PxQueryFlag::eNO_BLOCK);
    for (;;) { // grow until PhysX reports every hit
        buf = PxRaycastBuffer(hits.data(), (PxU32)hits.size());
        g_State->scene->raycast(PxVec3(origin[0], origin[1], origin[2]), d, maxDistance, buf,
                                PxHitFlag::eDEFAULT, fd, &lf);
        if (buf.getNbTouches() >= (PxU32)hits.size() && hits.size() < 4096) { hits.resize(hits.size() * 2); continue; }
        break;
    }
    const PxU32 n = buf.getNbTouches();
    std::vector<const PxRaycastHit*> sorted;
    sorted.reserve(n);
    for (PxU32 i = 0; i < n; ++i) sorted.push_back(&buf.getTouch(i));
    std::sort(sorted.begin(), sorted.end(), [](const PxRaycastHit* a, const PxRaycastHit* b) { return a->distance < b->distance; });
    for (int i = 0; out && i < (int)sorted.size() && i < maxHits; ++i) {
        out[i] = RaycastHit{};
        FillHit(*sorted[(size_t)i], out[i]);
    }
    return (int)n;
}

bool SphereCastFiltered(const float origin[3], const float dir[3], float radius, float maxDistance,
                        const QueryFilter& f, RaycastHit& outHit) {
    outHit = RaycastHit{};
    if (radius <= 0.0f) return false;
    return SweepFiltered(PxSphereGeometry(radius), PxTransform(PxVec3(origin[0], origin[1], origin[2])),
                         dir, maxDistance, f, outHit);
}

bool SphereCastSolid(const float origin[3], const float dir[3], float radius, float maxDistance,
                     const QueryFilter& f, RaycastHit& outHit) {
    outHit = RaycastHit{};
    if (radius <= 0.0f) return false;
    return SweepFiltered(PxSphereGeometry(radius), PxTransform(PxVec3(origin[0], origin[1], origin[2])),
                         dir, maxDistance, f, outHit, /*skipSimulated=*/true);
}

bool BoxCast(const float center[3], const float halfExtents[3], const float rotation[4], const float dir[3],
             float maxDistance, const QueryFilter& f, RaycastHit& outHit) {
    outHit = RaycastHit{};
    const PxVec3 he(std::max(halfExtents[0], 1e-4f), std::max(halfExtents[1], 1e-4f), std::max(halfExtents[2], 1e-4f));
    return SweepFiltered(PxBoxGeometry(he), PxTransform(PxVec3(center[0], center[1], center[2]), ToQuat(rotation)),
                         dir, maxDistance, f, outHit);
}

bool CapsuleCast(const float point1[3], const float point2[3], float radius, const float dir[3],
                 float maxDistance, const QueryFilter& f, RaycastHit& outHit) {
    outHit = RaycastHit{};
    if (radius <= 0.0f) return false;
    const PxVec3 a(point1[0], point1[1], point1[2]), b(point2[0], point2[1], point2[2]);
    PxVec3 axis = b - a;
    const float len = axis.magnitude();
    if (len < 1e-5f) return SphereCastFiltered(point1, dir, radius, maxDistance, f, outHit); // degenerate
    axis *= 1.0f / len;
    // PhysX capsules lie along local +X: rotate X onto the segment.
    const PxVec3 x(1.0f, 0.0f, 0.0f);
    const PxVec3 c = x.cross(axis);
    const float w = 1.0f + x.dot(axis);
    PxQuat q = w < 1e-6f ? PxQuat(PxPi, PxVec3(0.0f, 1.0f, 0.0f)) : PxQuat(c.x, c.y, c.z, w).getNormalized();
    return SweepFiltered(PxCapsuleGeometry(radius, len * 0.5f), PxTransform((a + b) * 0.5f, q),
                         dir, maxDistance, f, outHit);
}

int OverlapSphereFiltered(const float center[3], float radius, const QueryFilter& f, unsigned* out, int maxEntities) {
    if (radius <= 0.0f) return 0;
    return OverlapFiltered(PxSphereGeometry(radius), PxTransform(PxVec3(center[0], center[1], center[2])),
                           f, out, maxEntities);
}

bool OverlapSphereStatic(const float center[3], float radius, const QueryFilter& f) {
    if (!g_State || !g_State->scene || radius <= 0.0f) return false;
    PxOverlapHit hits[8];
    PxOverlapBuffer buf(hits, 8);
    LayerQueryFilter lf = MakeFilter(f, /*touchMode=*/true);
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::ePREFILTER | PxQueryFlag::eNO_BLOCK);
    g_State->scene->overlap(PxSphereGeometry(radius), PxTransform(PxVec3(center[0], center[1], center[2])), buf, fd, &lf);
    return buf.getNbTouches() > 0;
}

int OverlapBox(const float center[3], const float halfExtents[3], const float rotation[4], const QueryFilter& f,
               unsigned* out, int maxEntities) {
    const PxVec3 he(std::max(halfExtents[0], 1e-4f), std::max(halfExtents[1], 1e-4f), std::max(halfExtents[2], 1e-4f));
    return OverlapFiltered(PxBoxGeometry(he), PxTransform(PxVec3(center[0], center[1], center[2]), ToQuat(rotation)),
                           f, out, maxEntities);
}


ScopedQueryPolicy::ScopedQueryPolicy(unsigned ignoreEntity, bool hitPlayer, bool hitCharacters) {
    t_QueryPolicy.push_back(QueryPolicy{ignoreEntity, hitPlayer, hitCharacters});
}
ScopedQueryPolicy::~ScopedQueryPolicy() {
    if (!t_QueryPolicy.empty()) t_QueryPolicy.pop_back();
}
} // namespace PhysicsWorld

namespace PhysicsWorld {
namespace {
// PhysX combines two materials' values with the higher-priority of their two combine modes
// (eAVERAGE < eMIN < eMULTIPLY < eMAX).
float CombineMaterialValue(float a, float b, PxCombineMode::Enum ma, PxCombineMode::Enum mb) {
    switch (std::max(ma, mb)) {
        case PxCombineMode::eMIN:      return std::min(a, b);
        case PxCombineMode::eMULTIPLY: return a * b;
        case PxCombineMode::eMAX:      return std::max(a, b);
        default:                       return 0.5f * (a + b);
    }
}

struct IgnoreSelfFilter : PxQueryFilterCallback {
    const PxRigidActor* self = nullptr;
    const PxRigidActor* player = nullptr;
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor* actor,
                                   PxHitFlags&) override {
        if (actor == self || actor == player) return PxQueryHitType::eNONE;
        if (shape && (shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE)) return PxQueryHitType::eNONE;
        return PxQueryHitType::eBLOCK;
    }
    PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override {
        return PxQueryHitType::eBLOCK;
    }
};
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld {
bool SweepBody(unsigned entity, const float from[3], const float dir[3], float distance, RaycastHit& outHit,
               float& outBounciness, float& outFriction) {
    outHit = RaycastHit{};
    if (!g_State || !g_State->scene || distance <= 0.0f) return false;
    PxRigidDynamic* b = DynamicFor(entity);
    PxShape* shape = nullptr;
    if (!b || b->getNbShapes() < 1 || b->getShapes(&shape, 1) != 1 || !shape) return false;
    PxVec3 d(dir[0], dir[1], dir[2]);
    if (d.magnitudeSquared() < 1e-12f) return false;
    d.normalize();

    const PxTransform actorPose(PxVec3(from[0], from[1], from[2]), b->getGlobalPose().q);
    const PxTransform shapePose = actorPose * shape->getLocalPose();
    IgnoreSelfFilter filter;
    filter.self = b;
    filter.player = PlayerActor();
    PxQueryFilterData fd;
    fd.flags = PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER;
    PxSweepBuffer buf;
    // Assume no initial overlap: the body starts where it's held, possibly touching something.
    const PxHitFlags hitFlags = PxHitFlag::eDEFAULT | PxHitFlag::eASSUME_NO_INITIAL_OVERLAP;
    if (!g_State->scene->sweep(shape->getGeometry(), shapePose, d, distance, buf, hitFlags, fd, &filter) ||
        !buf.hasBlock)
        return false;
    const PxSweepHit& h = buf.block;
    outHit.Hit = true;
    outHit.Distance = h.distance;
    outHit.Point[0] = h.position.x; outHit.Point[1] = h.position.y; outHit.Point[2] = h.position.z;
    outHit.Normal[0] = h.normal.x;  outHit.Normal[1] = h.normal.y;  outHit.Normal[2] = h.normal.z;
    outHit.Entity = h.actor ? UserDataToEntity(h.actor->userData) : 0xFFFFFFFFu;

    PxMaterial* ma = nullptr;
    PxMaterial* mb = nullptr;
    shape->getMaterials(&ma, 1);
    if (h.shape) h.shape->getMaterials(&mb, 1);
    if (ma && mb) {
        outBounciness = CombineMaterialValue(ma->getRestitution(), mb->getRestitution(),
                                             ma->getRestitutionCombineMode(), mb->getRestitutionCombineMode());
        outFriction = CombineMaterialValue(ma->getDynamicFriction(), mb->getDynamicFriction(),
                                           ma->getFrictionCombineMode(), mb->getFrictionCombineMode());
    } else {
        outBounciness = 0.0f;
        outFriction = 0.6f;
    }
    return true;
}
} // namespace PhysicsWorld
