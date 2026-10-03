#include "UnitTestSupport.h"

#include "AI/NavMesh.h"
#include "AI/NpcDirector.h"
#include "AssetLibrary.h"
#include "ComponentRegistry.h"
#include "Components.h"
#include "Npc/NpcRagdoll.h"
#include "PhysicsWorld.h"
#include "SceneSerializer.h"
#include "World.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <string>
#include "GameModuleAPI.h" // BodyState
#include "Npc/NpcBody.h"
#include "Npc/NpcDroppedWeapon.h"

// Unit tests for ragdolls, NPC death and NPC physics. Add a function per test and list it below.

namespace {

// A soldier standing facing +z (x is the body's left), arms down at 45 degrees (an A pose), every limb straight.
bool StandingBone(const char* name, glm::vec3& out) {
    static const std::map<std::string, glm::vec3> kBones = {
        {"pelvis", {0, 1.0f, 0}},        {"spine_02", {0, 1.15f, 0}},      {"spine_03", {0, 1.3f, 0}},       {"neck_01", {0, 1.5f, 0}},
        {"head", {0, 1.6f, 0}},          {"upperarm_l", {0.2f, 1.45f, 0}}, {"lowerarm_l", {0.45f, 1.2f, 0}}, {"hand_l", {0.65f, 1.0f, 0}},
        {"upperarm_r", {-0.2f, 1.45f, 0}}, {"lowerarm_r", {-0.45f, 1.2f, 0}}, {"hand_r", {-0.65f, 1.0f, 0}},
        {"thigh_l", {0.1f, 0.95f, 0}},   {"calf_l", {0.1f, 0.5f, 0}},      {"foot_l", {0.1f, 0.08f, 0}},
        {"thigh_r", {-0.1f, 0.95f, 0}},  {"calf_r", {-0.1f, 0.5f, 0}},     {"foot_r", {-0.1f, 0.08f, 0}},
        {"middle_01_l", {0.72f, 0.93f, 0}}, {"middle_01_r", {-0.72f, 0.93f, 0}}, {"ball_l", {0.1f, 0.02f, 0.14f}}, {"ball_r", {-0.1f, 0.02f, 0.14f}},
    };
    const auto it = kBones.find(name);
    if (it == kBones.end()) return false;
    out = it->second;
    return true;
}

// The same body on a rig without the fingers' and toes' bones.
bool StandingBoneNoTips(const char* name, glm::vec3& out) {
    const std::string n(name);
    if (n.rfind("middle_01", 0) == 0 || n.rfind("ball_", 0) == 0) return false;
    return StandingBone(name, out);
}

glm::vec3 DirOf(int ragdoll, int part) {
    float p[3], q[4];
    PhysicsWorld::GetRagdollPart(ragdoll, part, p, q);
    const glm::quat r(q[3], q[0], q[1], q[2]);
    return r * glm::vec3(1, 0, 0);
}

// Shoves part `part` (a calf, a forearm) along `push` (world) at its far end, with the drives off, and returns the largest angle,
// in degrees, its bone ever makes with its parent's bone on the side `push` goes (the joint bending that way) over a second.
float BendAlong(const RagdollSettingsComponent& cfg, int part, const glm::vec3& push, float impulsePerKg) {
    World world;
    PhysicsWorld::Create(world);
    if (!PhysicsWorld::IsActive()) return -1.0f;
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
    const bool built = NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts);
    CHECK(built);
    const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
    const int id = PhysicsWorld::CreateRagdoll(4343u, parts, NpcRagdoll::kRagParts, &prm);
    CHECK(id >= 0);
    float worst = 0.0f;
    if (id >= 0) {
        const int parent = NpcRagdollDefOf(part).Parent;
        glm::vec3 end;
        StandingBone(NpcRagdollDefOf(part).End, end);
        const glm::vec3 j = push * NpcRagdoll::PartMass(&cfg, part) * impulsePerKg;
        const float ji[3] = {j.x, j.y, j.z}, at[3] = {end.x, end.y, end.z};
        PhysicsWorld::RagdollImpulse(id, part, ji, at);
        for (int i = 0; i < 60; ++i) {
            PhysicsWorld::Step(1.0f / 60.0f, world, {});
            const glm::vec3 a = DirOf(id, parent), b = DirOf(id, part);
            if (glm::dot(b - a, push) > 0.0f)
                worst = std::max(worst, glm::degrees(std::acos(std::clamp(glm::dot(a, b), -1.0f, 1.0f))));
        }
        PhysicsWorld::DestroyRagdoll(id);
    }
    PhysicsWorld::Destroy();
    return worst;
}

// Knees and elbows are hinges: shoved backwards (a knee) / forwards-to-back (an elbow) they stop at straight; with the old
// symmetric cones (AnatomicalLimits off) they folded the wrong way by tens of degrees.
void TestRagdollHingesDoNotHyperextend() {
    RagdollSettingsComponent anatomical, legacy;
    legacy.AnatomicalLimits = false;
    const glm::vec3 fwd(0, 0, 1);
    const int kCalfL = 8, kCalfR = 10, kForearmL = 4, kForearmR = 6;
    const float kneeNew = std::max(BendAlong(anatomical, kCalfL, fwd, 3.0f), BendAlong(anatomical, kCalfR, fwd, 3.0f));
    const float kneeOld = std::max(BendAlong(legacy, kCalfL, fwd, 3.0f), BendAlong(legacy, kCalfR, fwd, 3.0f));
    const float elbowNew = std::max(BendAlong(anatomical, kForearmL, -fwd, 3.0f), BendAlong(anatomical, kForearmR, -fwd, 3.0f));
    const float elbowOld = std::max(BendAlong(legacy, kForearmL, -fwd, 3.0f), BendAlong(legacy, kForearmR, -fwd, 3.0f));
    std::printf("[UnitTest] max hyperextension: knee %.1f deg (was %.1f), elbow %.1f deg (was %.1f)\n", kneeNew, kneeOld, elbowNew, elbowOld);
    CHECK(kneeNew < 6.0f && elbowNew < 6.0f);
    CHECK(kneeOld > 15.0f && elbowOld > 15.0f);
    // The hands and feet are damped and heavy-inertia so they can't whip the limb above through its limit (without: elbow ~20 deg), but
    // the wrist and ankle stay free: a shove still folds them well past the damping's reach.
    const float wrist = std::max(BendAlong(anatomical, NpcRagdoll::kHandL, fwd, 3.0f), BendAlong(anatomical, NpcRagdoll::kHandL, -fwd, 3.0f));
    const float ankle = std::max(BendAlong(anatomical, NpcRagdoll::kFootL, fwd, 3.0f), BendAlong(anatomical, NpcRagdoll::kFootL, -fwd, 3.0f));
    std::printf("[UnitTest] free joints under the same shove: wrist folds %.1f deg, ankle %.1f deg\n", wrist, ankle);
    CHECK(wrist > 25.0f && ankle > 15.0f);
    RagdollSettingsComponent undamped = anatomical;
    undamped.DistalJointDamping = 0.0f; undamped.DistalInertiaScale = 1.0f;
    CHECK(BendAlong(undamped, 4, -fwd, 3.0f) > 15.0f); // the cause: without them the elbow overshoots
    // The proper way still folds: a knee bends back, an elbow forward.
    CHECK(BendAlong(anatomical, kCalfL, -fwd, 3.0f) > 30.0f);
    CHECK(BendAlong(anatomical, kForearmR, fwd, 3.0f) > 30.0f);
}

// The Ragdoll Settings reach the parts: masses, ranges and the body-wide tuning.
void TestRagdollSettingsReachTheParts() {
    RagdollSettingsComponent cfg;
    cfg.CalfMass = 9.0f;
    cfg.ThighFlexMax = 60.0f;
    cfg.LinearDamping = 0.5f;
    cfg.SolverPosIters = 24;
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts));
    CHECK(std::fabs(parts[8].Mass - 9.0f) < 1e-4f && std::fabs(parts[10].Mass - 9.0f) < 1e-4f);
    CHECK(parts[7].Anatomical && std::fabs(parts[7].SwingZMax - 60.0f) < 1e-4f);
    CHECK(parts[8].Anatomical && parts[8].SwingZMin == 0.0f); // a knee: no extension past straight
    CHECK(std::fabs(NpcRagdoll::BodyParams(cfg).LinearDamping - 0.5f) < 1e-6f && NpcRagdoll::BodyParams(cfg).SolverPosIters == 24);
    // The defaults are what was hard-coded: a body of ~75 kg.
    float total = 0.0f;
    for (int i = 0; i < NpcRagdoll::kRagParts; ++i) total += NpcRagdoll::PartMass(nullptr, i);
    CHECK(std::fabs(total - 75.0f) < 2.0f);
    const RagdollSettingsComponent def;
    for (int i = 0; i < NpcRagdoll::kRagParts; ++i) CHECK(std::fabs(NpcRagdoll::PartMass(&def, i) - NpcRagdollDefOf(i).Mass) < 1e-5f);
    // Legacy limits keep the old cone.
    RagdollSettingsComponent old;
    old.AnatomicalLimits = false;
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), old, parts));
    CHECK(!parts[8].Anatomical && std::fabs(parts[8].SwingDeg - NpcPartDefOf(8).Swing) < 1e-5f);
}

// Each part starts with its own bone's velocity (the finite difference of two poses), not only the body's.
void TestRagdollPartsInheritTheirBonesVelocity() {
    const RagdollSettingsComponent cfg;
    const float dt = 1.0f / 60.0f, angle = 0.25f; // the left hand swung 0.25 rad about the elbow (z) in one frame
    auto swung = [&](float a, const glm::vec3& shift) {
        return [=](const char* name, glm::vec3& out) {
            if (!StandingBone(name, out)) return false;
            if (std::string(name) == "hand_l" || std::string(name) == "middle_01_l") {
                glm::vec3 elbow;
                StandingBone("lowerarm_l", elbow);
                const glm::vec3 r = out - elbow;
                out = elbow + glm::vec3(r.x * std::cos(a) - r.y * std::sin(a), r.x * std::sin(a) + r.y * std::cos(a), r.z);
            }
            if (std::string(name) == "foot_r") out += shift;
            return true;
        };
    };
    auto speedOf = [](const float v[3]) { return glm::length(glm::vec3(v[0], v[1], v[2])); };
    PhysicsWorld::RagdollPart before[NpcRagdoll::kRagParts], now[NpcRagdoll::kRagParts], jump[NpcRagdoll::kRagParts], idle[NpcRagdoll::kRagParts];
    glm::mat4 wb[NpcRagdoll::kRagParts], wn[NpcRagdoll::kRagParts], wj[NpcRagdoll::kRagParts];
    CHECK(NpcRagdoll::BuildParts(swung(0.0f, glm::vec3(0.0f)), glm::mat4(1.0f), glm::vec3(0.0f), cfg, before, wb));
    CHECK(NpcRagdoll::BuildParts(swung(angle, glm::vec3(0.0f)), glm::mat4(1.0f), glm::vec3(0.0f), cfg, now, wn));
    NpcRagdoll::InheritVelocity(wb, wn, dt, cfg, now);
    const glm::vec3 w(now[4].AngularVelocity[0], now[4].AngularVelocity[1], now[4].AngularVelocity[2]);
    std::printf("[UnitTest] forearm spin %.2f rad/s (bone turned %.2f rad in %.0f ms)\n", glm::length(w), angle, dt * 1000.0f);
    CHECK(std::fabs(std::fabs(w.z) - angle / dt) < 0.05f * angle / dt && std::fabs(w.x) < 0.05f && std::fabs(w.y) < 0.05f);
    CHECK(speedOf(now[4].Velocity) > 0.3f); // its centre moves too
    for (int i : {0, 1, 2, 3, NpcRagdoll::kNeck}) CHECK(speedOf(now[i].AngularVelocity) < 1e-3f);
    // The hand turns with the forearm (the fingers' bone was swung with it), the feet and the other hand don't move.
    const glm::vec3 wh(now[NpcRagdoll::kHandL].AngularVelocity[0], now[NpcRagdoll::kHandL].AngularVelocity[1], now[NpcRagdoll::kHandL].AngularVelocity[2]);
    std::printf("[UnitTest] hand spin %.2f rad/s, speed %.2f m/s\n", glm::length(wh), speedOf(now[NpcRagdoll::kHandL].Velocity));
    CHECK(std::fabs(std::fabs(wh.z) - angle / dt) < 0.05f * angle / dt && speedOf(now[NpcRagdoll::kHandL].Velocity) > 0.3f);
    for (int i : {NpcRagdoll::kHandR, NpcRagdoll::kFootL, NpcRagdoll::kFootR}) CHECK(speedOf(now[i].AngularVelocity) < 1e-3f && speedOf(now[i].Velocity) < 1e-3f);
    // A teleport (a bone 5 m off in one frame) is clamped, a bad dt adds nothing, and the scale switches it off.
    CHECK(NpcRagdoll::BuildParts(swung(0.0f, glm::vec3(5.0f, 0.0f, 0.0f)), glm::mat4(1.0f), glm::vec3(0.0f), cfg, jump, wj));
    NpcRagdoll::InheritVelocity(wb, wj, dt, cfg, jump);
    float fastest = 0.0f;
    for (int i = 0; i < NpcRagdoll::kRagParts; ++i) fastest = std::max(fastest, speedOf(jump[i].Velocity));
    CHECK(fastest > 1.0f && fastest <= cfg.MaxLimbSpeed + 1e-3f);
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f), cfg, idle));
    NpcRagdoll::InheritVelocity(wb, wn, 0.0f, cfg, idle);
    NpcRagdoll::InheritVelocity(wb, wn, 1.0f, cfg, idle);
    CHECK(idle[4].AngularVelocity[2] == 0.0f && idle[4].Velocity[0] == 1.0f);
    RagdollSettingsComponent off = cfg;
    off.LimbVelocityScale = 0.0f;
    NpcRagdoll::InheritVelocity(wb, wn, dt, off, idle);
    CHECK(idle[4].AngularVelocity[2] == 0.0f);
    // And PhysX takes it: the spun forearm turns in the first steps.
    World world;
    PhysicsWorld::Create(world);
    if (!PhysicsWorld::IsActive()) return;
    const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
    const int id = PhysicsWorld::CreateRagdoll(4344u, now, NpcRagdoll::kRagParts, &prm);
    CHECK(id >= 0);
    if (id >= 0) {
        PhysicsWorld::SetRagdollDrive(id, 0.0f, 0.0f);
        const glm::vec3 d0 = DirOf(id, 4);
        for (int i = 0; i < 3; ++i) PhysicsWorld::Step(1.0f / 60.0f, world, {});
        const float turned = std::acos(std::clamp(glm::dot(d0, DirOf(id, 4)), -1.0f, 1.0f));
        std::printf("[UnitTest] forearm turned %.3f rad in the first three steps (a bone swung %.2f rad/frame)\n", turned, angle);
        CHECK(turned > 0.05f);
        PhysicsWorld::DestroyRagdoll(id);
    }
    PhysicsWorld::Destroy();
}

