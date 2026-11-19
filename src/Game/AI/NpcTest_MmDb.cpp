// --npc-test mmdb (docs/MOTION_MATCHING_HANDOFF.md, Phase 3): the motion matching runtime on the cooked Manny database,
// in LocomotionLab.
//
// Offline, at the start (no rendering in the way):
//   - the database loads (MotionDatabase) within the budgets
//   - search speed: 1000 queries (database frames' features plus noise, mixed filters, a playing frame) through the box
//     search; the first 200 also by brute force - the answers must be identical
//   - every searchable frame tried finds itself (cost 0), and the same query always gives the same frame
//   - the gait speed table (GaitSpeedTable::Measure), ordered crouch < walk < jog < run
//   - left/right coverage: per gait (walk, jog, run, crouch-walk at the table's speed), trajectory-only queries for
//     turning left and right at 45 and 90 deg/s and strafing left and right; each side's best match's trajectory error at
//     +1 s, and the searchable frames per gait that turn or strafe each way
// Then live (visible, LocomotionLab's open floor 10 m south of the 1 m ledge): a bare Quantum body driven by
// MotionMatcher, TrajectoryPredictor as its simulated capsule (collision-aware: sphere casts against the solid world),
// through a scripted course - idle, a walk, walk circles left and right, strafes, jog circles, run circles, a stop, a
// crouch-walk circle, then a walk into the ledge at an angle (the prediction slides along it). Drawn on the Game view:
// the predicted path (grey), the desired trajectory (yellow), the matched frame's trajectory (magenta), the root (green),
// the feet (blue planted). Checked: searches ~10 per second and how long they take, jumps blended (the output's pose pop
// at a jump against the raw jump's), crouch only while asked, never an airborne frame, the trajectory error per gait, the
// wall slide. With nothing baked (CI) it SKIPs.
#include "NpcTest.h"

#include "AssetLibrary.h"
#include "Components.h"
#include "Core/Paths.h"
#include "FirstPersonPresentation.h"
#include "GameModuleAPI.h"
#include "Model.h"
#include "PhysicsWorld.h"
#include "Player.h"
#include "World.h"
#include "../MotionMatching/MotionBake.h"
#include "../MotionMatching/MotionDatabase.h"
#include "../MotionMatching/MotionDebugDraw.h"
#include "../MotionMatching/MotionGait.h"
#include "../MotionMatching/MotionMatcher.h"
#include "../MotionMatching/MotionMath.h"
#include "../MotionMatching/MotionSearch.h"
#include "../MotionMatching/RootSynth.h"
#include "../MotionMatching/TrajectoryPredictor.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>

namespace {

using MotionMath::kPi;
constexpr float kDeg = kPi / 180.0f;
const glm::vec3 kStage(-6.0f, 0.0f, -4.0f);      // the course starts here, facing west; the ledge's south face is at z = 6
const glm::vec3 kPlayerAt(-6.0f, 0.05f, -11.0f); // the player stands 7 m south and turns to follow the body
const glm::vec3 kLedgeFrom(-9.0f, 0.0f, 2.0f);   // the last segment walks here first,
const glm::vec3 kLedgeAim(-6.0f, 0.0f, 6.0f);    // then at the middle of the ledge's south face (x -8..-4), ~39 deg off its normal
constexpr float kRadius = 0.3f, kProbeHeight = 0.5f; // the simulated capsule's sweep: a sphere at knee height

struct Segment {
    const char* Name;
    float Seconds;
    int Gait;      // MotionLabel::Gait
    bool Crouch;
    float TurnDeg; // the travel heading's turn rate, deg/s (+ = left)
    float StrafeDeg; // travel = facing + this (+ = left); the facing holds still while strafing
    bool Wall = false;
};
const Segment kCourse[] = {
    {"idle", 2.0f, MotionLabel::Idle, false, 0, 0},
    {"walk straight", 3.0f, MotionLabel::Walk, false, 0, 0},
    {"walk circle left", 6.0f, MotionLabel::Walk, false, 60, 0},
    {"walk circle right", 6.0f, MotionLabel::Walk, false, -60, 0},
    {"stop", 2.0f, MotionLabel::Idle, false, 0, 0},
    {"walk strafe left", 4.0f, MotionLabel::Walk, false, 0, 90},
    {"walk strafe right", 4.0f, MotionLabel::Walk, false, 0, -90},
    {"jog circle left", 5.0f, MotionLabel::Jog, false, 72, 0},
    {"jog circle right", 5.0f, MotionLabel::Jog, false, -72, 0},
    {"run circle left", 6.0f, MotionLabel::Run, false, 60, 0},
    {"run circle right", 6.0f, MotionLabel::Run, false, -60, 0},
    {"stop", 2.5f, MotionLabel::Idle, false, 0, 0},
    {"crouch circle left", 5.0f, MotionLabel::Walk, true, 45, 0},
    {"crouch idle", 2.0f, MotionLabel::Idle, true, 0, 0},
    {"stand", 1.5f, MotionLabel::Idle, false, 0, 0},
    {"walk into the ledge", 14.0f, MotionLabel::Walk, false, 0, 0, true},
};
constexpr int kSegments = (int)(sizeof(kCourse) / sizeof(kCourse[0]));

float CourseSeconds() {
    float s = 0.0f;
    for (const Segment& g : kCourse) s += g.Seconds;
    return s;
}

struct FkChain { // model-space transforms of a few nodes (as mmview's)
    std::vector<int> Order, Parent;
    void Build(const Model& m, const std::vector<int>& targets) {
        std::vector<char> need((size_t)m.NodeCount(), 0);
        for (int t : targets)
            for (int i = t; i >= 0; i = m.NodeParent(i)) need[(size_t)i] = 1;
        for (int i = 0; i < m.NodeCount(); ++i)
            if (need[(size_t)i]) Order.push_back(i);
        Parent.resize((size_t)m.NodeCount());
        for (int i = 0; i < m.NodeCount(); ++i) Parent[(size_t)i] = m.NodeParent(i);
    }
    void Run(const std::vector<LocalTRS>& pose, std::vector<glm::mat4>& g) const {
        g.resize(pose.size());
        for (int i : Order) {
            const glm::mat4 local = pose[(size_t)i].ToMatrix();
            const int p = Parent[(size_t)i];
            g[(size_t)i] = p >= 0 ? AffineMul(g[(size_t)p], local) : local;
        }
    }
};

float PoseAngle(const MotionPose& a, const MotionPose& b) {
    float w = 0.0f;
    for (size_t i = 0; i < a.Rot.size() && i < b.Rot.size(); ++i) w = std::max(w, MotionMath::QuatAngle(a.Rot[i], b.Rot[i]));
    return w;
}

float Pct(std::vector<float> v, float p) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)std::lround(p * (float)(v.size() - 1)))];
}

