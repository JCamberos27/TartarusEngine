#include "PhysicsWorld.h"

#include "Log.h"
#include "ProjectSettings.h"
#include "World.h"
#include "Components.h"
#include "Model.h"
#include "GameModuleAPI.h" // RaycastHit / TriggerEvent / ContactEvent / BodyState / ForceMode, kPlayerEntity

#include <PxPhysicsAPI.h>
#ifdef TARTARUS_PHYSX_OMNIPVD
#include <omnipvd/PxOmniPvd.h>
#include <OmniPvdWriter.h>            // from physx/pvdruntime/include — add that dir in CMakeLists
#include <OmniPvdFileWriteStream.h>
#include <OmniPvdWriteStream.h>
#endif

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp> // eulerAngleYXZ / extractEulerAngleYXZ — matches ComposeTransform

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
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

// PhysX calls this back during fetchResults. onTrigger feeds trigger enter/exit; onContact
// feeds solid-contact enter/stay/exit (#185 PR 7). The other four events are unused.
struct SimEventCallback : PxSimulationEventCallback {
    PhysicsState* owner = nullptr;
    void onTrigger(PxTriggerPair* pairs, PxU32 count) override;
    void onContact(const PxContactPairHeader& header, const PxContactPair* pairs, PxU32 count) override;
    void onConstraintBreak(PxConstraintInfo* constraints, PxU32 count) override;
    void onWake(PxActor**, PxU32) override {}
    void onSleep(PxActor**, PxU32) override {}
    void onAdvance(const PxRigidBody* const*, const PxTransform*, PxU32) override {}
};