// The torso parts' inertia comes from a box wider than deep, not the round capsule; the scale multiplies it.
void TestRagdollShapedInertia() {
    World world;
    PhysicsWorld::Create(world);
    if (!PhysicsWorld::IsActive()) return;
    auto inertia = [&](const RagdollSettingsComponent& cfg, int part, float out[3]) {
        PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
        CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts));
        const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
        const int id = PhysicsWorld::CreateRagdoll(4345u, parts, NpcRagdoll::kRagParts, &prm);
        CHECK(id >= 0);
        const bool ok = id >= 0 && PhysicsWorld::GetRagdollPartInertia(id, part, out);
        CHECK(ok);
        if (id >= 0) PhysicsWorld::DestroyRagdoll(id);
    };
    RagdollSettingsComponent shaped, capsule, heavy;
    capsule.ShapedTorsoInertia = false;
    heavy.InertiaScale = 2.0f;
    float s[3] = {}, c[3] = {}, h[3] = {}, armShaped[3] = {}, armCapsule[3] = {};
    inertia(shaped, 1, s); inertia(capsule, 1, c); inertia(heavy, 1, h);
    inertia(shaped, 4, armShaped); inertia(capsule, 4, armCapsule);
    std::printf("[UnitTest] chest inertia (about the bone, across, front-back): shaped %.3f %.3f %.3f, capsule %.3f %.3f %.3f\n", s[0], s[1], s[2], c[0], c[1], c[2]);
    CHECK(std::fabs(s[0] - c[0]) > 0.05f * c[0]);         // not the capsule's
    CHECK(s[1] < s[2]);                                    // a box wider than deep: easier to bend forward than sideways
    CHECK(std::fabs(c[1] - c[2]) < 0.02f * c[1]);          // a capsule is symmetric across its axis
    CHECK(std::fabs(h[0] - 2.0f * s[0]) < 0.01f * s[0] && std::fabs(h[2] - 2.0f * s[2]) < 0.01f * s[2]);
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(armShaped[k] - armCapsule[k]) < 1e-5f); // limbs keep the capsule's own
    PhysicsWorld::Destroy();
}

// Each region's drive fades on its own clock; the defaults are the old single fade.
void TestRagdollPerRegionDriveFade() {
    const RagdollSettingsComponent def;
    for (int i = 0; i < NpcRagdoll::kRagParts; ++i) CHECK(std::fabs(NpcRagdoll::PartFade(&def, i) - NpcRagdoll::kDriveFade) < 1e-6f);
    CHECK(std::fabs(NpcRagdoll::PartFade(nullptr, 3) - NpcRagdoll::kDriveFade) < 1e-6f);
    RagdollSettingsComponent cfg;
    cfg.DriveFade = 0.4f;
    cfg.ThighFadeScale = 0.5f; cfg.CalfFadeScale = 0.5f; cfg.SpineFadeScale = 1.5f; cfg.HeadFadeScale = 2.0f;
    CHECK(std::fabs(NpcRagdoll::PartFade(&cfg, 7) - 0.2f) < 1e-6f && std::fabs(NpcRagdoll::PartFade(&cfg, 10) - 0.2f) < 1e-6f);
    CHECK(std::fabs(NpcRagdoll::PartFade(&cfg, 1) - 0.6f) < 1e-6f && std::fabs(NpcRagdoll::PartFade(&cfg, 2) - 0.8f) < 1e-6f);
    CHECK(std::fabs(NpcRagdoll::PartFade(&cfg, 4) - 0.4f) < 1e-6f); // forearms untouched
    // At 0.2 s the legs are limp while the spine still holds two thirds, the head three quarters.
    CHECK(NpcRagdoll::DriveAt(0.2f, NpcRagdoll::PartFade(&cfg, 7)) == 0.0f);
    CHECK(std::fabs(NpcRagdoll::DriveAt(0.2f, NpcRagdoll::PartFade(&cfg, 1)) - (1.0f - 0.2f / 0.6f)) < 1e-5f);
    CHECK(std::fabs(NpcRagdoll::DriveAt(0.2f, NpcRagdoll::PartFade(&cfg, 2)) - 0.75f) < 1e-5f);
    CHECK(NpcRagdoll::DriveAt(0.0f, 0.3f) == 1.0f);
    // The neck, hands and feet have their own scales.
    cfg.NeckFadeScale = 3.0f; cfg.HandFadeScale = 0.25f; cfg.FootFadeScale = 0.5f;
    CHECK(std::fabs(NpcRagdoll::PartFade(&cfg, NpcRagdoll::kNeck) - 1.2f) < 1e-6f);
    for (int i : {NpcRagdoll::kHandL, NpcRagdoll::kHandR}) CHECK(std::fabs(NpcRagdoll::PartFade(&cfg, i) - 0.1f) < 1e-6f);
    for (int i : {NpcRagdoll::kFootL, NpcRagdoll::kFootR}) CHECK(std::fabs(NpcRagdoll::PartFade(&cfg, i) - 0.2f) < 1e-6f);
}

// The hitboxes keep today's eleven parts (gameplay doesn't change); the ragdoll's own table adds the neck, hands and feet after
// them, with the forearms and calves stopping at the wrist and ankle and the head hanging off the neck.
void TestRagdollTableIsSplitFromHitboxes() {
    static const char* const kBones[NpcRagdoll::kParts] = {"pelvis", "spine_03", "head", "upperarm_l", "lowerarm_l", "upperarm_r", "lowerarm_r", "thigh_l", "calf_l", "thigh_r", "calf_r"};
    static const float kRadius[NpcRagdoll::kParts] = {0.13f, 0.15f, 0.1f, 0.055f, 0.045f, 0.055f, 0.045f, 0.075f, 0.055f, 0.075f, 0.055f};
    static const float kExtend[NpcRagdoll::kParts] = {0, 0, 0.2f, 0, 0.1f, 0, 0.1f, 0, 0.06f, 0, 0.06f};
    for (int i = 0; i < NpcRagdoll::kParts; ++i) {
        const NpcPartDef& h = NpcPartDefOf(i);
        CHECK(std::strcmp(h.Bone, kBones[i]) == 0 && std::fabs(h.Radius - kRadius[i]) < 1e-6f && std::fabs(h.Extend - kExtend[i]) < 1e-6f);
        CHECK(std::strcmp(NpcRagdollDefOf(i).Bone, kBones[i]) == 0); // the same bone at the same index
    }
    CHECK(NpcRagdoll::kRagParts == 16);
    const char* const added[5] = {"neck_01", "hand_l", "hand_r", "foot_l", "foot_r"};
    for (int k = 0; k < 5; ++k) CHECK(std::strcmp(NpcRagdollDefOf(NpcRagdoll::kNeck + k).Bone, added[k]) == 0);
    CHECK(NpcRagdollDefOf(4).Extend == 0.0f && NpcRagdollDefOf(8).Extend == 0.0f); // the hand and foot capsules take over
    CHECK(NpcRagdollDefOf(2).Parent == NpcRagdoll::kNeck && NpcRagdollDefOf(NpcRagdoll::kNeck).Parent == 1);
    CHECK(NpcRagdollDefOf(NpcRagdoll::kHandL).Parent == 4 && NpcRagdollDefOf(NpcRagdoll::kFootR).Parent == 10);
    // Built from a body, the head is jointed to the neck and the hands and feet to the forearms and calves.
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
    const RagdollSettingsComponent cfg;
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts));
    CHECK(parts[2].Parent == NpcRagdoll::kNeck && parts[NpcRagdoll::kNeck].Parent == 1 && parts[NpcRagdoll::kHandR].Parent == 6 && parts[NpcRagdoll::kFootL].Parent == 8);
}

// The new regions' masses and ranges come from the Ragdoll Settings, with human defaults (wrist, ankle, neck), on rigs with or
// without the fingers' and toes' bones.
void TestRagdollNeckHandsFeetSettings() {
    const RagdollSettingsComponent def;
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), def, parts));
    const int N = NpcRagdoll::kNeck, H = NpcRagdoll::kHandL, F = NpcRagdoll::kFootL, HR = NpcRagdoll::kHandR, FR = NpcRagdoll::kFootR;
    CHECK(parts[N].Anatomical && parts[H].Anatomical && parts[F].Anatomical && parts[HR].Anatomical && parts[FR].Anatomical);
    CHECK(std::fabs(parts[N].SwingZMax - def.NeckFlexMax) < 1e-4f && std::fabs(parts[N].SwingZMin + def.NeckExtMax) < 1e-4f);
    CHECK(std::fabs(parts[H].SwingZMax - def.HandFlexMax) < 1e-4f && std::fabs(parts[H].SwingZMin + def.HandExtMax) < 1e-4f);
    CHECK(std::fabs(parts[F].SwingZMax - def.FootFlexMax) < 1e-4f && std::fabs(parts[F].SwingZMin + def.FootExtMax) < 1e-4f); // dorsi 20 / plantar 50
    CHECK(std::fabs(parts[F].TwistMax - def.FootTwistIn) < 1e-4f || std::fabs(parts[F].TwistMax - def.FootTwistOut) < 1e-4f);
    CHECK(std::fabs(parts[H].Mass - 0.5f) < 1e-5f && std::fabs(parts[F].Mass - 1.0f) < 1e-5f && std::fabs(parts[N].Mass - 1.0f) < 1e-5f);
    // The total is a ~75 kg man.
    float total = 0.0f;
    for (int i = 0; i < NpcRagdoll::kRagParts; ++i) total += parts[i].Mass;
    std::printf("[UnitTest] ragdoll mass %.1f kg over %d parts\n", total, NpcRagdoll::kRagParts);
    CHECK(std::fabs(total - 75.0f) < 2.0f);
    // The settings reach them: mass, limits (a wrist stiffened, an ankle loosened), and the legacy cones.
    RagdollSettingsComponent cfg;
    cfg.HandMass = 0.9f; cfg.FootMass = 1.7f; cfg.NeckMass = 2.0f; cfg.HandFlexMax = 30.0f; cfg.FootExtMax = 80.0f; cfg.NeckTwistIn = 10.0f; cfg.NeckTwistOut = 12.0f;
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts));
    CHECK(std::fabs(parts[HR].Mass - 0.9f) < 1e-5f && std::fabs(parts[FR].Mass - 1.7f) < 1e-5f && std::fabs(parts[N].Mass - 2.0f) < 1e-5f);
    CHECK(std::fabs(parts[H].SwingZMax - 30.0f) < 1e-4f && std::fabs(parts[F].SwingZMin + 80.0f) < 1e-4f);
    CHECK(std::fabs(parts[N].TwistMin + 10.0f) < 1e-4f && std::fabs(parts[N].TwistMax - 12.0f) < 1e-4f);
    RagdollSettingsComponent legacy;
    legacy.AnatomicalLimits = false;
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), legacy, parts));
    CHECK(!parts[H].Anatomical && !parts[F].Anatomical && !parts[N].Anatomical && parts[H].SwingDeg > 1.0f);
    // A rig without the fingers' and toes' bones still builds them: a stub along the forearm, a stub forward from the ankle.
    PhysicsWorld::RagdollPart stub[NpcRagdoll::kRagParts];
    CHECK(NpcRagdoll::BuildParts(StandingBoneNoTips, glm::mat4(1.0f), glm::vec3(0.0f), def, stub));
    const glm::vec3 ankle(0.1f, 0.08f, 0.0f);
    const glm::vec3 footC(stub[F].Position[0], stub[F].Position[1], stub[F].Position[2]);
    CHECK(footC.z > 0.03f && glm::length(footC - ankle) < 0.15f && stub[F].HalfLength > 0.01f);
    CHECK(stub[H].Position[0] > 0.65f && stub[H].Position[1] < 1.0f && stub[H].HalfLength > 0.01f);
    // A body that is missing a part's own bone does not build.
    CHECK(!NpcRagdoll::BuildParts([](const char* n, glm::vec3& o) { return std::strcmp(n, "hand_r") != 0 && StandingBone(n, o); }, glm::mat4(1.0f), glm::vec3(0.0f), def, stub));
}

// The whole sixteen-part body, limp on a floor: the joints (the head now hangs off the neck) hold together, and the hands and feet
// rest on the floor rather than sinking through it. Prints how deep they go.
void TestRagdollHandsAndFeetRestOnTheFloor() {
    World world;
    const entt::entity floorE = world.CreateEmptyEntity(glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Floor");
    ColliderComponent col;
    col.Kind = ColliderComponent::Shape::Box;
    col.HalfExtents = glm::vec3(20.0f, 0.5f, 20.0f);
    world.Registry.emplace<ColliderComponent>(floorE, col);
    world.SyncActiveInHierarchy();
    world.RebuildWorldTransformCache();
    PhysicsWorld::Create(world);
    if (!PhysicsWorld::IsActive()) return;
    const RagdollSettingsComponent cfg;
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts));
    const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
    const int id = PhysicsWorld::CreateRagdoll(4346u, parts, NpcRagdoll::kRagParts, &prm);
    CHECK(id >= 0);
    if (id >= 0) {
        PhysicsWorld::SetRagdollDrive(id, 0.0f, 0.0f);
        // The lowest point of a part's capsule (axis along its local x).
        auto lowest = [&](int part) {
            float p[3], q[4];
            PhysicsWorld::GetRagdollPart(id, part, p, q);
            const glm::quat r(q[3], q[0], q[1], q[2]);
            return p[1] - std::fabs((r * glm::vec3(1, 0, 0)).y) * parts[part].HalfLength - parts[part].Radius;
        };
        float startLow = 1e9f;
        for (int i = NpcRagdoll::kHandL; i < NpcRagdoll::kRagParts; ++i) startLow = std::min(startLow, lowest(i));
        float deepest = 0.0f;
        for (int i = 0; i < 240; ++i) {
            PhysicsWorld::Step(1.0f / 60.0f, world, {});
            if (i >= 120)
                for (int k = NpcRagdoll::kHandL; k < NpcRagdoll::kRagParts; ++k) deepest = std::max(deepest, -lowest(k));
        }
        float neck[3], head[3], q[4];
        CHECK(PhysicsWorld::GetRagdollPart(id, NpcRagdoll::kNeck, neck, q) && PhysicsWorld::GetRagdollPart(id, 2, head, q));
        const float gap = glm::length(glm::vec3(neck[0] - head[0], neck[1] - head[1], neck[2] - head[2]));
        float footLow = 1e9f;
        for (int k : {NpcRagdoll::kFootL, NpcRagdoll::kFootR}) footLow = std::min(footLow, lowest(k));
        std::printf("[UnitTest] limp body on a floor: hands/feet deepest %.4f m below it, feet bottom %.3f m (started %.3f), neck-head gap %.2f m\n", deepest, footLow, startLow, gap);
        CHECK(deepest < 0.02f && std::isfinite(gap) && gap < 0.4f);
        PhysicsWorld::DestroyRagdoll(id);
    }
    PhysicsWorld::Destroy();
}

