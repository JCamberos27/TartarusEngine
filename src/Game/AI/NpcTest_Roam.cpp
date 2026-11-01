// --npc-test roam: the player walked the scene's rooms - every door the doors scenario finds, a point 0.8 m either side of
// it, visited nearest-first along the navigation mesh (so upstairs by the stairs) with all the doors open - and measured on
// each leg: whether he got there, how long he was stuck (under a quarter of his walking speed), and whether he fell (half a
// metre under the route). The scene's navigability measured, not guessed: NPC_TEST_SCENE picks it (Apartment Hallway by default).
#include "NpcTest.h"
#include "NavMesh.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
constexpr float kAside = 0.8f;   // each waypoint is this far off its door's wall plane
constexpr float kReach = 0.35f;  // a corner (and a waypoint) is reached within this, flat
constexpr float kStuckRatio = 0.25f;
constexpr float kFall = 0.5f;    // under the route's lowest point by this much: he fell
constexpr int kMaxPoints = 40;

float Flat(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }
} // namespace

void NpcTest::RoamSetUp(World& world, const Player& player) {
    (void)world;
    // The walkable mesh of the solid world, the doors in it where they stand open now, for the player's capsule.
    std::vector<float> verts;
    std::vector<int> tris;
    PhysicsWorld::CollectStaticGeometry(verts, tris, true);
    NavBuildSettings ns;
    ns.AgentRadius = player.Size.x * 0.5f;
    ns.AgentHeight = player.Size.y;
    m_RoNav = std::make_shared<NavMesh>();
    std::string err;
    if (!m_RoNav->Build(verts, tris, ns, &err)) std::printf("[NpcTest] roam: navigation mesh failed: %s\n", err.c_str());
    const NavMesh& nav = *m_RoNav;
    std::vector<glm::vec3> pts;
    for (const DoorSpot& d : m_DDoors)
        for (float s : {-1.0f, 1.0f}) {
            glm::vec3 p;
            if (nav.Closest(d.Centre + d.Normal * (s * kAside) + glm::vec3(0.0f, 0.3f, 0.0f), p, glm::vec3(0.3f, 0.8f, 0.3f)))
                pts.push_back(p);
            else
                ++m_RoOffMesh;
        }
    // Nearest-first by path from the spawn: the route a person exploring the place would more or less take.
#pragma warning(suppress : 4996)
    if (const char* own = std::getenv("NPC_TEST_ROAM_POINTS"); own && *own) {
        // "x y z; x y z; ...": walk these instead, from the first (a stairs check, a corridor)
        pts.clear();
        for (const char* c = own; *c;) {
            glm::vec3 p, q;
            int n = 0;
#pragma warning(suppress : 4996)
            if (std::sscanf(c, " %f %f %f%n", &p.x, &p.y, &p.z, &n) != 3) break;
            c += n;
            while (*c == ';' || *c == ' ') ++c;
            if (nav.Closest(p, q, glm::vec3(0.5f, 1.0f, 0.5f))) pts.push_back(q);
            else ++m_RoOffMesh;
        }
        if (!pts.empty()) { // walked in the order given, a leg with no route on the mesh straight at its point
            const glm::vec3 f = pts.front() + glm::vec3(0.0f, 0.05f, 0.0f);
            PhysicsWorld::SetCharacterFootPosition(&f.x);
            m_RoStart = pts.front();
            m_RoPoints.assign(pts.begin() + 1, pts.end());
            std::printf("[NpcTest] roam: %zu given waypoints, %d off the mesh\n", m_RoPoints.size(), m_RoOffMesh);
            return;
        }
    }
    glm::vec3 at;
    PhysicsWorld::GetCharacterFootPosition(&at.x);
    if (!nav.Closest(at, at, glm::vec3(1.0f, 2.0f, 1.0f)) && !pts.empty()) at = pts.front();
    m_RoStart = at;
    while (!pts.empty() && (int)m_RoPoints.size() < kMaxPoints) {
        int best = -1;
        float bestLen = 1e9f;
        for (int i = 0; i < (int)pts.size(); ++i) {
            const float len = nav.PathLength(at, pts[(size_t)i]);
            if (len >= 0.0f && len < bestLen) bestLen = len, best = i;
        }
        if (best < 0) break; // the rest can't be reached from here
        at = pts[(size_t)best];
        m_RoPoints.push_back(at);
        pts.erase(pts.begin() + best);
    }
    m_RoUnreachable = (int)pts.size();
    for (const glm::vec3& p : pts) std::printf("[NpcTest] roam: unreachable waypoint (%.2f %.2f %.2f)\n", p.x, p.y, p.z);
    std::printf("[NpcTest] roam: %zu doors, %zu waypoints on the route, %d unreachable from it, %d off the mesh\n", m_DDoors.size(),
                m_RoPoints.size(), m_RoUnreachable, m_RoOffMesh);
}

