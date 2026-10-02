#include "UnitTestSupport.h"

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

#include <cmath>
#include <cstring>
#include <map>
#include <string>

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
    // (The elbow overshoots more than the knee since the hand became its own part: it whips through the wrist limit and pulls the forearm
    // with it, ~20 deg in this violent 3 m/s shove; with the wrist locked it is ~3. Still a fraction of the legacy fold.)
    CHECK(kneeNew < 6.0f && elbowNew < 25.0f);
    CHECK(kneeOld > 15.0f && elbowOld > 15.0f);
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
}
