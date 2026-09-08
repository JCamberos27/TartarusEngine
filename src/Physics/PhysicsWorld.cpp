#include "PhysicsWorld.h"

#include "Log.h"
#include "ProjectSettings.h"
#include "World.h"
#include "Components.h"
#include "Model.h"
#include "GameModuleAPI.h" // RaycastHit / TriggerEvent (shared PODs), kPlayerEntity

#include <PxPhysicsAPI.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp> // eulerAngleYXZ / extractEulerAngleYXZ — matches ComposeTransform

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace physx;

namespace {

// PhysX routes every internal warning/error through this. Map its severities onto the engine
// log so a misconfigured scene shows up in the Console panel like anything else, rather than
// only on stderr.
class EngineErrorCallback : public PxErrorCallback {
public:
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        std::string text = "PhysX: " + std::string(message ? message : "(null)");
        if (file) text += " (" + std::string(file) + ":" + std::to_string(line) + ")";
        switch (code) {
            case PxErrorCode::eDEBUG_INFO:
                Log::Info(text);
                break;
            case PxErrorCode::eDEBUG_WARNING:
            case PxErrorCode::ePERF_WARNING:
                Log::Warn(text);
                break;
            default:
                Log::Error(text);
                break;
        }
    }
};

using TriggerPair = std::pair<std::uint32_t, std::uint32_t>; // (trigger entity, other entity)

struct PhysicsState;

// PhysX calls this back during fetchResults for every trigger touch found/lost. We only
// implement onTrigger; the other five events are unused. Enter/Exit go straight onto the
// frame's event list and the tracked overlap set (Step synthesises Stay from what's left).
struct TriggerCallback : PxSimulationEventCallback {
    PhysicsState* owner = nullptr;
    void onTrigger(PxTriggerPair* pairs, PxU32 count) override;
    void onContact(const PxContactPairHeader&, const PxContactPair*, PxU32) override {}
    void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
    void onWake(PxActor**, PxU32) override {}
    void onSleep(PxActor**, PxU32) override {}
    void onAdvance(const PxRigidBody* const*, const PxTransform*, PxU32) override {}
};

struct PhysicsState {
    PxDefaultAllocator     allocator;
    EngineErrorCallback    errorCallback;
    TriggerCallback        triggerCb;
    PxFoundation*          foundation   = nullptr;
    PxPhysics*             physics      = nullptr;
    PxDefaultCpuDispatcher* dispatcher  = nullptr;
    PxScene*              scene         = nullptr;
    PxMaterial*           defaultMaterial = nullptr;
    PxControllerManager*  controllerMgr = nullptr; // #185 PR 3
    PxController*         controller    = nullptr; // the Play-mode Player's capsule (lazy)
    // #185 PR 4 — RigidbodyComponent entities. Force-driven bodies get their pose written back
    // to TransformComponent after each Step; kinematic bodies are driven the other way, from
    // TransformComponent, before it. Actors themselves are owned by the scene.
    std::vector<std::pair<PxRigidDynamic*, entt::entity>> dynamics;
    std::vector<std::pair<PxRigidDynamic*, entt::entity>> kinematics;
    // #185 PR 5 — trigger state. triggerOverlaps: rigidbody pairs currently inside a trigger
    // (maintained by onTrigger). playerTriggers: trigger entities the Player capsule is inside
    // (maintained by a per-frame overlap query). triggerEvents: this frame's transitions,
    // rebuilt each Step and drained by GetTriggerEvents. enteredThisFrame: dedup so a pair that
    // fired Enter this frame doesn't also get a synthetic Stay.
    std::set<TriggerPair>       triggerOverlaps;
    std::set<std::uint32_t>     playerTriggers;
    std::vector<TriggerEvent>   triggerEvents;
    std::set<TriggerPair>       enteredThisFrame;
    // #185 PR 6 — meshes cooked from RenderableComponent geometry for ConvexHull / Mesh
    // colliders. Refcounted by PhysX; released after the scene (which owns the shapes that
    // reference them) and before PxPhysics.
    std::vector<PxConvexMesh*>   convexMeshes;
    std::vector<PxTriangleMesh*> triangleMeshes;
#ifdef TARTARUS_PHYSX_PVD
    PxPvd*               pvd            = nullptr;
    PxPvdTransport*      pvdTransport   = nullptr;
#endif
    float                stepAccumulator = 0.0f;
};

