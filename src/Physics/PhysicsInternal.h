#pragma once

// Private to the src/Physics/Physics*.cpp files: the shared state and helpers that used to sit in PhysicsWorld.cpp
// behind one anonymous namespace. Not for use outside the physics module.

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

namespace PhysicsWorld::detail {

struct PhysicsState;

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
    ~PhysicsCore(); // releases everything above in order (see PhysicsWorld.cpp)
};

struct PhysicsState {
    ~PhysicsState(); // releases the joints, ragdoll PhysX objects and the scene, in that order (see PhysicsWorld.cpp)
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
    // Of those, a creature body's (CreateBodyHitboxes: a spider's, with no capsule): for shots only - they never push or
    // block a character's mover.
    std::unordered_set<const PxRigidActor*> bodyHitboxActors;
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
    // ReleaseBody's backspin raises a ball's max angular velocity; the old cap comes back on the next grab or when the body is removed.
    PxRigidDynamic* spunBody = nullptr;
    float           spunSavedMaxAngVel = 0.0f;
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
extern PhysicsState* g_State;
constexpr int   kMaxSubSteps      = 4;

// --- #185 debug tooling: settings + history that OUTLIVE a Play session -----------------
// The editor sets these from the Physics panel; they must survive Play->Stop->Play, so they
// can't live in PhysicsState.
extern unsigned g_DebugDrawFlags;
extern bool g_QueryRecording;

// Recent contact points, for the fading impact sparks (PDD_Contacts). Appended in onContact,
// aged each Step, drawn + culled in CopyDebugLines.
struct ContactViz {
    PxVec3 p{0.0f}, n{0.0f, 1.0f, 0.0f};
    float  impulse = 0.0f;
    float  age = 0.0f;      // seconds since the touch; culled past kContactFade
    bool   impact = false;  // a real hit (bright burst) vs. a gentle new touch
};
constexpr int   kContactRing = 64;
extern ContactViz g_Contacts[kContactRing];
extern int g_ContactHead, g_ContactCount;

// Recent scene queries, for PDD_Raycasts.
struct QueryRecord {
    int    kind = 0;             // 0 ray, 1 sweep(sphere), 2 overlap(sphere)
    PxVec3 origin{0.0f}, dir{0.0f}, hitPoint{0.0f}, hitNormal{0.0f};
    float  dist = 0.0f, radius = 0.0f, age = 0.0f;
    bool   hit = false;
};
constexpr int   kQueryRing = 64;
extern QueryRecord g_Queries[kQueryRing];
extern int g_QueryHead, g_QueryCount;

void PushContactViz(const PxVec3& p, const PxVec3& n, float impulse, bool impact);

void RecordQuery(const QueryRecord& q);

// Sim step comes from the project setting (#236 A4 stored it; #185 is the first consumer).
// Guard against a zero/negative value in settings.json turning the loop below infinite.
float FixedStep();

// --- Actor build (#185 PR 2 statics, PR 4 dynamics) --------------------------------------

inline PxVec3 ToPx(const glm::vec3& v) { return PxVec3(v.x, v.y, v.z); }

inline PxQuat QuatToPx(const glm::quat& rotation) {
    const glm::quat q = NormalizeRotation(rotation);
    return PxQuat(q.x, q.y, q.z, q.w);
}

// entt::entity id <-> the void* PhysX stores per actor. A hit always comes back with an actor,
// and every actor in our scene is one of ours, so the round-trip needs no sentinel: entity 0
// stores as nullptr and reads back as 0, which is a valid entity the caller can compare.
inline void* EntityToUserData(entt::entity e) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(entt::to_integral(e)));
}
inline std::uint32_t UserDataToEntity(void* p) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p));
}

using PhysicsWorld::kBodyPartHitbox;
using PhysicsWorld::kBodyPartRagdoll;
// A body part's shape tag (see BodyPartKind): (kind << 16) | part index; untagged shapes hold nullptr.
inline void* BodyPartTag(unsigned kind, unsigned part) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>((kind << 16) | part));
}
inline unsigned BodyPartKindOf(const void* tag) { return (unsigned)(reinterpret_cast<std::uintptr_t>(tag) >> 16); }

// `only` (#166 / #201): build just these entities - ones spawned, re-activated or given a
// Collider while playing (see SyncEntities). Null = every active collider (Play-enter).
void BuildActors(PhysicsState& s, const World& world, const std::unordered_set<std::uint32_t>* only = nullptr);