// Both settings components save and load, and an old scene (no such fields) loads as the defaults.
void TestRagdollAndSquadSettingsRoundTrip() {
    World world;
    AssetLibrary assets;
    const entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Rules");
    auto& rag = world.Registry.emplace<RagdollSettingsComponent>(e);
    rag.DriveFade = 0.9f; rag.CalfFlexMax = 100.0f; rag.PelvisMass = 20.0f; rag.AnatomicalLimits = false; rag.SolverPosIters = 30; rag.CorpseShotBase = 3.0f;
    auto& sq = world.Registry.emplace<SquadSettingsComponent>(e);
    sq.HeavyHitDamage = 77.0f; sq.CoverReach = 1.25f; sq.MeleeTime = 0.8f; sq.FallGravity = 9.81f; sq.CorpseTime = 99.0f; sq.HitboxRange = 30.0f;
    const std::string json = SceneSerializer::SaveToString(world, assets);
    World back;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(back, assets2, json));
    bool seen = false;
    for (entt::entity b : back.Registry.view<RagdollSettingsComponent>()) {
        const auto& r = back.Registry.get<RagdollSettingsComponent>(b);
        const auto* s = back.Registry.try_get<SquadSettingsComponent>(b);
        seen = true;
        CHECK(r.DriveFade == 0.9f && r.CalfFlexMax == 100.0f && r.PelvisMass == 20.0f && !r.AnatomicalLimits && r.SolverPosIters == 30 && r.CorpseShotBase == 3.0f);
        CHECK(r.ThighFlexMax == RagdollSettingsComponent().ThighFlexMax); // untouched fields keep their defaults
        CHECK(s && s->HeavyHitDamage == 77.0f && s->CoverReach == 1.25f && s->MeleeTime == 0.8f && s->FallGravity == 9.81f && s->CorpseTime == 99.0f && s->HitboxRange == 30.0f);
    }
    CHECK(seen);
    // A scene saved before these fields existed: Squad Settings with only the old ones.
    World old;
    AssetLibrary assets3;
    const std::string oldJson = std::string(R"({"formatVersion":3,"empties":[{"name":"S","id":1,"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1],)") +
                                R"("Squad Settings":{"Squad Size":3}}]})";
    CHECK(SceneSerializer::LoadFromString(old, assets3, oldJson));
    // Whatever the key spelling, an entity that has the component carries the original values in the fields it doesn't name.
    int oldFound = 0;
    for (entt::entity b : old.Registry.view<SquadSettingsComponent>()) {
        const auto& s = old.Registry.get<SquadSettingsComponent>(b);
        ++oldFound;
        CHECK(s.SquadSize == 3 && s.HeavyHitDamage == 40.0f && s.CorpseTime == 14.0f && s.CoverStep == 0.8f && s.FallGravity == 18.0f);
    }
    CHECK(oldFound == 1);
}

// The director takes the scene's settings (and the defaults without any).
void TestNpcDirectorReadsSettings() {
    NpcDirector director;
    World world;
    director.ApplySettings(world.Registry);
    CHECK(director.Settings().HeavyHitDamage == 40.0f && director.Settings().BleedOutTime == 20.0f && director.Settings().CoverReach == 0.85f);
    CHECK(director.RagdollSettings().DriveStiffness == 700.0f && director.RagdollSettings().AnatomicalLimits);
    const entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Rules");
    auto& sq = world.Registry.emplace<SquadSettingsComponent>(e);
    sq.HeavyHitDamage = 12.0f; sq.CorpseTime = 3.0f; sq.Difficulty = 1.5f;
    world.Registry.emplace<RagdollSettingsComponent>(e).DriveStiffness = 123.0f;
    director.ApplySettings(world.Registry);
    CHECK(director.Settings().HeavyHitDamage == 12.0f && director.Settings().CorpseTime == 3.0f);
    CHECK(director.Difficulty() == 1.5f);
    CHECK(director.RagdollSettings().DriveStiffness == 123.0f);
    world.Registry.clear();
    director.ApplySettings(world.Registry); // gone again: back to the defaults
    CHECK(director.Settings().HeavyHitDamage == 40.0f && director.RagdollSettings().DriveStiffness == 700.0f);
}

// A round that doesn't kill kicks the struck region along the round and the spring settles back; scaled by damage and region.
void TestNpcFlinchKicksAndSettles() {
    const RagdollSettingsComponent cfg;
    const glm::vec3 up(0, 1, 0), fwd(0, 0, 1);
    auto sp3 = [](const NpcFlinch& f, float t) { glm::vec3 r[NpcFlinch::kBones]; f.Rotations(t, r); return r[2]; }; // spine_03
    NpcFlinch f;
    f.Hit(1, up, fwd, 40.0f, 10.0f, cfg); // the chest, a round going +z
    CHECK(f.Count() == 2 && f.Active(10.0f) && !f.Active(10.0f + 1.5f * cfg.FlinchDuration + 0.01f));
    const float peakT = 10.0f + cfg.FlinchDuration / 5.5f;
    const glm::vec3 atPeak = sp3(f, peakT);
    const float want = NpcFlinch::PeakAngle(cfg, 40.0f, 1, 0.55f);
    std::printf("[UnitTest] chest flinch: spine_03 kicks %.2f deg at %.0f ms (wanted %.2f), %.2f deg at the duration, %.3f deg after\n", glm::degrees(glm::length(atPeak)),
                (peakT - 10.0f) * 1000.0f, glm::degrees(want), glm::degrees(glm::length(sp3(f, 10.0f + cfg.FlinchDuration))), glm::degrees(glm::length(sp3(f, 10.5f))));
    CHECK(glm::length(sp3(f, 10.0f)) == 0.0f);                   // nothing at the instant of the hit
    CHECK(std::fabs(glm::length(atPeak) - want) < 0.02f * want); // the peak is the peak angle
    CHECK(glm::length(sp3(f, 10.0f + cfg.FlinchDuration)) < 0.13f * want && glm::length(sp3(f, 10.5f)) < 0.01f * want); // and it is back
    // The bone's tip goes where the round did.
    const glm::quat q = glm::angleAxis(glm::length(atPeak), glm::normalize(atPeak));
    CHECK((q * up).z > 0.0f && std::fabs((q * up).x) < 1e-4f);
    // Heavier rounds kick harder (to a cap), a head or an arm further than the chest, off means off, a round along the bone does nothing.
    NpcFlinch light, heavy, huge, head, arm, off, along;
    light.Hit(1, up, fwd, 10.0f, 0.0f, cfg); heavy.Hit(1, up, fwd, 80.0f, 0.0f, cfg); head.Hit(2, up, fwd, 40.0f, 0.0f, cfg);
    arm.Hit(4, glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1, 0, 0), 40.0f, 0.0f, cfg);
    RagdollSettingsComponent disabled = cfg;
    disabled.HitFlinch = false;
    off.Hit(1, up, fwd, 40.0f, 0.0f, disabled);
    along.Hit(1, up, up, 40.0f, 0.0f, cfg);
    CHECK(off.Count() == 0 && along.Count() == 0);
    auto peak = [&](const NpcFlinch& n, int bone) { glm::vec3 r[NpcFlinch::kBones]; n.Rotations(cfg.FlinchDuration / 5.5f, r); return glm::length(r[bone]); };
    CHECK(peak(heavy, 2) > 6.0f * peak(light, 2) && peak(heavy, 2) > 1.9f * glm::length(atPeak)); // 80 vs 10 damage: x2 vs x0.3 of the reference
    CHECK(peak(head, 4) > glm::length(atPeak) && peak(arm, 7) > glm::length(atPeak)); // head and forearm kick further than the chest's spine_03
    std::printf("[UnitTest] flinch peaks (deg): light %.2f, 40 dmg %.2f, heavy %.2f, head %.2f, forearm %.2f\n", glm::degrees(peak(light, 2)), glm::degrees(glm::length(atPeak)),
                glm::degrees(peak(heavy, 2)), glm::degrees(peak(head, 4)), glm::degrees(peak(arm, 7)));
    // Rounds in a quick burst add up, to the cap.
    NpcFlinch burst;
    for (int i = 0; i < 20; ++i) burst.Hit(1, up, fwd, 80.0f, 0.0f, cfg);
    CHECK(peak(burst, 2) <= glm::radians(cfg.FlinchMaxAngle) + 1e-4f && peak(burst, 2) > peak(heavy, 2));
    // The duration and strength fields do what they say.
    RagdollSettingsComponent slow = cfg;
    slow.FlinchDuration = 0.6f; slow.FlinchAngle = 14.0f;
    NpcFlinch s2;
    s2.Hit(1, up, fwd, 40.0f, 0.0f, slow);
    glm::vec3 r2[NpcFlinch::kBones];
    s2.Rotations(0.6f / 5.5f, r2);
    CHECK(std::fabs(glm::length(r2[2]) - 2.0f * glm::length(atPeak)) < 0.03f * glm::length(atPeak) * 2.0f && s2.Active(0.8f) && !f.Active(10.5f));
}

// The new Ragdoll Settings fields save and load.
void TestRagdollNewFieldsRoundTrip() {
    World world;
    AssetLibrary assets;
    const entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Rules");
    auto& rag = world.Registry.emplace<RagdollSettingsComponent>(e);
    rag.HandMass = 0.9f; rag.FootExtMax = 66.0f; rag.NeckTwistOut = 22.0f; rag.FootFadeScale = 2.0f; rag.HitFlinch = false; rag.FlinchAngle = 11.0f; rag.FlinchDuration = 0.45f;
    const std::string json = SceneSerializer::SaveToString(world, assets);
    World back;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(back, assets2, json));
    int seen = 0;
    for (entt::entity b : back.Registry.view<RagdollSettingsComponent>()) {
        const auto& r = back.Registry.get<RagdollSettingsComponent>(b);
        ++seen;
        CHECK(r.HandMass == 0.9f && r.FootExtMax == 66.0f && r.NeckTwistOut == 22.0f && r.FootFadeScale == 2.0f && !r.HitFlinch && r.FlinchAngle == 11.0f && r.FlinchDuration == 0.45f);
        CHECK(r.NeckMass == RagdollSettingsComponent().NeckMass && r.FlinchMaxAngle == RagdollSettingsComponent().FlinchMaxAngle);
    }
    CHECK(seen == 1);
}

// A level with a lot of walls: the open edges are merged into whole walls (nothing left that joins end to start), and
// finding them is quick - the merge was cubic, and it ran on the first Play frame (~70 ms on the Sandbox).
void TestNavBoundaryEdgesMergeQuickly() {
    std::vector<float> verts;
    std::vector<int> tris;
    auto quadFloor = [&](float h) {
        const int b = (int)verts.size() / 3;
        const float v[4][3] = {{-70, 0, -70}, {-70, 0, 70}, {70, 0, 70}, {70, 0, -70}};
        for (auto& p : v) { verts.push_back(p[0]); verts.push_back(h); verts.push_back(p[2]); }
        for (int i : {0, 1, 2, 0, 2, 3}) tris.push_back(b + i);
    };
    quadFloor(0.0f);
    static const int f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                                 {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (int gx = 0; gx < 24; ++gx)
        for (int gz = 0; gz < 24; ++gz) {
            const float cx = -57.5f + 5.0f * (float)gx, cz = -57.5f + 5.0f * (float)gz, hs = 1.0f + 0.05f * (float)((gx + gz) % 5);
            const int b = (int)verts.size() / 3;
            for (int i = 0; i < 8; ++i) {
                verts.push_back(cx + ((i & 1) ? hs : -hs));
                verts.push_back((i & 2) ? 2.5f : 0.0f);
                verts.push_back(cz + ((i & 4) ? hs : -hs));
            }
            for (const auto& t : f) { tris.push_back(b + t[0]); tris.push_back(b + t[1]); tris.push_back(b + t[2]); }
        }
    NavMesh nav;
    std::string error;
    CHECK(nav.Build(verts, tris, NavBuildSettings(), &error));
    std::vector<NavMesh::Edge> edges;
    const auto t0 = std::chrono::steady_clock::now();
    nav.BoundaryEdges(edges);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(edges.size() >= 24 * 24 * 4);
    CHECK(ms < 250.0); // the cubic merge needed seconds here
    int joinable = 0;
    for (size_t i = 0; i < edges.size(); ++i)
        for (size_t j = 0; j < edges.size(); ++j) {
            if (i == j || glm::length(edges[i].B - edges[j].A) > 0.02f || glm::dot(edges[i].Out, edges[j].Out) < 0.999f) continue;
            if (glm::dot(glm::normalize(edges[i].B - edges[i].A), glm::normalize(edges[j].B - edges[j].A)) >= 0.999f) ++joinable;
        }
    CHECK(joinable == 0);
}

// ---- Powered ragdoll: kills on a floor, measured ------------------------------------------------------------------------------
// A bare ragdoll (the standing pose above) is killed on a floor the way NpcRagdoll::Start does it (Launch) and stepped for six seconds;
// the same kill runs with the old passive death (Powered Ragdoll off, the drives fade to nothing in 0.25 s) and the new powered one.

struct KillCase { const char* Name; int Hit; glm::vec3 Shot; glm::vec3 Run; };
struct KillStats {
    float MeanJointSpin = 0.0f;   // rad/s, mean over joints and the first second
    float PeakJointDeg100 = 0.0f; // degrees, the most any joint turned in 100 ms (first two seconds)
    float FallAngle = 0.0f;       // degrees between the pelvis's horizontal travel and the body's momentum
    float Travel = 0.0f;          // m, pelvis horizontal travel to rest
    float FallTime = 6.0f;        // s until the pelvis is below 0.3 m
    float SettleTime = 6.0f;      // s of the last part speed above 1 cm/s
    float JitterMm = 0.0f;        // RMS part motion (what a straight line through it leaves) in the second after the fastest part first drops under 5 cm/s
    float Slide = 0.0f;           // m the pelvis moves after the chest, head or pelvis first touches the floor
    float KneeL = 0.0f, KneeR = 0.0f; // degrees, each knee's flexion at rest (the angle between thigh and calf)
    float HipL = 0.0f, HipR = 0.0f;   // degrees, each hip's flexion at rest (the angle the thigh makes with the trunk's line, 0 = straight)
    float PelvisY = 0.0f;         // m, the pelvis's height at rest
    float TrunkUp = 0.0f, ThighUp = 0.0f, CalfUp = 0.0f; // the trunk's, thigh's and calf's bone direction's height component at rest (+1 up)
    float PoseEarly[4] = {};      // knees and hips (L, R) half a second into the fall
    float KneeAsymEarly = 0.0f;   // degrees, |left knee - right knee| flexion half a second into the fall
    float PartY[16] = {};         // m, every part's centre's height at rest
    float Pose[6] = {};           // the end pose's signature: knees, hips, the pelvis's lean, the spine's bend (degrees)
    bool Finite = true;
};

glm::quat PartQuat(int id, int part, glm::vec3* pos = nullptr) {
    float p[3], q[4];
    PhysicsWorld::GetRagdollPart(id, part, p, q);
    if (pos) *pos = glm::vec3(p[0], p[1], p[2]);
    return glm::quat(q[3], q[0], q[1], q[2]);
}

float AngleBetween(const glm::quat& a, const glm::quat& b) {
    const float d = std::fabs(glm::dot(a, b));
    return 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f));
}

