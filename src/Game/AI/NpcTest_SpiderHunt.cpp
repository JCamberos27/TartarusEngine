// --npc-test spiderhunt: hunters (spiders with their own AI) let loose on the player, who stands in the middle of
// Apartment 301's living room, can't die, and doesn't fight back. First one spider (a dropper: it waits overhead and
// drops on the player), then three more. Measured: it notices the player, gets above them and drops; the chargers
// pounce and bite; the bites and pounces land on the player (damage, a shove); never more than two attack at once; and
// no spider ends up through the floor or lost in the air. Shots of the first drop, pounce and bite
// (NpcTest_SpiderHunt.cpp; Creatures/SpiderAI.cpp).
#include "NpcTest.h"
#include "Combat/PlayerVitals.h"
#include "Components.h"
#include "Creatures/SpiderSystem.h"
#include "GameModuleAPI.h" // RaycastHit, QueryFilter
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr float kDt = 1.0f / 60.0f;
const glm::vec3 kStand(2.5f, 0.02f, -6.4f);      // the player: the living room's middle
const glm::vec3 kEye(2.5f, 1.6f, -4.25f);        // the Scene view: the room's entry end
const glm::vec3 kFirst(3.9f, 1.0f, -8.8f);       // the first hunter: the far corner
const glm::vec3 kMore[3] = {{1.2f, 1.0f, -8.9f}, {4.1f, 1.0f, -4.6f}, {1.0f, 1.0f, -4.5f}};
constexpr float kMoreAt = 22.0f;                 // when the other three come
constexpr float kEnd = 60.0f;

bool FloorUnder(const glm::vec3& from, glm::vec3& out) {
    RaycastHit h;
    QueryFilter f;
    f.HitTriggers = 0;
    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    if (!PhysicsWorld::RaycastSolid(&from.x, &down.x, 3.0f, f, h) || !h.Hit) return false;
    out = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
    return true;
}
} // namespace

void NpcTest::SpiderHuntDrive(Player& player, PlayerVitals& vitals, float& wantYaw, float& wantPitch) {
    vitals.GodMode = true; // hits register (damage events, the shove) but the run goes on
    player.ScriptMove = glm::vec2(0.0f);
    if (!m_HPlaced) {
        PhysicsWorld::SetCharacterFootPosition(&kStand.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = kStand + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        m_HPlaced = true;
    }
    wantYaw = -90.0f; // into the room, toward the far wall
    wantPitch = 0.0f;
}

void NpcTest::SpiderHunt(World& world) {
    if (m_HDone) return;
    m_HClock += kDt;
    if (m_HStage == 0) {
        m_HStage = 1;
        const entt::entity lamp = world.CreateEmptyEntity(glm::vec3(2.5f, 2.0f, -5.6f), glm::vec3(0.0f), glm::vec3(1.0f), "Spider Test Light");
        auto& light = world.Registry.emplace<LightComponent>(lamp);
        light.Intensity = 6.0f;
        light.Range = 9.0f;
        glm::vec3 at;
        const bool ok = FloorUnder(kFirst, at) && SpawnHunter;
        if (ok) m_HRoots.push_back(SpawnHunter(at, 135.0f));
        Check(ok && m_HRoots.back() != 0xFFFFFFFFu, "spiderhunt: a hunter is built from Spider.json");
        return;
    }
    if (m_HStage == 1 && m_HClock >= kMoreAt) {
        m_HStage = 2;
        for (const glm::vec3& p : kMore) {
            glm::vec3 at;
            if (FloorUnder(p, at) && SpawnHunter) m_HRoots.push_back(SpawnHunter(at, 0.0f));
        }
        std::printf("[NpcTest] spiderhunt: %zu hunters now\n", m_HRoots.size());
    }

    // Every frame: who is attacking, nobody through the floor or lost in the air.
    int attackers = 0;
    SpiderSystem::Stats first;
    bool haveFirst = false;
    int drops = 0, pounces = 0, bites = 0, leaps = 0;
    bool anyAware = false;
    for (size_t k = 0; k < m_HRoots.size(); ++k) {
        SpiderSystem::Stats st;
        const entt::entity e = (entt::entity)m_HRoots[k];
        if (!Spiders || !world.Registry.valid(e) || !Spiders->Inspect(e, st) || !st.Ready) continue;
        if (k == 0) {
            first = st;
            haveFirst = true;
        }
        attackers += st.HasToken ? 1 : 0;
        drops += st.Drops;
        pounces += st.Pounces;
        bites += st.Bites;
        leaps += st.Leaps;
        anyAware = anyAware || st.Aware;
        if (st.Pos.y < -0.5f) ++m_HFellThrough;
        // The shots: the first drop, pounce and bite to land, framed on that spider.
        const char* shot = nullptr;
        if (st.Drops > 0 && !m_HDropShot) { m_HDropShot = true; shot = "spiderhunt_drop"; }
        else if (st.Pounces > 0 && !m_HPounceShot) { m_HPounceShot = true; shot = "spiderhunt_pounce"; }
        else if (st.Bites > 0 && !m_HBiteShot) { m_HBiteShot = true; shot = "spiderhunt_bite"; }
        if (shot) m_Shot = shot;
    }
    m_HMaxAttackers = std::max(m_HMaxAttackers, attackers);
    if (haveFirst) {
        m_CamPos = kEye;
        const glm::vec3 d = glm::normalize(first.Pos + first.Up * 0.2f - kEye);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        m_HaveCam = true;
    }
    if (m_HClock < kEnd) return;

    std::printf("[NpcTest] spiderhunt: %zu hunters, drops %d, pounces %d, bites %d (leaps / drops tried %d), at most %d attacking "
                "at once, %d frames under the floor\n",
                m_HRoots.size(), drops, pounces, bites, leaps, m_HMaxAttackers, m_HFellThrough);
    Check(m_HRoots.size() == 4, "spiderhunt: four hunters built");
    Check(anyAware, "spiderhunt: the hunters notice the player");
    Check(drops >= 1, "spiderhunt: a hunter drops on the player from above");
    Check(pounces >= 1, "spiderhunt: a hunter pounces on the player");
    Check(bites >= 2, "spiderhunt: the hunters bite the player");
    Check(m_HMaxAttackers <= 2, "spiderhunt: never more than two attack at once");
    Check(m_HFellThrough == 0, "spiderhunt: no spider through the floor");
    std::printf("[NpcTest] spiderhunt: %d spider sounds started, %d asked for with nothing to play\n", SpiderSystem::SoundsStarted(),
                SpiderSystem::SoundsMissing());
    Check(SpiderSystem::SoundsMissing() == 0, "spiderhunt: every spider sound it asks for has takes in the manifest");
    Check(SpiderSystem::SoundsStarted() > 0, "spiderhunt: the spiders are heard (taps, hiss, screech, bite)");
    m_HDone = m_Done = true;
}
