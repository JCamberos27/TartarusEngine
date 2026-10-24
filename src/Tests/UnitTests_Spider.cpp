#include "UnitTestSupport.h"

#include "Creatures/SpiderLocomotion.h"
#include "Creatures/SpiderNav.h"
#include "IK.h"

#include <cstdio>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

// A room's inside (its six walls) and solid boxes standing in it: what the spider climbs in these tests.
class BoxWorld : public Spider::SurfaceQuery {
public:
    glm::vec3 RoomMin{-2.0f, 0.0f, -2.0f}, RoomMax{2.0f, 2.4f, 2.0f};
    struct Box { glm::vec3 Min, Max; };
    std::vector<Box> Boxes;

    bool Ray(const glm::vec3& o, const glm::vec3& d, float maxDistance, Spider::SurfaceHit& out) const override {
        float best = maxDistance;
        bool hit = false;
        glm::vec3 n(0.0f);
        for (int a = 0; a < 3; ++a) { // the room's walls, from inside
            if (std::fabs(d[a]) < 1e-9f) continue;
            const float wall = d[a] > 0.0f ? RoomMax[a] : RoomMin[a];
            const float t = (wall - o[a]) / d[a];
            if (t >= 0.0f && t < best) {
                best = t;
                hit = true;
                n = glm::vec3(0.0f);
                n[a] = d[a] > 0.0f ? -1.0f : 1.0f;
            }
        }
        for (const Box& b : Boxes) { // boxes, from outside (slabs)
            float t0 = 0.0f, t1 = best;
            int axis = -1;
            bool miss = false;
            for (int a = 0; a < 3 && !miss; ++a) {
                if (std::fabs(d[a]) < 1e-9f) {
                    miss = o[a] < b.Min[a] || o[a] > b.Max[a];
                    continue;
                }
                float ta = (b.Min[a] - o[a]) / d[a], tb = (b.Max[a] - o[a]) / d[a];
                if (ta > tb) std::swap(ta, tb);
                if (ta > t0) { t0 = ta; axis = a; }
                t1 = std::min(t1, tb);
                miss = t0 > t1;
            }
            if (miss || axis < 0 || t0 >= best) continue;
            best = t0;
            hit = true;
            n = glm::vec3(0.0f);
            n[axis] = d[axis] > 0.0f ? -1.0f : 1.0f;
        }
        if (!hit) return false;
        out.Hit = true;
        out.Distance = best;
        out.Point = o + d * best;
        out.Normal = n;
        return true;
    }
    bool Occupied(const glm::vec3& c, float r) const override {
        for (int a = 0; a < 3; ++a) // past the room's walls
            if (c[a] - r < RoomMin[a] || c[a] + r > RoomMax[a]) return true;
        for (const Box& b : Boxes)
            if (glm::length(glm::clamp(c, b.Min, b.Max) - c) < r) return true;
        return false;
    }
    // The room's and the boxes' faces as triangles (what PhysicsWorld::CollectStaticGeometry gives the game).
    void Triangles(std::vector<float>& verts, std::vector<int>& tris) const {
        auto box = [&](const glm::vec3& lo, const glm::vec3& hi) {
            const int base = (int)verts.size() / 3;
            for (int k = 0; k < 8; ++k) {
                const glm::vec3 p((k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z);
                verts.insert(verts.end(), {p.x, p.y, p.z});
            }
            const int faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
            for (const auto& f : faces) tris.insert(tris.end(), {base + f[0], base + f[1], base + f[2], base + f[0], base + f[2], base + f[3]});
        };
        box(RoomMin, RoomMax);
        for (const Box& b : Boxes) box(b.Min, b.Max);
    }
};

// Follows a route over `graph` to `goal` with the mover, as SpiderSystem does.
struct RouteResult {
    bool Planned = false, Arrived = false, Fell = false;
    glm::vec3 Up{0.0f};
    int Nodes = 0, OffSurface = 0; // route length; nodes on walls / ceilings / other faces than the start's
};
RouteResult FollowRoute(const BoxWorld& w, const Spider::SurfaceGraph& graph, Spider::Mover& m, const glm::vec3& goal, float seconds) {
    RouteResult r;
    std::vector<int> nodes;
    const glm::vec3 up = m.Up();
    const int from = graph.Nearest(m.Position(), 0.8f, &up), to = graph.Nearest(goal, 1.0f);
    r.Planned = from >= 0 && to >= 0 && graph.FindPath(from, to, Spider::PathCosts{}, nodes);
    if (!r.Planned) return r;
    std::vector<glm::vec3> route, normals;
    for (int n : nodes) {
        route.push_back(graph.Pos(n));
        normals.push_back(graph.Normal(n));
        if (glm::dot(graph.Normal(n), up) < 0.95f) ++r.OffSurface;
    }
    r.Nodes = (int)nodes.size();
    Spider::MoverSettings s;
    Spider::MoveInput in;
    in.HasGoal = true;
    int at = 0;
    const float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds && !r.Arrived; t += dt) {
        in.Goal = Spider::RouteAim(route, normals, at, m.Position(), m.Up(), goal);
        m.Step(w, s, in, dt);
        r.Fell = r.Fell || !m.Attached();
        r.Arrived = m.Arrived() && glm::length(m.Position() - goal) < 0.2f;
    }
    r.Up = m.Up();
    return r;
}

