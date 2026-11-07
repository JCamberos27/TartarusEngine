#include "PhysicsInternal.h"

namespace PhysicsWorld::detail {
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
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
namespace {
// Trigger entities whose volume the Player capsule currently overlaps. One overlap query per
// Step — cheap for the handful of trigger shapes a scene has.
// Accepts only trigger shapes whose layer can collide with the Player's layer (the matrix row, as
// MoveCharacter uses); everything else (floors, props, ragdoll links) never reaches the hit buffer.
struct PlayerTriggerFilter : PxQueryFilterCallback {
    PxU32 mask = 0xFFFFFFFFu;
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape* shape, const PxRigidActor*, PxHitFlags&) override {
        if (!shape || !(shape->getFlags() & PxShapeFlag::eTRIGGER_SHAPE)) return PxQueryHitType::eNONE;
        const PxU32 layerBit = shape->getSimulationFilterData().word0;
        if (layerBit && !(layerBit & mask)) return PxQueryHitType::eNONE;
        return PxQueryHitType::eTOUCH;
    }
    PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*, const PxRigidActor*) override {
        return PxQueryHitType::eTOUCH;
    }
};
} // namespace
} // namespace PhysicsWorld

namespace PhysicsWorld::detail {
void CollectPlayerTriggers(std::set<std::uint32_t>& out) {
    out.clear();
    if (!g_State->playerBody) return;
    const auto* b = g_State->playerBody.get();
    const PxCapsuleGeometry geom(b->Cap.Radius, b->Cap.HalfHeight);
    const PxVec3 centre = ToPx(b->Feet + glm::vec3(0.0f, b->Cap.Radius + b->Cap.HalfHeight, 0.0f));
    const PxTransform pose(centre, PxQuat(PxHalfPi, PxVec3(0, 0, 1))); // capsule axis X -> up

    // #191 — the touch buffer grows until PhysX reports every overlap (it used to be a fixed 16,
    // silently dropping Enter / Stay / Exit for any trigger past the 16th).
    PxQueryFilterData fd(PxQueryFlag::eSTATIC | PxQueryFlag::eDYNAMIC | PxQueryFlag::eNO_BLOCK | PxQueryFlag::ePREFILTER);
    PlayerTriggerFilter filter;
    filter.mask = ProjectSettings::Physics().LayerCollisionMask[std::clamp(ProjectSettings::Physics().PlayerLayer, 0, 31)];
    static std::vector<PxOverlapHit> hits(64);
    for (;;) {
        PxOverlapBuffer buf(hits.data(), (PxU32)hits.size());
        if (!g_State->scene->overlap(geom, pose, buf, fd, &filter)) return;
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
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
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

int GetContactEvents(ContactEvent* out, int maxEvents) {
    if (!g_State) return 0;
    const int total = (int)g_State->contactEvents.size();
    const int n = (maxEvents < total) ? maxEvents : total;
    for (int i = 0; i < n && out; ++i) out[i] = g_State->contactEvents[(size_t)i];
    return total;
}

void SetEventLogging(bool enabled) { g_LogEvents = enabled; }
} // namespace PhysicsWorld