// Main's death: the drives fade to nothing in a quarter second, the old body physics.
RagdollSettingsComponent PassiveCfg() {
    RagdollSettingsComponent c;
    c.PoweredRagdoll = false;
    return c;
}

KillStats KillOnAFloor(const RagdollSettingsComponent& cfg, const KillCase& kc) {
    KillStats st;
    World world;
    const entt::entity floorE = world.CreateEmptyEntity(glm::vec3(0.0f, -0.53f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Floor");
    ColliderComponent col;
    col.Kind = ColliderComponent::Shape::Box;
    col.HalfExtents = glm::vec3(40.0f, 0.5f, 40.0f);
    world.Registry.emplace<ColliderComponent>(floorE, col);
    world.SyncActiveInHierarchy();
    world.RebuildWorldTransformCache();
    PhysicsWorld::Create(world);
    if (!PhysicsWorld::IsActive()) return st;
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
    glm::mat4 partWorld[NpcRagdoll::kRagParts];
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), kc.Run, cfg, parts, partWorld));
    const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
    const int id = PhysicsWorld::CreateRagdoll(4350u, parts, NpcRagdoll::kRagParts, &prm);
    CHECK(id >= 0);
    if (id < 0) { PhysicsWorld::Destroy(); return st; }
    NpcRagdollMotor motor;
    const bool powered = cfg.PoweredRagdoll;
    const float impulseMag = 50.0f; // what the director gives a 50 damage round: 30 + 0.4 * damage
    const glm::vec3 impulse = glm::normalize(kc.Shot) * impulseMag;
    const glm::vec3 point(partWorld[kc.Hit][3]);
    if (!powered) PhysicsWorld::SetRagdollDrive(id, cfg.DriveStiffness, cfg.DriveDamping);
    NpcRagdoll::Launch(id, parts, partWorld, glm::mat4(1.0f), kc.Run, impulse, point, kc.Hit, cfg, powered ? &motor : nullptr);
    float mass = 0.0f;
    for (int i = 0; i < NpcRagdoll::kRagParts; ++i) mass += NpcRagdoll::PartMass(&cfg, i);
    const glm::vec3 momentum = kc.Run * mass + impulse * (powered ? cfg.HitImpulseScale : 1.0f);
    const glm::vec3 refDir = glm::normalize(glm::vec3(momentum.x, 0.0f, momentum.z));
    const float dt = 1.0f / 60.0f;
    const int steps = 360;
    std::vector<std::vector<glm::quat>> rel((size_t)steps + 1, std::vector<glm::quat>(NpcRagdoll::kRagParts));
    std::vector<std::vector<glm::vec3>> pos((size_t)steps + 1, std::vector<glm::vec3>(NpcRagdoll::kRagParts));
    auto sample = [&](int k) {
        for (int i = 0; i < NpcRagdoll::kRagParts; ++i) {
            const glm::quat q = PartQuat(id, i, &pos[(size_t)k][(size_t)i]);
            rel[(size_t)k][(size_t)i] = i == 0 ? q : glm::inverse(PartQuat(id, NpcRagdollDefOf(i).Parent)) * q;
        }
    };
    sample(0);
    const glm::vec3 start = pos[0][0];
    float firstContact = -1.0f, lastFast = 0.0f;
    glm::vec3 contactPos(0.0f);
    float spinSum = 0.0f;
    int spinN = 0, quietAt = -1;
    for (int k = 1; k <= steps; ++k) {
        const float t = (float)k * dt;
        if (powered) motor.Update(dt);
        else {
            for (int i = 1; i < NpcRagdoll::kRagParts; ++i) {
                const float f = NpcRagdoll::DriveAt(t, NpcRagdoll::PartFade(&cfg, i));
                const bool distal = i == NpcRagdoll::kHandL || i == NpcRagdoll::kHandR || i == NpcRagdoll::kFootL || i == NpcRagdoll::kFootR;
                PhysicsWorld::SetRagdollPartDrive(id, i, cfg.DriveStiffness * f * f, std::max(cfg.DriveDamping * f * f, distal ? cfg.DistalJointDamping : 0.0f));
            }
        }
        PhysicsWorld::Step(dt, world, {});
        sample(k);
        if (k == 30) {
            auto knee = [&](int thigh, int calf) { return glm::degrees(std::acos(std::clamp(glm::dot(DirOf(id, thigh), DirOf(id, calf)), -1.0f, 1.0f))); };
            st.KneeAsymEarly = std::fabs(knee(7, 8) - knee(9, 10));
            st.PoseEarly[0] = knee(7, 8); st.PoseEarly[1] = knee(9, 10);
            st.PoseEarly[2] = 180.0f - glm::degrees(std::acos(std::clamp(glm::dot(DirOf(id, 0), DirOf(id, 7)), -1.0f, 1.0f)));
            st.PoseEarly[3] = 180.0f - glm::degrees(std::acos(std::clamp(glm::dot(DirOf(id, 0), DirOf(id, 9)), -1.0f, 1.0f)));
        }
        float lin = 0.0f, ang = 0.0f;
        PhysicsWorld::RagdollMotion(id, &lin, &ang);
        if (lin > 0.01f) lastFast = t;
        if (quietAt < 0 && t > 0.3f && lin < 0.05f) quietAt = k;
        if (!std::isfinite(lin) || !std::isfinite(pos[(size_t)k][0].x)) st.Finite = false;
        if (t <= 1.0f)
            for (int i = 1; i < NpcRagdoll::kRagParts; ++i) { spinSum += AngleBetween(rel[(size_t)k][(size_t)i], rel[(size_t)k - 1][(size_t)i]) / dt; ++spinN; }
        if (k >= 6 && t <= 2.0f)
            for (int i = 1; i < NpcRagdoll::kRagParts; ++i)
                st.PeakJointDeg100 = std::max(st.PeakJointDeg100, glm::degrees(AngleBetween(rel[(size_t)k][(size_t)i], rel[(size_t)k - 6][(size_t)i])));
        if (st.FallTime >= 6.0f && pos[(size_t)k][0].y < 0.3f) st.FallTime = t;
        if (firstContact < 0.0f)
            for (int i : {0, 1, 2}) {
                const glm::quat q = PartQuat(id, i);
                const float low = pos[(size_t)k][(size_t)i].y - std::fabs((q * glm::vec3(1, 0, 0)).y) * parts[i].HalfLength - parts[i].Radius;
                if (low < 0.0f) { firstContact = t; contactPos = pos[(size_t)k][0]; break; }
            }
    }
    {
        auto angle = [&](int a, int b) { return glm::degrees(std::acos(std::clamp(glm::dot(DirOf(id, a), DirOf(id, b)), -1.0f, 1.0f))); };
        st.KneeL = angle(7, 8); st.KneeR = angle(9, 10);
        st.HipL = 180.0f - angle(0, 7); st.HipR = 180.0f - angle(0, 9);
        st.PelvisY = pos[(size_t)steps][0].y;
        for (int i = 0; i < NpcRagdoll::kRagParts; ++i) st.PartY[i] = pos[(size_t)steps][(size_t)i].y;
        st.Pose[0] = st.KneeL; st.Pose[1] = st.KneeR; st.Pose[2] = st.HipL; st.Pose[3] = st.HipR;
        st.Pose[4] = 90.0f - glm::degrees(std::acos(std::clamp(DirOf(id, 0).y, -1.0f, 1.0f)));
        st.Pose[5] = angle(0, 1);
        st.TrunkUp = DirOf(id, 0).y; st.ThighUp = DirOf(id, 7).y; st.CalfUp = DirOf(id, 8).y;
    }
    st.MeanJointSpin = spinN ? spinSum / (float)spinN : 0.0f;
    st.SettleTime = lastFast;
    const glm::vec3 end = pos[(size_t)steps][0];
    const glm::vec3 travel(end.x - start.x, 0.0f, end.z - start.z);
    st.Travel = glm::length(travel);
    st.FallAngle = st.Travel > 1e-3f ? glm::degrees(std::acos(std::clamp(glm::dot(travel / st.Travel, refDir), -1.0f, 1.0f))) : 180.0f;
    st.Slide = firstContact >= 0.0f ? glm::length(glm::vec3(end.x - contactPos.x, 0.0f, end.z - contactPos.z)) : 0.0f;
    double sq = 0.0;
    const int w0 = quietAt < 0 ? steps - 60 : std::min(quietAt, steps - 60), w1 = std::min(w0 + 60, steps), n = w1 - w0 + 1;
    for (int i = 0; i < NpcRagdoll::kRagParts; ++i)
        for (int axis = 0; axis < 3; ++axis) { // least-squares line through the window, per axis
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int k = w0; k <= w1; ++k) { const double x = k - w0, y = pos[(size_t)k][(size_t)i][axis]; sx += x; sy += y; sxx += x * x; sxy += x * y; }
            const double den = n * sxx - sx * sx, b = den > 0 ? (n * sxy - sx * sy) / den : 0.0, a = (sy - b * sx) / n;
            for (int k = w0; k <= w1; ++k) { const double r = pos[(size_t)k][(size_t)i][axis] - (a + b * (k - w0)); sq += r * r; }
        }
    st.JitterMm = 1000.0f * (float)std::sqrt(sq / (double)(n * NpcRagdoll::kRagParts));
    PhysicsWorld::DestroyRagdoll(id);
    PhysicsWorld::Destroy();
    return st;
}

const std::vector<KillCase>& KillCases() {
    static const std::vector<KillCase> cases = {
        {"stand head front", 2, {0, 0, -1}, {0, 0, 0}}, {"stand head side", 2, {1, 0, 0}, {0, 0, 0}}, {"stand head back", 2, {0, 0, 1}, {0, 0, 0}},
        {"stand chest front", 1, {0, 0, -1}, {0, 0, 0}}, {"stand chest side", 1, {-1, 0, 0}, {0, 0, 0}}, {"stand chest back", 1, {0, 0, 1}, {0, 0, 0}},
        {"stand thigh front", 7, {0, 0, -1}, {0, 0, 0}}, {"stand thigh side", 7, {1, 0, 0}, {0, 0, 0}}, {"stand thigh back", 9, {0, 0, 1}, {0, 0, 0}},
        {"run chest front", 1, {0, 0, -1}, {0, 0, 4}}, {"run chest side", 1, {1, 0, 0}, {0, 0, 4}}, {"run chest back", 1, {0, 0, 1}, {0, 0, 4}},
    };
    return cases;
}

struct KillSummary { KillStats Mean, Worst; int N = 0; };
KillSummary SummariseKills(const RagdollSettingsComponent& cfg, bool running, bool print, const char* label) {
    KillSummary s;
    s.Mean.FallTime = s.Mean.SettleTime = s.Worst.FallTime = s.Worst.SettleTime = 0.0f;
    for (const KillCase& kc : KillCases()) {
        const bool isRun = glm::length(kc.Run) > 0.1f;
        if (isRun != running) continue;
        const KillStats k = KillOnAFloor(cfg, kc);
        if (print)
            std::printf("[UnitTest]   %-4s %-18s spin %5.2f rad/s  peak %5.1f deg/100ms  fall %5.1f deg  travel %4.2f m  down %4.2f s  settle %4.2f s  jitter %5.2f mm  slide %4.2f m  rest: knees %3.0f/%3.0f hips %3.0f/%3.0f pelvis %4.2f m  dirs-y trunk %5.2f thigh %5.2f calf %5.2f\n",
                        label, kc.Name, k.MeanJointSpin, k.PeakJointDeg100, k.FallAngle, k.Travel, k.FallTime, k.SettleTime, k.JitterMm, k.Slide, k.KneeL, k.KneeR, k.HipL, k.HipR, k.PelvisY, k.TrunkUp, k.ThighUp, k.CalfUp);
        s.Mean.MeanJointSpin += k.MeanJointSpin; s.Mean.PeakJointDeg100 += k.PeakJointDeg100; s.Mean.FallAngle += k.FallAngle; s.Mean.Travel += k.Travel;
        s.Mean.FallTime += k.FallTime; s.Mean.SettleTime += k.SettleTime; s.Mean.JitterMm += k.JitterMm; s.Mean.Slide += k.Slide;
        s.Worst.SettleTime = std::max(s.Worst.SettleTime, k.SettleTime); s.Worst.JitterMm = std::max(s.Worst.JitterMm, k.JitterMm);
        s.Worst.Slide = std::max(s.Worst.Slide, k.Slide); s.Worst.FallAngle = std::max(s.Worst.FallAngle, k.FallAngle);
        s.Worst.Finite = s.Worst.Finite && k.Finite;
        ++s.N;
    }
    const float n = (float)std::max(s.N, 1);
    s.Mean.MeanJointSpin /= n; s.Mean.PeakJointDeg100 /= n; s.Mean.FallAngle /= n; s.Mean.Travel /= n; s.Mean.FallTime /= n;
    s.Mean.SettleTime /= n; s.Mean.JitterMm /= n; s.Mean.Slide /= n;
    return s;
}

