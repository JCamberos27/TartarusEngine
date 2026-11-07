#include "PhysicsInternal.h"

namespace {
PxFilterFlags EngineFilterShader(
    PxFilterObjectAttributes attributes0, PxFilterData filterData0,
    PxFilterObjectAttributes attributes1, PxFilterData filterData1,
    PxPairFlags& pairFlags, const void* constantBlock, PxU32 constantBlockSize) {
    if (constantBlock && constantBlockSize >= sizeof(PxU32) * 32) {
        const PxU32* mask = static_cast<const PxU32*>(constantBlock);
        const PxU32 la = filterData0.word1 & 31u, lb = filterData1.word1 & 31u;
        if (((mask[la] >> lb) & 1u) == 0u) return PxFilterFlag::eKILL;
    }
    // A character passing through the dead (word1's kPassRagdolls bit: SetNpcPassesRagdolls) against a ragdoll's part.
    if (((filterData0.word1 & kPassRagdolls) && IsRagdollPart(filterData1)) || ((filterData1.word1 & kPassRagdolls) && IsRagdollPart(filterData0)))
        return PxFilterFlag::eKILL;
    // The parts of one ragdoll (word2: its id + 1 in the low half, the part in the high half) collide with each other unless
    // either one's word3 has the other's bit: parent and child, a pair that overlapped when the body was built (until it
    // separates), a gun with the hand that held it.
    {
        const PxU32 r0 = filterData0.word2 & 0xFFFFu, r1 = filterData1.word2 & 0xFFFFu;
        if (r0 != r1 && ((filterData0.word1 | filterData1.word1) & kPassBodies) && IsRagdollPart(filterData0) && IsRagdollPart(filterData1))
            return PxFilterFlag::eKILL;
        if (r0 != 0u && r0 == r1) {
            // (An ignorer's slot is past the 32 part bits: only its own mask decides.)
            const PxU32 p0 = (filterData0.word2 >> 16) & 63u, p1 = (filterData1.word2 >> 16) & 63u;
            if ((p1 < 32u && ((filterData0.word3 >> p1) & 1u)) || (p0 < 32u && ((filterData1.word3 >> p0) & 1u))) return PxFilterFlag::eKILL;
        }
    }
    if (PxFilterObjectIsTrigger(attributes0) || PxFilterObjectIsTrigger(attributes1)) {
        pairFlags = PxPairFlag::eTRIGGER_DEFAULT;
        return PxFilterFlag::eDEFAULT;
    }
    pairFlags = PxPairFlag::eCONTACT_DEFAULT
              | PxPairFlag::eDETECT_CCD_CONTACT
              | PxPairFlag::eNOTIFY_TOUCH_FOUND
              | PxPairFlag::eNOTIFY_TOUCH_PERSISTS
              | PxPairFlag::eNOTIFY_TOUCH_LOST
              | PxPairFlag::eNOTIFY_CONTACT_POINTS;
    return PxFilterFlag::eDEFAULT;
}
PhysicsCore* g_Core = nullptr;
} // namespace

namespace PhysicsWorld::detail {
// Whole world lives or dies as one unit between Create()/Destroy(), so a single owning pointer
// (not a pile of file-scope globals) keeps the lifetime obvious and the teardown ordered.
PhysicsState* g_State = nullptr;

// Releases in the order Shutdown() used to by hand: cooked meshes and materials first, then the defaults, the dispatcher
// and PxPhysics, the PVD pieces after physics, and the foundation last.
PhysicsCore::~PhysicsCore() {
    for (auto& kv : materialCache)          if (kv.second) kv.second->release(); // #185 PR 7
    for (auto& kv : convexCache)            if (kv.second) kv.second->release(); // #185 PR 6/12
    for (auto& kv : triangleCache)          if (kv.second) kv.second->release();
    if (defaultMaterial) defaultMaterial->release();
    if (dispatcher)      dispatcher->release();
    if (physics)         physics->release();
#ifdef TARTARUS_PHYSX_PVD
    if (pvd) {
        pvd->disconnect();
        pvd->release();
    }
    if (pvdTransport) pvdTransport->release();
#endif
#ifdef TARTARUS_PHYSX_OMNIPVD
    if (omniPvd) omniPvd->release(); // after physics->release()
#endif
    if (foundation) foundation->release();
}

// Reverse construction order: the joints (#185 PR 11), the ragdolls' own PhysX objects, then the scene, whose release drops
// every actor/shape it owns. The borrowed PhysicsCore pieces are never released here.
PhysicsState::~PhysicsState() {
    for (PxJoint* j : joints)               if (j) j->release(); // #185 PR 11 — before the scene
    for (auto& r : ragdolls) { if (r.Frozen && r.Art) { r.Art->release(); r.Art = nullptr; } if (r.Good) { r.Good->release(); r.Good = nullptr; } for (PxJoint* j : r.Attach) if (j) j->release(); if (r.Mat) { r.Mat->release(); r.Mat = nullptr; } if (r.SlickMat) { r.SlickMat->release(); r.SlickMat = nullptr; } } // (the articulations go with the scene)
    if (scene)           scene->release();
}
} // namespace PhysicsWorld::detail

