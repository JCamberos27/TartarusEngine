#include "PhysicsInternal.h"

namespace PhysicsWorld::detail {
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
} // namespace PhysicsWorld::detail

namespace PhysicsWorld {
// --- Gravity gun (#185 hardening, physics-playground debug harness) ------------------
// "Carry an object in front of the camera" grab. GrabBody latches a dynamic body and
// suspends its gravity; UpdateGrab (called each frame with the desired world point) servos
// its velocity toward that point and cancels spin; ReleaseBody lets go, optionally firing
// a velocity-change impulse to launch it.

void GrabBody(unsigned entity) {
    if (!g_State) return;
    PxRigidDynamic* b = DynamicFor(entity);
    if (!b) return;
    if (g_State->spunBody == b) { b->setMaxAngularVelocity(g_State->spunSavedMaxAngVel); g_State->spunBody = nullptr; }
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
            if (g_State->spunBody && g_State->spunBody != b) g_State->spunBody->setMaxAngularVelocity(g_State->spunSavedMaxAngVel);
            if (g_State->spunBody != b) { g_State->spunBody = b; g_State->spunSavedMaxAngVel = b->getMaxAngularVelocity(); }
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
} // namespace PhysicsWorld
