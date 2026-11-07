#include "PhysicsInternal.h"

namespace {
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
} // namespace

namespace PhysicsWorld {
// --- Player character ---------------------------------------------------------------

bool HasCharacter() {
    return g_State && g_State->playerBody;
}
} // namespace PhysicsWorld

namespace PhysicsWorld {
namespace {
const PxQuat kCapsuleUp(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f)); // PhysX capsules lie along X

// The CC_ flags (PhysicsWorld.h) are CharacterMover's MoverFlag values; MoveCharacter returns them unconverted.
static_assert((unsigned)CC_SIDES == (unsigned)MoverSides && (unsigned)CC_UP == (unsigned)MoverUp && (unsigned)CC_DOWN == (unsigned)MoverDown, "CC_ flags must match MoverFlag");

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
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld::detail {
bool FiniteVec3(const float v[3]) {
    return v && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
namespace {
PxShape* MakeBodyShape(PhysicsState& s, PxRigidDynamic& a, const MoverCapsule& cap, const PxFilterData& sim) {
    PxShape* sh = s.defaultMaterial ? PxRigidActorExt::createExclusiveShape(a, PxCapsuleGeometry(cap.Radius, cap.HalfHeight), *s.defaultMaterial)
                                    : nullptr;
    if (!sh) return nullptr;
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
    if (!b->Actor) {
        Log::Error("PhysX: createRigidDynamic failed for a character capsule.");
        return nullptr;
    }
    b->Actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
    b->Shape = MakeBodyShape(s, *b->Actor, cap, sim);
    if (!b->Shape) {
        Log::Error("PhysX: creating a character capsule's shape failed.");
        b->Actor->release();
        b->Actor = nullptr;
        return nullptr;
    }
    b->Actor->userData = reinterpret_cast<void*>(static_cast<std::uintptr_t>(entity));
    s.scene->addActor(*b->Actor);
    return b;
}

// A new cylinder length (crouch / stand), the feet where they were; the shape's filter data and userData carry over.
void ResizeBody(PhysicsState& s, CharacterBody& b, float cylinderHalfHeight) {
    const PxFilterData sim = b.Shape->getSimulationFilterData();
    MoverCapsule cap = b.Cap;
    cap.HalfHeight = cylinderHalfHeight;
    PxShape* fresh = MakeBodyShape(s, *b.Actor, cap, sim); // made before the old one goes, so a failure leaves the capsule as it was
    if (!fresh) { Log::Error("PhysX: resizing a character capsule failed."); return; }
    fresh->setQueryFilterData(b.Shape->getQueryFilterData());
    fresh->userData = b.Shape->userData;
    b.Actor->detachShape(*b.Shape);
    b.Cap = cap;
    b.Shape = fresh;
}

// Where a character's body is for everyone else's sweeps this same frame, as well as for the simulation: a kinematic target
// only lands at the next simulate, and two soldiers in one doorway each met the other where he had been a frame before.
// setGlobalPose is a deliberate teleport here (the feet stay put while the capsule's centre moves with its length, or a
// body is put down at new feet); the target is set to the same pose so the last move's target can't pull it back.
void PlaceBody(CharacterBody& b) {
    const PxTransform t(BodyCentre(b));
    b.Actor->setGlobalPose(t);
    b.Actor->setKinematicTarget(t);
}

// A mover step for either kind of character: sweeps through `w`, records the result and the debug trail, moves the feet.
void RunMover(CharacterBody& b, IMoverWorld& w, const glm::vec3& velocity, const glm::vec3& delta, const MoverSettings& ms,
              bool snapToGround) {
    const bool debug = (g_DebugDrawFlags & PDD_Movers) != 0u;
    b.Last = CharacterMover::Move(w, b.Cap, b.Feet, velocity, delta, ms, b.Grounded, snapToGround, debug ? &b.Debug : nullptr);
    if (!debug) b.Debug.Clear();
    b.Feet = b.Last.Feet;
    b.Grounded = b.Last.Grounded;
}

// Whether a capsule of this cylinder length would fit standing at `foot` (its radius `capRadius`).
bool CapsuleFitsAt(const glm::vec3& foot, float capRadius, float cylinderHalfHeight) {
    // A hair smaller and lifted, so the floor under the feet and a wall alongside don't count.
    const float radius = std::max(0.02f, capRadius - 0.03f);
    const PxVec3 centre(foot.x, foot.y + 0.04f + cylinderHalfHeight + radius, foot.z);
    QueryFilter f;
    f.HitTriggers = 0;
    return OverlapFiltered(PxCapsuleGeometry(radius, cylinderHalfHeight), PxTransform(centre, kCapsuleUp), f, nullptr, 0) == 0;
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
            // A spider's hitboxes: a biting spider against the player's legs slid them away every frame it held there.
            if (actor && W->S.bodyHitboxActors.count(actor)) return PxQueryHitType::eNONE;
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
        // The touch buffer grows until PhysX reports every overlap (a fixed 16 dropped the deepest ones in a crowd).
        static thread_local std::vector<PxOverlapHit> hits(16);
        PxOverlapBuffer buf;
        for (;;) {
            buf = PxOverlapBuffer(hits.data(), (PxU32)hits.size());
            if (!S.scene->overlap(geom, pose, buf, fd, &f)) return false;
            if (buf.getNbTouches() >= (PxU32)hits.size() && hits.size() < 4096) { hits.resize(hits.size() * 2); continue; }
            break;
        }
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
} // namespace PhysicsWorld

namespace PhysicsWorld {
void CreateCharacter(float radius, float cylinderHalfHeight, const float footPos[3]) {
    if (!g_State || HasCharacter()) return;
    if (radius <= 0.0f || cylinderHalfHeight <= 0.0f) return;
    const PxU32 pl = (PxU32)std::clamp(ProjectSettings::Physics().PlayerLayer, 0, 31);
    if (!FiniteVec3(footPos)) return;
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
        MoverSettings ms = ProjectMoverSettings();
        ms.DoorAssist = g_State->playerDoorAssist;
        ms.DoorAssistMaxAngle = g_State->playerDoorAssistMaxAngle;
        RunMover(*b, w, velocity, delta, ms, snapToGround);
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

float PlatformYawDelta() {
    return g_State ? g_State->platformYawDeltaDeg : 0.0f;
}

bool ResizeCharacter(float cylinderHalfHeight) {
    if (!HasCharacter() || cylinderHalfHeight <= 0.0f) return false;
    if (CharacterBody* b = g_State->playerBody.get()) {
        ResizeBody(*g_State, *b, cylinderHalfHeight);
        PlaceBody(*b); // the feet stay put
        return true;
    }
    return false;
}

bool CharacterFitsAt(float cylinderHalfHeight) {
    if (!HasCharacter() || cylinderHalfHeight <= 0.0f) return false;
    const CharacterBody* b = g_State->playerBody.get();
    return CapsuleFitsAt(b->Feet, b->Cap.Radius, cylinderHalfHeight);
}
} // namespace PhysicsWorld

namespace PhysicsWorld {
namespace {
// --- NPC characters --------------------------------------------------------------------
NpcCharacter* NpcSlot(CharacterId id) {
    NpcCharacter* n = g_State ? HandleTarget(g_State->npcs, id) : nullptr;
    return n && n->Used() ? n : nullptr;
}
CharacterBody* NpcBody(CharacterId id) {
    const NpcCharacter* n = NpcSlot(id);
    return n ? n->body.get() : nullptr;
}
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld::detail {
PxRigidActor* NpcActor(CharacterId id) {
    const NpcCharacter* n = NpcSlot(id);
    return n ? n->Actor() : nullptr;
}
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
namespace {
CharacterId AddNpcSlot(NpcCharacter&& n) {
    const int slot = AcquireSlot(g_State->npcs, [](const NpcCharacter& x) { return !x.Used(); });
    if (slot < 0) return kNoCharacter;
    NpcCharacter& dst = g_State->npcs[(size_t)slot];
    n.Gen = NextGen(dst.Gen);
    dst = std::move(n);
    return MakeHandle((size_t)slot, dst.Gen);
}
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld {
CharacterId CreateNpcCharacter(unsigned entity, float radius, float cylinderHalfHeight, const float footPos[3]) {
    if (!g_State || !g_State->scene || !(radius > 0.0f) || !(cylinderHalfHeight > 0.0f) || !FiniteVec3(footPos)) return kNoCharacter;
    NpcCharacter n;
    n.entity = entity;
    n.body = MakeBody(*g_State, MoverCapsule{radius, cylinderHalfHeight}, glm::vec3(footPos[0], footPos[1], footPos[2]), entity,
                      PxFilterData(1u, 0u, 0u, 0u)); // the Default layer
    if (!n.body) return kNoCharacter; // (MakeBody logged why)
    PxRigidDynamic* actor = n.body->Actor;
    const CharacterId id = AddNpcSlot(std::move(n));
    if (id == kNoCharacter) { Log::Error("PhysX: out of NPC character slots."); actor->release(); return kNoCharacter; }
    g_State->npcActors.insert(actor);
    return id;
}

void DestroyNpcCharacter(CharacterId id) {
    NpcCharacter* n = NpcSlot(id);
    if (!n) return;
    PxRigidActor* a = n->Actor();
    g_State->npcActors.erase(a);
    // The Player standing on this soldier's capsule (a kinematic: his ground, MoveCharacter carries him with it) lets go of it
    // first - released, the next move read the freed actor.
    if (a && g_State->playerGround == a) g_State->playerGround = nullptr;
    ForgetHitboxCapsule(*g_State, a);
    ReleaseRagdollAttachments(*g_State, a);
    if (n->body) n->body->Actor->release();
    const std::uint16_t gen = n->Gen;
    *n = NpcCharacter{};
    n->Gen = gen;
}

bool IgnoreRagdollForNpc(CharacterId id, int ragdoll) {
    PxRigidActor* a = NpcActor(id);
    if (!a) return false;
    const bool on = ValidRagdoll(ragdoll);
    std::vector<PxShape*> shapes = ShapesOf(*a);
    for (PxShape* sh : shapes) {
        PxFilterData f = sh->getSimulationFilterData();
        // As IgnoreRagdollParts: the ignorer slot of the ragdoll's own numbering, every part's bit in the mask.
        f.word2 = on ? (((PxU32)HandleSlot(ragdoll) + 1u) | (kRagdollIgnorerSlot << 16)) : 0u;
        f.word3 = on ? 0xFFFFFFFFu : 0u;
        sh->setSimulationFilterData(f);
    }
    if (a->getScene()) g_State->scene->resetFiltering(*a);
    return true;
}

void SetNpcPassesRagdolls(CharacterId id, bool on) {
    PxRigidActor* a = NpcActor(id);
    if (!a || NpcSlot(id)->passRagdolls == on) return;
    NpcSlot(id)->passRagdolls = on;
    std::vector<PxShape*> shapes = ShapesOf(*a);
    for (PxShape* sh : shapes) {
        PxFilterData f = sh->getSimulationFilterData();
        f.word1 = on ? (f.word1 | kPassRagdolls) : (f.word1 & ~kPassRagdolls);
        sh->setSimulationFilterData(f);
    }
    if (a->getScene()) g_State->scene->resetFiltering(*a);
}

unsigned MoveNpcCharacter(CharacterId id, const float disp[3], float dt) {
    NpcCharacter* n = NpcSlot(id);
    if (!n || !(dt > 0.0f) || !std::isfinite(dt) || !FiniteVec3(disp)) return 0; // a NaN move is dropped: the feet stay at the last good ones
    if (CharacterBody* b = n->body.get()) {
        // The same collision-matrix row, his own hitboxes and (crawling) the dead skipped, as the Default layer's body.
        PhysxMoverWorld w(*g_State, b->Actor, ProjectSettings::Physics().LayerCollisionMask[0]);
        w.SelfEntity = n->entity;
        w.PassRagdolls = n->passRagdolls;
        const glm::vec3 delta(disp[0], disp[1], disp[2]);
        // Walking keeps to the floor down steps and slopes; anything going up is left to his own gravity.
        const bool snap = b->Grounded && delta.y <= 0.0f;
        RunMover(*b, w, delta / dt, delta, ProjectMoverSettings(), snap);
        PlaceBody(*b);
        return b->Last.Flags;
    }
    return 0;
}

void SetNpcFootPosition(CharacterId id, const float footPos[3]) {
    if (!FiniteVec3(footPos)) return;
    if (CharacterBody* b = NpcBody(id)) {
        b->Feet = glm::vec3(footPos[0], footPos[1], footPos[2]);
        PlaceBody(*b);
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
        PlaceBody(*b);
        return true;
    }
    return false;
}

bool NpcFitsAt(CharacterId id, float cylinderHalfHeight) {
    if (!NpcSlot(id) || cylinderHalfHeight <= 0.0f) return false;
    const CharacterBody* b = NpcBody(id);
    ScopedQueryPolicy self(NpcSlot(id)->entity, /*hitPlayer=*/true);
    return CapsuleFitsAt(b->Feet, b->Cap.Radius, cylinderHalfHeight);
}

CharacterId NpcCharacterOf(unsigned entity) {
    if (!g_State) return kNoCharacter;
    for (size_t i = 0; i < g_State->npcs.size(); ++i)
        if (g_State->npcs[i].Used() && g_State->npcs[i].entity == entity) return MakeHandle(i, g_State->npcs[i].Gen);
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
} // namespace PhysicsWorld