namespace {
std::uint64_t g_SimulationGeneration=0;

constexpr float kDefaultFixedStep = 1.0f / 60.0f;
} // namespace

namespace PhysicsWorld::detail {
// Sim step comes from the project setting (#236 A4 stored it; #185 is the first consumer).
// Guard against a zero/negative value in settings.json turning the loop below infinite.
float FixedStep() {
    float t = ProjectSettings::Physics().FixedTimestep;
    return (t > 0.0f) ? t : kDefaultFixedStep;
}
} // namespace PhysicsWorld::detail

namespace {
// #167 - stand up the session-lifetime half once; later Plays reuse it.
bool EnsureCore() {
    if (g_Core) return true;
    auto* c = new PhysicsCore();

    c->foundation = PxCreateFoundation(PX_PHYSICS_VERSION, c->allocator, c->errorCallback);
    if (!c->foundation) {
        Log::Error("PhysX: PxCreateFoundation failed — physics disabled for this Play session.");
        delete c;
        return false;
    }

#ifdef TARTARUS_PHYSX_PVD
    // PhysX Visual Debugger: connect if a PVD instance is listening, otherwise carry on. Debug
    // builds only (see CMakeLists) — it makes the collider/character work in PR 2-3 far easier
    // to inspect, and costs nothing when nothing is listening.
    c->pvd = PxCreatePvd(*c->foundation);
    c->pvdTransport = PxDefaultPvdSocketTransportCreate("127.0.0.1", 5425, 10);
    c->pvd->connect(*c->pvdTransport, PxPvdInstrumentationFlag::eALL);
#endif

    PxPvd* pvdArg = nullptr;
#ifdef TARTARUS_PHYSX_PVD
    pvdArg = c->pvd;
#endif
    PxOmniPvd* omniArg = nullptr;
#ifdef TARTARUS_PHYSX_OMNIPVD
    // #185 G — write an .ovd capture next to the exe for the Omniverse PhysX inspector (full
    // timeline scrubbing of every actor / shape / joint / contact / query). Opt-in at build
    // time: needs PX_BUILDPVDRUNTIME=ON so PVDRuntime_64.dll gets built + harvested (see
    // CMakeLists). A missing runtime DLL just means no capture — the sim runs regardless.
    c->omniPvd = PxCreateOmniPvd(*c->foundation);
    if (c->omniPvd && c->omniPvd->getWriter() && c->omniPvd->getFileWriteStream()) {
        OmniPvdFileWriteStream* fs = c->omniPvd->getFileWriteStream();
        fs->setFileName("physx_capture.ovd");
        c->omniPvd->getWriter()->setWriteStream(*static_cast<OmniPvdWriteStream*>(fs));
        omniArg = c->omniPvd;
    } else if (c->omniPvd) {
        c->omniPvd->release();
        c->omniPvd = nullptr;
        Log::Warn("PhysX: OmniPVD runtime unavailable (PVDRuntime_64.dll missing) — no .ovd capture.");
    }
#endif
    c->physics = PxCreatePhysics(PX_PHYSICS_VERSION, *c->foundation, PxTolerancesScale(),
                                 /*trackOutstandingAllocations=*/true, pvdArg, omniArg);
    if (!c->physics) {
        Log::Error("PhysX: PxCreatePhysics failed — physics disabled for this Play session.");
        delete c; // ~PhysicsCore releases the PVD pieces, then the foundation (physics is null)
        return false;
    }
#ifdef TARTARUS_PHYSX_OMNIPVD
    if (c->omniPvd && c->physics->getOmniPvd()) {
        c->physics->getOmniPvd()->startSampling();
        Log::Info("PhysX: OmniPVD capture -> physx_capture.ovd");
    }
#endif

    // Keep the worker pool modest: the engine is otherwise single-threaded, and PR 2 only has
    // static actors. hardware_concurrency() can report 0 — clamp to at least 1.
    unsigned hw = std::thread::hardware_concurrency();
    c->workers = std::max<PxU32>(1, std::min<PxU32>(hw ? hw - 1 : 1, 4));
    c->dispatcher = PxDefaultCpuDispatcherCreate(c->workers);
    c->defaultMaterial = c->physics->createMaterial(0.6f, 0.6f, 0.0f);

    g_Core = c;
    Log::Info("PhysX initialised (" + std::to_string(c->workers) + " worker threads).");
    return true;
}
} // namespace