// Whole world lives or dies as one unit between Create()/Destroy(), so a single owning pointer
// (not a pile of file-scope globals) keeps the lifetime obvious and the teardown ordered.
PhysicsState* g_State = nullptr;

constexpr float kDefaultFixedStep = 1.0f / 60.0f;
constexpr int   kMaxSubSteps      = 4;

// Sim step comes from the project setting (#236 A4 stored it; #185 is the first consumer).
// Guard against a zero/negative value in settings.json turning the loop below infinite.
float FixedStep() {
    float t = ProjectSettings::Physics().FixedTimestep;
    return (t > 0.0f) ? t : kDefaultFixedStep;
}

// --- Actor build (#185 PR 2 statics, PR 4 dynamics) --------------------------------------

PxVec3 ToPx(const glm::vec3& v) { return PxVec3(v.x, v.y, v.z); }

// Build the actor rotation the same way World's ComposeTransform does (Ry * Rx * Rz), so a
// rotated collider — and a dynamic body's written-back pose — line up with how the renderer
// interprets TransformComponent.RotationEuler.
PxQuat EulerToPx(const glm::vec3& degrees) {
    glm::mat4 r = glm::eulerAngleYXZ(glm::radians(degrees.y), glm::radians(degrees.x), glm::radians(degrees.z));
    glm::quat q = glm::quat_cast(r);
    return PxQuat(q.x, q.y, q.z, q.w);
}
glm::vec3 PxQuatToEulerDeg(const PxQuat& q) {
    glm::quat g(q.w, q.x, q.y, q.z);
    float ey, ex, ez;
    glm::extractEulerAngleYXZ(glm::mat4_cast(g), ey, ex, ez);
    return glm::degrees(glm::vec3(ex, ey, ez));
}

// entt::entity id <-> the void* PhysX stores per actor. A hit always comes back with an actor,
// and every actor in our scene is one of ours, so the round-trip needs no sentinel: entity 0
// stores as nullptr and reads back as 0, which is a valid entity the caller can compare.
void* EntityToUserData(entt::entity e) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(entt::to_integral(e)));
}
std::uint32_t UserDataToEntity(void* p) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p));
}

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

