#include "PhysicsInternal.h"

namespace PhysicsWorld {
namespace {
// --- Ragdolls --------------------------------------------------------------------------------
// One reduced-coordinate articulation per body: the joints are exact (a limb can't drift off its parent or stretch however hard
// it is hit, and light hands on heavy forearms don't whip through their limits), each joint is a spherical one whose three
// axes (twist about the joint frame's X, swing about its Y and Z) are limited and driven, and the parts collide with each other
// except where the filter says not (see EngineFilterShader).

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
        if (!PoseIsFinite(pose)) { copy->release(); rd.Rest = 0.0f; return; } // (not retried every frame)
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
} // namespace PhysicsWorld

namespace PhysicsWorld::detail {
// Once a frame: corpses asleep a while freeze; the ones walked into thaw.
void FreezeRestingRagdolls(PhysicsState& s, float dt) {
    for (int id : s.thawRequests)
        if (PhysicsState::Ragdoll* rd = RagdollInSlot(s, id)) ThawRagdoll(s, *rd);
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

PhysicsState::Ragdoll* RagdollAt(PhysicsState& s, int id) {
    PhysicsState::Ragdoll* rd = HandleTarget(s.ragdolls, id);
    return rd && rd->Art ? rd : nullptr;
}

PhysicsState::Ragdoll* RagdollInSlot(PhysicsState& s, int slot) {
    return slot >= 0 && slot < (int)s.ragdolls.size() && s.ragdolls[(size_t)slot].Art ? &s.ragdolls[(size_t)slot] : nullptr;
}

void ReleaseRagdollAttachments(PhysicsState& s, const PxRigidActor* actor) {
    if (!actor) return;
    for (auto& rd : s.ragdolls)
        for (PxJoint*& j : rd.Attach) {
            if (!j) continue;
            PxRigidActor *a0 = nullptr, *a1 = nullptr;
            j->getActors(a0, a1);
            if (a0 == actor || a1 == actor) { j->release(); j = nullptr; } // (RagdollAttachmentHeld then reads false)
        }
}

PhysicsState::Ragdoll* RagdollAt(int id) {
    return g_State ? RagdollAt(*g_State, id) : nullptr;
}

bool ValidRagdoll(int ragdoll) {
    return RagdollAt(ragdoll) != nullptr;
}
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
namespace {
const PxArticulationAxis::Enum kSphericalAxes[3] = {PxArticulationAxis::eTWIST, PxArticulationAxis::eSWING1, PxArticulationAxis::eSWING2};
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld::detail {
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
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
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
} // namespace PhysicsWorld

namespace PhysicsWorld {
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
    // `id` is the slot (word2's numbering); the caller gets MakeHandle(id, gen).
    const int id = AcquireSlot(s.ragdolls, [](const PhysicsState::Ragdoll& r) { return !r.Art; });
    if (id < 0) { Log::Error("PhysX: out of ragdoll slots."); return -1; }
    PhysicsState::Ragdoll& rd = s.ragdolls[(size_t)id];
    const std::uint16_t gen = NextGen(rd.Gen);
    rd = PhysicsState::Ragdoll();
    rd.Gen = gen;
    const RagdollParams prm = params ? *params : RagdollParams();
    PxArticulationReducedCoordinate* art = s.physics->createArticulationReducedCoordinate();
    if (!art) return -1;
    art->setSolverIterationCounts((PxU32)std::max(prm.SolverPosIters, 1), (PxU32)std::max(prm.SolverVelIters, 1));
    art->setSleepThreshold(prm.SleepThreshold);
    if (prm.StabilizationThreshold >= 0.0f) art->setStabilizationThreshold(prm.StabilizationThreshold);
    PxMaterial* mat = s.physics->createMaterial(prm.StaticFriction, prm.DynamicFriction, prm.Restitution);
    if (!mat) {
        Log::Error("PhysX: createMaterial failed for a ragdoll.");
        art->release();
        rd = PhysicsState::Ragdoll();
        return -1;
    }
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
            if (!slick) {
                Log::Error("PhysX: createMaterial failed for a ragdoll's slick material.");
                art->release();
                mat->release();
                rd = PhysicsState::Ragdoll();
                return -1;
            }
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
        if (!sh) {
            Log::Error("PhysX: createExclusiveShape failed for a ragdoll part.");
            art->release();
            mat->release();
            if (slick) slick->release();
            rd = PhysicsState::Ragdoll();
            return -1;
        }
        sh->setLocalPose(shapeLocal);
        sh->setSimulationFilterData(PxFilterData(1u, 0u, ((PxU32)id + 1u) | ((PxU32)i << 16), rd.Ignore[(size_t)i]));
        sh->setQueryFilterData(PxFilterData(1u, 0u, (PxU32)id + 1u, 0u));
        if (prm.SkinOffset > 0.0f) {
            // The flesh over the bone: contacts rest this far out, so the skinned mesh lies on the floor instead of in it.
            sh->setContactOffset(std::max(sh->getContactOffset(), prm.SkinOffset + 0.02f));
            sh->setRestOffset(prm.SkinOffset);
        }
        sh->userData = BodyPartTag(kBodyPartRagdoll, (unsigned)i); // RaycastBodyParts: ragdoll part i
        // Thin parts fast (a spider dropped off the ceiling lands at ~7 m/s, 12 cm a step): contacts found ahead of the
        // step, or a leg's tip sank 4 cm into the floor on landing. (Opt-in: it changes how a body's contacts resolve.)
        if (prm.SpeculativeCCD) link->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_SPECULATIVE_CCD, true);
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
    return MakeHandle((size_t)id, gen);
}

void DestroyRagdoll(int ragdoll) {
    if (!g_State) return;
    PhysicsState& s = *g_State;
    PhysicsState::Ragdoll* rdp = RagdollAt(s, ragdoll);
    if (!rdp) return;
    auto& rd = *rdp;
    const int slot = HandleSlot(ragdoll);
    for (PxJoint* j : rd.Attach) if (j) j->release();
    if (rd.Frozen) { s.scene->removeActor(*rd.Frozen); rd.Frozen->release(); }
    if (rd.Good) rd.Good->release();
    if (rd.Art) rd.Art->release(); // its links, shapes and joints, and out of the scene
    if (rd.Mat) rd.Mat->release();
    if (rd.SlickMat) rd.SlickMat->release();
    const std::uint16_t gen = rd.Gen;
    rd = PhysicsState::Ragdoll();
    rd.Gen = gen;
    // What was told to ignore it lets go: the slot's next ragdoll is someone else's (a dropped gun went on passing through it).
    auto letGo = [&](PxRigidActor& a) {
        bool changed = false;
        for (PxShape* sh : ShapesOf(a)) {
            PxFilterData f = sh->getSimulationFilterData();
            if (!IgnoresRagdollSlot(f, slot)) continue;
            f.word2 = 0u;
            f.word3 = 0u;
            sh->setSimulationFilterData(f);
            changed = true;
        }
        if (changed && a.getScene()) s.scene->resetFiltering(a);
        return changed;
    };
    for (auto& [entity, body] : s.bodyByEntity)
        if (body && letGo(*body)) body->setMaxDepenetrationVelocity(PX_MAX_F32); // (as IgnoreRagdollParts with no mask)
    for (auto& n : s.npcs)
        if (PxRigidActor* a = n.Actor()) letGo(*a);
}

void SetRagdollDamping(int ragdoll, float linear, float angular) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return;
    for (PxArticulationLink* b : rdp->Bodies)
        if (b) { b->setLinearDamping(std::max(linear, 0.0f)); b->setAngularDamping(std::max(angular, 0.0f)); }
}

void SetRagdollFriction(int ragdoll, float staticFriction, float dynamicFriction) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return;
    const auto& rd = *rdp;
    if (!rd.Mat) return;
    rd.Mat->setStaticFriction(std::max(staticFriction, 0.0f));
    rd.Mat->setDynamicFriction(std::max(dynamicFriction, 0.0f));
    if (rd.SlickMat) {
        rd.SlickMat->setStaticFriction(std::max(staticFriction, 0.0f) * rd.SlickScale);
        rd.SlickMat->setDynamicFriction(std::max(dynamicFriction, 0.0f) * rd.SlickScale);
    }
}

bool RagdollMotion(int ragdoll, float* outMaxLinear, float* outMaxAngular) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return false;
    const auto& rd = *rdp;
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
} // namespace PhysicsWorld

