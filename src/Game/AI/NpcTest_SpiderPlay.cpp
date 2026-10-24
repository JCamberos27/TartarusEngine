// --npc-test spiderplay: hunters as editor Play has them. Three are left alone in Apartment 301's living room while the
// player stands still out in the hallway, where Play puts them; unaware, they go up to the ceiling. The player fires down
// the hallway and waits there; then comes in through 301's door, stands under the ceiling and fires at them. Measured:
// they don't notice a quiet player through the walls; they perch; the shot brings them out to the player in the hallway;
// in the room they come down off the ceiling and attack; none ever crawls the void under the floor; and the surface
// graph they move on - maybe read from the disk cache - is the one the builder makes from the level now
// (NpcTest_SpiderPlay.cpp; Creatures/SpiderAI.cpp, Creatures/SpiderNav.cpp).
#include "NpcTest.h"
#include "Combat/PlayerVitals.h"
#include "Components.h"
#include "Creatures/SpiderSystem.h"
#include "FirstPersonPresentation.h"
#include "GameModuleAPI.h" // RaycastHit, QueryFilter
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr float kDt = 1.0f / 60.0f;
const glm::vec3 kHall(-9.6f, 0.02f, 0.0f);  // the playtest scene's Player Spawn, out in the hallway
const glm::vec3 kDoor(2.5f, 0.02f, -4.3f);  // 301's living room, just inside the door
constexpr float kUnderZ = -6.4f;            // where the player stops: the middle of the room, under the hunters
const glm::vec3 kHunters[3] = {{1.3f, 1.0f, -8.7f}, {3.9f, 1.0f, -8.6f}, {4.0f, 1.0f, -5.2f}}; // as the playtest has them
constexpr float kAloneFor = 14.0f; // left alone this long before the player fires down the hallway
constexpr float kComeIn = 36.0f;   // then waits there this long before coming into the room
constexpr float kEnd = 66.0f;

// The third floor's outside wall faces (x and z) and this storey's floor and ceiling: past a wall face is the building's
// outside (the facade, the roof), above 2.75 the void over 301's ceiling (2.7), under -0.15 the slab's underside. The
// windows are closed to spiders (Creature Barrier), so no spider ever gets to any of it.
bool OutsideTheStorey(const glm::vec3& p) {
    constexpr float kWallX = 10.75f, kWallZ = 9.825f, kSkin = 0.03f;
    return std::fabs(p.x) > kWallX - kSkin || std::fabs(p.z) > kWallZ - kSkin || p.y > 2.75f || p.y < -0.15f;
}

bool Perched(const SpiderSystem::Stats& st) { return st.Attached && st.Pos.y > 2.0f && st.Up.y < -0.8f; }
} // namespace