void BuildActors(PhysicsState& s, const World& world) {
    int statics = 0, dynamic = 0, kinematic = 0, skipped = 0;
    auto view = world.Registry.view<const TransformComponent, const ColliderComponent>(entt::exclude<InactiveTag>);
    for (entt::entity e : view) {
        const auto& t  = view.get<const TransformComponent>(e);
        const auto& c  = view.get<const ColliderComponent>(e);
        const auto* rb = world.Registry.try_get<const RigidbodyComponent>(e); // null => static (PR 2)

        const PxShapeFlags flags = c.IsTrigger
            ? (PxShapeFlag::eTRIGGER_SHAPE | PxShapeFlag::eSCENE_QUERY_SHAPE)
            : (PxShapeFlag::eSIMULATION_SHAPE | PxShapeFlag::eSCENE_QUERY_SHAPE);

        PxTransform actorPose(PxIdentity);
        PxTransform shapeLocal(PxIdentity);
        PxShape* shape = nullptr;
        auto make = [&](const PxGeometry& g) {
            shape = s.physics->createShape(g, *s.defaultMaterial, /*isExclusive=*/true, flags);
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
                Log::Warn("PhysX: entity " + std::to_string(entt::to_integral(e)) +
                          " has a mesh collider but no usable mesh — skipped.");
                ++skipped;
                continue;
            }
            actorPose  = PxTransform(ToPx(t.Position), EulerToPx(t.RotationEuler));
            shapeLocal = PxTransform(ToPx(c.Center));
            const PxMeshScale meshScale(ToPx(glm::abs(t.Scale)));

            // A triangle mesh can't be on a non-kinematic dynamic body (PhysX restriction) —
            // fall back to a convex hull for that case.
            const bool dynamicNonKin = rb && !rb->IsKinematic;
            const bool useTriangle = (c.Kind == ColliderComponent::Shape::Mesh) && !dynamicNonKin;
            if (c.Kind == ColliderComponent::Shape::Mesh && dynamicNonKin)
                Log::Warn("PhysX: entity " + std::to_string(entt::to_integral(e)) +
                          " — a triangle-mesh collider can't be dynamic; using a convex hull.");

            if (useTriangle) {
                PxTriangleMesh* tm = CookTriangle(*s.physics, verts, idx);
                if (!tm) { ++skipped; continue; }
                s.triangleMeshes.push_back(tm);
                make(PxTriangleMeshGeometry(tm, meshScale));
            } else {
                PxConvexMesh* cm = CookConvex(*s.physics, verts);
                if (!cm) {
                    Log::Warn("PhysX: entity " + std::to_string(entt::to_integral(e)) +
                              " — convex cook failed (mesh too complex?) — skipped.");
                    ++skipped;
                    continue;
                }
                s.convexMeshes.push_back(cm);
                make(PxConvexMeshGeometry(cm, meshScale));
            }
        } else if (c.HalfExtents == glm::vec3(0.0f)) {
            // Auto: an axis-aligned box the size of the render bounds.
            glm::vec3 center, half;
            AutoBoxWorld(world.Registry, e, t, center, half);
            if (half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f) { ++skipped; continue; }
            make(PxBoxGeometry(ToPx(half)));
            if (rb) {
                // Pose at the entity origin so the simulated pose writes straight back to
                // TransformComponent; the box's offset from that origin becomes the shape pose.
                actorPose  = PxTransform(ToPx(t.Position), EulerToPx(t.RotationEuler));
                shapeLocal = PxTransform(ToPx(center - t.Position));
            } else {
                actorPose = PxTransform(ToPx(center)); // PR 2 static: AABB centre, no rotation
            }
        } else {
            actorPose  = PxTransform(ToPx(t.Position), EulerToPx(t.RotationEuler));
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

        if (!rb) {
            PxRigidStatic* a = s.physics->createRigidStatic(actorPose);
            a->attachShape(*shape);
            a->userData = EntityToUserData(e);
            shape->release();
            s.scene->addActor(*a);
            ++statics;
            continue;
        }

        PxRigidDynamic* b = s.physics->createRigidDynamic(actorPose);
        b->attachShape(*shape);
        b->userData = EntityToUserData(e);
        shape->release();
        if (!c.IsTrigger) // setMassAndUpdateInertia needs a simulation shape
            PxRigidBodyExt::setMassAndUpdateInertia(*b, rb->Mass > 0.0f ? rb->Mass : 1.0f);
        b->setLinearDamping(std::max(0.0f, rb->LinearDamping));
        b->setAngularDamping(std::max(0.0f, rb->AngularDamping));
        b->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !rb->UseGravity);
        if (rb->IsKinematic) {
            b->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
            s.kinematics.push_back({b, e});
            ++kinematic;
        } else {
            b->setLinearVelocity(ToPx(rb->InitialVelocity));
            s.dynamics.push_back({b, e});
            ++dynamic;
        }
        s.scene->addActor(*b);
    }

    std::string msg = "PhysX: " + std::to_string(statics) + " static, " + std::to_string(dynamic) +
                      " dynamic, " + std::to_string(kinematic) + " kinematic collider(s)";
    if (skipped) msg += "; " + std::to_string(skipped) + " skipped (degenerate size)";
    Log::Info(msg + ".");
}

// --- Triggers (#185 PR 5) --------------------------------------------------------------

const char* EntityLabel(std::uint32_t e, char buf[24]) {
    if (e == kPlayerEntity) return "Player";
    std::snprintf(buf, 24, "entity %u", e);
    return buf;
}

