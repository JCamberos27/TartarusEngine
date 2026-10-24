#include "UnitTestSupport.h"

#include "Creatures/SpiderDamage.h"
#include "Creatures/SpiderLocomotion.h"
#include "Creatures/SpiderNav.h"
#include "Creatures/SpiderRagdoll.h"
#include "Creatures/SpiderSystem.h"
#include "Components.h"
#include "GameModuleAPI.h"
#include "IK.h"
#include "PhysicsWorld.h"
#include "ProjectPaths.h"
#include "Renderer/BloodPalette.h"
#include "World.h"

#include <filesystem>
#include <fstream>
#include <sstream>

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
    int Sunk = 0; // frames its head or abdomen was inside a surface
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
        // The body's ends (as MoverSettings has them, a little in from the tips) never inside the room's walls.
        const glm::vec3 mid = m.Position() + m.Up() * s.BodyLift;
        if (w.Occupied(mid + m.Forward() * (s.BodyFront - 0.03f), 0.0f) || w.Occupied(mid - m.Forward() * (s.BodyBack - 0.03f), 0.0f)) ++r.Sunk;
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
    CHECK(wall.WorstGap < 0.35f); // (its middle swings clear of the corner as it turns)
    CHECK(wall.Sunk == 0);
    CHECK(wall.Up.x < -0.97f); // side-on, its back to the room
    const ClimbResult ceiling = Climb(w, m, glm::vec3(0.5f, 2.4f, 0.8f), 15.0f);
    CHECK(ceiling.Arrived);
    CHECK(!ceiling.Fell);
    CHECK(ceiling.WorstGap < 0.35f);
    CHECK(ceiling.Sunk == 0);
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

void TestSpiderWedgedStaysDown() {
    // Wedged in a slot shorter than its body (0.55 m; head 0.26 + abdomen 0.4 m), standing still: both ends touch, so the
    // push-out lifts it - only a little off the floor, however long it stays there (unbounded, it rose every frame).
    BoxWorld w;
    w.RoomMax.y = 1.2f;
    w.Boxes.push_back({glm::vec3(-2.0f, 0.0f, -2.0f), glm::vec3(2.0f, 1.2f, -0.25f)});
    w.Boxes.push_back({glm::vec3(-2.0f, 0.0f, 0.3f), glm::vec3(2.0f, 1.2f, 2.0f)});
    Spider::Mover m;
    m.Reset(glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)); // facing -z, into the end wall
    Spider::MoverSettings s;
    Spider::MoveInput in;
    in.HasGoal = false;
    float highest = 0.0f;
    for (int i = 0; i < 300; ++i) {
        m.Step(w, s, in, 1.0f / 60.0f);
        highest = std::max(highest, m.Position().y);
    }
    CHECK(highest < 0.4f);
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

// Eight legs' worth of JSON for a rig text (the gait test's names and groups), and `parts` spliced in.
std::string RigText(const std::string& parts) {
    std::string legs, a = "[", b = "[";
    const char* names[Spider::kLegs] = {"L1", "R1", "L2", "R2", "L3", "R3", "L4", "R4"};
    const int groups[Spider::kLegs] = {0, 1, 1, 0, 0, 1, 1, 0};
    for (int i = 0; i < Spider::kLegs; ++i) {
        legs += std::string(i ? "," : "") + "{\"name\":\"" + names[i] + "\",\"bones\":[\"a" + std::to_string(i) + "\",\"b" +
                std::to_string(i) + "\"]}";
        std::string& g = groups[i] ? b : a;
        g += std::string(g.size() > 1 ? "," : "") + "\"" + names[i] + "\"";
    }
    return "{\"legs\":[" + legs + "],\"gaitGroups\":[" + a + "]," + b + "]]" + (parts.empty() ? "" : ",\"parts\":" + parts) + "}";
}