namespace PhysicsWorld {
void Create(const World& world) {
    if (g_State) return; // idempotent — a stray second OnEnterPlayMode must not leak a scene
    if (!EnsureCore()) return;

    auto* s = new PhysicsState(*g_Core);
    s->simCb.owner = s;      // #185 PR 5
    // #185 PR 8 — snapshot the layer collision matrix; PhysX copies it as the filter constant block.
    static_assert(LayerRegistry::kCount == 32, "the filter shader's constant block is one PxU32 row per layer");
    for (int i = 0; i < 32; ++i) s->layerMask[i] = ProjectSettings::Physics().LayerCollisionMask[i];

    PxSceneDesc desc(s->physics->getTolerancesScale());
    const glm::vec3 g = ProjectSettings::Physics().Gravity;
    desc.gravity                 = PxVec3(g.x, g.y, g.z);
    desc.cpuDispatcher           = s->dispatcher;
    desc.filterShader            = EngineFilterShader; // triggers, contacts (#185 PR 7), layers (PR 8), CCD (PR 9)
    desc.filterShaderData        = s->layerMask;
    desc.filterShaderDataSize    = sizeof(s->layerMask);
    desc.simulationEventCallback = &s->simCb;
    desc.flags                  |= PxSceneFlag::eENABLE_CCD; // per-body, opt in via ContinuousCollision (#185 PR 9)
    // Same result every run regardless of how the solver threads carve up the work — cheap
    // insurance for a future that wants replays / deterministic netcode (#185 hardening).
    desc.flags                  |= PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
    // Resting contacts (a corpse's sixteen parts on the floor) hold still instead of creeping (bodies opt in by their stabilization
    // threshold; PhysX's own default is tiny).
    desc.flags                  |= PxSceneFlag::eENABLE_STABILIZATION;
    // TGS: the temporal Gauss-Seidel solver holds jointed chains and articulations (the ragdolls' joint limits under a falling
    // body's weight) far better than the default PGS at the same iteration count.
    desc.solverType              = PxSolverType::eTGS;
    s->scene = s->physics->createScene(desc);
    if (!s->scene) {
        Log::Error("PhysX: createScene failed — physics disabled for this Play session.");
        delete s;
        return;
    }

    g_State = s;
    ++g_SimulationGeneration;
    Log::Info("PhysX world created (" + std::to_string(g_Core->workers) + " worker threads).");

    BuildActors(*s, world);
    BuildJoints(*s, world); // #185 PR 11
}

void Destroy() {
    if (!g_State) return;
    PhysicsState* s = g_State;
    g_State = nullptr; // clear first so a re-entrant Step() during teardown is a no-op
    ++g_SimulationGeneration;

    // ~PhysicsState releases in reverse construction order: the joints, the ragdolls' PhysX objects, then the scene
    // (which drops every actor/shape it owns). The core (physics, dispatcher, materials, cooked meshes) stays up for the
    // next Play (#167).
    delete s;
    Log::Info("PhysX world destroyed.");
}

void Shutdown() {
    Destroy();
    if (!g_Core) return;
    PhysicsCore* c = g_Core;
    g_Core = nullptr;
    delete c; // ~PhysicsCore releases the cached materials and meshes, the defaults, the dispatcher, PxPhysics, the PVD pieces, then the foundation
}

bool IsActive() {
    return g_State != nullptr;
}

std::uint64_t SimulationGeneration() {return g_SimulationGeneration;}
} // namespace PhysicsWorld