// The old passive death against the powered one, over twelve kills (standing and running; head, chest and leg hits; front, side, back).
void TestPoweredRagdollDeathsAgainstPassive() {
    const RagdollSettingsComponent powered, passive = PassiveCfg();
    const KillSummary bs = SummariseKills(passive, false, true, "old"), ns = SummariseKills(powered, false, true, "new");
    const KillSummary br = SummariseKills(passive, true, true, "old"), nr = SummariseKills(powered, true, true, "new");
    if (bs.N == 0) return; // no physics
    auto row = [](const char* what, const KillStats& o, const KillStats& n) {
        std::printf("[UnitTest] %-8s spin %5.2f -> %5.2f rad/s | peak %5.1f -> %5.1f deg/100ms | fall angle %5.1f -> %5.1f deg | travel %4.2f -> %4.2f m | down %4.2f -> %4.2f s | settle %4.2f -> %4.2f s | jitter %5.2f -> %5.2f mm | slide %4.2f -> %4.2f m\n",
                    what, o.MeanJointSpin, n.MeanJointSpin, o.PeakJointDeg100, n.PeakJointDeg100, o.FallAngle, n.FallAngle, o.Travel, n.Travel, o.FallTime, n.FallTime,
                    o.SettleTime, n.SettleTime, o.JitterMm, n.JitterMm, o.Slide, n.Slide);
    };
    row("standing", bs.Mean, ns.Mean);
    row("running", br.Mean, nr.Mean);
    CHECK(ns.Worst.Finite && nr.Worst.Finite);
    // Less noodle, not frozen: the joints turn slower than the passive rag's and the peak 100 ms swing is smaller, but they do move.
    CHECK(ns.Mean.MeanJointSpin < 0.7f * bs.Mean.MeanJointSpin && ns.Mean.MeanJointSpin > 0.3f && nr.Mean.MeanJointSpin < 0.9f * br.Mean.MeanJointSpin);
    CHECK(ns.Mean.PeakJointDeg100 < 0.8f * bs.Mean.PeakJointDeg100 && nr.Mean.PeakJointDeg100 < 0.8f * br.Mean.PeakJointDeg100 && ns.Mean.PeakJointDeg100 > 10.0f);
    // It goes down along the shot, not straight down, and it takes its time doing it.
    CHECK(ns.Mean.FallAngle < 35.0f && nr.Mean.FallAngle < 35.0f && ns.Mean.FallAngle < 0.5f * bs.Mean.FallAngle);
    CHECK(ns.Mean.Travel > 1.15f * bs.Mean.Travel);
    CHECK(ns.Mean.FallTime > 0.6f && ns.Mean.FallTime < 1.2f);
    // And it comes to rest: still within 2.5 s, no jitter, no skating.
    CHECK(ns.Worst.SettleTime < 3.0f && nr.Worst.SettleTime < 3.5f); // (a body still up on its knees is let down first: a little longer than the revamp's 2.5)
    CHECK(ns.Worst.JitterMm < 1.2f && nr.Worst.JitterMm < 1.2f);
    CHECK(ns.Mean.Slide < 0.16f && bs.Mean.Slide > 0.30f && nr.Mean.Slide < 0.7f * br.Mean.Slide);
    CHECK(bs.Worst.SettleTime > ns.Worst.SettleTime); // the old one was slower to rest
}

// The muscles' strength: legs start partial and hold a beat, then each region decays in its own time (legs first, then spine, neck and arms)
// to a residual that is never zero; the struck joint is weaker.
void TestRagdollMuscleToneCurves() {
    RagdollSettingsComponent c;
    c.RelaxTone = 0.0f; // the decay curves and the collapse pose (the lying phase is measured in TestRagdollEndsLyingNotKneeling)
    using M = NpcRagdollMotor;
    const int kThighL = 7, kCalfL = 8, kSpine = 1, kNeck = NpcRagdoll::kNeck, kUpperArmL = 3;
    // At the moment of death: the legs partial (they give out), the rest at full strength.
    CHECK(std::fabs(M::PartStrength(c, kThighL, 0.0f, -1) - (c.ToneResidual + (1.0f - c.ToneResidual) * c.StaggerLegStrength)) < 1e-5f);
    CHECK(std::fabs(M::PartStrength(c, kSpine, 0.0f, -1) - 1.0f) < 1e-5f && std::fabs(M::PartStrength(c, kUpperArmL, 0.0f, -1) - 1.0f) < 1e-5f);
    // The legs hold through the stagger, then fade.
    CHECK(M::PartStrength(c, kThighL, c.StaggerTime * 0.99f, -1) == M::PartStrength(c, kThighL, 0.0f, -1));
    CHECK(M::PartStrength(c, kThighL, c.StaggerTime + 0.2f, -1) < M::PartStrength(c, kThighL, 0.0f, -1));
    // Legs give out before the spine, the spine before the neck and arms.
    const float t = c.StaggerTime + 0.5f;
    CHECK(M::PartStrength(c, kCalfL, t, -1) < M::PartStrength(c, kSpine, t, -1));
    CHECK(M::PartStrength(c, kSpine, t, -1) < M::PartStrength(c, kNeck, t, -1) + 1e-6f);
    CHECK(M::PartStrength(c, kSpine, 1.0f, -1) < M::PartStrength(c, kUpperArmL, 1.0f, -1));
    // Monotone decay to the residual, which is never zero.
    for (int part = 1; part < NpcRagdoll::kRagParts; ++part) {
        float last = 2.0f;
        for (float tt = 0.0f; tt < 4.0f; tt += 0.05f) {
            const float s = M::PartStrength(c, part, tt, -1);
            CHECK(s <= last + 1e-6f && s >= c.ToneResidual - 1e-6f);
            last = s;
        }
        CHECK(std::fabs(M::PartStrength(c, part, 30.0f, -1) - c.ToneResidual) < 1e-5f && c.ToneResidual > 0.0f);
    }
    // The struck joint gives way: weaker than the same joint unstruck; a hit pelvis weakens both hips, a hit head the neck too.
    CHECK(M::PartStrength(c, kSpine, 0.0f, kSpine) < 0.5f * M::PartStrength(c, kSpine, 0.0f, -1));
    CHECK(M::PartStrength(c, 7, 0.0f, 0) < M::PartStrength(c, 7, 0.0f, -1) && M::PartStrength(c, 9, 0.0f, 0) < M::PartStrength(c, 9, 0.0f, -1));
    CHECK(M::PartStrength(c, kNeck, 0.0f, 2) < M::PartStrength(c, kNeck, 0.0f, -1) && M::PartStrength(c, 3, 0.0f, 2) == M::PartStrength(c, 3, 0.0f, -1));
    // The collapse pose: knees and hips fold, the target eases in over the blend time, and zero amount keeps the death pose.
    const glm::quat k0 = M::TargetAt(c, kCalfL, 0.0f, true), kEnd = M::TargetAt(c, kCalfL, 5.0f, true);
    CHECK(glm::degrees(AngleBetween(k0, glm::quat(1, 0, 0, 0))) < 1e-3f);
    CHECK(std::fabs(glm::degrees(AngleBetween(kEnd, glm::quat(1, 0, 0, 0))) - c.KneeFlexCollapse) < 0.1f);
    const glm::quat mid = M::TargetAt(c, kCalfL, c.CollapseBlendTime * 0.5f, true);
    CHECK(mid.w > kEnd.w && mid.w < 1.0f);
    RagdollSettingsComponent hold = c;
    hold.CollapseAmount = 0.0f;
    CHECK(AngleBetween(M::TargetAt(hold, kCalfL, 5.0f, true), glm::quat(1, 0, 0, 0)) < 1e-4f);
    // Falling forward the spine curls forward and the arms brace; falling back the spine arches a little and the neck tucks.
    CHECK(M::CollapseFlexion(c, kSpine, true) > 0.0f && M::CollapseFlexion(c, kSpine, false) < 0.0f);
    CHECK(M::CollapseFlexion(c, kUpperArmL, true) > M::CollapseFlexion(c, kUpperArmL, false));
    CHECK(M::CollapseFlexion(c, kNeck, false) > 0.0f && M::CollapseFlexion(c, kNeck, true) < 0.0f);
}

// The joints really chase the target: with the muscles at full strength, a body that dies standing folds its knees toward the collapse pose
// (aiming for the death pose instead, they stay straight).
void TestRagdollMusclesFoldTheKnees() {
    auto kneeAfter = [&](const RagdollSettingsComponent& cfg, float seconds, float* pelvisY) {
        World world;
        const entt::entity floorE = world.CreateEmptyEntity(glm::vec3(0.0f, -0.53f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Floor");
        ColliderComponent col;
        col.Kind = ColliderComponent::Shape::Box;
        col.HalfExtents = glm::vec3(40.0f, 0.5f, 40.0f);
        world.Registry.emplace<ColliderComponent>(floorE, col);
        world.SyncActiveInHierarchy();
        world.RebuildWorldTransformCache();
        PhysicsWorld::Create(world);
        float knee = -1.0f;
        if (!PhysicsWorld::IsActive()) return knee;
        PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
        glm::mat4 pw[NpcRagdoll::kRagParts];
        CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts, pw));
        const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
        const int id = PhysicsWorld::CreateRagdoll(4351u, parts, NpcRagdoll::kRagParts, &prm);
        CHECK(id >= 0);
        NpcRagdollMotor motor;
        NpcRagdoll::Launch(id, parts, pw, glm::mat4(1.0f), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0, 1, 0), 1, cfg, cfg.PoweredRagdoll ? &motor : nullptr);
        if (!cfg.PoweredRagdoll) PhysicsWorld::SetRagdollDrive(id, 0.0f, 0.0f);
        for (int k = 0; k < (int)(seconds * 60.0f); ++k) {
            if (cfg.PoweredRagdoll) motor.Update(1.0f / 60.0f);
            PhysicsWorld::Step(1.0f / 60.0f, world, {});
        }
        glm::vec3 p;
        PartQuat(id, 0, &p);
        if (pelvisY) *pelvisY = p.y;
        knee = glm::degrees(std::acos(std::clamp(glm::dot(DirOf(id, 7), DirOf(id, 8)), -1.0f, 1.0f)));
        PhysicsWorld::DestroyRagdoll(id);
        PhysicsWorld::Destroy();
        return knee;
    };
    RagdollSettingsComponent strong; // full strength, held: only the target differs from standing
    strong.StaggerLegStrength = 1.0f; strong.StaggerTime = 5.0f; strong.HitWeakness = 1.0f; strong.SettleDelay = 50.0f;
    strong.CollapseBlendTime = 0.4f;
    strong.RelaxTone = 0.0f; // (the lying phase would extend the knees again)
    float yStrong = 0.0f;
    const float kneeStrong = kneeAfter(strong, 0.8f, &yStrong);
    if (kneeStrong < 0.0f) return;
    std::printf("[UnitTest] knee fold at 0.8 s: %.1f deg (target %.0f), pelvis %.2f m\n", kneeStrong, strong.KneeFlexCollapse, yStrong);
    CHECK(kneeStrong > 0.5f * strong.KneeFlexCollapse); // (gravity may fold them further than the target)
    RagdollSettingsComponent still = strong;
    still.CollapseAmount = 0.0f; // aim for the death pose: the knees stay straight
    CHECK(kneeAfter(still, 0.8f, nullptr) < 0.3f * strong.KneeFlexCollapse);
}

// A body that has come to rest is put to sleep and stays so (no drive write wakes it) until something hits it.
void TestRagdollSettlesAndSleeps() {
    World world;
    const entt::entity floorE = world.CreateEmptyEntity(glm::vec3(0.0f, -0.53f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Floor");
    ColliderComponent col;
    col.Kind = ColliderComponent::Shape::Box;
    col.HalfExtents = glm::vec3(40.0f, 0.5f, 40.0f);
    world.Registry.emplace<ColliderComponent>(floorE, col);
    world.SyncActiveInHierarchy();
    world.RebuildWorldTransformCache();
    PhysicsWorld::Create(world);
    if (!PhysicsWorld::IsActive()) return;
    const RagdollSettingsComponent cfg;
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
    glm::mat4 pw[NpcRagdoll::kRagParts];
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts, pw));
    const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
    const int id = PhysicsWorld::CreateRagdoll(4352u, parts, NpcRagdoll::kRagParts, &prm);
    CHECK(id >= 0);
    NpcRagdollMotor motor;
    NpcRagdoll::Launch(id, parts, pw, glm::mat4(1.0f), glm::vec3(0.0f), glm::vec3(0, 0, -50), glm::vec3(0, 1.3f, 0), 1, cfg, &motor);
    CHECK(motor.Settle() == 0.0f && !motor.ForcedToRest());
    float settledAt = -1.0f;
    for (int k = 0; k < 480; ++k) {
        motor.Update(1.0f / 60.0f);
        PhysicsWorld::Step(1.0f / 60.0f, world, {});
        if (settledAt < 0.0f && PhysicsWorld::RagdollAsleep(id)) settledAt = (float)k / 60.0f;
    }
    std::printf("[UnitTest] body asleep after %.2f s, settle %.2f, strength left %.3f\n", settledAt, motor.Settle(), motor.Strength());
    CHECK(settledAt > 0.0f && settledAt < 3.0f);
    CHECK(motor.Strength() >= cfg.ToneResidual * 0.99f && motor.Strength() < 0.2f); // limp, but never fully
    for (int k = 0; k < 60; ++k) { motor.Update(1.0f / 60.0f); PhysicsWorld::Step(1.0f / 60.0f, world, {}); }
    CHECK(PhysicsWorld::RagdollAsleep(id)); // and it stays so
    // A shot into the corpse wakes it, and it comes to rest again.
    const float j[3] = {0, 0, 30.0f}, at[3] = {0, 0.2f, 0};
    PhysicsWorld::RagdollImpulse(id, 0, j, at);
    motor.Wake();
    CHECK(!PhysicsWorld::RagdollAsleep(id) && motor.Settle() == 0.0f);
    for (int k = 0; k < 420; ++k) { motor.Update(1.0f / 60.0f); PhysicsWorld::Step(1.0f / 60.0f, world, {}); }
    CHECK(PhysicsWorld::RagdollAsleep(id));
    PhysicsWorld::DestroyRagdoll(id);
    PhysicsWorld::Destroy();
}

// Powered Ragdoll off keeps the old body (no grip, no stabilization); on, the new body physics reach PhysX's parameters. The new fields save and load.
void TestRagdollPoweredSettings() {
    RagdollSettingsComponent off;
    off.PoweredRagdoll = false;
    CHECK(!NpcRagdoll::BodyParams(off).Grip && NpcRagdoll::BodyParams(off).StabilizationThreshold < 0.0f);
    const RagdollSettingsComponent on;
    CHECK(on.PoweredRagdoll && NpcRagdoll::BodyParams(on).Grip && std::fabs(NpcRagdoll::BodyParams(on).StabilizationThreshold - on.StabilizationThreshold) < 1e-6f);
    World world;
    AssetLibrary assets;
    const entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Rules");
    auto& rag = world.Registry.emplace<RagdollSettingsComponent>(e);
    rag.PoweredRagdoll = false; rag.ToneStiffness = 321.0f; rag.LegsToneTime = 0.33f; rag.StaggerLegStrength = 0.5f; rag.HitBodyShare = 0.25f; rag.SettleFriction = 3.0f; rag.DownHeight = 0.4f; rag.GripFloor = false;
    const std::string json = SceneSerializer::SaveToString(world, assets);
    World back;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(back, assets2, json));
    int seen = 0;
    for (entt::entity b : back.Registry.view<RagdollSettingsComponent>()) {
        const auto& r = back.Registry.get<RagdollSettingsComponent>(b);
        ++seen;
        CHECK(!r.PoweredRagdoll && r.ToneStiffness == 321.0f && r.LegsToneTime == 0.33f && r.StaggerLegStrength == 0.5f && r.HitBodyShare == 0.25f && r.SettleFriction == 3.0f && r.DownHeight == 0.4f && !r.GripFloor);
        CHECK(r.SpineToneTime == RagdollSettingsComponent().SpineToneTime && r.HitImpulseScale == RagdollSettingsComponent().HitImpulseScale);
    }
    CHECK(seen == 1);
    // A scene saved before the powered ragdoll existed loads with it on (the new default).
    World old;
    AssetLibrary assets3;
    const std::string oldJson = std::string(R"({"formatVersion":3,"empties":[{"name":"S","id":1,"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1],)") +
                                R"("Ragdoll Settings":{"Drive Fade":0.5}}]})";
    CHECK(SceneSerializer::LoadFromString(old, assets3, oldJson));
    int found = 0;
    for (entt::entity b : old.Registry.view<RagdollSettingsComponent>()) { ++found; CHECK(old.Registry.get<RagdollSettingsComponent>(b).PoweredRagdoll); }
    CHECK(found == 1);
}

