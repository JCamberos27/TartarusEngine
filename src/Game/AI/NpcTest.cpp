#include "NpcTest.h"
#include <algorithm>
#include <cctype>
#include "IK.h"
#include "World.h"

#include "Combat/CombatFx.h"
#include "Combat/PlayerVitals.h"
#include "FirstPersonBody.h"
#include "FirstPersonPresentation.h"
#include "GameModuleAPI.h"
#include "NpcDirector.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "Components.h"
#include "Npc/NpcRagdoll.h"
#include "Npc/NpcHitboxes.h"
#include "Core/Env.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>


NpcTest::NpcTest(const std::string& scenario) : m_Scenario(scenario.empty() ? "labloco" : scenario) {
    if (m_Scenario == "deaths") m_Duration = 70.0f;
    if (m_Scenario == "feet") m_Duration = 10.0f;
    if (m_Scenario == "stopsteps") m_Duration = StopStepsSeconds();
    if (m_Scenario == "npcloco") m_Duration = NpcLocoSeconds();
    if (m_Scenario == "npcperf") m_Duration = NpcPerfSeconds();
    if (m_Scenario == "labloco") m_Duration = LabLocoSeconds();
    if (m_Scenario == "mmplayer" || m_Scenario == "mmstress") {
        // The testbed's player parts.
        Env::Set(Env::Var::TestbedParts, m_Scenario == "mmstress" ? "S" : "ACM");
    }
    if (m_Scenario == "testbed" || m_Scenario == "mmplayer" || m_Scenario == "mmstress") {
        m_Duration = TestbedSeconds();
        m_SkipLegs = TestbedLegsOff();
        if (!m_SkipLegs.empty()) std::printf("[NpcTest] leg filter '%s': %zu legs left out\n", Env::Str(Env::Var::TestbedLegs).c_str(), m_SkipLegs.size());
    }
    if (m_Scenario == "mmview") m_Duration = MmViewSeconds();
    if (m_Scenario == "mmdb") m_Duration = MmDbSeconds();
    if (m_Scenario == "flame") m_Duration = 14.0f;
    if (m_Scenario == "revolver") m_Duration = 116.0f;
    if (m_Scenario == "blood") m_Duration = 44.0f;
    if (m_Scenario == "decals") m_Duration = 52.0f;
    if (m_Scenario == "doors") m_Duration = 300.0f; // (done once the route is walked)
    if (m_Scenario == "roam") m_Duration = 420.0f;  // (done once the route is walked)
    if (m_Scenario == "ragdoll") m_Duration = 240.0f;
    if (Env::IsSet(Env::Var::NpcTestSeconds)) m_Duration = std::max(5.0f, Env::Float(Env::Var::NpcTestSeconds, 0.0f));
    if (Env::IsSet(Env::Var::NpcTestRecord)) m_RecordDir = Env::Str(Env::Var::NpcTestRecord);
    m_TraceDir = Env::IsSet(Env::Var::NpcTestTrace) ? Env::Str(Env::Var::NpcTestTrace) : m_RecordDir;
    std::cout << "[NpcTest] scenario '" << m_Scenario << "', " << m_Duration << " s" << std::endl;
}

void NpcTest::PrintCosts(const NpcDirector& npcs) const {
    const auto& t = npcs.ThinkCost();
    const auto& l = npcs.LateCost();
    std::printf("[NpcTest] cost avg/max ms over %d frames: think %.3f/%.3f  late %.3f/%.3f  (AI+late avg %.3f)\n", t.Frames, t.Avg(),
                t.Max, l.Avg(), l.Max, t.Avg() + l.Avg());
    for (int i = 0; i < NpcDirector::SubCount; ++i)
        std::printf("[NpcTest]   %-12s avg %.3f  max %.3f\n", NpcDirector::SubName(i), npcs.SubCost(i).Avg(), npcs.SubCost(i).Max);
}

void NpcTest::SlideLeg(const std::string& leg) {
    static const char* const kWho[] = {"player"};
    std::vector<std::string> who(std::begin(kWho), std::end(kWho));
    for (const std::string& n : m_SlideBodies) who.push_back(n);
    for (const std::string& n : who) {
        const IK::FootSlideMeterTotals t = IK::FootSlideMeterFor(n).TakeTotals();
        if (t.Plants == 0) continue;
        if (n == "player") {
            m_SlideWorstPlayer[leg] = t.MaxDrift;
            m_SlideContactPlayer[leg] = t.MaxContactDrift;
        } else {
            m_SlideWorstNpc[leg] = std::max(m_SlideWorstNpc[leg], t.MaxDrift);
        }
        std::printf("[Slide] %-26s %-10s %2d plants, drift mean %4.1f cm, worst %5.1f cm; contact (heel or ball) mean %4.1f, worst %5.1f cm "
                    "(from first contact, the swing's end in: %.1f / %.1f)\n", leg.c_str(), n.c_str(), t.Plants, t.MeanCm(), t.MaxDrift * 100.0f,
                    t.MeanContactCm(), t.MaxContactDrift * 100.0f, t.FromContact.MaxDrift * 100.0f, t.FromContact.MaxContactDrift * 100.0f);
    }
}

void NpcTest::Check(bool ok, const std::string& what) {
    // A leg filter: a check naming a leg that didn't run (itself, or a leg it is measured against) is skipped. Names are matched
    // longest first, so "M jog" doesn't match inside "M jog turning".
    if (!m_SkipLegs.empty()) {
        static std::vector<std::string> sorted;
        if (sorted.empty()) {
            sorted = m_SkipLegs;
            std::sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        }
        for (const std::string& off : sorted)
            for (size_t at = what.find(off); at != std::string::npos; at = what.find(off, at + 1)) {
                const size_t end = at + off.size();
                const bool whole = (at == 0 || !std::isalnum((unsigned char)what[at - 1])) &&
                                   (end >= what.size() || !(std::isalnum((unsigned char)what[end]) ||
                                                            (what[end] == ' ' && end + 1 < what.size() && std::isalpha((unsigned char)what[end + 1]) &&
                                                             what.compare(end, 2, " (") != 0)));
                if (!whole) continue;
                std::cout << "[NpcTest] SKIP (leg filter: '" << off << "' didn't run) " << what << std::endl;
                return;
            }
    }
    ++m_Checks;
    if (!ok) ++m_Failures;
    std::cout << "[NpcTest] " << (ok ? "PASS " : "FAIL ") << what << std::endl;
}

