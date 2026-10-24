#include "PhysicMaterialAsset.h"
#include "PhysicsWorld.h"
#include "CharacterMover.h"

#include "Log.h"
#include "ProjectSettings.h"
#include "TimeService.h"
#include "World.h"
#include "Components.h"
#include "Model.h"
#include "GameModuleAPI.h" // RaycastHit / TriggerEvent / ContactEvent / BodyState / ForceMode, kPlayerEntity

#include <PxPhysicsAPI.h>
#include <unordered_set>
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
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
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

// A per-step table (a body's pose or velocity before the step) refilled every substep. Refilling an
// unordered_map after clear() freed and reallocated a node per awake body per substep; this keeps the
// nodes and marks which entries the current refill wrote, so a body skipped this time reads as absent.
template <class K, class V>
struct RefilledMap {
    std::unordered_map<K, std::pair<V, std::uint32_t>> Map;
    std::uint32_t Stamp = 1;
    void BeginRefill() { if (++Stamp == 0) { Map.clear(); Stamp = 1; } } // what clear() was
    void Set(K k, const V& v) { auto& e = Map[k]; e.first = v; e.second = Stamp; }
    const V* Find(K k) const {
        const auto it = Map.find(k);
        return it != Map.end() && it->second.second == Stamp ? &it->second.first : nullptr;
    }
    void Erase(K k) { Map.erase(k); }
};

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
// data is its layer 0-31, constantBlock is ProjectSettings' 32-word mask, #150), and asks for CCD
// contact detection so a body with the eENABLE_CCD flag actually sweeps (#185 PR 9).
// word1 above the layer (its low five bits): a character's shape that passes through ragdolls (SetNpcPassesRagdolls).
constexpr PxU32 kPassRagdolls = 1u << 30;
// word1 of a ragdoll's parts: it started in or on another body, and passes the other ragdolls (not its own parts, not the world)
// until it is clear of them (UpdateBodyPasses).
constexpr PxU32 kPassBodies = 1u << 29;
// A ragdoll's own part (word2: its id + 1, the part 0..31 above), not an object told to ignore one (slot kRagdollIgnorerSlot).
constexpr PxU32 kRagdollIgnorerSlot = 32u;
inline bool IsRagdollPart(const PxFilterData& f) { return (f.word2 & 0xFFFFu) != 0u && ((f.word2 >> 16) & 63u) < kRagdollIgnorerSlot; }

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

// #167 - everything that doesn't depend on the scene lives for the whole editor session: the
// foundation, PxPhysics, the CPU dispatcher, materials and the cooked-mesh cache. Before, all of
// it was torn down on every Stop and rebuilt on every Play, so each Play re-cooked every mesh
// collider. Created on the first Play, released by PhysicsWorld::Shutdown() at exit.
struct PhysicsCore {
    PxDefaultAllocator      allocator;
    EngineErrorCallback     errorCallback;
    PxFoundation*           foundation      = nullptr;
    PxPhysics*              physics         = nullptr;
    PxDefaultCpuDispatcher* dispatcher      = nullptr;
    PxMaterial*             defaultMaterial = nullptr;
    PxU32                   workers         = 1;
    std::map<long long, PxMaterial*>                 materialCache; // key: GetMaterial()
    // Keyed by CookKey (model path + file timestamp + geometry size), so a model reimported
    // mid-session gets a fresh collider instead of the stale cooked one.
    std::unordered_map<std::string, PxConvexMesh*>   convexCache;
    std::unordered_map<std::string, PxTriangleMesh*> triangleCache;
#ifdef TARTARUS_PHYSX_PVD
    PxPvd*               pvd            = nullptr;
    PxPvdTransport*      pvdTransport   = nullptr;
#endif
#ifdef TARTARUS_PHYSX_OMNIPVD
    PxOmniPvd*           omniPvd        = nullptr; // #185 G — .ovd capture for the Omniverse PhysX inspector
#endif
};
PhysicsCore* g_Core = nullptr;

struct PhysicsState {
    explicit PhysicsState(PhysicsCore& core)
        : foundation(core.foundation), physics(core.physics), dispatcher(core.dispatcher),
          defaultMaterial(core.defaultMaterial), materialCache(core.materialCache),
          convexCache(core.convexCache), triangleCache(core.triangleCache) {}
    SimEventCallback       simCb;
    PxU32                  layerMask[32] = {}; // #185 PR 8 constant block; one row per layer (#150), filled at Create
    // Borrowed from PhysicsCore (#167) - never released here.
    PxFoundation*          foundation   = nullptr;
    PxPhysics*             physics      = nullptr;
    PxDefaultCpuDispatcher* dispatcher  = nullptr;
    PxScene*              scene         = nullptr;
    PxMaterial*           defaultMaterial = nullptr;
    // The Play-mode Player's capsule (lazy): a kinematic capsule of our own, moved by CharacterMover's collide-and-slide.
    struct CharacterBody {
        PxRigidDynamic* Actor = nullptr;
        PxShape* Shape = nullptr;
        MoverCapsule Cap;
        glm::vec3 Feet{0.0f};
        bool Grounded = false;
        MoverResult Last;
        MoverDebug Debug;
    };
    std::unique_ptr<CharacterBody> playerBody;
    float playerDoorAssist = 0.0f, playerDoorAssistMaxAngle = 35.0f; // SetCharacterDoorwayAssist
    // NPC capsules, indexed by CharacterId (a released slot stays null). npcActors: their actors,
    // for the query filters that skip characters.
    struct NpcCharacter {
        std::uint32_t entity = 0xFFFFFFFFu;
        bool passRagdolls = false;
        std::unique_ptr<CharacterBody> body;
        bool Used() const { return body != nullptr; }
        PxRigidActor* Actor() const { return body ? static_cast<PxRigidActor*>(body->Actor) : nullptr; }
    };
    std::vector<NpcCharacter>               npcs;
    std::unordered_set<const PxRigidActor*> npcActors;
    // #185 PR 4 — RigidbodyComponent entities. Force-driven bodies get their pose written back
    // to TransformComponent after each Step; kinematic bodies are driven the other way, from
    // TransformComponent, before it. Actors themselves are owned by the scene.
    std::vector<std::pair<PxRigidDynamic*, entt::entity>> dynamics;
    std::vector<std::pair<PxRigidDynamic*, entt::entity>> kinematics;
    // #168 - each dynamic body's pose before the most recent substep, so the rendered pose can
    // be interpolated between it and the current one. Rebuilt every substep (never stale).
    RefilledMap<PxRigidDynamic*, PxTransform> prevPose;
    // A sleeping body's pose as last written to its TransformComponent (local Position/Rotation): while it sleeps
    // and the component still holds it, the writeback skips it (pose conversion and Euler hint, every frame,
    // for every settled body in the scene).
    struct SettledWrite { glm::vec3 Position; glm::quat Rotation; };
    std::unordered_map<PxRigidDynamic*, SettledWrite> settled;
    // #185 PR 5 — trigger state. triggerOverlaps: rigidbody pairs currently inside a trigger
    // (maintained by onTrigger). playerTriggers: trigger entities the Player capsule is inside
    // (maintained by a per-frame overlap query). triggerEvents: this frame's transitions,
    // rebuilt each Step and drained by GetTriggerEvents. enteredThisFrame: dedup so a pair that
    // fired Enter this frame doesn't also get a synthetic Stay.
    std::set<TriggerPair>       triggerOverlaps;
    std::set<std::uint32_t>     playerTriggers;
    std::vector<TriggerEvent>   triggerEvents;
    std::set<TriggerPair>       enteredThisFrame;
    // #185 PR 7 — one PxMaterial per distinct (friction, bounciness) rounded to 1/100; the
    // per-entity dynamic-body lookup for the force API; this frame's solid-contact events.
    std::map<long long, PxMaterial*>&                materialCache; // PhysicsCore's (#167)
    std::unordered_map<std::uint32_t, PxRigidDynamic*> bodyByEntity;
    std::vector<ContactEvent>                        contactEvents;
    // #185 PR 11 — joints built on Play-enter, released before the scene. jointOwner maps each
    // back to the entity that authored it, for a useful break message. staticByEntity lets a
    // joint's other end be a static collider (#185 hardening).
    std::vector<PxJoint*> joints;
    // Ragdolls by id: one reduced-coordinate articulation each (released with the scene, or by DestroyRagdoll).
    // Bodies: its links by part. Prev: each part's pose before the latest substep, so GetRagdollPart can interpolate like #168.
    // Drive: per part, the spherical joint to its parent (null for the root). Ignore: per part, the parts its shape doesn't
    // collide with (the filter's word3); Pending: pairs ignored only because they overlapped at build, let go once apart.
    // Attach: joints that hold something to a part (a gun in the hand), released with it. ExtraGravity: m/s^2 added downward.
    struct Ragdoll {
        PxArticulationReducedCoordinate* Art = nullptr;
        std::vector<PxArticulationLink*> Bodies;
        std::vector<PxTransform> Prev;
        std::vector<PxArticulationJointReducedCoordinate*> Drive;
        std::vector<PxU32> Ignore;
        std::vector<std::pair<int, int>> Pending;
        std::vector<PxJoint*> Attach;
        std::vector<float> DriveInertia; // per part: what its joint turns (the part and all that hangs off it, about the joint), kg m^2
        std::vector<PxTransform> Shape;  // per part: its capsule in the link's frame (the link's frame is its joint's: see CreateRagdoll)
        // per part: a drive target given against the pose the body was built in (the motor's) is the joint's rotation Pre * t * Post
        // (the joint's zero is the middle of its range, not the build pose: see CreateRagdoll)
        std::vector<PxQuat> TargetPre, TargetPost;
        PxMaterial* Mat = nullptr;
        PxMaterial* SlickMat = nullptr; // the parts with a FrictionScale (the hands): Mat's friction times SlickScale
        float SlickScale = 1.0f;
        float ExtraGravity = 0.0f;
        // The articulation's last sane state (joint positions and root pose, after a substep that left every part finite and
        // within speed): a blown-up solve (a body piled into others) is put back to it, still, instead of going on as NaN -
        // which spread to whatever it touched and slowed every later step to hundreds of milliseconds.
        PxArticulationCache* Good = nullptr;
        bool Blown = false; // (logged once)
        bool PassBodies = false; // its parts carry kPassBodies: it began in or on another body, and passes them till clear
        // A corpse at rest is frozen: its articulation taken out of the scene and its capsules left as one static actor where
        // they lay (same filters, materials and part tags). A body falling onto a pile used to wake every corpse in it and
        // solve them all at the ragdolls' 32 iterations - tens to hundreds of milliseconds a step as the bodies piled up.
        // A hit (RagdollImpulse), the Player walking into it or anything attached to it puts the articulation back.
        PxRigidStatic* Frozen = nullptr;
        float Rest = 0.0f; // seconds asleep (unfrozen)
    };
    std::vector<Ragdoll> ragdolls;
    std::vector<int> thawRequests; // frozen ragdolls the Player walked into (the Player's character hit handling): thawed before the next step
    // Per-bone hitboxes of living NPCs (see CreateNpcHitboxes): their kinematic actors, the capsule each shadows while
    // active. hitboxActors: every hitbox actor, for the query filters; hitboxedCapsules: the capsules currently shadowed.
    struct Hitboxes { std::vector<PxRigidDynamic*> Bodies; const PxRigidActor* Capsule = nullptr; bool Active = false; };
    std::vector<Hitboxes> hitboxes;
    std::unordered_set<const PxRigidActor*> hitboxActors;
    std::unordered_set<const PxRigidActor*> hitboxedCapsules;
    std::unordered_map<PxJoint*, std::uint32_t>       jointOwner;
    std::unordered_map<std::uint32_t, PxRigidStatic*> staticByEntity;
    // Dynamic bodies with at least one joint: solved with more iterations, and grabbed gently.
    std::unordered_set<const PxRigidActor*> jointed;
    // #182 - raw entity id -> OrderComponent value, captured when actors are built, so trigger /
    // contact / joint log lines (written from PhysX callbacks, no registry at hand) can name
    // entities by the stable id the Console links to.
    std::unordered_map<std::uint32_t, int> orderByEntity;
    // #166 / #201 - every entity BuildActors has handled (built or skipped), so SyncEntities
    // can spot ones that appeared or disappeared since.
    std::unordered_set<std::uint32_t> attempted;
    // #185 PR 12 / #167 — cooked meshes, cached in PhysicsCore for the whole session.
    std::unordered_map<std::string, PxConvexMesh*>&   convexCache;
    std::unordered_map<std::string, PxTriangleMesh*>& triangleCache;
    // #185 PR 10 — Player's kinematic ground (moving platform) + its last pose, for carry.
    PxRigidDynamic* playerGround = nullptr;
    PxVec3          playerGroundLastPos{0.0f};
    PxQuat          playerGroundLastRot{PxIdentity};
    bool            playerGroundHitThisMove = false;
    float           playerMoveDt = 0.0f;  // the step of the move in progress (MoveCharacter), for the push
    float           platformYawDeltaDeg = 0.0f; // this frame's platform spin, for Player yaw
    // #168 - each kinematic's pose as authored THIS frame (its kinematic target). The simulated
    // pose only advances on frames where a substep runs, so carrying the Player by it moved the
    // capsule 0 or 2 steps' worth per frame at high refresh rates (judder); the target moves
    // every frame, in step with what the renderer draws.
    RefilledMap<const PxRigidDynamic*, PxTransform> kinematicFramePose;
    // #185 hardening — every dynamic body's linear velocity captured just before simulate(),
    // so onContact (post-solve) can report the real closing speed of an impact.
    RefilledMap<const PxActor*, PxVec3> preStepVel;
    // #185 hardening — gravity-gun grab (physics-playground debug harness). The held body plus
    // the gravity flag / angular damping saved at grab time so ReleaseBody restores them.
    PxRigidDynamic* grabbed = nullptr;
    bool            grabSavedGravityDisabled = false;
    float           grabSavedAngularDamping  = 0.0f;
    // The hold point, its velocity and the hold orientation, set once per frame by UpdateGrab and
    // applied every substep (DriveGrab), the point advancing by its velocity between frames.
    bool            grabHasTarget = false;
    bool            grabHasRotation = false;
    PxVec3          grabTarget{0.0f};
    PxVec3          grabTargetVelocity{0.0f};
    PxQuat          grabTargetRotation{PxIdentity};
    // #185 debug tooling — per-session counters.
    int             lastSubsteps = 0;
    float           lastStepMillis = 0.0f;
    unsigned        frameIndex = 0;
    double          simClock = 0.0;
    float                stepAccumulator = 0.0f;
};

// Whole world lives or dies as one unit between Create()/Destroy(), so a single owning pointer
// (not a pile of file-scope globals) keeps the lifetime obvious and the teardown ordered.
PhysicsState* g_State = nullptr;
std::uint64_t g_SimulationGeneration=0;

constexpr float kDefaultFixedStep = 1.0f / 60.0f;
constexpr int   kMaxSubSteps      = 4;

// --- #185 debug tooling: settings + history that OUTLIVE a Play session -----------------
// The editor sets these from the Physics panel; they must survive Play->Stop->Play, so they
// can't live in PhysicsState.
unsigned g_DebugDrawFlags = 0;
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

PxQuat QuatToPx(const glm::quat& rotation) {
    const glm::quat q = NormalizeRotation(rotation);
    return PxQuat(q.x, q.y, q.z, q.w);
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

using PhysicsWorld::kBodyPartHitbox;
using PhysicsWorld::kBodyPartRagdoll;
// A body part's shape tag (see BodyPartKind): (kind << 16) | part index; untagged shapes hold nullptr.
void* BodyPartTag(unsigned kind, unsigned part) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>((kind << 16) | part));
}
unsigned BodyPartKindOf(const void* tag) { return (unsigned)(reinterpret_cast<std::uintptr_t>(tag) >> 16); }

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

// `only` (#166 / #201): build just these entities - ones spawned, re-activated or given a
// Collider while playing (see SyncEntities). Null = every active collider (Play-enter).
void BuildActors(PhysicsState& s, const World& world, const std::unordered_set<std::uint32_t>* only = nullptr) {
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
    for (auto it = s.triggerOverlaps.begin(); it != s.triggerOverlaps.end();)
        it = (it->first == id || it->second == id) ? s.triggerOverlaps.erase(it) : std::next(it);
    s.playerTriggers.erase(id);
    s.scene->removeActor(*actor);
    actor->release();
}

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

// --- Triggers (#185 PR 5) --------------------------------------------------------------

const char* EntityLabel(const PhysicsState& s, std::uint32_t e, char buf[24]) {
    if (e == kPlayerEntity) return "Player";
    auto it = s.orderByEntity.find(e);
    if (it != s.orderByEntity.end()) std::snprintf(buf, 24, "entity #%d", it->second); // #182
    else                             std::snprintf(buf, 24, "entity id %u", e);
    return buf;
}

bool g_LogEvents = false; // #169 - SetEventLogging