namespace PhysicsWorld {
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
} // namespace PhysicsWorld

namespace PhysicsWorld {
void RagdollSleep(int ragdoll) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return;
    auto& rd = *rdp;
    SleepRestingNeighbours(*g_State, rd); // (a frozen body's parts are where they lay: it still finds them)
    if (!rd.Frozen && !rd.Art->isSleeping()) rd.Art->putToSleep(); // (its velocities go to zero)
}

void RagdollAddSpin(int ragdoll, const float angular[3]) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp || !angular) return;
    auto& rd = *rdp;
    ThawRagdoll(*g_State, rd);
    // The whole body turned as one - each part given the turn's velocity at where it is, and the turn itself (velocity
    // changes: no masses or inertias to get right, no joint fought) - rolled over the low edge on the side it tips toward,
    // as a body rolls on the floor (turned about its centre instead, its feet's friction on the floor held it).
    PxVec3 com(0.0f);
    float mass = 0.0f, low = 1e9f, spread = 0.0f;
    for (PxRigidBody* b : rd.Bodies) {
        if (!b) continue;
        const PxVec3 p = (b->getGlobalPose() * b->getCMassLocalPose()).p;
        com += p * b->getMass();
        mass += b->getMass();
        low = std::min(low, p.y);
    }
    if (mass <= 0.0f) return;
    com /= mass;
    const PxVec3 w(angular[0], angular[1], angular[2]);
    const float rate = w.magnitude();
    if (rate < 1e-6f) return;
    PxVec3 down = (w / rate).cross(PxVec3(0.0f, 1.0f, 0.0f)); // (horizontal, where the turn carries the body down)
    for (PxRigidBody* b : rd.Bodies)
        if (b) spread = std::max(spread, ((b->getGlobalPose() * b->getCMassLocalPose()).p - com).dot(down));
    const PxVec3 pivot = PxVec3(com.x, low, com.z) + down * spread;
    for (PxRigidBody* b : rd.Bodies) {
        if (!b) continue;
        const PxVec3 r = (b->getGlobalPose() * b->getCMassLocalPose()).p - pivot;
        b->addForce(w.cross(r), PxForceMode::eVELOCITY_CHANGE, true);
        b->addTorque(w, PxForceMode::eVELOCITY_CHANGE, true);
    }
}

