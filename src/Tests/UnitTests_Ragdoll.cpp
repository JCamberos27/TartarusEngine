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
    };
    const auto it = kBones.find(name);
    if (it == kBones.end()) return false;
    out = it->second;
    return true;
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
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kParts];
    const bool built = NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts);
    CHECK(built);
    const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
    const int id = PhysicsWorld::CreateRagdoll(4343u, parts, NpcRagdoll::kParts, &prm);
    CHECK(id >= 0);
    float worst = 0.0f;
    if (id >= 0) {
        const int parent = NpcPartDefOf(part).Parent;
        glm::vec3 end;
        StandingBone(NpcPartDefOf(part).End, end);
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
    PhysicsWorld::RagdollPart parts[NpcRagdoll::kParts];
    CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts));
    CHECK(std::fabs(parts[8].Mass - 9.0f) < 1e-4f && std::fabs(parts[10].Mass - 9.0f) < 1e-4f);
    CHECK(parts[7].Anatomical && std::fabs(parts[7].SwingZMax - 60.0f) < 1e-4f);
    CHECK(parts[8].Anatomical && parts[8].SwingZMin == 0.0f); // a knee: no extension past straight
    CHECK(std::fabs(NpcRagdoll::BodyParams(cfg).LinearDamping - 0.5f) < 1e-6f && NpcRagdoll::BodyParams(cfg).SolverPosIters == 24);
    // The defaults are what was hard-coded: a body of ~75 kg.
    float total = 0.0f;
    for (int i = 0; i < NpcRagdoll::kParts; ++i) total += NpcRagdoll::PartMass(nullptr, i);
    CHECK(std::fabs(total - 75.0f) < 8.0f);
    const RagdollSettingsComponent def;
    for (int i = 0; i < NpcRagdoll::kParts; ++i) CHECK(std::fabs(NpcRagdoll::PartMass(&def, i) - NpcPartDefOf(i).Mass) < 1e-5f);
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
            if (std::string(name) == "hand_l") {
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
    PhysicsWorld::RagdollPart before[NpcRagdoll::kParts], now[NpcRagdoll::kParts], jump[NpcRagdoll::kParts], idle[NpcRagdoll::kParts];
    glm::mat4 wb[NpcRagdoll::kParts], wn[NpcRagdoll::kParts], wj[NpcRagdoll::kParts];
    CHECK(NpcRagdoll::BuildParts(swung(0.0f, glm::vec3(0.0f)), glm::mat4(1.0f), glm::vec3(0.0f), cfg, before, wb));
    CHECK(NpcRagdoll::BuildParts(swung(angle, glm::vec3(0.0f)), glm::mat4(1.0f), glm::vec3(0.0f), cfg, now, wn));
    NpcRagdoll::InheritVelocity(wb, wn, dt, cfg, now);
    const glm::vec3 w(now[4].AngularVelocity[0], now[4].AngularVelocity[1], now[4].AngularVelocity[2]);
    std::printf("[UnitTest] forearm spin %.2f rad/s (bone turned %.2f rad in %.0f ms)\n", glm::length(w), angle, dt * 1000.0f);
    CHECK(std::fabs(std::fabs(w.z) - angle / dt) < 0.05f * angle / dt && std::fabs(w.x) < 0.05f && std::fabs(w.y) < 0.05f);
    CHECK(speedOf(now[4].Velocity) > 0.3f); // its centre moves too
    for (int i : {0, 1, 2, 3}) CHECK(speedOf(now[i].AngularVelocity) < 1e-3f);
    // A teleport (a bone 5 m off in one frame) is clamped, a bad dt adds nothing, and the scale switches it off.
    CHECK(NpcRagdoll::BuildParts(swung(0.0f, glm::vec3(5.0f, 0.0f, 0.0f)), glm::mat4(1.0f), glm::vec3(0.0f), cfg, jump, wj));
    NpcRagdoll::InheritVelocity(wb, wj, dt, cfg, jump);
    float fastest = 0.0f;
    for (int i = 0; i < NpcRagdoll::kParts; ++i) fastest = std::max(fastest, speedOf(jump[i].Velocity));
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
    const int id = PhysicsWorld::CreateRagdoll(4344u, now, NpcRagdoll::kParts, &prm);
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
        PhysicsWorld::RagdollPart parts[NpcRagdoll::kParts];
        CHECK(NpcRagdoll::BuildParts(StandingBone, glm::mat4(1.0f), glm::vec3(0.0f), cfg, parts));
        const PhysicsWorld::RagdollParams prm = NpcRagdoll::BodyParams(cfg);
        const int id = PhysicsWorld::CreateRagdoll(4345u, parts, NpcRagdoll::kParts, &prm);
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
    for (int i = 0; i < NpcRagdoll::kParts; ++i) CHECK(std::fabs(NpcRagdoll::PartFade(&def, i) - NpcRagdoll::kDriveFade) < 1e-6f);
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

} // namespace

void RegisterRagdollTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"RagdollHingesDoNotHyperextend", TestRagdollHingesDoNotHyperextend});
    tests.push_back({"RagdollSettingsReachTheParts", TestRagdollSettingsReachTheParts});
    tests.push_back({"RagdollAndSquadSettingsRoundTrip", TestRagdollAndSquadSettingsRoundTrip});
    tests.push_back({"RagdollPartsInheritTheirBonesVelocity", TestRagdollPartsInheritTheirBonesVelocity});
    tests.push_back({"RagdollShapedInertia", TestRagdollShapedInertia});
    tests.push_back({"RagdollPerRegionDriveFade", TestRagdollPerRegionDriveFade});
    tests.push_back({"NpcDirectorReadsSettings", TestNpcDirectorReadsSettings});
}
