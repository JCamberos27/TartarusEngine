#include "PhysicsInternal.h"

namespace {
// The legacy AABB collider size (World.cpp's ColliderWorldBounds, kept in step): the entity's
// render bounds scaled by its transform, or a plain unit box for a mesh-less entity. Returned
// as centre + half-extents in world space, axis-aligned.
void AutoBoxWorld(const entt::registry& reg, entt::entity e, const TransformComponent& t,
                  glm::vec3& outCenter, glm::vec3& outHalf) {
    if (const auto* r = reg.try_get<RenderableComponent>(e); r && r->ModelRef) {
        glm::vec3 a = t.Position + r->ModelRef->BoundsMin() * t.Scale;
        glm::vec3 b = t.Position + r->ModelRef->BoundsMax() * t.Scale;
        outCenter = 0.5f * (a + b);
        outHalf   = 0.5f * glm::abs(b - a); // abs guards a negative Scale flipping the corners
    } else {
        outCenter = t.Position;
        outHalf   = 0.5f * glm::abs(t.Scale);
    }
}

// Cook a convex hull / triangle mesh straight into PxPhysics (immediate insertion — no
// serialize round-trip). Returns null on failure (empty geometry, cook rejected).
PxConvexMesh* CookConvex(PxPhysics& physics, const std::vector<glm::vec3>& verts) {
    if (verts.size() < 4) return nullptr;
    PxConvexMeshDesc d;
    d.points.count  = (PxU32)verts.size();
    d.points.stride = sizeof(glm::vec3);
    d.points.data   = verts.data();
    d.flags         = PxConvexFlag::eCOMPUTE_CONVEX;
    d.vertexLimit   = (PxU16)255; // PhysX hull cap; a denser source is decimated to fit
    PxCookingParams params(physics.getTolerancesScale());
    return PxCreateConvexMesh(params, d, physics.getPhysicsInsertionCallback());
}
PxTriangleMesh* CookTriangle(PxPhysics& physics, const std::vector<glm::vec3>& verts,
                             const std::vector<unsigned int>& indices) {
    if (verts.size() < 3 || indices.size() < 3) return nullptr;
    PxTriangleMeshDesc d;
    d.points.count     = (PxU32)verts.size();
    d.points.stride    = sizeof(glm::vec3);
    d.points.data      = verts.data();
    d.triangles.count  = (PxU32)(indices.size() / 3);
    d.triangles.stride = 3 * sizeof(unsigned int);
    d.triangles.data   = indices.data();
    PxCookingParams params(physics.getTolerancesScale());
    return PxCreateTriangleMesh(params, d, physics.getPhysicsInsertionCallback());
}

// One PxMaterial per distinct (friction, static friction, bounciness, combine modes), values
// rounded to 1/100 so a slider wobble doesn't spawn hundreds (#185 PR 7, #170 / #204).
PxMaterial* GetMaterial(PhysicsState& s, const ColliderComponent& c) {
    const float friction   = std::max(0.0f, c.Friction);
    const float staticFr   = std::max(0.0f, c.StaticFriction);
    const float bounciness = std::min(1.0f, std::max(0.0f, c.Bounciness));
    const int frictionCombine = std::clamp(c.FrictionCombine, 0, 3);
    const int bounceCombine   = std::clamp(c.BounceCombine, 0, 3);
    const long long key = (long long)std::lround(friction * 100.0f) |
                          ((long long)std::lround(staticFr * 100.0f) << 16) |
                          ((long long)std::lround(bounciness * 100.0f) << 32) |
                          ((long long)frictionCombine << 48) | ((long long)bounceCombine << 52);
    auto it = s.materialCache.find(key);
    if (it != s.materialCache.end()) return it->second;
    PxMaterial* m = s.physics->createMaterial(staticFr, friction, bounciness);
    // PxCombineMode is eAVERAGE, eMIN, eMULTIPLY, eMAX - the order ColliderComponent stores.
    m->setFrictionCombineMode((PxCombineMode::Enum)frictionCombine);
    m->setRestitutionCombineMode((PxCombineMode::Enum)bounceCombine);
    s.materialCache.emplace(key, m);
    return m;
}
} // namespace