void PushTriggerEvent(PhysicsState& s, std::uint32_t kind, std::uint32_t trig, std::uint32_t other) {
    TriggerEvent ev;
    ev.Kind = kind; ev.Trigger = trig; ev.Other = other;
    s.triggerEvents.push_back(ev);
    if (kind == TriggerEvent::Enter || kind == TriggerEvent::Exit) {
        char a[24], b[24];
        Log::Info(std::string("Trigger ") + (kind == TriggerEvent::Enter ? "enter: " : "exit:  ") +
                  EntityLabel(other, a) + (kind == TriggerEvent::Enter ? " -> " : " <- ") +
                  EntityLabel(trig, b));
    }
}

void TriggerCallback::onTrigger(PxTriggerPair* pairs, PxU32 count) {
    if (!owner) return;
    for (PxU32 i = 0; i < count; ++i) {
        const PxTriggerPair& p = pairs[i];
        if (p.flags & (PxTriggerPairFlag::eREMOVED_SHAPE_TRIGGER | PxTriggerPairFlag::eREMOVED_SHAPE_OTHER))
            continue;
        const std::uint32_t trig  = UserDataToEntity(p.triggerActor->userData);
        const std::uint32_t other = UserDataToEntity(p.otherActor->userData);
        const TriggerPair key{trig, other};
        if (p.status & PxPairFlag::eNOTIFY_TOUCH_FOUND) {
            if (owner->triggerOverlaps.insert(key).second) {
                owner->enteredThisFrame.insert(key);
                PushTriggerEvent(*owner, TriggerEvent::Enter, trig, other);
            }
        } else if (p.status & PxPairFlag::eNOTIFY_TOUCH_LOST) {
            if (owner->triggerOverlaps.erase(key))
                PushTriggerEvent(*owner, TriggerEvent::Exit, trig, other);
        }
    }
}

} // namespace