// The trajectory error between a query's and a frame's raw trajectory features at +1 s: position (m), facing (deg).
void TrajError(const float* q12, const float* f12, float& posErr, float& facingDeg) {
    posErr = std::hypot(q12[4] - f12[4], q12[5] - f12[5]);
    const float a = std::atan2(q12[10], q12[11]), b = std::atan2(f12[10], f12[11]);
    facingDeg = std::fabs(MotionMath::WrapPi(a - b)) / kDeg;
}

} // namespace

struct NpcTest::MmDbRun {
    bool Started = false, Skipped = false, Finished = false;
    MotionDatabase Db;
    GaitSpeedTable Gaits;
    std::shared_ptr<Model> Body;
    entt::entity Entity = entt::null;
    MotionClipStream::Binding Bind;
    std::vector<LocalTRS> BindPose, Pose;
    FkChain Fk;
    MotionMatcher Matcher;
    TrajectoryPredictor Sim;
    float Clock = 0.0f, Facing = -0.5f * kPi; // course time; facing west
    int Seg = -1;
    bool LedgeLeg = false; // the wall segment: at kLedgeFrom, now heading for the face
    glm::vec3 Watch = kStage + glm::vec3(0.0f, 0.9f, 0.0f);
    std::vector<float> Lines;
    // Live stats.
    int Frames = 0, Finite = 0, CrouchWrong = 0, AirborneMatched = 0, WallSegmentHits = 0;
    float WallWorstPenetration = -1.0f; // m the sphere goes into the ledge (negative: its closest gap)
    std::vector<float> JumpRaw, JumpOut, NonJumpStep;
    struct SegStats { std::vector<float> PosErr, FacingErr; int Searches = 0, Jumps = 0; int GaitFrames[4] = {0, 0, 0, 0}; };
    std::vector<SegStats> Stats = std::vector<SegStats>((size_t)kSegments);
};

float NpcTest::MmDbSeconds() { return CourseSeconds() + 3.0f; }

void NpcTest::MmDbDrive(Player& player, FirstPersonPresentation& weapon, float dt, float& wantYaw, float& wantPitch) {
    if (!m_MmDb) m_MmDb = std::make_shared<MmDbRun>();
    MmDbRun& r = *m_MmDb;
    if (!r.Started) {
        PhysicsWorld::SetCharacterFootPosition(&kPlayerAt.x);
        player.Velocity = glm::vec3(0.0f);
        player.Cam.Position = kPlayerAt + glm::vec3(0.0f, player.EyeHeight, 0.0f);
        weapon.SetEquipped(false); // unarmed
    }
    const glm::vec3 d = r.Watch - glm::vec3(0.0f, 0.35f, 0.0f) - player.Cam.Position;
    wantYaw = glm::degrees(std::atan2(d.z, d.x));
    wantPitch = glm::degrees(std::atan2(d.y, std::max(0.5f, glm::length(glm::vec2(d.x, d.z)))));
    (void)dt;
}

void NpcTest::MmDbLines(std::vector<float>& out) const {
    if (m_MmDb) out.insert(out.end(), m_MmDb->Lines.begin(), m_MmDb->Lines.end());
}