namespace PhysicsWorld::detail {
// `only` (#166 / #201): build just these entities - ones spawned, re-activated or given a
// Collider while playing (see SyncEntities). Null = every active collider (Play-enter).
void BuildActors(PhysicsState& s, const World& world, const std::unordered_set<std::uint32_t>* only) {
    int statics = 0, dynamic = 0, kinematic = 0, skipped = 0;
    s.orderByEntity.clear();
    for (auto [e, order] : world.Registry.view<const OrderComponent>().each())
        s.orderByEntity[entt::to_integral(e)] = order.Value;
    auto view = world.Registry.view<const TransformComponent, const ColliderComponent>(entt::exclude<InactiveTag>);
    for (entt::entity e : view) {
        if (only && !only->count(entt::to_integral(e))) continue;
        s.attempted.insert(entt::to_integral(e)); // built or skipped: either way, not retried every frame
        // #114 — world space: a collider on a child entity used to be built at its LOCAL
        // offset from the world origin.
        const TransformComponent t = world.WorldSpaceTransform(e);
        const auto& c  = view.get<const ColliderComponent>(e);
        const auto* rb = world.Registry.try_get<const RigidbodyComponent>(e); // null => static (PR 2)

        const PxShapeFlags flags = c.IsTrigger
            ? (PxShapeFlag::eTRIGGER_SHAPE | PxShapeFlag::eSCENE_QUERY_SHAPE)
            : (PxShapeFlag::eSIMULATION_SHAPE | PxShapeFlag::eSCENE_QUERY_SHAPE);

        PxTransform actorPose(PxIdentity);
        PxTransform shapeLocal(PxIdentity);
        PxShape* shape = nullptr;
        PxMaterial* mat = GetMaterial(s, ResolvePhysicMaterial(c)); // #185 PR 7, #170 combine modes + Physic Material asset
        auto make = [&](const PxGeometry& g) {
            shape = s.physics->createShape(g, *mat, /*isExclusive=*/true, flags);
        };

        const float sx = std::abs(t.Scale.x), sy = std::abs(t.Scale.y), sz = std::abs(t.Scale.z);
        const bool meshKind = (c.Kind == ColliderComponent::Shape::ConvexHull ||
                               c.Kind == ColliderComponent::Shape::Mesh);

        if (meshKind) {
            // Cook a hull / triangle mesh from the entity's render geometry (#185 PR 6).
            const auto* rc = world.Registry.try_get<const RenderableComponent>(e);
            std::vector<glm::vec3> verts;
            std::vector<unsigned int> idx;
            if (rc && rc->ModelRef) rc->ModelRef->CollisionGeometry(verts, idx);
            if (verts.size() < 4) {
                Log::Warn("PhysX: " + EntityLogRef(world.Registry, e) +
                          " has a mesh collider but no usable mesh — skipped.", EntityLogContext(world.Registry, e));
                ++skipped;
                continue;
            }
            actorPose  = PxTransform(ToPx(t.Position), QuatToPx(t.Rotation));
            shapeLocal = PxTransform(ToPx(c.Center));
            const PxMeshScale meshScale(ToPx(glm::abs(t.Scale)));

            // A triangle mesh can't be on a non-kinematic dynamic body (PhysX restriction) —
            // fall back to a convex hull for that case.
            const bool dynamicNonKin = rb && !rb->IsKinematic;
            const bool useTriangle = (c.Kind == ColliderComponent::Shape::Mesh) && !dynamicNonKin;
            if (c.Kind == ColliderComponent::Shape::Mesh && dynamicNonKin)
                Log::Warn("PhysX: " + EntityLogRef(world.Registry, e) +
                          " — a triangle-mesh collider can't be dynamic; using a convex hull.", EntityLogContext(world.Registry, e));

            // Cook once per model for the editor session (#185 PR 12, #167). A cooked mesh is
            // scale-independent (scale rides on the geometry's PxMeshScale), so the key is the
            // model's identity: path, file timestamp and geometry size, which all change when
            // the model is reimported.
            std::string key = (rc && rc->ModelRef) ? rc->ModelRef->Path() : std::string();
            {
                std::error_code ec;
                const auto stamp = std::filesystem::last_write_time(key, ec);
                key += "|" + std::to_string(ec ? 0 : (long long)stamp.time_since_epoch().count()) +
                       "|" + std::to_string(verts.size()) + "|" + std::to_string(idx.size());
            }
            if (useTriangle) {
                PxTriangleMesh*& tm = s.triangleCache[key];
                if (!tm) tm = CookTriangle(*s.physics, verts, idx);
                if (!tm) { s.triangleCache.erase(key); ++skipped; continue; }
                make(PxTriangleMeshGeometry(tm, meshScale));
            } else {
                PxConvexMesh*& cm = s.convexCache[key];
                if (!cm) cm = CookConvex(*s.physics, verts); // TEMP
                if (!cm) {
                    s.convexCache.erase(key);
                    // Better a rough box than no collider at all (#185 hardening).
                    glm::vec3 center, half;
                    AutoBoxWorld(world.Registry, e, t, center, half);
                    if (half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f) { ++skipped; continue; }
                    Log::Warn("PhysX: " + EntityLogRef(world.Registry, e) +
                              " — convex cook failed (mesh too dense) — using a bounds box.", EntityLogContext(world.Registry, e));
                    actorPose  = PxTransform(ToPx(t.Position), QuatToPx(t.Rotation));
                    shapeLocal = PxTransform(ToPx(center - t.Position));
                    make(PxBoxGeometry(ToPx(half)));
                } else {
                    make(PxConvexMeshGeometry(cm, meshScale));
                }
            }
        } else if (c.HalfExtents == glm::vec3(0.0f)) {
            // Auto: an axis-aligned box the size of the render bounds.
            glm::vec3 center, half;
            AutoBoxWorld(world.Registry, e, t, center, half);
            if (half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f) { ++skipped; continue; }
            make(PxBoxGeometry(ToPx(half)));
            // Pose at the entity origin (with its rotation) and the box's local offset as the
            // shape pose — for dynamic bodies so the simulated pose writes straight back to
            // TransformComponent, and (#115) for statics too: they used an unrotated box at the
            // AABB centre, so a box rotated into a ramp/wall collided as an axis-aligned block
            // that didn't match what was drawn.
            actorPose  = PxTransform(ToPx(t.Position), QuatToPx(t.Rotation));
            shapeLocal = PxTransform(ToPx(center - t.Position));
        } else {
            actorPose  = PxTransform(ToPx(t.Position), QuatToPx(t.Rotation));
            shapeLocal = PxTransform(ToPx(c.Center));
            switch (c.Kind) {
                case ColliderComponent::Shape::Box: {
                    glm::vec3 h = c.HalfExtents * glm::abs(t.Scale);
                    if (h.x <= 0.0f || h.y <= 0.0f || h.z <= 0.0f) { ++skipped; continue; }
                    make(PxBoxGeometry(ToPx(h)));
                    break;
                }
                case ColliderComponent::Shape::Sphere: {
                    float r = c.HalfExtents.x * std::max({sx, sy, sz});
                    if (r <= 0.0f) { ++skipped; continue; }
                    make(PxSphereGeometry(r));
                    break;
                }
                case ColliderComponent::Shape::Capsule: {
                    // PhysX capsules run along local X; author them along Y by rotating the shape
                    // 90 deg about Z. Radius = max horizontal scale, half-height = vertical.
                    float r  = c.HalfExtents.x * std::max(sx, sz);
                    float hh = c.HalfExtents.y * sy;
                    if (r <= 0.0f || hh <= 0.0f) { ++skipped; continue; }
                    shapeLocal = shapeLocal * PxTransform(PxQuat(PxHalfPi, PxVec3(0, 0, 1)));
                    make(PxCapsuleGeometry(r, hh));
                    break;
                }
                default: break; // ConvexHull / Mesh handled above (meshKind)
            }
        }
        if (!shape) { ++skipped; continue; }
        shape->setLocalPose(shapeLocal);

        // #185 PR 8 — stamp the layer (word1) so EngineFilterShader can apply the matrix.
        const auto* lc = world.Registry.try_get<const LayerComponent>(e);
        const PxU32 layer = (lc && lc->Layer >= 0 && lc->Layer < 32) ? (PxU32)lc->Layer : 0u;
        shape->setSimulationFilterData(PxFilterData(1u << layer, layer, 0u, 0u));

        if (!rb) {
            PxRigidStatic* a = s.physics->createRigidStatic(actorPose);
            a->attachShape(*shape);
            a->userData = EntityToUserData(e);
            shape->release();
            s.scene->addActor(*a);
            s.staticByEntity[entt::to_integral(e)] = a; // #185 hardening — joints can target statics
            ++statics;
            continue;
        }

        PxRigidDynamic* b = s.physics->createRigidDynamic(actorPose);
        b->attachShape(*shape);
        // #169 - ProjectSettings' Solver Iterations was stored and shown but never applied.
        b->setSolverIterationCounts((PxU32)std::clamp(ProjectSettings::Physics().SolverIterations, 1, 255));
        b->userData = EntityToUserData(e);
        shape->release();
        if (!c.IsTrigger) // setMassAndUpdateInertia needs a simulation shape
            PxRigidBodyExt::setMassAndUpdateInertia(*b, rb->Mass > 0.0f ? rb->Mass : 1.0f);
        b->setLinearDamping(std::max(0.0f, rb->LinearDamping));
        b->setAngularDamping(std::max(0.0f, rb->AngularDamping));
        b->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !rb->UseGravity);
        if (rb->ContinuousCollision && !rb->IsKinematic)
            b->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true); // #185 PR 9
        if (rb->IsKinematic) {
            b->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
            s.kinematics.push_back({b, e});
            ++kinematic;
        } else {
            b->setLinearVelocity(ToPx(rb->InitialVelocity));
            // #185 hardening — per-axis constraints.
            PxRigidDynamicLockFlags lock = PxRigidDynamicLockFlags(0);
            if (rb->FreezePositionX) lock |= PxRigidDynamicLockFlag::eLOCK_LINEAR_X;
            if (rb->FreezePositionY) lock |= PxRigidDynamicLockFlag::eLOCK_LINEAR_Y;
            if (rb->FreezePositionZ) lock |= PxRigidDynamicLockFlag::eLOCK_LINEAR_Z;
            if (rb->FreezeRotationX) lock |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_X;
            if (rb->FreezeRotationY) lock |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Y;
            if (rb->FreezeRotationZ) lock |= PxRigidDynamicLockFlag::eLOCK_ANGULAR_Z;
            if (lock) b->setRigidDynamicLockFlags(lock);
            s.dynamics.push_back({b, e});
            ++dynamic;
        }
        s.bodyByEntity[entt::to_integral(e)] = b; // #185 PR 7 — force API + read-back lookup
        s.scene->addActor(*b);
    }

    std::string msg = "PhysX: " + std::string(only ? "added " : "") + std::to_string(statics) + " static, " +
                      std::to_string(dynamic) + " dynamic, " + std::to_string(kinematic) + " kinematic collider(s)";
    if (skipped) msg += "; " + std::to_string(skipped) + " skipped (degenerate size)";
    Log::Info(msg + (only ? " while playing." : "."));
}
} // namespace PhysicsWorld::detail