namespace PhysicsWorld {

void Create(const World& world) {
    if (g_State) return; // idempotent — a stray second OnEnterPlayMode must not leak a scene

    auto* s = new PhysicsState();

    s->foundation = PxCreateFoundation(PX_PHYSICS_VERSION, s->allocator, s->errorCallback);
    if (!s->foundation) {
        Log::Error("PhysX: PxCreateFoundation failed — physics disabled for this Play session.");
        delete s;
        return;
    }

#ifdef TARTARUS_PHYSX_PVD
    // PhysX Visual Debugger: connect if a PVD instance is listening, otherwise carry on. Debug
    // builds only (see CMakeLists) — it makes the collider/character work in PR 2-3 far easier
    // to inspect, and costs nothing when nothing is listening.
    s->pvd = PxCreatePvd(*s->foundation);
    s->pvdTransport = PxDefaultPvdSocketTransportCreate("127.0.0.1", 5425, 10);
    s->pvd->connect(*s->pvdTransport, PxPvdInstrumentationFlag::eALL);
#endif

    PxPvd* pvdArg = nullptr;
#ifdef TARTARUS_PHYSX_PVD
    pvdArg = s->pvd;
#endif
    s->physics = PxCreatePhysics(PX_PHYSICS_VERSION, *s->foundation, PxTolerancesScale(),
                                 /*trackOutstandingAllocations=*/true, pvdArg);
    if (!s->physics) {
        Log::Error("PhysX: PxCreatePhysics failed — physics disabled for this Play session.");
#ifdef TARTARUS_PHYSX_PVD
        if (s->pvd) s->pvd->release();
        if (s->pvdTransport) s->pvdTransport->release();
#endif
        s->foundation->release();
        delete s;
        return;
    }

    // Keep the worker pool modest: the engine is otherwise single-threaded, and PR 2 only has
    // static actors. hardware_concurrency() can report 0 — clamp to at least 1.
    unsigned hw = std::thread::hardware_concurrency();
    PxU32 workers = std::max<PxU32>(1, std::min<PxU32>(hw ? hw - 1 : 1, 4));
    s->dispatcher = PxDefaultCpuDispatcherCreate(workers);

    s->defaultMaterial = s->physics->createMaterial(0.6f, 0.6f, 0.0f);

    s->triggerCb.owner = s; // #185 PR 5
    PxSceneDesc desc(s->physics->getTolerancesScale());
    const glm::vec3 g = ProjectSettings::Physics().Gravity;
    desc.gravity                 = PxVec3(g.x, g.y, g.z);
    desc.cpuDispatcher           = s->dispatcher;
    desc.filterShader            = PxDefaultSimulationFilterShader; // handles trigger pairs
    desc.simulationEventCallback = &s->triggerCb;
    s->scene = s->physics->createScene(desc);
    if (!s->scene) {
        Log::Error("PhysX: createScene failed — physics disabled for this Play session.");
        s->defaultMaterial->release();
        s->dispatcher->release();
        s->physics->release();
#ifdef TARTARUS_PHYSX_PVD
        if (s->pvd) s->pvd->release();
        if (s->pvdTransport) s->pvdTransport->release();
#endif
        s->foundation->release();
        delete s;
        return;
    }

    s->controllerMgr = PxCreateControllerManager(*s->scene);

    g_State = s;
    Log::Info("PhysX world created (" + std::to_string(workers) + " worker threads).");

    BuildActors(*s, world);
}

void Destroy() {
    if (!g_State) return;
    PhysicsState* s = g_State;
    g_State = nullptr; // clear first so a re-entrant Step() during teardown is a no-op

    // Reverse construction order. scene->release() drops every actor/shape it owns.
    if (s->controller)      s->controller->release();
    if (s->controllerMgr)   s->controllerMgr->release();
    if (s->scene)           s->scene->release();
    for (PxConvexMesh* m : s->convexMeshes)   if (m) m->release(); // #185 PR 6 — after the scene's shapes
    for (PxTriangleMesh* m : s->triangleMeshes) if (m) m->release();
    if (s->defaultMaterial) s->defaultMaterial->release();
    if (s->dispatcher)      s->dispatcher->release();
    if (s->physics)         s->physics->release();
#ifdef TARTARUS_PHYSX_PVD
    if (s->pvd) {
        s->pvd->disconnect();
        s->pvd->release();
    }
    if (s->pvdTransport) s->pvdTransport->release();
#endif
    if (s->foundation) s->foundation->release();

    delete s;
    Log::Info("PhysX world destroyed.");
}

bool IsActive() {
    return g_State != nullptr;
}

// Trigger entities whose volume the Player capsule currently overlaps. One overlap query per
// Step — cheap for the handful of trigger shapes a scene has.
void CollectPlayerTriggers(std::set<std::uint32_t>& out) {
    out.clear();
    if (!g_State->controller) return;
    auto* cap = static_cast<PxCapsuleController*>(g_State->controller);
    const PxExtendedVec3 c = cap->getPosition();
    const PxCapsuleGeometry geom(cap->getRadius(), 0.5f * cap->getHeight());
    const PxTransform pose(PxVec3((float)c.x, (float)c.y, (float)c.z),
                           PxQuat(PxHalfPi, PxVec3(0, 0, 1))); // capsule axis X -> up

    PxOverlapHit hits[16];
    PxOverlapBuffer buf(hits, 16);
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::eNO_BLOCK);
    if (!g_State->scene->overlap(geom, pose, buf, fd)) return;
    for (PxU32 i = 0; i < buf.getNbTouches(); ++i) {
        const PxShape* sh = buf.getTouch(i).shape;
        if (sh && (sh->getFlags() & PxShapeFlag::eTRIGGER_SHAPE) && buf.getTouch(i).actor)
            out.insert(UserDataToEntity(buf.getTouch(i).actor->userData));
    }
}