void PushTriggerEvent(PhysicsState& s, std::uint32_t kind, std::uint32_t trig, std::uint32_t other) {
    TriggerEvent ev;
    ev.Kind = kind; ev.Trigger = trig; ev.Other = other;
    s.triggerEvents.push_back(ev);
    if (g_LogEvents && (kind == TriggerEvent::Enter || kind == TriggerEvent::Exit)) {
        char a[24], b[24];
        Log::Info(std::string("Trigger ") + (kind == TriggerEvent::Enter ? "enter: " : "exit:  ") +
                  EntityLabel(s, other, a) + (kind == TriggerEvent::Enter ? " -> " : " <- ") +
                  EntityLabel(s, trig, b));
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
        const PxVec3* v = a ? s.preStepVel.Find(a) : nullptr;
        return v ? *v : PxVec3(0.0f);
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

        if (g_LogEvents && kind != ContactEvent::Stay && sumImpulse > 0.75f) {
            char la[24], lb[24];
            char imp[32]; std::snprintf(imp, sizeof(imp), " (impulse %.1f)", sumImpulse);
            Log::Info(std::string("Contact ") + (kind == ContactEvent::Enter ? "hit:  " : "end:  ") +
                      EntityLabel(*owner, a, la) + " <-> " + EntityLabel(*owner, b, lb) +
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
        char label[24];
        if (g_LogEvents) Log::Info(std::string("Joint broke on ") + EntityLabel(*owner, owE, label) + ".");
    }
}

// #185 PR 10 — the Player capsule hit something while moving. Push a dynamic body out of the
// way (scaled so light things fly and heavy things barely budge), and if the thing is a
// kinematic body underfoot, remember it so MoveCharacter can carry the Player along with it.
// The pose a moving platform is carried by: this frame's authored target (#168), else the
// simulated pose (a kinematic with no Transform-driven target).
static PxTransform PlatformPose(const PhysicsState& s, const PxRigidDynamic* body) {
    const PxTransform* p = s.kinematicFramePose.Find(body);
    return p ? *p : body->getGlobalPose();
}

// What a character's move does to what it runs into: a dynamic body (a crate, a corpse's part) is shoved along the move, a
// frozen corpse is thawed to be shoved from the next step, and a kinematic one stood on (a moving platform) carries the
// Player from then on. `n` is the touch's normal, `dir` the move's direction.
void HandleCharacterHit(PhysicsState& s, PxRigidActor* actor, const PxShape* shape, const PxVec3& n, const PxVec3& dir,
                        const PxVec3& pos) {
    if (!actor) return;
    // A corpse's part (an articulation link) is pushed like any other body.
    PxRigidBody* body = actor->is<PxRigidDynamic>() ? static_cast<PxRigidBody*>(actor->is<PxRigidDynamic>())
                                                    : static_cast<PxRigidBody*>(actor->is<PxArticulationLink>());
    // A frozen corpse (a static copy of its parts): thawed before the next step, and pushed from then on.
    if (!body && shape && BodyPartKindOf(shape->userData) == kBodyPartRagdoll) {
        const PxU32 id = shape->getQueryFilterData().word2;
        if (id > 0u) s.thawRequests.push_back((int)id - 1);
        return;
    }
    if (!body) return;
    const bool kin = (body->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC);

    if (kin) {
        PxRigidDynamic* platform = actor->is<PxRigidDynamic>();
        // (Not a soldier's hitbox: they are the rounds' to find, not ground - stood over a crawler, the Player rode his hitboxes,
        // and they were released under him when he died.)
        if (platform && s.hitboxActors.count(platform)) platform = nullptr;
        // Nor a soldier's capsule: nobody stands on anybody's head (it rode him around the room).
        if (platform && s.npcActors.count(platform)) platform = nullptr;
        if (platform && n.y > 0.4f) { // standing on top of a moving platform
            if (s.playerGround != platform) {
                s.playerGround = platform;
                const PxTransform gp = PlatformPose(s, platform);
                s.playerGroundLastPos = gp.p;
                s.playerGroundLastRot = gp.q;
            }
            s.playerGroundHitThisMove = true;
        }
        return; // don't shove a kinematic body around
    }

    // Push proportional to how hard we walked into it. Impulse scales with mass so the
    // resulting velocity change is ~constant regardless of body weight (a heavy crate still
    // budges, a light one doesn't rocket away). Strength is project-tunable, per 60 Hz move: the
    // player moves once per rendered frame, so the impulse scales with the move's step (at 144 fps
    // a crate was shoved 2.4x as hard as at 60).
    const float mass = body->getMass() > 0.0f ? body->getMass() : 1.0f;
    const float rate = s.playerMoveDt > 0.0f ? std::min(s.playerMoveDt * 60.0f, 6.0f) : 1.0f;
    const float strength = ProjectSettings::Physics().PlayerPushStrength * rate;
    PxRigidBodyExt::addForceAtPos(*body, dir * (strength * mass), pos, PxForceMode::eIMPULSE, true);
}

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
#ifdef TARTARUS_PHYSX_PVD
        if (c->pvd) c->pvd->release();
        if (c->pvdTransport) c->pvdTransport->release();
#endif
#ifdef TARTARUS_PHYSX_OMNIPVD
        if (c->omniPvd) c->omniPvd->release();
#endif
        c->foundation->release();
        delete c;
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

    // Reverse construction order. scene->release() drops every actor/shape it owns. The core
    // (physics, dispatcher, materials, cooked meshes) stays up for the next Play (#167).
    for (PxJoint* j : s->joints)               if (j) j->release(); // #185 PR 11 — before the scene
    for (auto& r : s->ragdolls) { if (r.Frozen && r.Art) { r.Art->release(); r.Art = nullptr; } if (r.Good) { r.Good->release(); r.Good = nullptr; } for (PxJoint* j : r.Attach) if (j) j->release(); if (r.Mat) { r.Mat->release(); r.Mat = nullptr; } if (r.SlickMat) { r.SlickMat->release(); r.SlickMat = nullptr; } } // (the articulations go with the scene)
    if (s->scene)           s->scene->release();

    delete s;
    Log::Info("PhysX world destroyed.");
}

void Shutdown() {
    Destroy();
    if (!g_Core) return;
    PhysicsCore* c = g_Core;
    g_Core = nullptr;
    for (auto& kv : c->materialCache)          if (kv.second) kv.second->release(); // #185 PR 7
    for (auto& kv : c->convexCache)            if (kv.second) kv.second->release(); // #185 PR 6/12
    for (auto& kv : c->triangleCache)          if (kv.second) kv.second->release();
    if (c->defaultMaterial) c->defaultMaterial->release();
    if (c->dispatcher)      c->dispatcher->release();
    if (c->physics)         c->physics->release();
#ifdef TARTARUS_PHYSX_PVD
    if (c->pvd) {
        c->pvd->disconnect();
        c->pvd->release();
    }
    if (c->pvdTransport) c->pvdTransport->release();
#endif
#ifdef TARTARUS_PHYSX_OMNIPVD
    if (c->omniPvd) c->omniPvd->release(); // after physics->release()
#endif
    if (c->foundation) c->foundation->release();
    delete c;
}

bool IsActive() {
    return g_State != nullptr;
}

// Trigger entities whose volume the Player capsule currently overlaps. One overlap query per
// Step — cheap for the handful of trigger shapes a scene has.
void CollectPlayerTriggers(std::set<std::uint32_t>& out) {
    out.clear();
    if (!g_State->playerBody) return;
    const auto* b = g_State->playerBody.get();
    const PxCapsuleGeometry geom(b->Cap.Radius, b->Cap.HalfHeight);
    const PxVec3 centre = ToPx(b->Feet + glm::vec3(0.0f, b->Cap.Radius + b->Cap.HalfHeight, 0.0f));
    const PxTransform pose(centre, PxQuat(PxHalfPi, PxVec3(0, 0, 1))); // capsule axis X -> up

    // #191 — the touch buffer grows until PhysX reports every overlap (it used to be a fixed 16,
    // silently dropping Enter / Stay / Exit for any trigger past the 16th).
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::eNO_BLOCK);
    static std::vector<PxOverlapHit> hits(64);
    for (;;) {
        PxOverlapBuffer buf(hits.data(), (PxU32)hits.size());
        if (!g_State->scene->overlap(geom, pose, buf, fd)) return;
        if (buf.getNbTouches() >= (PxU32)hits.size() && hits.size() < 4096) {
            hits.resize(hits.size() * 2); // possibly truncated — retry with room to spare
            continue;
        }
        for (PxU32 i = 0; i < buf.getNbTouches(); ++i) {
            const PxShape* sh = buf.getTouch(i).shape;
            if (sh && (sh->getFlags() & PxShapeFlag::eTRIGGER_SHAPE) && buf.getTouch(i).actor)
                out.insert(UserDataToEntity(buf.getTouch(i).actor->userData));
        }
        return;
    }
}

// The gravity gun's servo, run before every substep: the body's velocity becomes the hold
// point's own velocity (feed-forward: walking or turning with it doesn't leave it trailing) plus
// a correction closing the remaining gap with a ~1/12 s time constant; its angular velocity
// turns it toward the hold orientation the same way.
void DriveGrab(PhysicsState& s, float dt) {
    PxRigidDynamic* b = s.grabbed;
    if (!b || !s.grabHasTarget) return;
    const PxTransform pose = b->getGlobalPose();
    const PxVec3 err = s.grabTarget - pose.p;
    s.grabTarget += s.grabTargetVelocity * dt; // where the hold point will be by the next substep
    if (s.jointed.count(b)) {
        // A body on joints: overwriting its velocity every step fights the joints with infinite
        // force, and the chain snaps and whips (worse on release). Ease toward the pull instead,
        // with a lower speed cap, and let the joints keep its spin.
        PxVec3 v = err * 12.0f;
        const float jointedMax = 8.0f;
        if (v.magnitude() > jointedMax) v = v.getNormalized() * jointedMax;
        b->setLinearVelocity(b->getLinearVelocity() + (v - b->getLinearVelocity()) * 0.25f);
        return;
    }
    PxVec3 v = s.grabTargetVelocity + err * 12.0f;
    const float maxSpeed = 30.0f; // cap so a distant grab doesn't tunnel through geometry
    if (v.magnitude() > maxSpeed) v = v.getNormalized() * maxSpeed;
    b->setLinearVelocity(v);
    PxVec3 w(0.0f);
    if (s.grabHasRotation) {
        PxQuat d = s.grabTargetRotation * pose.q.getConjugate();
        if (d.w < 0.0f) d = PxQuat(-d.x, -d.y, -d.z, -d.w); // the short way round
        float angle = 0.0f;
        PxVec3 axis(1.0f, 0.0f, 0.0f);
        d.toRadiansAndUnitAxis(angle, axis);
        if (angle > 1e-4f && axis.isFinite()) w = axis * (angle * 10.0f);
        const float maxSpin = 20.0f; // rad/s
        if (w.magnitude() > maxSpin) w = w.getNormalized() * maxSpin;
    }
    b->setAngularVelocity(w);
}

void RagdollBeforeSubstep(PhysicsState& s, PhysicsState::Ragdoll& rd); // (with the ragdolls, below)
void RagdollAfterSubstep(PhysicsState::Ragdoll& rd);
void FreezeRestingRagdolls(PhysicsState& s, float dt);

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

// #185 debug — reject a NaN/inf simulated pose instead of poisoning the TransformComponent
// (which then spreads through the world-transform cache and the renderer). Freezes the body.
bool PoseIsFinite(const PxTransform& p) {
    auto ok = [](float f) { return std::isfinite(f); };
    return ok(p.p.x) && ok(p.p.y) && ok(p.p.z) &&
           ok(p.q.x) && ok(p.q.y) && ok(p.q.z) && ok(p.q.w);
}

namespace {std::uint64_t g_EventSequence=0;}
std::uint64_t EventSequence() {return g_EventSequence;}
std::uint64_t SimulationGeneration() {return g_SimulationGeneration;}
void Step(float dt, World& world, const std::function<void(float fixedDt)>& onFixedStep) {
    ++g_EventSequence;
    if (!g_State || !g_State->scene) return;
    // dt arrives time-scaled (Time::DeltaTime); the debug-draw history fades in real time.
    const float rawDt = Time::UnscaledDeltaTime();
    g_State->triggerEvents.clear();
    g_State->enteredThisFrame.clear();
    g_State->contactEvents.clear(); // #185 PR 7 — onContact refills it during the sim below
    // Frozen (time scale 0): no sim, so no events this frame - but last frame's are cleared above
    // rather than replayed to the game module every frame. The debug-draw history keeps its age,
    // so contact sparks stay visible to inspect while frozen.
    SyncEntities(*g_State, world); // #166 / #201 - even while frozen, so a deleted object stops blocking
    if (dt <= 0.0f) { g_State->lastSubsteps = 0; g_State->lastStepMillis = 0.0f; return; }

    // Age the debug-draw history (contacts / queries) so sparks fade over ~½ second.
    for (int i = 0; i < g_ContactCount; ++i) g_Contacts[i].age += rawDt;
    for (int i = 0; i < g_QueryCount; ++i)   g_Queries[i].age  += rawDt;

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
        RunOneSubstep(*g_State, fixedStep);
        g_State->stepAccumulator -= fixedStep;
        ++steps;
    }
    if (steps) FreezeRestingRagdolls(*g_State, fixedStep * (float)steps);
    g_State->lastSubsteps = steps;
    g_State->lastStepMillis = steps ? std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - t0).count() : 0.0f;

    // Write each force-driven body's simulated pose back to its TransformComponent so the
    // renderer, gizmos and world-transform cache all see it move. Play -> Stop reloads the
    // authored scene, so this is never persisted.
    // #168 - `alpha` is how far this frame sits between the last substep and the next one; the
    // written pose is blended (or predicted) by it per the body's Interpolate mode, so motion
    // stays smooth when frames outnumber physics steps.
    const float alpha = std::clamp(g_State->stepAccumulator / fixedStep, 0.0f, 1.0f);
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
        if (!PoseIsFinite(p)) { // #185 — blow-up guard: don't poison the TransformComponent
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
        const int mode = body == g_State->grabbed ? 2 : (rb ? rb->Interpolation : 1);
        if (mode == 1) {
            if (const PxTransform* prev = g_State->prevPose.Find(body); prev && PoseIsFinite(*prev)) {
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

// ScopedQueryPolicy's stack (see the header). Empty = the default: skip the Player, hit NPCs.
struct QueryPolicy { std::uint32_t ignoreEntity = 0xFFFFFFFFu; bool hitPlayer = false; bool hitCharacters = true; };
thread_local std::vector<QueryPolicy> t_QueryPolicy;

const PxRigidActor* PlayerActor();

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
    const PxRigidActor* skip = nullptr;
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape*, const PxRigidActor* actor,
                                   PxHitFlags&) override {
        if (actor && actor == skip) return PxQueryHitType::eNONE;
        return PolicyRejects(actor) ? PxQueryHitType::eNONE : PxQueryHitType::eBLOCK;
    }
    PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*,
                                    const PxRigidActor*) override {
        return PxQueryHitType::eBLOCK;
    }
};
const PxRigidActor* PlayerActor() {
    if (!g_State) return nullptr;
    return g_State->playerBody ? g_State->playerBody->Actor : nullptr;
}

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

// --- Player character ---------------------------------------------------------------

bool HasCharacter() {
    return g_State && g_State->playerBody;
}

namespace {
// --- The custom character mover ------------------------------------------------------------
using CharacterBody = PhysicsState::CharacterBody;
const PxQuat kCapsuleUp(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f)); // PhysX capsules lie along X

MoverSettings ProjectMoverSettings() {
    const auto& p = ProjectSettings::Physics();
    MoverSettings m;
    m.Skin = p.CharacterSkinWidth;
    m.StepHeight = p.CharacterStepHeight;
    m.WalkableCos = std::cos(glm::radians(p.CharacterWalkableSlopeDeg));
    m.StepDownSnap = p.CharacterStepDownSnap;
    m.LedgeTolerance = p.CharacterLedgeTolerance;
    return m;
}

PxVec3 BodyCentre(const CharacterBody& b) {
    return ToPx(b.Feet + glm::vec3(0.0f, b.Cap.Radius + b.Cap.HalfHeight, 0.0f));
}

PxShape* MakeBodyShape(PhysicsState& s, PxRigidDynamic& a, const MoverCapsule& cap, const PxFilterData& sim) {
    PxShape* sh = PxRigidActorExt::createExclusiveShape(a, PxCapsuleGeometry(cap.Radius, cap.HalfHeight), *s.defaultMaterial);
    sh->setLocalPose(PxTransform(kCapsuleUp));
    sh->setSimulationFilterData(sim);
    return sh;
}

// A kinematic capsule in the scene, standing at `feet`: what rays, sight lines, ragdolls and bodies meet of the character.
std::unique_ptr<CharacterBody> MakeBody(PhysicsState& s, const MoverCapsule& cap, const glm::vec3& feet, std::uint32_t entity,
                                        const PxFilterData& sim) {
    auto b = std::make_unique<CharacterBody>();
    b->Cap = cap;
    b->Feet = feet;
    b->Actor = s.physics->createRigidDynamic(PxTransform(BodyCentre(*b)));
    b->Actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
    b->Shape = MakeBodyShape(s, *b->Actor, cap, sim);
    b->Actor->userData = reinterpret_cast<void*>(static_cast<std::uintptr_t>(entity));
    s.scene->addActor(*b->Actor);
    return b;
}

