#include "PhysicsWorld.h"

#include "Log.h"
#include "ProjectSettings.h"

#include <PxPhysicsAPI.h>

#include <algorithm>
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

} // namespace

namespace PhysicsWorld {

void Create() {
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

    // Keep the worker pool modest: the engine is otherwise single-threaded, and PR 1 has zero
    // actors to simulate anyway. hardware_concurrency() can report 0 — clamp to at least 1.
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
}

void Destroy() {
    if (!g_State) return;
    PhysicsState* s = g_State;
    g_State = nullptr; // clear first so a re-entrant Step() during teardown is a no-op

    // Reverse construction order.
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

} // namespace PhysicsWorld
