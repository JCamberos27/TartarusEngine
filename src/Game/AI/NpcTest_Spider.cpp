// --npc-test spider: one spider sent round Apartment 301's living room - across the floor, up the right-hand wall, over
// the ceiling, down the left-hand wall and back over the floor at a run - and measured on every leg of the route: there
// in time, never let go, its body on the surface, its planted feet still, its legs' IK reaching the feet the gait put
// down, never both halves of the gait lifted at once, and the surface rays it cost. Shots of each arrival and of each
// change of surface (NpcTest_Spider.cpp; Creatures/SpiderSystem.h).
#include "NpcTest.h"
#include "Components.h"
#include "Creatures/SpiderSystem.h"
#include "GameModuleAPI.h" // RaycastHit, QueryFilter
#include "PhysicsWorld.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr float kDt = 1.0f / 60.0f;  // the harness steps the game at 60 Hz
constexpr float kLegBudget = 20.0f;  // seconds each leg of the route may take
constexpr int kShotDelay = 12;       // frames from an event to its shot (the Scene view settles on the spider)
const glm::vec3 kEye(2.5f, 1.4f, -4.25f); // the living room's entry end, looking in

bool Cast(const glm::vec3& from, const glm::vec3& dir, glm::vec3& point) {
    RaycastHit h;
    QueryFilter f;
    f.HitTriggers = 0;
    if (!PhysicsWorld::RaycastSolid(&from.x, &dir.x, 6.0f, f, h) || !h.Hit) return false;
    point = glm::vec3(h.Point[0], h.Point[1], h.Point[2]);
    return true;
}

void YawPitchAt(const glm::vec3& from, const glm::vec3& to, float& yaw, float& pitch) {
    const glm::vec3 d = glm::normalize(to - from);
    yaw = glm::degrees(std::atan2(d.z, d.x));
    pitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
}
} // namespace