// ---- Powered ragdoll polish: lying down, directional falls, reactions, corpse hits ----------------------------------------------
// The behaviour of this round switched off (the revamp's death: the target stays at the collapse pose, both knees fold alike whichever way the
// body falls, no reactions, a body at rest is put to sleep however it lies).
RagdollSettingsComponent RevampCfg() {
    RagdollSettingsComponent c;
    c.RelaxTone = 0.0f; c.RestFixTime = 0.0f;
    c.BuckleAsymmetry = 0.0f; c.BuckleLead = 0.0f; c.BackKneeScale = 1.0f; c.BackHipScale = 1.0f;
    c.BraceWeight = 0.0f; c.WoundGrabWeight = 0.0f; c.HeadTuckWeight = 0.0f;
    return c;
}

// A bare ragdoll killed as `kc` (the way KillOnAFloor does) and its parts' positions at each of `seconds` (ascending); `fall` gets the motor's reading of the fall.
std::vector<std::vector<glm::vec3>> KillPositions(const RagdollSettingsComponent& cfg, const KillCase& kc, const std::vector<float>& seconds, NpcRagdollMotor::Fall* fall = nullptr) {
    std::vector<std::vector<glm::vec3>> out;
    World world;
    const entt::entity floorE = world.CreateEmptyEntity(glm::vec3(0.0f, -0.53f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Floor");
    ColliderComponent col;
    col.Kind = ColliderComponent::Shape::Box;
    col.HalfExtents = glm::vec3(40.0f, 0.5f, 40.0f);
    world.Registry.emplace<ColliderComponent>(floorE, col);
    world.SyncActiveInHierarchy();
    world.RebuildWorldTransformCache();
    PhysicsWorld::Create(world);
    if (!PhysicsWorld::IsActive()) return out;
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
    glm::mat4 partWorld[NpcRagdoll::kRagParts];
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), kc.Run, cfg, parts, partWorld));
    const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
    const int id = PhysicsWorld::CreateRagdoll(4350u, parts, NpcRagdoll::kRagParts, &prm);
    CHECK(id >= 0);
    if (id < 0) { PhysicsWorld::Destroy(); return out; }
    NpcRagdollMotor motor;
    NpcRagdoll::Launch(id, parts, partWorld, glm::mat4(1.0f), kc.Run, glm::normalize(kc.Shot) * 50.0f, glm::vec3(partWorld[kc.Hit][3]), kc.Hit, cfg, &motor);
    if (fall) *fall = motor.FallState();
    const float dt = 1.0f / 60.0f;
    size_t next = 0;
    for (int k = 1; next < seconds.size(); ++k) {
        motor.Update(dt);
        PhysicsWorld::Step(dt, world, {});
        if ((float)k * dt + 1e-4f >= seconds[next]) {
            std::vector<glm::vec3> p((size_t)NpcRagdoll::kRagParts);
            for (int i = 0; i < NpcRagdoll::kRagParts; ++i) PartQuat(id, i, &p[(size_t)i]);
            out.push_back(p);
            ++next;
        }
    }
    PhysicsWorld::DestroyRagdoll(id);
    PhysicsWorld::Destroy();
    return out;
}

// A body that dies does not end kneeling, curled up or propped on its toes and hands: the target keeps going to a lying pose, held by the legs
// while it forms, and a body still up on its knees or arms is not settled and not put to sleep yet. Measured over the twelve kills, with the
// revamp's death against this one.
void TestRagdollEndsLyingNotKneeling() {
    struct Rest { float KneeMean = 0, KneeMax = 0, HipMean = 0, PelvisMax = 0, ChestSlide = 0; int Folded = 0, Propped = 0, N = 0; float Settle = 0, Jitter = 0; };
    auto measure = [](const RagdollSettingsComponent& cfg) {
        Rest r;
        for (const KillCase& kc : KillCases()) {
            const KillStats k = KillOnAFloor(cfg, kc);
            r.KneeMean += 0.5f * (k.KneeL + k.KneeR); r.KneeMax = std::max({r.KneeMax, k.KneeL, k.KneeR});
            r.HipMean += 0.5f * (k.HipL + k.HipR);
            r.PelvisMax = std::max(r.PelvisMax, k.PelvisY);
            r.Folded += std::max(k.KneeL, k.KneeR) > 45.0f ? 1 : 0;
            r.Propped += k.PelvisY > 0.22f ? 1 : 0;
            r.Settle = std::max(r.Settle, k.SettleTime); r.Jitter = std::max(r.Jitter, k.JitterMm);
            ++r.N;
        }
        r.KneeMean /= (float)std::max(r.N, 1); r.HipMean /= (float)std::max(r.N, 1);
        return r;
    };
    const Rest before = measure(RevampCfg()), after = measure(RagdollSettingsComponent());
    if (before.N == 0) return; // no physics
    std::printf("[UnitTest] rest pose over 12 kills, revamp -> lying: knee mean %.1f -> %.1f deg, worst %.0f -> %.0f | hip mean %.1f -> %.1f | pelvis worst %.2f -> %.2f m | knees > 45 deg: %d -> %d, pelvis > 0.22 m: %d -> %d | settle worst %.2f -> %.2f s, jitter %.2f -> %.2f mm\n",
                before.KneeMean, after.KneeMean, before.KneeMax, after.KneeMax, before.HipMean, after.HipMean, before.PelvisMax, after.PelvisMax, before.Folded, after.Folded,
                before.Propped, after.Propped, before.Settle, after.Settle, before.Jitter, after.Jitter);
    CHECK(before.Folded >= 2 && before.Propped >= 1); // the kneeling the video showed
    CHECK(after.Folded == 0 && after.KneeMax < 45.0f && after.Propped == 0 && after.PelvisMax < 0.22f);
    CHECK(after.KneeMean < 0.8f * before.KneeMean && after.HipMean < 0.7f * before.HipMean);
    CHECK(after.Settle < 3.0f && after.Jitter < 1.2f); // and it still comes to rest, without jitter
    // The lying pose in the target: knees and hips back to a slight bend, the arms most of the way to hanging; zero Relax Tone leaves the collapse pose.
    using M = NpcRagdollMotor;
    const RagdollSettingsComponent c;
    const int kThighL = 7, kCalfL = 8;
    CHECK(std::fabs(glm::degrees(AngleBetween(M::TargetAt(c, kCalfL, 5.0f, true), glm::quat(1, 0, 0, 0))) - c.LyingKnee) < 0.1f);
    CHECK(std::fabs(glm::degrees(AngleBetween(M::TargetAt(c, kThighL, 5.0f, true), glm::quat(1, 0, 0, 0))) - c.LyingHip) < 0.1f);
    CHECK(M::RelaxAmount(c, 0.0f) == 0.0f && M::RelaxAmount(c, c.RelaxDelay + c.RelaxTime) > 0.999f && M::RelaxAmount(RevampCfg(), 5.0f) == 0.0f);
    CHECK(std::fabs(glm::degrees(AngleBetween(M::TargetAt(RevampCfg(), kCalfL, 5.0f, true), glm::quat(1, 0, 0, 0))) - c.KneeFlexCollapse) < 0.1f);
    // The legs hold the lying pose while it forms (a plateau at Relax Tone), then let go; the arms are not held (they would prop a prone body).
    CHECK(std::fabs(M::RelaxStrength(c, c.RelaxDelay + c.RelaxTime) - c.RelaxTone) < 1e-4f && M::RelaxStrength(c, 30.0f) == 0.0f);
    CHECK(M::PartStrength(c, kCalfL, 1.0f, -1) > 0.9f * c.RelaxTone && M::PartStrength(c, 3, 2.5f, -1) < 0.2f);
}

// A fall's direction shapes the legs: the one on the side the body falls toward buckles first and folds further, a fall back folds the knees
// less and sits down, a fall forward folds them; falling toward the shot's line still holds (< 35 degrees). Measured over eight shots round the body.
void TestRagdollKneesBuckleWithTheShot() {
    using M = NpcRagdollMotor;
    const RagdollSettingsComponent c;
    const int kThighL = 7, kCalfL = 8, kCalfR = 10;
    M::Fall left; // falling toward the body's left
    left.Fwd = 0.0f; left.Lat = 1.0f;
    CHECK(M::LegLead(left, kCalfL) > 0.9f && M::LegLead(left, kCalfR) < -0.9f && M::LegLead(left, 3) == 0.0f);
    CHECK(M::CollapseFlexion(c, kCalfL, left) > 1.3f * M::CollapseFlexion(c, kCalfR, left)); // the lead knee folds further
    CHECK(M::PartStrength(c, kCalfL, c.StaggerTime + 0.1f, -1, &left) < M::PartStrength(c, kCalfR, c.StaggerTime + 0.1f, -1, &left)); // and gives out first
    M::Fall right = left;
    right.Lat = -1.0f;
    CHECK(M::CollapseFlexion(c, kCalfR, right) > 1.3f * M::CollapseFlexion(c, kCalfL, right)); // mirrored
    M::Fall fwd, back;
    back.Fwd = -1.0f;
    CHECK(M::CollapseFlexion(c, kCalfL, back) < 0.6f * M::CollapseFlexion(c, kCalfL, fwd)); // a back fall folds the knees less ...
    CHECK(M::CollapseFlexion(c, kThighL, back) > 1.3f * M::CollapseFlexion(c, kThighL, fwd)); // ... and sits (the legs go forward)
    CHECK(M::CollapseFlexion(c, kCalfL, fwd) == c.KneeFlexCollapse && M::CollapseFlexion(c, kCalfL, fwd) == M::CollapseFlexion(c, kCalfR, fwd) + 0.0f);
    M::Fall lean = fwd; // straight on: the body's own lean breaks the tie
    lean.Bias = 1.0f;
    CHECK(M::CollapseFlexion(c, kCalfL, lean) != M::CollapseFlexion(c, kCalfR, lean));
    // The motor reads the fall off the body's axes: a body whose shot goes along its left, facing +z, falls toward its left (x is the body's left).
    // The body here faces +z with +x its left, as the tests' standing pose does.
    std::vector<KillCase> shots;
    static const char* const names[8] = {"fwd", "fwd-left", "left", "back-left", "back", "back-right", "right", "fwd-right"};
    for (int i = 0; i < 8; ++i) {
        const float a = glm::radians(45.0f * (float)i);
        shots.push_back({names[i], 1, {std::sin(a), 0, std::cos(a)}, {0, 0, 0}});
    }
    struct Spread { float Early = 0, Rest = 0, Asym = 0, MeanFall = 0, WorstFall = 0; };
    auto spread = [&](const RagdollSettingsComponent& cfg) {
        Spread sp;
        std::vector<std::array<float, 4>> early, rest;
        for (const KillCase& kc : shots) {
            const KillStats k = KillOnAFloor(cfg, kc);
            early.push_back({k.PoseEarly[0], k.PoseEarly[1], k.PoseEarly[2], k.PoseEarly[3]});
            rest.push_back({k.KneeL, k.KneeR, k.HipL, k.HipR});
            sp.MeanFall += k.FallAngle / (float)shots.size();
            sp.WorstFall = std::max(sp.WorstFall, k.FallAngle);
            sp.Asym += k.KneeAsymEarly / (float)shots.size();
        }
        // Variety: the mean distance between the poses (knee and hip angles of both legs, degrees) of every two of the eight shots.
        auto variety = [](const std::vector<std::array<float, 4>>& sig) {
            float dist = 0.0f;
            int pairs = 0;
            for (size_t i = 0; i < sig.size(); ++i)
                for (size_t j = i + 1; j < sig.size(); ++j) {
                    float d2 = 0.0f;
                    for (size_t q = 0; q < 4; ++q) d2 += (sig[i][q] - sig[j][q]) * (sig[i][q] - sig[j][q]);
                    dist += std::sqrt(d2);
                    ++pairs;
                }
            return dist / (float)std::max(pairs, 1);
        };
        sp.Early = variety(early);
        sp.Rest = variety(rest);
        return sp;
    };
    const Spread old = spread(RevampCfg());
    if (old.Early <= 0.0f) return; // no physics
    const Spread now = spread(RagdollSettingsComponent());
    std::printf("[UnitTest] eight shot directions, revamp -> directional: pose variety half a second in (mean pairwise distance of knee/hip angles) %.1f -> %.1f deg, at rest %.1f -> %.1f deg | knee asymmetry at 0.5 s %.1f -> %.1f deg | fall angle mean %.1f -> %.1f, worst %.1f -> %.1f deg\n",
                old.Early, now.Early, old.Rest, now.Rest, old.Asym, now.Asym, old.MeanFall, now.MeanFall, old.WorstFall, now.WorstFall);
    CHECK(now.Early > 1.15f * old.Early);        // falls vary with the direction of the hit: half a second in the bodies are in different poses ...
    CHECK(now.Rest > 8.0f);                      // ... and (the lying pose pulls them together) they do not all end alike
    CHECK(now.Asym > 1.5f * old.Asym + 2.0f);   // the two knees are not alike (the lead leg is further folded)
    CHECK(now.MeanFall < 35.0f && now.WorstFall < 60.0f); // and the body still goes down along the shot
}