namespace {
// #166 / #201 - drops one entity's actor (and every joint attached to it) from the scene, with
// all the per-body bookkeeping that points at it.
void RemoveActor(PhysicsState& s, std::uint32_t id) {
    PxRigidActor* actor = nullptr;
    if (auto it = s.bodyByEntity.find(id); it != s.bodyByEntity.end()) {
        PxRigidDynamic* b = it->second;
        actor = b;
        s.bodyByEntity.erase(it);
        auto drop = [b](std::vector<std::pair<PxRigidDynamic*, entt::entity>>& v) {
            v.erase(std::remove_if(v.begin(), v.end(), [b](const auto& p) { return p.first == b; }), v.end());
        };
        drop(s.dynamics);
        drop(s.kinematics);
        s.prevPose.Erase(b);
        s.settled.erase(b);
        s.kinematicFramePose.Erase(b);
        s.preStepVel.Erase(b);
        s.jointed.erase(b);
        if (s.grabbed == b) { s.grabbed = nullptr; s.grabHasTarget = false; s.grabHasRotation = false; }
        if (s.spunBody == b) s.spunBody = nullptr;
        if (s.playerGround == b) s.playerGround = nullptr;
    } else if (auto st = s.staticByEntity.find(id); st != s.staticByEntity.end()) {
        actor = st->second;
        s.staticByEntity.erase(st);
    }
    if (!actor) return;
    for (PxJoint*& j : s.joints) {
        if (!j) continue;
        PxRigidActor *a0 = nullptr, *a1 = nullptr;
        j->getActors(a0, a1);
        if (a0 == actor || a1 == actor) {
            s.jointOwner.erase(j);
            j->release();
            j = nullptr;
        }
    }
    s.joints.erase(std::remove(s.joints.begin(), s.joints.end(), nullptr), s.joints.end());
    // The game still hears about the pairs that end because an actor went away.
    for (auto it = s.triggerOverlaps.begin(); it != s.triggerOverlaps.end();) {
        if (it->first == id || it->second == id) {
            PushTriggerEvent(s, TriggerEvent::Exit, it->first, it->second);
            it = s.triggerOverlaps.erase(it);
        } else {
            ++it;
        }
    }
    if (s.playerTriggers.erase(id)) PushTriggerEvent(s, TriggerEvent::Exit, id, kPlayerEntity);
    s.scene->removeActor(*actor);
    actor->release();
}
} // namespace