void RagdollImpulse(int ragdoll, int part, const float impulse[3], const float point[3]) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return;
    auto& rd = *rdp;
    if (part < 0 || part >= (int)rd.Bodies.size() || !rd.Bodies[(size_t)part]) return;
    ThawRagdoll(*g_State, rd);
    PxRigidBodyExt::addForceAtPos(*rd.Bodies[(size_t)part], PxVec3(impulse[0], impulse[1], impulse[2]),
                                  PxVec3(point[0], point[1], point[2]), PxForceMode::eIMPULSE, true);
}

bool GetRagdollPart(int ragdoll, int part, float outPos[3], float outRotXYZW[4]) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return false;
    auto& rd = *rdp;
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
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return;
    for (size_t i = 0; i < rdp->Drive.size(); ++i) SetRagdollPartDrive(ragdoll, (int)i, stiffness, damping);
}

void SetRagdollPartDrive(int ragdoll, int part, float stiffness, float damping) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return;
    auto& drive = rdp->Drive;
    if (part < 0 || part >= (int)drive.size() || !drive[(size_t)part]) return;
    const float I = rdp->DriveInertia[(size_t)part];
    const PxArticulationDrive d(std::max(stiffness, 0.0f) * I, std::max(damping, 0.0f) * I, PX_MAX_F32, PxArticulationDriveType::eFORCE);
    for (PxArticulationAxis::Enum ax : kSphericalAxes)
        if (drive[(size_t)part]->getMotion(ax) != PxArticulationMotion::eLOCKED) drive[(size_t)part]->setDriveParams(ax, d);
}

bool GetRagdollPartInertia(int ragdoll, int part, float out[3]) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp || !out) return false;
    auto& rd = *rdp;
    if (part < 0 || part >= (int)rd.Bodies.size() || !rd.Bodies[(size_t)part]) return false;
    const PxVec3 I = rd.Bodies[(size_t)part]->getMassSpaceInertiaTensor();
    out[0] = I.x; out[1] = I.y; out[2] = I.z;
    return true;
}

