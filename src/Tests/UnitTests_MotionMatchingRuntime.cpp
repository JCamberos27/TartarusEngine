// Motion matching runtime (docs/MOTION_MATCHING_HANDOFF.md, Phase 3) on a synthetic database: the box search against
// brute force, the label filters, deterministic ties, the search schedule, dead blending, the spring trajectory and its
// collision, the gait speed table, the query's root-space convention, and the matcher end to end. The last test loads
// the cooked Manny database and SKIPs (never FAILs) when it isn't baked (CI has no licensed assets).
#include "UnitTestSupport.h"

#include "../Game/MotionMatching/Inertialization.h"
#include "../Game/MotionMatching/MotionDatabase.h"
#include "../Game/MotionMatching/MotionGait.h"
#include "../Game/MotionMatching/MotionMatcher.h"
#include "../Game/MotionMatching/MotionMath.h"
#include "../Game/MotionMatching/MotionSearch.h"
#include "../Game/MotionMatching/RootSynth.h"
#include "../Game/MotionMatching/TrajectoryPredictor.h"
#include "../Game/FirstPersonBody.h"
#include "../Game/FirstPersonBodyMotion.h"
#include "../Game/Player.h"

#include "Core/Paths.h"

#include <glm/gtc/quaternion.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <random>

namespace {

using MotionMath::kPi;
constexpr float kFps = 60.0f;
constexpr float kRestYaw = 0.25f; // not zero, so the root-space convention is exercised

void Skip(const char* test, const std::string& why) { std::printf("[UnitTest] SKIP %s: %s\n", test, why.c_str()); }

// One synthetic take: speed(t) m/s along the root's travel direction, turning at yawRate(t) rad/s, the body facing
// `strafe` radians off the travel direction, pelvis at `pelvisY`; feet step at 1.6 Hz; two streamed bones swing with the
// stride. `airFrom..airTo` (frames) lifts the feet and pelvis (an airborne stretch), `proneFrom..proneTo` drops the pelvis.
struct TakeSpec {
    std::string Name, Tags;
    int Frames = 240;
    float Speed = 1.3f, YawRate = 0.0f, Strafe = 0.0f, PelvisY = 0.95f, Heading = 0.0f;
    float SpeedEnd = -1.0f; // ramps to this by the end (-1: constant)
    int AirFrom = -1, AirTo = -1, ProneFrom = -1, ProneTo = -1;
};

struct SynthDb {
    MotionClipStream Stream;
    MotionFeatureSet Feat;
};

SynthDb Build(const std::vector<TakeSpec>& takes, bool finish = true) {
    SynthDb s;
    s.Stream.Reset(kFps, {"pelvis", "thigh_l", "thigh_r"});
    s.Stream.SourcePelvisHeight = 0.95f;
    s.Stream.RestYaw = kRestYaw;
    MotionFeatureConfig cfg;
    for (const TakeSpec& t : takes) {
        RootTrack root;
        MotionClipFrames fr;
        fr.Fps = kFps;
        fr.RestYaw = kRestYaw;
        std::vector<MotionClipStream::RootSample> rs;
        std::vector<glm::vec3> pelvisLocal;
        std::vector<glm::quat> rot;
        glm::vec3 p(0.0f);
        float travel = t.Heading;
        for (int i = 0; i < t.Frames; ++i) {
            const float time = (float)i / kFps;
            const float u = (float)i / (float)std::max(1, t.Frames - 1);
            const float speed = t.SpeedEnd >= 0.0f ? t.Speed + (t.SpeedEnd - t.Speed) * u : t.Speed;
            if (i > 0) {
                travel += t.YawRate / kFps;
                p += MotionMath::YawDir(travel) * (speed / kFps);
            }
            const float yaw = travel + t.Strafe;
            root.Position.push_back(p);
            root.Yaw.push_back(yaw);
            const glm::mat4 R = RootSynth::RootMatrix(p, yaw, kRestYaw);
            const float phase = 2.0f * kPi * 1.6f * time;
            const bool air = i >= t.AirFrom && i < t.AirTo;
            const bool prone = i >= t.ProneFrom && i < t.ProneTo;
            const float lift = air ? 0.3f : 0.0f;
            const glm::vec3 pelvisL(0.0f, prone ? 0.1f : t.PelvisY + (air ? 0.1f : 0.0f), 0.0f);
            const glm::vec3 restFwd = MotionMath::YawDir(kRestYaw), restLeft = MotionMath::YawDir(kRestYaw + 0.5f * kPi);
            const float stride = speed > 0.1f ? 0.25f : 0.0f;
            const glm::vec3 footL = restLeft * 0.1f + restFwd * (stride * std::sin(phase)) +
                                    glm::vec3(0, 0.09f + lift + 0.08f * std::max(0.0f, std::cos(phase)) * (stride > 0 ? 1.0f : 0.0f), 0);
            const glm::vec3 footR = -restLeft * 0.1f - restFwd * (stride * std::sin(phase)) +
                                    glm::vec3(0, 0.09f + lift + 0.08f * std::max(0.0f, -std::cos(phase)) * (stride > 0 ? 1.0f : 0.0f), 0);
            fr.Pelvis.push_back(glm::vec3(R * glm::vec4(pelvisL, 1.0f)));
            fr.Foot[0].push_back(glm::vec3(R * glm::vec4(footL, 1.0f)));
            fr.Foot[1].push_back(glm::vec3(R * glm::vec4(footR, 1.0f)));
            rs.push_back({p, yaw});
            pelvisLocal.push_back(pelvisL);
            rot.push_back(glm::angleAxis(0.05f * std::sin(phase), glm::vec3(0, 1, 0)));
            rot.push_back(glm::angleAxis(0.4f * std::sin(phase), glm::vec3(1, 0, 0)));
            rot.push_back(glm::angleAxis(-0.4f * std::sin(phase), glm::vec3(1, 0, 0)));
        }
        s.Feat.AddClip(t.Name, fr, root, cfg, MotionClipIntent::FromTags(t.Tags));
        s.Stream.AddClip({t.Name, t.Tags, 0, 0, 0.0f}, rs, pelvisLocal, rot);
    }
    if (finish) s.Feat.Finish();
    return s;
}

std::vector<TakeSpec> StandardTakes() {
    std::vector<TakeSpec> t;
    t.push_back({"Walk_Straight", "walk;card", 300, 1.3f});
    t.push_back({"Walk_Left", "walk;circles;forward", 300, 1.3f, 0.6f});
    t.push_back({"Walk_Right", "walk;circles;forward", 300, 1.3f, -0.6f});
    t.push_back({"Walk_StrafeLeft", "walk;strafe", 240, 1.1f, 0.0f, -0.5f * kPi});
    t.push_back({"Walk_StrafeRight", "walk;strafe", 240, 1.1f, 0.0f, 0.5f * kPi});
    t.push_back({"Jog_Straight", "jog;card", 300, 2.0f});
    t.push_back({"Jog_Left", "jog;circles;forward", 300, 2.0f, 0.8f});
    t.push_back({"Run_Straight", "run;card", 300, 4.5f});
    t.push_back({"Crouch_Walk", "crouch;card", 300, 0.9f, 0.0f, 0.0f, 0.6f});
    t.push_back({"Stand_Idle", "stand;idle", 300, 0.0f});
    TakeSpec start{"Stand_to_Walk", "stand;start", 240, 0.0f};
    start.SpeedEnd = 1.3f;
    t.push_back(start);
    TakeSpec jump{"Stand_Jump", "stand;jump", 200, 0.0f};
    jump.AirFrom = 60;
    jump.AirTo = 90;
    t.push_back(jump);
    TakeSpec prone{"Transitions_Prone", "transitions", 200, 0.0f};
    prone.ProneFrom = 50;
    prone.ProneTo = 120;
    t.push_back(prone);
    return t;
}

bool Adopt(MotionDatabase& db, SynthDb&& s) {
    std::string err;
    const bool ok = db.Adopt(std::move(s.Stream), std::move(s.Feat), &err);
    if (!ok) std::printf("[MotionMatching] adopt failed: %s\n", err.c_str());
    return ok;
}

// The query a matcher would build at database frame `f` for its own future (its features, exactly).
void FrameQuery(const MotionDatabase& db, std::uint32_t f, float* q) {
    for (int d = 0; d < MotionDatabase::kDims; ++d) q[d] = db.Normalized(f)[d];
}

void TestDatabaseBuild() {
    MotionDatabase db;
    SynthDb src = Build(StandardTakes());
    std::vector<float> raw500(MotionDatabase::kDims);
    src.Feat.Decode(500, raw500.data());
    CHECK(Adopt(db, std::move(src)));
    CHECK(db.FrameCount() > 3000 && db.Stream().Clips().size() == 13);
    // Boxes cover every frame and bound it.
    CHECK(db.SmallBoxes().size() == (db.FrameCount() + 15) / 16 && db.LargeBoxes().size() == (db.FrameCount() + 63) / 64);
    bool inside = true;
    for (std::uint32_t i = 0; i < db.FrameCount(); ++i) {
        const auto& b = db.SmallBoxes()[i / 16];
        const auto& L = db.LargeBoxes()[i / 64];
        for (int d = 0; d < MotionDatabase::kDims; ++d) {
            const std::int16_t x = db.Quantized(i)[d];
            inside = inside && x >= b.Min[d] && x <= b.Max[d] && x >= L.Min[d] && x <= L.Max[d];
        }
        inside = inside && (b.FlagOr & db.Flags(i)) == db.Flags(i) && (b.FlagAnd & db.Flags(i)) == b.FlagAnd;
    }
    CHECK(inside);
    // The raw features come back from the normalized ones (the database keeps only those); one scale per group.
    const auto r = db.Raw(500);
    float worst = 0.0f;
    for (int d = 0; d < 27; ++d) worst = std::max(worst, std::fabs(r[(size_t)d] - raw500[(size_t)d]));
    CHECK(worst < 1e-4f);
    // The features are held once, as 16-bit values (no float copy).
    CHECK(db.Features().Bytes() == (size_t)db.FrameCount() * (MotionDatabase::kDims * 2 + 3));
    CHECK(db.Scale(15) == db.Scale(20) && db.Scale(0) == db.Scale(5));
    // A mismatched pair is refused.
    SynthDb a = Build(StandardTakes());
    std::vector<TakeSpec> fewer = StandardTakes();
    fewer.pop_back();
    SynthDb b = Build(fewer);
    MotionDatabase bad;
    std::string err;
    CHECK(!bad.Adopt(std::move(a.Stream), std::move(b.Feat), &err) && !bad.Loaded());
    // The labels the search depends on are there.
    int air = 0, excluded = 0, crouch = 0;
    for (std::uint32_t i = 0; i < db.FrameCount(); ++i) {
        air += db.Flags(i) & MotionLabel::Airborne ? 1 : 0;
        excluded += db.Flags(i) & MotionLabel::Excluded ? 1 : 0;
        crouch += db.Flags(i) & MotionLabel::Crouch ? 1 : 0;
    }
    CHECK(air > 10 && excluded > 50 && crouch > 200);
}

void TestSearchMatchesBruteForce() {
    MotionDatabase db;
    CHECK(Adopt(db, Build(StandardTakes())));
    std::mt19937 rng(1234);
    std::uniform_int_distribution<std::uint32_t> frame(0, db.FrameCount() - 1);
    std::normal_distribution<float> noise(0.0f, 0.3f);
    int mismatches = 0;
    std::uint64_t scoredBox = 0, scoredBrute = 0;
    for (int k = 0; k < 400; ++k) {
        float q[27];
        FrameQuery(db, frame(rng), q);
        for (float& x : q) x += noise(rng);
        MotionSearchParams p;
        switch (k % 5) {
            case 1: p.Require = MotionLabel::Crouch; break;
            case 2: p.Forbid = MotionLabel::Crouch | MotionLabel::Airborne; p.DesiredGait = MotionLabel::Jog; break;
            case 3: p.TransitionCost = 0.5f; break;
            case 4: p.Forbid = 0; p.Require = MotionLabel::Airborne; break;
            default: break;
        }
        const std::int64_t current = k % 2 ? (std::int64_t)frame(rng) : -1;
        const MotionSearchResult a = MotionSearch::Search(db, q, p, current);
        const MotionSearchResult b = MotionSearch::SearchBrute(db, q, p, current);
        if (a.Frame != b.Frame || a.Cost != b.Cost || a.Switched != b.Switched) ++mismatches;
        scoredBox += a.FramesScored;
        scoredBrute += b.FramesScored;
        if (a.Frame >= 0 && a.Switched) {
            CHECK(MotionSearch::Passes(db.Flags((std::uint32_t)a.Frame), p));
        }
    }
    std::printf("[MotionMatching] search vs brute force: %d mismatches in 400; frames scored %.0f vs %.0f per search\n", mismatches,
                (double)scoredBox / 400.0, (double)scoredBrute / 400.0);
    CHECK(mismatches == 0);
    CHECK(scoredBox * 2 < scoredBrute); // the boxes prune
}

// The 16-bit features: the search on them finds the float features' best frame, or one whose float cost is within the
// quantization's reach of it.
void TestQuantizedSearch() {
    SynthDb src = Build(StandardTakes(), false);
    std::vector<float> raw((size_t)src.Feat.FrameCount() * MotionDatabase::kDims);
    for (std::uint32_t f = 0; f < src.Feat.FrameCount(); ++f) src.Feat.Decode(f, raw.data() + (size_t)f * MotionDatabase::kDims);
    src.Feat.Finish();
    float worstQ = 0.0f;
    for (int d = 0; d < MotionDatabase::kDims; ++d) worstQ = std::max(worstQ, src.Feat.QError()[(size_t)d] / src.Feat.Std()[(size_t)d]);
    MotionDatabase db;
    CHECK(Adopt(db, std::move(src)));
    CHECK(worstQ < 1e-3f);
    std::mt19937 rng(99);
    std::uniform_int_distribution<std::uint32_t> frame(0, db.FrameCount() - 1);
    std::normal_distribution<float> noise(0.0f, 0.3f);
    int differ = 0;
    float worstGap = 0.0f;
    for (int k = 0; k < 300; ++k) {
        float q[27];
        FrameQuery(db, frame(rng), q);
        for (float& x : q) x += noise(rng);
        MotionSearchParams p;
        const MotionSearchResult r = MotionSearch::Search(db, q, p);
        // Float brute force on the exact features, the same filter and weights.
        float best = 1e30f, atQuant = 1e30f;
        std::int64_t bestAt = -1;
        for (std::uint32_t i = 0; i < db.FrameCount(); ++i) {
            if (!MotionSearch::Passes(db.Flags(i), p)) continue;
            float n[27], c = 0.0f;
            db.Normalize(raw.data() + (size_t)i * MotionDatabase::kDims, n);
            for (int d = 0; d < 27; ++d) c += p.GroupWeight[MotionFeature::GroupOf(d)] * (q[d] - n[d]) * (q[d] - n[d]);
            if (c < best) { best = c; bestAt = i; }
            if ((std::int64_t)i == r.Frame) atQuant = c;
        }
        differ += bestAt != r.Frame ? 1 : 0;
        worstGap = std::max(worstGap, atQuant - best);
    }
    std::printf("[MotionMatching] 16-bit features: worst error %.2e std; search vs float brute force: %d of 300 differ, worst float-cost gap %.2e\n",
                worstQ, differ, worstGap);
    CHECK(worstGap < 1e-3f);
}

// The style filter: asked for one style, only its frames are candidates - exactly as brute force.
void TestStyleSearch() {
    MotionDatabase db;
    CHECK(Adopt(db, Build(StandardTakes())));
    const int walk = db.Stream().FindClip("Walk_Straight"), jog = db.Stream().FindClip("Jog_Straight");
    CHECK(db.Style(db.Clip(walk).First + 50) == MotionLabel::StyleWalk && db.Style(db.Clip(jog).First + 50) == MotionLabel::StyleJog);
    CHECK(db.Style(db.Clip(db.Stream().FindClip("Stand_Idle")).First + 50) == MotionLabel::StyleStand);
    CHECK(db.Style(db.Clip(db.Stream().FindClip("Run_Straight")).First + 100) == MotionLabel::StyleRun);
    float q[27];
    FrameQuery(db, db.Clip(walk).First + 100, q);
    MotionSearchParams p;
    p.Forbid = MotionLabel::Crouch | MotionLabel::Airborne;
    MotionSearchResult r = MotionSearch::Search(db, q, p);
    CHECK(r.Frame >= 0 && db.Style((std::uint32_t)r.Frame) == MotionLabel::StyleWalk);
    p.DesiredStyle = MotionLabel::StyleJog;
    r = MotionSearch::Search(db, q, p);
    CHECK(r.Frame >= 0 && db.Style((std::uint32_t)r.Frame) == MotionLabel::StyleJog);
    CHECK(MotionSearch::Cost(db, q, db.Clip(walk).First + 100, p) == std::numeric_limits<float>::infinity());
    // ... exactly as brute force, for any style asked.
    std::mt19937 rng(5);
    std::uniform_int_distribution<std::uint32_t> frame(0, db.FrameCount() - 1);
    int mismatches = 0;
    for (int k = 0; k < 200; ++k) {
        FrameQuery(db, frame(rng), q);
        MotionSearchParams s;
        s.DesiredStyle = k % MotionLabel::StyleCount;
        const MotionSearchResult a = MotionSearch::Search(db, q, s), b = MotionSearch::SearchBrute(db, q, s);
        mismatches += a.Frame != b.Frame || a.Cost != b.Cost ? 1 : 0;
    }
    CHECK(mismatches == 0);
}

// The fingers dead-blend with the rest: from what was shown to the new clip's, over the blend time.
void TestFingersDeadBlend() {
    Inertialization in;
    in.Params.BlendTime = 0.2f;
    MotionPose a, b, target;
    a.Rot = b.Rot = target.Rot = {glm::quat(1, 0, 0, 0)};
    a.Fingers = {glm::angleAxis(0.30f, glm::vec3(0, 0, 1))};
    b.Fingers = {glm::angleAxis(0.31f, glm::vec3(0, 0, 1))};
    target.Fingers = {glm::angleAxis(-0.5f, glm::vec3(0, 0, 1))};
    in.Transition(a, b, 1.0f / 60.0f);
    MotionPose out;
    in.Update(1.0f / 60.0f, target, out);
    CHECK(out.Fingers.size() == 1 && MotionMath::QuatAngle(out.Fingers[0], b.Fingers[0]) < 0.05f); // starts where it was
    for (int k = 0; k < 20; ++k) in.Update(1.0f / 60.0f, target, out);
    CHECK(!in.Blending() && MotionMath::QuatAngle(out.Fingers[0], target.Fingers[0]) < 1e-5f);
    // The cache key: a load with a rig (its hands-up frames excluded) and one without are different databases.
    const std::string base = "assets/Animations/MotionMatching/Manny";
    CHECK(SharedMotionDatabase::CacheKey(base, false, "") != SharedMotionDatabase::CacheKey(base, true, "body.fbx"));
    CHECK(SharedMotionDatabase::CacheKey(base, true, "a.fbx") != SharedMotionDatabase::CacheKey(base, true, "b.fbx"));
    CHECK(SharedMotionDatabase::CacheKey(base, true, "a.fbx") == SharedMotionDatabase::CacheKey(base, true, "a.fbx"));
}

void TestSearchFiltersAndTies() {
    // Duplicate takes: identical frames in two clips.
    std::vector<TakeSpec> takes = StandardTakes();
    TakeSpec dup = takes[0];
    dup.Name = "Walk_Straight_Copy";
    takes.push_back(dup);
    MotionDatabase db;
    CHECK(Adopt(db, Build(takes)));
    const auto& copy = db.Clip((int)db.Stream().Clips().size() - 1);
    const auto& orig = db.Clip(0);
    float q[27];
    // A query equal to a frame of the copy finds a frame of the original (the lower indices: the copy's frames are
    // bit-identical to the original's, so only the visiting order can choose) - every time. (The cost is the query's round
    // trip through the 16-bit features' map: not quite 0.)
    FrameQuery(db, copy.First + 100, q);
    MotionSearchParams p;
    const MotionSearchResult a = MotionSearch::Search(db, q, p);
    CHECK(a.Frame >= (std::int64_t)orig.First && a.Frame < (std::int64_t)(orig.First + orig.Count) && a.Cost < 1e-6f);
    for (int k = 0; k < 5; ++k) CHECK(MotionSearch::Search(db, q, p).Frame == a.Frame);
    // Playing the copy's frame: it's as good as the original, so it stays (the playing frame wins a tie).
    const MotionSearchResult stay = MotionSearch::Search(db, q, p, copy.First + 100);
    CHECK(stay.Frame == (std::int64_t)copy.First + 100 && !stay.Switched && stay.CurrentCost < 1e-6f);
    // Excluded, NoSearch and (by default) Airborne frames are never matched, even by their own features.
    for (std::uint32_t i = 0; i < db.FrameCount(); i += 7) {
        const std::uint8_t f = db.Flags(i);
        if (!(f & (MotionLabel::Excluded | MotionLabel::NoSearch | MotionLabel::Airborne))) continue;
        FrameQuery(db, i, q);
        const MotionSearchResult r = MotionSearch::Search(db, q, p);
        CHECK(r.Frame >= 0 && !(db.Flags((std::uint32_t)r.Frame) & (MotionLabel::Excluded | MotionLabel::NoSearch | MotionLabel::Airborne)));
    }
    // Require Crouch: a standing walk's query still lands on a crouched frame; Forbid it: never.
    FrameQuery(db, orig.First + 50, q);
    p.Require = MotionLabel::Crouch;
    MotionSearchResult r = MotionSearch::Search(db, q, p);
    CHECK(r.Frame >= 0 && (db.Flags((std::uint32_t)r.Frame) & MotionLabel::Crouch));
    p.Require = 0;
    p.Forbid = MotionLabel::Crouch | MotionLabel::Airborne;
    const int crouchClip = db.Stream().FindClip("Crouch_Walk");
    FrameQuery(db, db.Clip(crouchClip).First + 50, q);
    r = MotionSearch::Search(db, q, p);
    CHECK(r.Frame >= 0 && !(db.Flags((std::uint32_t)r.Frame) & MotionLabel::Crouch));
    // The gait cost biases: a query halfway between walk and jog goes to the desired gait.
    float qw[27], qj[27];
    FrameQuery(db, orig.First + 100, qw);
    FrameQuery(db, db.Clip(db.Stream().FindClip("Jog_Straight")).First + 100, qj);
    for (int d = 0; d < 27; ++d) q[d] = 0.5f * (qw[d] + qj[d]);
    p = MotionSearchParams{};
    p.GaitWeight = 5.0f;
    p.DesiredGait = MotionLabel::Jog;
    r = MotionSearch::Search(db, q, p);
    CHECK(r.Frame >= 0 && db.Gait((std::uint32_t)r.Frame) == MotionLabel::Jog);
    p.DesiredGait = MotionLabel::Walk;
    r = MotionSearch::Search(db, q, p);
    CHECK(r.Frame >= 0 && db.Gait((std::uint32_t)r.Frame) == MotionLabel::Walk);
    // The playing clip's near frames are skipped (IgnoreNear), the playing frame itself competes.
    p = MotionSearchParams{};
    FrameQuery(db, orig.First + 101, q);
    r = MotionSearch::Search(db, q, p, orig.First + 100);
    CHECK(r.Frame < (std::int64_t)orig.First + 80 || r.Frame > (std::int64_t)orig.First + 120 || r.Frame == (std::int64_t)orig.First + 100);
}

void TestSearchSchedule() {
    MotionSearchSchedule s;
    const float dt = 1.0f / 60.0f;
    const glm::vec3 v(1.3f, 0.0f, 0.0f);
    int searches = 0;
    for (int i = 0; i < 600; ++i) searches += s.Tick(dt, v, 0.0f, 7, false) ? 1 : 0;
    CHECK(searches >= 99 && searches <= 101); // every 0.1 s (6 frames) for 10 s, plus the first
    CHECK(s.ForcedByInput == 0 && s.ForcedByEnd == 0);
    s.Tick(dt, v, 0.0f, 7, false);
    CHECK(s.Tick(dt, glm::vec3(0.0f, 0.0f, 2.0f), 0.0f, 7, false));          // the input changed: at once
    CHECK(!s.Tick(dt, glm::vec3(0.0f, 0.0f, 2.1f), 0.0f, 7, false));         // a small change: wait
    CHECK(s.Tick(dt, glm::vec3(0.0f, 0.0f, 2.1f), 0.6f, 7, false));          // facing 34 degrees: at once
    CHECK(s.Tick(dt, glm::vec3(0.0f, 0.0f, 2.1f), 0.6f, 8, false));          // the filter changed
    CHECK(s.Tick(dt, glm::vec3(0.0f, 0.0f, 2.1f), 0.6f, 8, true));           // out of frames
    CHECK(s.ForcedByInput == 3 && s.ForcedByEnd == 1);
}

float PoseAngle(const MotionPose& a, const MotionPose& b) {
    float w = 0.0f;
    for (size_t i = 0; i < a.Rot.size(); ++i) w = std::max(w, MotionMath::QuatAngle(a.Rot[i], b.Rot[i]));
    return w;
}

void TestInertialization() {
    namespace IM = InertializationMath;
    // Scaled angle-axis round trip, both ways round.
    const glm::vec3 v(0.3f, -1.2f, 0.7f);
    CHECK(glm::length(IM::ToScaledAngleAxis(IM::FromScaledAngleAxis(v)) - v) < 1e-5f);
    const glm::quat qa = glm::angleAxis(0.4f, glm::vec3(0, 1, 0)), qb = glm::angleAxis(0.7f, glm::vec3(0, 1, 0));
    CHECK(glm::length(IM::AngularVelocity(qa, qb, 0.1f) - glm::vec3(0, 3.0f, 0)) < 1e-4f);
    CHECK(glm::length(IM::AngularVelocity(qa, -qb, 0.1f) - glm::vec3(0, 3.0f, 0)) < 1e-4f); // the sign of q doesn't matter

    // The source spins bone 0 at 3 rad/s about Y and slides the pelvis at 1 m/s; the target is a fixed, different pose.
    const float dt = 1.0f / 60.0f;
    MotionPose prev, last, target;
    prev.Rot = {glm::angleAxis(0.0f, glm::vec3(0, 1, 0)), glm::quat(1, 0, 0, 0)};
    last.Rot = {glm::angleAxis(3.0f * dt, glm::vec3(0, 1, 0)), glm::quat(1, 0, 0, 0)};
    prev.Pelvis = glm::vec3(0.0f, 0.95f, 0.0f);
    last.Pelvis = glm::vec3(dt, 0.95f, 0.0f);
    target.Rot = {glm::angleAxis(-1.0f, glm::vec3(0, 1, 0)), glm::angleAxis(0.8f, glm::vec3(1, 0, 0))};
    target.Pelvis = glm::vec3(0.3f, 0.9f, 0.1f);
    Inertialization in;
    in.Transition(prev, last, dt);
    CHECK(in.Blending());
    MotionPose out;
    in.Update(dt, target, out);
    // Continuity: the first output is where the source was heading (its velocity carried on), not the target.
    const float step0 = MotionMath::QuatAngle(last.Rot[0], out.Rot[0]);
    std::printf("[MotionMatching] dead blend: first step %.4f rad (source velocity %.4f), target %.2f rad away\n", step0, 3.0f * dt,
                MotionMath::QuatAngle(out.Rot[0], target.Rot[0]));
    CHECK(step0 < 3.0f * dt * 1.6f);
    // (the pelvis carries on at its 1 m/s and starts 2 % of the way to the target: the first blend weight)
    CHECK(glm::length(out.Pelvis - last.Pelvis) < 1.0f * dt + 0.05f * glm::length(target.Pelvis - last.Pelvis));
    // It ends exactly on the target after BlendTime, with no pops on the way (no step over 4x the largest of the
    // source's own step and the even share of the gap).
    float worstStep = 0.0f;
    MotionPose before = out;
    int frames = 1;
    while (in.Blending() && frames < 1000) {
        in.Update(dt, target, out);
        worstStep = std::max(worstStep, PoseAngle(before, out));
        before = out;
        ++frames;
    }
    const float share = 1.4f / (in.Params.BlendTime / dt); // the largest gap (bone 0 ~1.05 rad, bone 1 0.8) over the frames
    const int expect = (int)std::lround(in.Params.BlendTime / dt);
    CHECK(frames == expect || frames == expect + 1); // (float time sums)
    CHECK(PoseAngle(out, target) < 1e-5f && glm::length(out.Pelvis - target.Pelvis) < 1e-5f);
    CHECK(worstStep < 4.0f * std::max(share, 3.0f * dt));
    // No blend running: the target passes through.
    MotionPose t2 = target;
    t2.Rot[0] = glm::angleAxis(0.2f, glm::vec3(1, 0, 0));
    in.Update(dt, t2, out);
    CHECK(PoseAngle(out, t2) == 0.0f);
    // A second jump mid-blend continues from the screen: the output keeps the step it was taking (no pop).
    in.Transition(last, target, dt);
    in.Update(dt, t2, out);
    MotionPose mid = out;
    in.Transition(before, mid, dt);
    in.Update(dt, last, out);
    std::printf("[MotionMatching] dead blend: chained jump step %.3f rad after a %.3f rad step\n", PoseAngle(mid, out),
                PoseAngle(before, mid));
    CHECK(PoseAngle(mid, out) < 1.5f * PoseAngle(before, mid) + 0.03f);
    // A wild source velocity is clamped.
    MotionPose spin = prev;
    spin.Rot[0] = glm::angleAxis(3.0f, glm::vec3(0, 1, 0));
    in.Transition(prev, spin, dt);
    in.Update(dt, spin, out);
    CHECK(MotionMath::QuatAngle(out.Rot[0], spin.Rot[0]) <= in.Params.MaxAngularVelocity * dt + 1e-3f);
}

void TestTrajectoryPredictor() {
    TrajectoryPredictor tp;
    tp.Reset(glm::vec3(0.0f), 0.0f);
    const glm::vec3 goal(1.3f, 0.0f, 0.0f);
    const float dt = 1.0f / 60.0f;
    // Prediction == simulation (the closed form at any step).
    const float times[3] = {1.0f / 3.0f, 2.0f / 3.0f, 1.0f};
    TrajectoryState pred[3];
    tp.Predict(goal, 1.0f, times, 3, pred);
    for (int i = 1; i <= 60; ++i) {
        tp.Update(dt, goal, 1.0f);
        if (i == 20) CHECK(glm::length(tp.State().Position - pred[0].Position) < 1e-3f);
        if (i == 40) CHECK(glm::length(tp.State().Position - pred[1].Position) < 1e-3f);
    }
    CHECK(glm::length(tp.State().Position - pred[2].Position) < 1e-3f);
    CHECK(std::fabs(tp.State().Yaw - pred[2].Yaw) < 1e-3f);
    // It reaches the goal velocity and facing (critically damped: no overshoot).
    float peak = 0.0f;
    for (int i = 0; i < 120; ++i) {
        tp.Update(dt, goal, 1.0f);
        peak = std::max(peak, tp.State().Velocity.x);
    }
    CHECK(std::fabs(tp.State().Velocity.x - 1.3f) < 0.01f && peak < 1.3f + 1e-3f);
    CHECK(std::fabs(tp.State().Yaw - 1.0f) < 0.01f);
    // Facing goes the short way across +-pi.
    tp.Reset(glm::vec3(0.0f), 3.0f);
    for (int i = 0; i < 120; ++i) tp.Update(dt, glm::vec3(0.0f), -3.0f);
    CHECK(std::fabs(MotionMath::WrapPi(tp.State().Yaw - (-3.0f))) < 0.01f && tp.State().Yaw > 3.0f);

    // A wall at x = 1 (a 0.3 m radius character stops at 0.7): the prediction runs along it, the character slides.
    TrajectoryPredictor w;
    w.Sweep = [](const glm::vec3& from, const glm::vec3& to, glm::vec3& stop, glm::vec3& n) {
        const float wall = 0.7f;
        if (to.x <= wall) return false;
        const float t = from.x >= wall ? 0.0f : (wall - from.x) / (to.x - from.x);
        stop = from + (to - from) * t - glm::vec3(1e-4f, 0, 0);
        n = glm::vec3(-1.0f, 0.0f, 0.0f);
        return true;
    };
    w.Reset(glm::vec3(0.0f), 0.0f);
    const glm::vec3 diag(2.0f, 0.0f, 1.0f);
    const float ft[3] = {0.5f, 1.0f, 2.0f};
    TrajectoryState fp[3];
    w.Predict(diag, 0.0f, ft, 3, fp);
    CHECK(fp[2].Position.x <= 0.7f && fp[2].Position.z > fp[1].Position.z && fp[1].Position.z > fp[0].Position.z);
    CHECK(fp[2].Velocity.x <= 1e-3f && fp[2].Velocity.z > 0.9f); // into the wall: gone; along it: kept
    for (int i = 0; i < 180; ++i) w.Update(dt, diag, 0.0f);
    CHECK(w.State().Position.x <= 0.7f && w.State().Position.z > 2.0f && w.Hits() > 0);
}

void TestGaitTable() {
    MotionDatabase db;
    CHECK(Adopt(db, Build(StandardTakes())));
    const GaitSpeedTable t = GaitSpeedTable::Measure(db);
    std::printf("[MotionMatching] synthetic gait table: %s\n", t.Describe().c_str());
    CHECK(std::fabs(t.Walk - 1.3f) < 0.03f && std::fabs(t.Jog - 2.0f) < 0.04f && std::fabs(t.Run - 4.5f) < 0.09f);
    CHECK(std::fabs(t.CrouchWalk - 0.9f) < 0.03f);
    CHECK(t.Legs[0] == 1 && t.Legs[1] == 1 && t.Legs[2] == 1 && t.Legs[3] == 1); // the circles turn too fast to be legs
    CHECK(t.GaitFor(0.1f, false) == MotionLabel::Idle && t.GaitFor(1.3f, false) == MotionLabel::Walk);
    CHECK(t.GaitFor(2.1f, false) == MotionLabel::Jog && t.GaitFor(4.0f, false) == MotionLabel::Run);
    CHECK(t.SpeedFor(MotionLabel::Jog, false) == t.Jog && t.SpeedFor(MotionLabel::Walk, true) == t.CrouchWalk);
    CHECK(t.SpeedFor(MotionLabel::Idle, false) == 0.0f);
    CHECK(std::fabs(t.Strafe[0] - 1.1f) < 0.03f && t.StrafeLegs[0] == 2 && t.StrafeLegs[2] == 0); // the two walk strafes
    CHECK(t.StrafeFor(MotionLabel::Run, false) == t.Strafe[0]); // no run (or jog) strafes: the walk's
    CHECK(std::fabs(t.StrafeFor(MotionLabel::Walk, true) - 0.75f * t.CrouchWalk) < 1e-6f); // no crouch strafe takes
}

// A frame's own future, as a matcher's Input, gives back exactly the frame's trajectory features (the query and the
// bake agree on root space, with a non-zero rest yaw).
void TestQueryConvention() {
    MotionDatabase db;
    CHECK(Adopt(db, Build(StandardTakes())));
    const int clip = db.Stream().FindClip("Walk_Left");
    const auto& c = db.Clip(clip);
    float worst = 0.0f;
    for (std::uint32_t f = c.First; f + 60 < c.First + c.Count; f += 17) {
        MotionMatcher::Input in;
        in.Position = db.Stream().Root(f).Position;
        in.Yaw = db.Stream().Root(f).Yaw;
        for (int k = 0; k < 3; ++k) {
            const auto& r = db.Stream().Root(f + (std::uint32_t)db.Features().TrajFrames[k]);
            in.FuturePosition[(size_t)k] = r.Position;
            in.FutureYaw[(size_t)k] = r.Yaw;
        }
        float t[12];
        MotionMatcher::TrajectoryFeatures(in, db.Stream().RestYaw, t);
        const auto raw = db.Raw(f);
        for (int d = 0; d < 12; ++d) worst = std::max(worst, std::fabs(t[d] - raw[(size_t)(15 + d)]));
    }
    CHECK(worst < 1e-4f);
}

// The matcher on the synthetic database, fed the trajectory of a walk that turns left after 2 s.
void TestMatcherEndToEnd() {
    MotionDatabase db;
    CHECK(Adopt(db, Build(StandardTakes())));
    MotionMatcher m;
    m.Reset(&db, db.Clip(db.Stream().FindClip("Stand_Idle")).First + 10);
    TrajectoryPredictor tp;
    tp.Reset(glm::vec3(0.0f), 0.0f);
    const float dt = 1.0f / 60.0f;
    const auto ts = db.TrajSeconds();
    int leftFrames = 0, straightFrames = 0, finite = 0;
    float worstStep = 0.0f;
    MotionPose prev;
    for (int i = 0; i < 300; ++i) {
        const float time = (float)i * dt;
        const float yaw = time < 2.0f ? 0.0f : 0.6f * (time - 2.0f);
        const glm::vec3 vel = MotionMath::YawDir(yaw) * 1.3f;
        tp.Update(dt, vel, yaw);
        // The desired trajectory: the predictor, with the goal turning on as the planner would.
        MotionMatcher::Input in;
        in.Position = tp.State().Position;
        in.Yaw = tp.State().Yaw;
        in.DesiredVelocity = vel;
        in.DesiredYaw = yaw;
        for (int k = 0; k < 3; ++k) {
            const float fy = time < 2.0f ? 0.0f : yaw + 0.6f * ts[(size_t)k];
            TrajectoryState s;
            tp.Predict(MotionMath::YawDir(fy) * 1.3f, fy, &ts[(size_t)k], 1, &s);
            in.FuturePosition[(size_t)k] = s.Position;
            in.FutureYaw[(size_t)k] = s.Yaw;
        }
        m.SearchParams.DesiredGait = MotionLabel::Walk;
        m.SearchParams.Forbid = MotionLabel::Airborne | MotionLabel::Crouch; // standing
        m.Update(dt, in);
        const std::string& name = db.Clip(db.ClipOf(m.Frame())).Name;
        if (time > 3.0f && name == "Walk_Left") ++leftFrames;
        if (time > 1.0f && time < 2.0f && (name == "Walk_Straight" || name == "Stand_to_Walk")) ++straightFrames;
        bool ok = true;
        for (const glm::quat& q : m.Pose().Rot) ok = ok && std::isfinite(q.w) && std::isfinite(q.x);
        finite += ok ? 1 : 0;
        if (i > 0) worstStep = std::max(worstStep, PoseAngle(prev, m.Pose()));
        prev = m.Pose();
    }
    std::printf("[MotionMatching] matcher: %d searches, %d jumps, left %d / 120 frames, straight %d / 60, worst pose step %.3f rad\n",
                m.Searches, m.Jumps, leftFrames, straightFrames, worstStep);
    CHECK(finite == 300);
    CHECK(m.Searches >= 50 && m.Searches <= 70); // 10 per second, plus input changes
    CHECK(m.Jumps >= 1);
    CHECK(leftFrames > 90 && straightFrames > 40);
    CHECK(worstStep < 0.25f); // the jumps are blended (the synthetic legs swing 0.4 rad each way)
}

// Phase 4's hooks: the matcher's play rate (speed warping), its locked playback with a hold frame and a forced jump (the
// airborne segment), a frame's own root speed, and the player's per-gait speeds by direction.
void TestPlayerHooks() {
    MotionDatabase db;
    CHECK(Adopt(db, Build(StandardTakes())));
    const auto& walk = db.Clip(db.Stream().FindClip("Walk_Straight"));
    const float dt = 1.0f / 60.0f;
    MotionMatcher::Input in;
    MotionMatcher m;
    m.Reset(&db, walk.First + 10);
    m.Locked = true;
    m.PlayRate = 2.0f;
    m.Update(dt, in); // the first update poses where it is
    for (int i = 0; i < 30; ++i) m.Update(dt, in);
    CHECK(std::abs((int)m.Frame() - (int)(walk.First + 70)) <= 1); // 30 updates at twice the rate: 60 frames on
    CHECK(m.Searches == 0);              // locked: never searched
    m.PlayRate = 1.0f;
    m.HoldFrame = walk.First + 80;
    for (int i = 0; i < 30; ++i) m.Update(dt, in);
    CHECK(m.Frame() == walk.First + 80); // held there
    m.ForceFrame(walk.First + 150);
    m.Update(dt, in);
    CHECK(m.Jumps == 1 && m.JumpedThisUpdate() == false && m.Inertializer.Blending());
    m.Locked = false;
    m.HoldFrame = -1;
    m.SearchParams.Forbid = MotionLabel::Airborne | MotionLabel::Crouch;
    m.Update(dt, in);
    CHECK(m.Searches == 1); // unlocked: the schedule searches at once
    const float speed = FirstPersonBodyMotion::RootSpeed(db, walk.First + 120);
    CHECK(std::fabs(speed - 1.3f) < 0.05f);
    CHECK(std::fabs(FirstPersonBodyMotion::RootYawRate(db, walk.First + 120)) < 1e-3f);

    Player::DirectionalSpeeds g;
    g.Active = true;
    g.Forward[0] = 1.27f; g.Strafe[0] = 1.14f;
    g.Forward[1] = 1.85f; g.Strafe[1] = 1.32f; // Shift: the jog
    g.Forward[2] = 0.79f; g.Strafe[2] = 0.76f;
    CHECK(std::fabs(PlayerGaitSpeed(g, {0.0f, 1.0f}, 0) - 1.27f) < 1e-4f);  // ahead
    CHECK(std::fabs(PlayerGaitSpeed(g, {1.0f, 0.0f}, 1) - 1.32f) < 1e-4f);  // the jog strafe
    CHECK(std::fabs(PlayerGaitSpeed(g, {0.0f, -1.0f}, 2) - 0.76f) < 1e-4f); // backwards: a strafe
    const float diag = PlayerGaitSpeed(g, {std::sin(0.4363f), std::cos(0.4363f)}, 1); // 25 degrees off: halfway
    CHECK(std::fabs(diag - 0.5f * (1.85f + 1.32f)) < 0.01f);
    CHECK(std::fabs(PlayerGaitSpeed(g, {0.0f, 0.0f}, 0) - 1.27f) < 1e-4f);
    // Backwards apart from sideways: straight back is Back, sideways still Strafe, between a smoothstep.
    g.Back[0] = 1.06f;
    g.Strafe[0] = 0.90f;
    CHECK(std::fabs(PlayerGaitSpeed(g, {0.0f, -1.0f}, 0) - 1.06f) < 1e-4f);
    CHECK(std::fabs(PlayerGaitSpeed(g, {1.0f, 0.0f}, 0) - 0.90f) < 1e-4f);
    const float backDiag = PlayerGaitSpeed(g, {std::sin(2.356f), std::cos(2.356f)}, 0); // 135 degrees off
    CHECK(std::fabs(backDiag - 0.98f) < 0.01f);
}

// The facing model (Stage 2): the character facing turns by the clip's own root turn, is pulled toward the simulation facing
// and is clamped off it; the simulation facing is a spring toward the facing wanted that nothing of the character feeds.
void TestFacingModel() {
    FirstPersonBodyMotion::Settings cfg;
    const float dt = 1.0f / 60.0f;
    // Converges: no clip turn, the character starts 34 deg off the simulation and settles onto it.
    float yaw = 0.6f;
    for (int i = 0; i < 120; ++i) yaw = FirstPersonBodyMotion::StepCharacterFacing(yaw, 0.0f, 0.0f, cfg, dt); // (10 half-lives)
    CHECK(std::fabs(yaw) < 0.01f);
    // The clamp holds: a clip turning hard one way never takes the character past MaxFacingOffset.
    yaw = 0.0f;
    float worst = 0.0f;
    for (int i = 0; i < 60; ++i) {
        yaw = FirstPersonBodyMotion::StepCharacterFacing(yaw, 0.2f, 0.0f, cfg, dt);
        worst = std::max(worst, std::fabs(yaw));
    }
    CHECK(worst <= cfg.MaxFacingOffset + 1e-5f);
    // A clip turning with the simulation leaves no lag: the simulation turning at 2 rad/s, the clip turning the same.
    yaw = 0.0f;
    float sim = 0.0f;
    for (int i = 0; i < 120; ++i) {
        sim += 2.0f * dt;
        yaw = FirstPersonBodyMotion::StepCharacterFacing(yaw, 2.0f * dt, MotionMath::WrapPi(sim), cfg, dt);
    }
    CHECK(std::fabs(MotionMath::WrapPi(yaw - sim)) < 1e-3f);
    // No feedback: the simulation facing is a spring toward the facing wanted alone - whatever the character's clips do.
    float s0 = 0.0f, r0 = 0.0f, s1 = 0.0f, r1 = 0.0f, c = 0.0f;
    for (int i = 0; i < 90; ++i) {
        TrajectoryPredictor::SpringScalar(s0, r0, 1.5f, cfg.SimFacingHalflife, dt);
        c = FirstPersonBodyMotion::StepCharacterFacing(c, (i % 2 ? 0.3f : -0.3f), s1, cfg, dt); // a wildly turning clip
        TrajectoryPredictor::SpringScalar(s1, r1, 1.5f, cfg.SimFacingHalflife, dt);
    }
    CHECK(s0 == s1 && r0 == r1);
    CHECK(std::fabs(s0 - 1.5f) < 0.01f);
}

// In Play the frame time varies (the npc tests run a fixed 1/60 s). A jump's dead blend must carry the velocity the output
// had - its last two poses over the time between them - whatever this update's dt is: right after a frame-time change it
// used this update's dt, scaling the carried velocity by their ratio (a hitch's long frame then a short one: 8x, into the
// clamp - a lurch at the jump; the other way the limb stalled). The output's angular velocity is checked across a forced
// jump to nearly the same pose (the frame 2 on, so the blend's pull toward it is small) for both orders.
void TestDeadBlendFrameTime() {
    MotionDatabase db;
    CHECK(Adopt(db, Build(StandardTakes())));
    const auto& walk = db.Clip(db.Stream().FindClip("Walk_Straight"));
    const float dtLong = 1.0f / 30.0f, dtShort = 1.0f / 240.0f;
    MotionMatcher::Input in;
    const auto speedOf = [](const MotionPose& a, const MotionPose& b, float dt) { // the thighs' (bones 1, 2) faster angular speed
        return std::max(MotionMath::QuatAngle(a.Rot[1], b.Rot[1]), MotionMath::QuatAngle(a.Rot[2], b.Rot[2])) / dt;
    };
    for (int order = 0; order < 2; ++order) {
        const float before = order == 0 ? dtShort : dtLong, after = order == 0 ? dtLong : dtShort;
        // Mid-swing (the synthetic thighs peak at 0.4 x 2 pi x 1.6 = 4 rad/s). (Before the fix the first order carried 1/8
        // of the velocity - the limb stalled, ~0.2 of it after the jump - and the second order 8x, into the 4 pi clamp.)
        MotionMatcher m;
        m.Reset(&db, walk.First + 60);
        m.Locked = true;
        m.Update(dtLong, in);
        for (int i = 0; i < 4; ++i) m.Update(dtLong, in);
        MotionPose prev = m.Pose();
        m.Update(before, in);
        const MotionPose last = m.Pose();
        const float vBefore = speedOf(prev, last, before);
        m.ForceFrame(m.Frame() + 2);
        m.Update(after, in);
        const float vAfter = speedOf(last, m.Pose(), after);
        std::printf("[MotionMatching] dead blend over a frame-time change (%.4f s then %.4f s): %.2f rad/s before the jump, %.2f after\n",
                    before, after, vBefore, vAfter);
        CHECK(vBefore > 1.0f); // (moving: a carried velocity to check)
        // (The source's velocity decays over the update - half-life 0.05 s: ~0.8 of it over a 1/30 s update.)
        CHECK(vAfter > 0.65f * vBefore && vAfter < 1.25f * vBefore);
    }
}

// Play test 4: the reselect history (MotionSearchParams::Avoid) is never matched - not even a frame's own self-match - and
// the box search still equals brute force with it; a held schedule (standing idle) never searches on the timer, but still on
// an input change and when the frame runs out.
void TestReselectAndIdleHold() {
    MotionDatabase db;
    CHECK(Adopt(db, Build(StandardTakes())));
    const auto& walk = db.Clip(db.Stream().FindClip("Walk_Straight"));
    int ok = 0;
    for (std::uint32_t f = walk.First + 40; f < walk.First + 200; f += 20) {
        float q[27];
        FrameQuery(db, f, q);
        MotionSearchParams p;
        p.Avoid[0] = (std::int64_t)f - 30;
        p.Avoid[1] = (std::int64_t)f + 30;
        p.AvoidCount = 1;
        const MotionSearchResult a = MotionSearch::Search(db, q, p), b = MotionSearch::SearchBrute(db, q, p);
        const bool outside = a.Frame < p.Avoid[0] || a.Frame > p.Avoid[1];
        // (Unavoided it finds an exact match: the frame, or a stride of the synthetic walk 75 frames off whose 16-bit features
        // are the same.)
        const MotionSearchResult unavoided = MotionSearch::Search(db, q, MotionSearchParams{});
        ok += outside && a.Frame == b.Frame && a.Cost == b.Cost && unavoided.Cost < 1e-6f && (unavoided.Frame - (std::int64_t)f) % 75 == 0;
    }
    CHECK(ok == 8);
    MotionSearchSchedule s;
    const float dt = 1.0f / 60.0f;
    CHECK(s.Tick(dt, glm::vec3(0.0f), 0.0f, 1, false)); // the first update searches
    s.Hold = true;
    int timer = 0;
    for (int i = 0; i < 120; ++i) timer += s.Tick(dt, glm::vec3(0.0f), 0.0f, 1, false);
    CHECK(timer == 0);                                       // 2 s held: no timer searches
    CHECK(s.Tick(dt, glm::vec3(0.0f), 0.0f, 1, true));       // the frame running out
    CHECK(s.Tick(dt, glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, 1, false)); // an input change
    s.Hold = false;
    for (int i = 0; i < 6; ++i) timer += s.Tick(dt, glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, 1, false);
    CHECK(timer == 1); // released: the timer again (0.1 s)
}

// The cooked Manny database: the boxes agree with brute force and search fast. SKIPs without a bake.
void TestCookedDatabaseSearch() {
    const char* name = "Motion matching: cooked database search";
    const std::string base = ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny");
    if (!std::filesystem::exists(base + ".mmanim") || !std::filesystem::exists(base + ".mmdb"))
        return Skip(name, "Manny.mmanim / .mmdb not baked");
    MotionDatabase db;
    std::string err;
    CHECK(db.Load(base, &err));
    if (!db.Loaded()) return;
    std::mt19937 rng(77);
    std::uniform_int_distribution<std::uint32_t> frame(0, db.FrameCount() - 1);
    std::normal_distribution<float> noise(0.0f, 0.2f);
    int mismatches = 0;
    double boxMs = 0.0, bruteMs = 0.0;
    for (int k = 0; k < 40; ++k) {
        float q[27];
        FrameQuery(db, frame(rng), q);
        for (float& x : q) x += noise(rng);
        MotionSearchParams p;
        if (k % 3 == 1) p.Forbid |= MotionLabel::Crouch;
        if (k % 3 == 2) p.DesiredGait = MotionLabel::Walk;
        const auto t0 = std::chrono::steady_clock::now();
        const MotionSearchResult a = MotionSearch::Search(db, q, p);
        const auto t1 = std::chrono::steady_clock::now();
        const MotionSearchResult b = MotionSearch::SearchBrute(db, q, p);
        const auto t2 = std::chrono::steady_clock::now();
        boxMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        bruteMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
        if (a.Frame != b.Frame || a.Cost != b.Cost) ++mismatches;
    }
    std::printf("[MotionMatching] cooked search: %u frames, %d mismatches in 40, avg %.3f ms (brute %.3f ms), load %.2f s\n",
                db.FrameCount(), mismatches, boxMs / 40.0, bruteMs / 40.0, db.LoadSeconds());
    std::printf("[MotionMatching] cooked gait table: %s\n", GaitSpeedTable::Measure(db).Describe().c_str());
    CHECK(mismatches == 0);
    CHECK(boxMs < bruteMs);
}

// Phase 4, the owner's play test 2: frames the player's own pose rules exclude after the build (hands up, bent over, legs
// crossed) are never matched, the boxes and the runway follow, and the box search is still exactly brute force's; and the
// spine's aim at the view, capped (unarmed).
void TestPlayerExclusionsAndAimCap() {
    MotionDatabase db;
    CHECK(Adopt(db, Build(StandardTakes())));
    const std::uint32_t n = db.FrameCount();
    std::vector<char> bad(n, 0);
    for (std::uint32_t f = 0; f < n; ++f) bad[f] = (f / 50) % 4 == 1; // every fourth run of 50 frames
    std::uint32_t already = 0, want = 0;
    for (std::uint32_t f = 0; f < n; ++f) {
        if (!bad[f]) continue;
        ++want;
        if (db.Flags(f) & MotionLabel::Excluded) ++already;
    }
    CHECK(db.ExcludeFrames(bad) == want - already);
    CHECK(db.ExcludeFrames(bad) == 0); // nothing new the second time
    for (std::uint32_t f = 0; f < n; ++f)
        if (bad[f]) CHECK((db.Flags(f) & MotionLabel::Excluded) != 0);
    // The runway stops at an excluded run: a frame just before one has at most the frames to it.
    int runwayOk = 0, runwaySeen = 0;
    for (std::uint32_t f = 1; f + 1 < n; ++f)
        if (!bad[f] && !(db.Flags(f) & MotionLabel::Excluded) && bad[f + 1] && db.ClipOf(f) == db.ClipOf(f + 1)) {
            ++runwaySeen;
            if (db.Runway(f) <= 1) ++runwayOk;
        }
    CHECK(runwaySeen > 0 && runwayOk == runwaySeen);
    std::mt19937 rng(5);
    std::uniform_int_distribution<std::uint32_t> frame(0, n - 1);
    std::normal_distribution<float> noise(0.0f, 0.3f);
    int mismatches = 0, excludedHits = 0;
    for (int k = 0; k < 200; ++k) {
        float q[27];
        FrameQuery(db, frame(rng), q);
        for (float& x : q) x += noise(rng);
        MotionSearchParams p;
        if (k % 2) p.DesiredGait = MotionLabel::Walk;
        const MotionSearchResult a = MotionSearch::Search(db, q, p);
        const MotionSearchResult b = MotionSearch::SearchBrute(db, q, p);
        if (a.Frame != b.Frame || a.Cost != b.Cost) ++mismatches;
        if (a.Frame >= 0 && bad[(size_t)a.Frame]) ++excludedHits;
    }
    CHECK(mismatches == 0);
    CHECK(excludedHits == 0);

    // The capped aim: no cap or no weight leaves it; small pitches nearly linear; big ones flatten under the cap; both signs.
    const float r = kPi / 180.0f;
    CHECK(FirstPersonBodyCappedSpineAim(85.0f * r, 0.6f, 0.0f, 1.0f) == 0.6f);
    CHECK(FirstPersonBodyCappedSpineAim(85.0f * r, 0.6f, 20.0f * r, 0.0f) == 0.6f);
    CHECK(std::abs(5.0f * FirstPersonBodyCappedSpineAim(5.0f * r, 0.6f, 20.0f * r, 1.0f) - 3.0f) < 0.05f);
    const float up = 85.0f * FirstPersonBodyCappedSpineAim(85.0f * r, 0.6f, 20.0f * r, 1.0f);
    CHECK(up > 19.0f && up < 20.0f);
    const float down = -85.0f * FirstPersonBodyCappedSpineAim(-85.0f * r, 0.9f, 35.0f * r, 1.0f);
    CHECK(down < -33.0f && down > -35.0f);
    const float half = FirstPersonBodyCappedSpineAim(85.0f * r, 0.6f, 20.0f * r, 0.5f);
    CHECK(std::abs(half - 0.5f * (0.6f + up / 85.0f)) < 1e-4f);
    float prev = 0.0f;
    bool monotonic = true;
    for (int d = 1; d <= 90; ++d) {
        const float chest = (float)d * FirstPersonBodyCappedSpineAim((float)d * r, 0.9f, 35.0f * r, 1.0f);
        monotonic = monotonic && chest >= prev;
        prev = chest;
    }
    CHECK(monotonic); // the chest never stops dead or turns back as the view goes on down

    // Under motion matching the aim fades with speed (the owner, 2026-11-17: the torso bends standing; moving, the head
    // looks and the body moves as captured): all of it standing, none by the walk/jog midpoint, never rising with speed.
    const float from = 0.1f, to = 0.5f * (1.27f + 1.85f);
    CHECK(FirstPersonBodyMmAimFade(0.0f, from, to) == 1.0f);
    CHECK(FirstPersonBodyMmAimFade(from, from, to) == 1.0f);
    CHECK(FirstPersonBodyMmAimFade(to, from, to) == 0.0f);
    CHECK(FirstPersonBodyMmAimFade(1.85f, from, to) == 0.0f);  // the jog
    CHECK(FirstPersonBodyMmAimFade(1.27f, from, to) <= 0.25f); // the walk keeps at most a quarter
    CHECK(FirstPersonBodyMmAimFade(0.5f, 1.0f, 1.0f) == 1.0f && FirstPersonBodyMmAimFade(1.5f, 1.0f, 1.0f) == 0.0f); // degenerate
    float last = 2.0f;
    bool falls = true;
    for (int k = 0; k <= 40; ++k) {
        const float f = FirstPersonBodyMmAimFade(0.05f * (float)k, from, to);
        falls = falls && f <= last && f >= 0.0f && f <= 1.0f;
        last = f;
    }
    CHECK(falls);
}

} // namespace

void RegisterMotionMatchingRuntimeTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Motion matching: database build and boxes", TestDatabaseBuild});
    tests.push_back({"Motion matching: search matches brute force", TestSearchMatchesBruteForce});
    tests.push_back({"Motion matching: search filters and ties", TestSearchFiltersAndTies});
    tests.push_back({"Motion matching: 16-bit features against float", TestQuantizedSearch});
    tests.push_back({"Motion matching: the style cost", TestStyleSearch});
    tests.push_back({"Motion matching: fingers in the dead blend, the database cache key", TestFingersDeadBlend});
    tests.push_back({"Motion matching: search schedule", TestSearchSchedule});
    tests.push_back({"Motion matching: dead blending", TestInertialization});
    tests.push_back({"Motion matching: trajectory springs and collision", TestTrajectoryPredictor});
    tests.push_back({"Motion matching: gait speed table", TestGaitTable});
    tests.push_back({"Motion matching: query root space", TestQueryConvention});
    tests.push_back({"Motion matching: matcher end to end", TestMatcherEndToEnd});
    tests.push_back({"Motion matching: player hooks (play rate, lock, gait speeds)", TestPlayerHooks});
    tests.push_back({"Motion matching: player exclusions, the capped aim and its fade", TestPlayerExclusionsAndAimCap});
    tests.push_back({"Motion matching: dead blend across a frame-time change", TestDeadBlendFrameTime});
    tests.push_back({"Motion matching: the facing model (simulation and character)", TestFacingModel});
    tests.push_back({"Motion matching: reselect history and the idle hold", TestReselectAndIdleHold});
    tests.push_back({"Motion matching: cooked database search", TestCookedDatabaseSearch});
}