void NpcTest::SpiderPlayDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch) {
    player.ScriptMove = glm::vec2(0.0f);
    wantPitch = 0.0f;
    auto place = [&](const glm::vec3& at) {
        PhysicsWorld::SetCharacterFootPosition(&at.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = at + glm::vec3(0.0f, player.EyeHeight, 0.0f);
    };
    if (m_PClock < kComeIn) {
        if (!m_PPlaced) {
            place(kHall);
            m_PPlaced = true;
        }
        wantYaw = 180.0f; // down the hallway, away from 301; still and quiet - then a burst that way
        m_Firing = m_PClock >= kAloneFor && m_PClock < kAloneFor + 1.0f;
        if (m_Firing && m_PShotAt < 0.0f) {
            m_PShotAt = m_PClock;
            m_PAmmoStart = weapon.Ammo();
        }
        if (m_PShotAt >= 0.0f) m_PAmmoSpent = std::max(m_PAmmoSpent, m_PAmmoStart - weapon.Ammo());
        return;
    }
    if (!m_PEntered) {
        place(kDoor);
        m_PEntered = true;
    }
    if (player.Cam.Position.z > kUnderZ) { // walking in
        wantYaw = -90.0f;
        player.ScriptMove = glm::vec2(0.0f, 1.0f);
        return;
    }
    if (!m_PHaveAim) return;
    // Under them: aim at the nearest hunter and fire bursts while settled on it.
    const glm::vec3 d = glm::normalize(m_PAim - player.Cam.Position);
    wantYaw = glm::degrees(std::atan2(d.z, d.x));
    wantPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
    const float err = std::abs(std::remainder(wantYaw - player.Cam.Yaw, 360.0f)) + std::abs(wantPitch - player.Cam.Pitch);
    m_FireHold += dt;
    m_Firing = err < 4.0f && std::fmod(m_FireHold, 1.0f) < 0.45f;
    if (m_Firing && m_PRoomShotAt < 0.0f) {
        m_PRoomShotAt = m_PClock;
        m_PRoomAmmo = weapon.Ammo();
    }
    if (m_PRoomShotAt >= 0.0f) m_PRoomSpent = std::max(m_PRoomSpent, m_PRoomAmmo - weapon.Ammo());
    m_Reload = weapon.Ammo() == 0;
}

void NpcTest::SpiderPlay(World& world) {
    if (m_PDone) return;
    m_PClock += kDt;
    if (m_PStage == 0) {
        m_PStage = 1;
        const entt::entity lamp = world.CreateEmptyEntity(glm::vec3(2.5f, 2.0f, -5.6f), glm::vec3(0.0f), glm::vec3(1.0f), "Spider Test Light");
        auto& light = world.Registry.emplace<LightComponent>(lamp);
        light.Intensity = 6.0f;
        light.Range = 9.0f;
        for (const glm::vec3& p : kHunters) {
            RaycastHit h;
            QueryFilter f;
            f.HitTriggers = 0;
            const glm::vec3 down(0.0f, -1.0f, 0.0f);
            if (SpawnHunter && PhysicsWorld::RaycastSolid(&p.x, &down.x, 3.0f, f, h) && h.Hit)
                m_PRoots.push_back(SpawnHunter(glm::vec3(h.Point[0], h.Point[1], h.Point[2]), 0.0f));
        }
        Check(m_PRoots.size() == 3, "spiderplay: three hunters in 301's living room, as the playtest scene has them");
        return;
    }
    if (m_PStage == 1 && Spiders && !Spiders->Nav().Empty()) { // the graph is up once the first hunter has thought
        m_PStage = 2;
        m_PNavSame = Spiders->NavMatchesFreshBuild(m_PNavWhy);
        m_PBarriers = (int)Spiders->Barriers().size();
        // Everything the graph links to 301's floor: none of it may be outside this storey of the building.
        const Spider::SurfaceGraph& g = Spiders->Nav();
        const glm::vec3 floorUp(0.0f, 1.0f, 0.0f);
        const int start = g.Nearest(glm::vec3(2.5f, 0.0f, -6.4f), 1.0f, &floorUp);
        std::vector<char> seen((size_t)g.NodeCount(), 0);
        std::vector<int> open;
        if (start >= 0) {
            seen[(size_t)start] = 1;
            open.push_back(start);
        }
        while (!open.empty()) {
            const int i = open.back();
            open.pop_back();
            ++m_PReachable;
            const glm::vec3& p = g.Pos(i);
            if (OutsideTheStorey(p)) {
                if (m_PVoid++ < 6)
                    std::printf("[NpcTest] spiderplay: linked to the room, outside the storey: node %d at %.2f %.2f %.2f, normal %.2f %.2f %.2f\n", i,
                                p.x, p.y, p.z, g.Normal(i).x, g.Normal(i).y, g.Normal(i).z);
            }
            for (int k = g.FirstLink(i); k < g.FirstLink(i + 1); ++k)
                if (!seen[(size_t)g.Neighbour(k)]) {
                    seen[(size_t)g.Neighbour(k)] = 1;
                    open.push_back(g.Neighbour(k));
                }
        }
    }

    float nearest = 1e9f;
    bool perchedAll = true;
    int attacks = 0;
    for (size_t k = 0; k < m_PRoots.size(); ++k) {
        SpiderSystem::Stats st;
        const entt::entity e = (entt::entity)m_PRoots[k];
        if (!Spiders || !world.Registry.valid(e) || !Spiders->Inspect(e, st) || !st.Ready) {
            perchedAll = false;
            continue;
        }
        if (m_PClock < kAloneFor && st.Aware) ++m_PAwareEarly;
        perchedAll = perchedAll && Perched(st);
        // The floor here is at 0: a body this far under it is on the slab's underside, in the void below the room.
        if (OutsideTheStorey(st.Pos) && k < 3 && !m_POutSaid[k]) {
            m_POutSaid[k] = true;
            std::printf("[NpcTest] spiderplay: hunter %zu outside the storey at %.1f s: %.2f %.2f %.2f up %.2f %.2f %.2f, mode %d, "
                        "goal %.2f %.2f %.2f\n", k, m_PClock, st.Pos.x, st.Pos.y, st.Pos.z, st.Up.x, st.Up.y, st.Up.z, (int)st.AiMode,
                        st.Goal.x, st.Goal.y, st.Goal.z);
        }
        if (st.Pos.y < -0.15f) ++m_PUnder;
        if (std::fabs(st.Pos.x) > 10.72f || std::fabs(st.Pos.z) > 9.795f) ++m_POutside;
        if (st.Pos.y > 2.75f) ++m_PAbove; // 301's ceiling is at 2.7: higher is on top of it, in the void below the floor above
        attacks += st.Drops + st.Pounces + st.Bites;
        if (k < 3 && m_PShotAt >= 0.0f && m_PClock < kComeIn && glm::length(st.Pos - kHall) < 4.0f) m_PReached[k] = true;
        if (k < 3 && m_PRoomShotAt >= 0.0f && st.Pos.y < 1.0f) m_PDown[k] = true;
        const float d = glm::length(st.Pos - kDoor);
        if (d < nearest) {
            nearest = d;
            m_PAim = st.Pos + st.Up * 0.15f;
            m_PHaveAim = true;
        }
    }
    if (m_PClock >= kAloneFor - kDt && m_PClock < kAloneFor) m_PPerchedAll = perchedAll && !m_PRoots.empty();
    if (attacks > 0 && m_PFirstAttackAt < 0.0f) m_PFirstAttackAt = m_PClock;
    if (m_PClock >= kComeIn && m_PAttacksBefore < 0) m_PAttacksBefore = attacks;
    if (m_PClock >= kComeIn && attacks > m_PAttacksBefore && m_PRoomAttackAt < 0.0f) m_PRoomAttackAt = m_PClock;
    if (m_PClock < kEnd) return;

    std::printf("[NpcTest] spiderplay: graph %s%s; aware frames while alone %d; perched when shot at %d; hallway shot %.1f s "
                "(%d rounds), first attack %.1f s, reached the player %d %d %d; room shot %.1f s (%d rounds), attack there %.1f s, "
                "down %d %d %d; %d frames under the floor\n",
                m_PNavSame ? "matches a fresh build" : "differs from a fresh build: ", m_PNavWhy.c_str(), m_PAwareEarly,
                (int)m_PPerchedAll, m_PShotAt, m_PAmmoSpent, m_PFirstAttackAt, (int)m_PReached[0], (int)m_PReached[1],
                (int)m_PReached[2], m_PRoomShotAt, m_PRoomSpent, m_PRoomAttackAt, (int)m_PDown[0], (int)m_PDown[1], (int)m_PDown[2],
                m_PUnder);
    Check(m_PStage == 2 && m_PNavSame, "spiderplay: the surface graph in use (cached or not) is what the builder makes from the level now");
    Check(m_PAwareEarly == 0, "spiderplay: left alone, the hunters don't notice a quiet player out in the hallway");
    Check(m_PPerchedAll, "spiderplay: left alone, every hunter goes up and perches on the ceiling");
    Check(m_PAmmoSpent > 0 && m_PRoomSpent > 0, "spiderplay: the player fires, in the hallway and then from under the ceiling");
    Check(m_PFirstAttackAt >= 0.0f && m_PFirstAttackAt < kComeIn, "spiderplay: woken by the shot, a hunter comes out and attacks the player in the hallway");
    Check(m_PReached[0] && m_PReached[1] && m_PReached[2], "spiderplay: every hunter finds its way out to the player in the hallway");
    Check(m_PRoomAttackAt >= 0.0f && m_PRoomAttackAt - kComeIn < 15.0f, "spiderplay: in the room, a hunter attacks within 15 s");
    Check(m_PDown[0] && m_PDown[1] && m_PDown[2], "spiderplay: in the room, every hunter comes down off the ceiling");
    std::printf("[NpcTest] spiderplay: %d creature barriers; %d of %d graph nodes linked to 301's floor, %d of them outside the "
                "storey; %d frames past an outside wall\n",
                m_PBarriers, m_PReachable, Spiders ? Spiders->Nav().NodeCount() : 0, m_PVoid, m_POutside);
    Check(m_PBarriers >= 20, "spiderplay: the hallway's windows are closed to spiders (a Creature Barrier on each)");
    Check(m_PReachable > 1000 && m_PVoid == 0, "spiderplay: no route from 301's floor leads out of the building or off this storey");
    Check(m_POutside == 0, "spiderplay: no hunter ever gets out through a window");
    Check(m_PUnder == 0, "spiderplay: no hunter ever crawls the void under the floor");
    Check(m_PAbove == 0, "spiderplay: no hunter ever gets above the ceiling");
    m_PDone = m_Done = true;
}