// Reactions: while the body is conscious its arms reach toward the fall (forward, back or out to the side) and, on a torso hit, the struck side's
// hand goes to the wound; the head tucks forward when it falls back. A head kill has none of it and goes limp sooner.
void TestRagdollReactionsLayerTheTarget() {
    using M = NpcRagdollMotor;
    const RagdollSettingsComponent c;
    const int kUpperArmL = 3, kForearmL = 4, kUpperArmR = 5, kNeck = NpcRagdoll::kNeck, kHead = 2;
    M::Fall fwd, back, head, wound;
    back.Fwd = -1.0f;
    head.Head = true;
    wound.WoundSide = 1; // the left hand
    // Arms: forward on a forward fall, back on a back fall, none on a head kill.
    CHECK(M::CollapseFlexion(c, kUpperArmL, fwd) > 0.9f * c.ShoulderCollapse && M::CollapseFlexion(c, kUpperArmL, back) < -0.1f * c.ShoulderCollapse);
    CHECK(M::CollapseFlexion(c, kUpperArmL, head) == 0.0f);
    // Out to the side: both arms swing toward a sideways fall, the one on that side further; Brace Weight 0 turns it all off.
    M::Fall left;
    left.Fwd = 0.0f; left.Lat = 1.0f;
    RagdollSettingsComponent noRelax = c;
    noRelax.RelaxTone = 0.0f; // (the target goes on to the lying pose after the collapse)
    const float armL = glm::degrees(AngleBetween(M::TargetAt(noRelax, kUpperArmL, c.CollapseBlendTime, left), glm::quat(1, 0, 0, 0)));
    const float armR = glm::degrees(AngleBetween(M::TargetAt(noRelax, kUpperArmR, c.CollapseBlendTime, left), glm::quat(1, 0, 0, 0)));
    CHECK(armL > c.BraceLateral * 0.9f && armR > 0.3f * c.BraceLateral && armR < 0.6f * armL);
    RagdollSettingsComponent noBrace = c;
    noBrace.BraceWeight = 0.0f;
    noBrace.RelaxTone = 0.0f;
    CHECK(AngleBetween(M::TargetAt(noBrace, kUpperArmL, c.CollapseBlendTime, left), glm::quat(1, 0, 0, 0)) < 1e-4f && M::CollapseFlexion(noBrace, kUpperArmL, fwd) == 0.0f);
    // The head tucks when the body falls back (not on a head kill); a Head Tuck Weight of 0 is the old none.
    CHECK(M::CollapseFlexion(c, kNeck, back) > 0.0f && M::CollapseFlexion(c, kHead, back) > 0.0f);
    M::Fall headBack = back;
    headBack.Head = true;
    CHECK(M::CollapseFlexion(c, kNeck, headBack) == 0.0f && M::CollapseFlexion(c, kHead, headBack) == 0.0f);
    // The wound grab: up quickly, held, let go within Wound Grab Time + 0.3 s; only the struck side's arm; none on a head kill or with weight 0.
    CHECK(M::WoundGrab(c, wound, 0.0f) == 0.0f && M::WoundGrab(c, wound, 0.3f * c.WoundGrabTime) > 0.9f * c.WoundGrabWeight);
    CHECK(M::WoundGrab(c, wound, c.WoundGrabTime + 0.4f) < 1e-4f && M::WoundGrab(c, fwd, 0.2f) == 0.0f && M::WoundGrab(c, head, 0.2f) == 0.0f);
    const float armHit = glm::degrees(AngleBetween(M::TargetAt(c, kUpperArmL, 0.25f, wound), M::TargetAt(c, kUpperArmL, 0.25f, fwd)));
    const float armOther = glm::degrees(AngleBetween(M::TargetAt(c, kUpperArmR, 0.25f, wound), M::TargetAt(c, kUpperArmR, 0.25f, fwd)));
    const float elbow = glm::degrees(AngleBetween(M::TargetAt(c, kForearmL, 0.25f, wound), M::TargetAt(c, kForearmL, 0.25f, fwd)));
    CHECK(armHit > 0.5f * c.WoundShoulder && armOther < 1e-3f && elbow > 0.5f * c.WoundElbow);
    // A head kill goes limp: the arms and neck let go in Head Kill Limp of the time; the legs are the same.
    CHECK(M::PartStrength(c, kUpperArmL, 0.6f, kHead) < 0.6f * M::PartStrength(c, kUpperArmL, 0.6f, 1) && M::PartStrength(c, kNeck, 0.6f, kHead) < M::PartStrength(c, kNeck, 0.6f, 1));
    CHECK(M::IsHeadHit(kHead) && !M::IsHeadHit(1) && M::PartStrength(c, 7, 0.4f, kHead) == M::PartStrength(c, 7, 0.4f, -1));
    // In the physics: a body shot in the chest from its right falls toward its left and both hands go that way (the lateral swing; the forward and back
    // reach are in the targets above); shot in the chest from the front, the struck-side hand is at the chest. (A bare ragdoll in the standing pose, the
    // same body both times, the layer on against off.)
    const KillCase fallsLeft{"chest from the right", 1, {1, 0, 0}, {0, 0, 0}}, shotInFront{"chest from the front", 1, {0, 0, -1}, {0, 0, 0}};
    RagdollSettingsComponent off = c;
    off.BraceWeight = 0.0f; off.WoundGrabWeight = 0.0f;
    const auto reachOn = KillPositions(c, fallsLeft, {0.45f}), reachOff = KillPositions(off, fallsLeft, {0.45f});
    if (reachOn.empty()) return; // no physics
    auto reach = [&](const std::vector<glm::vec3>& p) {
        return 0.5f * (glm::dot(p[(size_t)NpcRagdoll::kHandL] - p[0], glm::vec3(1, 0, 0)) + glm::dot(p[(size_t)NpcRagdoll::kHandR] - p[0], glm::vec3(1, 0, 0)));
    };
    std::printf("[UnitTest] hands toward the fall (left) from the pelvis at 0.45 s: brace off %.2f m, on %.2f m\n", reach(reachOff[0]), reach(reachOn[0]));
    CHECK(reach(reachOn[0]) > reach(reachOff[0]) + 0.03f);
    NpcRagdollMotor::Fall fall;
    const auto gripOn = KillPositions(c, shotInFront, {0.3f}, &fall), gripOff = KillPositions(off, shotInFront, {0.3f});
    CHECK(fall.WoundSide != 0 && fall.WoundFront && !fall.Head);
    const int hand = fall.WoundSide > 0 ? NpcRagdoll::kHandL : NpcRagdoll::kHandR;
    auto toWound = [&](const std::vector<glm::vec3>& p) { return glm::length(p[(size_t)hand] - p[1]); };
    std::printf("[UnitTest] struck-side hand to the chest at 0.3 s: wound grab off %.2f m, on %.2f m\n", toWound(gripOff[0]), toWound(gripOn[0]));
    CHECK(toWound(gripOn[0]) < toWound(gripOff[0]) - 0.05f);
}

// A round into a corpse: a point impulse on the part struck (capped), the body wakes and stays loose a moment so that part visibly reacts (more
// than with the loose time off, where the settle crushes it at once), then it settles and sleeps again without going far.
void TestRagdollCorpseHitReacts() {
    auto run = [&](float wakeTime, float* struckMove, float* farMove, float* pelvisDrift, float* sleptAt, float* speed) {
        World world;
        const entt::entity floorE = world.CreateEmptyEntity(glm::vec3(0.0f, -0.53f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Floor");
        ColliderComponent col;
        col.Kind = ColliderComponent::Shape::Box;
        col.HalfExtents = glm::vec3(40.0f, 0.5f, 40.0f);
        world.Registry.emplace<ColliderComponent>(floorE, col);
        world.SyncActiveInHierarchy();
        world.RebuildWorldTransformCache();
        PhysicsWorld::Create(world);
        if (!PhysicsWorld::IsActive()) return false;
        RagdollSettingsComponent cfg;
        cfg.CorpseWakeTime = wakeTime;
        PhysicsWorld::RagdollPart parts[NpcRagdoll::kRagParts];
        glm::mat4 pw[NpcRagdoll::kRagParts];
        CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts, pw));
        const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
        const int id = PhysicsWorld::CreateRagdoll(4353u, parts, NpcRagdoll::kRagParts, &prm);
        CHECK(id >= 0);
        NpcRagdollMotor motor;
        NpcRagdoll::Launch(id, parts, pw, glm::mat4(1.0f), glm::vec3(0.0f), glm::vec3(0, 0, -50), glm::vec3(0, 1.3f, 0), 1, cfg, &motor);
        for (int k = 0; k < 480 && !PhysicsWorld::RagdollAsleep(id); ++k) { motor.Update(1.0f / 60.0f); PhysicsWorld::Step(1.0f / 60.0f, world, {}); }
        CHECK(PhysicsWorld::RagdollAsleep(id));
        // The wrapper (NpcRagdoll) owns the ragdoll in the game; here the same two calls it makes: the point impulse and the motor's poke.
        glm::vec3 before[NpcRagdoll::kRagParts];
        for (int i = 0; i < NpcRagdoll::kRagParts; ++i) PartQuat(id, i, &before[i]);
        const int struck = 4; // lowerarm_l
        const float mass = NpcRagdoll::PartMass(&cfg, struck);
        *speed = std::min(cfg.CorpseShotBase + cfg.CorpseShotPerDamage * 34.0f, cfg.CorpseShotMaxSpeed);
        const glm::vec3 impulse = glm::normalize(glm::vec3(0.0f, 0.5f, 1.0f)) * (mass * *speed);
        const float j[3] = {impulse.x, impulse.y, impulse.z}, at[3] = {before[struck].x, before[struck].y, before[struck].z};
        PhysicsWorld::RagdollImpulse(id, struck, j, at);
        motor.Poke(cfg.CorpseWakeTime);
        CHECK(!PhysicsWorld::RagdollAsleep(id) && motor.Settle() == 0.0f);
        *sleptAt = -1.0f;
        *struckMove = *farMove = 0.0f;
        for (int k = 1; k <= 300; ++k) {
            motor.Update(1.0f / 60.0f);
            PhysicsWorld::Step(1.0f / 60.0f, world, {});
            if (k == 18) { // 0.3 s on
                glm::vec3 p;
                PartQuat(id, struck, &p);
                *struckMove = glm::length(p - before[struck]);
                PartQuat(id, NpcRagdoll::kHandR, &p);
                *farMove = glm::length(p - before[NpcRagdoll::kHandR]);
            }
            if (*sleptAt < 0.0f && PhysicsWorld::RagdollAsleep(id)) *sleptAt = (float)k / 60.0f;
        }
        glm::vec3 p;
        PartQuat(id, 0, &p);
        *pelvisDrift = glm::length(p - before[0]);
        CHECK(PhysicsWorld::RagdollAsleep(id));
        PhysicsWorld::DestroyRagdoll(id);
        PhysicsWorld::Destroy();
        return true;
    };
    float struckOff, farOff, driftOff, sleepOff, speedOff, struckOn, farOn, driftOn, sleepOn, speedOn;
    if (!run(0.0f, &struckOff, &farOff, &driftOff, &sleepOff, &speedOff)) return; // no physics
    CHECK(run(0.45f, &struckOn, &farOn, &driftOn, &sleepOn, &speedOn));
    std::printf("[UnitTest] corpse hit: struck calf moved %.1f -> %.1f cm in 0.3 s (loose time 0 -> 0.45 s), far hand %.1f -> %.1f cm, pelvis drift %.1f -> %.1f cm, asleep again after %.2f -> %.2f s\n",
                struckOff * 100.0f, struckOn * 100.0f, farOff * 100.0f, farOn * 100.0f, driftOff * 100.0f, driftOn * 100.0f, sleepOff, sleepOn);
    CHECK(struckOn > 1.25f * struckOff && struckOn > 0.04f);   // the struck limb reacts visibly
    CHECK(struckOn > 1.5f * farOn);                              // locally: the part the round hit moves most
    CHECK(sleepOn > 0.0f && sleepOn < 4.0f && driftOn < 0.5f); // and it settles and sleeps again, without going far
    // The shove is capped whatever the round, and scales with the damage under the cap.
    RagdollSettingsComponent c;
    CHECK(speedOn <= c.CorpseShotMaxSpeed + 1e-4f && c.CorpseShotBase + c.CorpseShotPerDamage * 34.0f < c.CorpseShotMaxSpeed);
}

// The new Ragdoll Settings fields save and load; a scene saved before them keeps the defaults.
void TestRagdollLyingFieldsRoundTrip() {
    World world;
    AssetLibrary assets;
    const entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Rules");
    auto& rag = world.Registry.emplace<RagdollSettingsComponent>(e);
    rag.RelaxTone = 0.31f; rag.RelaxTime = 0.77f; rag.RestFixTime = 0.9f; rag.LyingKnee = 17.0f; rag.BuckleAsymmetry = 0.2f; rag.BackKneeScale = 0.6f;
    rag.BraceWeight = 0.4f; rag.WoundGrabWeight = 0.3f; rag.HeadTuckWeight = 0.5f; rag.HeadKillLimp = 0.7f; rag.CorpseWakeTime = 0.9f; rag.CorpseShotMaxSpeed = 2.5f;
    const std::string json = SceneSerializer::SaveToString(world, assets);
    World back;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(back, assets2, json));
    int seen = 0;
    for (entt::entity b : back.Registry.view<RagdollSettingsComponent>()) {
        const auto& r = back.Registry.get<RagdollSettingsComponent>(b);
        ++seen;
        CHECK(r.RelaxTone == 0.31f && r.RelaxTime == 0.77f && r.RestFixTime == 0.9f && r.LyingKnee == 17.0f && r.BuckleAsymmetry == 0.2f && r.BackKneeScale == 0.6f);
        CHECK(r.BraceWeight == 0.4f && r.WoundGrabWeight == 0.3f && r.HeadTuckWeight == 0.5f && r.HeadKillLimp == 0.7f && r.CorpseWakeTime == 0.9f && r.CorpseShotMaxSpeed == 2.5f);
        CHECK(r.LyingHip == RagdollSettingsComponent().LyingHip && r.RestPelvisMax == RagdollSettingsComponent().RestPelvisMax);
    }
    CHECK(seen == 1);
    World old;
    AssetLibrary assets3;
    const std::string oldJson = std::string(R"({"formatVersion":3,"empties":[{"name":"S","id":1,"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1],)") +
                                R"("Ragdoll Settings":{"Tone Residual":0.1}}]})";
    CHECK(SceneSerializer::LoadFromString(old, assets3, oldJson));
    int found = 0;
    for (entt::entity b : old.Registry.view<RagdollSettingsComponent>()) {
        ++found;
        const auto& r = old.Registry.get<RagdollSettingsComponent>(b);
        CHECK(r.ToneResidual == 0.1f && r.RelaxTone == RagdollSettingsComponent().RelaxTone && r.BraceWeight == RagdollSettingsComponent().BraceWeight);
    }
    CHECK(found == 1);
}

// ---- gear on death: the dropped weapon (Npc/NpcDroppedWeapon) ----

