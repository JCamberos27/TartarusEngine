#include "UnitTestSupport.h"

#include "Creatures/SpiderLocomotion.h"
#include "IK.h"

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
};

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

void RegisterSpiderTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"SolveChainReachesAndBendsTowardPole", TestSolveChainReachesAndBendsTowardPole});
    tests.push_back({"SpiderClimbsFloorWallCeiling", TestSpiderClimbsFloorWallCeiling});
    tests.push_back({"SpiderGoesOverABox", TestSpiderGoesOverABox});
    tests.push_back({"SpiderFallsAndLands", TestSpiderFallsAndLands});
    tests.push_back({"SpiderGaitAlternates", TestSpiderGaitAlternates});
    tests.push_back({"SpiderRigParses", TestSpiderRigParses});
}