void Step(float dt, World& world) {
    if (!g_State || !g_State->scene) return;
    if (dt <= 0.0f) return;

    g_State->triggerEvents.clear();
    g_State->enteredThisFrame.clear();

    // Kinematic bodies are driven by their TransformComponent (e.g. an Animator-moved platform):
    // push this frame's authored pose into the actor before stepping so it sweeps other bodies.
    for (auto& [body, e] : g_State->kinematics) {
        if (!world.Registry.valid(e)) continue;
        const auto* tc = world.Registry.try_get<const TransformComponent>(e);
        if (!tc) continue;
        body->setKinematicTarget(PxTransform(ToPx(tc->Position), EulerToPx(tc->RotationEuler)));
    }

    // Fixed-timestep accumulator: gameplay physics must not vary with frame rate. Cap the
    // catch-up at kMaxSubSteps so a big hitch (asset load, breakpoint) doesn't trigger a
    // multi-second simulation spiral on the next frame.
    const float fixedStep = FixedStep();
    g_State->stepAccumulator += dt;
    if (g_State->stepAccumulator > fixedStep * (kMaxSubSteps + 1))
        g_State->stepAccumulator = fixedStep * (kMaxSubSteps + 1);

    int steps = 0;
    while (g_State->stepAccumulator >= fixedStep && steps < kMaxSubSteps) {
        g_State->scene->simulate(fixedStep);
        g_State->scene->fetchResults(/*block=*/true);
        g_State->stepAccumulator -= fixedStep;
        ++steps;
    }

    // Write each force-driven body's simulated pose back to its TransformComponent so the
    // renderer, gizmos and world-transform cache all see it move. Play -> Stop reloads the
    // authored scene, so this is never persisted.
    for (auto& [body, e] : g_State->dynamics) {
        if (!world.Registry.valid(e)) continue;
        auto* tc = world.Registry.try_get<TransformComponent>(e);
        if (!tc) continue;
        const PxTransform p = body->getGlobalPose();
        tc->Position      = glm::vec3(p.p.x, p.p.y, p.p.z);
        tc->RotationEuler = PxQuatToEulerDeg(p.q);
    }

    // --- Trigger transitions (#185 PR 5) -----------------------------------------------
    // onTrigger (above, during fetchResults) already pushed rigidbody Enter/Exit. The Player
    // capsule doesn't report through onTrigger, so diff a fresh overlap set against last frame.
    std::set<std::uint32_t> nowPlayer;
    CollectPlayerTriggers(nowPlayer);
    for (std::uint32_t t : nowPlayer)
        if (!g_State->playerTriggers.count(t)) {
            g_State->enteredThisFrame.insert({t, kPlayerEntity});
            PushTriggerEvent(*g_State, TriggerEvent::Enter, t, kPlayerEntity);
        }
    for (std::uint32_t t : g_State->playerTriggers)
        if (!nowPlayer.count(t))
            PushTriggerEvent(*g_State, TriggerEvent::Exit, t, kPlayerEntity);
    g_State->playerTriggers.swap(nowPlayer);

    // Synthesise Stay for every overlap still active that didn't just fire Enter.
    for (const auto& pr : g_State->triggerOverlaps)
        if (!g_State->enteredThisFrame.count(pr))
            PushTriggerEvent(*g_State, TriggerEvent::Stay, pr.first, pr.second);
    for (std::uint32_t t : g_State->playerTriggers)
        if (!g_State->enteredThisFrame.count({t, kPlayerEntity}))
            PushTriggerEvent(*g_State, TriggerEvent::Stay, t, kPlayerEntity);
}

bool Raycast(const float origin[3], const float dir[3], float maxDistance, RaycastHit& outHit) {
    outHit = RaycastHit{};
    if (!g_State || !g_State->scene) return false;

    PxVec3 d(dir[0], dir[1], dir[2]);
    const float len = d.magnitude();
    if (len < 1e-8f || maxDistance <= 0.0f) return false;
    d *= (1.0f / len);

    PxRaycastBuffer buf;
    if (!g_State->scene->raycast(PxVec3(origin[0], origin[1], origin[2]), d, maxDistance, buf) || !buf.hasBlock)
        return false;

    const PxRaycastHit& b = buf.block;
    outHit.Hit = true;
    outHit.Distance = b.distance;
    outHit.Point[0]  = b.position.x; outHit.Point[1]  = b.position.y; outHit.Point[2]  = b.position.z;
    outHit.Normal[0] = b.normal.x;   outHit.Normal[1] = b.normal.y;   outHit.Normal[2] = b.normal.z;
    outHit.Entity = b.actor ? UserDataToEntity(b.actor->userData) : outHit.Entity;
    return true;
}