void NpcTest::SpiderSetUp(World& world) {
    // 301 is dark at night: a work light over the room so the shots show the spider.
    const entt::entity lamp = world.CreateEmptyEntity(glm::vec3(2.5f, 2.0f, -5.4f), glm::vec3(0.0f), glm::vec3(1.0f), "Spider Test Light");
    auto& light = world.Registry.emplace<LightComponent>(lamp);
    light.Intensity = 6.0f;
    light.Range = 9.0f;
    struct Spot { const char* Name; glm::vec3 From, Dir; bool Run; };
    const Spot spots[] = {
        {"floor", {2.5f, 1.0f, -6.6f}, {0.0f, -1.0f, 0.0f}, false},
        {"right wall", {2.5f, 1.3f, -6.6f}, {1.0f, 0.0f, 0.0f}, false},
        {"ceiling", {3.6f, 1.3f, -6.6f}, {0.0f, 1.0f, 0.0f}, false},
        {"ceiling across", {2.1f, 1.3f, -7.6f}, {0.0f, 1.0f, 0.0f}, false}, // clear of the left wall's corner (a body there leans into its climb)
        {"left wall", {2.5f, 1.3f, -7.6f}, {-1.0f, 0.0f, 0.0f}, false},
        {"floor at a run", {2.5f, 1.0f, -5.3f}, {0.0f, -1.0f, 0.0f}, true},
        // Two surfaces away: only the surface graph's route (up the nearest wall, down the nearest wall) gets there.
        {"ceiling by route", {3.4f, 1.3f, -8.6f}, {0.0f, 1.0f, 0.0f}, false},
        {"floor by route", {1.9f, 1.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, true},
    };
    for (const Spot& s : spots) {
        SpiderLeg leg;
        leg.Name = s.Name;
        leg.Run = s.Run;
        if (!Cast(s.From, s.Dir, leg.Goal)) {
            Check(false, std::string("the route's '") + s.Name + "' spot is on a surface");
            continue;
        }
        m_SLegs.push_back(leg);
    }
    glm::vec3 start;
    const bool floor = Cast(glm::vec3(2.5f, 1.0f, -5.0f), glm::vec3(0.0f, -1.0f, 0.0f), start);
    Check(floor, "301's living-room floor is under the start");
    m_SRoot = floor && SpawnSpider ? SpawnSpider(start, 0.0f) : 0xFFFFFFFFu;
    Check(m_SRoot != 0xFFFFFFFFu, "a spider is built from Spider.json");
    std::printf("[NpcTest] spider: %zu legs of route, start %.2f %.2f %.2f\n", m_SLegs.size(), start.x, start.y, start.z);
}

void NpcTest::SpiderRoute(World& world) {
    if (m_SDone) return;
    if (!m_SReady) {
        m_SReady = true;
        SpiderSetUp(world);
    }
    m_SClock += kDt;
    const entt::entity root = (entt::entity)m_SRoot;
    SpiderSystem::Stats st;
    if (!Spiders || !world.Registry.valid(root) || !Spiders->Inspect(root, st) || !st.Ready) {
        if (m_SClock > 5.0f) {
            Check(false, "the spider comes alive in Play (its model, rig and system)");
            m_SDone = m_Done = true;
        }
        return;
    }
    m_SWorstRays = std::max(m_SWorstRays, st.Rays);

    // The view: from the room's entry end, on the spider.
    m_CamPos = kEye;
    YawPitchAt(kEye, st.Pos + st.Up * 0.15f, m_CamYaw, m_CamPitch);
    m_HaveCam = true;

    // A little time on the start spot first: the legs settle.
    if (m_SSettle < 1.0f) {
        m_SSettle += kDt;
        if (m_SSettle >= 1.0f && m_SLeg < (int)m_SLegs.size()) {
            auto& sc = world.Registry.get<SpiderComponent>(root);
            sc.Goal = m_SLegs[0].Goal;
            sc.Run = m_SLegs[0].Run;
            sc.HasGoal = true;
            m_SLegs[0].StepsAtStart = st.Steps;
        }
        return;
    }
    if (m_SLeg >= (int)m_SLegs.size()) return;
    SpiderLeg& leg = m_SLegs[(size_t)m_SLeg];
    leg.Clock += kDt;
    ++leg.Frames;
    if (!st.Attached) leg.Fell = true;
    else if (st.SurfaceGap >= 0.0f) leg.WorstGap = std::max(leg.WorstGap, std::fabs(st.SurfaceGap));
    leg.WorstTip = std::max(leg.WorstTip, st.TipError);
    leg.SumTip += st.TipError;
    leg.WorstSlide = std::max(leg.WorstSlide, st.PlantedSlide);

    // A shot as it tips from one surface onto the next, and one at each arrival.
    char name[96];
    const bool tilted = std::fabs(st.Up.y) > 0.3f && std::fabs(st.Up.y) < 0.8f;
    if (tilted && !m_SShotTilt) {
        m_SShotTilt = true;
        std::snprintf(name, sizeof name, "spider_%d_%s_edge", m_SLeg + 1, leg.Name.c_str());
        m_Shot = name;
    }
    const bool timedOut = leg.Clock > kLegBudget;
    if (!st.Arrived && !timedOut) return;

    leg.Arrived = st.Arrived;
    const float steps = (float)(st.Steps - leg.StepsAtStart);
    std::printf("[NpcTest] spider leg %d '%s': %s in %.1f s, up %.2f %.2f %.2f, worst gap %.1f cm, IK miss mean %.1f / worst %.1f cm, "
                "planted slide %.2f mm, %d steps, %s\n",
                m_SLeg + 1, leg.Name.c_str(), leg.Arrived ? "there" : "NOT there", leg.Clock, st.Up.x, st.Up.y, st.Up.z,
                leg.WorstGap * 100.0f, leg.Frames ? leg.SumTip / (float)leg.Frames * 100.0f : 0.0f, leg.WorstTip * 100.0f,
                leg.WorstSlide * 1000.0f, (int)steps, leg.Fell ? "FELL" : "held on");
    std::printf("[NpcTest]   route %d nodes (%d planned so far)\n", st.RouteNodes, st.Replans);
    Check(leg.Arrived, "spider: '" + leg.Name + "' reached in time");
    if (leg.Name.find("by route") != std::string::npos)
        Check(st.RouteNodes > 0, "spider: '" + leg.Name + "' went by the surface graph's route");
    Check(!leg.Fell, "spider: never let go on the way to '" + leg.Name + "'");
    Check(leg.WorstGap < 0.04f, "spider: the body stays on the surface on the way to '" + leg.Name + "'");
    Check(leg.Frames && leg.SumTip / (float)leg.Frames < 0.02f && leg.WorstTip < 0.08f,
          "spider: the legs reach their feet on the way to '" + leg.Name + "'");
    Check(leg.WorstSlide < 0.002f, "spider: planted feet stay put on the way to '" + leg.Name + "'");
    std::printf("[NpcTest]   surface normal %.2f %.2f %.2f, legs' %.2f %.2f %.2f\n", st.SurfaceNormal.x, st.SurfaceNormal.y, st.SurfaceNormal.z,
                st.LegNormal.x, st.LegNormal.y, st.LegNormal.z);
    if (leg.Name.find("ceiling") != std::string::npos) Check(st.Up.y < -0.9f, "spider: upside down on the '" + leg.Name + "'");
    if (leg.Name.find("wall") != std::string::npos) Check(std::fabs(st.Up.x) > 0.9f, "spider: side-on to the '" + leg.Name + "'");
    std::snprintf(name, sizeof name, "spider_%d_%s", m_SLeg + 1, leg.Name.c_str());
    m_Shot = name;

    m_SShotTilt = false;
    if (++m_SLeg < (int)m_SLegs.size()) {
        SpiderLeg& next = m_SLegs[(size_t)m_SLeg];
        auto& sc = world.Registry.get<SpiderComponent>(root);
        sc.Goal = next.Goal;
        sc.Run = next.Run;
        sc.HasGoal = true;
        next.StepsAtStart = st.Steps;
        return;
    }
    Check(st.BothLifted == 0, "spider: four legs always down (never both gait groups lifted; " + std::to_string(st.BothLifted) +
                              " frames)");
    Check(st.Steps >= 40, "spider: it walked on its legs (" + std::to_string(st.Steps) + " steps)");
    Check(m_SWorstRays <= 20, "spider: at most 20 surface rays a frame (worst " + std::to_string(m_SWorstRays) + ")");
    std::printf("[NpcTest] spider: %d steps, worst %d rays a frame, both groups lifted on %d frames\n", st.Steps, m_SWorstRays,
                st.BothLifted);
    m_SDone = m_Done = true;
}