void NpcTest::Drive(Player& player, FirstPersonPresentation& weapon, NpcDirector& npcs, PlayerVitals& vitals, float dt) {
    (void)dt;
    (void)npcs;
    player.ScriptedMove = true;
    player.ScriptMove = glm::vec2(0.0f);
    player.ScriptSprint = false;
    player.ScriptJump = false;
    vitals.GodMode = true;
    m_Firing = false;
    if (vitals.IsDead()) return;
    // Default: look north over the arena.
    float wantYaw = -90.0f, wantPitch = -3.0f;
    if (m_Scenario == "ragdoll") { wantYaw = m_RViewYaw; wantPitch = -10.0f; } // over the bodies (then away from them)
    if (m_Scenario == "decals") DecalsDrive(player, wantYaw, wantPitch);
    if (m_Scenario == "doors") DoorsDrive(player, dt, wantYaw, wantPitch);
    if (m_Scenario == "stopsteps") StopStepsDrive(player, dt, wantYaw, wantPitch);
    if (m_Scenario == "npcloco") NpcLocoDrive(player, wantYaw, wantPitch);
    if (m_Scenario == "npcperf") NpcPerfDrive(player, wantYaw, wantPitch);
    if (m_Scenario == "labloco") LabLocoDrive(player, weapon, dt, wantYaw, wantPitch);
    if (m_Scenario == "testbed" || m_Scenario == "mmplayer" || m_Scenario == "mmstress") TestbedDrive(player, weapon, dt, wantYaw, wantPitch);
    if (m_Scenario == "roam") RoamDrive(player, dt, wantYaw, wantPitch);
    if (m_Scenario == "mmview") MmViewDrive(player, weapon, dt, wantYaw, wantPitch);
    if (m_Scenario == "mmdb") MmDbDrive(player, weapon, dt, wantYaw, wantPitch);
    if (m_Scenario == "flame") {
        // The muzzle flame: the AK's single shots from the hip (1-3.5 s), then bursts on the sights
        // (4-7 s); then the second slot (the Remington): shots from the hip (8.5-10.5 s), then on the
        // sights (11-13 s).
        m_FireHold += dt;
        const float t = m_FireHold;
        if (t >= 7.5f && !m_FlameSwitched && weapon.SlotCount() > 1) {
            weapon.SelectSlot(1);
            m_FlameSwitched = true;
        }
        const float u = t >= 7.5f ? t - 7.5f : t; // each gun's own clock
        if (u >= 1.0f && u < 3.5f) m_Firing = std::fmod(u, 0.5f) < 0.05f;
        if (u >= 4.0f && u < (t >= 7.5f ? 5.5f : 7.0f)) {
            m_Target = "flame"; // sights up
            m_Firing = u >= 4.5f && std::fmod(u, 1.0f) < 0.3f;
        } else {
            m_Target.clear();
        }
        m_Reload = weapon.Ammo() == 0;
    }
    if (m_Scenario == "revolver") {
        RevolverDrive(player, weapon, dt);
        wantYaw += m_RevLook.x;
        wantPitch += m_RevLook.y;
    }
    if (m_LastAmmo >= 0 && weapon.Ammo() < m_LastAmmo) m_PlayerShots += m_LastAmmo - weapon.Ammo();
    m_LastAmmo = weapon.Ammo();
    // Turn the view toward it like a player would (a fast but finite turn).
    const float dy = std::remainder(wantYaw - player.Cam.Yaw, 360.0f), dp = wantPitch - player.Cam.Pitch;
    const float step = 240.0f * dt;
    player.Cam.Yaw += std::clamp(dy, -step, step);
    player.Cam.Pitch += std::clamp(dp, -step, step);
}

bool NpcTest::SceneCamera(glm::vec3& pos, float& yaw, float& pitch) const {
    if (!m_HaveCam) return false;
    pos = m_CamPos;
    yaw = m_CamYaw;
    pitch = m_CamPitch;
    return true;
}

