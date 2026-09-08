#include "PhysicsWorld.h"

#include "Log.h"
#include "ProjectSettings.h"
#include "World.h"
#include "Components.h"
#include "Model.h"
#include "GameModuleAPI.h" // RaycastHit (shared POD)

#include <PxPhysicsAPI.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>

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

struct PhysicsState {
    PxDefaultAllocator     allocator;
    EngineErrorCallback    errorCallback;
    PxFoundation*          foundation   = nullptr;
    PxPhysics*             physics      = nullptr;
    PxDefaultCpuDispatcher* dispatcher  = nullptr;
    PxScene*              scene         = nullptr;
    PxMaterial*           defaultMaterial = nullptr;
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

// --- Static-actor build (#185 PR 2) --------------------------------------------------------

PxVec3 ToPx(const glm::vec3& v) { return PxVec3(v.x, v.y, v.z); }

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

// Create + attach + register one static actor. `geom` is only borrowed for the createShape call.
void AddStatic(PhysicsState& s, entt::entity e, bool isTrigger,
               const PxTransform& actorPose, const PxGeometry& geom, const PxTransform& shapeLocal) {
    PxShapeFlags flags = isTrigger
        ? (PxShapeFlag::eTRIGGER_SHAPE | PxShapeFlag::eSCENE_QUERY_SHAPE)
        : (PxShapeFlag::eSIMULATION_SHAPE | PxShapeFlag::eSCENE_QUERY_SHAPE);

    PxShape* shape = s.physics->createShape(geom, *s.defaultMaterial, /*isExclusive=*/true, flags);
    if (!shape) return;
    shape->setLocalPose(shapeLocal);

    PxRigidStatic* actor = s.physics->createRigidStatic(actorPose);
    actor->attachShape(*shape);
    actor->userData = EntityToUserData(e);
    shape->release(); // the actor holds the reference now
    s.scene->addActor(*actor);
}

void BuildStatics(PhysicsState& s, const World& world) {
    int built = 0, skipped = 0;
    auto view = world.Registry.view<const TransformComponent, const ColliderComponent>(entt::exclude<InactiveTag>);
    for (entt::entity e : view) {
        const auto& t = view.get<const TransformComponent>(e);
        const auto& c = view.get<const ColliderComponent>(e);

        if (c.HalfExtents == glm::vec3(0.0f)) {
            // Match the legacy path exactly: an axis-aligned box the size of the render bounds,
            // rotation ignored. Center offset is already folded into the world AABB.
            glm::vec3 center, half;
            AutoBoxWorld(world.Registry, e, t, center, half);
            if (half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f) { ++skipped; continue; }
            AddStatic(s, e, c.IsTrigger, PxTransform(ToPx(center)), PxBoxGeometry(ToPx(half)), PxTransform(PxIdentity));
            ++built;
            continue;
        }

        glm::quat q(glm::radians(t.RotationEuler));
        PxTransform actorPose(ToPx(t.Position), PxQuat(q.x, q.y, q.z, q.w));
        PxTransform shapeLocal(ToPx(c.Center));

        const float sx = std::abs(t.Scale.x), sy = std::abs(t.Scale.y), sz = std::abs(t.Scale.z);
        switch (c.Kind) {
            case ColliderComponent::Shape::Box: {
                glm::vec3 h = c.HalfExtents * glm::abs(t.Scale);
                if (h.x <= 0.0f || h.y <= 0.0f || h.z <= 0.0f) { ++skipped; continue; }
                AddStatic(s, e, c.IsTrigger, actorPose, PxBoxGeometry(ToPx(h)), shapeLocal);
                break;
            }
            case ColliderComponent::Shape::Sphere: {
                float r = c.HalfExtents.x * std::max({sx, sy, sz});
                if (r <= 0.0f) { ++skipped; continue; }
                AddStatic(s, e, c.IsTrigger, actorPose, PxSphereGeometry(r), shapeLocal);
                break;
            }
            case ColliderComponent::Shape::Capsule: {
                // PhysX capsules run along local X; author them along Y (local up) by rotating
                // the shape 90 deg about Z. Radius follows the max horizontal scale, half-height
                // the vertical.
                float r  = c.HalfExtents.x * std::max(sx, sz);
                float hh = c.HalfExtents.y * sy;
                if (r <= 0.0f || hh <= 0.0f) { ++skipped; continue; }
                shapeLocal = shapeLocal * PxTransform(PxQuat(PxHalfPi, PxVec3(0, 0, 1)));
                AddStatic(s, e, c.IsTrigger, actorPose, PxCapsuleGeometry(r, hh), shapeLocal);
                break;
            }
        }
        ++built;
    }

    std::string msg = "PhysX: " + std::to_string(built) + " static collider(s) added";
    if (skipped) msg += ", " + std::to_string(skipped) + " skipped (degenerate size)";
    Log::Info(msg + ".");
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

    PxSceneDesc desc(s->physics->getTolerancesScale());
    const glm::vec3 g = ProjectSettings::Physics().Gravity;
    desc.gravity        = PxVec3(g.x, g.y, g.z);
    desc.cpuDispatcher  = s->dispatcher;
    desc.filterShader   = PxDefaultSimulationFilterShader;
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

    g_State = s;
    Log::Info("PhysX world created (" + std::to_string(workers) + " worker threads).");

    BuildStatics(*s, world);
}

void Destroy() {
    if (!g_State) return;
    PhysicsState* s = g_State;
    g_State = nullptr; // clear first so a re-entrant Step() during teardown is a no-op

    // Reverse construction order. scene->release() drops every actor/shape it owns.
    if (s->scene)           s->scene->release();
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

void Step(float dt) {
    if (!g_State || !g_State->scene) return;
    if (dt <= 0.0f) return;

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

} // namespace PhysicsWorld