// A new cylinder length (crouch / stand), the feet where they were; the shape's filter data carries over.
void ResizeBody(PhysicsState& s, CharacterBody& b, float cylinderHalfHeight) {
    const PxFilterData sim = b.Shape->getSimulationFilterData();
    b.Actor->detachShape(*b.Shape);
    b.Cap.HalfHeight = cylinderHalfHeight;
    b.Shape = MakeBodyShape(s, *b.Actor, b.Cap, sim);
}

// The mover's view of the PhysX scene for one character: its collision-matrix row, never itself or a trigger.
struct PhysxMoverWorld : IMoverWorld {
    PhysicsState& S;
    const PxRigidActor* Self = nullptr;
    PxU32 Mask = 0xFFFFFFFFu;
    // A soldier's: everything of his own entity (his hitboxes), and the dead while he crawls over them.
    std::uint32_t SelfEntity = 0xFFFFFFFFu;
    bool PassRagdolls = false;
    PhysxMoverWorld(PhysicsState& s, const PxRigidActor* self, PxU32 mask) : S(s), Self(self), Mask(mask) {}

    struct Filter : PxQueryFilterCallback {
        const PhysxMoverWorld* W = nullptr;
        PxVec3 Dir{0.0f};
        bool Touch = false; // overlaps: every one; sweeps: the first
        PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor* actor, PxHitFlags&) override {
            if (actor == W->Self || (shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE)) return PxQueryHitType::eNONE;
            if (W->SelfEntity != 0xFFFFFFFFu && actor && UserDataToEntity(actor->userData) == W->SelfEntity) return PxQueryHitType::eNONE;
            if (W->PassRagdolls && IsRagdollPart(shape->getSimulationFilterData())) return PxQueryHitType::eNONE;
            const PxU32 layer = shape->getSimulationFilterData().word1 & 31u;
            if (!((W->Mask >> layer) & 1u)) return PxQueryHitType::eNONE;
            return Touch ? PxQueryHitType::eTOUCH : PxQueryHitType::eBLOCK;
        }
        // Something already overlapped only stops a move that goes deeper into it.
        PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit& hit, const PxShape*, const PxRigidActor*) override {
            const auto& h = static_cast<const PxSweepHit&>(hit);
            if (h.hadInitialOverlap() && h.normal.dot(Dir) >= -1e-3f) return PxQueryHitType::eNONE;
            return PxQueryHitType::eBLOCK;
        }
    };
    bool IsCharacter(const PxRigidActor* a) const {
        return a && (S.npcActors.count(a) || S.hitboxActors.count(a) ||
                     (S.playerBody && a == S.playerBody->Actor));
    }
    static PxTransform Pose(const MoverCapsule& cap, const glm::vec3& feet) {
        return PxTransform(ToPx(feet + glm::vec3(0.0f, cap.Radius + cap.HalfHeight, 0.0f)), kCapsuleUp);
    }
    bool Sweep(const MoverCapsule& cap, const glm::vec3& feet, const glm::vec3& dir, float dist, MoverHit& out) override {
        Filter f;
        f.W = this;
        f.Dir = ToPx(dir);
        const PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER | PxQueryFlag::ePOSTFILTER);
        PxSweepBuffer buf;
        if (dist <= 0.0f || !S.scene->sweep(PxCapsuleGeometry(cap.Radius, cap.HalfHeight), Pose(cap, feet), f.Dir, dist, buf,
                                            PxHitFlag::eDEFAULT | PxHitFlag::eMTD, fd, &f) || !buf.hasBlock)
            return false;
        const PxSweepHit& h = buf.block;
        out = MoverHit{};
        out.Distance = h.hadInitialOverlap() ? 0.0f : std::max(0.0f, h.distance);
        out.Point = glm::vec3(h.position.x, h.position.y, h.position.z);
        out.Normal = glm::vec3(h.normal.x, h.normal.y, h.normal.z);
        out.Actor = h.actor;
        out.Shape = h.shape;
        out.Character = IsCharacter(h.actor);
        return true;
    }
    bool Raycast(const glm::vec3& from, const glm::vec3& dir, float dist, MoverHit& out) override {
        Filter f;
        f.W = this;
        const PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER);
        PxRaycastBuffer buf;
        if (!S.scene->raycast(ToPx(from), ToPx(dir), dist, buf, PxHitFlag::eDEFAULT, fd, &f) || !buf.hasBlock) return false;
        const PxRaycastHit& h = buf.block;
        out = MoverHit{};
        out.Distance = h.distance;
        out.Point = glm::vec3(h.position.x, h.position.y, h.position.z);
        out.Normal = glm::vec3(h.normal.x, h.normal.y, h.normal.z);
        out.Actor = h.actor;
        out.Shape = h.shape;
        out.Character = IsCharacter(h.actor);
        return true;
    }
    bool Penetration(const MoverCapsule& cap, const glm::vec3& feet, glm::vec3& outPush) override {
        Filter f;
        f.W = this;
        f.Touch = true;
        const PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER | PxQueryFlag::eNO_BLOCK);
        const PxCapsuleGeometry geom(cap.Radius, cap.HalfHeight);
        const PxTransform pose = Pose(cap, feet);
        PxOverlapHit hits[16];
        PxOverlapBuffer buf(hits, 16);
        if (!S.scene->overlap(geom, pose, buf, fd, &f)) return false;
        float deepest = 0.0f;
        PxVec3 way(0.0f);
        for (PxU32 i = 0; i < buf.getNbTouches(); ++i) {
            const PxOverlapHit& o = buf.getTouch(i);
            PxVec3 dir;
            PxF32 depth = 0.0f;
            if (PxGeometryQuery::computePenetration(dir, depth, geom, pose, o.shape->getGeometry(),
                                                    PxShapeExt::getGlobalPose(*o.shape, *o.actor)) &&
                depth > deepest) {
                deepest = depth;
                way = dir;
            }
        }
        if (deepest <= 1e-4f) return false;
        outPush = glm::vec3(way.x, way.y, way.z) * (deepest + 0.001f);
        return true;
    }
};
} // namespace

void CreateCharacter(float radius, float cylinderHalfHeight, const float footPos[3]) {
    if (!g_State || HasCharacter()) return;
    if (radius <= 0.0f || cylinderHalfHeight <= 0.0f) return;
    const PxU32 pl = (PxU32)std::clamp(ProjectSettings::Physics().PlayerLayer, 0, 31);
    g_State->playerBody = MakeBody(*g_State, MoverCapsule{radius, cylinderHalfHeight}, glm::vec3(footPos[0], footPos[1], footPos[2]),
                                   (std::uint32_t)kPlayerEntity, PxFilterData(1u << pl, pl, 0u, 0u));
}

void SetCharacterFootPosition(const float footPos[3]) {
    if (!HasCharacter()) return;
    if (CharacterBody* b = g_State->playerBody.get()) {
        b->Feet = glm::vec3(footPos[0], footPos[1], footPos[2]);
        b->Actor->setGlobalPose(PxTransform(BodyCentre(*b)));
    }
}

void GetCharacterFootPosition(float outFootPos[3]) {
    if (!HasCharacter()) return;
    if (const CharacterBody* b = g_State->playerBody.get()) {
        outFootPos[0] = b->Feet.x; outFootPos[1] = b->Feet.y; outFootPos[2] = b->Feet.z;
    }
}

bool GetCharacterCapsule(float outFootPos[3], float* outRadius, float* outCylHalfHeight) {
    if (!HasCharacter()) return false;
    if (const CharacterBody* b = g_State->playerBody.get()) {
        outFootPos[0] = b->Feet.x; outFootPos[1] = b->Feet.y; outFootPos[2] = b->Feet.z;
        if (outRadius)        *outRadius = b->Cap.Radius;
        if (outCylHalfHeight) *outCylHalfHeight = b->Cap.HalfHeight;
        return true;
    }
    return false;
}

bool LastMoverResult(float outVelocity[3], bool* outGrounded, float* outStepDelta) {
    const CharacterBody* b = g_State ? g_State->playerBody.get() : nullptr;
    if (!b) return false;
    outVelocity[0] = b->Last.Velocity.x; outVelocity[1] = b->Last.Velocity.y; outVelocity[2] = b->Last.Velocity.z;
    if (outGrounded) *outGrounded = b->Last.Grounded;
    if (outStepDelta) *outStepDelta = b->Last.StepDelta;
    return true;
}

void SetCharacterDoorwayAssist(float strength, float maxAngleDeg) {
    if (!g_State) return;
    g_State->playerDoorAssist = std::clamp(strength, 0.0f, 1.0f);
    g_State->playerDoorAssistMaxAngle = std::clamp(maxAngleDeg, 0.0f, 89.0f);
}