void NpcTest::After(World& world, NpcDirector& npcs, const PlayerVitals& vitals, const Player& player) {
    (void)world;
    (void)vitals;
    const float now = npcs.Now();
    m_Time = now;
    m_Shot.clear();
    if (m_Scenario == "deaths") {
        Deaths(world, npcs, now);
        return;
    }
    if (m_Scenario == "blood") {
        Blood(world, npcs, now);
        return;
    }
    if (m_Scenario == "ragdoll") {
        Ragdoll(world, npcs, player);
        return;
    }
    if (m_Scenario == "decals") {
        Decals(world, npcs, now);
        return;
    }
    if (m_Scenario == "doors") {
        Doors(world, player);
        return;
    }
    if (m_Scenario == "stopsteps") {
        StopSteps(player, now);
        return;
    }
    if (m_Scenario == "npcloco") {
        NpcLoco(world, npcs, now);
        return;
    }
    if (m_Scenario == "npcperf") {
        NpcPerf(world, npcs, now);
        return;
    }
    if (m_Scenario == "labloco") {
        LabLoco(world, npcs, player, now);
        return;
    }
    if (m_Scenario == "testbed" || m_Scenario == "mmplayer" || m_Scenario == "mmstress") {
        Testbed(world, npcs, player, now);
        return;
    }
    if (m_Scenario == "roam") {
        Roam(world, player);
        return;
    }
    if (m_Scenario == "mmview") {
        MmView(world);
        return;
    }
    if (m_Scenario == "mmdb") {
        MmDb(world);
        return;
    }
    if (m_Scenario == "feet") {
        Feet(npcs, now);
        return;
    }
    if (m_Scenario == "revolver") {
        RevolverAfter(player);
        return;
    }
    if (m_Scenario == "flame") {
        // The Scene view off the player's right shoulder, on the muzzle.
        const glm::vec3 eye = player.Cam.Position, front = player.Cam.Front();
        const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
        m_CamPos = eye + right * 0.9f + front * 0.5f + glm::vec3(0.0f, 0.1f, 0.0f);
        const glm::vec3 d = glm::normalize(eye + front * 0.7f - glm::vec3(0.0f, 0.15f, 0.0f) - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        m_HaveCam = true;
        if (now >= m_Duration && !m_Done) {
            m_Done = true;
            Check(m_PlayerShots > 0, "the player fired (" + std::to_string(m_PlayerShots) + " rounds)");
            Check(m_FlameSwitched, "the player switched to the second weapon");
        }
        return;
    }
}

// --- deaths: hitboxes, hit reactions, deaths -------------------------------------------------------------------------
namespace {
unsigned RootId(const Npc& n) { return (unsigned)entt::to_integral(n.Root); }

struct DeathCase {
    const char* Name;
    int Part;          // the hitbox part shot (NpcRagdoll's order)
    float Damage;      // the weapon's damage per round
    bool StaggerAfterFirst, DiesFirst;
};
const DeathCase kDeathCases[] = {
    {"head", 2, 34.0f, false, true},       // 34 x the head multiplier: dead at once
    {"chest", 1, 34.0f, false, false},     // a few rounds
    {"thigh", 7, 40.0f, false, false},     // a leg: a few rounds, then it dies
    {"forearm", 4, 70.0f, true, false},    // a heavy hit on an arm: it staggers, a second kills
};
constexpr int kLastStep = 4; // deaths: the stage after the corpse is shot
constexpr int kDeathCaseCount = (int)(sizeof(kDeathCases) / sizeof(kDeathCases[0]));
const char* const kCorpseBones[4] = {"pelvis", "head", "hand_l", "foot_r"};
} // namespace

void NpcTest::Deaths(World& world, NpcDirector& npcs, float now) {
    (void)world;
    npcs.NoLod = true;
    npcs.TrackDeathPop = true;
    for (const auto& up : npcs.Npcs()) // the dummies stand still for the rays and the kills
        if (up) up->Roam = false;
    m_Shot.clear();
    FirstPersonWeaponGameplay w;
    w.HeadMultiplier = 2.0f;
    w.LimbMultiplier = 0.75f;
    w.FalloffStart = 1000.0f;
    w.FalloffEnd = 2000.0f;
    w.FalloffMin = 1.0f;
    auto used = [&](const Npc& n) { return std::find(m_DUsed.begin(), m_DUsed.end(), n.Name) != m_DUsed.end(); };
    auto fresh = [&]() -> Npc* {
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && !used(*up) && up->Hitboxes && up->Hitboxes->Active() && up->Cct != PhysicsWorld::kNoCharacter)
                return up.get();
        return nullptr;
    };
    auto find = [&](const std::string& name) -> Npc* {
        for (const auto& up : npcs.Npcs())
            if (up && up->Name == name) return up.get();
        return nullptr;
    };
    // A round down a part's middle: from behind it, or from the outside for an arm.
    auto from = [&](const Npc& n, int part, glm::vec3& origin, glm::vec3& dir) {
        const float yaw = n.Body.Yaw();
        const glm::vec3 fwd(std::sin(yaw), 0.0f, std::cos(yaw)), left(std::cos(yaw), 0.0f, -std::sin(yaw));
        const glm::vec3 c = n.Hitboxes->Centre(n.Body, part);
        dir = fwd;
        if (part == 3 || part == 4) dir = -left;
        else if (part == 5 || part == 6) dir = left;
        origin = c - dir * 3.0f;
    };
    auto shoot = [&](Npc& n, int part, float damage) {
        w.Damage = damage;
        glm::vec3 origin, dir;
        from(n, part, origin, dir);
        const glm::vec3 c = n.Hitboxes->Centre(n.Body, part);
        bool killed = false, head = false;
        npcs.OnPlayerHit(world, RootId(n), c, origin, dir, w, &killed, &head);
        return killed;
    };
    auto step = [&](int to) { m_DStep = to; m_DAt = now; };

    // The Scene view watches the dummy being shot (for NPC_TEST_RECORD): placed off its side when it
    // becomes the target, then it only turns to keep its pelvis in frame as it falls.
    if (Npc* t = m_DNpc[0].empty() ? nullptr : find(m_DNpc[0])) {
        glm::vec3 pelvis;
        if (t->Body.BoneWorld(world, "pelvis", pelvis)) {
            const float yaw = t->Body.Yaw();
            const glm::vec3 side(std::cos(yaw), 0.0f, -std::sin(yaw)), fwd(std::sin(yaw), 0.0f, std::cos(yaw));
            if (m_DCamFor != t->Name) {
                m_DCamFor = t->Name;
                m_CamPos = pelvis + side * 3.2f + fwd * 1.6f + glm::vec3(0.0f, 0.6f, 0.0f);
            }
            const glm::vec3 d = glm::normalize(pelvis - glm::vec3(0.0f, 0.2f, 0.0f) - m_CamPos);
            m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
            m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
            m_HaveCam = true;
        }
    }
    switch (m_DStep) {
    case 0: { // wait for dummies with hitboxes
        int ready = 0;
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && up->Hitboxes && up->Hitboxes->Active()) ++ready;
        if (now > 3.0f && ready >= 3) step(1);
        break;
    }
    case 1: { // a ray at every bone's middle names that bone; a grazing ray hits the capsule only for an NPC's own queries
        if (now - m_DAt < 0.3f) break;
        Npc* a = fresh();
        if (!a) break;
        int right = 0;
        std::string wrong;
        for (int part = 0; part < NpcRagdoll::kParts; ++part) {
            glm::vec3 origin, dir;
            from(*a, part, origin, dir);
            const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z};
            PhysicsWorld::BodyPartHit bp;
            if (PhysicsWorld::RaycastBodyParts(o, d, 6.0f, bp) && bp.Kind == PhysicsWorld::kBodyPartHitbox && bp.Part == part && bp.Entity == RootId(*a)) ++right;
            else wrong += std::string(" ") + NpcPartDefOf(part).Bone + "->" + std::to_string(bp.Part);
        }
        std::printf("[NpcTest] hitbox rays: %d / %d parts named correctly%s\n", right, NpcRagdoll::kParts, wrong.c_str());
        Check(right == NpcRagdoll::kParts, "a ray down each bone's hitbox names that bone" + wrong);
        // The movement capsule is skipped by the player's rays but not by the dummies': a ray just inside the capsule's
        // edge at thigh height misses every hitbox. (PhysX gives a controller's query actor 0.8 of its radius.)
        float capFeet[3], capRadius = 0.3f;
        PhysicsWorld::GetNpcCapsule(a->Cct, capFeet, &capRadius, nullptr);
        const float side = 0.8f * capRadius * 0.9f;
        const float yaw = a->Body.Yaw();
        const glm::vec3 fwd(std::sin(yaw), 0.0f, std::cos(yaw)), left(std::cos(yaw), 0.0f, -std::sin(yaw));
        // (From in front, down the line it faces: open ground, not a wall at its back.)
        const glm::vec3 g = a->Feet + glm::vec3(0.0f, 0.55f, 0.0f) + left * side + fwd * 1.2f;
        const float o[3] = {g.x, g.y, g.z}, d[3] = {-fwd.x, -fwd.y, -fwd.z};
        QueryFilter f;
        f.HitTriggers = 0;
        RaycastHit hit;
        bool playerHit = PhysicsWorld::RaycastFiltered(o, d, 2.4f, f, hit) && hit.Hit && hit.Entity == RootId(*a);
        // (A limb's own hitbox in the ray's way - the unarmed idle's hands and thighs - is a hit on the body, not the capsule.)
        if (PhysicsWorld::BodyPartHit bp; playerHit && PhysicsWorld::RaycastBodyParts(o, d, 2.4f, bp) && bp.Entity == RootId(*a) &&
                                          bp.Kind == PhysicsWorld::kBodyPartHitbox && std::fabs(bp.Distance - hit.Distance) < 0.01f) {
            std::printf("[NpcTest] the corner ray met hitbox part %d at %.2f m (not the capsule)\n", bp.Part, bp.Distance);
            playerHit = false;
        }
        bool dummyHit = false;
        {
            PhysicsWorld::ScopedQueryPolicy policy(0xFFFFFFFEu, /*hitPlayer=*/true);
            dummyHit = PhysicsWorld::RaycastFiltered(o, d, 2.4f, f, hit) && hit.Hit && hit.Entity == RootId(*a);
        }
        Check(!playerHit, "the player's shots pass through the movement capsule's empty corners");
        Check(dummyHit, "a dummy's own queries still meet the movement capsule");
        step(2);
        break;
    }
    case 2: { // the kills
        if (m_DCase >= kDeathCaseCount) { step(3); break; }
        if (m_DNpc[0].empty()) {
            Npc* n = fresh();
            if (!n) break;
            const DeathCase& dc = kDeathCases[m_DCase];
            m_DNpc[0] = n->Name;
            m_DUsed.push_back(n->Name);
            const bool first = shoot(*n, dc.Part, dc.Damage);
            if (dc.DiesFirst) Check(first && n->Dead, std::string(dc.Name) + ": one round kills");
            else {
                Check(!n->Dead, std::string(dc.Name) + ": the first round doesn't kill");
                Check(!npcs.RagdollSettings().HitFlinch || npcs.FlinchActive(n->Index), std::string(dc.Name) + ": a round that doesn't kill kicks the struck region (hit flinch)");
            }
            if (dc.StaggerAfterFirst)
                Check(n->StaggerUntil > npcs.Now(), std::string(dc.Name) + ": a heavy hit staggers it");
            int rounds = 1;
            while (!n->Dead && rounds < 12) { shoot(*n, dc.Part, dc.Damage); ++rounds; }
            std::printf("[NpcTest] %s: dead after %d round(s)\n", dc.Name, rounds);
            Check(n->Dead, std::string(dc.Name) + ": dead after " + std::to_string(rounds) + " round(s)");
            m_DAt = now;
            break;
        }
        Npc* n = find(m_DNpc[0]);
        if (!n) { m_DNpc[0].clear(); ++m_DCase; break; }
        if (n->Ragdoll || now - m_DAt > 1.0f) {
            ++m_DDeaths;
            const bool ragdoll = n->Ragdoll != nullptr;
            if (ragdoll) ++m_DRagdolls;
            Check(ragdoll, std::string(kDeathCases[m_DCase].Name) + ": a ragdoll took the body");
            if (ragdoll) {
                std::printf("[NpcTest] %s: pose pop %.1f cm between the last animated frame and the first ragdoll one\n",
                            kDeathCases[m_DCase].Name, n->DeathPop * 100.0f);
                Check(n->DeathPop >= 0.0f && n->DeathPop <= 0.15f,
                      std::string(kDeathCases[m_DCase].Name) + ": no pose pop over 15 cm (" + std::to_string(n->DeathPop * 100.0f) + " cm)");
                m_DWorstPop = std::max(m_DWorstPop, n->DeathPop);
                Check(n->Ragdoll->DriveLeft() > 0.0f || npcs.Now() - n->DiedAt > n->Ragdoll->DriveFadeTime(),
                      std::string(kDeathCases[m_DCase].Name) + ": the joints start powered");
            }
            m_DNpc[0].clear();
            ++m_DCase;
        }
        break;
    }
    case 3: { // a corpse is shot
        Npc* corpse = nullptr;
        for (const auto& up : npcs.Npcs())
            if (up && up->Dead && up->Ragdoll) { corpse = up.get(); break; }
        if (!corpse) { if (now - m_DAt > 10.0f) { Check(false, "a corpse to shoot"); step(kLastStep); } break; }
        const bool asleep = corpse->Ragdoll->Asleep();
        if (!asleep && now - corpse->DiedAt < 7.0f) break; // let it settle
        if (m_DNpc[1].empty()) {
            m_DNpc[1] = corpse->Name;
            m_DCorpseAsleep = asleep;
            std::printf("[NpcTest] corpse %s %.1f s after death\n", asleep ? "asleep" : "still moving", now - corpse->DiedAt);
            Check(asleep || !npcs.RagdollSettings().PoweredRagdoll, "a powered corpse has come to rest and sleeps within 7 s");
            {
                // At rest it lies, not kneels: no knee folded past 60 degrees (thigh centre -> calf centre -> foot centre).
                float worst = 0.0f;
                for (int side = 0; side < 2; ++side) {
                    const glm::vec3 a = corpse->Ragdoll->PartPosition(7 + 2 * side), b = corpse->Ragdoll->PartPosition(8 + 2 * side), f = corpse->Ragdoll->PartPosition(14 + side);
                    const glm::vec3 u = glm::normalize(b - a), v = glm::normalize(f - b);
                    worst = std::max(worst, glm::degrees(std::acos(std::clamp(glm::dot(u, v), -1.0f, 1.0f))));
                }
                std::printf("[NpcTest] corpse at rest: worst knee bend %.0f deg\n", worst);
                Check(worst < 60.0f || !npcs.RagdollSettings().PoweredRagdoll, "a powered corpse at rest lies, its knees not folded past 60 degrees (" + std::to_string(worst) + ")");
            }
            for (int k = 0; k < NpcRagdoll::kParts; ++k) m_DCorpseBefore[k] = corpse->Ragdoll->PartPosition(k);
            for (int k = 0; k < 4; ++k) corpse->Body.BoneWorld(kCorpseBones[k], m_DBoneBefore[k]);
            // From the side at the chest part's middle.
            const glm::vec3 c = corpse->Ragdoll->PartPosition(1);
            const float o[3] = {c.x - 3.0f, c.y, c.z}, d[3] = {1.0f, 0.0f, 0.0f};
            PhysicsWorld::BodyPartHit bp;
            const bool part = PhysicsWorld::RaycastBodyParts(o, d, 6.0f, bp) && bp.Kind == PhysicsWorld::kBodyPartRagdoll && bp.Entity == RootId(*corpse);
            Check(part, "a ray at a corpse names a ragdoll part");
            w.Damage = 34.0f;
            npcs.OnPlayerHit(world, RootId(*corpse), c, glm::vec3(o[0], o[1], o[2]), glm::vec3(1.0f, 0.0f, 0.0f), w, nullptr, nullptr);
            Check(!corpse->Ragdoll->Asleep(), std::string("a round wakes the corpse") + (m_DCorpseAsleep ? "" : " (it was still moving)"));
            m_DAt = now;
            break;
        }
        Npc* c = find(m_DNpc[1]);
        if (!c || !c->Ragdoll) { step(kLastStep); break; }
        if (now - m_DAt > 0.6f) {
            float moved = 0.0f;
            for (int k = 0; k < NpcRagdoll::kParts; ++k) moved += glm::length(c->Ragdoll->PartPosition(k) - m_DCorpseBefore[k]);
            float drawn = 0.0f;
            for (int k = 0; k < 4; ++k) {
                glm::vec3 bone(0.0f);
                c->Body.BoneWorld(kCorpseBones[k], bone);
                drawn += glm::length(bone - m_DBoneBefore[k]);
            }
            std::printf("[NpcTest] corpse shot: parts moved %.1f cm in all, the drawn bones %.1f cm\n", moved * 100.0f, drawn * 100.0f);
            Check(moved > 0.10f, "the corpse reacts to a shot (parts moved " + std::to_string(moved * 100.0f) + " cm in all)");
            Check(drawn > 0.05f, "the drawn body follows the shoved corpse (" + std::to_string(drawn * 100.0f) + " cm)");
            step(kLastStep);
        }
        break;
    }
    default:
        break;
    }
    if (now >= m_Duration && !m_Done) {
        m_Done = true;
        PrintCosts(npcs);
        Check(m_DStep >= kLastStep, "every stage of the scenario ran (reached " + std::to_string(m_DStep) + ")");
        Check(m_DDeaths == kDeathCaseCount && m_DRagdolls == kDeathCaseCount, "every kill ended in a ragdoll (" + std::to_string(m_DRagdolls) + ")");
        std::printf("[NpcTest] worst pose pop %.1f cm\n", m_DWorstPop * 100.0f);
    }
    // Early finish once the last stage is through.
    if (m_DStep >= kLastStep && !m_Done && now - m_DAt > 1.0f) m_Duration = now;
}