void NpcTest::RoamDrive(Player& player, float dt, float& wantYaw, float& wantPitch) {
    player.ScriptMove = glm::vec2(0.0f);
    wantPitch = -3.0f;
    wantYaw = player.Cam.Yaw;
    m_DDt = dt;
    if (!m_RoReady || m_RoLeg >= (int)m_RoPoints.size()) return;
    glm::vec3 feet;
    PhysicsWorld::GetCharacterFootPosition(&feet.x);
    if (m_RoPath.empty()) {
        // The leg's route from where he stands (a failed leg starts him on its waypoint instead).
        bool partial = false;
#pragma warning(suppress : 4996)
        const char* straight = std::getenv("NPC_TEST_ROAM_STRAIGHT"); // straight at each point, no route (a stairs probe)
        if ((straight && *straight == '1') ||
            !m_RoNav->FindPath(m_RoFrom, m_RoPoints[(size_t)m_RoLeg], m_RoPath, &partial) || m_RoPath.empty())
            m_RoPath.assign(1, m_RoPoints[(size_t)m_RoLeg]);
        else if (partial)
            std::printf("[NpcTest] roam leg %d: the mesh's route ends short, at (%.2f %.2f %.2f)\n", m_RoLeg, m_RoPath.back().x,
                        m_RoPath.back().y, m_RoPath.back().z);
        m_RoCorner = 0;
        m_RoLow = m_RoFrom.y;
        float len = 0.0f;
        glm::vec3 prev = m_RoFrom;
        for (const glm::vec3& c : m_RoPath) {
            len += glm::length(c - prev);
            prev = c;
            m_RoLow = std::min(m_RoLow, c.y);
        }
        m_RoBudget = 2.0f * len / std::max(player.MoveSpeed, 0.1f) + 3.0f;
        m_RoClock = m_RoStuck = 0.0f;
    }
    while (m_RoCorner + 1 < (int)m_RoPath.size() && Flat(feet, m_RoPath[(size_t)m_RoCorner]) < kReach) ++m_RoCorner;
    const glm::vec3 to = m_RoPath[(size_t)m_RoCorner] - feet;
    if (glm::length(glm::vec2(to.x, to.z)) > 1e-3f) wantYaw = glm::degrees(std::atan2(to.z, to.x));
    player.Cam.Yaw = wantYaw;
    player.ScriptMove = glm::vec2(0.0f, 1.0f);
}

