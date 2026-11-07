#include "PhysicsInternal.h"

namespace PhysicsWorld {
namespace {
// #185 debug — one fixed substep: velocity snapshot (for impact speed), simulate, fetch.
// Shared by Step() and StepOneSubstep().
void RunOneSubstep(PhysicsState& s, float fixedStep) {
    DriveGrab(s, fixedStep);
    s.preStepVel.BeginRefill();
    s.prevPose.BeginRefill();
    for (auto& [body, e] : s.dynamics) {
        // A sleeping body has no velocity and won't have moved: no entry reads as zero velocity and as no
        // interpolation (the current pose), the same thing.
        if (body->isSleeping()) continue;
        s.preStepVel.Set(body, body->getLinearVelocity());
        s.prevPose.Set(body, body->getGlobalPose()); // #168
    }
    for (auto& rd : s.ragdolls) {
        if (rd.Frozen) continue;
        rd.Prev.resize(rd.Bodies.size());
        for (size_t i = 0; i < rd.Bodies.size(); ++i) rd.Prev[i] = rd.Bodies[i]->getGlobalPose();
        RagdollBeforeSubstep(s, rd);
    }
    s.scene->simulate(fixedStep);
    s.scene->fetchResults(/*block=*/true);
    for (auto& rd : s.ragdolls) RagdollAfterSubstep(rd);
    ++s.frameIndex;
    s.simClock += fixedStep;
}
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld::detail {
// #185 debug — reject a NaN/inf simulated pose instead of poisoning the TransformComponent
// (which then spreads through the world-transform cache and the renderer). Freezes the body.
bool PoseIsFinite(const PxTransform& p) {
    auto ok = [](float f) { return std::isfinite(f); };
    return ok(p.p.x) && ok(p.p.y) && ok(p.p.z) &&
           ok(p.q.x) && ok(p.q.y) && ok(p.q.z) && ok(p.q.w);
}
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
namespace {
// Step() and StepOneSubstep() share these phases so the editor's single-step button can't drift
// from the real step.

// Clears last frame's events, syncs entities, then pushes kinematic targets. Returns false (after
// clearing and syncing only) when dt <= 0: frozen time runs no sim, so no events are replayed, but
// a deleted object still stops blocking.
bool BeginFrame(World& world, float dt) {
    g_State->triggerEvents.clear();
    g_State->enteredThisFrame.clear();
    g_State->contactEvents.clear(); // #185 PR 7 — onContact refills it during the sim
    SyncEntities(*g_State, world); // #166 / #201 - even while frozen, so a deleted object stops blocking
    if (dt <= 0.0f) { g_State->lastSubsteps = 0; g_State->lastStepMillis = 0.0f; return false; }

    // Kinematic bodies are driven by their TransformComponent (e.g. an Animator-moved platform):
    // push this frame's authored pose into the actor before stepping so it sweeps other bodies.
    g_State->kinematicFramePose.BeginRefill();
    for (auto& [body, e] : g_State->kinematics) {
        if (!world.Registry.valid(e)) continue;
        if (!world.Registry.all_of<TransformComponent>(e)) continue;
        const TransformComponent w = world.WorldSpaceTransform(e); // #114
        const PxTransform target(ToPx(w.Position), QuatToPx(w.Rotation));
        body->setKinematicTarget(target);
        g_State->kinematicFramePose.Set(body, target); // #168
    }
    return true;
}

// Write each force-driven body's simulated pose back to its TransformComponent so the renderer,
// gizmos and world-transform cache all see it move. Play -> Stop reloads the authored scene, so
// this is never persisted. #168 - `alpha` is how far this frame sits between the last substep and
// the next one; the written pose is blended (or predicted) by it per the body's Interpolate mode.
// `exactPose` writes the simulated pose as is (no blend, no prediction) for single-stepping.
void WriteBackDynamics(World& world, float fixedStep, float alpha, bool exactPose) {
    for (auto& [body, e] : g_State->dynamics) {
        if (!world.Registry.valid(e)) continue;
        auto* tc = world.Registry.try_get<TransformComponent>(e);
        if (!tc) continue;
        // (Root bodies only: a child's local pose also moves with its parent.)
        const auto* hier = world.Registry.try_get<HierarchyComponent>(e);
        const bool sleeping = body->isSleeping() && (!hier || hier->Parent == entt::null);
        if (sleeping) {
            if (const auto it = g_State->settled.find(body);
                it != g_State->settled.end() && it->second.Position == tc->Position && it->second.Rotation == tc->Rotation)
                continue;
        } else {
            g_State->settled.erase(body);
        }
        const PxTransform p = body->getGlobalPose();
        if (!detail::PoseIsFinite(p)) { // #185 — blow-up guard: don't poison the TransformComponent
            body->setLinearVelocity(PxVec3(0.0f));
            body->setAngularVelocity(PxVec3(0.0f));
            body->putToSleep();
            const TransformComponent w = world.WorldSpaceTransform(e); // #114
            body->setGlobalPose(PxTransform(ToPx(w.Position), QuatToPx(w.Rotation)));
            g_State->prevPose.Erase(body); // #168 - don't blend from the blown-up pose
            Log::Warn("PhysX: " + EntityLogRef(world.Registry, e) +
                      " produced a non-finite pose — frozen at its last good transform.", EntityLogContext(world.Registry, e));
            continue;
        }
        // #114 — the PhysX pose is WORLD space; convert through the parent (was written
        // straight into the local fields, teleporting any simulated child).
        PxTransform shown = p;
        const auto* rb = world.Registry.try_get<RigidbodyComponent>(e);
        // The gravity gun's held body is always extrapolated: it's servoed to a point in front
        // of a camera that moves every rendered frame, and drawing it a physics step behind
        // (Interpolate) made it judder against the view whenever the player moved.
        const int mode = exactPose ? 0 : body == g_State->grabbed ? 2 : (rb ? rb->Interpolation : 1);
        // (A body asleep shows - and records as settled - its true pose, not a blend toward it.)
        if (mode == 1 && !body->isSleeping()) {
            if (const PxTransform* prev = g_State->prevPose.Find(body); prev && detail::PoseIsFinite(*prev)) {
                const PxTransform& a = *prev;
                shown.p = a.p + (p.p - a.p) * alpha;
                const glm::quat qa(a.q.w, a.q.x, a.q.y, a.q.z), qb(p.q.w, p.q.x, p.q.y, p.q.z);
                const glm::quat q = glm::slerp(qa, qb, alpha);
                shown.q = PxQuat(q.x, q.y, q.z, q.w);
            }
        } else if (mode == 2 && !body->isSleeping()) {
            const float ahead = alpha * fixedStep;
            shown.p = p.p + body->getLinearVelocity() * ahead;
            const PxVec3 w = body->getAngularVelocity();
            const PxQuat spin(w.x * ahead * 0.5f, w.y * ahead * 0.5f, w.z * ahead * 0.5f, 0.0f);
            shown.q = (p.q + spin * p.q).getNormalized();
        }
        world.SetWorldPose(e, glm::vec3(shown.p.x, shown.p.y, shown.p.z),
                           glm::quat(shown.q.w, shown.q.x, shown.q.y, shown.q.z));
        if (sleeping) g_State->settled[body] = {tc->Position, tc->Rotation};
    }
}

// --- Trigger transitions (#185 PR 5) -----------------------------------------------
// onTrigger (during fetchResults) already pushed rigidbody Enter/Exit. The Player capsule doesn't
// report through onTrigger, so diff a fresh overlap set against last frame, then synthesise Stay
// for every overlap still active that didn't just fire Enter.
void UpdateTriggerEvents() {
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

    for (const auto& pr : g_State->triggerOverlaps)
        if (!g_State->enteredThisFrame.count(pr))
            PushTriggerEvent(*g_State, TriggerEvent::Stay, pr.first, pr.second);
    for (std::uint32_t t : g_State->playerTriggers)
        if (!g_State->enteredThisFrame.count({t, kPlayerEntity}))
            PushTriggerEvent(*g_State, TriggerEvent::Stay, t, kPlayerEntity);
}

void EndFrame(World& world, int steps, float fixedStep, float alpha, bool exactPose) {
    if (steps) FreezeRestingRagdolls(*g_State, fixedStep * (float)steps);
    WriteBackDynamics(world, fixedStep, alpha, exactPose);
    UpdateTriggerEvents();
}
} // namespace

void Step(float dt, World& world, const std::function<void(float fixedDt)>& onFixedStep) {
    if (!g_State || !g_State->scene) return;
    // dt arrives time-scaled (Time::DeltaTime); the debug-draw history fades in real time.
    const float rawDt = Time::UnscaledDeltaTime();
    if (!BeginFrame(world, dt)) return;

    // Age the debug-draw history (contacts / queries) so sparks fade over ~½ second.
    for (int i = 0; i < g_ContactCount; ++i) g_Contacts[i].age += rawDt;
    for (int i = 0; i < g_QueryCount; ++i)   g_Queries[i].age  += rawDt;

    // Fixed-timestep accumulator: gameplay physics must not vary with frame rate. Cap the
    // catch-up at kMaxSubSteps so a big hitch (asset load, breakpoint) doesn't trigger a
    // multi-second simulation spiral on the next frame. What's past the cap is dropped: capped a
    // step higher, the hitch frame ran its kMaxSubSteps and left a whole step owed, so the next
    // ordinary frame ran two (a lurch right after the hitch).
    const float fixedStep = FixedStep();
    g_State->stepAccumulator += dt;
    if (g_State->stepAccumulator > fixedStep * kMaxSubSteps)
        g_State->stepAccumulator = fixedStep * kMaxSubSteps;

    const auto t0 = std::chrono::steady_clock::now();
    int steps = 0;
    // (A few microseconds of slack: float rounding must not leave the last of a whole number of steps owed.)
    while (g_State->stepAccumulator >= fixedStep - 1e-6f && steps < kMaxSubSteps) {
        if (onFixedStep) onFixedStep(fixedStep); // FixedUpdate runs before the sim, as in Unity
        if (!g_State || !g_State->scene) return; // game code tore physics down in FixedUpdate
        RunOneSubstep(*g_State, fixedStep);
        g_State->stepAccumulator -= fixedStep;
        ++steps;
    }
    if (!g_State || !g_State->scene) return;
    g_State->lastSubsteps = steps;
    g_State->lastStepMillis = steps ? std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - t0).count() : 0.0f;
    const float alpha = std::clamp(g_State->stepAccumulator / fixedStep, 0.0f, 1.0f);
    EndFrame(world, steps, fixedStep, alpha, /*exactPose=*/false);
}

void StepOneSubstep(World& world) {
    if (!g_State || !g_State->scene) return;
    const float fixedStep = FixedStep();
    if (!BeginFrame(world, fixedStep)) return;
    const auto t0 = std::chrono::steady_clock::now();
    RunOneSubstep(*g_State, fixedStep);
    g_State->lastSubsteps = 1;
    g_State->lastStepMillis = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    EndFrame(world, 1, fixedStep, /*alpha=*/1.0f, /*exactPose=*/true);
}
} // namespace PhysicsWorld