// Runs the mover toward `goal` for up to `seconds`; reports the worst gap off the surface while it was attached and
// whether it ever let go.
struct ClimbResult {
    bool Arrived = false, Fell = false;
    float WorstGap = 0.0f, Seconds = 0.0f;
    glm::vec3 Up{0.0f};
};
ClimbResult Climb(const BoxWorld& w, Spider::Mover& m, const glm::vec3& goal, float seconds) {
    ClimbResult r;
    Spider::MoverSettings s;
    Spider::MoveInput in;
    in.HasGoal = true;
    in.Goal = goal;
    const float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < seconds; t += dt) {
        m.Step(w, s, in, dt);
        if (!m.Attached()) r.Fell = true;
        else if (m.SurfaceGap() >= 0.0f) r.WorstGap = std::max(r.WorstGap, std::fabs(m.SurfaceGap()));
        if (m.Arrived()) {
            r.Arrived = true;
            r.Seconds = t;
            break;
        }
    }
    r.Up = m.Up();
    return r;
}

void TestSolveChainReachesAndBendsTowardPole() {
    // Four bones along +X, a metre each, the tip a metre past the last.
    IK::Pose pose(4);
    std::vector<int> parents{-1, 0, 1, 2};
    for (int i = 1; i < 4; ++i) pose[(size_t)i].T = glm::vec3(1.0f, 0.0f, 0.0f);
    std::vector<glm::mat4> g;
    IK::ComputeGlobals(pose, parents, g);
    const int chain[4] = {0, 1, 2, 3};
    const glm::vec3 tip(1.0f, 0.0f, 0.0f), target(2.5f, -1.0f, 0.0f), pole(1.5f, 5.0f, 0.0f);
    CHECK(IK::SolveChain(pose, parents, g, chain, 4, tip, target, pole, 1.0f));
    const glm::vec3 end = glm::vec3(g[3] * glm::vec4(tip, 1.0f));
    CHECK(glm::length(end - target) < 1e-3f);
    for (int i = 0; i < 3; ++i) // the bones keep their lengths
        CHECK(std::fabs(glm::length(IK::Position(g[(size_t)i + 1]) - IK::Position(g[(size_t)i])) - 1.0f) < 1e-4f);
    // An arch toward the pole: the inner joints stand above the root-to-target line.
    for (int i = 1; i < 4; ++i) {
        const glm::vec3 p = IK::Position(g[(size_t)i]);
        const float lineY = -1.0f * (p.x / 2.5f);
        CHECK(p.y > lineY);
    }
    // Out of reach: the chain points straight at it, at full length.
    const glm::vec3 far(10.0f, 0.0f, 3.0f);
    CHECK(IK::SolveChain(pose, parents, g, chain, 4, tip, far, pole, 1.0f));
    const glm::vec3 end2 = glm::vec3(g[3] * glm::vec4(tip, 1.0f));
    CHECK(std::fabs(glm::length(end2) - 4.0f) < 1e-3f);
    CHECK(glm::dot(glm::normalize(end2), glm::normalize(far)) > 0.9999f);
    // A broken chain (not parent to child) is refused.
    const int broken[2] = {0, 2};
    CHECK(!IK::SolveChain(pose, parents, g, broken, 2, tip, target, pole, 1.0f));
}