// The body-part table: Spider.rig.json's 26 parts (the thorax the one root, the abdomen on it, three segments a leg in
// a chain off the thorax, every zone named), and a table that is wrong in each way the parser refuses.
void TestSpiderPartsParse() {
    std::string text;
    {
        std::ifstream in(std::filesystem::u8path(ProjectPaths::Resolve("assets/Characters/Spider/Spider.rig.json")), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    Spider::RigDef rig;
    std::string error;
    CHECK(!text.empty() && Spider::ParseRig(text, rig, &error));
    CHECK(rig.Parts.size() == 26);
    int roots = 0, legParts = 0, heads = 0, abdomens = 0;
    std::array<int, Spider::kLegs> perLeg{};
    for (const Spider::PartDef& p : rig.Parts) {
        if (p.Parent < 0) { ++roots; CHECK(p.Name == "thorax" && p.Zone == Spider::PartZone::Head); }
        heads += p.Zone == Spider::PartZone::Head;
        abdomens += p.Zone == Spider::PartZone::Abdomen;
        if (p.Leg >= 0) {
            ++legParts;
            ++perLeg[(size_t)p.Leg];
            CHECK(p.Zone == Spider::PartZone::Leg);
            // A leg's first segment hangs off the thorax, the next two off the one before.
            const Spider::PartDef& parent = rig.Parts[(size_t)p.Parent];
            CHECK(p.Segment == 0 ? parent.Name == "thorax" : (parent.Leg == p.Leg && parent.Segment == p.Segment - 1));
            CHECK(!p.End.empty() && p.Radius > 0.01f && p.Mass > 0.0f);
        }
    }
    CHECK(roots == 1 && heads == 1 && abdomens == 1 && legParts == 24);
    for (int n : perLeg) CHECK(n == 3);
    CHECK(rig.ZoneMultiplier[0] > 2.0f && rig.ZoneMultiplier[2] < 1.0f);

    const std::string root = "{\"name\":\"t\",\"bone\":\"B\",\"end\":\"E\",\"zone\":\"head\"}";
    CHECK(Spider::ParseRig(RigText("[" + root + "]"), rig) && rig.Parts.size() == 1);
    CHECK(!Spider::ParseRig(RigText("[" + root + ",{\"name\":\"x\",\"bone\":\"C\",\"end\":\"D\",\"parent\":\"nobody\"}]"), rig));
    CHECK(!Spider::ParseRig(RigText("[" + root + "," + root + "]"), rig));                     // two roots
    CHECK(!Spider::ParseRig(RigText("[{\"name\":\"t\",\"bone\":\"B\",\"end\":\"E\",\"zone\":\"tail\"}]"), rig)); // no such zone
    CHECK(!Spider::ParseRig(RigText("[{\"name\":\"t\",\"bone\":\"B\"}]"), rig));           // no end, no length
    std::string many = "[" + root;
    for (int i = 0; i < 32; ++i)
        many += ",{\"name\":\"p" + std::to_string(i) + "\",\"bone\":\"B\",\"end\":\"E\",\"parent\":\"t\"}";
    CHECK(!Spider::ParseRig(RigText(many + "]"), rig)); // 33 parts: past the ragdoll's 32
}

// A spider's table: thorax, abdomen and eight legs of three segments (26 parts), standing at the origin as the rig
// does, for the ragdoll test.
void SpiderBody(std::vector<Spider::PartDef>& defs, std::vector<SpiderRagdoll::PartPoint>& points, float lift) {
    defs.clear();
    points.clear();
    Spider::PartDef t;
    t.Name = "thorax"; t.Bone = "B"; t.End = "E"; t.Radius = 0.1f; t.Mass = 1.0f; t.Zone = Spider::PartZone::Head;
    defs.push_back(t);
    points.push_back({{0.0f, 0.15f + lift, -0.19f}, {0.0f, 0.15f + lift, -0.19f}, {0.0f, 0.15f + lift, 0.12f}});
    Spider::PartDef a = t;
    a.Name = "abdomen"; a.Radius = 0.135f; a.Mass = 1.2f; a.Zone = Spider::PartZone::Abdomen; a.Parent = 0; a.Swing = 25.0f;
    defs.push_back(a);
    points.push_back({{0.0f, 0.16f + lift, 0.12f}, {0.0f, 0.16f + lift, 0.12f}, {0.0f, 0.25f + lift, 0.5f}});
    const float angles[Spider::kLegs] = {35, -35, 75, -75, 105, -105, 145, -145};
    for (int leg = 0; leg < Spider::kLegs; ++leg) {
        const float r = glm::radians(angles[leg]);
        const glm::vec3 out(-std::sin(r), 0.0f, -std::cos(r));
        const glm::vec3 hip = glm::vec3(0.0f, 0.08f + lift, 0.0f) + out * 0.03f, coxa = hip + out * 0.12f + glm::vec3(0, 0.03f, 0);
        const glm::vec3 knee = hip + out * 0.2f + glm::vec3(0, 0.26f, 0), ankle = hip + out * 0.33f + glm::vec3(0, 0.12f, 0);
        const glm::vec3 tip = hip + out * 0.45f + glm::vec3(0, -0.07f, 0);
        const float radius[3] = {0.032f, 0.026f, 0.02f}, mass[3] = {0.22f, 0.15f, 0.1f}, curl[3] = {-25.0f, 75.0f, 65.0f};
        const glm::vec3 from[3] = {coxa, knee, ankle}, to[3] = {knee, ankle, tip}, anchor[3] = {hip, knee, ankle};
        for (int s = 0; s < 3; ++s) {
            Spider::PartDef d;
            d.Name = "leg"; d.Bone = "B"; d.End = "E"; d.Radius = radius[s]; d.Mass = mass[s]; d.Zone = Spider::PartZone::Leg;
            d.Leg = leg; d.Segment = s; d.Curl = curl[s]; d.Swing = s ? 60.0f : 45.0f; d.Twist = s ? 10.0f : 20.0f;
            d.Parent = s == 0 ? 0 : (int)defs.size() - 1;
            defs.push_back(d);
            points.push_back({anchor[s], from[s], to[s]});
        }
    }
}

// More than 16 parts: a 26-part spider builds as one ragdoll (the old cap was 16), its legs don't collide with each
// other but do with the body, and powered by the death curl it falls, folds and stays whole: every part finite, none
// flung. 33 parts are refused.
void TestSpiderRagdollBuilds() {
    World world;
    PhysicsWorld::Create(world);
    CHECK(PhysicsWorld::IsActive());
    if (!PhysicsWorld::IsActive()) return;
    std::vector<Spider::PartDef> defs;
    std::vector<SpiderRagdoll::PartPoint> points;
    SpiderBody(defs, points, 1.0f);
    const int count = (int)defs.size();
    CHECK(count == 26 && count > 16 && count <= PhysicsWorld::kMaxRagdollParts);
    std::array<PhysicsWorld::RagdollPart, Spider::kMaxParts> parts{};
    std::array<glm::mat4, Spider::kMaxParts> partWorld{};
    CHECK(SpiderRagdoll::BuildParts(defs.data(), count, points.data(), glm::vec3(0, 1, 0), parts.data(), partWorld.data()));
    const PhysicsWorld::RagdollParams prm = SpiderRagdoll::BodyParams();
    const int id = PhysicsWorld::CreateRagdoll(9191u, parts.data(), count, &prm);
    CHECK(id >= 0);
    if (id >= 0) {
        CHECK(!PhysicsWorld::RagdollPairCollides(id, 4, 7));   // L1's tip and R1's tip: different legs
        CHECK(!PhysicsWorld::RagdollPairCollides(id, 0, 2));   // the thorax and L1's first segment: parent and child
        std::array<glm::quat, Spider::kMaxParts> targets{};
        std::array<float, Spider::kMaxParts> weight{};
        for (int k = 0; k < count; ++k) {
            weight[(size_t)k] = defs[(size_t)k].Leg >= 0 ? 1.0f : 0.0f;
            const glm::vec3 c = points[(size_t)k].B;
            glm::vec3 out(c.x, 0.0f, c.z);
            targets[(size_t)k] = SpiderRagdoll::CurlTarget(defs[(size_t)k], glm::quat_cast(glm::mat3(partWorld[(size_t)k])), glm::vec3(0, 1, 0),
                                                           glm::length(out) > 1e-4f ? glm::normalize(out) : glm::vec3(1, 0, 0));
        }
        SpiderRagdoll::LimpMotor motor;
        motor.Begin(id, count, targets.data(), weight.data(), SpiderRagdoll::LimpMotor::Settings());
        float tipStart = 0.0f;
        for (int k = 4; k < count; k += 3) tipStart += glm::length(glm::vec2(points[(size_t)k].B.x, points[(size_t)k].B.z));
        bool finite = true;
        float fastest = 0.0f;
        for (int f = 0; f < 60; ++f) { // a second: the curl at full strength, the body falling free
            motor.Update(1.0f / 60.0f);
            PhysicsWorld::Step(1.0f / 60.0f, world, {});
            float lin = 0.0f, ang = 0.0f;
            PhysicsWorld::RagdollMotion(id, &lin, &ang);
            fastest = std::max(fastest, ang);
            for (int k = 0; k < count; ++k) {
                float p[3], q[4];
                PhysicsWorld::GetRagdollPart(id, k, p, q);
                finite = finite && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
            }
        }
        // The legs came in: each tip nearer the thorax's line than it stood.
        float thorax[3], tq[4], tipNow = 0.0f;
        PhysicsWorld::GetRagdollPart(id, 0, thorax, tq);
        for (int k = 4; k < count; k += 3) {
            float p[3], q[4];
            PhysicsWorld::GetRagdollPart(id, k, p, q);
            const glm::quat r(q[3], q[0], q[1], q[2]);
            float rad = 0.0f, half = 0.0f;
            PhysicsWorld::GetRagdollPartShape(id, k, &rad, &half);
            const glm::vec3 tip = glm::vec3(p[0], p[1], p[2]) + r * glm::vec3(half, 0, 0);
            tipNow += glm::length(glm::vec2(tip.x - thorax[0], tip.z - thorax[2]));
        }
        CHECK(finite);
        CHECK(fastest < 40.0f); // rad/s: no leg spun out
        CHECK(tipNow < tipStart * 0.85f);
        PhysicsWorld::DestroyRagdoll(id);
    }
    std::array<PhysicsWorld::RagdollPart, 33> tooMany{};
    for (int i = 1; i < 33; ++i) { tooMany[(size_t)i].Parent = 0; tooMany[(size_t)i].Position[1] = 0.3f * (float)i; }
    CHECK(PhysicsWorld::CreateRagdoll(9192u, tooMany.data(), 33, &prm) < 0);
    PhysicsWorld::Destroy();
}

// The blood palette: a person's is exactly what it was before palettes were per body (the constants every stain and
// spray was drawn with), palette 0 is a person's, and anything unknown falls back to it; the ichor is yellow-green.
void TestBloodPaletteHumanUnchanged() {
    const BloodPalette::Look& h = BloodPalette::Of(BloodPalette::kHuman);
    auto same = [](const glm::vec3& a, float x, float y, float z) { return a.x == x && a.y == y && a.z == z; };
    CHECK(same(h.Fresh, 0.16f, 0.006f, 0.005f));
    CHECK(same(h.Dried, 0.045f, 0.014f, 0.010f));
    CHECK(same(h.Thin, 0.20f, 0.0075f, 0.006f));
    CHECK(same(h.Mist, 0.09f, 0.0034f, 0.0028f));
    CHECK(same(h.Subsurface, 0.34f, 0.010f, 0.007f));
    CHECK(h.RoughFresh == 0.38f && h.RoughPool == 0.32f && h.RoughDried == 0.75f && h.RoughSpray == 0.28f && h.RoughSprite == 0.30f &&
          h.RoughCloth == 0.35f);
    CHECK(h.Thickness == 0.55f && h.FilmOpacity == 1.0f); // the spray's thickness the renderer always set; stains fully covering
    CHECK(same(BloodPalette::Human.Fresh, 0.16f, 0.006f, 0.005f) && same(BloodPalette::Fresh, 0.16f, 0.006f, 0.005f));
    CHECK(&BloodPalette::Of(-1) == &BloodPalette::Of(0) && &BloodPalette::Of(57) == &BloodPalette::Of(0));
    const BloodPalette::Look& i = BloodPalette::Of(BloodPalette::kIchor);
    CHECK(i.Fresh.g > i.Fresh.r && i.Fresh.r > 3.0f * i.Fresh.b); // yellow-green
    CHECK(i.Dried.g < i.Fresh.g * 0.3f);                           // darker dried: olive
    CHECK(i.FilmOpacity < 1.0f);                                    // a little translucent
}

// Damage by zone: the head 2.5x, the abdomen 1.2x, a leg 0.5x (the defaults and the rig's), and what knocks a spider off
// a wall (a shotgun's pellets, or a round past 25).
void TestSpiderZoneMultipliers() {
    Spider::RigDef rig;
    CHECK(SpiderDamage::ZoneMultiplier(rig, Spider::PartZone::Head) == 2.5f);
    CHECK(SpiderDamage::ZoneMultiplier(rig, Spider::PartZone::Abdomen) == 1.2f);
    CHECK(SpiderDamage::ZoneMultiplier(rig, Spider::PartZone::Leg) == 0.5f);
    const std::string text = RigText("[]").substr(0, RigText("[]").size() - 1) +
                             ",\"zones\":{\"head\":{\"damage\":3.0},\"leg\":{\"damage\":0.25,\"flinch\":40}}}";
    CHECK(Spider::ParseRig(text, rig));
    CHECK(SpiderDamage::ZoneMultiplier(rig, Spider::PartZone::Head) == 3.0f);
    CHECK(SpiderDamage::ZoneMultiplier(rig, Spider::PartZone::Abdomen) == 1.2f);
    CHECK(SpiderDamage::ZoneMultiplier(rig, Spider::PartZone::Leg) == 0.25f && rig.FlinchDeg[2] == 40.0f);
    // A rifle round (34): two to the body and it is nearly dead (60), one to the head kills.
    Spider::RigDef d;
    CHECK(34.0f * SpiderDamage::ZoneMultiplier(d, Spider::PartZone::Head) >= 60.0f);
    CHECK(34.0f * SpiderDamage::ZoneMultiplier(d, Spider::PartZone::Leg) + 34.0f * SpiderDamage::ZoneMultiplier(d, Spider::PartZone::Abdomen) < 60.0f);
    CHECK(SpiderDamage::Heavy(10.0f, 8) && SpiderDamage::Heavy(26.0f, 1) && !SpiderDamage::Heavy(17.0f, 1));
    // The ichor's hit: the spider's palette, the abdomen throwing more.
    SpiderDamage::Result r;
    r.Damage = 40.0f;
    r.Zone = Spider::PartZone::Abdomen;
    r.Living = true;
    const BloodFx::Hit h = SpiderDamage::Blood(r);
    CHECK(h.Palette == BloodPalette::kIchor && h.Damage > r.Damage && !h.Corpse && !h.Head);
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

void TestSpiderNavOneSidedPlaneSplits() {
    // A thin ceiling plane across a taller room, solid only from below (a mesh collider is one-sided): the walls go on
    // above it into the void, and no link may join the room's walls to theirs, whichever way the link's ray is cast.
    struct PlaneWorld : BoxWorld {
        float PlaneY = 1.2f;
        bool Ray(const glm::vec3& o, const glm::vec3& d, float maxDistance, Spider::SurfaceHit& out) const override {
            Spider::SurfaceHit h;
            const bool room = BoxWorld::Ray(o, d, maxDistance, h);
            if (d.y > 1e-6f && o.y < PlaneY) { // its face is down: only a ray going up hits it
                const float t = (PlaneY - o.y) / d.y;
                if (t <= maxDistance && (!room || t < h.Distance)) {
                    out.Hit = true;
                    out.Distance = t;
                    out.Point = o + d * t;
                    out.Normal = glm::vec3(0.0f, -1.0f, 0.0f);
                    return true;
                }
            }
            if (room) out = h;
            return room;
        }
    };
    PlaneWorld w;
    std::vector<float> verts;
    std::vector<int> tris;
    w.Triangles(verts, tris);
    Spider::SurfaceGraph g;
    CHECK(g.Build(verts, tris, w, Spider::NavSettings{}));
    int crossing = 0;
    for (int i = 0; i < g.NodeCount(); ++i)
        for (int k = g.FirstLink(i); k < g.FirstLink(i + 1); ++k)
            if ((g.Pos(i).y - w.PlaneY) * (g.Pos(g.Neighbour(k)).y - w.PlaneY) < 0.0f) ++crossing;
    CHECK(crossing == 0);
    // Above and below are separate pieces, and a pick restricted to the floor's piece never lands above the plane.
    const glm::vec3 upY(0.0f, 1.0f, 0.0f);
    const int floor = g.Nearest(glm::vec3(0.0f, 0.0f, 0.0f), 1.0f, &upY);
    CHECK(floor >= 0);
    const int high = g.Nearest(glm::vec3(1.9f, 2.0f, 0.0f), 1.0f);
    CHECK(high >= 0 && g.Component(high) != g.Component(floor));
    const int pick = g.Nearest(glm::vec3(1.9f, 2.0f, 0.0f), 1.5f, nullptr, g.Component(floor));
    CHECK(pick < 0 || g.Pos(pick).y < w.PlaneY);
}

void TestSpiderBarrierClosesAnOpening() {
    // A window's opening has no collider: a creature barrier across it (turned 90 degrees, as a window in a side wall is)
    // closes it to spiders alone. Without one, the graph links the two sides and a spider walks through; with one,
    // no link crosses it, the sides are separate pieces, and a spider sent across never gets past it.
    BoxWorld w;
    std::vector<Spider::Barrier> barriers(1);
    barriers[0].Centre = glm::vec3(0.0f, 1.2f, 0.0f);
    barriers[0].Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    barriers[0].Half = glm::vec3(0.03f, 1.3f, 2.1f); // its local z runs along world x, wall to wall and into them
    const Spider::BarrierQuery closed(w, barriers);
    const std::vector<Spider::Barrier> none;
    const Spider::BarrierQuery open(w, none);
    std::vector<float> verts;
    std::vector<int> tris;
    w.Triangles(verts, tris);
    const Spider::NavSettings s;
    const glm::vec3 upY(0.0f, 1.0f, 0.0f);
    auto crossings = [](const Spider::SurfaceGraph& g) {
        int n = 0;
        for (int i = 0; i < g.NodeCount(); ++i)
            for (int k = g.FirstLink(i); k < g.FirstLink(i + 1); ++k) n += g.Pos(i).z * g.Pos(g.Neighbour(k)).z < 0.0f ? 1 : 0;
        return n;
    };
    Spider::SurfaceGraph without, with;
    CHECK(without.Build(verts, tris, open, s));
    CHECK(crossings(without) > 0);
    CHECK(with.Build(verts, tris, closed, s));
    CHECK(crossings(with) == 0);
    const int near = with.Nearest(glm::vec3(0.0f, 0.0f, 1.5f), 1.0f, &upY), far = with.Nearest(glm::vec3(0.0f, 0.0f, -1.5f), 1.0f, &upY);
    CHECK(near >= 0 && far >= 0 && with.Component(near) != with.Component(far));
    // The cache key changes with the barriers: a graph baked before a window was closed isn't taken.
    const std::uint64_t h = Spider::SurfaceGraph::HashInput(verts, tris, s);
    CHECK(Spider::SurfaceGraph::HashBarriers(h, barriers) != h);
    CHECK(Spider::SurfaceGraph::HashBarriers(h, none) == h);

    // The mover alone (no graph): straight at the far side, it climbs the barrier like a wall but never passes it.
    auto furthest = [](const Spider::SurfaceQuery& q) {
        Spider::Mover m;
        m.Reset(glm::vec3(0.0f, 0.0f, 1.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        Spider::MoverSettings ms;
        Spider::MoveInput in;
        in.HasGoal = true;
        in.Goal = glm::vec3(0.0f, 0.0f, -1.5f);
        float least = 1e9f;
        for (int i = 0; i < 20 * 60; ++i) {
            m.Step(q, ms, in, 1.0f / 60.0f);
            least = std::min(least, m.Position().z);
        }
        return least;
    };
    CHECK(furthest(open) < -1.0f);
    CHECK(furthest(closed) > 0.0f);
}

void TestSpiderProbeStartsInsideASolid() {
    // The real world, through PhysX: a floor, and a low solid shelf 0.2-0.5 m over it. A spider standing under the shelf
    // probes for its footing from a quarter metre up - inside the shelf. PhysX reports a solid a ray starts inside at the
    // ray's origin; taken as footing, it stood the body on its own probe's start and lifted it every frame, up through
    // the shelf (in 301, through furniture and the ceiling slab onto the roof). The spider's query passes through it to
    // the floor.
    World world;
    const glm::vec3 zero(0.0f), one(1.0f);
    ColliderComponent col;
    col.Kind = ColliderComponent::Shape::Box;
    col.HalfExtents = glm::vec3(5.0f, 0.5f, 5.0f);
    world.Registry.emplace<ColliderComponent>(world.CreateEmptyEntity(glm::vec3(0.0f, -0.5f, 0.0f), zero, one, "Floor"), col);
    col.HalfExtents = glm::vec3(2.0f, 0.15f, 2.0f);
    world.Registry.emplace<ColliderComponent>(world.CreateEmptyEntity(glm::vec3(0.0f, 0.35f, 0.0f), zero, one, "Shelf"), col);
    world.SyncActiveInHierarchy();
    world.RebuildWorldTransformCache();
    PhysicsWorld::Create(world);
    CHECK(PhysicsWorld::IsActive());
    if (!PhysicsWorld::IsActive()) return;

    const float from[3] = {0.0f, 0.25f, 0.0f}, down[3] = {0.0f, -1.0f, 0.0f};
    QueryFilter f;
    f.HitTriggers = 0;
    RaycastHit h;
    CHECK(PhysicsWorld::RaycastSolid(from, down, 1.0f, f, h) && h.Distance < 1e-4f); // PhysX: the shelf, at the origin
    CHECK(PhysicsWorld::RaycastSolid(from, down, 1.0f, f, h, /*fromOutside=*/true) && std::fabs(h.Distance - 0.25f) < 1e-3f &&
          h.Normal[1] > 0.99f); // the floor

    const Spider::WorldQuery q;
    Spider::Mover m;
    m.Reset(glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    Spider::MoverSettings s;
    Spider::MoveInput in;
    in.HasGoal = false;
    float highest = 0.0f;
    for (int i = 0; i < 120; ++i) {
        m.Step(q, s, in, 1.0f / 60.0f);
        highest = std::max(highest, m.Position().y);
    }
    CHECK(m.Attached());
    CHECK(highest < 0.15f);
    PhysicsWorld::Destroy();
}

void TestSpiderShoveNeverStacks() {
    // A bite shoves a standing player back at its speed; a second from the side, while they still reel, leaves them no
    // faster (the old cap was only along each shove's own line: two at right angles made 1.41 times the speed, and a
    // pounce with a bite threw the player 1.5 m in a second). Already running faster, a shove doesn't slow them.
    const glm::vec3 a = SpiderSystem::Shove(glm::vec3(0.0f), glm::vec3(2.5f, 0.0f, 0.0f));
    CHECK(std::fabs(a.x - 2.5f) < 1e-4f && std::fabs(a.z) < 1e-4f);
    const glm::vec3 b = SpiderSystem::Shove(a, glm::vec3(0.0f, 0.0f, 3.75f));
    CHECK(glm::length(glm::vec2(b.x, b.z)) <= 3.75f + 1e-4f);
    CHECK(b.z > 2.0f); // it does turn them toward the new shove
    const glm::vec3 c = SpiderSystem::Shove(glm::vec3(0.0f, -1.0f, 5.0f), glm::vec3(2.5f, 0.0f, 0.0f));
    CHECK(std::fabs(glm::length(glm::vec2(c.x, c.z)) - 5.0f) < 1e-3f && std::fabs(c.y + 1.0f) < 1e-6f);
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
    // A graph an older builder baked from this same level isn't taken either: the key holds the builder's version.
    const std::uint64_t older = Spider::SurfaceGraph::HashInput(verts, tris, s, Spider::SurfaceGraph::kBuildVersion - 1);
    CHECK(older != hash);
    CHECK(g.Save(path, older));
    CHECK(!back.Load(path, hash));
    CHECK(g.Save(path, hash));
    CHECK(back.Load(path, hash));
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
    tests.push_back({"SpiderWedgedStaysDown", TestSpiderWedgedStaysDown});
    tests.push_back({"SpiderNavOneSidedPlaneSplits", TestSpiderNavOneSidedPlaneSplits});
    tests.push_back({"SpiderBarrierClosesAnOpening", TestSpiderBarrierClosesAnOpening});
    tests.push_back({"SpiderProbeStartsInsideASolid", TestSpiderProbeStartsInsideASolid});
    tests.push_back({"SpiderShoveNeverStacks", TestSpiderShoveNeverStacks});
    tests.push_back({"SpiderFallsAndLands", TestSpiderFallsAndLands});
    tests.push_back({"SpiderGaitAlternates", TestSpiderGaitAlternates});
    tests.push_back({"SpiderRigParses", TestSpiderRigParses});
    tests.push_back({"SpiderPartsParse", TestSpiderPartsParse});
    tests.push_back({"SpiderRagdollBuilds", TestSpiderRagdollBuilds});
    tests.push_back({"BloodPaletteHumanUnchanged", TestBloodPaletteHumanUnchanged});
    tests.push_back({"SpiderZoneMultipliers", TestSpiderZoneMultipliers});
}