void NpcTest::Roam(World& world, const Player& player) {
    if (!m_RoReady) {
        if (m_RoWait == 0.0f) DoorsSetUp(world); // the doors as the doors scenario finds them, all opened
        m_RoWait += std::max(m_DDt, 1e-4f);
        if (m_RoWait < 1.0f) return; // the leaves swinging open
        RoamSetUp(world, player);
        if (!m_RoNav->Valid()) {
            Check(false, "roam: a navigation mesh was built");
            m_Done = true;
            return;
        }
        m_RoReady = true;
        PhysicsWorld::GetCharacterFootPosition(&m_RoFrom.x);
        m_RoPrev = m_RoFrom;
        if (m_RoPoints.empty()) {
            Check(false, "roam: the scene has waypoints to walk");
            m_Done = true;
        }
        return;
    }
    if (m_RoLeg >= (int)m_RoPoints.size()) return;
    glm::vec3 feet;
    PhysicsWorld::GetCharacterFootPosition(&feet.x);
    const float dt = m_DDt;
    m_RoClock += dt;
    if (dt > 0.0f && m_RoClock > 0.3f) {
        const float speed = Flat(feet, m_RoPrev) / dt;
        if (speed < kStuckRatio * player.MoveSpeed) {
            const bool first = m_RoStuck < 0.5f;
            m_RoStuck += dt;
            if (first && m_RoStuck >= 0.5f) // where it happened, once per leg
                std::printf("[NpcTest] roam leg %d stuck at (%.2f %.2f %.2f), heading for corner %d (%.2f %.2f %.2f)\n", m_RoLeg,
                            feet.x, feet.y, feet.z, m_RoCorner, m_RoPath[(size_t)m_RoCorner].x, m_RoPath[(size_t)m_RoCorner].y,
                            m_RoPath[(size_t)m_RoCorner].z);
        }
    }
    m_RoPrev = feet;
#pragma warning(suppress : 4996)
    if (const char* t = std::getenv("NPC_TEST_ROAM_TRACE"); t && *t == '1' && std::fmod(m_RoClock, 0.25f) < dt)
        std::printf("[NpcTest]   roam t %.2f feet (%.2f %.2f %.2f)\n", m_RoClock, feet.x, feet.y, feet.z);
    const glm::vec3& goal = m_RoPoints[(size_t)m_RoLeg];
    const bool fell = feet.y < m_RoLow - kFall;
    const bool there = Flat(feet, goal) < kReach && std::abs(feet.y - goal.y) < 0.6f;
    if (!there && !fell && m_RoClock < m_RoBudget) return;
    const bool stairs = std::abs(goal.y - m_RoFrom.y) > 1.2f;
    std::printf("[NpcTest] roam leg %d (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)%s: %s, %.2f s, stuck %.2f s\n", m_RoLeg, m_RoFrom.x,
                m_RoFrom.y, m_RoFrom.z, goal.x, goal.y, goal.z, stairs ? " [stairs]" : "",
                there ? "reached" : fell ? "FELL" : "NOT REACHED", m_RoClock, m_RoStuck);
    ++m_RoTotal;
    if (there) ++m_RoOk;
    if (fell) ++m_RoFalls;
    if (stairs) {
        ++m_RoStairs;
        if (there) ++m_RoStairsOk;
    }
    m_RoStuckSum += m_RoStuck;
    if (m_RoStuck > m_RoWorstStuck) {
        m_RoWorstStuck = m_RoStuck;
        m_RoWorstAt = m_RoLeg;
    }
    if (!there) { // start the next leg from this one's waypoint
        const glm::vec3 p = goal + glm::vec3(0.0f, 0.05f, 0.0f);
        PhysicsWorld::SetCharacterFootPosition(&p.x);
        m_RoFrom = goal;
    } else {
        m_RoFrom = feet;
    }
    m_RoPrev = m_RoFrom;
    m_RoPath.clear();
    if (++m_RoLeg < (int)m_RoPoints.size()) return;
    std::printf("[NpcTest] roam: %d/%d legs reached, stairs %d/%d, falls %d, stuck %.2f s in all (worst %.2f s, leg %d), %d "
                "waypoints unreachable, %d off the mesh\n",
                m_RoOk, m_RoTotal, m_RoStairsOk, m_RoStairs, m_RoFalls, m_RoStuckSum, m_RoWorstStuck, m_RoWorstAt, m_RoUnreachable,
                m_RoOffMesh);
    Check(m_RoOk == m_RoTotal, "roam: every leg reached (" + std::to_string(m_RoOk) + "/" + std::to_string(m_RoTotal) + ")");
    Check(m_RoFalls == 0, "roam: never fell through or off (" + std::to_string(m_RoFalls) + ")");
    Check(m_RoWorstStuck <= 1.0f, "roam: never stuck a second on one leg (worst " + std::to_string(m_RoWorstStuck) + " s)");
    m_Done = true;
}