void TestSpiderClimbsFloorWallCeiling() {
    BoxWorld w;
    Spider::Mover m;
    m.Reset(glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)); // on the floor, facing -Z
    // Surface to neighbouring surface (a goal two surfaces away is the surface graph's to route): up the +X wall, onto
    // the ceiling, down the -X wall, back onto the floor.
    const ClimbResult wall = Climb(w, m, glm::vec3(2.0f, 1.2f, 0.5f), 15.0f);
    CHECK(wall.Arrived);
    CHECK(!wall.Fell);
    CHECK(wall.WorstGap < 0.03f);
    CHECK(wall.Up.x < -0.97f); // side-on, its back to the room
    const ClimbResult ceiling = Climb(w, m, glm::vec3(0.5f, 2.4f, 0.8f), 15.0f);
    CHECK(ceiling.Arrived);
    CHECK(!ceiling.Fell);
    CHECK(ceiling.WorstGap < 0.03f);
    CHECK(ceiling.Up.y < -0.97f); // upside down
    const ClimbResult other = Climb(w, m, glm::vec3(-2.0f, 1.2f, 0.0f), 15.0f);
    CHECK(other.Arrived);
    CHECK(other.Up.x > 0.97f);
    const ClimbResult back = Climb(w, m, glm::vec3(-1.5f, 0.0f, 0.0f), 15.0f);
    CHECK(back.Arrived);
    CHECK(!back.Fell);
    CHECK(back.Up.y > 0.97f);
}