void NpcTest::MmDbOffline() {
    MmDbRun& r = *m_MmDb;
    const MotionDatabase& db = r.Db;
    char b[512];

    // --- Search speed, boxes against brute force ---
    std::mt19937 rng(20261116);
    std::vector<std::uint32_t> searchable;
    for (std::uint32_t i = 0; i < db.FrameCount(); ++i)
        if (MotionSearch::Passes(db.Flags(i), MotionSearchParams{})) searchable.push_back(i);
    if (searchable.empty()) {
        Check(false, "the database has searchable frames");
        return;
    }
    std::uniform_int_distribution<size_t> pick(0, searchable.size() - 1);
    std::normal_distribution<float> noise(0.0f, 0.25f);
    std::vector<float> ms;
    int mismatches = 0, compared = 0;
    double bruteMs = 0.0;
    std::uint64_t scored = 0;
    for (int k = 0; k < 1000; ++k) {
        float q[27];
        const auto picked = db.Normalized(searchable[pick(rng)]);
        std::memcpy(q, picked.data(), sizeof q);
        for (float& x : q) x += noise(rng);
        MotionSearchParams p;
        if (k % 4 == 1) p.Forbid |= MotionLabel::Crouch;
        if (k % 4 == 2) p.Require = MotionLabel::Crouch;
        if (k % 4 == 3) p.DesiredGait = (int)(k / 4) % 4;
        const std::int64_t current = k % 2 ? (std::int64_t)searchable[pick(rng)] : -1;
        const auto t0 = std::chrono::steady_clock::now();
        const MotionSearchResult a = MotionSearch::Search(db, q, p, current);
        ms.push_back((float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        scored += a.FramesScored;
        if (k < 200) {
            const auto t1 = std::chrono::steady_clock::now();
            const MotionSearchResult c = MotionSearch::SearchBrute(db, q, p, current);
            bruteMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
            ++compared;
            if (a.Frame != c.Frame || a.Cost != c.Cost || a.Switched != c.Switched) ++mismatches;
        }
    }
    double avg = 0.0;
    for (float x : ms) avg += x;
    avg /= (double)ms.size();
    std::snprintf(b, sizeof b, "the box search gives brute force's answer: %d of %d differ", mismatches, compared);
    Check(mismatches == 0, b);
    std::snprintf(b, sizeof b,
                  "search time over %u frames: avg %.3f ms, p99 %.3f, max %.3f (brute force avg %.3f ms); %.0f frames scored "
                  "per search; avg under 1 ms, p99 under 3",
                  db.FrameCount(), avg, Pct(ms, 0.99f), Pct(ms, 1.0f), bruteMs / std::max(1, compared), (double)scored / 1000.0);
    Check(avg < 1.0 && Pct(ms, 0.99f) < 3.0f, b);

    // --- Self match and determinism ---
    int selfMiss = 0, nondet = 0;
    for (int k = 0; k < 200; ++k) {
        const std::uint32_t f = searchable[pick(rng)];
        MotionSearchParams p;
        p.Forbid = MotionLabel::Airborne; // (the default: `searchable` passes it)
        const auto self = db.Normalized(f);
        const MotionSearchResult a = MotionSearch::Search(db, self.data(), p);
        if (a.Frame < 0 || a.Cost > 1e-6f) ++selfMiss;
        const MotionSearchResult c = MotionSearch::Search(db, self.data(), p);
        if (c.Frame != a.Frame || c.Cost != a.Cost) ++nondet;
    }
    Check(selfMiss == 0, "every searchable frame finds itself (cost 0): " + std::to_string(selfMiss) + " of 200 don't");
    Check(nondet == 0, "the same query gives the same frame: " + std::to_string(nondet) + " of 200 differ");

    // --- The gait table ---
    r.Gaits = GaitSpeedTable::Measure(db);
    std::printf("[MmDb] gait speed table (leg plateaus): %s\n", r.Gaits.Describe().c_str());
    const GaitSpeedTable& t = r.Gaits;
    std::snprintf(b, sizeof b, "gait speed table: crouch %.2f < walk %.2f < jog %.2f < run %.2f m/s, every gait measured (legs %d/%d/%d/%d)",
                  t.CrouchWalk, t.Walk, t.Jog, t.Run, t.Legs[3], t.Legs[0], t.Legs[1], t.Legs[2]);
    Check(t.Legs[0] > 0 && t.Legs[1] > 0 && t.Legs[2] > 0 && t.Legs[3] > 0 && t.CrouchWalk < t.Walk && t.Walk < t.Jog && t.Jog < t.Run, b);

    // --- Left/right coverage ---
    // Frames: per gait (crouch-walk apart), the searchable frames turning (the facing at +1 s over 15 deg off) or
    // strafing (the travel at +1 s over 50 deg off the facing, 0.3 m or more) each way. + = left (yaw grows to the left).
    const float rest = db.Stream().RestYaw;
    int turn[4][2] = {}, strafe[4][2] = {}, total[4] = {};
    for (std::uint32_t f : searchable) {
        const std::uint8_t fl = db.Flags(f);
        const int g = (fl & MotionLabel::Crouch) ? 3 : db.Gait(f) == MotionLabel::Idle ? -1 : db.Gait(f) - 1;
        if (g < 0 || ((fl & MotionLabel::Crouch) && db.Gait(f) == MotionLabel::Idle)) continue;
        const auto x = db.Raw(f);
        ++total[g];
        const float turnAng = MotionMath::WrapPi(std::atan2(x[25], x[26]) - rest);
        if (std::fabs(turnAng) > 15.0f * kDeg) ++turn[g][turnAng > 0 ? 0 : 1];
        const float px = x[19], pz = x[20];
        if (std::hypot(px, pz) > 0.3f) {
            const float travelAng = MotionMath::WrapPi(std::atan2(px, pz) - rest);
            if (std::fabs(travelAng) > 50.0f * kDeg && std::fabs(travelAng) < 130.0f * kDeg) ++strafe[g][travelAng > 0 ? 0 : 1];
        }
    }
    static const char* const kG[4] = {"walk", "jog", "run", "crouch"};
    std::printf("[MmDb] left/right frames (searchable, moving):       gait   frames   turn L / R (ratio)   strafe L / R (ratio)\n");
    bool balanced = true;
    for (int g = 0; g < 4; ++g) {
        const float tr = (float)std::min(turn[g][0], turn[g][1]) / (float)std::max(1, std::max(turn[g][0], turn[g][1]));
        const float sr = (float)std::min(strafe[g][0], strafe[g][1]) / (float)std::max(1, std::max(strafe[g][0], strafe[g][1]));
        std::printf("[MmDb]                                                   %-6s %6d   %6d / %-6d (%.2f)   %6d / %-6d (%.2f)\n", kG[g],
                    total[g], turn[g][0], turn[g][1], tr, strafe[g][0], strafe[g][1], sr);
        if (g != 2) balanced = balanced && tr >= 0.5f; // (run: reported; its turns are few either way)
    }
    Check(balanced, "walk, jog and crouch turn both ways about as often (the fewer side at least half the other)");

    // Queries: trajectory only (the pose groups weigh nothing), at the table's speed, the character at the origin facing
    // the rest yaw... any yaw: the features are in root space.
    std::printf("[MmDb] coverage queries: %-6s %-7s %4s %8s %8s %8s   %8s %8s %8s   %s\n", "gait", "case", "m/s", "L err m", "L deg", "L cost",
                "R err m", "R deg", "R cost", "matched L | R");
    struct Case { const char* Name; float TurnDeg, StrafeDeg; };
    const Case cases[] = {{"turn 45", 45, 0}, {"turn 90", 90, 0}, {"strafe", 0, 90}};
    const auto ts = db.TrajSeconds();
    bool covered = true, symmetric = true;
    float worstAsym = 0.0f;
    std::string worstAsymAt;
    for (int g = 0; g < 4; ++g) {
        const bool crouch = g == 3;
        const float forward = crouch ? t.CrouchWalk : g == 0 ? t.Walk : g == 1 ? t.Jog : t.Run;
        const float strafeV = t.StrafeFor(crouch ? MotionLabel::Walk : g + 1, crouch);
        MotionSearchParams p;
        p.GroupWeight[MotionFeature::FootPos] = p.GroupWeight[MotionFeature::FootVel] = p.GroupWeight[MotionFeature::PelvisVel] = 0.0f;
        if (crouch) p.Require = MotionLabel::Crouch;
        else p.Forbid = MotionLabel::Airborne | MotionLabel::Crouch;
        p.DesiredGait = crouch ? MotionLabel::Walk : g + 1;
        for (const Case& c : cases) {
            // Strafes at the pack's strafe speed (its strafes are slower than its forward gaits: GaitSpeedTable).
            const float v = c.StrafeDeg != 0.0f ? strafeV : forward;
            const float tol = 0.1f + 0.15f * v; // at +1 s: 10 cm plus 15 % of the second's travel
            float err[2], deg[2], cost[2];
            std::string clip[2];
            for (int side = 0; side < 2; ++side) {
                const float sgn = side == 0 ? 1.0f : -1.0f;
                MotionMatcher::Input in;
                in.Position = glm::vec3(0.0f);
                in.Yaw = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    // Integrate the heading over time (fine steps).
                    glm::vec3 pos(0.0f);
                    float heading = sgn * c.StrafeDeg * kDeg;
                    const int steps = 240;
                    const float dts = ts[(size_t)k] / (float)steps;
                    for (int s = 0; s < steps; ++s) {
                        pos += MotionMath::YawDir(heading + 0.5f * sgn * c.TurnDeg * kDeg * dts) * (v * dts);
                        heading += sgn * c.TurnDeg * kDeg * dts;
                    }
                    in.FuturePosition[(size_t)k] = pos;
                    in.FutureYaw[(size_t)k] = sgn * c.TurnDeg * kDeg * ts[(size_t)k];
                }
                float raw[27] = {}, q[27];
                for (int d = 0; d < 15; ++d) raw[d] = db.Mean(d);
                MotionMatcher::TrajectoryFeatures(in, rest, raw + 15);
                db.Normalize(raw, q);
                const MotionSearchResult res = MotionSearch::Search(db, q, p);
                if (res.Frame < 0) {
                    covered = false;
                    err[side] = deg[side] = 999.0f;
                    cost[side] = 0.0f;
                    clip[side] = "-";
                    continue;
                }
                TrajError(raw + 15, db.Raw((std::uint32_t)res.Frame).data() + 15, err[side], deg[side]);
                cost[side] = res.Cost;
                const std::uint32_t f = (std::uint32_t)res.Frame;
                char at[96];
                std::snprintf(at, sizeof at, "%s@%.1f", db.Clip(db.ClipOf(f)).Name.c_str(), db.SecondsInClip(f));
                clip[side] = at;
            }
            std::printf("[MmDb] coverage queries: %-6s %-7s %4.2f %8.2f %8.1f %8.2f   %8.2f %8.1f %8.2f   %s | %s\n", kG[g], c.Name, v, err[0],
                        deg[0], cost[0], err[1], deg[1], cost[1], clip[0].c_str(), clip[1].c_str());
            const bool core = g != 2;
            // Turns must match; strafes are reported (the crouch strafes are short: 8 legs in the pack) - but both are
            // held to left/right symmetry below.
            if (core && c.StrafeDeg == 0.0f && (err[0] > tol || err[1] > tol || deg[0] > 20.0f || deg[1] > 20.0f)) covered = false;
            const float asym = std::fabs(err[0] - err[1]);
            if (core && asym > worstAsym) { worstAsym = asym; worstAsymAt = std::string(kG[g]) + " " + c.Name; }
            if (core && asym > tol) symmetric = false;
        }
    }
    Check(covered, "walk, jog and crouch-walk find both left and right turns (45, 90 deg/s, the table's speed) within 10 cm + 15 % "
                   "of a second's travel at +1 s and 20 deg (strafes and run: reported)");
    std::snprintf(b, sizeof b, "left and right match about as well (turns and strafes, walk/jog/crouch): worst gap %.2f m (%s), "
                  "under 10 cm + 15 %% of a second's travel", worstAsym, worstAsymAt.c_str());
    Check(symmetric, b);
}

void NpcTest::MmDb(World& world) {
    if (!m_MmDb) m_MmDb = std::make_shared<MmDbRun>();
    MmDbRun& r = *m_MmDb;
    if (m_Done) return;
    const float dt = 1.0f / 60.0f; // the game's fixed step
    if (!r.Started) {
        r.Started = true;
        const std::string base = ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny");
        if (!std::filesystem::exists(base + ".mmanim") || !std::filesystem::exists(base + ".mmdb")) {
            std::printf("[NpcTest] SKIP mmdb: %s.mmanim / .mmdb not baked (--bake-motion)\n", base.c_str());
            r.Skipped = m_Done = true;
            return;
        }
        MotionBakeConfig cfg;
        std::string err;
        if (!cfg.Load(ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny.mmconfig"), err) ||
            !std::filesystem::exists(cfg.TargetRig) || !Assets) {
            std::printf("[NpcTest] SKIP mmdb: %s\n", err.empty() ? "the target rig is missing" : err.c_str());
            r.Skipped = m_Done = true;
            return;
        }
        const bool loaded = r.Db.Load(base, &err);
        char b[256];
        const double poseMB = (double)r.Db.PoseBytes() / 1048576.0, featMB = (double)r.Db.FeatureBytes() / 1048576.0;
        // (The caps are the bake's own: Manny.mmconfig's budgets.)
        std::snprintf(b, sizeof b, "the database loads within the budgets: %u frames, %zu clips, poses %.1f MB (%.0f), features "
                      "(16-bit, labels, boxes) %.1f MB (%.0f), %.2f s (%.1f)%s%s", r.Db.FrameCount(), r.Db.Stream().Clips().size(), poseMB,
                      cfg.BudgetPoseMB, featMB, cfg.BudgetFeatureMB, r.Db.LoadSeconds(), cfg.BudgetLoadSeconds, loaded ? "" : ": ",
                      loaded ? "" : err.c_str());
        Check(loaded && r.Db.LoadSeconds() < cfg.BudgetLoadSeconds && poseMB <= cfg.BudgetPoseMB && featMB <= cfg.BudgetFeatureMB, b);
        if (!loaded) { m_Done = true; return; }
        const auto v0 = std::chrono::steady_clock::now();
        MmDbOffline();
        std::printf("[MmDb] offline checks in %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - v0).count());

        r.Body = Assets->LoadModel(cfg.TargetRig);
        if (!r.Body || r.Body->NodeCount() == 0) {
            Check(false, "the Quantum body loads");
            m_Done = true;
            return;
        }
        r.Body = r.Body->CreateInstance();
        r.Body->BindLocalPose(r.BindPose);
        {
            std::vector<glm::mat4> g;
            MotionBake::RootSpaceGlobals(*r.Body, r.BindPose, -1, g);
            const int p = r.Body->NodeIndex("pelvis");
            r.Bind = r.Db.Stream().Bind(*r.Body, p >= 0 ? g[(size_t)p][3].y : 0.0f);
        }
        Check(r.Bind.Bound == (int)r.Db.Stream().Bones().size() && r.Bind.Pelvis >= 0, "the stream binds to the Quantum body");
        r.Entity = world.CreateModelEntity(r.Body, kStage, glm::vec3(0.0f), glm::vec3(1.0f), "[Runtime] MM Matcher Body");
        Assets->ApplyMaterialRemap(*r.Body, world.Registry.get<RenderableComponent>(r.Entity).Materials);

        // The simulated capsule: a sphere swept at knee height against the solid world.
        r.Sim.Sweep = [](const glm::vec3& from, const glm::vec3& to, glm::vec3& stopAt, glm::vec3& normal) {
            glm::vec3 d = to - from;
            d.y = 0.0f;
            const float len = glm::length(d);
            if (len < 1e-6f) return false;
            d /= len;
            const glm::vec3 o = from + glm::vec3(0.0f, kProbeHeight, 0.0f);
            QueryFilter f;
            f.HitTriggers = 0;
            RaycastHit hit;
            if (!PhysicsWorld::SphereCastSolid(&o.x, &d.x, kRadius, len, f, hit) || !hit.Hit) return false;
            stopAt = from + d * std::max(0.0f, hit.Distance - 0.01f);
            normal = glm::vec3(hit.Normal[0], 0.0f, hit.Normal[2]);
            return true;
        };
        r.Sim.Reset(kStage, r.Facing);
        // Start on an idle frame.
        std::uint32_t start = 0;
        const int idle = r.Db.Stream().FindClip("Stand_Idle_Neutral_Take001");
        if (idle >= 0) start = r.Db.Clip(idle).First + 60;
        r.Matcher.Reset(&r.Db, start);
        m_HaveCam = true;
    }
    if (r.Skipped || !r.Body) return;

    // --- The course ---
    if (r.Clock >= CourseSeconds()) {
        if (!r.Finished) {
            r.Finished = true;
            m_Done = true;
            const MotionMatcher& m = r.Matcher;
            char b[320];
            const float rate = (float)m.Searches / std::max(1e-3f, r.Clock);
            std::snprintf(b, sizeof b, "the matcher searched %d times in %.1f s (%.1f per second: every 0.1 s, %d on input changes, %d at "
                                       "a take's end), between 9.5 and 20 per second",
                          m.Searches, r.Clock, rate, m.Schedule.ForcedByInput, m.Schedule.ForcedByEnd);
            Check(rate >= 9.5f && rate <= 20.0f, b);
            std::snprintf(b, sizeof b, "live searches: avg %.3f ms, p99 %.3f, max %.3f; avg under 1 ms", m.SearchMsTotal / std::max(1, m.Searches),
                          Pct(m.SearchMs, 0.99f), m.SearchMsMax);
            Check(m.SearchMsTotal / std::max(1, m.Searches) < 1.0, b);
            float rawAvg = 0.0f, outAvg = 0.0f;
            for (float x : r.JumpRaw) rawAvg += x;
            for (float x : r.JumpOut) outAvg += x;
            rawAvg /= std::max<size_t>(1, r.JumpRaw.size());
            outAvg /= std::max<size_t>(1, r.JumpOut.size());
            std::snprintf(b, sizeof b, "jumps are blended: %d jumps; the pose's step at a jump avg %.1f deg (worst %.1f) against the raw "
                                       "jump's %.1f (worst %.1f) and a normal frame's p99 %.1f deg",
                          m.Jumps, outAvg / kDeg, Pct(r.JumpOut, 1.0f) / kDeg, rawAvg / kDeg, Pct(r.JumpRaw, 1.0f) / kDeg,
                          Pct(r.NonJumpStep, 0.99f) / kDeg);
            Check(m.Jumps > 10 && outAvg < 0.5f * rawAvg && Pct(r.JumpOut, 1.0f) < 1.5f * Pct(r.NonJumpStep, 0.99f) + 2.0f * kDeg, b);
            Check(r.Finite == r.Frames && r.Frames > 0, "every output pose is finite (" + std::to_string(r.Finite) + " of " + std::to_string(r.Frames) + ")");
            Check(r.CrouchWrong == 0, "crouched frames only while crouching, standing ones only while standing (" + std::to_string(r.CrouchWrong) + " wrong)");
            Check(r.AirborneMatched == 0, "never an airborne frame (" + std::to_string(r.AirborneMatched) + ")");
            // The trajectory error per segment.
            std::printf("[MmDb] %-22s %8s %6s %8s %8s %8s %8s   gait i/w/j/r\n", "segment", "searches", "jumps", "err p50", "err p90",
                        "deg p50", "deg p90");
            bool tracked = true;
            for (int s = 0; s < kSegments; ++s) {
                const auto& st = r.Stats[(size_t)s];
                std::printf("[MmDb] %-22s %8d %6d %8.2f %8.2f %8.1f %8.1f   %d/%d/%d/%d\n", kCourse[s].Name, st.Searches, st.Jumps, Pct(st.PosErr, 0.5f),
                            Pct(st.PosErr, 0.9f), Pct(st.FacingErr, 0.5f), Pct(st.FacingErr, 0.9f), st.GaitFrames[0], st.GaitFrames[1],
                            st.GaitFrames[2], st.GaitFrames[3]);
                const Segment& g = kCourse[s];
                const float v = g.StrafeDeg != 0.0f ? r.Gaits.StrafeFor(g.Gait, g.Crouch) : r.Gaits.SpeedFor(g.Gait, g.Crouch);
                if (g.Gait != MotionLabel::Run && !g.Wall && Pct(st.PosErr, 0.5f) > 0.15f + 0.2f * v) tracked = false;
            }
            Check(tracked, "the matched trajectory follows the desired one: each segment's median error at +1 s under 15 cm + 20 % of "
                           "a second's travel (run and the wall: reported)");
            std::snprintf(b, sizeof b, "the walk into the ledge slides along it: %d sweep hits, the 0.3 m sphere's worst %.3f m into the "
                          "ledge (negative: its closest gap), under 0.02", r.WallSegmentHits, r.WallWorstPenetration);
            Check(r.WallSegmentHits > 0 && r.WallWorstPenetration < 0.02f, b);
        }
        return;
    }

    float acc = 0.0f;
    int seg = 0;
    for (; seg < kSegments; ++seg) {
        if (r.Clock < acc + kCourse[seg].Seconds) break;
        acc += kCourse[seg].Seconds;
    }
    seg = std::min(seg, kSegments - 1);
    const Segment& g = kCourse[seg];
    if (seg != r.Seg) {
        r.Seg = seg;
        std::printf("[MmDb] %5.1f s  %s\n", r.Clock, g.Name);
    }
    if (g.Wall) { // home on kLedgeFrom, then walk at the face from there (obliquely: the walk slides along it)
        const glm::vec3 p = r.Sim.State().Position;
        if (!r.LedgeLeg && glm::length(glm::vec2(kLedgeFrom.x - p.x, kLedgeFrom.z - p.z)) < 0.5f) {
            r.LedgeLeg = true;
            r.Facing = MotionMath::YawOf(kLedgeAim - kLedgeFrom);
            std::printf("[MmDb] %5.1f s  at the ledge's approach, heading for its face\n", r.Clock);
        }
        if (!r.LedgeLeg) r.Facing = MotionMath::YawOf(kLedgeFrom - p);
    }
    if (g.Gait != MotionLabel::Idle) r.Facing += g.TurnDeg * kDeg * dt;
    const float speed = g.StrafeDeg != 0.0f ? r.Gaits.StrafeFor(g.Gait, g.Crouch) : r.Gaits.SpeedFor(g.Gait, g.Crouch);
    const float travel = r.Facing + g.StrafeDeg * kDeg;
    const glm::vec3 desiredVel = MotionMath::YawDir(travel) * speed;
    const int hitsBefore = r.Sim.Hits();
    r.Sim.Update(dt, desiredVel, r.Facing);
    if (g.Wall) r.WallSegmentHits += r.Sim.Hits() - hitsBefore;

    // The query: the predictor's future, the goal turning on as the course does.
    MotionMatcher::Input in;
    in.Position = r.Sim.State().Position;
    in.Yaw = r.Sim.State().Yaw;
    in.DesiredVelocity = desiredVel;
    in.DesiredYaw = r.Facing;
    const auto ts = r.Db.TrajSeconds();
    TrajectoryState future[3];
    r.Sim.Predict(desiredVel, r.Facing, ts.data(), 3, future, g.Gait != MotionLabel::Idle ? g.TurnDeg * kDeg : 0.0f);
    for (int k = 0; k < 3; ++k) {
        in.FuturePosition[(size_t)k] = future[k].Position;
        in.FutureYaw[(size_t)k] = future[k].Yaw;
    }
    MotionMatcher& m = r.Matcher;
    m.SearchParams = MotionSearchParams{};
    if (g.Crouch) m.SearchParams.Require = MotionLabel::Crouch;
    else m.SearchParams.Forbid = MotionLabel::Airborne | MotionLabel::Crouch;
    m.SearchParams.DesiredGait = g.Gait;
    const MotionPose before = m.Pose();
    m.Update(dt, in);

    // Stats.
    MmDbRun::SegStats& st = r.Stats[(size_t)seg];
    if (m.SearchedThisUpdate()) {
        ++st.Searches;
        float e, d;
        TrajError(m.LastQueryRaw() + 15, r.Db.Raw(m.Frame()).data() + 15, e, d);
        st.PosErr.push_back(e);
        st.FacingErr.push_back(d);
    }
    if (r.Frames > 0) {
        if (m.JumpedThisUpdate()) {
            ++st.Jumps;
            r.JumpRaw.push_back(PoseAngle(before, m.Target()));
            r.JumpOut.push_back(PoseAngle(before, m.Pose()));
        } else if (!m.Inertializer.Blending()) {
            r.NonJumpStep.push_back(PoseAngle(before, m.Pose()));
        }
    }
    const std::uint8_t flags = r.Db.Flags(m.Frame());
    ++st.GaitFrames[std::min<int>(3, r.Db.Gait(m.Frame()))];
    // (the first search after a filter change happens this very update: the schedule's filter key)
    if (((flags & MotionLabel::Crouch) != 0) != g.Crouch) ++r.CrouchWrong;
    if (flags & MotionLabel::Airborne) ++r.AirborneMatched;
    if (g.Wall) {
        // The ledge: x -8..-4, z 6..10 (1 m tall, over the probe). The swept sphere must stay out of it: its centre's
        // ground distance to the box no less than its radius (the corners included).
        const glm::vec3 p = r.Sim.State().Position;
        const float dx = std::max({-8.0f - p.x, 0.0f, p.x - (-4.0f)}), dz = std::max({6.0f - p.z, 0.0f, p.z - 10.0f});
        r.WallWorstPenetration = std::max(r.WallWorstPenetration, kRadius - std::hypot(dx, dz));
    }

    // The body.
    r.Pose = r.BindPose;
    MotionMatcher::WritePose(m.Pose(), r.Db.Stream(), r.Bind, r.Pose);
    r.Body->ApplyLocalPose(r.Pose);
    const TrajectoryState& s = r.Sim.State();
    world.SetWorldPose(r.Entity, s.Position, glm::angleAxis(s.Yaw - r.Db.Stream().RestYaw, glm::vec3(0.0f, 1.0f, 0.0f)));
    ++r.Frames;
    bool finite = true;
    for (const glm::quat& q : m.Pose().Rot) finite = finite && std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
    r.Finite += finite && std::isfinite(m.Pose().Pelvis.y) ? 1 : 0;

    // The views follow the body.
    r.Watch = s.Position + glm::vec3(0.0f, 0.9f, 0.0f);
    {
        const glm::vec3 look = s.Position + glm::vec3(0.0f, 0.75f, 0.0f);
        m_CamPos = look + glm::vec3(-3.5f, 2.6f, -4.0f);
        const glm::vec3 d = glm::normalize(look - m_CamPos);
        m_CamYaw = glm::degrees(std::atan2(d.z, d.x));
        m_CamPitch = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
    }

    // Debug draw.
    r.Lines.clear();
    const float rest = r.Db.Stream().RestYaw;
    {
        TrajectoryState path[31];
        float times[31];
        for (int k = 0; k < 31; ++k) times[k] = (float)(k + 1) / 30.0f;
        r.Sim.Predict(desiredVel, r.Facing, times, 31, path, g.Gait != MotionLabel::Idle ? g.TurnDeg * kDeg : 0.0f);
        MotionDebugDraw::Predicted(r.Lines, path, 31, glm::vec4(0.6f, 0.6f, 0.6f, 0.8f), 0.02f);
    }
    MotionDebugDraw::Trajectory(r.Lines, s.Position, s.Yaw, rest, m.LastQueryRaw() + 15, glm::vec4(1.0f, 0.85f, 0.1f, 1.0f), 0.04f);
    const auto matched = r.Db.Raw(m.Frame());
    MotionDebugDraw::Trajectory(r.Lines, s.Position, s.Yaw, rest, matched.data() + 15, glm::vec4(1.0f, 0.2f, 0.9f, 1.0f), 0.06f);
    MotionDebugDraw::Character(r.Lines, s.Position, s.Yaw, glm::vec4(0.2f, 1.0f, 0.3f, 1.0f));
    MotionDebugDraw::Feet(r.Lines, s.Position, s.Yaw, rest, matched.data(), flags);
    if (m.JumpedThisUpdate()) MotionDebugDraw::Line(r.Lines, s.Position + glm::vec3(0, 1.9f, 0), s.Position + glm::vec3(0, 2.2f, 0), glm::vec4(1, 0.2f, 0.9f, 1));

    if (m.JumpedThisUpdate() && st.Jumps <= 3)
        std::printf("[MmDb] %5.1f s  %s: jump to %s @ %.2f s (cost %.2f, playing %.2f)\n", r.Clock, g.Name,
                    r.Db.Clip(r.Db.ClipOf(m.Frame())).Name.c_str(), r.Db.SecondsInClip(m.Frame()), m.LastResult().Cost,
                    m.LastResult().CurrentCost);
    r.Clock += dt;
}