namespace PhysicsWorld::detail {
// #166 / #201 - keeps the PhysX scene in step with the World while playing: actors go away for
// entities that were destroyed, deactivated or lost their Collider, and are built for ones that
// were spawned, re-activated or given a Collider. (Before, Play-enter was the only sync, so a
// deleted or deactivated object left an invisible collider behind and a duplicated one fell
// through nothing.)
// Joints are not re-made here: BuildJoints walks every JointComponent at Play-enter and would
// duplicate the existing ones, so an entity that gains a collider mid-Play gets an actor but no
// joint. Nothing can spawn one yet; revisit with Instantiate (#166).
void SyncEntities(PhysicsState& s, const World& world) {
    std::vector<std::uint32_t> gone;
    for (std::uint32_t id : s.attempted) {
        const entt::entity e = static_cast<entt::entity>(id);
        if (!world.Registry.valid(e) || !world.Registry.all_of<TransformComponent, ColliderComponent>(e) ||
            world.Registry.all_of<InactiveTag>(e))
            gone.push_back(id);
    }
    for (std::uint32_t id : gone) {
        RemoveActor(s, id);
        s.attempted.erase(id);
    }
    std::unordered_set<std::uint32_t> added;
    for (entt::entity e : world.Registry.view<const TransformComponent, const ColliderComponent>(entt::exclude<InactiveTag>))
        if (!s.attempted.count(entt::to_integral(e))) added.insert(entt::to_integral(e));
    if (!added.empty()) BuildActors(s, world, &added);
}
} // namespace PhysicsWorld::detail