// #166 / #201 - keeps the PhysX scene in step with the World while playing: actors go away for
// entities that were destroyed, deactivated or lost their Collider, and are built for ones that
// were spawned, re-activated or given a Collider. (Before, Play-enter was the only sync, so a
// deleted or deactivated object left an invisible collider behind and a duplicated one fell
// through nothing.)
// Joints are not re-made here: BuildJoints walks every JointComponent at Play-enter and would
// duplicate the existing ones, so an entity that gains a collider mid-Play gets an actor but no
// joint. Nothing can spawn one yet; revisit with Instantiate (#166).
void SyncEntities(PhysicsState& s, const World& world);

// The static or dynamic/kinematic actor built for `entity` (dynamic first), or null.
PxRigidActor* ActorFor(const PhysicsState& s, std::uint32_t entity);

// All of an actor's shapes (a vector sized by getNbShapes, filled by getShapes).
std::vector<PxShape*> ShapesOf(const PxRigidActor& actor);

void BuildJoints(PhysicsState& s, const World& world);

// --- Triggers (#185 PR 5) --------------------------------------------------------------

const char* EntityLabel(const PhysicsState& s, std::uint32_t e, char buf[24]);

extern bool g_LogEvents;

void PushTriggerEvent(PhysicsState& s, std::uint32_t kind, std::uint32_t trig, std::uint32_t other);

// Closing speed of the two actors along `n`, from the velocities captured just before the
// step (see PhysicsState::preStepVel) — so it reflects the actual impact, not the post-solve
// resting state.
float NormalClosingSpeed(PhysicsState& s, const PxActor* a0, const PxActor* a1, const PxVec3& n);

// #185 PR 10 — the Player capsule hit something while moving. Push a dynamic body out of the
// way (scaled so light things fly and heavy things barely budge), and if the thing is a
// kinematic body underfoot, remember it so MoveCharacter can carry the Player along with it.
// The pose a moving platform is carried by: this frame's authored target (#168), else the
// simulated pose (a kinematic with no Transform-driven target).
inline PxTransform PlatformPose(const PhysicsState& s, const PxRigidDynamic* body) {
    const PxTransform* p = s.kinematicFramePose.Find(body);
    return p ? *p : body->getGlobalPose();
}

void CollectPlayerTriggers(std::set<std::uint32_t>& out);

// The gravity gun's servo, run before every substep: the body's velocity becomes the hold
// point's own velocity (feed-forward: walking or turning with it doesn't leave it trailing) plus
// a correction closing the remaining gap with a ~1/12 s time constant; its angular velocity
// turns it toward the hold orientation the same way.
void DriveGrab(PhysicsState& s, float dt);

// #185 debug — reject a NaN/inf simulated pose instead of poisoning the TransformComponent
// (which then spreads through the world-transform cache and the renderer). Freezes the body.
bool PoseIsFinite(const PxTransform& p);
const PxRigidActor* PlayerActor();

// --- The custom character mover ------------------------------------------------------------
using CharacterBody = PhysicsState::CharacterBody;

bool FiniteVec3(const float v[3]);

// --- Forces & read-back (#185 PR 7) --------------------------------------------------------

// A non-kinematic dynamic body for `entity`, or null. bodyByEntity holds kinematics too, so
// filter those out here — force/velocity calls on a kinematic body are meaningless.
PxRigidDynamic* DynamicFor(unsigned entity);

bool NormalizedDir(const float dir[3], PxVec3& out);

PxQuat ToQuat(const float rotation[4]);

int OverlapFiltered(const PxGeometry& geom, const PxTransform& pose, const QueryFilter& f,
                    unsigned* out, int maxEntities);

// Once a frame: corpses asleep a while freeze; the ones walked into thaw.
void FreezeRestingRagdolls(PhysicsState& s, float dt);

// The live ragdoll `id` (a valid index whose articulation exists), or null. The no-state overload looks in g_State.
PhysicsState::Ragdoll* RagdollAt(PhysicsState& s, int id);
PhysicsState::Ragdoll* RagdollAt(int id);
bool ValidRagdoll(int ragdoll); // RagdollAt(ragdoll) != nullptr

// Before every substep: the extra gravity, and pairs that overlapped at build let collide once they are apart.
void RagdollAfterSubstep(PhysicsState::Ragdoll& rd);

void RagdollBeforeSubstep(PhysicsState& s, PhysicsState::Ragdoll& rd);

using NpcCharacter = PhysicsState::NpcCharacter;
PxRigidActor* NpcActor(CharacterId id);

} // namespace PhysicsWorld::detail

using namespace PhysicsWorld::detail;