// --- feet: foot IK on the Arena's ramp ---------------------------------------------------------------------------------
// One dummy, standing still: each foot's height over the ground under it measured on the flat, then standing across the
// ramp (one foot uphill of the other) and facing up it. With the feet on the ground both stay at the flat's height.
namespace {
const glm::vec3 kRampCentre(0.0f, 2.0f, -7.0f); // "Plinth Ramp": 14 degrees, rising toward -z
bool FootGap(const Npc& n, const char* bone, float& gap) {
    glm::vec3 foot;
    if (!n.Body.BoneWorld(bone, foot)) return false;
    const float o[3] = {foot.x, foot.y + 0.5f, foot.z}, d[3] = {0.0f, -1.0f, 0.0f};
    QueryFilter f;
    f.HitTriggers = 0;
    RaycastHit hit;
    if (!PhysicsWorld::RaycastSolid(o, d, 1.5f, f, hit) || !hit.Hit) return false;
    gap = foot.y - hit.Point[1];
    return true;
}
} // namespace

void NpcTest::Feet(NpcDirector& npcs, float now) {
    npcs.FootIKEverywhere = true;
    Npc* n = nullptr;
    for (const auto& up : npcs.Npcs())
        if (up && !up->Dead && (m_FProbe.empty() || up->Name == m_FProbe)) { n = up.get(); break; }
    if (!n || n->Cct == PhysicsWorld::kNoCharacter) return;
    m_FProbe = n->Name;
    for (const auto& up : npcs.Npcs())
        if (up && !up->Dead) { up->Roam = false; up->WantMove = false; up->LookPoint = up->Feet + glm::vec3(0.0f, 1.6f, 10.0f); }
    // Stages: 0 flat (measure at 2.5 s), 1 across the ramp (placed at 3 s, measured at 5.5 s), 2 up it (6 s, 8.5 s).
    struct Stage { float Place, Measure; glm::vec3 Look; const char* Name; };
    static const Stage kStages[3] = {{-1.0f, 2.5f, glm::vec3(0.0f, 0.0f, 10.0f), "flat"},
                                     {3.0f, 5.5f, glm::vec3(10.0f, 0.0f, 0.0f), "across"},
                                     {6.0f, 8.5f, glm::vec3(0.0f, 0.0f, -10.0f), "uphill"}};
    for (int s = 0; s < 3; ++s) {
        const Stage& st = kStages[s];
        if (st.Place >= 0.0f && now >= st.Place && m_FStage < s) {
            m_FStage = s;
            const float p[3] = {kRampCentre.x, kRampCentre.y, kRampCentre.z};
            PhysicsWorld::SetNpcFootPosition(n->Cct, p);
            // The director's own state goes with it: it reads the jump as a speed otherwise (metres in a frame) and eases
            // the pace down from there, sliding the dummy off the ramp and across the Arena before the measurement.
            n->Feet = kRampCentre;
            n->Velocity = glm::vec3(0.0f);
            n->PushVel = glm::vec3(0.0f);
            n->FallSpeed = 0.0f;
        }
    }
    const Stage& cur = kStages[std::max(m_FStage, 0)];
    n->LookPoint = n->Feet + glm::vec3(0.0f, 1.6f, 0.0f) + cur.Look;
    for (int s = 0; s < 3; ++s) {
        const Stage& st = kStages[s];
        if (now < st.Measure || m_FMeasured > s) continue;
        m_FMeasured = s + 1;
        float l = 0.0f, r = 0.0f;
        const bool ok = FootGap(*n, "foot_l", l) && FootGap(*n, "foot_r", r);
        std::printf("[NpcTest] feet %-6s at (%.2f, %.2f, %.2f): left %.1f cm, right %.1f cm over the ground\n", st.Name, n->Feet.x, n->Feet.y,
                    n->Feet.z, l * 100.0f, r * 100.0f);
        if (s == 0) {
            m_FFlat = 0.5f * (l + r);
            Check(ok, "the feet were measured on the flat");
        } else {
            Check(ok && n->Feet.y > 0.3f, std::string("the dummy stands on the ramp (") + st.Name + ")");
            Check(ok && std::abs(l - m_FFlat) < 0.04f && std::abs(r - m_FFlat) < 0.04f,
                  std::string("both feet on the ramp's surface, ") + st.Name + " (within 4 cm of the flat's " +
                      std::to_string((int)std::lround(m_FFlat * 100.0f)) + " cm)");
        }
        m_Shot = std::string("feet_") + st.Name;
    }
    // The Scene view on its legs, side on (for the shots).
    {
        const float yaw = n->Body.Yaw() + glm::radians(-90.0f);
        const glm::vec3 hip = n->Feet + glm::vec3(0.0f, 0.6f, 0.0f);
        m_CamPos = hip + glm::vec3(std::sin(yaw), 0.15f, std::cos(yaw)) * 2.4f;
        const glm::vec3 d = glm::normalize(hip - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        m_HaveCam = true;
    }
    if (now >= 9.0f && !m_Done) {
        m_Done = true;
        Check(m_FMeasured == 3, "every stage ran");
    }
}

// --- blood: the volumetric blood on every kind of hit ----------------------------------------------
// tests/scenes/BloodTest.json: four dummies in a line with a wall 1.5 m behind them, standing still. One is wounded in
// the chest, one killed through the chest (the spray meets the wall), one through the head, and the chest kill's
// corpse is shot. The Scene view watches each from the side; shots at the moments the spray is up, falling and down.
void NpcTest::Blood(World& world, NpcDirector& npcs, float now) {
    npcs.NoLod = true;
    for (const auto& up : npcs.Npcs()) // standing still
        if (up) { up->Roam = false; up->WantMove = false; }
    m_Shot.clear();
    struct Case { const char* Name; int Part; float Damage; bool Corpse; };
    static const Case kCases[] = {{"wound", 1, 12.0f, false},
                                  {"chest_kill", 1, 400.0f, false},
                                  {"head_kill", 2, 400.0f, false},
                                  {"corpse", 1, 30.0f, true},
                                  {"point_blank", 1, 12.0f, false}}; // a round from a metre away: back onto the gun
    constexpr int kCaseCount = (int)(sizeof(kCases) / sizeof(kCases[0]));
    static const float kShotTimes[] = {0.1f, 0.3f, 0.6f, 1.0f, 1.8f};
    constexpr int kShotCount = (int)(sizeof(kShotTimes) / sizeof(kShotTimes[0]));
    auto find = [&](const std::string& name) -> Npc* {
        for (const auto& up : npcs.Npcs())
            if (up && up->Name == name) return up.get();
        return nullptr;
    };
    // The Scene view off the dummy's left, a little in front, level with the chest.
    auto frame = [&](const Npc& n) {
        glm::vec3 chest = n.Feet + glm::vec3(0.0f, 1.2f, 0.0f);
        if (n.Hitboxes && n.Hitboxes->Active()) chest = n.Hitboxes->Centre(n.Body, 1);
        if (m_BCamSet) return;
        m_BCamSet = true;
        // Square to the spray's line (it leaves along -Z toward the wall), a little in front and above.
        m_CamPos = chest + glm::vec3(-2.6f, 0.4f, 0.1f);
        const glm::vec3 d = glm::normalize(chest + glm::vec3(0.0f, -0.45f, -0.8f) - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        m_HaveCam = true;
    };
    switch (m_BStep) {
    case 0: { // a line of dummies with hitboxes, sorted west to east
        std::vector<const Npc*> ready;
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead && up->Hitboxes && up->Hitboxes->Active()) ready.push_back(up.get());
        if (now < 3.0f || ready.size() < 3) break;
        std::sort(ready.begin(), ready.end(), [](const Npc* a, const Npc* b) { return a->Feet.x < b->Feet.x; });
        m_BLine.clear();
        for (const Npc* n : ready) m_BLine.push_back(n->Name);
        Check(BloodStats && BloodStats->Loaded, "the volumetric blood data is imported and loaded");
        m_BStep = 1;
        m_BAt = now;
        break;
    }
    case 1: { // the cases, 3 s apart
        if (m_BCase >= kCaseCount) { m_BStep = 4; m_BAt = now; m_BSurface = 0; m_BFired = false; break; }
        const Case& c = kCases[m_BCase];
        // The wound and the kills each on their own dummy; the corpse is the chest kill's.
        const bool pointBlank = std::string(c.Name) == "point_blank";
        const std::string& who = m_BLine[std::min<size_t>(c.Corpse ? 1 : pointBlank ? 3 : (size_t)m_BCase, m_BLine.size() - 1)];
        Npc* n = find(who);
        if (!n) { ++m_BCase; break; }
        if (!m_BFired) {
            if (now - m_BAt < 0.5f) break;
            m_BCamSet = false;
            frame(*n);
            FirstPersonWeaponGameplay w;
            w.Damage = c.Damage;
            w.FalloffStart = 1000.0f;
            w.FalloffEnd = 2000.0f;
            w.FalloffMin = 1.0f;
            glm::vec3 target = n->Feet + glm::vec3(0.0f, 1.2f, 0.0f);
            if (!c.Corpse && n->Hitboxes && n->Hitboxes->Active()) target = n->Hitboxes->Centre(n->Body, c.Part);
            if (c.Corpse && n->Ragdoll) target = n->Ragdoll->PartPosition(c.Part);
            // From the player's side of the line (+Z), level: out through the back, toward the wall.
            const glm::vec3 dir = c.Corpse ? glm::normalize(glm::vec3(0.0f, -0.6f, -1.0f)) : glm::vec3(0.0f, 0.0f, -1.0f);
            const glm::vec3 origin = target - dir * (pointBlank ? 1.0f : 4.0f);
            m_BGearBefore = BloodStats ? BloodStats->GearSplats : 0;
            bool killed = false, head = false;
            const bool hit = npcs.OnPlayerHit(world, RootId(*n), target, origin, dir, w, &killed, &head);
            m_BSpawnBefore = BloodStats ? BloodStats->SpraysSpawned : 0;
            m_BPuffsBefore = BloodStats ? BloodStats->Puffs : 0;
            m_BHeadshotsBefore = BloodStats ? BloodStats->BigHeadshots : 0;
            m_BFired = true;
            m_BFiredAt = now;
            m_BShot = 0;
            std::printf("[NpcTest] blood %s: %s hit=%d killed=%d head=%d\n", c.Name, who.c_str(), hit ? 1 : 0, killed ? 1 : 0, head ? 1 : 0);
            break;
        }
        frame(*n);
        const float t = now - m_BFiredAt;
        if (m_BShot < kShotCount && t >= kShotTimes[m_BShot]) {
            char name[64];
            std::snprintf(name, sizeof(name), "blood_%d_%s_%02d", m_BCase, c.Name, (int)(kShotTimes[m_BShot] * 10.0f + 0.5f));
            m_Shot = name;
            ++m_BShot;
        }
        if (!m_BChecked && t > 0.05f && BloodStats) {
            // The hit threw a spray (the director queued the flesh hit; the host drained it into the blood).
            m_BChecked = true;
            Check(BloodStats->SpraysSpawned > m_BSpawnBefore,
                  std::string("blood ") + c.Name + ": a spray thrown (" + std::to_string(BloodStats->SpraysSpawned - m_BSpawnBefore) + ")");
            if (std::string(c.Name) == "chest_kill") {
                Check(BloodStats->LastSprayClipped, "blood chest_kill: the spray meets the wall behind (clip plane set)");
                Check(BloodStats->LastExited && BloodStats->LastExitFound, "blood chest_kill: the round exits where his back really is");
            }
            if (!c.Corpse)
                Check(BloodStats->Puffs > m_BPuffsBefore, std::string("blood ") + c.Name + ": the impact puff (burst, mist, droplets)");
            if (std::string(c.Name) == "head_kill")
                Check(BloodStats->BigHeadshots > m_BHeadshotsBefore, "blood head_kill: a big headshot (the gore sound over the flesh hit)");
            if (pointBlank)
                Check(BloodStats->GearSplats > m_BGearBefore,
                      "blood point_blank: blood back on the player's gun / hands (" + std::to_string(BloodStats->GearSplats - m_BGearBefore) + ")");
        }
        if (t >= 3.0f) { ++m_BCase; m_BFired = false; m_BChecked = false; m_BAt = now; }
        break;
    }
    case 2: // every spray has fallen and is gone; the pools have spread: an overview of what's left
        if (now - m_BAt < 5.0f && !m_BLine.empty()) {
            // Close on the wounded dummy (still standing): the entry wound and the run below it.
            if (const Npc* w = find(m_BLine[0])) {
                const glm::vec3 chest = w->Feet + glm::vec3(0.0f, 1.25f, 0.0f);
                m_CamPos = chest + glm::vec3(0.35f, 0.15f, 1.15f);
                const glm::vec3 d = glm::normalize(chest - glm::vec3(0.0f, 0.1f, 0.0f) - m_CamPos);
                m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
                m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
                m_HaveCam = true;
                if (!m_BCloseUp && now - m_BAt > 4.0f) { m_Shot = "blood_wounded"; m_BCloseUp = true; }
            }
            break;
        }
        if (now - m_BAt < 16.0f) {
            m_CamPos = glm::vec3(-2.5f, 3.2f, 0.5f);
            const glm::vec3 d = glm::normalize(glm::vec3(0.5f, 0.0f, -5.0f) - m_CamPos);
            m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
            m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
            m_HaveCam = true;
            if (!m_BOverview && now - m_BAt > 15.0f) { m_Shot = "blood_overview"; m_BOverview = true; }
            break;
        }
        if (BloodStats) {
            std::printf("[NpcTest] blood: %d sprays thrown, %d still playing, GPU %.3f ms avg / %.3f ms worst frame\n",
                        BloodStats->SpraysSpawned, BloodStats->ActiveSprays, BloodStats->GpuMsAvg, BloodStats->GpuMsMax);
            Check(BloodStats->ActiveSprays == 0, "every spray played out and was dropped");
            std::printf("[NpcTest] blood: %d stains, %d pools\n", BloodStats->DecalsSpawned, BloodStats->PoolsSpawned);
            Check(BloodStats->DecalsSpawned >= 6, "the blood left stains (floor splats, wall spatter)");
            Check(BloodStats->PoolsSpawned >= 2, "a pool spread under each of the two corpses");
            std::printf("[NpcTest] blood: %d splats on bodies\n", BloodStats->SplatsSpawned);
            Check(BloodStats->SplatsSpawned >= 4, "the hits left blood on the bodies (entry, exit, the run below)");
        }
        m_Done = true;
        m_BStep = 3;
        break;
    case 4: { // rounds into each surface panel: its own burst and its own textured hole
        static const char* const kPanels[] = {"Metal Plate", "Wood Panel", "Brick Panel", "Glass Pane", "Mud Bank", "Tile Panel", "Concrete Block"};
        constexpr int kPanelCount = (int)(sizeof(kPanels) / sizeof(kPanels[0]));
        if (m_BSurface >= kPanelCount || !ShootWorld || !BloodStats) { m_BStep = 2; m_BAt = now; break; }
        glm::vec3 centre(0.0f);
        bool found = false;
        for (auto [e, name] : world.Registry.view<NameComponent>().each())
            if (name.Name == kPanels[m_BSurface]) { centre = glm::vec3(world.GetCachedWorldTransform(e)[3]); found = true; }
        if (!found) { ++m_BSurface; break; }
        // Square on, from the room side (+X), a little above.
        m_CamPos = centre + glm::vec3(1.6f, 0.35f, 0.25f);
        const glm::vec3 look = glm::normalize(centre - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(look.z, look.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(look.y, -1.0f, 1.0f)));
        m_HaveCam = true;
        const float t = now - m_BAt;
        if (!m_BFired && t > 0.4f) {
            m_BImpactsBefore = BloodStats->Impacts;
            m_BHolesBefore = BloodStats->TexturedHoles;
            for (int k = 0; k < 3; ++k) // three rounds, a hand's width apart
                ShootWorld(centre + glm::vec3(3.0f, 0.25f - 0.2f * (float)k, -0.2f + 0.2f * (float)k), glm::vec3(-1.0f, 0.0f, 0.0f));
            m_BFired = true;
            m_BFiredAt = now;
        }
        if (m_BFired) {
            const float s = now - m_BFiredAt;
            std::string panel = kPanels[m_BSurface];
            for (char& c : panel) c = c == ' ' ? '_' : (char)std::tolower((unsigned char)c);
            if (s >= 0.08f && s - (float)(1.0 / 60.0) < 0.08f) m_Shot = "surface_" + panel + "_burst";
            if (s >= 1.6f) {
                m_Shot = "surface_" + panel + "_hole";
                Check(BloodStats->Impacts >= m_BImpactsBefore + 3 && BloodStats->TexturedHoles >= m_BHolesBefore + 3,
                      std::string("surface ") + kPanels[m_BSurface] + ": its burst and textured holes (" +
                          std::to_string(BloodStats->TexturedHoles - m_BHolesBefore) + ")");
                ++m_BSurface;
                m_BFired = false;
                m_BAt = now;
            }
        }
        break;
    }
    default:
        break;
    }
}

// --- ragdoll: the deaths, watched from four sides ------------------------------------------------------------------------
namespace {
// tests/scenes/RagdollTest.json's NPC Spawns, in order.
enum RagSpawn { RS_Open, RS_Ak, RS_Shotgun, RS_Crate, RS_Stairs, RS_Ledge, RS_Wall, RS_Sprinter, RS_Jogger, RS_Strafer, RS_PileA, RS_PileB, RS_Count };
struct RagCase {
    const char* Name;
    int Spawn;
    int Part;        // the hitbox part struck
    float From;      // degrees round the body the round comes from: 0 its front, 90 its left, 180 behind, -90 its right
    float Damage;
    int Move;        // 0 stands, 1 sprints, 2 jogs, 3 strafes
    glm::vec3 MoveTo;
};
const RagCase kRagCases[] = {
    {"open_head_front", RS_Open, 2, 0.0f, 120.0f, 0, {}},
    {"ak_chest_left", RS_Ak, 1, 90.0f, 45.0f, 0, {}},
    {"shotgun_back", RS_Shotgun, 1, 180.0f, 150.0f, 0, {}},
    {"crate_chest_front", RS_Crate, 1, 0.0f, 150.0f, 0, {}},
    {"stairs_back", RS_Stairs, 1, 180.0f, 60.0f, 0, {}},
    {"ledge_back", RS_Ledge, 0, 180.0f, 60.0f, 0, {}},
    {"wall_chest_front", RS_Wall, 1, 0.0f, 60.0f, 0, {}},
    {"sprint_thigh", RS_Sprinter, 7, 0.0f, 45.0f, 1, {9.0f, 0.0f, 4.0f}},
    {"jog_chest_right", RS_Jogger, 1, -90.0f, 45.0f, 2, {-9.0f, 0.0f, 8.0f}},
    {"strafe_head", RS_Strafer, 2, 0.0f, 120.0f, 3, {}},
    {"pile_first", RS_PileA, 1, 0.0f, 45.0f, 0, {}},
    {"pile_onto", RS_PileB, 1, 0.0f, 60.0f, 0, {}},
};
constexpr int kRagCaseCount = (int)(sizeof(kRagCases) / sizeof(kRagCases[0]));
// Moments (seconds after the round) each death is frozen at and shot; then once more at rest.
constexpr float kRagMoments[] = {0.15f, 0.4f, 0.8f, 1.6f};
constexpr int kRagMomentCount = (int)(sizeof(kRagMoments) / sizeof(kRagMoments[0]));
constexpr float kRagRestBy = 7.0f;   // a body still moving this long after the round fails the rest check
struct RagView { const char* Name; float Yaw, Pitch, Dist; bool Ground; };
const RagView kRagViews[4] = {{"front", 0.0f, -14.0f, 1.9f, false}, {"side", 90.0f, -14.0f, 1.9f, false},
                              {"high", 225.0f, -55.0f, 2.1f, false}, {"ground", 150.0f, 0.0f, 1.8f, true}};
constexpr int kFramesPerView = 3; // the camera is placed, the next frame draws with it, the third is kept

Npc* NpcAtSpawn(NpcDirector& npcs, int spawn) {
    for (const auto& up : npcs.Npcs())
        if (up && up->SpawnIndex == spawn) return up.get();
    return nullptr;
}
glm::vec3 BodyCentre(const Npc& n) {
    return n.Ragdoll ? 0.5f * (n.Ragdoll->PartPosition(0) + n.Ragdoll->PartPosition(1)) : n.Feet + glm::vec3(0.0f, 1.0f, 0.0f);
}
} // namespace

void NpcTest::Ragdoll(World& world, NpcDirector& npcs, const Player& player) {
    (void)player;
    const float now = npcs.Now();
    npcs.NoLod = true;
    m_Shot.clear();
    if (m_RCase < 0) {
        // Every dummy in before the first death.
        int alive = 0;
        for (const auto& up : npcs.Npcs())
            if (up && !up->Dead) ++alive;
        if (now < 2.0f || alive < RS_Count) {
            if (now > 20.0f && !m_Done) {
                Check(false, "every RagdollTest dummy spawned (" + std::to_string(alive) + " of " + std::to_string(RS_Count) + ")");
                m_Done = true;
            }
            return;
        }
        m_RCase = 0;
        m_RStage = 0;
        m_RAt = now;
    }
    // Every corpse that goes must have been out of sight for Unseen Time first.
    {
        static std::vector<std::pair<std::string, float>> seen; // name -> last time the player could see it (-1: never checked)
        for (auto& [name, last] : seen) {
            bool there = false;
            for (const auto& up : npcs.Npcs())
                if (up && up->Name == name) there = true;
            if (!there && last > -1e8f) {
                Check(now - last >= npcs.Settings().UnseenTime - 0.05f,
                      name + "'s corpse went only after " + std::to_string(npcs.Settings().UnseenTime) + " s out of sight (" + std::to_string(now - last) + " s)");
                last = -1e9f;
            }
        }
        PlayerSnapshot ps;
        ps.Valid = true;
        ps.Eye = player.Cam.Position;
        ps.Forward = player.Cam.Front();
        for (const auto& up : npcs.Npcs()) {
            if (!up || !up->Dead || !up->Ragdoll) continue;
            auto it = std::find_if(seen.begin(), seen.end(), [&](const auto& e) { return e.first == up->Name; });
            if (it == seen.end()) { seen.push_back({up->Name, now}); it = seen.end() - 1; }
            for (int part : {0, 1, 2})
                if (NpcDirector::PlayerSees(ps, up->Ragdoll->PartPosition(part))) it->second = now;
        }
    }

    // A frozen moment: four views of the body, then time goes on.
    if (m_RFreeze) {
        Npc* n = m_RName.empty() ? nullptr : NpcAtSpawn(npcs, m_RCase < kRagCaseCount ? kRagCases[m_RCase].Spawn : RS_Open);
        const int view = m_RFrame / kFramesPerView, sub = m_RFrame % kFramesPerView;
        if (!n || view >= 4) {
            m_RFreeze = false;
            ++m_RMoment;
            return;
        }
        const RagView& v = kRagViews[view];
        const glm::vec3 c = BodyCentre(*n);
        const float yaw = m_RYaw + glm::radians(v.Yaw);
        const glm::vec3 flat(std::sin(yaw), 0.0f, std::cos(yaw));
        const float pitch = glm::radians(v.Pitch);
        m_CamPos = c + (flat * std::cos(pitch) - glm::vec3(0.0f, std::sin(pitch), 0.0f)) * v.Dist;
        if (v.Ground) {
            float lowest = c.y;
            if (n->Ragdoll)
                for (int k = 0; k < NpcRagdoll::kRagParts; ++k) lowest = std::min(lowest, n->Ragdoll->PartPosition(k).y);
            m_CamPos.y = lowest + 0.15f;
        }
        const glm::vec3 d = glm::normalize(c - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        m_HaveCam = true;
        if (sub == kFramesPerView - 1) {
            char name[128];
            const bool rest = m_RMoment >= kRagMomentCount;
            char when[16];
            if (rest) std::snprintf(when, sizeof when, "rest");
            else std::snprintf(when, sizeof when, "%04dms", (int)std::lround(kRagMoments[m_RMoment] * 1000.0f));
            std::snprintf(name, sizeof name, "rag_%02d_%s_%s_%s", m_RCase, m_RName.c_str(), when, v.Name);
            m_Shot = name;
        }
        ++m_RFrame;
        return;
    }

    // The persistence stage, after every death.
    if (m_RCase >= kRagCaseCount) {
        if (m_RLookAway == 0) {
            // Looking at the bodies until the last is past Corpse Time: all still there (the check above fails any that went while seen).
            if (now - m_RLastDeath > npcs.Settings().CorpseTime + 1.0f) {
                int corpses = 0;
                for (const auto& up : npcs.Npcs())
                    if (up && up->Dead) ++corpses;
                std::printf("[NpcTest] ragdoll: %d corpse(s) while looked at, %.0f s after the first death\n", corpses, now - m_RKilledAt);
                m_RLookAway = 1;
                m_RLookAwayAt = now;
                m_RViewYaw = 90.0f; // turn round: nothing of them in view
            }
        } else if (now - m_RLookAwayAt > npcs.Settings().UnseenTime + 1.5f) {
            int corpses = 0;
            for (const auto& up : npcs.Npcs())
                if (up && up->Dead) ++corpses;
            Check(corpses == 0, "out of sight past Corpse Time, every corpse has gone (" + std::to_string(corpses) + " left)");
            std::printf("[NpcTest] ragdoll worst: self %.1f cm (%s), ground %.1f cm (%s), joint %.1f deg (%s)\n", m_RWorstSelf * 100.0f,
                        m_RWorstSelfAt.c_str(), m_RWorstGround * 100.0f, m_RWorstGroundAt.c_str(), m_RWorstJoint, m_RWorstJointAt.c_str());
            PrintCosts(npcs);
            m_Done = true;
            m_Duration = now;
        }
        return;
    }

    const RagCase& rc = kRagCases[m_RCase];
    Npc* n = NpcAtSpawn(npcs, rc.Spawn);
    if (!n) {
        Check(false, std::string(rc.Name) + ": its dummy is there");
        ++m_RCase;
        m_RStage = 0;
        m_RAt = now;
        return;
    }
    // The dummy's part centre (its hitbox's middle: the bone to the next).
    auto partCentre = [&](int part) {
        const NpcPartDef& d = NpcPartDefOf(part);
        glm::vec3 a(0.0f), b(0.0f);
        if (!n->Body.BoneWorld(world, d.Bone, a)) return n->Feet + glm::vec3(0.0f, 1.2f, 0.0f);
        if (d.End && n->Body.BoneWorld(world, d.End, b)) return 0.5f * (a + b);
        return a + glm::vec3(0.0f, 0.08f, 0.0f);
    };
    if (m_RStage == 0) {
        // Getting going (the movers), then the round.
        const float t = now - m_RAt;
        const float yaw0 = n->Body.Yaw();
        const glm::vec3 fwd0(std::sin(yaw0), 0.0f, std::cos(yaw0));
        if (rc.Move != 0 && !n->Dead) {
            if (rc.Move == 3) {
                npcs.ScriptedMove(n->Index, n->Feet + glm::vec3(4.0f, 0.0f, 0.0f), Gait::Walk);
                n->LookPoint = n->Feet + glm::vec3(0.0f, 1.45f, 10.0f);
            } else {
                npcs.ScriptedMove(n->Index, rc.MoveTo, rc.Move == 1 ? Gait::Run : Gait::Jog);
                n->LookPoint = rc.MoveTo + glm::vec3(0.0f, 1.6f, 0.0f);
            }
        }
        const float speed = glm::length(glm::vec2(n->Velocity.x, n->Velocity.z));
        const float want = rc.Move == 1 ? 3.8f : rc.Move == 2 ? 2.4f : rc.Move == 3 ? 1.0f : 0.0f;
        const bool ready = rc.Move == 0 ? t > 0.6f : ((speed >= want && t > 1.0f) || t > 4.0f);
        if (!ready) return;
        // The round: along its line through the part, from the side the case names.
        const float from = glm::radians(rc.From);
        const glm::vec3 left0(std::cos(yaw0), 0.0f, -std::sin(yaw0));
        const glm::vec3 travel = -(fwd0 * std::cos(from) + left0 * std::sin(from));
        const glm::vec3 point = partCentre(rc.Part);
        std::printf("[NpcTest] ragdoll %s: %s killed at %.1f m/s by a %.0f-damage round into %s from %.0f deg\n", rc.Name, n->Name.c_str(), speed,
                    rc.Damage, NpcPartDefOf(rc.Part).Bone, rc.From);
        npcs.Kill(world, *n, travel, point, 30.0f + 0.4f * rc.Damage, rc.Part);
        m_RName = rc.Name;
        m_RYaw = yaw0; // (the views turn about the body's facing at death)
        if (m_RCase == 0) m_RKilledAt = now;
        m_RLastDeath = now;
        m_RAt = now;
        m_RStage = 1;
        m_RMoment = 0;
        m_RCaseSelf = m_RCaseGround = m_RCaseJoint = m_RGroundNow = 0.0f;
        m_RCaseGroundAt.clear();
        m_RCaseJointAt.clear();
        return;
    }
    // Falling and lying: every frame's checks, the frozen moments, then the case's verdict.
    const float t = now - m_RAt;
    if (n->Ragdoll) {
        const int id = n->Ragdoll->Id();
        int a = -1, b = -1;
        const float self = PhysicsWorld::RagdollWorstSelfPenetration(id, &a, &b);
        if (self > m_RCaseSelf) m_RCaseSelf = self;
        if (self > m_RWorstSelf) {
            m_RWorstSelf = self;
            m_RWorstSelfAt = std::string(rc.Name) + " " + NpcRagdollDefOf(a).Bone + "/" + NpcRagdollDefOf(b).Bone + " at " + std::to_string(t) + " s";
        }
        float groundNow = 0.0f;
        for (int k = 0; k < NpcRagdoll::kRagParts; ++k) {
            // Into the ground: the capsule's lowest point against the solid world under it.
            float p[3], q[4], r = 0.05f, hl = 0.1f;
            if (!PhysicsWorld::GetRagdollPart(id, k, p, q) || !PhysicsWorld::GetRagdollPartShape(id, k, &r, &hl)) continue;
            const glm::vec3 c(p[0], p[1], p[2]), axis = glm::quat(q[3], q[0], q[1], q[2]) * glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 end = axis.y < 0.0f ? c + axis * hl : c - axis * hl;
            const float o[3] = {end.x, end.y + 0.4f, end.z}, down[3] = {0.0f, -1.0f, 0.0f};
            RaycastHit hit;
            // (Only a point really inside something counts: a hand hanging past a ledge's edge is under its top, not in it.)
            const float bottom[3] = {end.x, end.y - r + 0.005f, end.z}, probe[3] = {0.0f, -1.0f, 0.0f};
            RaycastHit inside;
            const bool in = PhysicsWorld::RaycastSolid(bottom, probe, 0.001f, QueryFilter{}, inside) && inside.Hit && inside.Distance < 1e-4f;
            if (in && PhysicsWorld::RaycastSolid(o, down, 2.0f, QueryFilter{}, hit) && hit.Hit && hit.Distance > 1e-3f) {
                const float into = hit.Point[1] - (end.y - r);
                groundNow = std::max(groundNow, into);
                if (into > m_RCaseGround) { m_RCaseGround = into; m_RCaseGroundAt = std::string(NpcRagdollDefOf(k).Bone) + " at " + std::to_string(t).substr(0, 4) + " s"; }
                if (into > m_RWorstGround) { m_RWorstGround = into; m_RWorstGroundAt = std::string(rc.Name) + " " + NpcRagdollDefOf(k).Bone; }
            }
            // Past its range.
            float pos[3], lo[3], hi[3];
            if (k > 0 && PhysicsWorld::GetRagdollJoint(id, k, pos, lo, hi))
                for (int ax = 0; ax < 3; ++ax) {
                    const float over = glm::degrees(std::max(lo[ax] - pos[ax], pos[ax] - hi[ax]));
                    if (over > m_RCaseJoint) {
                        static const char* const kAxisName[3] = {"twist", "swing y", "swing z"};
                        m_RCaseJoint = over;
                        m_RCaseJointAt = std::string(NpcRagdollDefOf(k).Bone) + " " + kAxisName[ax] + " at " + std::to_string(t).substr(0, 4) + " s";
                    }
                    if (over > m_RWorstJoint) {
                        static const char* const kAxes[3] = {"twist", "swing y", "swing z"};
                        m_RWorstJoint = over;
                        m_RWorstJointAt = std::string(rc.Name) + " " + NpcRagdollDefOf(k).Bone + " " + kAxes[ax];
                    }
                }
        }
        m_RGroundNow = groundNow;
    }
    // (At rest: the body asleep.)
    const bool rest = n->Ragdoll && (n->Ragdoll->Asleep() || t > kRagRestBy) && t > 2.0f;
    if (m_RMoment < kRagMomentCount ? t >= kRagMoments[m_RMoment] : (rest && m_RMoment == kRagMomentCount)) {
        m_RFreeze = true;
        m_RFrame = 0;
        return;
    }
    if (m_RMoment <= kRagMomentCount) return; // the rest moment is still to come
    // The verdict.
    const std::string nm = rc.Name;
    Check(n->Ragdoll != nullptr, nm + ": a ragdoll took the body");
    if (n->Ragdoll) {
        std::printf("[NpcTest] ragdoll %s: self %.1f cm, ground %.1f cm (%s), joints %.1f deg past range (%s), %s after %.2f s (grounded at %.2f s)\n",
                    rc.Name, m_RCaseSelf * 100.0f, m_RCaseGround * 100.0f, m_RCaseGroundAt.c_str(), m_RCaseJoint, m_RCaseJointAt.c_str(),
                    n->Ragdoll->Asleep() ? "asleep" : "still moving", t, n->Ragdoll->Motor().GroundedAt());
        Check(m_RCaseSelf < 0.03f, nm + ": no part of the body inside another (worst " + std::to_string(m_RCaseSelf * 100.0f) + " cm)");
        Check(m_RCaseGround < 0.06f, nm + ": no part driven into the ground at impact (worst " + std::to_string(m_RCaseGround * 100.0f) + " cm)");
        Check(m_RGroundNow < 0.01f, nm + ": at rest, nothing inside the ground (" + std::to_string(m_RGroundNow * 100.0f) + " cm)");
        Check(m_RCaseJoint < 15.0f, nm + ": no joint bent past its range (worst " + std::to_string(m_RCaseJoint) + " deg over, " + m_RCaseJointAt + ")");
        Check(n->Ragdoll->Asleep(), nm + ": the body comes to rest and sleeps within " + std::to_string((int)kRagRestBy) + " s");
        if (!n->Ragdoll->Asleep()) {
            // What keeps it up: the fastest part.
            float lin = 0.0f, ang = 0.0f;
            PhysicsWorld::RagdollMotion(n->Ragdoll->Id(), &lin, &ang);
            std::printf("[NpcTest] ragdoll %s: still moving: fastest part %.3f m/s %.2f rad/s, settle %.2f, resting %d\n", rc.Name, lin, ang,
                        n->Ragdoll->Motor().Settle(), (int)n->Ragdoll->Motor().ForcedToRest());
        }
    }
    // On to the next.
    ++m_RCase;
    m_RStage = 0;
    m_RAt = now;
    m_RName.clear();
}