// A world with a floor whose top is at `top`, physics up.
entt::entity GearFloor(World& world, float top) {
    const entt::entity floorE = world.CreateEmptyEntity(glm::vec3(0.0f, top - 0.5f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Floor");
    ColliderComponent col;
    col.Kind = ColliderComponent::Shape::Box;
    col.HalfExtents = glm::vec3(40.0f, 0.5f, 40.0f);
    world.Registry.emplace<ColliderComponent>(floorE, col);
    world.SyncActiveInHierarchy();
    world.RebuildWorldTransformCache();
    PhysicsWorld::Create(world);
    return floorE;
}

// The gun leaves the hands at the hands' velocity (plus a capped share of the round's impulse), with or without the collision delay.
void TestDroppedWeaponTakesTheHandsVelocity() {
    World world;
    GearFloor(world, -100.0f);
    if (!PhysicsWorld::IsActive()) return;
    const entt::entity gun = world.CreateEmptyEntity(glm::vec3(0.0f, 1.2f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Gun");
    DroppedWeaponSettingsComponent cfg;
    cfg.Spin = 0.0f;
    cfg.CollisionDelay = 0.0f;
    cfg.ImpulseShare = 0.0f;
    auto drop = NpcDroppedWeapon::DropAt(world, gun, glm::vec3(3.0f, 0.0f, -2.0f), glm::vec3(0.0f), cfg);
    CHECK(drop && drop->Built());
    for (int k = 0; k < 4; ++k) { drop->Update(world, 1.0f / 60.0f); PhysicsWorld::Step(1.0f / 60.0f, world, {}); }
    BodyState s;
    CHECK(PhysicsWorld::GetBodyState((unsigned)entt::to_integral(drop->Entity()), s) && s.Valid);
    CHECK(std::abs(s.Velocity[0] - 3.0f) < 0.3f && std::abs(s.Velocity[2] + 2.0f) < 0.3f); // the hands' velocity, not zero
    drop->Stop(world);
    // The round's share: along its line, capped by Max Shot Speed.
    cfg.ImpulseShare = 0.2f;
    cfg.MaxShotSpeed = 1.5f;
    auto shot = NpcDroppedWeapon::DropAt(world, gun, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 100.0f), cfg);
    CHECK(shot && std::abs(shot->StartVelocity().z - 1.5f) < 1e-4f && std::abs(shot->StartVelocity().x) < 1e-4f);
    shot->Stop(world);
    cfg.MaxShotSpeed = 50.0f;
    auto soft = NpcDroppedWeapon::DropAt(world, gun, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 35.0f), cfg);
    CHECK(soft && std::abs(soft->StartVelocity().z - 0.2f * 35.0f / cfg.Mass) < 1e-3f);
    soft->Stop(world);
    // With the collision delay it flies on its own first (hands' velocity and gravity), then the body takes it at that speed.
    cfg.ImpulseShare = 0.0f;
    cfg.CollisionDelay = 0.1f;
    auto late = NpcDroppedWeapon::DropAt(world, gun, glm::vec3(6.0f, 0.0f, 0.0f), glm::vec3(0.0f), cfg);
    CHECK(late && !late->Built());
    const float y0 = world.WorldSpaceTransform(late->Entity()).Position.y;
    for (int k = 0; k < 5; ++k) late->Update(world, 1.0f / 60.0f);
    CHECK(!late->Built() && world.WorldSpaceTransform(late->Entity()).Position.x > 0.4f && world.WorldSpaceTransform(late->Entity()).Position.y < y0);
    for (int k = 0; k < 3; ++k) { late->Update(world, 1.0f / 60.0f); PhysicsWorld::Step(1.0f / 60.0f, world, {}); }
    CHECK(late->Built() && PhysicsWorld::GetBodyState((unsigned)entt::to_integral(late->Entity()), s) && s.Valid && s.Velocity[0] > 5.0f);
    late->Stop(world);
    PhysicsWorld::Destroy();
}

// Thrown over a floor, the gun comes down, tumbles on and comes to rest (asleep) lying on it, not through it.
void TestDroppedWeaponSettlesAndSleeps() {
    World world;
    GearFloor(world, 0.0f);
    if (!PhysicsWorld::IsActive()) return;
    const entt::entity gun = world.CreateEmptyEntity(glm::vec3(0.0f, 1.3f, 0.0f), glm::vec3(0.0f, 30.0f, 0.0f), glm::vec3(1.0f), "Gun");
    const DroppedWeaponSettingsComponent cfg; // the defaults: delay, spin, damping
    auto drop = NpcDroppedWeapon::DropAt(world, gun, glm::vec3(2.0f, 0.5f, 1.0f), glm::vec3(0.0f, 0.0f, 20.0f), cfg);
    CHECK(drop && !drop->Asleep());
    float sleptAt = -1.0f;
    float lowest = 1e9f;
    for (int k = 0; k < 60 * 8; ++k) {
        drop->Update(world, 1.0f / 60.0f);
        PhysicsWorld::Step(1.0f / 60.0f, world, {});
        lowest = std::min(lowest, world.WorldSpaceTransform(drop->Entity()).Position.y);
        if (sleptAt < 0.0f && drop->Asleep()) sleptAt = (float)k / 60.0f;
    }
    const glm::vec3 at = world.WorldSpaceTransform(drop->Entity()).Position;
    std::printf("[UnitTest] dropped gun asleep after %.2f s at (%.2f %.2f %.2f), lowest %.3f\n", sleptAt, at.x, at.y, at.z, lowest);
    CHECK(sleptAt > 0.2f && sleptAt < 6.0f && drop->Asleep()); // it fell and rested
    CHECK(at.y > -0.02f && at.y < 0.3f && lowest > -0.05f);    // on the floor, never through it
    CHECK(at.x > 0.5f);                                        // it travelled with the hands
    drop->Stop(world);
    PhysicsWorld::Destroy();
}

// The drop goes with the corpse: Stop (or Lifetime) removes the entity and its body; off in the settings, nothing drops.
void TestDroppedWeaponGoesWithTheCorpse() {
    World world;
    GearFloor(world, 0.0f);
    if (!PhysicsWorld::IsActive()) return;
    const entt::entity gun = world.CreateEmptyEntity(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Gun");
    DroppedWeaponSettingsComponent cfg;
    cfg.CollisionDelay = 0.0f;
    auto drop = NpcDroppedWeapon::DropAt(world, gun, glm::vec3(0.0f), glm::vec3(0.0f), cfg);
    CHECK(drop);
    const entt::entity e = drop->Entity();
    PhysicsWorld::Step(1.0f / 60.0f, world, {});
    BodyState s;
    CHECK(world.Registry.valid(e) && PhysicsWorld::GetBodyState((unsigned)entt::to_integral(e), s) && s.Valid);
    drop->Stop(world);
    PhysicsWorld::Step(1.0f / 60.0f, world, {});
    CHECK(!world.Registry.valid(e) && !(PhysicsWorld::GetBodyState((unsigned)entt::to_integral(e), s) && s.Valid));
    CHECK(world.Registry.valid(gun)); // the weapon presentation's own entity is its own to stop
    // Lifetime: it goes by itself.
    cfg.Lifetime = 0.5f;
    auto brief = NpcDroppedWeapon::DropAt(world, gun, glm::vec3(0.0f), glm::vec3(0.0f), cfg);
    const entt::entity e2 = brief->Entity();
    for (int k = 0; k < 20; ++k) { brief->Update(world, 1.0f / 60.0f); PhysicsWorld::Step(1.0f / 60.0f, world, {}); }
    CHECK(world.Registry.valid(e2));
    for (int k = 0; k < 20; ++k) { brief->Update(world, 1.0f / 60.0f); PhysicsWorld::Step(1.0f / 60.0f, world, {}); }
    CHECK(!world.Registry.valid(e2));
    // Off: no drop. A hidden (holstered) gun: no drop.
    cfg.Enabled = false;
    CHECK(!NpcDroppedWeapon::DropAt(world, gun, glm::vec3(0.0f), glm::vec3(0.0f), cfg));
    cfg.Enabled = true;
    world.Registry.emplace<DeactivatedTag>(gun);
    CHECK(!NpcDroppedWeapon::DropAt(world, gun, glm::vec3(0.0f), glm::vec3(0.0f), cfg));
    PhysicsWorld::Destroy();
}

// Dropping the gun releases the body's weapon hold (the arms are the ragdoll's); the body tracks the gun's velocity for the drop.
void TestDroppedWeaponReleasesTheHold() {
    World world;
    const entt::entity gun = world.CreateEmptyEntity(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Gun");
    NpcBody body;
    CHECK(!body.WeaponReleased());
    auto drop = NpcDroppedWeapon::Drop(world, body, gun, glm::vec3(0.0f));
    CHECK(body.WeaponReleased() && drop);
    if (drop) drop->Stop(world);
    // The same with no gun to drop (the hold still lets go).
    NpcBody bare;
    CHECK(!NpcDroppedWeapon::Drop(world, bare, entt::null, glm::vec3(0.0f)) && bare.WeaponReleased());
    // The gun's velocity, from the positions it was seen at.
    NpcBody mover;
    CHECK(glm::length(mover.GunVelocity()) == 0.0f);
    for (int k = 0; k < 30; ++k) mover.TrackGun(glm::vec3(5.0f * (float)k / 60.0f, 1.0f, 0.0f), 1.0f / 60.0f);
    CHECK(std::abs(mover.GunVelocity().x - 5.0f) < 0.3f && std::abs(mover.GunVelocity().y) < 1e-3f);
    // A bound: it is also what the drop gets.
    NpcBody fast;
    for (int k = 0; k < 30; ++k) fast.TrackGun(glm::vec3(5.0f * (float)k / 60.0f, 1.0f, 0.0f), 1.0f / 60.0f);
    auto moving = NpcDroppedWeapon::Drop(world, fast, gun, glm::vec3(0.0f));
    CHECK(moving && std::abs(moving->StartVelocity().x - fast.GunVelocity().x) < 1e-4f);
    if (moving) moving->Stop(world);
}

// The settings: defaults without the component, the scene's when there is one, and they survive a save.
void TestDroppedWeaponSettings() {
    World none;
    const DroppedWeaponSettingsComponent d;
    const DroppedWeaponSettingsComponent got = NpcDroppedWeapon::SettingsIn(none);
    CHECK(got.Enabled && got.Mass == d.Mass && got.ImpulseShare == d.ImpulseShare && got.Lifetime == 0.0f);
    CHECK(d.Mass > 2.0f && d.Mass < 6.0f && d.Bounciness < 0.3f);
    World world;
    AssetLibrary assets;
    const entt::entity e = world.CreateEmptyEntity(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f), "Rules");
    auto& c = world.Registry.emplace<DroppedWeaponSettingsComponent>(e);
    c.Enabled = false; c.Mass = 2.5f; c.ImpulseShare = 0.4f; c.Lifetime = 30.0f; c.CollisionDelay = 0.2f;
    CHECK(!NpcDroppedWeapon::SettingsIn(world).Enabled && NpcDroppedWeapon::SettingsIn(world).Mass == 2.5f);
    const std::string json = SceneSerializer::SaveToString(world, assets);
    World back;
    AssetLibrary assets2;
    CHECK(SceneSerializer::LoadFromString(back, assets2, json));
    int seen = 0;
    for (entt::entity b : back.Registry.view<DroppedWeaponSettingsComponent>()) {
        const auto& r = back.Registry.get<DroppedWeaponSettingsComponent>(b);
        ++seen;
        CHECK(!r.Enabled && r.Mass == 2.5f && r.ImpulseShare == 0.4f && r.Lifetime == 30.0f && r.CollisionDelay == 0.2f && r.Friction == d.Friction);
    }
    CHECK(seen == 1);
}

} // namespace

void RegisterRagdollTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"RagdollHingesDoNotHyperextend", TestRagdollHingesDoNotHyperextend});
    tests.push_back({"RagdollSettingsReachTheParts", TestRagdollSettingsReachTheParts});
    tests.push_back({"RagdollAndSquadSettingsRoundTrip", TestRagdollAndSquadSettingsRoundTrip});
    tests.push_back({"RagdollPartsInheritTheirBonesVelocity", TestRagdollPartsInheritTheirBonesVelocity});
    tests.push_back({"RagdollShapedInertia", TestRagdollShapedInertia});
    tests.push_back({"RagdollPerRegionDriveFade", TestRagdollPerRegionDriveFade});
    tests.push_back({"NpcDirectorReadsSettings", TestNpcDirectorReadsSettings});
    tests.push_back({"RagdollTableIsSplitFromHitboxes", TestRagdollTableIsSplitFromHitboxes});
    tests.push_back({"RagdollNeckHandsFeetSettings", TestRagdollNeckHandsFeetSettings});
    tests.push_back({"RagdollHandsAndFeetRestOnTheFloor", TestRagdollHandsAndFeetRestOnTheFloor});
    tests.push_back({"NpcFlinchKicksAndSettles", TestNpcFlinchKicksAndSettles});
    tests.push_back({"RagdollNewFieldsRoundTrip", TestRagdollNewFieldsRoundTrip});
    tests.push_back({"NavBoundaryEdgesMergeQuickly", TestNavBoundaryEdgesMergeQuickly});
    tests.push_back({"PoweredRagdollDeathsAgainstPassive", TestPoweredRagdollDeathsAgainstPassive});
    tests.push_back({"RagdollMuscleToneCurves", TestRagdollMuscleToneCurves});
    tests.push_back({"RagdollMusclesFoldTheKnees", TestRagdollMusclesFoldTheKnees});
    tests.push_back({"RagdollSettlesAndSleeps", TestRagdollSettlesAndSleeps});
    tests.push_back({"RagdollPoweredSettings", TestRagdollPoweredSettings});
    tests.push_back({"RagdollEndsLyingNotKneeling", TestRagdollEndsLyingNotKneeling});
    tests.push_back({"RagdollKneesBuckleWithTheShot", TestRagdollKneesBuckleWithTheShot});
    tests.push_back({"RagdollReactionsLayerTheTarget", TestRagdollReactionsLayerTheTarget});
    tests.push_back({"RagdollCorpseHitReacts", TestRagdollCorpseHitReacts});
    tests.push_back({"RagdollLyingFieldsRoundTrip", TestRagdollLyingFieldsRoundTrip});
    tests.push_back({"DroppedWeaponTakesTheHandsVelocity", TestDroppedWeaponTakesTheHandsVelocity});
    tests.push_back({"DroppedWeaponSettlesAndSleeps", TestDroppedWeaponSettlesAndSleeps});
    tests.push_back({"DroppedWeaponGoesWithTheCorpse", TestDroppedWeaponGoesWithTheCorpse});
    tests.push_back({"DroppedWeaponReleasesTheHold", TestDroppedWeaponReleasesTheHold});
    tests.push_back({"DroppedWeaponSettings", TestDroppedWeaponSettings});
}