// --- Character controller ---------------------------------------------------------------

bool HasCharacter() {
    return g_State && g_State->controller;
}

void CreateCharacter(float radius, float cylinderHalfHeight, const float footPos[3]) {
    if (!g_State || !g_State->controllerMgr || g_State->controller) return;
    if (radius <= 0.0f || cylinderHalfHeight <= 0.0f) return;

    const float halfCapsule = radius + cylinderHalfHeight; // foot -> centre distance

    PxCapsuleControllerDesc desc;
    desc.radius       = radius;
    desc.height       = 2.0f * cylinderHalfHeight; // cylinder segment (sphere-centre to sphere-centre)
    desc.position     = PxExtendedVec3(footPos[0], footPos[1] + halfCapsule, footPos[2]);
    desc.upDirection  = PxVec3(0.0f, 1.0f, 0.0f);
    desc.stepOffset   = 0.3f;                       // auto-climb ledges up to this tall
    desc.slopeLimit   = std::cos(0.8726646f);       // walkable up to ~50 degrees
    desc.contactOffset = 0.05f;
    desc.material     = g_State->defaultMaterial;
    desc.climbingMode = PxCapsuleClimbingMode::eCONSTRAINED; // don't let the sphere cap boost climbs

    if (!desc.isValid()) {
        Log::Error("PhysX: capsule controller desc invalid — Player will not collide this session.");
        return;
    }
    g_State->controller = g_State->controllerMgr->createController(desc);
    if (!g_State->controller) {
        Log::Error("PhysX: createController failed — Player will not collide this session.");
        return;
    }
    // Tag the CCT actor so anything that reads userData (trigger callback, a future raycast
    // hit on the player) can tell it apart from a real entity (#185 PR 5).
    if (PxRigidActor* a = g_State->controller->getActor())
        a->userData = reinterpret_cast<void*>(static_cast<std::uintptr_t>(kPlayerEntity));
}

void SetCharacterFootPosition(const float footPos[3]) {
    if (!HasCharacter()) return;
    g_State->controller->setFootPosition(PxExtendedVec3(footPos[0], footPos[1], footPos[2]));
}

void GetCharacterFootPosition(float outFootPos[3]) {
    if (!HasCharacter()) return;
    const PxExtendedVec3 f = g_State->controller->getFootPosition();
    outFootPos[0] = (float)f.x;
    outFootPos[1] = (float)f.y;
    outFootPos[2] = (float)f.z;
}

unsigned MoveCharacter(const float disp[3], float dt) {
    if (!HasCharacter() || dt <= 0.0f) return 0;
    PxControllerFilters filters;
    PxControllerCollisionFlags f =
        g_State->controller->move(PxVec3(disp[0], disp[1], disp[2]), /*minDist=*/0.001f, dt, filters);
    unsigned out = 0;
    if (f & PxControllerCollisionFlag::eCOLLISION_SIDES) out |= CC_SIDES;
    if (f & PxControllerCollisionFlag::eCOLLISION_UP)    out |= CC_UP;
    if (f & PxControllerCollisionFlag::eCOLLISION_DOWN)  out |= CC_DOWN;
    return out;
}

int GetTriggerEvents(TriggerEvent* out, int maxEvents) {
    if (!g_State) return 0;
    const int total = (int)g_State->triggerEvents.size();
    const int n = (maxEvents < total) ? maxEvents : total;
    for (int i = 0; i < n && out; ++i) out[i] = g_State->triggerEvents[(size_t)i];
    return total;
}

bool IsTriggerOccupied(unsigned triggerEntity) {
    if (!g_State) return false;
    if (g_State->playerTriggers.count(triggerEntity)) return true;
    for (const auto& pr : g_State->triggerOverlaps)
        if (pr.first == triggerEntity) return true;
    return false;
}

} // namespace PhysicsWorld