void SetRagdollDriveTarget(int ragdoll, int part, const float rotXYZW[4]) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp || !rotXYZW) return;
    auto& drive = rdp->Drive;
    if (part < 0 || part >= (int)drive.size() || !drive[(size_t)part]) return;
    // (The target turns the joint from the build pose; the joint's own rotation is from the middle of its range.)
    const auto& rd = *rdp;
    PxQuat q = ToQuat(rotXYZW);
    if ((size_t)part < rd.TargetPre.size()) q = rd.TargetPre[(size_t)part] * q * rd.TargetPost[(size_t)part];
    const PxVec3 v = VectorFromRotation(q.getNormalized());
    const float axes[3] = {v.x, v.y, v.z};
    for (int k = 0; k < 3; ++k) // (a sleeping body stays so)
        if (drive[(size_t)part]->getMotion(kSphericalAxes[k]) != PxArticulationMotion::eLOCKED) drive[(size_t)part]->setDriveTarget(kSphericalAxes[k], axes[k], /*autowake=*/false);
}

bool RagdollAsleep(int ragdoll) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return true;
    const auto& rd = *rdp;
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

bool RagdollFrozen(int ragdoll) { const PhysicsState::Ragdoll* rd = RagdollAt(ragdoll); return rd && rd->Frozen; }

bool RagdollPairCollides(int ragdoll, int a, int b) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return false;
    const auto& rd = *rdp;
    if (a < 0 || b < 0 || a >= (int)rd.Ignore.size() || b >= (int)rd.Ignore.size() || a == b) return false;
    return !(rd.Ignore[(size_t)a] & (1u << b)) && !(rd.Ignore[(size_t)b] & (1u << a));
}

bool GetRagdollJoint(int ragdoll, int part, float outPos[3], float outLo[3], float outHi[3]) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return false;
    auto& drive = rdp->Drive;
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
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return -1;
    PhysicsState& s = *g_State;
    auto& rd = *rdp;
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
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return false;
    auto& rd = *rdp;
    if (handle < 0 || handle >= (int)rd.Attach.size() || !rd.Attach[(size_t)handle]) return false;
    return !(rd.Attach[(size_t)handle]->getConstraintFlags() & PxConstraintFlag::eBROKEN);
}

void DetachFromRagdoll(int ragdoll, int handle) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return;
    auto& rd = *rdp;
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
    // Whatever it was passing, it is pushed back out gently, not fired out of the body; PhysX's default (unlimited) comes back with the mask.
    body->setMaxDepenetrationVelocity(on ? 1.5f : PX_MAX_F32);
    std::vector<PxShape*> shapes = ShapesOf(*body);
    for (PxShape* sh : shapes) {
        PxFilterData f = sh->getSimulationFilterData();
        // The ignorer slot of the ragdoll's own numbering: not one of its parts, so only this object's mask decides.
        f.word2 = on ? (((PxU32)HandleSlot(ragdoll) + 1u) | (kRagdollIgnorerSlot << 16)) : 0u;
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
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return 0u;
    PhysicsState& s = *g_State;
    auto it = s.bodyByEntity.find(entity);
    if (it == s.bodyByEntity.end() || !it->second) return 0u;
    PxRigidDynamic* body = it->second;
    const auto& rd = *rdp;
    std::vector<PxShape*> shapes = ShapesOf(*body);
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
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp || partMask == 0u) return false;
    PhysicsState& s = *g_State;
    auto it = s.bodyByEntity.find(entity);
    if (it == s.bodyByEntity.end() || !it->second) return false;
    PxRigidDynamic* body = it->second;
    const auto& rd = *rdp;
    std::vector<PxShape*> shapes = ShapesOf(*body);
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
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return false;
    const auto& rd = *rdp;
    if (part < 0 || part >= (int)rd.Bodies.size()) return false;
    PxShape* sh = LinkShape(rd.Bodies[(size_t)part]);
    if (!sh || sh->getGeometry().getType() != PxGeometryType::eCAPSULE) return false;
    const PxCapsuleGeometry& g = static_cast<const PxCapsuleGeometry&>(sh->getGeometry());
    if (outRadius) *outRadius = g.radius;
    if (outHalfLength) *outHalfLength = g.halfHeight;
    return true;
}

float RagdollWorstSelfPenetration(int ragdoll, int* outA, int* outB) {
    PhysicsState::Ragdoll* rdp = RagdollAt(ragdoll);
    if (!rdp) return 0.0f;
    const auto& rd = *rdp;
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
} // namespace PhysicsWorld