namespace {
// --- Joints (#185 PR 11) ---------------------------------------------------------------

// Rotate a joint's default axis (local X) onto `axis`.
PxQuat AxisToLocalFrame(const glm::vec3& axis) {
    glm::vec3 a = axis;
    if (glm::dot(a, a) < 1e-8f) a = glm::vec3(1, 0, 0);
    a = glm::normalize(a);
    return PxShortestRotation(PxVec3(1, 0, 0), PxVec3(a.x, a.y, a.z));
}

void ApplyJointLimits(PxJoint* joint, const JointComponent& j, const PxTolerancesScale& scale) {
    if (!j.UseLimit) return;
    const float lo = std::min(j.LimitLower, j.LimitUpper);
    const float hi = std::max(j.LimitLower, j.LimitUpper);
    switch (j.Kind) {
        case JointComponent::Type::Hinge: {
            auto* r = static_cast<PxRevoluteJoint*>(joint);
            r->setLimit(PxJointAngularLimitPair(glm::radians(lo), glm::radians(hi)));
            r->setRevoluteJointFlag(PxRevoluteJointFlag::eLIMIT_ENABLED, true);
            break;
        }
        case JointComponent::Type::Slider: {
            auto* p = static_cast<PxPrismaticJoint*>(joint);
            p->setLimit(PxJointLinearLimitPair(scale, lo, hi));
            p->setPrismaticJointFlag(PxPrismaticJointFlag::eLIMIT_ENABLED, true);
            break;
        }
        case JointComponent::Type::Distance: {
            auto* d = static_cast<PxDistanceJoint*>(joint);
            d->setMinDistance(std::max(0.0f, lo));
            d->setMaxDistance(std::max(std::max(0.0f, lo), hi));
            d->setDistanceJointFlag(PxDistanceJointFlag::eMIN_DISTANCE_ENABLED, true);
            d->setDistanceJointFlag(PxDistanceJointFlag::eMAX_DISTANCE_ENABLED, true);
            break;
        }
        default: break; // Fixed / Ball — no limit
    }
}
} // namespace

