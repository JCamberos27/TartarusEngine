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
#include "Player.h"
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

void NpcTest::SpiderWatchDrive(Player& player, float& wantYaw, float& wantPitch) {
    player.ScriptMove = glm::vec2(0.0f);
    if (!m_WatchPlaced) {
        // 301's living room: by its doorway, or (spiderdeath, whose own camera is there) at the far end looking back
        const glm::vec3 door = m_Scenario == "spiderdeath" ? glm::vec3(2.5f, 0.02f, -9.0f) : glm::vec3(2.5f, 0.02f, -4.3f);
        PhysicsWorld::SetCharacterFootPosition(&door.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = door + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        m_WatchPlaced = true;
    }
    wantYaw = -90.0f;
    wantPitch = 0.0f;
    if (m_HaveWatch) YawPitchAt(player.Cam.Position, m_WatchAt, wantYaw, wantPitch);
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
    // The Scene view beside the spider, a little off its surface, so a change of surface shows side-on; the player's
    // view (SpiderWatchDrive) has the room-wide shot.
    {
        glm::vec3 side = glm::normalize(glm::cross(st.Up, st.Forward) + glm::vec3(1e-4f, 0.0f, 0.0f));
        if (glm::dot(side, glm::vec3(2.5f, 2.0f, -5.4f) - st.Pos) < 0.0f) side = -side; // the work light's side: lit, not a silhouette
        glm::vec3 eye = st.Pos + side * 1.5f + st.Up * 0.6f - st.Forward * 0.3f;
        eye = glm::clamp(eye, glm::vec3(0.45f, 0.15f, -9.2f), glm::vec3(4.6f, 2.5f, -4.15f)); // inside the room
        m_CamPos = eye;
        YawPitchAt(eye, st.Pos + st.Up * 0.15f, m_CamYaw, m_CamPitch);
        m_HaveCam = true;
        m_WatchAt = st.Pos + st.Up * 0.15f;
        m_HaveWatch = true;
    }

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
    Check(leg.WorstGap < 0.35f, "spider: the body stays on the surface (its middle swings out round corners) on the way to '" + leg.Name + "'");
    Check(leg.Frames && leg.SumTip / (float)leg.Frames < 0.02f && leg.WorstTip < 0.2f,
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
    Check(m_SWorstRays <= 26, "spider: at most 26 surface rays a frame (worst " + std::to_string(m_SWorstRays) + ")");
    std::printf("[NpcTest] spider: %d steps, worst %d rays a frame, both groups lifted on %d frames\n", st.Steps, m_SWorstRays,
                st.BothLifted);
    SpiderJank("spider");
    m_SDone = m_Done = true;
}

void NpcTest::SpiderJank(const char* test) {
    if (!Spiders) return;
    SpiderSystem::Stats::JankStats j;
    const int n = Spiders->SumJank(j);
    const SpiderSystem::Timing& tm = Spiders->Times();
    const double perSpider = j.CostFrames ? j.CostUs / (double)j.CostFrames : 0.0;
    const double perFrame = tm.Frames ? tm.SumUs / (double)tm.Frames : 0.0;
    std::printf("[NpcTest] %s jank (%d spiders): pops %d (worst %.3f m at %.2f %.2f %.2f, mode %d, moves 0x%x), snaps %d (worst %.1f deg "
                "at %.2f %.2f %.2f, mode %d, moves 0x%x)\n", test, n, j.Pops, j.WorstPop, j.WorstPopAt.x, j.WorstPopAt.y, j.WorstPopAt.z,
                j.WorstPopMode, j.WorstPopMoves, j.Snaps, j.WorstSnap, j.WorstSnapAt.x, j.WorstSnapAt.y, j.WorstSnapAt.z, j.WorstSnapMode,
                j.WorstSnapMoves);
    std::printf("[NpcTest] %s jank: stutters %d, refused %d | feet in air %d, inside %d | slide %.2f cm, leg pops %d, skating %d, "
                "overreach %d | flips %d, spins %d, dither %.2f s | air landings %d, floating %d, stop slides %d\n", test, j.Stutters,
                j.Refused, j.FeetInAir, j.FeetInside, j.WorstSlide * 100.0f, j.LegPops, j.Skating, j.Overreach, j.Flips, j.Spins,
                j.Dither, j.AirLandings, j.Floating, j.StopSlides);
    std::printf("[NpcTest] %s cost: %.1f us a spider a frame (worst %.1f), system %.1f us a frame (worst %.1f), door blocks worst "
                "%.1f us, graph bake %.1f ms, %d route searches (mean %.1f us, worst %.1f us, %d nodes)\n", test, perSpider,
                j.WorstCostUs, perFrame, tm.WorstUs, tm.WorstDoorBlocksUs, tm.NavUs / 1000.0, tm.Searches,
                tm.Searches ? tm.SearchUs / tm.Searches : 0.0, tm.WorstSearchUs, tm.WorstExpanded);
    const std::string t = std::string(test) + ": ";
    // Budgets (measured in all five: door blocks 250-410 us at worst, a spider 59-85 us a frame on average).
    Check(tm.WorstDoorBlocksUs < 500.0, t + "door blocks cost under 0.5 ms in any frame");
    Check(perSpider < 100.0, t + "a spider costs under 100 us a frame on average");
    Check(j.Pops == 0, t + "no position pops (never further in a frame than its speed takes it)");
    Check(j.Snaps == 0, t + "no rotation snaps (its up rolls over edges at its rate)");
    Check(j.Stutters == 0 && j.Refused == 0, t + "no stutter (no stop-starts, no refused steps)");
    Check(j.FeetInAir == 0 && j.FeetInside == 0, t + "standing feet on a surface, never in the air or inside one");
    Check(j.WorstSlide < 0.002f && j.LegPops == 0 && j.Skating == 0 && j.Overreach == 0,
          t + "legs keep contact: planted feet under 2 mm of slide, no leg pops, gait in step with the body, no overreach");
    Check(j.Flips == 0 && j.Spins == 0 && j.Dither < 0.5f, t + "no flip-flopping routes, spins or dithering");
    Check(j.AirLandings == 0 && j.Floating == 0 && j.StopSlides == 0, t + "no mid-air landings, floating or sliding after a stop");
}