// Custom filter shader: PxDefaultSimulationFilterShader solves contacts but reports none.
// This keeps its trigger handling, adds the NOTIFY flags so onContact fires for solid pairs
// (#185 PR 7), kills pairs the layer matrix disables (#185 PR 8; word1 of each shape's filter
// data is its layer 0-7, constantBlock is ProjectSettings' 8-word mask), and asks for CCD
// contact detection so a body with the eENABLE_CCD flag actually sweeps (#185 PR 9).
PxFilterFlags EngineFilterShader(
    PxFilterObjectAttributes attributes0, PxFilterData filterData0,
    PxFilterObjectAttributes attributes1, PxFilterData filterData1,
    PxPairFlags& pairFlags, const void* constantBlock, PxU32 constantBlockSize) {
    if (constantBlock && constantBlockSize >= sizeof(PxU32) * 8) {
        const PxU32* mask = static_cast<const PxU32*>(constantBlock);
        const PxU32 la = filterData0.word1 & 7u, lb = filterData1.word1 & 7u;
        if (((mask[la] >> lb) & 1u) == 0u) return PxFilterFlag::eKILL;
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

// The CCT hit report: push dynamic bodies the Player walks into, and remember a kinematic body
// it's standing on so MoveCharacter can carry it along (#185 PR 10).
struct PlayerHitReport : PxUserControllerHitReport {
    PhysicsState* owner = nullptr;
    void onShapeHit(const PxControllerShapeHit& hit) override;
    void onControllerHit(const PxControllersHit&) override {}
    void onObstacleHit(const PxControllerObstacleHit&) override {}
};

struct PhysicsState {
    PxDefaultAllocator     allocator;
    EngineErrorCallback    errorCallback;
    SimEventCallback       simCb;
    PlayerHitReport        hitReport;
    PxU32                  layerMask[8] = {0xFFu,0xFFu,0xFFu,0xFFu,0xFFu,0xFFu,0xFFu,0xFFu}; // #185 PR 8 constant block
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
    // #185 PR 7 — one PxMaterial per distinct (friction, bounciness) rounded to 1/100; the
    // per-entity dynamic-body lookup for the force API; this frame's solid-contact events.
    std::map<std::pair<int, int>, PxMaterial*>       materialCache;
    std::unordered_map<std::uint32_t, PxRigidDynamic*> bodyByEntity;
    std::vector<ContactEvent>                        contactEvents;
    // #185 PR 11 — joints built on Play-enter, released before the scene. jointOwner maps each
    // back to the entity that authored it, for a useful break message. staticByEntity lets a
    // joint's other end be a static collider (#185 hardening).
    std::vector<PxJoint*> joints;
    std::unordered_map<PxJoint*, std::uint32_t>       jointOwner;
    std::unordered_map<std::uint32_t, PxRigidStatic*> staticByEntity;
    // #185 PR 12 — cooked meshes cached by model path so a Play->Stop->Play doesn't re-cook.
    // These own the meshes (released in Destroy); convexMeshes/triangleMeshes above just track
    // the non-cached (should be none now) plus keep the release loop simple.
    std::unordered_map<std::string, PxConvexMesh*>   convexCache;
    std::unordered_map<std::string, PxTriangleMesh*> triangleCache;
    // #185 PR 10 — Player's kinematic ground (moving platform) + its last pose, for carry.
    PxRigidDynamic* playerGround = nullptr;
    PxVec3          playerGroundLastPos{0.0f};
    PxQuat          playerGroundLastRot{PxIdentity};
    bool            playerGroundHitThisMove = false;
    float           platformYawDeltaDeg = 0.0f; // this frame's platform spin, for Player yaw
    // #185 hardening — every dynamic body's linear velocity captured just before simulate(),
    // so onContact (post-solve) can report the real closing speed of an impact.
    std::unordered_map<const PxActor*, PxVec3> preStepVel;
    // #185 hardening — gravity-gun grab (physics-playground debug harness). The held body plus
    // the gravity flag / angular damping saved at grab time so ReleaseBody restores them.
    PxRigidDynamic* grabbed = nullptr;
    bool            grabSavedGravityDisabled = false;
    float           grabSavedAngularDamping  = 0.0f;
    // #185 debug tooling — per-session counters.
    int             lastSubsteps = 0;
    float           lastStepMillis = 0.0f;
    unsigned        frameIndex = 0;
    double          simClock = 0.0;
#ifdef TARTARUS_PHYSX_PVD
    PxPvd*               pvd            = nullptr;
    PxPvdTransport*      pvdTransport   = nullptr;
#endif
#ifdef TARTARUS_PHYSX_OMNIPVD
    PxOmniPvd*           omniPvd        = nullptr; // #185 G — .ovd capture for the Omniverse PhysX inspector
#endif
    float                stepAccumulator = 0.0f;
};

// Whole world lives or dies as one unit between Create()/Destroy(), so a single owning pointer
// (not a pile of file-scope globals) keeps the lifetime obvious and the teardown ordered.
PhysicsState* g_State = nullptr;

constexpr float kDefaultFixedStep = 1.0f / 60.0f;
constexpr int   kMaxSubSteps      = 4;

// --- #185 debug tooling: settings + history that OUTLIVE a Play session -----------------
// The editor sets these from the Physics panel; they must survive Play->Stop->Play, so they
// can't live in PhysicsState.
unsigned g_DebugDrawFlags = 0;
float    g_SimTimeScale   = 1.0f;
bool     g_QueryRecording = false;

// Recent contact points, for the fading impact sparks (PDD_Contacts). Appended in onContact,
// aged each Step, drawn + culled in CopyDebugLines.
struct ContactViz {
    PxVec3 p{0.0f}, n{0.0f, 1.0f, 0.0f};
    float  impulse = 0.0f;
    float  age = 0.0f;      // seconds since the touch; culled past kContactFade
    bool   impact = false;  // a real hit (bright burst) vs. a gentle new touch
};
constexpr int   kContactRing = 64;
constexpr float kContactFade = 1.8f;   // bounce sparks linger ~2 s
ContactViz g_Contacts[kContactRing];
int g_ContactHead = 0, g_ContactCount = 0;

// Recent scene queries, for PDD_Raycasts.
struct QueryRecord {
    int    kind = 0;             // 0 ray, 1 sweep(sphere), 2 overlap(sphere)
    PxVec3 origin{0.0f}, dir{0.0f}, hitPoint{0.0f}, hitNormal{0.0f};
    float  dist = 0.0f, radius = 0.0f, age = 0.0f;
    bool   hit = false;
};
constexpr int   kQueryRing = 64;
constexpr float kQueryFade = 0.5f;
QueryRecord g_Queries[kQueryRing];
int g_QueryHead = 0, g_QueryCount = 0;

void PushContactViz(const PxVec3& p, const PxVec3& n, float impulse, bool impact) {
    ContactViz& c = g_Contacts[g_ContactHead];
    c = ContactViz{}; c.p = p; c.n = n; c.impulse = impulse; c.impact = impact;
    g_ContactHead = (g_ContactHead + 1) % kContactRing;
    if (g_ContactCount < kContactRing) ++g_ContactCount;
}

void RecordQuery(const QueryRecord& q) {
    if (!g_QueryRecording) return;
    g_Queries[g_QueryHead] = q;
    g_QueryHead = (g_QueryHead + 1) % kQueryRing;
    if (g_QueryCount < kQueryRing) ++g_QueryCount;
}

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

// One PxMaterial per distinct (friction, bounciness), rounded to 1/100 so a slider wobble
// doesn't spawn hundreds (#185 PR 7). Friction feeds both the static and dynamic coefficient.
PxMaterial* GetMaterial(PhysicsState& s, float friction, float bounciness) {
    friction   = std::max(0.0f, friction);
    bounciness = std::min(1.0f, std::max(0.0f, bounciness));
    const std::pair<int, int> key{(int)std::lround(friction * 100.0f), (int)std::lround(bounciness * 100.0f)};
    auto it = s.materialCache.find(key);
    if (it != s.materialCache.end()) return it->second;
    PxMaterial* m = s.physics->createMaterial(friction, friction, bounciness);
    s.materialCache.emplace(key, m);
    return m;
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
        PxMaterial* mat = GetMaterial(s, c.Friction, c.Bounciness); // #185 PR 7
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

            // Cook once per model path per Play session (#185 PR 12) — a cooked mesh is
            // scale-independent (scale rides on the geometry's PxMeshScale), so the cache key
            // is just the path.
            const std::string key = (rc && rc->ModelRef) ? rc->ModelRef->Path() : std::string();
            if (useTriangle) {
                PxTriangleMesh*& tm = s.triangleCache[key];
                if (!tm) tm = CookTriangle(*s.physics, verts, idx);
                if (!tm) { s.triangleCache.erase(key); ++skipped; continue; }
                make(PxTriangleMeshGeometry(tm, meshScale));
            } else {
                PxConvexMesh*& cm = s.convexCache[key];
                if (!cm) cm = CookConvex(*s.physics, verts);
                if (!cm) {
                    s.convexCache.erase(key);
                    // Better a rough box than no collider at all (#185 hardening).
                    glm::vec3 center, half;
                    AutoBoxWorld(world.Registry, e, t, center, half);
                    if (half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f) { ++skipped; continue; }
                    Log::Warn("PhysX: entity " + std::to_string(entt::to_integral(e)) +
                              " — convex cook failed (mesh too dense) — using a bounds box.");
                    actorPose  = PxTransform(ToPx(t.Position), EulerToPx(t.RotationEuler));
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

        // #185 PR 8 — stamp the layer (word1) so EngineFilterShader can apply the matrix.
        const auto* lc = world.Registry.try_get<const LayerComponent>(e);
        const PxU32 layer = (lc && lc->Layer >= 0 && lc->Layer < 8) ? (PxU32)lc->Layer : 0u;
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

    std::string msg = "PhysX: " + std::to_string(statics) + " static, " + std::to_string(dynamic) +
                      " dynamic, " + std::to_string(kinematic) + " kinematic collider(s)";
    if (skipped) msg += "; " + std::to_string(skipped) + " skipped (degenerate size)";
    Log::Info(msg + ".");
}

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

void BuildJoints(PhysicsState& s, const World& world) {
    auto jointView = world.Registry.view<const JointComponent, const TransformComponent>(entt::exclude<InactiveTag>);
    if (jointView.begin() == jointView.end()) return;

    // Resolve the "other end" by OrderComponent value — over dynamic/kinematic bodies AND
    // static collider actors, so a joint can hinge onto a wall (#185 hardening).
    std::unordered_map<int, PxRigidActor*> byOrder;
    for (entt::entity e : world.Registry.view<const OrderComponent>()) {
        const int ord = world.Registry.get<const OrderComponent>(e).Value;
        const std::uint32_t id = entt::to_integral(e);
        if (auto it = s.bodyByEntity.find(id); it != s.bodyByEntity.end())        byOrder[ord] = it->second;
        else if (auto st = s.staticByEntity.find(id); st != s.staticByEntity.end()) byOrder[ord] = st->second;
    }

    int made = 0, skipped = 0;
    for (entt::entity e : jointView) {
        const auto& j = jointView.get<const JointComponent>(e);
        const std::string tag = "entity " + std::to_string(entt::to_integral(e));

        auto self = s.bodyByEntity.find(entt::to_integral(e));
        if (self == s.bodyByEntity.end()) {
            Log::Warn("PhysX: " + tag + " has a Joint but no Rigidbody — skipped."); ++skipped; continue;
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
        ++made;
    }
    if (made || skipped)
        Log::Info("PhysX: " + std::to_string(made) + " joint(s)" +
                  (skipped ? ", " + std::to_string(skipped) + " skipped" : "") + ".");
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

void SimEventCallback::onTrigger(PxTriggerPair* pairs, PxU32 count) {
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

// Closing speed of the two actors along `n`, from the velocities captured just before the
// step (see PhysicsState::preStepVel) — so it reflects the actual impact, not the post-solve
// resting state.
float NormalClosingSpeed(PhysicsState& s, const PxActor* a0, const PxActor* a1, const PxVec3& n) {
    auto vel = [&](const PxActor* a) -> PxVec3 {
        auto it = a ? s.preStepVel.find(a) : s.preStepVel.end();
        return it != s.preStepVel.end() ? it->second : PxVec3(0.0f);
    };
    return std::abs((vel(a0) - vel(a1)).dot(n));
}

void SimEventCallback::onContact(const PxContactPairHeader& header, const PxContactPair* pairs, PxU32 count) {
    if (!owner) return;
    if (header.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 | PxContactPairHeaderFlag::eREMOVED_ACTOR_1))
        return;
    const std::uint32_t a = UserDataToEntity(header.actors[0] ? header.actors[0]->userData : nullptr);
    const std::uint32_t b = UserDataToEntity(header.actors[1] ? header.actors[1]->userData : nullptr);

    for (PxU32 i = 0; i < count; ++i) {
        const PxContactPair& cp = pairs[i];
        if (cp.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 | PxContactPairFlag::eREMOVED_SHAPE_1)) continue;

        std::uint32_t kind;
        if      (cp.events & PxPairFlag::eNOTIFY_TOUCH_FOUND)    kind = ContactEvent::Enter;
        else if (cp.events & PxPairFlag::eNOTIFY_TOUCH_LOST)     kind = ContactEvent::Exit;
        else if (cp.events & PxPairFlag::eNOTIFY_TOUCH_PERSISTS) kind = ContactEvent::Stay;
        else continue;

        PxContactPairPoint pts[8];
        const PxU32 n = cp.extractContacts(pts, 8);
        float sumImpulse = 0.0f;
        PxVec3 p0(0.0f), nrm(0.0f, 1.0f, 0.0f);
        for (PxU32 k = 0; k < n; ++k) {
            sumImpulse += pts[k].impulse.magnitude();
            if (k == 0) { p0 = pts[k].position; nrm = pts[k].normal; }
        }
        // Resting stacks generate a persistent contact every substep with a small hold impulse;
        // only surface Stay for genuine ongoing pressure.
        if (kind == ContactEvent::Stay && sumImpulse < 0.05f) continue;

        ContactEvent ev;
        ev.Kind = kind; ev.A = a; ev.B = b;
        ev.Point[0]  = p0.x;  ev.Point[1]  = p0.y;  ev.Point[2]  = p0.z;
        ev.Normal[0] = nrm.x; ev.Normal[1] = nrm.y; ev.Normal[2] = nrm.z;
        ev.Impulse     = sumImpulse;
        ev.NormalSpeed = NormalClosingSpeed(*owner, header.actors[0], header.actors[1], nrm);
        owner->contactEvents.push_back(ev);

        // Visual debugger (#185 PDD_Contacts): every fresh touch leaves a fading spark; a
        // firm one (real bounce / drop) gets the brighter "impact" burst.
        if (kind == ContactEvent::Enter && n > 0)
            PushContactViz(p0, nrm, sumImpulse, sumImpulse > 1.5f);

        if (kind != ContactEvent::Stay && sumImpulse > 0.75f) {
            char la[24], lb[24];
            char imp[32]; std::snprintf(imp, sizeof(imp), " (impulse %.1f)", sumImpulse);
            Log::Info(std::string("Contact ") + (kind == ContactEvent::Enter ? "hit:  " : "end:  ") +
                      EntityLabel(a, la) + " <-> " + EntityLabel(b, lb) +
                      (kind == ContactEvent::Enter ? imp : ""));
        }
    }
}

void SimEventCallback::onConstraintBreak(PxConstraintInfo* constraints, PxU32 count) {
    if (!owner) return;
    for (PxU32 i = 0; i < count; ++i) {
        PxU32 typeID = 0xFFFFFFFF;
        void* ext = constraints[i].constraint ? constraints[i].constraint->getExternalReference(typeID) : nullptr;
        if (typeID != PxConstraintExtIDs::eJOINT || !ext) continue;
        auto it = owner->jointOwner.find(static_cast<PxJoint*>(ext));
        const std::uint32_t owE = it != owner->jointOwner.end() ? it->second : 0xFFFFFFFFu;
        Log::Info("Joint broke on entity " + std::to_string(owE) + ".");
    }
}

// #185 PR 10 — the Player capsule hit something while moving. Push a dynamic body out of the
// way (scaled so light things fly and heavy things barely budge), and if the thing is a
// kinematic body underfoot, remember it so MoveCharacter can carry the Player along with it.
void PlayerHitReport::onShapeHit(const PxControllerShapeHit& hit) {
    if (!owner || !hit.actor) return;
    PxRigidDynamic* body = hit.actor->is<PxRigidDynamic>();
    if (!body) return;
    const bool kin = (body->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC);
    const PxVec3 n(hit.worldNormal.x, hit.worldNormal.y, hit.worldNormal.z);

    if (kin) {
        if (n.y > 0.4f) { // standing on top of a moving platform
            if (owner->playerGround != body) {
                owner->playerGround = body;
                const PxTransform gp = body->getGlobalPose();
                owner->playerGroundLastPos = gp.p;
                owner->playerGroundLastRot = gp.q;
            }
            owner->playerGroundHitThisMove = true;
        }
        return; // don't shove a kinematic body around
    }

    // Push proportional to how hard we walked into it. Impulse scales with mass so the
    // resulting velocity change is ~constant regardless of body weight (a heavy crate still
    // budges, a light one doesn't rocket away). Strength is project-tunable.
    const PxVec3 dir(hit.dir.x, hit.dir.y, hit.dir.z);
    const float mass = body->getMass() > 0.0f ? body->getMass() : 1.0f;
    const float strength = ProjectSettings::Physics().PlayerPushStrength;
    PxRigidBodyExt::addForceAtPos(*body, dir * (strength * mass),
                                  PxVec3((float)hit.worldPos.x, (float)hit.worldPos.y, (float)hit.worldPos.z),
                                  PxForceMode::eIMPULSE, true);
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
    PxOmniPvd* omniArg = nullptr;
#ifdef TARTARUS_PHYSX_OMNIPVD
    // #185 G — write an .ovd capture next to the exe for the Omniverse PhysX inspector (full
    // timeline scrubbing of every actor / shape / joint / contact / query). Opt-in at build
    // time: needs PX_BUILDPVDRUNTIME=ON so PVDRuntime_64.dll gets built + harvested (see
    // CMakeLists). A missing runtime DLL just means no capture — the sim runs regardless.
    s->omniPvd = PxCreateOmniPvd(*s->foundation);
    if (s->omniPvd && s->omniPvd->getWriter() && s->omniPvd->getFileWriteStream()) {
        OmniPvdFileWriteStream* fs = s->omniPvd->getFileWriteStream();
        fs->setFileName("physx_capture.ovd");
        s->omniPvd->getWriter()->setWriteStream(*static_cast<OmniPvdWriteStream*>(fs));
        omniArg = s->omniPvd;
    } else if (s->omniPvd) {
        s->omniPvd->release();
        s->omniPvd = nullptr;
        Log::Warn("PhysX: OmniPVD runtime unavailable (PVDRuntime_64.dll missing) — no .ovd capture.");
    }
#endif
    s->physics = PxCreatePhysics(PX_PHYSICS_VERSION, *s->foundation, PxTolerancesScale(),
                                 /*trackOutstandingAllocations=*/true, pvdArg, omniArg);
    if (!s->physics) {
        Log::Error("PhysX: PxCreatePhysics failed — physics disabled for this Play session.");
#ifdef TARTARUS_PHYSX_PVD
        if (s->pvd) s->pvd->release();
        if (s->pvdTransport) s->pvdTransport->release();
#endif
#ifdef TARTARUS_PHYSX_OMNIPVD
        if (s->omniPvd) s->omniPvd->release();
#endif
        s->foundation->release();
        delete s;
        return;
    }
#ifdef TARTARUS_PHYSX_OMNIPVD
    if (s->omniPvd && s->physics->getOmniPvd()) {
        s->physics->getOmniPvd()->startSampling();
        Log::Info("PhysX: OmniPVD capture -> physx_capture.ovd");
    }
#endif

    // Keep the worker pool modest: the engine is otherwise single-threaded, and PR 2 only has
    // static actors. hardware_concurrency() can report 0 — clamp to at least 1.
    unsigned hw = std::thread::hardware_concurrency();
    PxU32 workers = std::max<PxU32>(1, std::min<PxU32>(hw ? hw - 1 : 1, 4));
    s->dispatcher = PxDefaultCpuDispatcherCreate(workers);

    s->defaultMaterial = s->physics->createMaterial(0.6f, 0.6f, 0.0f);

    s->simCb.owner = s;      // #185 PR 5
    s->hitReport.owner = s;  // #185 PR 10
    // #185 PR 8 — snapshot the layer collision matrix; PhysX copies it as the filter constant block.
    for (int i = 0; i < 8; ++i) s->layerMask[i] = ProjectSettings::Physics().LayerCollisionMask[i];

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
#ifdef TARTARUS_PHYSX_OMNIPVD
        if (s->omniPvd) s->omniPvd->release();
#endif
        s->foundation->release();
        delete s;
        return;
    }

    s->controllerMgr = PxCreateControllerManager(*s->scene);

    g_State = s;
    Log::Info("PhysX world created (" + std::to_string(workers) + " worker threads).");

    BuildActors(*s, world);
    BuildJoints(*s, world); // #185 PR 11
}

void Destroy() {
    if (!g_State) return;
    PhysicsState* s = g_State;
    g_State = nullptr; // clear first so a re-entrant Step() during teardown is a no-op

    // Reverse construction order. scene->release() drops every actor/shape it owns.
    for (PxJoint* j : s->joints)               if (j) j->release(); // #185 PR 11 — before the scene
    if (s->controller)      s->controller->release();
    if (s->controllerMgr)   s->controllerMgr->release();
    if (s->scene)           s->scene->release();
    for (auto& kv : s->materialCache)          if (kv.second) kv.second->release(); // #185 PR 7
    for (auto& kv : s->convexCache)            if (kv.second) kv.second->release(); // #185 PR 6/12 — after the scene's shapes
    for (auto& kv : s->triangleCache)          if (kv.second) kv.second->release();
    for (PxConvexMesh* m : s->convexMeshes)   if (m) m->release(); // legacy non-cached path (now unused)
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
#ifdef TARTARUS_PHYSX_OMNIPVD
    if (s->omniPvd) s->omniPvd->release(); // after physics->release()
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

// #185 debug — one fixed substep: velocity snapshot (for impact speed), simulate, fetch.
// Shared by Step() and StepOneSubstep().
void RunOneSubstep(PhysicsState& s, float fixedStep) {
    s.preStepVel.clear();
    for (auto& [body, e] : s.dynamics) s.preStepVel[body] = body->getLinearVelocity();
    s.scene->simulate(fixedStep);
    s.scene->fetchResults(/*block=*/true);
    ++s.frameIndex;
    s.simClock += fixedStep;
}

// #185 debug — reject a NaN/inf simulated pose instead of poisoning the TransformComponent
// (which then spreads through the world-transform cache and the renderer). Freezes the body.
bool PoseIsFinite(const PxTransform& p) {
    auto ok = [](float f) { return std::isfinite(f); };
    return ok(p.p.x) && ok(p.p.y) && ok(p.p.z) &&
           ok(p.q.x) && ok(p.q.y) && ok(p.q.z) && ok(p.q.w);
}

void Step(float dt, World& world) {
    if (!g_State || !g_State->scene) return;
    if (dt <= 0.0f) return;
    const float rawDt = dt;
    dt *= g_SimTimeScale;                 // #185 — slow-mo / freeze
    if (dt <= 0.0f) { g_State->lastSubsteps = 0; g_State->lastStepMillis = 0.0f; return; }

    // Age the debug-draw history (contacts / queries) so sparks fade over ~½ second.
    for (int i = 0; i < g_ContactCount; ++i) g_Contacts[i].age += rawDt;
    for (int i = 0; i < g_QueryCount; ++i)   g_Queries[i].age  += rawDt;

    g_State->triggerEvents.clear();
    g_State->enteredThisFrame.clear();
    g_State->contactEvents.clear(); // #185 PR 7 — onContact refills it during the sim below

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

    const auto t0 = std::chrono::steady_clock::now();
    int steps = 0;
    while (g_State->stepAccumulator >= fixedStep && steps < kMaxSubSteps) {
        RunOneSubstep(*g_State, fixedStep);
        g_State->stepAccumulator -= fixedStep;
        ++steps;
    }
    g_State->lastSubsteps = steps;
    g_State->lastStepMillis = steps ? std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - t0).count() : 0.0f;

    // Write each force-driven body's simulated pose back to its TransformComponent so the
    // renderer, gizmos and world-transform cache all see it move. Play -> Stop reloads the
    // authored scene, so this is never persisted.
    for (auto& [body, e] : g_State->dynamics) {
        if (!world.Registry.valid(e)) continue;
        auto* tc = world.Registry.try_get<TransformComponent>(e);
        if (!tc) continue;
        const PxTransform p = body->getGlobalPose();
        if (!PoseIsFinite(p)) { // #185 — blow-up guard: don't poison the TransformComponent
            body->setLinearVelocity(PxVec3(0.0f));
            body->setAngularVelocity(PxVec3(0.0f));
            body->putToSleep();
            body->setGlobalPose(PxTransform(ToPx(tc->Position), EulerToPx(tc->RotationEuler)));
            Log::Warn("PhysX: entity " + std::to_string(entt::to_integral(e)) +
                      " produced a non-finite pose — frozen at its last good transform.");
            continue;
        }
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

// Drop the Player capsule from scene queries — a "from the player" ray/sweep almost never
// wants to hit itself, and its shape sits right on the query origin (#185 hardening).
struct ExcludeActorFilter : PxQueryFilterCallback {
    const PxRigidActor* skip = nullptr;
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape*, const PxRigidActor* actor,
                                   PxHitFlags&) override {
        return (actor && actor == skip) ? PxQueryHitType::eNONE : PxQueryHitType::eBLOCK;
    }
    PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*,
                                    const PxRigidActor*) override {
        return PxQueryHitType::eBLOCK;
    }
};
const PxRigidActor* PlayerActor() {
    return (g_State && g_State->controller) ? g_State->controller->getActor() : nullptr;
}

bool Raycast(const float origin[3], const float dir[3], float maxDistance, RaycastHit& outHit) {
    outHit = RaycastHit{};
    if (!g_State || !g_State->scene) return false;

    PxVec3 d(dir[0], dir[1], dir[2]);
    const float len = d.magnitude();
    if (len < 1e-8f || maxDistance <= 0.0f) return false;
    d *= (1.0f / len);

    ExcludeActorFilter filter; filter.skip = PlayerActor();
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
    desc.reportCallback = &g_State->hitReport;               // #185 PR 10 — push bodies / ride platforms

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
    // hit on the player) can tell it apart from a real entity (#185 PR 5). Put it on the
    // project-configured Player layer so the collision matrix applies to it (#185 PR 8).
    if (PxRigidActor* a = g_State->controller->getActor()) {
        a->userData = reinterpret_cast<void*>(static_cast<std::uintptr_t>(kPlayerEntity));
        const PxU32 pl = (PxU32)std::clamp(ProjectSettings::Physics().PlayerLayer, 0, 7);
        const PxU32 nbShapes = a->getNbShapes();
        std::vector<PxShape*> shapes(nbShapes);
        a->getShapes(shapes.data(), nbShapes);
        for (PxShape* sh : shapes) sh->setSimulationFilterData(PxFilterData(1u << pl, pl, 0u, 0u));
    }
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

bool GetCharacterCapsule(float outFootPos[3], float* outRadius, float* outCylHalfHeight) {
    if (!HasCharacter()) return false;
    auto* cap = static_cast<PxCapsuleController*>(g_State->controller);
    const PxExtendedVec3 f = cap->getFootPosition();
    outFootPos[0] = (float)f.x; outFootPos[1] = (float)f.y; outFootPos[2] = (float)f.z;
    if (outRadius)         *outRadius = cap->getRadius();
    if (outCylHalfHeight)  *outCylHalfHeight = 0.5f * cap->getHeight();
    return true;
}

unsigned MoveCharacter(const float disp[3], float dt) {
    if (!HasCharacter() || dt <= 0.0f) return 0;

    PxVec3 d(disp[0], disp[1], disp[2]);
    // #185 PR 10 — ride a moving platform: add the ground kinematic's translation since the
    // last move, and expose its yaw change for Player::Update to fold into the camera.
    g_State->playerGroundHitThisMove = false;
    g_State->platformYawDeltaDeg = 0.0f;
    if (g_State->playerGround) {
        const PxTransform gp = g_State->playerGround->getGlobalPose();
        d += gp.p - g_State->playerGroundLastPos;
        const PxQuat dq = gp.q * g_State->playerGroundLastRot.getConjugate();
        const float yaw = std::atan2(2.0f * (dq.w * dq.y + dq.x * dq.z),
                                     1.0f - 2.0f * (dq.y * dq.y + dq.z * dq.z));
        g_State->platformYawDeltaDeg = glm::degrees(yaw);
        g_State->playerGroundLastPos = gp.p;
        g_State->playerGroundLastRot = gp.q;
    }

    PxControllerFilters filters;
    PxControllerCollisionFlags f =
        g_State->controller->move(d, /*minDist=*/0.001f, dt, filters); // onShapeHit fires in here

    unsigned out = 0;
    if (f & PxControllerCollisionFlag::eCOLLISION_SIDES) out |= CC_SIDES;
    if (f & PxControllerCollisionFlag::eCOLLISION_UP)    out |= CC_UP;
    if (f & PxControllerCollisionFlag::eCOLLISION_DOWN)  out |= CC_DOWN;

    // Stepped off the platform (no downward hit on it this move) — stop carrying it.
    if (!g_State->playerGroundHitThisMove || !(out & CC_DOWN))
        g_State->playerGround = nullptr;
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

// --- Forces & read-back (#185 PR 7) --------------------------------------------------------

namespace {
// A non-kinematic dynamic body for `entity`, or null. bodyByEntity holds kinematics too, so
// filter those out here — force/velocity calls on a kinematic body are meaningless.
PxRigidDynamic* DynamicFor(unsigned entity) {
    if (!g_State) return nullptr;
    auto it = g_State->bodyByEntity.find(entity);
    if (it == g_State->bodyByEntity.end()) return nullptr;
    PxRigidDynamic* b = it->second;
    return (b->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) ? nullptr : b;
}
PxForceMode::Enum ToForceMode(unsigned mode) {
    switch (mode) {
        case 1:  return PxForceMode::eIMPULSE;
        case 2:  return PxForceMode::eVELOCITY_CHANGE;
        case 3:  return PxForceMode::eACCELERATION;
        default: return PxForceMode::eFORCE;
    }
}
} // namespace

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

int GetContactEvents(ContactEvent* out, int maxEvents) {
    if (!g_State) return 0;
    const int total = (int)g_State->contactEvents.size();
    const int n = (maxEvents < total) ? maxEvents : total;
    for (int i = 0; i < n && out; ++i) out[i] = g_State->contactEvents[(size_t)i];
    return total;
}

float PlatformYawDelta() {
    return g_State ? g_State->platformYawDeltaDeg : 0.0f;
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

    ExcludeActorFilter filter; filter.skip = PlayerActor();
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
    PxOverlapHit hits[64];
    PxOverlapBuffer buf(hits, 64);
    const PxTransform pose(PxVec3(center[0], center[1], center[2]));
    if (!g_State->scene->overlap(PxSphereGeometry(radius), pose, buf)) return 0;

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

// ==================================================================================
// #185 debug tooling — a small, general-use visual debugger
// ==================================================================================

namespace {
PxVec3 WorldCOM(const PxRigidDynamic* d) {
    return d->getGlobalPose().transform(d->getCMassLocalPose().p);
}
} // namespace

PhysicsDebugStats GetDebugStats() {
    PhysicsDebugStats s;
    if (!g_State || !g_State->scene) return s;
    s.Active = true;
    s.Substeps = g_State->lastSubsteps;
    s.StepMillis = g_State->lastStepMillis;
    s.TimeScale = g_SimTimeScale;
    s.DynamicBodies = (int)g_State->dynamics.size();
    s.KinematicBodies = (int)g_State->kinematics.size();
    s.StaticActors = (int)g_State->staticByEntity.size();
    for (auto& [b, e] : g_State->dynamics) (b->isSleeping() ? s.AsleepBodies : s.AwakeBodies)++;
    s.ContactsThisFrame = (int)g_State->contactEvents.size();
    s.TriggerOverlaps = (int)g_State->triggerOverlaps.size() + (int)g_State->playerTriggers.size();
    s.Joints = (int)g_State->joints.size();
    for (PxJoint* j : g_State->joints)
        if (j && (j->getConstraintFlags() & PxConstraintFlag::eBROKEN)) s.BrokenJoints++;
    const PxVec3 g = g_State->scene->getGravity();
    s.Gravity[0] = g.x; s.Gravity[1] = g.y; s.Gravity[2] = g.z;
    s.FixedStep = FixedStep();
    s.FrameIndex = g_State->frameIndex;
    return s;
}

void     SetDebugDrawFlags(unsigned flags) { g_DebugDrawFlags = flags; }
unsigned GetDebugDrawFlags()               { return g_DebugDrawFlags; }

// Build this frame's debug geometry as pre-coloured, pre-faded RGBA line segments. The renderer
// draws them additively, so alpha is the glow/fade. 7 floats/vertex, 2 verts/line.
int CopyDebugLines(float* out, int maxLines) {
    if (!g_State || !g_State->scene) return 0;
    int n = 0;
    auto L = [&](const PxVec3& a, const PxVec3& b, float r, float g, float bl, float al) {
        if (out && n < maxLines) {
            float* p = out + n * 14;
            p[0]=a.x; p[1]=a.y; p[2]=a.z; p[3]=r; p[4]=g; p[5]=bl; p[6]=al;
            p[7]=b.x; p[8]=b.y; p[9]=b.z; p[10]=r; p[11]=g; p[12]=bl; p[13]=al;
        }
        ++n;
    };
    auto basis = [](const PxVec3& d, PxVec3& u, PxVec3& v) {
        const PxVec3 a = (std::fabs(d.x) < 0.9f) ? PxVec3(1,0,0) : PxVec3(0,1,0);
        u = d.cross(a); const float m = u.magnitude();
        u = (m > 1e-4f) ? u * (1.0f / m) : PxVec3(0,1,0);
        v = d.cross(u);
    };
    auto arrow = [&](const PxVec3& from, const PxVec3& to, float r, float g, float bl, float al) {
        L(from, to, r, g, bl, al);
        PxVec3 d = to - from; const float len = d.magnitude();
        if (len < 1e-4f) return;
        d = d * (1.0f / len);
        PxVec3 u, v; basis(d, u, v);
        const float h = std::min(0.18f, len * 0.35f);
        for (int s = -1; s <= 1; s += 2) {
            L(to, to - d * h + u * (h * 0.5f * (float)s), r, g, bl, al);
            L(to, to - d * h + v * (h * 0.5f * (float)s), r, g, bl, al);
        }
    };
    auto ring = [&](const PxVec3& c, const PxVec3& u, const PxVec3& v, float rad, int seg,
                    float r, float g, float bl, float al) {
        PxVec3 prev = c + u * rad;
        for (int i = 1; i <= seg; ++i) {
            const float a = (float)i / (float)seg * 6.2831853f;
            const PxVec3 p = c + (u * std::cos(a) + v * std::sin(a)) * rad;
            L(prev, p, r, g, bl, al); prev = p;
        }
    };
    const unsigned f = g_DebugDrawFlags;

    // Palette (pure, high-contrast): contacts + raycasts red (#FF0000); velocity + sleep green
    // (#00FF00). Impulse / speed reads through the fade alpha, not hue, so colour stays saturated.
    const float GR = 0.0f, GG = 1.0f, GB = 0.0f;   // green — velocity, sleep
    const float RR = 1.0f, RG = 0.0f, RB = 0.0f;   // red   — contacts, raycasts

    // --- Contacts & impacts: a fading spark + impulse-scaled normal arrow; firm hits get an
    //     expanding ring. Lingers ~2 s (kContactFade).
    if (f & PDD_Contacts) {
        for (int i = 0; i < g_ContactCount; ++i) {
            const ContactViz& c = g_Contacts[i];
            if (c.age >= kContactFade) continue;
            const float k  = 1.0f - c.age / kContactFade;   // 1 -> 0
            const float al = k;                              // linear fade — stays visible longer
            const float sz = (c.impact ? 0.22f : 0.10f) * (0.55f + 0.45f * k);
            L(c.p - PxVec3(sz,0,0), c.p + PxVec3(sz,0,0), RR, RG, RB, al);
            L(c.p - PxVec3(0,sz,0), c.p + PxVec3(0,sz,0), RR, RG, RB, al);
            L(c.p - PxVec3(0,0,sz), c.p + PxVec3(0,0,sz), RR, RG, RB, al);
            arrow(c.p, c.p + c.n * (0.25f + std::min(1.2f, c.impulse * 0.08f)), RR, RG, RB, al);
            if (c.impact) {
                PxVec3 u, v; basis(c.n, u, v);
                // Shockwave ring: expands modestly, holds full brightness until the last third.
                const float rr = sz * 2.6f * (1.0f + (1.0f - k) * 1.4f);
                const float ra = std::min(1.0f, k * 2.2f);   // only fade near the end
                ring(c.p, u, v, rr,        18, RR, RG, RB, ra);
                ring(c.p, u, v, rr * 0.88f, 18, RR, RG, RB, ra);
            }
        }
    }

    // --- Raycasts / sweeps: bright travelled segment, faint continuation on a miss, a small
    //     burst + normal arrow on a hit. Overlap queries draw as three rings. All red.
    if (f & PDD_Raycasts) {
        for (int i = 0; i < g_QueryCount; ++i) {
            const QueryRecord& q = g_Queries[i];
            if (q.age >= kQueryFade) continue;
            const float k  = 1.0f - q.age / kQueryFade;
            const float al = 0.20f + 0.80f * k;
            if (q.kind == 2) {
                const PxVec3 x(1,0,0), y(0,1,0), z(0,0,1);
                ring(q.origin, x, y, q.radius, 24, RR, RG, RB, al);
                ring(q.origin, y, z, q.radius, 24, RR, RG, RB, al);
                ring(q.origin, z, x, q.radius, 24, RR, RG, RB, al);
                continue;
            }
            const PxVec3 end = q.hit ? q.hitPoint : q.origin + q.dir * q.dist;
            L(q.origin, end, RR, RG, RB, al);
            if (!q.hit) {
                L(end, q.origin + q.dir * q.dist, RR, RG, RB, al * 0.28f);
            } else {
                const float s = 0.13f;
                L(q.hitPoint - PxVec3(s,0,0), q.hitPoint + PxVec3(s,0,0), RR, RG, RB, al);
                L(q.hitPoint - PxVec3(0,s,0), q.hitPoint + PxVec3(0,s,0), RR, RG, RB, al);
                L(q.hitPoint - PxVec3(0,0,s), q.hitPoint + PxVec3(0,0,s), RR, RG, RB, al);
                arrow(q.hitPoint, q.hitPoint + q.hitNormal * 0.5f, RR, RG, RB, al);
            }
        }
    }

    // --- Velocity: a green arrow from each awake body's centre of mass.
    if (f & PDD_Velocity) {
        for (auto& [b, e] : g_State->dynamics) {
            if (b->isSleeping()) continue;
            const PxVec3 v = b->getLinearVelocity();
            if (v.magnitude() < 0.15f) continue;
            const PxVec3 cm = WorldCOM(b);
            arrow(cm, cm + v * 0.16f, GR, GG, GB, 0.95f);
        }
    }

    // --- Sleep: a dim green "z" hovering over each sleeping body.
    if (f & PDD_Sleep) {
        for (auto& [b, e] : g_State->dynamics) {
            if (!b->isSleeping()) continue;
            const PxVec3 t = b->getGlobalPose().p + PxVec3(0, 0.5f, 0);
            L(t + PxVec3(-0.08f, 0.12f, 0), t + PxVec3(0.08f, 0.12f, 0), GR, GG, GB, 0.5f);
            L(t + PxVec3( 0.08f, 0.12f, 0), t + PxVec3(-0.08f, 0.0f, 0), GR, GG, GB, 0.5f);
            L(t + PxVec3(-0.08f, 0.0f, 0),  t + PxVec3(0.08f, 0.0f, 0),  GR, GG, GB, 0.5f);
        }
    }
    return n;
}

void  SetSimTimeScale(float scale) { g_SimTimeScale = std::clamp(scale, 0.0f, 2.0f); }
float GetSimTimeScale()            { return g_SimTimeScale; }

void StepOneSubstep(World& world) {
    if (!g_State || !g_State->scene) return;
    g_State->triggerEvents.clear();
    g_State->enteredThisFrame.clear();
    g_State->contactEvents.clear();
    for (auto& [body, e] : g_State->kinematics) {
        if (!world.Registry.valid(e)) continue;
        if (const auto* tc = world.Registry.try_get<const TransformComponent>(e))
            body->setKinematicTarget(PxTransform(ToPx(tc->Position), EulerToPx(tc->RotationEuler)));
    }
    RunOneSubstep(*g_State, FixedStep());
    g_State->lastSubsteps = 1;
    for (auto& [body, e] : g_State->dynamics) {
        if (!world.Registry.valid(e)) continue;
        auto* tc = world.Registry.try_get<TransformComponent>(e);
        if (!tc) continue;
        const PxTransform p = body->getGlobalPose();
        if (!PoseIsFinite(p)) continue;
        tc->Position = glm::vec3(p.p.x, p.p.y, p.p.z);
        tc->RotationEuler = PxQuatToEulerDeg(p.q);
    }
}

void SetQueryRecording(bool on) { g_QueryRecording = on; if (!on) { g_QueryCount = 0; g_QueryHead = 0; } }
bool GetQueryRecording()        { return g_QueryRecording; }

void SetActorPose(unsigned entity, const float posXYZ[3], const float rotEulerDeg[3], bool zeroVelocity) {
    if (!g_State) return;
    const PxTransform pose(PxVec3(posXYZ[0], posXYZ[1], posXYZ[2]),
                           EulerToPx(glm::vec3(rotEulerDeg[0], rotEulerDeg[1], rotEulerDeg[2])));
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

// --- Gravity gun (#185 hardening, physics-playground debug harness) ------------------
// "Carry an object in front of the camera" grab. GrabBody latches a dynamic body and
// suspends its gravity; UpdateGrab (called each frame with the desired world point) servos
// its velocity toward that point and cancels spin; ReleaseBody lets go, optionally firing
// a velocity-change impulse to launch it.

void GrabBody(unsigned entity) {
    if (!g_State) return;
    PxRigidDynamic* b = DynamicFor(entity);
    if (!b) return;
    g_State->grabbed = b;
    g_State->grabSavedAngularDamping  = b->getAngularDamping();
    g_State->grabSavedGravityDisabled = b->getActorFlags().isSet(PxActorFlag::eDISABLE_GRAVITY);
    b->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, true);
    b->setAngularDamping(8.0f);
    b->wakeUp();
}

bool IsGrabbing() { return g_State && g_State->grabbed != nullptr; }

void UpdateGrab(const float target[3]) {
    if (!g_State || !g_State->grabbed) return;
    PxRigidDynamic* b = g_State->grabbed;
    const PxVec3 to = PxVec3(target[0], target[1], target[2]) - b->getGlobalPose().p;
    PxVec3 v = to * 12.0f; // proportional pull toward the hold point
    const float maxSpeed = 30.0f; // cap so a distant grab doesn't tunnel through geometry
    if (v.magnitude() > maxSpeed) v = v.getNormalized() * maxSpeed;
    b->setLinearVelocity(v);
    b->setAngularVelocity(PxVec3(0.0f));
}

void ReleaseBody(bool launch, const float impulse[3]) {
    if (!g_State || !g_State->grabbed) return;
    PxRigidDynamic* b = g_State->grabbed;
    b->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, g_State->grabSavedGravityDisabled);
    b->setAngularDamping(g_State->grabSavedAngularDamping);
    if (launch) {
        b->setLinearVelocity(PxVec3(0.0f));
        b->addForce(PxVec3(impulse[0], impulse[1], impulse[2]), PxForceMode::eVELOCITY_CHANGE, /*autowake=*/true);
    }
    g_State->grabbed = nullptr;
}

// --- Debug (#185 PR 12) --------------------------------------------------------------

int CopyContactPoints(float* outXYZ, int maxPoints) {
    if (!g_State) return 0;
    int n = 0;
    for (const ContactEvent& ev : g_State->contactEvents) {
        if (ev.Kind == ContactEvent::Exit) continue;
        if (outXYZ && n < maxPoints) {
            outXYZ[n * 3 + 0] = ev.Point[0];
            outXYZ[n * 3 + 1] = ev.Point[1];
            outXYZ[n * 3 + 2] = ev.Point[2];
        }
        ++n;
    }
    return n;
}

} // namespace PhysicsWorld