void TestSpiderGoesOverABox() {
    // A crate between it and the goal, too wide to walk round in the time: up its side, over the top, down the far
    // side (two convex edges) and back onto the floor.
    BoxWorld w;
    w.Boxes.push_back({glm::vec3(-2.0f, 0.0f, -0.4f), glm::vec3(2.0f, 0.6f, 0.4f)});
    Spider::Mover m;
    m.Reset(glm::vec3(0.0f, 0.0f, 1.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    const ClimbResult top = Climb(w, m, glm::vec3(0.0f, 0.6f, 0.0f), 15.0f);
    CHECK(top.Arrived);
    CHECK(!top.Fell);
    CHECK(top.Up.y > 0.97f);
    const ClimbResult r = Climb(w, m, glm::vec3(0.0f, 0.0f, -1.5f), 15.0f);
    CHECK(r.Arrived);
    CHECK(!r.Fell);
    CHECK(r.Up.y > 0.97f);
}

void TestSpiderFallsAndLands() {
    // Let go of the ceiling (nothing under it from the start): it falls, rights itself and lands on the floor.
    BoxWorld w;
    w.RoomMax.y = 50.0f; // no ceiling in reach
    Spider::Mover m;
    m.Reset(glm::vec3(0.0f, 2.0f, 0.0f), glm::angleAxis(glm::radians(180.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
    Spider::MoverSettings s;
    Spider::MoveInput in;
    bool fell = false;
    for (int f = 0; f < 240; ++f) {
        m.Step(w, s, in, 1.0f / 60.0f);
        fell = fell || !m.Attached();
    }
    CHECK(fell);
    CHECK(m.Attached());
    CHECK(std::fabs(m.Position().y) < 0.01f);
    CHECK(m.Up().y > 0.9f);
}

void TestSpiderGaitAlternates() {
    BoxWorld w;
    w.RoomMin = glm::vec3(-50.0f, 0.0f, -50.0f);
    w.RoomMax = glm::vec3(50.0f, 10.0f, 50.0f);
    // Eight legs in a ring round the body, hips at 0.15 m, feet at 0.6 m; groups L1 R2 L3 R4 / R1 L2 R3 L4.
    std::array<Spider::LegRest, Spider::kLegs> rest{};
    const float angles[Spider::kLegs] = {35, -35, 75, -75, 105, -105, 145, -145}; // L1 R1 L2 R2 L3 R3 L4 R4, from -Z
    const int groups[Spider::kLegs] = {0, 1, 1, 0, 0, 1, 1, 0};
    for (int i = 0; i < Spider::kLegs; ++i) {
        const float a = glm::radians(angles[i]);
        const glm::vec3 dir(-std::sin(a), 0.0f, -std::cos(a));
        rest[(size_t)i].Hip = dir * 0.15f + glm::vec3(0.0f, 0.25f, 0.0f);
        rest[(size_t)i].Foot = dir * 0.6f;
        rest[(size_t)i].Reach = 0.75f;
        rest[(size_t)i].Group = groups[i];
    }
    Spider::Gait gait;
    glm::vec3 pos(0.0f);
    gait.Init(rest, glm::translate(glm::mat4(1.0f), pos));
    Spider::GaitSettings s;
    const glm::vec3 vel(0.0f, 0.0f, -1.0f);
    const float dt = 1.0f / 60.0f;
    std::array<glm::vec3, Spider::kLegs> prev{};
    std::array<bool, Spider::kLegs> wasPlanted{};
    float worstSlide = 0.0f, worstStretch = 0.0f;
    int steps = 0;
    for (int f = 0; f < 180; ++f) {
        pos += vel * dt;
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), pos);
        gait.Step(w, s, model, pos, glm::vec3(0.0f, 1.0f, 0.0f), vel, true, dt);
        steps += gait.TakeLanded();
        for (int i = 0; i < Spider::kLegs; ++i) {
            const Spider::LegState& l = gait.Leg(i);
            if (l.Planted && wasPlanted[(size_t)i]) worstSlide = std::max(worstSlide, glm::length(l.Foot - prev[(size_t)i]));
            prev[(size_t)i] = l.Foot;
            wasPlanted[(size_t)i] = l.Planted;
            const glm::vec3 hip = pos + rest[(size_t)i].Hip;
            worstStretch = std::max(worstStretch, glm::length(l.Foot - hip) / rest[(size_t)i].Reach);
        }
    }
    CHECK(gait.BothGroupsLifted() == 0); // four legs always down
    CHECK(steps >= 16);                  // 3 m at a walk: every leg stepped, several times
    CHECK(worstSlide < 1e-5f);           // a planted foot doesn't move
    CHECK(worstStretch < 1.05f);         // and no foot is left behind past the leg's reach
    for (int i = 0; i < Spider::kLegs; ++i) CHECK(std::fabs(gait.Leg(i).Foot.y) < 0.08f);
}

void TestSpiderRigParses() {
    std::string legs, a = "[", b = "[";
    const char* names[Spider::kLegs] = {"L1", "R1", "L2", "R2", "L3", "R3", "L4", "R4"};
    const int groups[Spider::kLegs] = {0, 1, 1, 0, 0, 1, 1, 0};
    for (int i = 0; i < Spider::kLegs; ++i) {
        legs += std::string(i ? "," : "") + "{\"name\":\"" + names[i] + "\",\"bones\":[\"a" + std::to_string(i) + "\",\"b" +
                std::to_string(i) + "\"]}";
        std::string& g = groups[i] ? b : a;
        g += std::string(g.size() > 1 ? "," : "") + "\"" + names[i] + "\"";
    }
    const std::string text = "{\"body\":{\"thorax\":\"Bone\",\"abdomen\":\"Bone.001\"},\"legs\":[" + legs +
                             "],\"gaitGroups\":[" + a + "]," + b + "]]}";
    Spider::RigDef rig;
    CHECK(Spider::ParseRig(text, rig));
    CHECK(rig.Thorax == "Bone");
    CHECK(rig.LegBones[5].size() == 2 && rig.LegBones[5][1] == "b5");
    CHECK(rig.LegGroup[0] == 0 && rig.LegGroup[1] == 1 && rig.LegGroup[3] == 0);
    std::string error;
    CHECK(!Spider::ParseRig("{\"legs\":[]}", rig, &error));
    CHECK(!error.empty());
}

} // namespace

void TestSpiderNavRoutesFloorToCeiling() {
    // A crate across the room's middle, wall to wall: from the floor on one side to the ceiling over the other.
    BoxWorld w;
    w.Boxes.push_back({glm::vec3(-2.0f, 0.0f, -0.4f), glm::vec3(2.0f, 0.6f, 0.4f)});
    std::vector<float> verts;
    std::vector<int> tris;
    w.Triangles(verts, tris);
    Spider::SurfaceGraph g;
    const Spider::NavSettings s;
    CHECK(g.Build(verts, tris, w, s));
    CHECK(g.NodeCount() > 400 && g.LinkCount() > g.NodeCount());
    // Every node has room over it, faces into the open, and every link is short.
    bool roomy = true, shortLinks = true;
    for (int i = 0; i < g.NodeCount(); ++i) {
        roomy = roomy && !w.Occupied(g.Pos(i) + g.Normal(i) * s.Room, s.RoomRadius * 0.9f);
        for (int k = g.FirstLink(i); k < g.FirstLink(i + 1); ++k) shortLinks = shortLinks && glm::length(g.Pos(g.Neighbour(k)) - g.Pos(i)) <= s.LinkRadius + 1e-4f;
    }
    CHECK(roomy);
    CHECK(shortLinks);
    // No node inside the crate or on the room's outside.
    int buried = 0;
    for (int i = 0; i < g.NodeCount(); ++i) buried += g.Pos(i).y < -0.01f || g.Pos(i).y > 2.41f ? 1 : 0;
    CHECK(buried == 0);

    Spider::Mover m;
    m.Reset(glm::vec3(0.0f, 0.0f, 1.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    const RouteResult up = FollowRoute(w, g, m, glm::vec3(0.5f, 2.4f, -1.2f), 30.0f);
    CHECK(up.Planned);
    CHECK(up.OffSurface > 0); // it leaves the floor: up a wall (or over the crate and up)
    CHECK(up.Arrived);
    CHECK(!up.Fell);
    CHECK(up.Up.y < -0.97f);
    // And back down to the floor on the start's side.
    const RouteResult down = FollowRoute(w, g, m, glm::vec3(-1.0f, 0.0f, 1.2f), 30.0f);
    CHECK(down.Arrived);
    CHECK(!down.Fell);
    CHECK(down.Up.y > 0.97f);

    // Walls cost more: a floor-to-floor route over the crate still goes over it (no way round), but stays as short.
    std::vector<int> a, b;
    const int from = g.Nearest(glm::vec3(0.0f, 0.0f, 1.5f)), to = g.Nearest(glm::vec3(0.0f, 0.0f, -1.5f));
    CHECK(g.FindPath(from, to, Spider::PathCosts{}, a));
    Spider::PathCosts climbShy;
    climbShy.Wall = 3.0f;
    climbShy.Ceiling = 5.0f;
    CHECK(g.FindPath(from, to, climbShy, b));
    bool ceilingUsed = false;
    for (int n : b) ceilingUsed = ceilingUsed || g.Normal(n).y < -0.7f;
    CHECK(!ceilingUsed);
}

void TestSpiderNavCacheRoundTrips() {
    BoxWorld w;
    std::vector<float> verts;
    std::vector<int> tris;
    w.Triangles(verts, tris);
    Spider::SurfaceGraph g, back;
    const Spider::NavSettings s;
    CHECK(g.Build(verts, tris, w, s));
    const std::uint64_t hash = Spider::SurfaceGraph::HashInput(verts, tris, s);
    const std::string path = "UnitTest_SpiderNav.snav";
    CHECK(g.Save(path, hash));
    CHECK(back.Load(path, hash));
    CHECK(!back.Load(path, hash + 1)); // another level's graph isn't taken
    CHECK(back.NodeCount() == g.NodeCount() && back.LinkCount() == g.LinkCount());
    std::vector<int> p1, p2;
    const int from = g.Nearest(glm::vec3(1.5f, 0.0f, 1.5f)), to = g.Nearest(glm::vec3(-1.5f, 2.4f, -1.5f));
    CHECK(g.FindPath(from, to, Spider::PathCosts{}, p1));
    CHECK(back.FindPath(back.Nearest(glm::vec3(1.5f, 0.0f, 1.5f)), back.Nearest(glm::vec3(-1.5f, 2.4f, -1.5f)), Spider::PathCosts{}, p2));
    CHECK(p1 == p2);
    std::remove(path.c_str());
}

void RegisterSpiderTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"SpiderNavRoutesFloorToCeiling", TestSpiderNavRoutesFloorToCeiling});
    tests.push_back({"SpiderNavCacheRoundTrips", TestSpiderNavCacheRoundTrips});
    tests.push_back({"SolveChainReachesAndBendsTowardPole", TestSolveChainReachesAndBendsTowardPole});
    tests.push_back({"SpiderClimbsFloorWallCeiling", TestSpiderClimbsFloorWallCeiling});
    tests.push_back({"SpiderGoesOverABox", TestSpiderGoesOverABox});
    tests.push_back({"SpiderFallsAndLands", TestSpiderFallsAndLands});
    tests.push_back({"SpiderGaitAlternates", TestSpiderGaitAlternates});
    tests.push_back({"SpiderRigParses", TestSpiderRigParses});
}