namespace PhysicsWorld::detail {
PxRigidActor* ActorFor(const PhysicsState& s, std::uint32_t entity) {
    if (auto it = s.bodyByEntity.find(entity); it != s.bodyByEntity.end()) return it->second;
    if (auto st = s.staticByEntity.find(entity); st != s.staticByEntity.end()) return st->second;
    return nullptr;
}

std::vector<PxShape*> ShapesOf(const PxRigidActor& actor) {
    std::vector<PxShape*> shapes(actor.getNbShapes());
    actor.getShapes(shapes.data(), (PxU32)shapes.size());
    return shapes;
}

void BuildJoints(PhysicsState& s, const World& world) {
    auto jointView = world.Registry.view<const JointComponent, const TransformComponent>(entt::exclude<InactiveTag>);
    if (jointView.begin() == jointView.end()) return;

    // Resolve the "other end" by OrderComponent value — over dynamic/kinematic bodies AND
    // static collider actors, so a joint can hinge onto a wall (#185 hardening).
    std::unordered_map<int, PxRigidActor*> byOrder;
    for (entt::entity e : world.Registry.view<const OrderComponent>()) {
        const int ord = world.Registry.get<const OrderComponent>(e).Value;
        const std::uint32_t id = entt::to_integral(e);
        if (PxRigidActor* a = ActorFor(s, id)) byOrder[ord] = a;
    }

    int made = 0, skipped = 0;
    for (entt::entity e : jointView) {
        const auto& j = jointView.get<const JointComponent>(e);
        const std::string tag = EntityLogRef(world.Registry, e);

        auto self = s.bodyByEntity.find(entt::to_integral(e));
        if (self == s.bodyByEntity.end()) {
            Log::Warn("PhysX: " + tag + " has a Joint but no Rigidbody — skipped.", EntityLogContext(world.Registry, e)); ++skipped; continue;
        }
        PxRigidActor* a0 = self->second;
        PxRigidActor* a1 = nullptr;
        if (j.ConnectedOrder >= 0) {
            auto o = byOrder.find(j.ConnectedOrder);
            if (o == byOrder.end()) {
                Log::Warn("PhysX: Joint on " + tag + " — connected actor (order " +
                          std::to_string(j.ConnectedOrder) + ") has no body/collider — skipped.");
                ++skipped; continue;
            }
            a1 = o->second;
        }

        const PxTransform f0(ToPx(j.Anchor), AxisToLocalFrame(j.Axis));
        const PxTransform worldFrame = a0->getGlobalPose() * f0;
        const PxTransform f1 = a1 ? (a1->getGlobalPose().getInverse() * worldFrame) : worldFrame;

        PxJoint* joint = nullptr;
        switch (j.Kind) {
            case JointComponent::Type::Fixed:    joint = PxFixedJointCreate(*s.physics, a0, f0, a1, f1); break;
            case JointComponent::Type::Hinge:    joint = PxRevoluteJointCreate(*s.physics, a0, f0, a1, f1); break;
            case JointComponent::Type::Ball:     joint = PxSphericalJointCreate(*s.physics, a0, f0, a1, f1); break;
            case JointComponent::Type::Slider:   joint = PxPrismaticJointCreate(*s.physics, a0, f0, a1, f1); break;
            case JointComponent::Type::Distance: joint = PxDistanceJointCreate(*s.physics, a0, f0, a1, f1); break;
        }
        if (!joint) { ++skipped; continue; }
        ApplyJointLimits(joint, j, s.physics->getTolerancesScale());
        if (j.BreakForce > 0.0f)
            joint->setBreakForce(j.BreakForce, j.BreakTorque > 0.0f ? j.BreakTorque : PX_MAX_F32);
        s.joints.push_back(joint);
        s.jointOwner[joint] = entt::to_integral(e);
        // Chains and ragdolls: the project's solver iteration count (8 by default) leaves a
        // chain of joints visibly stretching and jittering - worst with a heavy body on light
        // links, like the Sandbox wrecking ball. Joined bodies get at least 32 position / 4
        // velocity iterations; unjointed bodies keep the project setting.
        for (PxRigidActor* a : {a0, a1}) {
            PxRigidDynamic* d = a ? a->is<PxRigidDynamic>() : nullptr;
            if (!d) continue;
            PxU32 pos = 0, vel = 0;
            d->getSolverIterationCounts(pos, vel);
            d->setSolverIterationCounts(std::max<PxU32>(pos, 32), std::max<PxU32>(vel, 4));
            s.jointed.insert(d);
        }
        ++made;
    }
    if (made || skipped)
        Log::Info("PhysX: " + std::to_string(made) + " joint(s)" +
                  (skipped ? ", " + std::to_string(skipped) + " skipped" : "") + ".");
}

// --- Forces & read-back (#185 PR 7) --------------------------------------------------------

// A non-kinematic dynamic body for `entity`, or null. bodyByEntity holds kinematics too, so
// filter those out here — force/velocity calls on a kinematic body are meaningless.
PxRigidDynamic* DynamicFor(unsigned entity) {
    if (!g_State) return nullptr;
    auto it = g_State->bodyByEntity.find(entity);
    if (it == g_State->bodyByEntity.end()) return nullptr;
    PxRigidDynamic* b = it->second;
    return (b->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) ? nullptr : b;
}
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
namespace {
PxForceMode::Enum ToForceMode(unsigned mode) {
    switch (mode) {
        case 1:  return PxForceMode::eIMPULSE;
        case 2:  return PxForceMode::eVELOCITY_CHANGE;
        case 3:  return PxForceMode::eACCELERATION;
        default: return PxForceMode::eFORCE;
    }
}
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld {
void AddForce(unsigned entity, const float force[3], unsigned mode) {
    if (PxRigidDynamic* b = DynamicFor(entity))
        b->addForce(PxVec3(force[0], force[1], force[2]), ToForceMode(mode), /*autowake=*/true);
}
void AddTorque(unsigned entity, const float torque[3], unsigned mode) {
    if (PxRigidDynamic* b = DynamicFor(entity))
        b->addTorque(PxVec3(torque[0], torque[1], torque[2]), ToForceMode(mode), /*autowake=*/true);
}
void AddForceAtPosition(unsigned entity, const float force[3], const float worldPos[3], unsigned mode) {
    if (PxRigidDynamic* b = DynamicFor(entity))
        PxRigidBodyExt::addForceAtPos(*b, PxVec3(force[0], force[1], force[2]),
                                      PxVec3(worldPos[0], worldPos[1], worldPos[2]),
                                      ToForceMode(mode), /*wakeup=*/true);
}
void AddExplosionForce(const float center[3], float radius, float strength, float upwardBias) {
    if (!g_State || radius <= 0.0f) return;
    const PxVec3 c(center[0], center[1], center[2]);
    for (auto& kv : g_State->bodyByEntity) {
        PxRigidDynamic* b = kv.second;
        if (b->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) continue;
        PxVec3 d = b->getGlobalPose().p - c;
        const float dist = d.magnitude();
        if (dist > radius) continue;
        const float falloff = 1.0f - dist / radius;
        PxVec3 dir = (dist > 1e-4f) ? (d / dist) : PxVec3(0.0f, 1.0f, 0.0f);
        dir.y += upwardBias;
        dir.normalize();
        b->addForce(dir * (strength * falloff), PxForceMode::eIMPULSE, /*autowake=*/true);
    }
}
void SetLinearVelocity(unsigned entity, const float v[3]) {
    if (PxRigidDynamic* b = DynamicFor(entity))
        b->setLinearVelocity(PxVec3(v[0], v[1], v[2]));
}

bool GetBodyState(unsigned entity, BodyState& out) {
    out = BodyState{};
    if (!g_State) return false;
    auto it = g_State->bodyByEntity.find(entity);
    if (it == g_State->bodyByEntity.end()) return false;
    PxRigidDynamic* b = it->second;
    const bool kin = (b->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC);
    const PxVec3 v = b->getLinearVelocity();
    const PxVec3 w = b->getAngularVelocity();
    out.Valid = true;
    out.Kinematic = kin;
    out.Sleeping = !kin && b->isSleeping();
    out.Velocity[0] = v.x; out.Velocity[1] = v.y; out.Velocity[2] = v.z;
    out.AngularVelocity[0] = w.x; out.AngularVelocity[1] = w.y; out.AngularVelocity[2] = w.z;
    return true;
}

void SetActorPose(unsigned entity, const float posXYZ[3], const float rotationXYZW[4], bool zeroVelocity) {
    if (!g_State) return;
    const glm::quat rotation(rotationXYZW[3], rotationXYZW[0], rotationXYZW[1], rotationXYZW[2]);
    const PxTransform pose(PxVec3(posXYZ[0], posXYZ[1], posXYZ[2]),
                           QuatToPx(rotation));
    if (auto it = g_State->bodyByEntity.find(entity); it != g_State->bodyByEntity.end()) {
        PxRigidDynamic* b = it->second;
        if (b->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) {
            b->setKinematicTarget(pose);
        } else {
            b->setGlobalPose(pose, /*autowake=*/true);
            if (zeroVelocity) { b->setLinearVelocity(PxVec3(0.0f)); b->setAngularVelocity(PxVec3(0.0f)); }
        }
        return;
    }
    if (auto st = g_State->staticByEntity.find(entity); st != g_State->staticByEntity.end())
        st->second->setGlobalPose(pose);
}

bool GetActorRotation(unsigned entity, float out[4]) {
    if (!g_State) return false;
    PxRigidActor* a = ActorFor(*g_State, entity);
    if (!a) return false;
    const PxQuat q = a->getGlobalPose().q;
    out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
    return true;
}

float BodyRadius(unsigned entity) {
    PxRigidDynamic* b = g_State ? DynamicFor(entity) : nullptr;
    PxShape* shape = nullptr;
    if (!b || b->getNbShapes() < 1 || b->getShapes(&shape, 1) != 1 || !shape) return 0.0f;
    const PxBounds3 bounds = PxShapeExt::getWorldBounds(*shape, *b);
    return 0.5f * bounds.getDimensions().maxElement();
}

float GetLinearDamping(unsigned entity) {
    if (!g_State) return 0.0f;
    auto it = g_State->bodyByEntity.find(entity);
    return it != g_State->bodyByEntity.end() ? it->second->getLinearDamping() : 0.0f;
}

bool GetActorPosition(unsigned entity, float out[3]) {
    if (!g_State) return false;
    PxRigidActor* a = ActorFor(*g_State, entity);
    if (!a) return false;
    const PxVec3 p = a->getGlobalPose().p;
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
    return true;
}
} // namespace PhysicsWorld