unsigned MoveCharacter(const float disp[3], float dt, bool snapToGround) {
    if (!HasCharacter() || dt <= 0.0f) return 0;

    PxVec3 d(disp[0], disp[1], disp[2]);
    // #185 PR 10 — ride a moving platform: add the ground kinematic's translation since the
    // last move, and expose its yaw change for Player::Update to fold into the camera.
    g_State->playerGroundHitThisMove = false;
    g_State->playerMoveDt = dt;
    g_State->platformYawDeltaDeg = 0.0f;
    if (g_State->playerGround) {
        const PxTransform gp = PlatformPose(*g_State, g_State->playerGround);
        d += gp.p - g_State->playerGroundLastPos;
        const PxQuat dq = gp.q * g_State->playerGroundLastRot.getConjugate();
        const float yaw = std::atan2(2.0f * (dq.w * dq.y + dq.x * dq.z),
                                     1.0f - 2.0f * (dq.y * dq.y + dq.z * dq.z));
        g_State->platformYawDeltaDeg = glm::degrees(yaw);
        g_State->playerGroundLastPos = gp.p;
        g_State->playerGroundLastRot = gp.q;
    }

    // #169 - the collision matrix row of the Player's layer filters what the mover sweeps against.
    const int playerLayer = std::clamp(ProjectSettings::Physics().PlayerLayer, 0, 31);
    const PxU32 layerMask = ProjectSettings::Physics().LayerCollisionMask[playerLayer];

    if (CharacterBody* b = g_State->playerBody.get()) {
        PhysxMoverWorld w(*g_State, b->Actor, layerMask);
        const glm::vec3 delta(d.x, d.y, d.z), velocity = glm::vec3(disp[0], disp[1], disp[2]) / dt;
        const bool debug = (g_DebugDrawFlags & PDD_Movers) != 0u;
        MoverSettings ms = ProjectMoverSettings();
        ms.DoorAssist = g_State->playerDoorAssist;
        ms.DoorAssistMaxAngle = g_State->playerDoorAssistMaxAngle;
        b->Last = CharacterMover::Move(w, b->Cap, b->Feet, velocity, delta, ms, b->Grounded, snapToGround,
                                       debug ? &b->Debug : nullptr);
        if (!debug) b->Debug.Clear();
        b->Feet = b->Last.Feet;
        b->Grounded = b->Last.Grounded;
        b->Actor->setKinematicTarget(PxTransform(BodyCentre(*b)));
        // What it ran into: bodies shoved, a platform underfoot carried on.
        const float dl = glm::length(delta);
        const PxVec3 dir = dl > 1e-6f ? ToPx(delta / dl) : PxVec3(0.0f, -1.0f, 0.0f);
        for (int i = 0; i < b->Last.HitCount; ++i) {
            const MoverHit& h = b->Last.Hits[i];
            HandleCharacterHit(*g_State, static_cast<PxRigidActor*>(const_cast<void*>(h.Actor)), static_cast<const PxShape*>(h.Shape),
                               ToPx(h.Normal), dir, ToPx(h.Point));
        }
        const MoverFloor& floor = b->Last.Floor;
        if (floor.Found && floor.Walkable && floor.Hit.Actor) {
            auto* a = static_cast<PxRigidActor*>(const_cast<void*>(floor.Hit.Actor));
            if (a->is<PxRigidDynamic>() && (a->is<PxRigidDynamic>()->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC))
                HandleCharacterHit(*g_State, a, static_cast<const PxShape*>(floor.Hit.Shape), ToPx(floor.Hit.Normal),
                                   PxVec3(0.0f, -1.0f, 0.0f), ToPx(floor.Hit.Point));
        }
        const unsigned out = b->Last.Flags;
        if (!g_State->playerGroundHitThisMove || !(out & CC_DOWN)) g_State->playerGround = nullptr;
        return out;
    }
    return 0;
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
// #170 — filtered queries: layer mask + trigger interaction, RaycastAll, box / capsule shapes
// ==================================================================================

namespace {

// Rejects the Player capsule, trigger shapes (unless asked for) and shapes whose layer bit
// (simulation filter word0, stamped at build time) isn't in the mask. `touchMode` turns every
// accepted hit into a touch, for the "all hits" queries.
struct LayerQueryFilter : PxQueryFilterCallback {
    const PxRigidActor* skip = nullptr;
    PxU32 mask = 0xFFFFFFFFu;
    bool triggers = true;
    bool touchMode = false;
    bool skipSimulated = false; // ignore simulated (non-kinematic) dynamic bodies
    bool skipStartInside = false; // ignore a shape the ray starts inside (PhysX reports it at distance 0, normal -dir)
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor* actor,
                                   PxHitFlags&) override {
        if (actor && actor == skip) return PxQueryHitType::eNONE;
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

int OverlapFiltered(const PxGeometry& geom, const PxTransform& pose, const QueryFilter& f,
                    unsigned* out, int maxEntities) {
    if (!g_State || !g_State->scene) return 0;
    PxOverlapHit hits[256];
    PxOverlapBuffer buf(hits, 256);
    LayerQueryFilter lf = MakeFilter(f, /*touchMode=*/true);
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER |
                         PxQueryFlag::eNO_BLOCK);
    if (!g_State->scene->overlap(geom, pose, buf, fd, &lf)) return 0;
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

} // namespace

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
    PxRaycastHit hits[256];
    PxRaycastBuffer buf(hits, 256);
    LayerQueryFilter lf = MakeFilter(f, /*touchMode=*/true);
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER |
                         PxQueryFlag::eNO_BLOCK);
    g_State->scene->raycast(PxVec3(origin[0], origin[1], origin[2]), d, maxDistance, buf,
                            PxHitFlag::eDEFAULT, fd, &lf);
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

bool ResizeCharacter(float cylinderHalfHeight) {
    if (!HasCharacter() || cylinderHalfHeight <= 0.0f) return false;
    if (CharacterBody* b = g_State->playerBody.get()) {
        ResizeBody(*g_State, *b, cylinderHalfHeight);
        b->Actor->setGlobalPose(PxTransform(BodyCentre(*b))); // the feet stay put
        return true;
    }
    return false;
}

bool CharacterFitsAt(float cylinderHalfHeight) {
    if (!HasCharacter() || cylinderHalfHeight <= 0.0f) return false;
    const CharacterBody* b = g_State->playerBody.get();
    const glm::vec3 foot = b->Feet;
    const float capRadius = b->Cap.Radius;
    // A hair smaller and lifted, so the floor under the feet and a wall alongside don't count.
    const float radius = std::max(0.02f, capRadius - 0.03f);
    const PxVec3 centre(foot.x, foot.y + 0.04f + cylinderHalfHeight + radius, foot.z);
    const PxQuat lieAlongY(PxPi * 0.5f, PxVec3(0.0f, 0.0f, 1.0f)); // PhysX capsules lie along X
    QueryFilter f;
    f.HitTriggers = 0;
    return OverlapFiltered(PxCapsuleGeometry(radius, cylinderHalfHeight), PxTransform(centre, lieAlongY), f, nullptr, 0) == 0;
}

// --- Ragdolls --------------------------------------------------------------------------------
// One reduced-coordinate articulation per body: the joints are exact (a limb can't drift off its parent or stretch however hard
// it is hit, and light hands on heavy forearms don't whip through their limits), each joint is a spherical one whose three
// axes (twist about the joint frame's X, swing about its Y and Z) are limited and driven, and the parts collide with each other
// except where the filter says not (see EngineFilterShader).

namespace {

constexpr float kPairClearance = 0.01f; // a pair closer than this (m) at build is let through until it has separated by as much

// The closest distance between segments [p0, p1] and [q0, q1] (Ericson, Real-Time Collision Detection 5.1.9).
float SegmentDistance(const PxVec3& p0, const PxVec3& p1, const PxVec3& q0, const PxVec3& q1) {
    const PxVec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const float a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(r);
    float s = 0.0f, t = 0.0f;
    if (a <= 1e-8f && e <= 1e-8f) return r.magnitude();
    if (a <= 1e-8f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = d1.dot(r);
        if (e <= 1e-8f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = d1.dot(d2), denom = a * e - b * b;
            s = denom > 1e-8f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = std::clamp(-c / a, 0.0f, 1.0f); }
            else if (t > 1.0f) { t = 1.0f; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
        }
    }
    return ((p0 + d1 * s) - (q0 + d2 * t)).magnitude();
}

// The gap between two capsules along local +X (negative: they overlap by that much).
float CapsuleGap(const PxTransform& a, float ha, float ra, const PxTransform& b, float hb, float rb) {
    const PxVec3 xa = a.q.getBasisVector0() * ha, xb = b.q.getBasisVector0() * hb;
    return SegmentDistance(a.p - xa, a.p + xa, b.p - xb, b.p + xb) - ra - rb;
}

PxShape* LinkShape(PxArticulationLink* link) {
    PxShape* sh = nullptr;
    if (link && link->getNbShapes() > 0) link->getShapes(&sh, 1);
    return sh;
}

float LinkGap(PxArticulationLink* a, PxArticulationLink* b) {
    PxShape* sa = LinkShape(a);
    PxShape* sb = LinkShape(b);
    PxCapsuleGeometry ga, gb;
    if (!sa || !sb || sa->getGeometry().getType() != PxGeometryType::eCAPSULE || sb->getGeometry().getType() != PxGeometryType::eCAPSULE)
        return 1e9f;
    ga = static_cast<const PxCapsuleGeometry&>(sa->getGeometry());
    gb = static_cast<const PxCapsuleGeometry&>(sb->getGeometry());
    return CapsuleGap(a->getGlobalPose() * sa->getLocalPose(), ga.halfHeight, ga.radius, b->getGlobalPose() * sb->getLocalPose(), gb.halfHeight, gb.radius);
}

// Part `part`'s ignore mask into its shape's filter, and the pairs it is in re-filtered.
void ApplyPartFilter(PhysicsState& s, PhysicsState::Ragdoll& rd, int part) {
    PxArticulationLink* link = rd.Bodies[(size_t)part];
    PxShape* sh = LinkShape(link);
    if (!sh) return;
    PxFilterData f = sh->getSimulationFilterData();
    f.word3 = rd.Ignore[(size_t)part];
    sh->setSimulationFilterData(f);
    if (link->getScene()) s.scene->resetFiltering(*link);
}

// Whether any of `rd`'s capsules is in (or within `margin` of) another ragdoll's - moving or frozen.
bool TouchesOtherBody(const PhysicsState& s, const PhysicsState::Ragdoll& rd, float margin) {
    auto capsule = [](const PxShape* sh, const PxTransform& actor, PxTransform& pose, PxCapsuleGeometry& g) {
        if (!sh || sh->getGeometry().getType() != PxGeometryType::eCAPSULE) return false;
        g = static_cast<const PxCapsuleGeometry&>(sh->getGeometry());
        pose = actor * sh->getLocalPose();
        return true;
    };
    for (PxArticulationLink* a : rd.Bodies) {
        PxTransform pa;
        PxCapsuleGeometry ga;
        if (!a || !capsule(LinkShape(a), a->getGlobalPose(), pa, ga)) continue;
        for (const PhysicsState::Ragdoll& o : s.ragdolls) {
            if (&o == &rd || !o.Art) continue;
            PxTransform pb;
            PxCapsuleGeometry gb;
            if (o.Frozen) {
                PxShape* shapes[32];
                const PxU32 n = o.Frozen->getShapes(shapes, 32);
                for (PxU32 k = 0; k < n; ++k)
                    if (capsule(shapes[k], o.Frozen->getGlobalPose(), pb, gb) && CapsuleGap(pa, ga.halfHeight, ga.radius, pb, gb.halfHeight, gb.radius) < margin)
                        return true;
            } else {
                for (PxArticulationLink* b : o.Bodies)
                    if (b && capsule(LinkShape(b), b->getGlobalPose(), pb, gb) && CapsuleGap(pa, ga.halfHeight, ga.radius, pb, gb.halfHeight, gb.radius) < margin)
                        return true;
            }
        }
    }
    return false;
}

// Every part of `rd` passing the other bodies, or colliding with them again.
void SetPassBodies(PhysicsState& s, PhysicsState::Ragdoll& rd, bool on) {
    if (rd.PassBodies == on) return;
    rd.PassBodies = on;
    for (PxArticulationLink* link : rd.Bodies) {
        PxShape* sh = LinkShape(link);
        if (!sh) continue;
        PxFilterData f = sh->getSimulationFilterData();
        f.word1 = on ? (f.word1 | kPassBodies) : (f.word1 & ~kPassBodies);
        sh->setSimulationFilterData(f);
        if (link->getScene()) s.scene->resetFiltering(*link);
    }
}

// The articulation back in the scene, asleep where it lay (a hit wakes it); the static copy gone.
void ThawRagdoll(PhysicsState& s, PhysicsState::Ragdoll& rd) {
    rd.Rest = 0.0f;
    if (!rd.Frozen) return;
    s.scene->removeActor(*rd.Frozen);
    rd.Frozen->release();
    rd.Frozen = nullptr;
    if (rd.Art && !rd.Art->getScene()) {
        s.scene->addArticulation(*rd.Art);
        rd.Art->putToSleep();
    }
}

void FreezeRagdoll(PhysicsState& s, PhysicsState::Ragdoll& rd) {
    if (rd.Frozen || !rd.Art || !rd.Art->getScene()) return;
    PxRigidStatic* copy = s.physics->createRigidStatic(PxTransform(PxIdentity));
    if (!copy) return;
    for (PxArticulationLink* l : rd.Bodies) {
        if (!l) continue;
        const PxTransform pose = l->getGlobalPose();
        if (!PoseIsFinite(pose)) { copy->release(); return; }
        PxShape* shapes[4];
        const PxU32 n = l->getShapes(shapes, 4);
        for (PxU32 k = 0; k < n; ++k) {
            const PxShape* src = shapes[k];
            PxMaterial* mats[4];
            const PxU16 nm = (PxU16)src->getMaterials(mats, 4);
            PxShape* c = s.physics->createShape(src->getGeometry(), mats, nm, true, src->getFlags());
            if (!c) continue;
            c->setLocalPose(pose * src->getLocalPose());
            c->setSimulationFilterData(src->getSimulationFilterData());
            c->setQueryFilterData(src->getQueryFilterData());
            c->setContactOffset(src->getContactOffset());
            c->setRestOffset(src->getRestOffset());
            c->userData = src->userData; // the part tag (RaycastBodyParts)
            copy->attachShape(*c);
            c->release();
        }
    }
    copy->userData = rd.Bodies.empty() || !rd.Bodies[0] ? nullptr : rd.Bodies[0]->userData; // the entity
    s.scene->removeArticulation(*rd.Art);
    s.scene->addActor(*copy);
    rd.Frozen = copy;
}

} // namespace

// Once a frame: corpses asleep a while freeze; the ones walked into thaw.
void FreezeRestingRagdolls(PhysicsState& s, float dt) {
    for (int id : s.thawRequests)
        if (id >= 0 && id < (int)s.ragdolls.size() && s.ragdolls[(size_t)id].Art) ThawRagdoll(s, s.ragdolls[(size_t)id]);
    s.thawRequests.clear();
    constexpr float kFreezeAfter = 1.0f;
    for (auto& rd : s.ragdolls) {
        if (!rd.Art || rd.Frozen) continue;
        // Passing the bodies it began in: clear of them (a hand's breadth), it collides with them again.
        if (rd.PassBodies && !TouchesOtherBody(s, rd, 0.05f)) SetPassBodies(s, rd, false);
        bool held = false; // a gun still in its hand: that joint needs the link in the scene
        for (PxJoint* j : rd.Attach) held = held || j;
        if (held || !rd.Art->isSleeping()) { rd.Rest = 0.0f; continue; }
        rd.Rest += dt;
        if (rd.Rest >= kFreezeAfter) FreezeRagdoll(s, rd);
    }
}

namespace {

bool ValidRagdoll(int ragdoll) {
    return g_State && ragdoll >= 0 && ragdoll < (int)g_State->ragdolls.size() && g_State->ragdolls[(size_t)ragdoll].Art;
}

const PxArticulationAxis::Enum kSphericalAxes[3] = {PxArticulationAxis::eTWIST, PxArticulationAxis::eSWING1, PxArticulationAxis::eSWING2};

} // namespace

// Before every substep: the extra gravity, and pairs that overlapped at build let collide once they are apart.
void RagdollAfterSubstep(PhysicsState::Ragdoll& rd) {
    if (!rd.Art || rd.Frozen || !rd.Good || rd.Art->isSleeping()) return;
    // Far past anything a falling body does: only a solve that has come apart moves a part this fast.
    constexpr float kMaxSpeed = 60.0f, kMaxSpin = 300.0f;
    bool sane = true;
    for (PxArticulationLink* l : rd.Bodies) {
        if (!l) continue;
        const PxVec3 v = l->getLinearVelocity(), w = l->getAngularVelocity();
        if (!PoseIsFinite(l->getGlobalPose()) || !v.isFinite() || !w.isFinite() || v.magnitudeSquared() > kMaxSpeed * kMaxSpeed ||
            w.magnitudeSquared() > kMaxSpin * kMaxSpin) { sane = false; break; }
    }
    if (sane) {
        rd.Art->copyInternalStateToCache(*rd.Good, PxArticulationCacheFlag::ePOSITION | PxArticulationCacheFlag::eROOT_TRANSFORM);
        return;
    }
    const PxU32 dofs = rd.Art->getDofs();
    for (PxU32 i = 0; i < dofs; ++i) rd.Good->jointVelocity[i] = 0.0f;
    rd.Good->rootLinkData->worldLinVel = PxVec3(0.0f);
    rd.Good->rootLinkData->worldAngVel = PxVec3(0.0f);
    rd.Art->applyCache(*rd.Good, PxArticulationCacheFlag::ePOSITION | PxArticulationCacheFlag::eVELOCITY |
                                     PxArticulationCacheFlag::eROOT_TRANSFORM | PxArticulationCacheFlag::eROOT_VELOCITIES, false);
    rd.Art->updateKinematic(PxArticulationKinematicFlag::ePOSITION | PxArticulationKinematicFlag::eVELOCITY);
    rd.Art->putToSleep();
    if (!rd.Blown) {
        rd.Blown = true;
        Log::Warn("PhysX: a ragdoll's solve blew up (a part non-finite or past 60 m/s) - put back to its last sane pose, at rest.");
    }
}

void RagdollBeforeSubstep(PhysicsState& s, PhysicsState::Ragdoll& rd) {
    if (!rd.Art || rd.Frozen || rd.Art->isSleeping()) return;
    if (rd.ExtraGravity != 0.0f) {
        const PxVec3 g = s.scene->getGravity();
        const float gl = g.magnitude();
        if (gl > 1e-4f) {
            const PxVec3 extra = g * (rd.ExtraGravity / gl);
            for (PxArticulationLink* l : rd.Bodies)
                if (l) l->addForce(extra, PxForceMode::eACCELERATION, false);
        }
    }
    for (size_t k = 0; k < rd.Pending.size();) {
        const auto [a, b] = rd.Pending[k];
        if (LinkGap(rd.Bodies[(size_t)a], rd.Bodies[(size_t)b]) > kPairClearance) {
            rd.Ignore[(size_t)a] &= ~(1u << b);
            rd.Ignore[(size_t)b] &= ~(1u << a);
            ApplyPartFilter(s, rd, a);
            ApplyPartFilter(s, rd, b);
            rd.Pending.erase(rd.Pending.begin() + (long long)k);
        } else {
            ++k;
        }
    }
}

namespace {
// A rotation vector (axis times angle, radians) as a quaternion, and back (the shorter way round).
PxQuat RotationFromVector(const PxVec3& v) {
    const float a = v.magnitude();
    return a > 1e-8f ? PxQuat(a, v / a) : PxQuat(PxIdentity);
}
PxVec3 VectorFromRotation(PxQuat q) {
    if (q.w < 0.0f) q = -q;
    float angle = 0.0f;
    PxVec3 axis(1.0f, 0.0f, 0.0f);
    q.toRadiansAndUnitAxis(angle, axis);
    return axis * angle;
}
} // namespace

void RagdollRotationToJointAxes(const float rotXYZW[4], float out[3]) {
    PxQuat q = ToQuat(rotXYZW);
    if (q.w < 0.0f) q = -q;
    float angle = 0.0f;
    PxVec3 axis(1.0f, 0.0f, 0.0f);
    q.toRadiansAndUnitAxis(angle, axis);
    // The articulation's spherical joint position is the rotation vector of the child's frame against the parent's, by axis.
    const PxVec3 v = axis * angle;
    out[0] = v.x; out[1] = v.y; out[2] = v.z;
}

int CreateRagdoll(unsigned entity, const RagdollPart* parts, int count, const RagdollParams* params) {
    if (!g_State || !g_State->scene || !parts || count <= 0 || count > kMaxRagdollParts) return -1;
    PhysicsState& s = *g_State;
    // Parents first: an articulation link is made under one that exists (the head hangs off the neck, which comes after it).
    std::vector<int> order;
    std::vector<char> placed((size_t)count, 0);
    for (int i = 0; i < count; ++i)
        if (parts[i].Parent < 0 || parts[i].Parent >= count || parts[i].Parent == i) { order.push_back(i); placed[(size_t)i] = 1; break; }
    if (order.empty()) return -1;
    for (bool grew = true; grew && (int)order.size() < count;) {
        grew = false;
        for (int i = 0; i < count; ++i)
            if (!placed[(size_t)i] && parts[i].Parent >= 0 && parts[i].Parent < count && placed[(size_t)parts[i].Parent]) {
                order.push_back(i);
                placed[(size_t)i] = 1;
                grew = true;
            }
    }
    if ((int)order.size() != count) return -1; // a second root, or a loop
    int id = -1;
    for (size_t i = 0; i < s.ragdolls.size(); ++i)
        if (!s.ragdolls[i].Art) { id = (int)i; break; }
    if (id < 0) { id = (int)s.ragdolls.size(); s.ragdolls.emplace_back(); }
    PhysicsState::Ragdoll& rd = s.ragdolls[(size_t)id];
    rd = PhysicsState::Ragdoll();
    const RagdollParams prm = params ? *params : RagdollParams();
    PxArticulationReducedCoordinate* art = s.physics->createArticulationReducedCoordinate();
    if (!art) return -1;
    art->setSolverIterationCounts((PxU32)std::max(prm.SolverPosIters, 1), (PxU32)std::max(prm.SolverVelIters, 1));
    art->setSleepThreshold(prm.SleepThreshold);
    if (prm.StabilizationThreshold >= 0.0f) art->setStabilizationThreshold(prm.StabilizationThreshold);
    PxMaterial* mat = s.physics->createMaterial(prm.StaticFriction, prm.DynamicFriction, prm.Restitution);
    if (prm.Grip) {
        mat->setFrictionCombineMode(PxCombineMode::eMAX);
        mat->setRestitutionCombineMode(PxCombineMode::eMULTIPLY);
    }
    // A hand slides off an edge it would otherwise hook on (and be levered past its range by the body hanging from it).
    PxMaterial* slick = nullptr;
    float slickScale = 1.0f;
    for (int i = 0; i < count && !slick; ++i)
        if (std::fabs(parts[i].FrictionScale - 1.0f) > 1e-3f) {
            slickScale = std::max(parts[i].FrictionScale, 0.0f);
            slick = s.physics->createMaterial(prm.StaticFriction * slickScale, prm.DynamicFriction * slickScale, prm.Restitution);
            slick->setFrictionCombineMode(PxCombineMode::eMIN); // (and the floor's grip doesn't win it back)
            if (prm.Grip) slick->setRestitutionCombineMode(PxCombineMode::eMULTIPLY);
        }
    auto poseOf = [&](int i) {
        const RagdollPart& p = parts[i];
        return PxTransform(PxVec3(p.Position[0], p.Position[1], p.Position[2]), PxQuat(p.Rotation[0], p.Rotation[1], p.Rotation[2], p.Rotation[3]).getNormalized());
    };
    // Who collides with whom: never parent and child; a pair already overlapping in the pose the body is built in is let through
    // until it has come apart (an arm across the chest holding a gun), then it collides like the rest.
    rd.Ignore.assign((size_t)count, 0u);
    for (int i = 0; i < count; ++i)
        for (int j = 0; j < count; ++j)
            if (j != i && (parts[i].IgnoreMask & (1u << j))) { rd.Ignore[(size_t)i] |= 1u << j; rd.Ignore[(size_t)j] |= 1u << i; }
    for (int i = 0; i < count; ++i) {
        const int pa = parts[i].Parent;
        if (pa < 0 || pa >= count || pa == i) continue;
        rd.Ignore[(size_t)i] |= 1u << pa;
        rd.Ignore[(size_t)pa] |= 1u << i;
    }
    if (!prm.SelfCollision) {
        for (PxU32& m : rd.Ignore) m = 0xFFFFFFFFu;
    } else {
        for (int i = 0; i < count; ++i)
            for (int j = i + 1; j < count; ++j) {
                if (rd.Ignore[(size_t)i] & (1u << j)) continue;
                const float gap = CapsuleGap(poseOf(i), std::max(parts[i].HalfLength, 0.005f), std::max(parts[i].Radius, 0.01f), poseOf(j),
                                             std::max(parts[j].HalfLength, 0.005f), std::max(parts[j].Radius, 0.01f));
                if (gap < kPairClearance) {
                    rd.Ignore[(size_t)i] |= 1u << j;
                    rd.Ignore[(size_t)j] |= 1u << i;
                    rd.Pending.push_back({i, j});
                }
            }
    }
    rd.Bodies.assign((size_t)count, nullptr);
    rd.Drive.assign((size_t)count, nullptr);
    // Each link's frame is its joint's child frame (the root's, its capsule's), and the capsule is turned inside it: PhysX builds
    // a spherical joint's drive target in the child link's frame but measures the joint in the joint frame, so the two have to be
    // the same frame or a drive about one axis turns the joint about another (a knee asked to bend pushes into its sideways limit).
    // A shoulder's zero is the middle of its range (its parent frame: the neutral frame turned there), not the pose the
    // body is built in: the joint position is a rotation vector, and a limb that may turn far from its zero (an arm raised to aim
    // swinging down and back) nears the half turn where that vector flips, and where the solver holds the limits worst. From the
    // middle, no joint is ever more than its half range from zero. The link starts at the build pose's position in that range.
    std::vector<PxTransform> body((size_t)count), frames((size_t)count);
    std::vector<PxVec3> centre((size_t)count, PxVec3(0.0f)), start((size_t)count, PxVec3(0.0f));
    rd.TargetPre.assign((size_t)count, PxQuat(PxIdentity));
    rd.TargetPost.assign((size_t)count, PxQuat(PxIdentity));
    for (int i = 0; i < count; ++i) {
        const RagdollPart& p = parts[i];
        const PxTransform cap = poseOf(i);
        const PxVec3 anchor(p.Anchor[0], p.Anchor[1], p.Anchor[2]);
        const bool root = i == order[0];
        if (root || !p.Anatomical) {
            frames[(size_t)i] = PxTransform(anchor, cap.q);
            body[(size_t)i] = PxTransform(cap.p, cap.q);
            continue;
        }
        const PxQuat neutral = PxQuat(p.LimitFrame[0], p.LimitFrame[1], p.LimitFrame[2], p.LimitFrame[3]).getNormalized();
        // Where the bone points in the neutral frame, as the rotation vector of the shortest arc from +X (no twist).
        const PxVec3 d = neutral.rotateInv(cap.q.rotate(PxVec3(1.0f, 0.0f, 0.0f)));
        const PxVec3 axis(0.0f, -d.z, d.y);
        const float sn = axis.magnitude(), theta = std::atan2(sn, d.x);
        PxVec3 built = sn > 1e-6f ? axis * (theta / sn) : PxVec3(0.0f);
        // Only a joint whose range spans more than a half turn (a shoulder) can be turned that far from where it was built; the
        // rest keep the build pose as their zero (their ranges shifted by where the bone points), the frames the drives were tuned in.
        const bool wide = !p.Hinge && (p.SwingZMax - p.SwingZMin > 180.0f || p.SwingYMax - p.SwingYMin > 180.0f);
        if (!wide) {
            frames[(size_t)i] = PxTransform(anchor, neutral);
            body[(size_t)i] = PxTransform(cap.p, neutral);
            centre[(size_t)i] = PxVec3(0.0f, built.y, built.z);
            continue;
        }
        const PxVec3 mid(PxPi / 360.0f * (p.TwistMin + p.TwistMax), PxPi / 360.0f * (p.SwingYMin + p.SwingYMax), PxPi / 360.0f * (p.SwingZMin + p.SwingZMax));
        const PxQuat qBuilt = RotationFromVector(built), qMid = RotationFromVector(mid);
        frames[(size_t)i] = PxTransform(anchor, (neutral * qMid).getNormalized());
        body[(size_t)i] = PxTransform(cap.p, (neutral * qBuilt).getNormalized());
        centre[(size_t)i] = mid;
        start[(size_t)i] = VectorFromRotation(qMid.getConjugate() * qBuilt);
        // The motor's targets turn the joint from the build pose in the neutral frame's axes.
        rd.TargetPre[(size_t)i] = qMid.getConjugate();
        rd.TargetPost[(size_t)i] = qBuilt;
    }
    rd.Shape.assign((size_t)count, PxTransform(PxIdentity));
    for (int i : order) {
        const RagdollPart& p = parts[i];
        const PxTransform pose = poseOf(i); // the capsule's
        const bool root = i == order[0];
        PxArticulationLink* link = art->createLink(root ? nullptr : rd.Bodies[(size_t)p.Parent], body[(size_t)i]);
        if (!link) { art->release(); mat->release(); if (slick) slick->release(); rd = PhysicsState::Ragdoll(); return -1; }
        const PxTransform shapeLocal = body[(size_t)i].transformInv(pose);
        rd.Shape[(size_t)i] = shapeLocal;
        PxShape* sh = PxRigidActorExt::createExclusiveShape(*link, PxCapsuleGeometry(std::max(p.Radius, 0.01f), std::max(p.HalfLength, 0.005f)),
                                                              slick && std::fabs(p.FrictionScale - 1.0f) > 1e-3f ? *slick : *mat);
        sh->setLocalPose(shapeLocal);
        sh->setSimulationFilterData(PxFilterData(1u, 0u, ((PxU32)id + 1u) | ((PxU32)i << 16), rd.Ignore[(size_t)i]));
        sh->setQueryFilterData(PxFilterData(1u, 0u, (PxU32)id + 1u, 0u));
        if (prm.SkinOffset > 0.0f) {
            // The flesh over the bone: contacts rest this far out, so the skinned mesh lies on the floor instead of in it.
            sh->setContactOffset(std::max(sh->getContactOffset(), prm.SkinOffset + 0.02f));
            sh->setRestOffset(prm.SkinOffset);
        }
        sh->userData = BodyPartTag(kBodyPartRagdoll, (unsigned)i); // RaycastBodyParts: ragdoll part i
        PxRigidBodyExt::setMassAndUpdateInertia(*link, std::max(p.Mass, 0.1f));
        if (p.InertiaHalfWidth > 0.0f || p.InertiaScale != 1.0f) {
            PxVec3 I = link->getMassSpaceInertiaTensor();
            if (p.InertiaHalfWidth > 0.0f) {
                // A box of the part's width and depth, as long as its capsule: principal axes along the bone, across the body, and
                // front to back.
                const float m = link->getMass(), w = 2.0f * p.InertiaHalfWidth, d = 2.0f * std::max(p.InertiaHalfDepth, 0.01f);
                const float L = 2.0f * (std::max(p.HalfLength, 0.005f) + std::max(p.Radius, 0.01f));
                PxVec3 lat = pose.q.rotateInv(PxVec3(p.InertiaLateral[0], p.InertiaLateral[1], p.InertiaLateral[2]));
                lat.x = 0.0f;
                if (lat.magnitudeSquared() > 1e-6f) {
                    lat.normalize();
                    const PxVec3 x(1.0f, 0.0f, 0.0f), z = x.cross(lat);
                    link->setCMassLocalPose(shapeLocal * PxTransform(PxVec3(0.0f), PxQuat(PxMat33(x, lat, z)).getNormalized()));
                    I = PxVec3(m / 12.0f * (w * w + d * d), m / 12.0f * (d * d + L * L), m / 12.0f * (w * w + L * L));
                }
            }
            link->setMassSpaceInertiaTensor(I * std::max(p.InertiaScale, 0.05f));
        }
        link->setLinearDamping(prm.LinearDamping);
        link->setAngularDamping(prm.AngularDamping);
        link->setMaxDepenetrationVelocity(prm.Depenetration);
        link->userData = reinterpret_cast<void*>(static_cast<std::uintptr_t>(entity));
        rd.Bodies[(size_t)i] = link;
        if (root) continue;
        PxArticulationJointReducedCoordinate* j = link->getInboundJoint();
        if (!j) continue;
        // The joint frame: at the anchor, its X along the child (the twist axis); an anatomical joint's is its neutral frame turned
        // to the middle of its range. The child's is the link's own frame there (a translation only).
        const PxTransform& frame = frames[(size_t)i];
        // A hinge (an elbow, a knee) turns about its flexion axis alone: it cannot bend sideways or twist at all, whatever loads it.
        const bool hinge = p.Hinge && p.Anatomical;
        j->setJointType(hinge ? PxArticulationJointType::eREVOLUTE : PxArticulationJointType::eSPHERICAL);
        j->setParentPose(body[(size_t)p.Parent].transformInv(frame));
        j->setChildPose(body[(size_t)i].transformInv(PxTransform(frame.p, body[(size_t)i].q)));
        for (PxArticulationAxis::Enum ax : kSphericalAxes)
            j->setMotion(ax, !hinge || ax == PxArticulationAxis::eSWING2 ? PxArticulationMotion::eLIMITED : PxArticulationMotion::eLOCKED);
        // Kept clear of a half turn: the joint's position is a rotation vector, which flips sign there (a limit at 180 degrees
        // would let the limb through it).
        const float lim = PxPi * 150.0f / 180.0f;
        auto range = [&](float loDeg, float hiDeg, float off, float at, PxArticulationAxis::Enum ax) {
            if (j->getMotion(ax) == PxArticulationMotion::eLOCKED) return;
            // (Widened to hold the pose it starts in: an animated pose a little past the anatomical range.)
            const float lo = std::min(std::clamp(PxPi * loDeg / 180.0f - off, -lim, 0.0f), at - 0.005f);
            const float hi = std::max(std::clamp(PxPi * hiDeg / 180.0f - off, 0.0f, lim), at + 0.005f);
            j->setLimitParams(ax, PxArticulationLimit(lo, hi));
        };
        if (p.Anatomical) {
            // The ranges are from the neutral; the joint's zero is their middle.
            const PxVec3& c = centre[(size_t)i];
            const PxVec3& at = start[(size_t)i];
            range(p.TwistMin, p.TwistMax, c.x, at.x, PxArticulationAxis::eTWIST);
            range(p.SwingYMin, p.SwingYMax, c.y, at.y, PxArticulationAxis::eSWING1);
            range(p.SwingZMin, p.SwingZMax, c.z, at.z, PxArticulationAxis::eSWING2);
        } else {
            const float tw = std::clamp(p.TwistDeg, 1.0f, 170.0f), sw = std::clamp(p.SwingDeg, 1.0f, 170.0f);
            range(-tw, tw, 0.0f, 0.0f, PxArticulationAxis::eTWIST);
            range(-sw, sw, 0.0f, 0.0f, PxArticulationAxis::eSWING1);
            range(-sw, sw, 0.0f, 0.0f, PxArticulationAxis::eSWING2);
        }
        rd.Drive[(size_t)i] = j;
    }
    // Each joint's drive is a torque spring scaled by what it turns: the part and everything hanging off it, about the joint
    // (a knee turns the calf and foot, a hip the whole leg, the lower spine the chest, arms and head). So a stiffness reads as
    // an acceleration (mass independent), as the old slerp drives' did, but by the body's own build, not the solver's.
    rd.DriveInertia.assign((size_t)count, 0.0f);
    for (int i = 0; i < count; ++i) {
        if (!rd.Drive[(size_t)i]) continue;
        const PxVec3 anchor(parts[i].Anchor[0], parts[i].Anchor[1], parts[i].Anchor[2]);
        float I = 0.0f;
        for (int d = 0; d < count; ++d) {
            bool under = false;
            for (int a = d, guard = 0; a >= 0 && a < count && guard < count; a = parts[a].Parent, ++guard)
                if (a == i) { under = true; break; }
            if (!under || !rd.Bodies[(size_t)d]) continue;
            const PxVec3 c = (rd.Bodies[(size_t)d]->getGlobalPose() * rd.Bodies[(size_t)d]->getCMassLocalPose()).p;
            const PxVec3 own = rd.Bodies[(size_t)d]->getMassSpaceInertiaTensor();
            I += rd.Bodies[(size_t)d]->getMass() * (c - anchor).magnitudeSquared() + (own.x + own.y + own.z) / 3.0f;
        }
        rd.DriveInertia[(size_t)i] = std::max(I, 1e-4f);
        const float damp = std::max(parts[i].JointDamping, 0.0f) * rd.DriveInertia[(size_t)i];
        for (PxArticulationAxis::Enum ax : kSphericalAxes) {
            if (rd.Drive[(size_t)i]->getMotion(ax) == PxArticulationMotion::eLOCKED) continue;
            const float arm = ax == PxArticulationAxis::eTWIST ? prm.TwistArmature : prm.SwingArmature;
            if (arm > 0.0f) rd.Drive[(size_t)i]->setArmature(ax, arm * rd.DriveInertia[(size_t)i]);
            rd.Drive[(size_t)i]->setDriveParams(ax, PxArticulationDrive(0.0f, damp, PX_MAX_F32, PxArticulationDriveType::eFORCE));
            const PxU32 k = ax == PxArticulationAxis::eTWIST ? 0u : ax == PxArticulationAxis::eSWING1 ? 1u : 2u;
            rd.Drive[(size_t)i]->setDriveTarget(ax, start[(size_t)i][k], false); // (holding the build pose)
        }
    }
    rd.Art = art;
    rd.Mat = mat; // kept for SetRagdollFriction (the shapes hold their own reference too); released with the ragdoll
    rd.SlickMat = slick;
    rd.SlickScale = slickScale;
    {
        const float gl = s.scene->getGravity().magnitude();
        rd.ExtraGravity = gl * (std::max(prm.GravityScale, 0.0f) - 1.0f);
    }
    if (!s.scene->addArticulation(*art)) {
        art->release();
        mat->release();
        if (slick) slick->release();
        rd = PhysicsState::Ragdoll();
        return -1;
    }
    // Every joint where the body was built (its zero is the middle of its range, not there), the links put where that has them
    // (where they already are).
    for (int i = 0; i < count; ++i) {
        PxArticulationJointReducedCoordinate* j = rd.Drive[(size_t)i];
        if (!j || start[(size_t)i].magnitudeSquared() < 1e-12f) continue;
        for (int k = 0; k < 3; ++k)
            if (j->getMotion(kSphericalAxes[k]) != PxArticulationMotion::eLOCKED) j->setJointPosition(kSphericalAxes[k], start[(size_t)i][(PxU32)k]);
    }
    art->updateKinematic(PxArticulationKinematicFlag::ePOSITION);
    // Every part's own motion: the root's as given, the rest as each joint's turn against its parent (the chain carries the
    // linear velocity the turns imply).
    const RagdollPart& r = parts[order[0]];
    art->setRootLinearVelocity(PxVec3(r.Velocity[0], r.Velocity[1], r.Velocity[2]));
    art->setRootAngularVelocity(PxVec3(r.AngularVelocity[0], r.AngularVelocity[1], r.AngularVelocity[2]));
    for (int i : order) {
        PxArticulationJointReducedCoordinate* j = rd.Drive[(size_t)i];
        if (!j) continue;
        const RagdollPart& p = parts[i];
        const RagdollPart& pp = parts[p.Parent];
        const PxVec3 w(p.AngularVelocity[0] - pp.AngularVelocity[0], p.AngularVelocity[1] - pp.AngularVelocity[1], p.AngularVelocity[2] - pp.AngularVelocity[2]);
        if (w.magnitudeSquared() < 1e-10f) continue;
        const PxQuat fq = (rd.Bodies[(size_t)i]->getGlobalPose() * j->getChildPose()).q;
        const PxVec3 basis[3] = {fq.getBasisVector0(), fq.getBasisVector1(), fq.getBasisVector2()};
        for (int k = 0; k < 3; ++k) {
            if (j->getMotion(kSphericalAxes[k]) == PxArticulationMotion::eLOCKED) continue;
            // Not on out through a limit the pose already sits at (an animated wrist bent to its range edge): the limit would
            // have to stop it in the first step, and a light hand goes through instead.
            float v = w.dot(basis[k]);
            const PxArticulationLimit l = j->getLimitParams(kSphericalAxes[k]);
            const float at = j->getJointPosition(kSphericalAxes[k]), edge = 0.1f;
            if ((v > 0.0f && at > l.high - edge) || (v < 0.0f && at < l.low + edge)) v = 0.0f;
            j->setJointVelocity(kSphericalAxes[k], v);
        }
    }
    // Begun in or on another body (a soldier's capsule passes the dead: he died standing in one), it passes the bodies till it is
    // clear of them. Pushed out of a frozen corpse - as immovable as a wall - it went the only way left, down through the floor; out
    // of a moving one the pair's solve blew up, and the last sane pose it went back to was already through it.
    if (TouchesOtherBody(s, rd, 0.0f)) SetPassBodies(s, rd, true);
    rd.Good = art->createCache();
    if (rd.Good) art->copyInternalStateToCache(*rd.Good, PxArticulationCacheFlag::ePOSITION | PxArticulationCacheFlag::eROOT_TRANSFORM);
    return id;
}

void DestroyRagdoll(int ragdoll) {
    if (!g_State || ragdoll < 0 || ragdoll >= (int)g_State->ragdolls.size()) return;
    auto& rd = g_State->ragdolls[(size_t)ragdoll];
    for (PxJoint* j : rd.Attach) if (j) j->release();
    if (rd.Frozen) { g_State->scene->removeActor(*rd.Frozen); rd.Frozen->release(); }
    if (rd.Good) rd.Good->release();
    if (rd.Art) rd.Art->release(); // its links, shapes and joints, and out of the scene
    if (rd.Mat) rd.Mat->release();
    if (rd.SlickMat) rd.SlickMat->release();
    rd = PhysicsState::Ragdoll();
}

void SetRagdollDamping(int ragdoll, float linear, float angular) {
    if (!ValidRagdoll(ragdoll)) return;
    for (PxArticulationLink* b : g_State->ragdolls[(size_t)ragdoll].Bodies)
        if (b) { b->setLinearDamping(std::max(linear, 0.0f)); b->setAngularDamping(std::max(angular, 0.0f)); }
}

void SetRagdollFriction(int ragdoll, float staticFriction, float dynamicFriction) {
    if (!ValidRagdoll(ragdoll)) return;
    const auto& rd = g_State->ragdolls[(size_t)ragdoll];
    if (!rd.Mat) return;
    rd.Mat->setStaticFriction(std::max(staticFriction, 0.0f));
    rd.Mat->setDynamicFriction(std::max(dynamicFriction, 0.0f));
    if (rd.SlickMat) {
        rd.SlickMat->setStaticFriction(std::max(staticFriction, 0.0f) * rd.SlickScale);
        rd.SlickMat->setDynamicFriction(std::max(dynamicFriction, 0.0f) * rd.SlickScale);
    }
}

bool RagdollMotion(int ragdoll, float* outMaxLinear, float* outMaxAngular) {
    if (!ValidRagdoll(ragdoll)) return false;
    const auto& rd = g_State->ragdolls[(size_t)ragdoll];
    float lin = 0.0f, ang = 0.0f;
    for (PxArticulationLink* b : rd.Bodies) {
        if (!b || rd.Frozen) continue; // (frozen: still)
        lin = std::max(lin, b->getLinearVelocity().magnitude());
        ang = std::max(ang, b->getAngularVelocity().magnitude());
    }
    if (outMaxLinear) *outMaxLinear = lin;
    if (outMaxAngular) *outMaxAngular = ang;
    return true;
}

namespace {
// What touches a body coming to rest and has come to rest with it (a gun on its arm, a second corpse across it) sleeps with it: a body
// put to sleep against one still awake is woken by the next step, and the two would keep each other awake for good.
void SleepRestingNeighbours(PhysicsState& s, const PhysicsState::Ragdoll& rd) {
    constexpr float kReach = 0.03f, kSlowLinear = 0.3f, kSlowAngular = 2.0f;
    const auto slow = [&](const PxRigidBody& b) {
        return b.getLinearVelocity().magnitude() < kSlowLinear && b.getAngularVelocity().magnitude() < kSlowAngular;
    };
    PxOverlapHit hits[32];
    for (PxArticulationLink* l : rd.Bodies) {
        PxShape* sh = l ? LinkShape(l) : nullptr;
        if (!sh || sh->getGeometry().getType() != PxGeometryType::eCAPSULE) continue;
        PxCapsuleGeometry cap = static_cast<const PxCapsuleGeometry&>(sh->getGeometry());
        cap.radius += std::max(sh->getRestOffset(), 0.0f) + kReach;
        PxOverlapBuffer buf(hits, 32);
        if (!s.scene->overlap(cap, l->getGlobalPose() * sh->getLocalPose(), buf, PxQueryFilterData(PxQueryFlag::eDYNAMIC | PxQueryFlag::eNO_BLOCK)))
            continue;
        for (PxU32 i = 0; i < buf.getNbTouches(); ++i) {
            const PxOverlapHit& h = buf.getTouch(i);
            if (!h.actor || (h.shape && (h.shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE))) continue;
            if (PxArticulationLink* other = h.actor->is<PxArticulationLink>()) {
                PxArticulationReducedCoordinate& art = other->getArticulation();
                if (&art == rd.Art || art.isSleeping()) continue;
                PxArticulationLink* links[64];
                const PxU32 n = art.getLinks(links, 64);
                bool still = true;
                for (PxU32 k = 0; k < n && still; ++k) still = slow(*links[k]);
                if (still) art.putToSleep();
            } else if (PxRigidDynamic* d = h.actor->is<PxRigidDynamic>()) {
                if ((d->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) || d->isSleeping() || !slow(*d)) continue;
                d->putToSleep();
            }
        }
    }
}
} // namespace

void RagdollSleep(int ragdoll) {
    if (!ValidRagdoll(ragdoll)) return;
    auto& rd = g_State->ragdolls[(size_t)ragdoll];
    SleepRestingNeighbours(*g_State, rd); // (a frozen body's parts are where they lay: it still finds them)
    if (!rd.Frozen && !rd.Art->isSleeping()) rd.Art->putToSleep(); // (its velocities go to zero)
}

void RagdollImpulse(int ragdoll, int part, const float impulse[3], const float point[3]) {
    if (!ValidRagdoll(ragdoll)) return;
    auto& rd = g_State->ragdolls[(size_t)ragdoll];
    if (part < 0 || part >= (int)rd.Bodies.size() || !rd.Bodies[(size_t)part]) return;
    ThawRagdoll(*g_State, rd);
    PxRigidBodyExt::addForceAtPos(*rd.Bodies[(size_t)part], PxVec3(impulse[0], impulse[1], impulse[2]),
                                  PxVec3(point[0], point[1], point[2]), PxForceMode::eIMPULSE, true);
}

bool GetRagdollPart(int ragdoll, int part, float outPos[3], float outRotXYZW[4]) {
    if (!ValidRagdoll(ragdoll)) return false;
    auto& rd = g_State->ragdolls[(size_t)ragdoll];
    if (part < 0 || part >= (int)rd.Bodies.size() || !rd.Bodies[(size_t)part]) return false;
    PxTransform t = rd.Bodies[(size_t)part]->getGlobalPose();
    // Drawn between the last two substeps (#168), so a body falling at 240 fps doesn't step at 60.
    if ((size_t)part < rd.Prev.size() && PoseIsFinite(rd.Prev[(size_t)part]) && PoseIsFinite(t)) {
        const float alpha = std::clamp(g_State->stepAccumulator / FixedStep(), 0.0f, 1.0f);
        const PxTransform& a = rd.Prev[(size_t)part];
        t.p = a.p + (t.p - a.p) * alpha;
        const glm::quat q = glm::slerp(glm::quat(a.q.w, a.q.x, a.q.y, a.q.z), glm::quat(t.q.w, t.q.x, t.q.y, t.q.z), alpha);
        t.q = PxQuat(q.x, q.y, q.z, q.w);
    }
    if ((size_t)part < rd.Shape.size()) t = t * rd.Shape[(size_t)part]; // the capsule's pose (its X along the bone)
    outPos[0] = t.p.x; outPos[1] = t.p.y; outPos[2] = t.p.z;
    outRotXYZW[0] = t.q.x; outRotXYZW[1] = t.q.y; outRotXYZW[2] = t.q.z; outRotXYZW[3] = t.q.w;
    return true;
}

void SetRagdollDrive(int ragdoll, float stiffness, float damping) {
    if (!ValidRagdoll(ragdoll)) return;
    for (size_t i = 0; i < g_State->ragdolls[(size_t)ragdoll].Drive.size(); ++i) SetRagdollPartDrive(ragdoll, (int)i, stiffness, damping);
}

void SetRagdollPartDrive(int ragdoll, int part, float stiffness, float damping) {
    if (!ValidRagdoll(ragdoll)) return;
    auto& drive = g_State->ragdolls[(size_t)ragdoll].Drive;
    if (part < 0 || part >= (int)drive.size() || !drive[(size_t)part]) return;
    const float I = g_State->ragdolls[(size_t)ragdoll].DriveInertia[(size_t)part];
    const PxArticulationDrive d(std::max(stiffness, 0.0f) * I, std::max(damping, 0.0f) * I, PX_MAX_F32, PxArticulationDriveType::eFORCE);
    for (PxArticulationAxis::Enum ax : kSphericalAxes)
        if (drive[(size_t)part]->getMotion(ax) != PxArticulationMotion::eLOCKED) drive[(size_t)part]->setDriveParams(ax, d);
}

bool GetRagdollPartInertia(int ragdoll, int part, float out[3]) {
    if (!ValidRagdoll(ragdoll) || !out) return false;
    auto& rd = g_State->ragdolls[(size_t)ragdoll];
    if (part < 0 || part >= (int)rd.Bodies.size() || !rd.Bodies[(size_t)part]) return false;
    const PxVec3 I = rd.Bodies[(size_t)part]->getMassSpaceInertiaTensor();
    out[0] = I.x; out[1] = I.y; out[2] = I.z;
    return true;
}

void SetRagdollDriveTarget(int ragdoll, int part, const float rotXYZW[4]) {
    if (!ValidRagdoll(ragdoll) || !rotXYZW) return;
    auto& drive = g_State->ragdolls[(size_t)ragdoll].Drive;
    if (part < 0 || part >= (int)drive.size() || !drive[(size_t)part]) return;
    // (The target turns the joint from the build pose; the joint's own rotation is from the middle of its range.)
    const auto& rd = g_State->ragdolls[(size_t)ragdoll];
    PxQuat q = ToQuat(rotXYZW);
    if ((size_t)part < rd.TargetPre.size()) q = rd.TargetPre[(size_t)part] * q * rd.TargetPost[(size_t)part];
    const PxVec3 v = VectorFromRotation(q.getNormalized());
    const float axes[3] = {v.x, v.y, v.z};
    for (int k = 0; k < 3; ++k) // (a sleeping body stays so)
        if (drive[(size_t)part]->getMotion(kSphericalAxes[k]) != PxArticulationMotion::eLOCKED) drive[(size_t)part]->setDriveTarget(kSphericalAxes[k], axes[k], /*autowake=*/false);
}

bool RagdollAsleep(int ragdoll) {
    if (!ValidRagdoll(ragdoll)) return true;
    const auto& rd = g_State->ragdolls[(size_t)ragdoll];
    return rd.Frozen || rd.Art->isSleeping();
}

void RagdollCounts(int& awake, int& asleep, int& frozen, float* fastestAwake) {
    awake = asleep = frozen = 0;
    if (fastestAwake) *fastestAwake = 0.0f;
    if (!g_State) return;
    for (const auto& rd : g_State->ragdolls) {
        if (!rd.Art) continue;
        if (rd.Frozen) { ++frozen; continue; }
        if (rd.Art->isSleeping()) { ++asleep; continue; }
        ++awake;
        if (fastestAwake)
            for (PxArticulationLink* l : rd.Bodies)
                if (l) *fastestAwake = std::max(*fastestAwake, l->getLinearVelocity().magnitude());
    }
}

bool RagdollFrozen(int ragdoll) { return ValidRagdoll(ragdoll) && g_State->ragdolls[(size_t)ragdoll].Frozen; }

bool RagdollPairCollides(int ragdoll, int a, int b) {
    if (!ValidRagdoll(ragdoll)) return false;
    const auto& rd = g_State->ragdolls[(size_t)ragdoll];
    if (a < 0 || b < 0 || a >= (int)rd.Ignore.size() || b >= (int)rd.Ignore.size() || a == b) return false;
    return !(rd.Ignore[(size_t)a] & (1u << b)) && !(rd.Ignore[(size_t)b] & (1u << a));
}

bool GetRagdollJoint(int ragdoll, int part, float outPos[3], float outLo[3], float outHi[3]) {
    if (!ValidRagdoll(ragdoll)) return false;
    auto& drive = g_State->ragdolls[(size_t)ragdoll].Drive;
    if (part < 0 || part >= (int)drive.size() || !drive[(size_t)part]) return false;
    PxArticulationJointReducedCoordinate* j = drive[(size_t)part];
    for (int k = 0; k < 3; ++k) {
        const bool locked = j->getMotion(kSphericalAxes[k]) == PxArticulationMotion::eLOCKED;
        if (outPos) outPos[k] = locked ? 0.0f : j->getJointPosition(kSphericalAxes[k]);
        const PxArticulationLimit l = locked ? PxArticulationLimit(0.0f, 0.0f) : j->getLimitParams(kSphericalAxes[k]);
        if (outLo) outLo[k] = l.low;
        if (outHi) outHi[k] = l.high;
    }
    return true;
}

int AttachToRagdollPart(unsigned entity, int ragdoll, int part, float breakForce) {
    if (!ValidRagdoll(ragdoll)) return -1;
    PhysicsState& s = *g_State;
    auto& rd = s.ragdolls[(size_t)ragdoll];
    if (part < 0 || part >= (int)rd.Bodies.size() || !rd.Bodies[(size_t)part]) return -1;
    ThawRagdoll(s, rd);
    auto it = s.bodyByEntity.find(entity);
    if (it == s.bodyByEntity.end() || !it->second) return -1;
    PxRigidDynamic* body = it->second;
    PxArticulationLink* link = rd.Bodies[(size_t)part];
    // Held where it is (both frames at the body's own pose), by a grip, not a weld: a stiff spring (acceleration units, so the gun's
    // weight doesn't matter) pulls it back to where the hand has it. A rigid joint would let a heavy gun lever a light hand
    // through its wrist and elbow limits.
    const PxTransform at = body->getGlobalPose();
    PxD6Joint* j = PxD6JointCreate(*s.physics, link, link->getGlobalPose().transformInv(at), body, PxTransform(PxIdentity));
    if (!j) return -1;
    for (PxD6Axis::Enum ax : {PxD6Axis::eX, PxD6Axis::eY, PxD6Axis::eZ, PxD6Axis::eTWIST, PxD6Axis::eSWING1, PxD6Axis::eSWING2})
        j->setMotion(ax, PxD6Motion::eFREE);
    // Sized by the held body itself (30 rad/s, critically damped), and capped: past the cap the gun lags the hand rather than
    // dragging it.
    const float m = std::max(body->getMass(), 0.1f), omega = 30.0f;
    const PxVec3 I3 = body->getMassSpaceInertiaTensor();
    const float I = std::max(std::max(I3.x, I3.y), std::max(I3.z, 1e-3f));
    const PxD6JointDrive linear(m * omega * omega, 2.0f * m * omega, 120.0f, /*isAcceleration=*/false);
    const PxD6JointDrive angular(I * omega * omega, 2.0f * I * omega, 8.0f, false); // (what a dead wrist can take without turning past its range)
    for (PxD6Drive::Enum d : {PxD6Drive::eX, PxD6Drive::eY, PxD6Drive::eZ}) j->setDrive(d, linear);
    j->setDrive(PxD6Drive::eSLERP, angular);
    j->setDrivePosition(PxTransform(PxIdentity));
    if (breakForce > 0.0f) j->setBreakForce(breakForce, breakForce);
    for (size_t k = 0; k < rd.Attach.size(); ++k)
        if (!rd.Attach[k]) { rd.Attach[k] = j; return (int)k; }
    rd.Attach.push_back(j);
    return (int)rd.Attach.size() - 1;
}

bool RagdollAttachmentHeld(int ragdoll, int handle) {
    if (!ValidRagdoll(ragdoll)) return false;
    auto& rd = g_State->ragdolls[(size_t)ragdoll];
    if (handle < 0 || handle >= (int)rd.Attach.size() || !rd.Attach[(size_t)handle]) return false;
    return !(rd.Attach[(size_t)handle]->getConstraintFlags() & PxConstraintFlag::eBROKEN);
}

void DetachFromRagdoll(int ragdoll, int handle) {
    if (!ValidRagdoll(ragdoll)) return;
    auto& rd = g_State->ragdolls[(size_t)ragdoll];
    if (handle < 0 || handle >= (int)rd.Attach.size() || !rd.Attach[(size_t)handle]) return;
    rd.Attach[(size_t)handle]->release();
    rd.Attach[(size_t)handle] = nullptr;
}

bool IgnoreRagdollParts(unsigned entity, int ragdoll, unsigned partMask) {
    if (!g_State) return false;
    PhysicsState& s = *g_State;
    auto it = s.bodyByEntity.find(entity);
    if (it == s.bodyByEntity.end() || !it->second) return false;
    PxRigidDynamic* body = it->second;
    const bool on = partMask != 0u && ValidRagdoll(ragdoll);
    // Whatever it was passing, it is pushed back out gently, not fired out of the body.
    body->setMaxDepenetrationVelocity(1.5f);
    std::vector<PxShape*> shapes(body->getNbShapes());
    body->getShapes(shapes.data(), (PxU32)shapes.size());
    for (PxShape* sh : shapes) {
        PxFilterData f = sh->getSimulationFilterData();
        // The ignorer slot of the ragdoll's own numbering: not one of its parts, so only this object's mask decides.
        f.word2 = on ? (((PxU32)ragdoll + 1u) | (kRagdollIgnorerSlot << 16)) : 0u;
        f.word3 = on ? partMask : 0u;
        sh->setSimulationFilterData(f);
    }
    if (body->getScene()) {
        s.scene->resetFiltering(*body);
        body->wakeUp(); // (two bodies asleep inside each other would stay so)
    }
    return true;
}

unsigned RagdollPartsOverlapping(unsigned entity, int ragdoll, unsigned partMask, float inflate) {
    if (!ValidRagdoll(ragdoll)) return 0u;
    PhysicsState& s = *g_State;
    auto it = s.bodyByEntity.find(entity);
    if (it == s.bodyByEntity.end() || !it->second) return 0u;
    PxRigidDynamic* body = it->second;
    const auto& rd = s.ragdolls[(size_t)ragdoll];
    std::vector<PxShape*> shapes(body->getNbShapes());
    body->getShapes(shapes.data(), (PxU32)shapes.size());
    const PxTransform bodyPose = body->getGlobalPose();
    unsigned out = 0u;
    for (int i = 0; i < (int)rd.Bodies.size() && i < kMaxRagdollParts; ++i) {
        if (!(partMask & (1u << i)) || !rd.Bodies[(size_t)i]) continue;
        PxShape* ps = LinkShape(rd.Bodies[(size_t)i]);
        if (!ps || ps->getGeometry().getType() != PxGeometryType::eCAPSULE) continue;
        PxCapsuleGeometry cap = static_cast<const PxCapsuleGeometry&>(ps->getGeometry());
        cap.radius = std::max(cap.radius + inflate, 0.005f);
        const PxTransform capPose = rd.Bodies[(size_t)i]->getGlobalPose() * ps->getLocalPose();
        for (PxShape* sh : shapes)
            if ((sh->getFlags() & PxShapeFlag::eSIMULATION_SHAPE) &&
                PxGeometryQuery::overlap(sh->getGeometry(), bodyPose * sh->getLocalPose(), cap, capPose)) { out |= 1u << i; break; }
    }
    return out;
}

bool PushOutOfRagdoll(unsigned entity, int ragdoll, unsigned partMask) {
    if (!ValidRagdoll(ragdoll) || partMask == 0u) return false;
    PhysicsState& s = *g_State;
    auto it = s.bodyByEntity.find(entity);
    if (it == s.bodyByEntity.end() || !it->second) return false;
    PxRigidDynamic* body = it->second;
    const auto& rd = s.ragdolls[(size_t)ragdoll];
    std::vector<PxShape*> shapes(body->getNbShapes());
    body->getShapes(shapes.data(), (PxU32)shapes.size());
    PxTransform pose = body->getGlobalPose();
    bool moved = false;
    for (int pass = 0; pass < 4; ++pass) {
        PxVec3 shift(0.0f);
        for (int i = 0; i < (int)rd.Bodies.size() && i < kMaxRagdollParts; ++i) {
            if (!(partMask & (1u << i)) || !rd.Bodies[(size_t)i]) continue;
            PxShape* ps = LinkShape(rd.Bodies[(size_t)i]);
            if (!ps || ps->getGeometry().getType() != PxGeometryType::eCAPSULE) continue;
            PxCapsuleGeometry cap = static_cast<const PxCapsuleGeometry&>(ps->getGeometry());
            cap.radius += std::max(ps->getRestOffset(), 0.0f); // (out of the skin too)
            const PxTransform capPose = rd.Bodies[(size_t)i]->getGlobalPose() * ps->getLocalPose();
            for (PxShape* sh : shapes) {
                if (!(sh->getFlags() & PxShapeFlag::eSIMULATION_SHAPE)) continue;
                PxVec3 dir;
                PxF32 depth = 0.0f;
                if (PxGeometryQuery::computePenetration(dir, depth, sh->getGeometry(), pose * sh->getLocalPose(), cap, capPose) && depth > 1e-4f)
                    shift += dir * (depth + 0.003f);
            }
        }
        if (shift.magnitudeSquared() < 1e-8f) break;
        // Never down: under a body lying on it, the shortest way out is through the floor. Pinned there, it stays to the contacts.
        if (shift.y < -0.25f * shift.magnitude()) return false;
        pose.p += shift;
        moved = true;
    }
    if (moved) {
        body->setGlobalPose(pose);
        body->setLinearVelocity(PxVec3(0.0f));
        body->setAngularVelocity(PxVec3(0.0f));
    }
    return moved;
}

bool GetRagdollPartShape(int ragdoll, int part, float* outRadius, float* outHalfLength) {
    if (!ValidRagdoll(ragdoll)) return false;
    const auto& rd = g_State->ragdolls[(size_t)ragdoll];
    if (part < 0 || part >= (int)rd.Bodies.size()) return false;
    PxShape* sh = LinkShape(rd.Bodies[(size_t)part]);
    if (!sh || sh->getGeometry().getType() != PxGeometryType::eCAPSULE) return false;
    const PxCapsuleGeometry& g = static_cast<const PxCapsuleGeometry&>(sh->getGeometry());
    if (outRadius) *outRadius = g.radius;
    if (outHalfLength) *outHalfLength = g.halfHeight;
    return true;
}

float RagdollWorstSelfPenetration(int ragdoll, int* outA, int* outB) {
    if (!ValidRagdoll(ragdoll)) return 0.0f;
    const auto& rd = g_State->ragdolls[(size_t)ragdoll];
    float worst = 0.0f;
    for (int a = 0; a < (int)rd.Bodies.size(); ++a)
        for (int b = a + 1; b < (int)rd.Bodies.size(); ++b) {
            if ((rd.Ignore[(size_t)a] & (1u << b)) || (rd.Ignore[(size_t)b] & (1u << a))) continue;
            const float depth = -LinkGap(rd.Bodies[(size_t)a], rd.Bodies[(size_t)b]);
            if (depth > worst) {
                worst = depth;
                if (outA) *outA = a;
                if (outB) *outB = b;
            }
        }
    return worst;
}

// --- Navigation geometry -----------------------------------------------------------------

int CollectStaticGeometry(std::vector<float>& verts, std::vector<int>& tris, bool includeKinematic) {
    if (!g_State || !g_State->scene) return 0;
    const int before = (int)tris.size() / 3;
    PxActorTypeFlags types = PxActorTypeFlag::eRIGID_STATIC;
    if (includeKinematic) types |= PxActorTypeFlag::eRIGID_DYNAMIC;
    const PxU32 n = g_State->scene->getNbActors(types);
    std::vector<PxActor*> actors(n);
    g_State->scene->getActors(types, actors.data(), n);
    auto addVert = [&](const PxVec3& v) {
        verts.push_back(v.x); verts.push_back(v.y); verts.push_back(v.z);
        return (int)verts.size() / 3 - 1;
    };
    auto addBox = [&](const PxTransform& pose, const PxVec3& h) {
        int base = -1;
        for (int i = 0; i < 8; ++i) {
            const PxVec3 c((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
            const int v = addVert(pose.transform(c));
            if (i == 0) base = v;
        }
        // Faces wound counter-clockwise seen from outside (Recast wants up-facing floors CCW from above).
        static const int f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                                     {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
        for (const auto& t : f) { tris.push_back(base + t[0]); tris.push_back(base + t[1]); tris.push_back(base + t[2]); }
    };
    std::vector<PxShape*> shapes;
    for (PxActor* a : actors) {
        auto* ra = a->is<PxRigidActor>();
        if (!ra) continue;
        if (auto* dyn = a->is<PxRigidDynamic>()) {
            if (!(dyn->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC)) continue;
            if (g_State->npcActors.count(ra) || ra == PlayerActor()) continue;
        }
        shapes.resize(ra->getNbShapes());
        ra->getShapes(shapes.data(), (PxU32)shapes.size());
        const PxTransform actorPose = ra->getGlobalPose();
        for (PxShape* sh : shapes) {
            if (sh->getFlags() & PxShapeFlag::eTRIGGER_SHAPE) continue;
            if (!(sh->getFlags() & PxShapeFlag::eSIMULATION_SHAPE)) continue;
            if (BodyPartKindOf(sh->userData) == kBodyPartRagdoll) continue; // a frozen corpse's part (FreezeRagdoll): not ground
            const PxTransform pose = actorPose * sh->getLocalPose();
            const PxGeometry& g = sh->getGeometry();
            switch (g.getType()) {
            case PxGeometryType::eBOX:
                addBox(pose, static_cast<const PxBoxGeometry&>(g).halfExtents);
                break;
            case PxGeometryType::eSPHERE: {
                const float r = static_cast<const PxSphereGeometry&>(g).radius;
                addBox(pose, PxVec3(r));
                break;
            }
            case PxGeometryType::eCAPSULE: {
                const auto& c = static_cast<const PxCapsuleGeometry&>(g);
                addBox(pose, PxVec3(c.halfHeight + c.radius, c.radius, c.radius));
                break;
            }
            case PxGeometryType::eCONVEXMESH: {
                const auto& c = static_cast<const PxConvexMeshGeometry&>(g);
                const PxConvexMesh* m = c.convexMesh;
                const PxVec3* mv = m->getVertices();
                const PxU8* ib = m->getIndexBuffer();
                const int base = (int)verts.size() / 3;
                const PxMat33 sc = c.scale.toMat33();
                for (PxU32 i = 0; i < m->getNbVertices(); ++i) addVert(pose.transform(sc * mv[i]));
                for (PxU32 p = 0; p < m->getNbPolygons(); ++p) {
                    PxHullPolygon poly;
                    if (!m->getPolygonData(p, poly) || poly.mNbVerts < 3) continue;
                    const PxU8* idx = ib + poly.mIndexBase;
                    // The polygon's outward plane normal says which way it faces: wind it to match.
                    const PxVec3 out = pose.rotate(sc.getInverse().getTranspose() * PxVec3(poly.mPlane[0], poly.mPlane[1], poly.mPlane[2]));
                    for (PxU32 k = 1; k + 1 < poly.mNbVerts; ++k) {
                        const int i0 = base + idx[0], i1 = base + idx[k], i2 = base + idx[k + 1];
                        const PxVec3 a0(verts[3 * i0], verts[3 * i0 + 1], verts[3 * i0 + 2]);
                        const PxVec3 a1(verts[3 * i1], verts[3 * i1 + 1], verts[3 * i1 + 2]);
                        const PxVec3 a2(verts[3 * i2], verts[3 * i2 + 1], verts[3 * i2 + 2]);
                        const bool flip = (a1 - a0).cross(a2 - a0).dot(out) < 0.0f;
                        tris.push_back(i0); tris.push_back(flip ? i2 : i1); tris.push_back(flip ? i1 : i2);
                    }
                }
                break;
            }
            case PxGeometryType::eTRIANGLEMESH: {
                const auto& t = static_cast<const PxTriangleMeshGeometry&>(g);
                const PxTriangleMesh* m = t.triangleMesh;
                const PxVec3* mv = m->getVertices();
                const int base = (int)verts.size() / 3;
                const PxMat33 sc = t.scale.toMat33();
                for (PxU32 i = 0; i < m->getNbVertices(); ++i) addVert(pose.transform(sc * mv[i]));
                const bool b16 = m->getTriangleMeshFlags() & PxTriangleMeshFlag::e16_BIT_INDICES;
                const void* ib = m->getTriangles();
                for (PxU32 i = 0; i < m->getNbTriangles(); ++i) {
                    int i0, i1, i2;
                    if (b16) { const PxU16* x = static_cast<const PxU16*>(ib) + 3 * i; i0 = x[0]; i1 = x[1]; i2 = x[2]; }
                    else     { const PxU32* x = static_cast<const PxU32*>(ib) + 3 * i; i0 = (int)x[0]; i1 = (int)x[1]; i2 = (int)x[2]; }
                    // Recast walks on triangles whose (v1-v0) x (v2-v0) points up; whichever way a mesh was
                    // wound, its floors are made to face up (an underside under solid geometry is never reached).
                    const PxVec3 a0(verts[3 * (base + i0)], verts[3 * (base + i0) + 1], verts[3 * (base + i0) + 2]);
                    const PxVec3 a1(verts[3 * (base + i1)], verts[3 * (base + i1) + 1], verts[3 * (base + i1) + 2]);
                    const PxVec3 a2(verts[3 * (base + i2)], verts[3 * (base + i2) + 1], verts[3 * (base + i2) + 2]);
                    const bool flip = (a1 - a0).cross(a2 - a0).y < 0.0f;
                    tris.push_back(base + i0);
                    tris.push_back(base + (flip ? i2 : i1));
                    tris.push_back(base + (flip ? i1 : i2));
                }
                break;
            }
            default: break; // planes, heightfields: none in the engine's scenes yet
            }
        }
    }
    return (int)tris.size() / 3 - before;
}

// --- NPC characters --------------------------------------------------------------------

ScopedQueryPolicy::ScopedQueryPolicy(unsigned ignoreEntity, bool hitPlayer, bool hitCharacters) {
    t_QueryPolicy.push_back(QueryPolicy{ignoreEntity, hitPlayer, hitCharacters});
}
ScopedQueryPolicy::~ScopedQueryPolicy() {
    if (!t_QueryPolicy.empty()) t_QueryPolicy.pop_back();
}

namespace {
using NpcCharacter = PhysicsState::NpcCharacter;
NpcCharacter* NpcSlot(CharacterId id) {
    if (!g_State || id < 0 || id >= (int)g_State->npcs.size()) return nullptr;
    NpcCharacter& n = g_State->npcs[(size_t)id];
    return n.Used() ? &n : nullptr;
}
CharacterBody* NpcBody(CharacterId id) {
    const NpcCharacter* n = NpcSlot(id);
    return n ? n->body.get() : nullptr;
}
PxRigidActor* NpcActor(CharacterId id) {
    const NpcCharacter* n = NpcSlot(id);
    return n ? n->Actor() : nullptr;
}
CharacterId AddNpcSlot(NpcCharacter&& n) {
    for (size_t i = 0; i < g_State->npcs.size(); ++i)
        if (!g_State->npcs[i].Used()) { g_State->npcs[i] = std::move(n); return (CharacterId)i; }
    g_State->npcs.push_back(std::move(n));
    return (CharacterId)g_State->npcs.size() - 1;
}
// Where a soldier's body is for everyone else's sweeps this same frame, as well as for the simulation: a kinematic target
// only lands at the next simulate, and two soldiers in one doorway each met the other where he had been a frame before.
void PlaceNpcBody(CharacterBody& b) {
    const PxTransform t(BodyCentre(b));
    b.Actor->setGlobalPose(t);
    b.Actor->setKinematicTarget(t);
}
} // namespace

CharacterId CreateNpcCharacter(unsigned entity, float radius, float cylinderHalfHeight, const float footPos[3]) {
    if (!g_State || radius <= 0.0f || cylinderHalfHeight <= 0.0f) return kNoCharacter;
    NpcCharacter n;
    n.entity = entity;
    n.body = MakeBody(*g_State, MoverCapsule{radius, cylinderHalfHeight}, glm::vec3(footPos[0], footPos[1], footPos[2]), entity,
                      PxFilterData(1u, 0u, 0u, 0u)); // the Default layer
    g_State->npcActors.insert(n.body->Actor);
    return AddNpcSlot(std::move(n));
}

void DestroyNpcCharacter(CharacterId id) {
    NpcCharacter* n = NpcSlot(id);
    if (!n) return;
    PxRigidActor* a = n->Actor();
    g_State->npcActors.erase(a);
    // The Player standing on this soldier's capsule (a kinematic: his ground, MoveCharacter carries him with it) lets go of it
    // first - released, the next move read the freed actor.
    if (a && g_State->playerGround == a) g_State->playerGround = nullptr;
    if (n->body) n->body->Actor->release();
    *n = NpcCharacter{};
}

bool IgnoreRagdollForNpc(CharacterId id, int ragdoll) {
    PxRigidActor* a = NpcActor(id);
    if (!a) return false;
    const bool on = ValidRagdoll(ragdoll);
    std::vector<PxShape*> shapes(a->getNbShapes());
    a->getShapes(shapes.data(), (PxU32)shapes.size());
    for (PxShape* sh : shapes) {
        PxFilterData f = sh->getSimulationFilterData();
        // As IgnoreRagdollParts: the ignorer slot of the ragdoll's own numbering, every part's bit in the mask.
        f.word2 = on ? (((PxU32)ragdoll + 1u) | (kRagdollIgnorerSlot << 16)) : 0u;
        f.word3 = on ? 0xFFFFFFFFu : 0u;
        sh->setSimulationFilterData(f);
    }
    if (a->getScene()) g_State->scene->resetFiltering(*a);
    return true;
}

void SetNpcPassesRagdolls(CharacterId id, bool on) {
    PxRigidActor* a = NpcActor(id);
    if (!a || g_State->npcs[(size_t)id].passRagdolls == on) return;
    g_State->npcs[(size_t)id].passRagdolls = on;
    std::vector<PxShape*> shapes(a->getNbShapes());
    a->getShapes(shapes.data(), (PxU32)shapes.size());
    for (PxShape* sh : shapes) {
        PxFilterData f = sh->getSimulationFilterData();
        f.word1 = on ? (f.word1 | kPassRagdolls) : (f.word1 & ~kPassRagdolls);
        sh->setSimulationFilterData(f);
    }
    if (a->getScene()) g_State->scene->resetFiltering(*a);
}

unsigned MoveNpcCharacter(CharacterId id, const float disp[3], float dt) {
    NpcCharacter* n = NpcSlot(id);
    if (!n || dt <= 0.0f) return 0;
    if (CharacterBody* b = n->body.get()) {
        // The same collision-matrix row, his own hitboxes and (crawling) the dead skipped, as the Default layer's body.
        PhysxMoverWorld w(*g_State, b->Actor, ProjectSettings::Physics().LayerCollisionMask[0]);
        w.SelfEntity = n->entity;
        w.PassRagdolls = n->passRagdolls;
        const glm::vec3 delta(disp[0], disp[1], disp[2]);
        // Walking keeps to the floor down steps and slopes; anything going up is left to his own gravity.
        const bool snap = b->Grounded && delta.y <= 0.0f;
        const bool debug = (g_DebugDrawFlags & PDD_Movers) != 0u;
        b->Last = CharacterMover::Move(w, b->Cap, b->Feet, delta / dt, delta, ProjectMoverSettings(), b->Grounded, snap,
                                       debug ? &b->Debug : nullptr);
        if (!debug) b->Debug.Clear();
        b->Feet = b->Last.Feet;
        b->Grounded = b->Last.Grounded;
        PlaceNpcBody(*b);
        return b->Last.Flags;
    }
    return 0;
}

void SetNpcFootPosition(CharacterId id, const float footPos[3]) {
    if (CharacterBody* b = NpcBody(id)) {
        b->Feet = glm::vec3(footPos[0], footPos[1], footPos[2]);
        PlaceNpcBody(*b);
    }
}

bool GetNpcCapsule(CharacterId id, float outFootPos[3], float* outRadius, float* outCylHalfHeight) {
    if (const CharacterBody* b = NpcBody(id)) {
        outFootPos[0] = b->Feet.x; outFootPos[1] = b->Feet.y; outFootPos[2] = b->Feet.z;
        if (outRadius)        *outRadius = b->Cap.Radius;
        if (outCylHalfHeight) *outCylHalfHeight = b->Cap.HalfHeight;
        return true;
    }
    return false;
}

bool ResizeNpcCharacter(CharacterId id, float cylinderHalfHeight) {
    if (cylinderHalfHeight <= 0.0f) return false;
    if (CharacterBody* b = NpcBody(id)) {
        ResizeBody(*g_State, *b, cylinderHalfHeight);
        PlaceNpcBody(*b);
        return true;
    }
    return false;
}

bool NpcFitsAt(CharacterId id, float cylinderHalfHeight) {
    if (!NpcSlot(id) || cylinderHalfHeight <= 0.0f) return false;
    float foot[3], capRadius = 0.0f;
    GetNpcCapsule(id, foot, &capRadius, nullptr);
    const float radius = std::max(0.02f, capRadius - 0.03f);
    const PxVec3 centre(foot[0], foot[1] + 0.04f + cylinderHalfHeight + radius, foot[2]);
    const PxQuat lieAlongY(PxPi * 0.5f, PxVec3(0.0f, 0.0f, 1.0f));
    ScopedQueryPolicy self(g_State->npcs[(size_t)id].entity, /*hitPlayer=*/true);
    QueryFilter f;
    f.HitTriggers = 0;
    return OverlapFiltered(PxCapsuleGeometry(radius, cylinderHalfHeight), PxTransform(centre, lieAlongY), f, nullptr, 0) == 0;
}

CharacterId NpcCharacterOf(unsigned entity) {
    if (!g_State) return kNoCharacter;
    for (size_t i = 0; i < g_State->npcs.size(); ++i)
        if (g_State->npcs[i].Used() && g_State->npcs[i].entity == entity) return (CharacterId)i;
    return kNoCharacter;
}

int BoxOverlapsCharacters(const float centre[3], const float halfExtents[3], const float rotation[4]) {
    if (!g_State || !g_State->scene) return 0;
    struct CharactersOnly : PxQueryFilterCallback {
        const PxRigidActor* Player = nullptr;
        PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape*, const PxRigidActor* actor, PxHitFlags&) override {
            return actor && (actor == Player || g_State->npcActors.count(actor)) ? PxQueryHitType::eTOUCH : PxQueryHitType::eNONE;
        }
        PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override {
            return PxQueryHitType::eTOUCH;
        }
    } f;
    f.Player = g_State->playerBody ? static_cast<const PxRigidActor*>(g_State->playerBody->Actor) : nullptr;
    const PxVec3 he(std::max(halfExtents[0], 1e-4f), std::max(halfExtents[1], 1e-4f), std::max(halfExtents[2], 1e-4f));
    PxOverlapHit hits[8];
    PxOverlapBuffer buf(hits, 8);
    const PxQueryFilterData fd(PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER | PxQueryFlag::eNO_BLOCK);
    g_State->scene->overlap(PxBoxGeometry(he), PxTransform(PxVec3(centre[0], centre[1], centre[2]), ToQuat(rotation)), buf, fd, &f);
    return (int)buf.getNbTouches();
}

// --- NPC hitboxes ----------------------------------------------------------------------

namespace {
PxTransform HitboxPose(const HitCapsule& c) {
    return PxTransform(PxVec3(c.Position[0], c.Position[1], c.Position[2]), ToQuat(c.Rotation));
}
void ShadowCapsule(PhysicsState::Hitboxes& h, bool on) {
    if (!g_State || !h.Capsule) return;
    if (on) g_State->hitboxedCapsules.insert(h.Capsule);
    else g_State->hitboxedCapsules.erase(h.Capsule);
}
} // namespace

namespace {
// Kinematic query-only capsules reporting `entity`; `capsule` (may be null) is the character capsule they stand in for.
int MakeHitboxes(PxRigidActor* capsule, std::uint32_t entity, const HitCapsule* parts, int count) {
    PhysicsState& s = *g_State;
    int id = -1;
    for (size_t i = 0; i < s.hitboxes.size(); ++i)
        if (s.hitboxes[i].Bodies.empty()) { id = (int)i; break; }
    if (id < 0) { id = (int)s.hitboxes.size(); s.hitboxes.emplace_back(); }
    PhysicsState::Hitboxes& h = s.hitboxes[(size_t)id];
    h.Capsule = capsule;
    for (int i = 0; i < count; ++i) {
        PxRigidDynamic* b = s.physics->createRigidDynamic(HitboxPose(parts[i]));
        PxShape* sh = PxRigidActorExt::createExclusiveShape(
            *b, PxCapsuleGeometry(std::max(parts[i].Radius, 0.01f), std::max(parts[i].HalfLength, 0.005f)), *s.defaultMaterial);
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
    }
    h.Active = true;
    ShadowCapsule(h, true);
    return id;
}
} // namespace

int CreateNpcHitboxes(CharacterId owner, const HitCapsule* parts, int count) {
    PxRigidActor* capsule = NpcActor(owner);
    if (!g_State || !g_State->scene || !capsule || !parts || count <= 0) return -1;
    return MakeHitboxes(capsule, g_State->npcs[(size_t)owner].entity, parts, count);
}

int CreateBodyHitboxes(unsigned entity, const HitCapsule* parts, int count) {
    if (!g_State || !g_State->scene || !parts || count <= 0) return -1;
    return MakeHitboxes(nullptr, entity, parts, count);
}

void SetNpcHitboxPoses(int id, const HitCapsule* parts, int count) {
    if (!g_State || !parts || id < 0 || id >= (int)g_State->hitboxes.size()) return;
    auto& h = g_State->hitboxes[(size_t)id];
    for (int i = 0; i < count && i < (int)h.Bodies.size(); ++i) h.Bodies[(size_t)i]->setGlobalPose(HitboxPose(parts[i]), false);
}

void SetNpcHitboxesActive(int id, bool active) {
    if (!g_State || id < 0 || id >= (int)g_State->hitboxes.size()) return;
    auto& h = g_State->hitboxes[(size_t)id];
    if (h.Bodies.empty() || h.Active == active) return;
    h.Active = active;
    for (PxRigidDynamic* b : h.Bodies) {
        PxShape* sh = nullptr;
        if (b->getShapes(&sh, 1) == 1 && sh) sh->setFlag(PxShapeFlag::eSCENE_QUERY_SHAPE, active);
    }
    ShadowCapsule(h, active);
}

void DestroyNpcHitboxes(int id) {
    if (!g_State || id < 0 || id >= (int)g_State->hitboxes.size()) return;
    auto& h = g_State->hitboxes[(size_t)id];
    ShadowCapsule(h, false);
    for (PxRigidDynamic* b : h.Bodies)
        if (b) {
            if (g_State->playerGround == b) g_State->playerGround = nullptr;
            g_State->hitboxActors.erase(b);
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
    s.TimeScale = Time::EffectiveTimeScale();
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

void SetEventLogging(bool enabled) { g_LogEvents = enabled; }

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

    // --- The custom character mover's last move (the Player's and every soldier's): what it slid along, the floor, the
    // steps it tried.
    auto drawMove = [&](const MoverDebug& d) {
        for (const auto& pl : d.Planes) {
            const float r = pl.Kind == 2 ? 1.0f : 0.0f, g = pl.Kind == 1 ? 1.0f : 0.3f, b = pl.Kind == 0 ? 1.0f : 0.0f;
            const PxVec3 at = ToPx(pl.Point), nrm = ToPx(pl.Normal);
            PxVec3 u, v;
            basis(nrm, u, v);
            ring(at, u, v, 0.12f, 12, r, g, b, 0.9f);
            arrow(at, at + nrm * 0.25f, r, g, b, 0.9f);
        }
        for (size_t i = 0; i + 1 < d.Creases.size(); i += 2) arrow(ToPx(d.Creases[i]), ToPx(d.Creases[i + 1]), 1.0f, 1.0f, 0.0f, 1.0f);
        for (const auto& st : d.Steps) {
            const PxVec3 at = ToPx(st.At);
            L(at, at + PxVec3(0.0f, std::max(st.Headroom, 0.01f), 0.0f), st.Taken ? 0.0f : 1.0f, st.Taken ? 1.0f : 0.0f, 0.0f, 1.0f);
        }
        for (size_t i = 0; i + 1 < d.Nudges.size(); i += 2) arrow(ToPx(d.Nudges[i]), ToPx(d.Nudges[i + 1]), 0.0f, 1.0f, 1.0f, 1.0f);
        if (d.Floor.Found) {
            const PxVec3 at = ToPx(d.Floor.Hit.Point);
            const float r = d.Floor.Walkable ? 0.0f : 1.0f, g = d.Floor.Walkable ? 1.0f : d.Floor.Perched ? 0.5f : 0.0f;
            ring(at, PxVec3(1, 0, 0), PxVec3(0, 0, 1), 0.06f, 10, r, g, 0.0f, 1.0f);
            L(at, ToPx(d.Feet), r, g, 0.0f, 0.6f);
        }
    };
    if (f & PDD_Movers) {
        if (g_State->playerBody) drawMove(g_State->playerBody->Debug);
        for (const auto& npc : g_State->npcs)
            if (npc.body) drawMove(npc.body->Debug);
    }
    return n;
}


void StepOneSubstep(World& world) {
    if (!g_State || !g_State->scene) return;
    ++g_EventSequence;
    g_State->triggerEvents.clear();
    g_State->enteredThisFrame.clear();
    g_State->contactEvents.clear();
    g_State->kinematicFramePose.BeginRefill();
    for (auto& [body, e] : g_State->kinematics) {
        if (!world.Registry.valid(e)) continue;
        if (world.Registry.all_of<TransformComponent>(e)) {
            const TransformComponent w = world.WorldSpaceTransform(e); // #114
            const PxTransform target(ToPx(w.Position), QuatToPx(w.Rotation));
            body->setKinematicTarget(target);
            g_State->kinematicFramePose.Set(body, target); // #168
        }
    }
    RunOneSubstep(*g_State, FixedStep());
    g_State->lastSubsteps = 1;
    for (auto& [body, e] : g_State->dynamics) {
        if (!world.Registry.valid(e)) continue;
        auto* tc = world.Registry.try_get<TransformComponent>(e);
        if (!tc) continue;
        const PxTransform p = body->getGlobalPose();
        if (!PoseIsFinite(p)) continue;
        world.SetWorldPose(e, glm::vec3(p.p.x, p.p.y, p.p.z), glm::quat(p.q.w, p.q.x, p.q.y, p.q.z)); // #114
    }
}

void SetQueryRecording(bool on) { g_QueryRecording = on; if (!on) { g_QueryCount = 0; g_QueryHead = 0; } }
bool GetQueryRecording()        { return g_QueryRecording; }

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
    g_State->grabHasTarget = false;
    g_State->grabHasRotation = false;
    g_State->grabSavedAngularDamping  = b->getAngularDamping();
    g_State->grabSavedGravityDisabled = b->getActorFlags().isSet(PxActorFlag::eDISABLE_GRAVITY);
    b->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, true);
    b->setAngularDamping(8.0f);
    b->wakeUp();
}

bool IsGrabbing() { return g_State && g_State->grabbed != nullptr; }

void UpdateGrab(const float target[3], const float targetVelocity[3], const float targetRotation[4]) {
    if (!g_State || !g_State->grabbed) return;
    g_State->grabHasTarget = true;
    g_State->grabTarget = PxVec3(target[0], target[1], target[2]);
    g_State->grabTargetVelocity = targetVelocity ? PxVec3(targetVelocity[0], targetVelocity[1], targetVelocity[2])
                                                 : PxVec3(0.0f);
    if (!g_State->grabTargetVelocity.isFinite()) g_State->grabTargetVelocity = PxVec3(0.0f);
    g_State->grabHasRotation = targetRotation != nullptr;
    if (targetRotation) {
        PxQuat q(targetRotation[0], targetRotation[1], targetRotation[2], targetRotation[3]);
        g_State->grabHasRotation = q.isFinite() && q.magnitudeSquared() > 1e-6f;
        if (g_State->grabHasRotation) g_State->grabTargetRotation = q.getNormalized();
    }
}

void ReleaseBody(bool launch, const float impulse[3], float backspinRadPerSec) {
    if (!g_State || !g_State->grabbed) return;
    PxRigidDynamic* b = g_State->grabbed;
    b->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, g_State->grabSavedGravityDisabled);
    b->setAngularDamping(g_State->grabSavedAngularDamping);
    g_State->grabHasTarget = false;
    if (launch) {
        const PxVec3 v(impulse[0], impulse[1], impulse[2]);
        b->setLinearVelocity(PxVec3(0.0f));
        b->setAngularVelocity(PxVec3(0.0f)); // a held body turning toward its hold rotation
        b->addForce(v, PxForceMode::eVELOCITY_CHANGE, /*autowake=*/true);
        // Backspin: angular velocity about the throw's right axis (throw x up), which turns the
        // top of the ball back toward the thrower. Round bodies only - a spun crate just tumbles.
        PxShape* shape = nullptr;
        const bool round = backspinRadPerSec > 0.0f && b->getNbShapes() > 0 && b->getShapes(&shape, 1) == 1 &&
                           shape->getGeometry().getType() == PxGeometryType::eSPHERE;
        const PxVec3 right = v.cross(PxVec3(0.0f, 1.0f, 0.0f));
        if (round && right.magnitude() > 1e-4f) {
            b->setMaxAngularVelocity(std::max(b->getMaxAngularVelocity(), backspinRadPerSec * 1.5f));
            b->setAngularVelocity(right.getNormalized() * backspinRadPerSec);
        }
    }
    g_State->grabbed = nullptr;
}

unsigned GrabbedEntity() {
    if (!g_State || !g_State->grabbed) return 0xFFFFFFFFu;
    for (const auto& [e, body] : g_State->bodyByEntity)
        if (body == g_State->grabbed) return e;
    return 0xFFFFFFFFu;
}

bool GetActorRotation(unsigned entity, float out[4]) {
    if (!g_State) return false;
    PxRigidActor* a = nullptr;
    if (auto it = g_State->bodyByEntity.find(entity); it != g_State->bodyByEntity.end()) a = it->second;
    else if (auto st = g_State->staticByEntity.find(entity); st != g_State->staticByEntity.end()) a = st->second;
    if (!a) return false;
    const PxQuat q = a->getGlobalPose().q;
    out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
    return true;
}

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
    PxRigidActor* a = nullptr;
    if (auto it = g_State->bodyByEntity.find(entity); it != g_State->bodyByEntity.end()) a = it->second;
    else if (auto st = g_State->staticByEntity.find(entity); st != g_State->staticByEntity.end()) a = st->second;
    if (!a) return false;
    const PxVec3 p = a->getGlobalPose().p;
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
    return true;
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
