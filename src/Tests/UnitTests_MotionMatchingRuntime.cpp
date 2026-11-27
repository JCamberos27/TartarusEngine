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
#include "../Editor/MotionRebake.h"

#include "Core/Paths.h"
#include "Model.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <thread>

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
    // Source and target about 180 degrees apart, the target turning across that line mid-blend (mmstress, 2026-11-18: two
    // takes' calves rolled ~130 deg apart about the shin, a 114 deg one-frame pop): the blend keeps the way it started - no
    // frame steps more than the blend's own pace over the half turn.
    {
        Inertialization flip;
        MotionPose a, b, o;
        a.Rot = {glm::quat(1, 0, 0, 0)};
        b.Rot = {glm::angleAxis(glm::radians(178.0f), glm::vec3(0, 1, 0))};
        flip.Transition(a, a, dt);
        float worst = 0.0f;
        MotionPose was = a;
        for (int f = 0; f < 15 && (f == 0 || flip.Blending()); ++f) {
            MotionPose t = a;
            t.Rot[0] = glm::angleAxis(glm::radians(178.0f + 0.5f * (float)f), glm::vec3(0, 1, 0)); // crosses 180 at f 4
            flip.Update(dt, t, o);
            worst = std::max(worst, PoseAngle(was, o));
            was = o;
            b = t;
        }
        // (smoothstep's steepest is 1.5x its mean: 185 deg over 12 frames peaks ~23 deg a frame)
        std::printf("[MotionMatching] dead blend across 180 deg: worst step %.1f deg (re-choosing the short way each frame, as before: 114)\n",
                    glm::degrees(worst));
        CHECK(glm::degrees(worst) < 30.0f);
        CHECK(PoseAngle(o, b) < 1e-4f); // and it still lands on the target
        CHECK(MotionMath::QuatAngle(InertializationMath::SlerpAsSigned(qa, qb, 0.5f), glm::slerp(qa, qb, 0.5f)) < 1e-6f); // the short way: glm's
    }
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
    const int settle = (int)std::ceil(8.0f * cfg.SimFacingHalflife / dt); // 8 halflives: settled within 0.01, whatever the default
    for (int i = 0; i < settle; ++i) {
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

// The glitch scan before the |dot| test (MotionMath::QuatAngle per bone): what ScanGlitches must reproduce exactly.
std::vector<char> GlitchScanReference(const MotionClipStream& s, float limit) {
    const std::uint32_t n = s.FrameCount();
    const int nb = (int)s.Bones().size();
    std::vector<char> bad(n, 0);
    std::vector<glm::quat> prev((size_t)nb), cur((size_t)nb);
    for (const auto& clip : s.Clips()) {
        if (clip.Count == 0) continue;
        for (int b = 0; b < nb; ++b) prev[(size_t)b] = s.Rotation(clip.First, b);
        for (std::uint32_t k = 1; k < clip.Count; ++k) {
            const std::uint32_t i = clip.First + k;
            bool glitch = false;
            for (int b = 0; b < nb; ++b) {
                cur[(size_t)b] = s.Rotation(i, b);
                glitch = glitch || MotionMath::QuatAngle(prev[(size_t)b], cur[(size_t)b]) > limit;
            }
            std::swap(prev, cur);
            if (!glitch) continue;
            const int pad = MotionDatabase::kGlitchPad;
            const std::uint32_t from = (std::uint32_t)std::max<std::int64_t>(clip.First, (std::int64_t)i - 1 - pad);
            const std::uint32_t to = std::min<std::uint32_t>(clip.First + clip.Count - 1, i + (std::uint32_t)pad);
            for (std::uint32_t j = from; j <= to; ++j) bad[j] = 1;
        }
    }
    return bad;
}

// ScanGlitches' |dot| < cos(L/2) test excludes exactly the frames the QuatAngle scan did: on turns just over and just
// under the limit (as stored, after the 48-bit packing) and on the cooked Manny bake when present.
void TestGlitchScanMatchesAngle() {
    const float limit = MotionDatabase::GlitchLimit(60.0f);
    // Synthetic: one bone, a clip per sample: rest, then a turn about a random axis; kept only when the stored (packed)
    // turn lands 0.5e-4..1.5e-4 rad over or under the limit.
    MotionClipStream s;
    s.Reset(60.0f, {"bone"});
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f), side(0.0f, 1.0f);
    int over = 0, under = 0, tries = 0;
    std::vector<char> expectOver;
    while ((over < 200 || under < 200) && ++tries < 200000) {
        glm::vec3 axis(u(rng), u(rng), u(rng));
        if (glm::length(axis) < 0.1f) continue;
        axis = glm::normalize(axis);
        const glm::quat base = glm::normalize(glm::quat(u(rng), u(rng), u(rng), u(rng)));
        const bool wantOver = side(rng) < 0.5f;
        const float target = limit + (wantOver ? 1e-4f : -1e-4f);
        const glm::quat a = MotionMath::UnpackQuat(MotionMath::PackQuat(base));
        const glm::quat b = MotionMath::UnpackQuat(MotionMath::PackQuat(base * glm::angleAxis(target, axis)));
        const float d = MotionMath::QuatAngle(a, b) - limit;
        if (std::fabs(d) < 0.5e-4f || std::fabs(d) > 1.5e-4f) continue;
        if (d > 0.0f ? over >= 200 : under >= 200) continue;
        (d > 0.0f ? over : under)++;
        const std::vector<glm::quat> rot = {base, base * glm::angleAxis(target, axis)};
        const std::vector<MotionClipStream::RootSample> root(2);
        const std::vector<glm::vec3> pelvis(2, glm::vec3(0.0f));
        s.AddClip({"t" + std::to_string(over + under), "", 0, 0, 0.0f}, root, pelvis, rot);
        expectOver.push_back(d > 0.0f ? 1 : 0);
    }
    CHECK(over == 200 && under == 200);
    std::vector<char> fast;
    CHECK(MotionDatabase::ScanGlitches(s, limit, fast));
    const std::vector<char> ref = GlitchScanReference(s, limit);
    CHECK(fast == ref);
    int wrong = 0;
    for (size_t c = 0; c < s.Clips().size(); ++c)
        wrong += ref[s.Clips()[c].First + 1] != expectOver[c]; // the reference itself splits over / under as stored
    CHECK(wrong == 0);
    // Low frame rates: the limit passes pi under ~11.7 fps (10 fps: 210 degrees; 1 fps: 2100). No turn is bigger than pi, so
    // nothing is flagged; cos(limit / 2) alone wraps and would flag big turns. Takes turning 30..179 degrees in a frame.
    {
        MotionClipStream big;
        big.Reset(1.0f, {"bone"});
        for (int k = 0; k < 16; ++k) {
            const float turn = glm::radians(30.0f + 149.0f * (float)k / 15.0f);
            const glm::quat a = glm::normalize(glm::quat(u(rng), u(rng), u(rng), u(rng)));
            const std::vector<glm::quat> rot = {a, a * glm::angleAxis(turn, glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f)))};
            big.AddClip({"b" + std::to_string(k), "", 0, 0, 0.0f}, std::vector<MotionClipStream::RootSample>(2),
                        std::vector<glm::vec3>(2, glm::vec3(0.0f)), rot);
        }
        for (const float fps : {10.0f, 1.0f}) {
            const float l = MotionDatabase::GlitchLimit(fps);
            CHECK(l > MotionMath::kPi);
            std::vector<char> f;
            CHECK(MotionDatabase::ScanGlitches(big, l, f));
            CHECK(f == GlitchScanReference(big, l));
            CHECK(std::count(f.begin(), f.end(), (char)1) == 0);
        }
    }
    // Cancelling stops it.
    std::vector<char> none;
    CHECK(!MotionDatabase::ScanGlitches(s, limit, none, [] { return false; }));

    // The cooked bake: the Excluded bitset (the glitch part) identical, and the timings.
    const std::string base = ProjectPaths::Resolve("assets/Animations/MotionMatching/Manny");
    if (!std::filesystem::exists(base + ".mmanim") || !std::filesystem::exists(base + ".mmdb")) {
        std::printf("[MotionMatching] glitch scan: Manny not baked, synthetic only\n");
        return;
    }
    MotionDatabase db;
    std::string err;
    CHECK(db.Load(base, &err));
    if (!db.Loaded()) return;
    const float l = MotionDatabase::GlitchLimit(db.Fps());
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<char> oldBad = GlitchScanReference(db.Stream(), l);
    const auto t1 = std::chrono::steady_clock::now();
    std::vector<char> newBad;
    CHECK(MotionDatabase::ScanGlitches(db.Stream(), l, newBad));
    const auto t2 = std::chrono::steady_clock::now();
    size_t diff = 0, marked = 0;
    for (size_t i = 0; i < oldBad.size() && i < newBad.size(); ++i) {
        diff += oldBad[i] != newBad[i];
        marked += oldBad[i] != 0;
    }
    CHECK(oldBad.size() == newBad.size());
    CHECK(diff == 0);
    const double oldMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double newMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    const double buildMs = (db.LoadSeconds() - db.ReadSeconds()) * 1000.0;
    std::printf("[MotionMatching] glitch scan: %u frames x %zu bones, %zu marked, %zu differ; scan old %.1f ms, new %.1f ms; "
                "Build %.1f ms (its scan %.1f ms; with the old scan ~%.1f ms)\n",
                db.FrameCount(), db.Stream().Bones().size(), marked, diff, oldMs, newMs, buildMs, db.ScanSeconds() * 1000.0,
                buildMs - db.ScanSeconds() * 1000.0 + oldMs);
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


// TrajectoryPredictor::PredictAlong (the NPCs' route, Phase 5): round a corner and into the goal. What matters: the facing ends
// the last leg's way, never the 180 deg turn back a passed goal used to give (the npcloco walks' ends: 3-4 jumps a second); the
// corner is steered round, not driven through; the move slows into the goal. Measured (2026-11-18): at 1 s (0.666, 0.612),
// 0.23 m/s, yaw 73 deg - the velocity spring carries it ~7 cm past the goal as the director's capsule's own ease carries it to
// within 5 cm of it; at 1/3 s already 1.7 cm round toward +X. The limits are those with a margin.
void TestPredictAlong() {
    TrajectoryPredictor tp;
    tp.Params.VelocityHalflife = 0.08f;
    tp.Params.FacingHalflife = 0.27f;
    TrajectoryState start;
    start.Velocity = glm::vec3(0.0f, 0.0f, 1.26f); // walking +Z (yaw 0)
    tp.SetState(start);
    const glm::vec3 pts[2] = {glm::vec3(0.0f, 0.0f, 0.6f), glm::vec3(0.6f, 0.0f, 0.6f)}; // a corner, then the goal 0.6 m to its +X
    TrajectoryPredictor::PathGoal path;
    path.Points = pts;
    path.Count = 2;
    path.Speed = 1.26f;
    path.ArriveTime = 0.15f;
    const float times[3] = {1.0f / 3.0f, 2.0f / 3.0f, 1.0f};
    TrajectoryState out[3];
    tp.PredictAlong(path, 0.0f, times, 3, out);
    for (int k = 0; k < 3; ++k) // (the route as predicted: what the limits below were measured on)
        std::printf("[PredictAlong] t %.2f pos (%.3f, %.3f) vel (%.3f, %.3f) yaw %.1f deg\n", times[k], out[k].Position.x, out[k].Position.z, out[k].Velocity.x, out[k].Velocity.z, out[k].Yaw * 57.2958f);
    const glm::vec3 end = out[2].Position;
    CHECK(glm::length(glm::vec2(end.x - 0.6f, end.z - 0.6f)) < 0.12f); // at the goal (its spring's carry past it)
    // Facing the last leg's way (+X: yaw +pi/2) and still turning toward it - not back along it.
    CHECK(out[2].Yaw > out[1].Yaw && std::fabs(MotionMath::WrapPi(out[2].Yaw - 1.5707963f)) < 0.5f);
    CHECK(glm::length(out[2].Velocity) < 0.4f);                           // slowing into it (from 1.26)
    CHECK(out[0].Position.x > 0.005f && out[1].Position.x > 0.25f);       // round the corner, not through it
    // Past the last point there is nowhere left to go: standing on it, the prediction stays (no 180 deg turn back to it).
    TrajectoryState there;
    there.Position = glm::vec3(0.61f, 0.0f, 0.6f);
    there.Yaw = 1.5707963f;
    tp.SetState(there);
    const glm::vec3 goalOnly[1] = {pts[1]};
    path.Points = goalOnly;
    path.Count = 1;
    tp.PredictAlong(path, 1.5707963f, times, 3, out);
    CHECK(std::fabs(MotionMath::WrapPi(out[2].Yaw - 1.5707963f)) < 0.05f);
    CHECK(glm::length(out[2].Position - there.Position) < 0.02f);
}

// The predicted start and stop against the capsule's own (PlayerApproachVelocity: an exponential, time constant accelTime
// speeding up, decelTime slowing down; the NPC director's pace is the same ease). The predictor's spring takes each one's
// halflife (tau ln 2), picked per step the way the capsule picks its time. A critically damped spring and an exponential with
// the same halflife cover the same distance to a stop or lose the same distance on a start (the spring's v0 (1 + y t) e^-yt,
// y = 2 / tau, integrates to v0 tau, as the exponential's v0 e^-t/tau does), so where the move settles is predicted exactly;
// only the shape on the way differs (printed: predicted vs capsule at 0.1 s and the query's 1/3, 2/3, 1 s).
// Tolerance 1 cm at 2/3 s and 1 s: by then both are settled (e^-6.7 of v0 tau left at worst, decel 0.15 s: < 0.1 mm), the
// capsule here is stepped at 1 kHz (trapezoid: < 0.1 mm off its continuous path), and its 0.01 m/s arrive snap moves the end
// by under 0.01 m/s x tau = 1.5 mm. One halflife for both ways (the old predictor) misses a 5 m/s stop by v0 (decel - accel):
// 10 cm for 0.08 / 0.10, 15 cm for the NPCs' 0.12 / 0.15 - checked below so the test can tell the two apart.
void TestPredictStartStop() {
    const float times[4] = {0.1f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f};
    struct Pair { float Accel, Decel; const char* Who; };
    const Pair pairs[2] = {{0.08f, 0.10f, "player"}, {0.12f, 0.15f, "npc"}};
    const glm::vec3 run(0.0f, 0.0f, 5.0f);
    // The capsule: position and velocity at times[], from v0 toward `target`.
    const auto capsule = [&](const glm::vec3& v0, const glm::vec3& target, const Pair& p, glm::vec3* pos, glm::vec3* vel) {
        glm::vec3 x(0.0f), v = v0;
        const float dt = 0.001f;
        int k = 0;
        for (int i = 1; k < 4; ++i) {
            const glm::vec3 nv = PlayerApproachVelocity(v, target, dt, p.Accel, p.Decel);
            x += (v + nv) * (0.5f * dt);
            v = nv;
            if (std::fabs(i * dt - times[k]) < 0.5f * dt) { pos[k] = x; vel[k] = v; ++k; }
        }
    };
    const auto predict = [&](const glm::vec3& v0, const glm::vec3& target, float accelH, float decelH, TrajectoryState* out) {
        TrajectoryPredictor tp;
        tp.Params.VelocityHalflife = accelH;
        tp.Params.VelocityDecelHalflife = decelH;
        TrajectoryState s;
        s.Velocity = v0;
        tp.SetState(s);
        tp.Predict(target, 0.0f, times, 4, out);
    };
    for (const Pair& p : pairs) {
        const float ah = p.Accel * 0.6931f, dh = p.Decel * 0.6931f;
        for (int stop = 0; stop < 2; ++stop) {
            const glm::vec3 v0 = stop ? run : glm::vec3(0.0f), target = stop ? glm::vec3(0.0f) : run;
            glm::vec3 cp[4], cv[4];
            capsule(v0, target, p, cp, cv);
            TrajectoryState pr[4], old[4];
            predict(v0, target, ah, dh, pr);
            predict(v0, target, ah, 0.0f, old); // one halflife both ways
            for (int k = 0; k < 4; ++k)
                std::printf("[PredictStartStop] %s %s %.2f/%.2f t %.2f: speed predicted %.3f capsule %.3f (one-halflife %.3f) m/s, "
                            "distance predicted %.3f capsule %.3f (one-halflife %.3f) m\n",
                            p.Who, stop ? "stop " : "start", p.Accel, p.Decel, times[k], glm::length(pr[k].Velocity),
                            glm::length(cv[k]), glm::length(old[k].Velocity), pr[k].Position.z, cp[k].z, old[k].Position.z);
            for (int k = 2; k < 4; ++k) CHECK(std::fabs(pr[k].Position.z - cp[k].z) < 0.01f);
            if (stop) CHECK(std::fabs(old[3].Position.z - cp[3].z) > 0.05f); // the old predictor's stop: v0 (decel - accel) short
            else CHECK(std::fabs(old[3].Position.z - pr[3].Position.z) < 1e-5f); // a start never takes the decel halflife
        }
    }
    // A slower move (run to walk) slows with the decel halflife too: it ends where the capsule does.
    const Pair& p = pairs[0];
    glm::vec3 cp[4], cv[4];
    const glm::vec3 walk(0.0f, 0.0f, 1.5f);
    capsule(run, walk, p, cp, cv);
    TrajectoryState pr[4];
    predict(run, walk, p.Accel * 0.6931f, p.Decel * 0.6931f, pr);
    CHECK(std::fabs(pr[3].Position.z - cp[3].z) < 0.01f);
}

// The first Play's database read ahead (Prefetch): Get takes the prefetched load (never a second read), loads itself while
// the prefetch is still queued (the job then does nothing), waits on a running one, and reads afresh after an Invalidate
// or a rewrite since the prefetch was queued; a cancelled or dropped prefetch never runs.
void TestSharedDatabasePrefetch() {
    std::error_code ec;
    const std::filesystem::path dir = UnitTestSupport::TempDir("mm_prefetch");
    std::filesystem::create_directories(dir, ec);
    const std::string base = (dir / "Prefetch").string();
    std::vector<TakeSpec> takes = StandardTakes();
    const auto save = [&](const std::vector<TakeSpec>& t) {
        SynthDb s = Build(t);
        std::string err;
        return s.Stream.Save(base + ".mmanim", &err) && s.Feat.Save(base + ".mmdb", &err);
    };
    CHECK(save(takes));
    std::vector<SharedMotionDatabase::PrefetchJob> queued;
    int waits = 0;
    const std::atomic<bool> noStop{false};

    SharedMotionDatabase::ReleaseKept();
    SharedMotionDatabase::SetPrefetchRunner(nullptr);
    SharedMotionDatabase::Prefetch(base);
    CHECK(!SharedMotionDatabase::PrefetchPending()); // no runner: nothing
    SharedMotionDatabase::SetPrefetchRunner([&](SharedMotionDatabase::PrefetchJob job) { queued.push_back(std::move(job)); },
                                            [&](bool waiting) { waits += waiting ? 1 : 0; });

    // Run, then taken: the files rewritten behind it (one take fewer, the old write times put back) still give the
    // prefetched frame count - Get never read them.
    SharedMotionDatabase::Prefetch(base);
    SharedMotionDatabase::Prefetch(base); // pending: not queued twice
    CHECK(SharedMotionDatabase::PrefetchPending() && queued.size() == 1);
    if (queued.size() != 1) return;
    queued[0](noStop);
    queued.clear();
    const auto stampA = std::filesystem::last_write_time(base + ".mmanim", ec), stampD = std::filesystem::last_write_time(base + ".mmdb", ec);
    std::vector<TakeSpec> fewer = takes;
    fewer.pop_back();
    CHECK(save(fewer));
    std::filesystem::last_write_time(base + ".mmanim", stampA, ec);
    std::filesystem::last_write_time(base + ".mmdb", stampD, ec);
    auto got = SharedMotionDatabase::Get(base);
    CHECK(got != nullptr && !SharedMotionDatabase::PrefetchPending());
    const std::uint32_t full = got ? got->Db.FrameCount() : 0;
    CHECK(full == Build(takes).Stream.FrameCount());
    CHECK(waits == 0);
    // Kept and current: nothing to prefetch.
    SharedMotionDatabase::Prefetch(base);
    CHECK(!SharedMotionDatabase::PrefetchPending() && queued.empty());
    got.reset();
    CHECK(save(takes));
    SharedMotionDatabase::ReleaseKept();

    // Still queued at Get: Get loads; the job then does nothing (one load).
    SharedMotionDatabase::Prefetch(base);
    CHECK(queued.size() == 1);
    got = SharedMotionDatabase::Get(base);
    CHECK(got && got->Db.FrameCount() == full && !SharedMotionDatabase::PrefetchPending());
    if (!queued.empty()) queued[0](noStop);
    queued.clear();
    CHECK(SharedMotionDatabase::Get(base) == got); // the job didn't replace it
    got.reset();
    SharedMotionDatabase::ReleaseKept();

    // Cancelled while queued (the scene closed): nothing held, the job does nothing.
    SharedMotionDatabase::Prefetch(base);
    SharedMotionDatabase::CancelPrefetch();
    CHECK(!SharedMotionDatabase::PrefetchPending());
    if (!queued.empty()) queued[0](noStop);
    queued.clear();

    // Invalidate (a rebake) after the prefetch ran: dropped, Get reads the new files.
    SharedMotionDatabase::Prefetch(base);
    if (!queued.empty()) queued[0](noStop);
    queued.clear();
    CHECK(save(fewer));
    std::filesystem::last_write_time(base + ".mmanim", stampA, ec);
    std::filesystem::last_write_time(base + ".mmdb", stampD, ec);
    SharedMotionDatabase::Invalidate(base);
    CHECK(!SharedMotionDatabase::PrefetchPending());
    got = SharedMotionDatabase::Get(base);
    CHECK(got && got->Db.FrameCount() < full);
    got.reset();
    SharedMotionDatabase::ReleaseKept();

    // Rewritten after it was queued (no Invalidate): stale, Get reads afresh.
    CHECK(save(takes));
    SharedMotionDatabase::Prefetch(base);
    if (!queued.empty()) queued[0](noStop);
    queued.clear();
    CHECK(save(fewer));
    std::filesystem::last_write_time(base + ".mmdb", std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5), ec);
    got = SharedMotionDatabase::Get(base);
    CHECK(got && got->Db.FrameCount() < full);
    got.reset();
    SharedMotionDatabase::ReleaseKept();

    // On a worker: Get waits for a running prefetch (or loads itself if it hasn't started) - one load, the full database.
    CHECK(save(takes));
    std::thread worker;
    SharedMotionDatabase::SetPrefetchRunner([&](SharedMotionDatabase::PrefetchJob job) {
        worker = std::thread([job = std::move(job), &noStop] { job(noStop); });
    }, [&](bool waiting) { waits += waiting ? 1 : 0; });
    SharedMotionDatabase::Prefetch(base);
    got = SharedMotionDatabase::Get(base);
    if (worker.joinable()) worker.join();
    CHECK(got && got->Db.FrameCount() == full && !SharedMotionDatabase::PrefetchPending());
    got.reset();

    SharedMotionDatabase::SetPrefetchRunner(nullptr);
    SharedMotionDatabase::ReleaseKept();
    std::filesystem::remove_all(dir, ec);
}

// The prefetch measured on the driver rig (RigShape, copied on the main thread): Get takes it only for a rig of the same
// shape, and then gives exactly what its own measuring gives; a job measured on another shape (an outfit changed the
// driver) or a Get with no rig reads afresh; a job stopped mid-build leaves nothing.
void TestSharedDatabasePrefetchRig() {
    std::error_code ec;
    const std::filesystem::path dir = UnitTestSupport::TempDir("mm_prefetch_rig");
    std::filesystem::create_directories(dir, ec);
    const std::string base = (dir / "PrefetchRig").string();
    {
        SynthDb s = Build(StandardTakes());
        std::string err;
        CHECK(s.Stream.Save(base + ".mmanim", &err) && s.Feat.Save(base + ".mmdb", &err));
    }
    // Two rigs, the same nodes: hands down at the hips (nothing over the shoulders) and hands up over the head (all of it).
    const auto rig = [&](const char* name, const char* handY) {
        const std::filesystem::path file = dir / name;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"name":"Root","mesh":0,"children":[1]},{"name":"pelvis","translation":[0,1,0],"children":[2]},
{"name":"spine_05","translation":[0,0.4,0],"children":[3,5]},
{"name":"upperarm_l","translation":[0.2,0.1,0],"children":[4]},{"name":"hand_l","translation":[0.1,)" << handY << R"(,0]},
{"name":"upperarm_r","translation":[-0.2,0.1,0],"children":[6]},{"name":"hand_r","translation":[-0.1,)" << handY << R"(,0]}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],
"buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]})";
        out.close();
        return Model::ImportDeferred(file.string(), ModelImportSettings{});
    };
    const auto down = rig("rig_down.gltf", "-0.5"), up = rig("rig_up.gltf", "0.5");
    CHECK(down && up);
    if (!down || !up) return;
    CHECK(SharedMotionDatabase::SameShape(*SharedMotionDatabase::ShapeOf(*down), *SharedMotionDatabase::ShapeOf(*down)));
    CHECK(!SharedMotionDatabase::SameShape(*SharedMotionDatabase::ShapeOf(*down), *SharedMotionDatabase::ShapeOf(*up)));

    // What Get measures itself, per rig (no runner: no prefetch).
    SharedMotionDatabase::SetPrefetchRunner(nullptr);
    SharedMotionDatabase::ReleaseKept();
    const auto same = [](const SharedMotionDatabase& a, const SharedMotionDatabase& b) {
        if (a.Db.FrameCount() != b.Db.FrameCount() || a.HandsUpFrames != b.HandsUpFrames || a.BentOverFrames != b.BentOverFrames ||
            a.IdleGestureCuts != b.IdleGestureCuts || a.StartFrame != b.StartFrame || a.TurnInPlaceRate != b.TurnInPlaceRate)
            return false;
        for (std::uint32_t f = 0; f < a.Db.FrameCount(); ++f)
            if (a.Db.Flags(f) != b.Db.Flags(f)) return false;
        return true;
    };
    auto directDown = SharedMotionDatabase::Get(base, nullptr, down.get());
    auto directUp = SharedMotionDatabase::Get(base, nullptr, up.get());
    CHECK(directDown && directUp);
    if (!directDown || !directUp) return;
    CHECK(directDown->HandsUpFrames == 0 && directUp->HandsUpFrames > 0); // the two shapes measure differently
    // Copies: a held load is what Get hands out while anything holds it (it would never reach the prefetch).
    const SharedMotionDatabase refDown = *directDown, refUp = *directUp;
    directDown.reset();
    directUp.reset();
    SharedMotionDatabase::ReleaseKept();

    std::vector<SharedMotionDatabase::PrefetchJob> queued;
    const std::atomic<bool> noStop{false}, stopNow{true};
    SharedMotionDatabase::SetPrefetchRunner([&](SharedMotionDatabase::PrefetchJob job) { queued.push_back(std::move(job)); });
    const auto run = [&](const std::atomic<bool>& stop) {
        for (auto& job : queued) job(stop);
        queued.clear();
    };

    // Measured on the worker, taken by a Get with the same shape: the same database as Get's own measuring.
    SharedMotionDatabase::Prefetch(base, SharedMotionDatabase::ShapeOf(*down));
    SharedMotionDatabase::Prefetch(base, SharedMotionDatabase::ShapeOf(*down)); // the same shape pending: not queued twice
    CHECK(queued.size() == 1);
    run(noStop);
    auto got = SharedMotionDatabase::Get(base, nullptr, down.get());
    CHECK(got && same(*got, refDown) && !SharedMotionDatabase::PrefetchPending());
    got.reset();
    SharedMotionDatabase::ReleaseKept();

    // Another shape queued for the same files: the pending one is replaced.
    SharedMotionDatabase::Prefetch(base, SharedMotionDatabase::ShapeOf(*down));
    SharedMotionDatabase::Prefetch(base, SharedMotionDatabase::ShapeOf(*up));
    CHECK(queued.size() == 2);
    run(noStop); // (the first was dropped: it does nothing)
    got = SharedMotionDatabase::Get(base, nullptr, up.get());
    CHECK(got && same(*got, refUp));
    got.reset();
    SharedMotionDatabase::ReleaseKept();

    // Measured on the hands-down rig, Get on the hands-up one (the driver changed): read afresh, measured on Get's rig.
    SharedMotionDatabase::Prefetch(base, SharedMotionDatabase::ShapeOf(*down));
    run(noStop);
    got = SharedMotionDatabase::Get(base, nullptr, up.get());
    CHECK(got && same(*got, refUp));
    got.reset();
    SharedMotionDatabase::ReleaseKept();

    // Measured on a rig, Get with none: read afresh, unmeasured.
    SharedMotionDatabase::Prefetch(base, SharedMotionDatabase::ShapeOf(*down));
    run(noStop);
    got = SharedMotionDatabase::Get(base);
    CHECK(got && got->IdleGestureCuts == -1 && got->HandsUpFrames == 0 && got->Db.FrameCount() == refDown.Db.FrameCount());
    got.reset();
    SharedMotionDatabase::ReleaseKept();

    // No rig at Prefetch (the driver's model not loaded yet): read and built on the worker, measured by Get on its rig.
    SharedMotionDatabase::Prefetch(base);
    run(noStop);
    got = SharedMotionDatabase::Get(base, nullptr, up.get());
    CHECK(got && same(*got, refUp));
    got.reset();
    SharedMotionDatabase::ReleaseKept();

    // Stopped (shutdown) once running: the build stops, nothing is held, Get reads.
    SharedMotionDatabase::Prefetch(base, SharedMotionDatabase::ShapeOf(*down));
    run(stopNow);
    got = SharedMotionDatabase::Get(base, nullptr, down.get());
    CHECK(got && same(*got, refDown));
    got.reset();

    SharedMotionDatabase::SetPrefetchRunner(nullptr);
    SharedMotionDatabase::ReleaseKept();
    std::filesystem::remove_all(dir, ec);
}

// Live rebake (hot reload): a rewritten database stays cached until Invalidate; after it, Get reads the new files while a
// holder of the old load keeps its copy, and Generation moves (FirstPersonBodyMotion::Stale).
void TestSharedDatabaseInvalidate() {
    std::error_code ec;
    const std::filesystem::path dir = UnitTestSupport::TempDir("mm_rebake");
    std::filesystem::create_directories(dir, ec);
    const std::string base = (dir / "Rebake").string();
    const auto save = [&](const std::vector<TakeSpec>& takes) {
        SynthDb s = Build(takes);
        std::string err;
        return s.Stream.Save(base + ".mmanim", &err) && s.Feat.Save(base + ".mmdb", &err);
    };
    std::vector<TakeSpec> takes = StandardTakes();
    CHECK(save(takes));
    auto first = SharedMotionDatabase::Get(base);
    CHECK(first != nullptr);
    if (!first) return;
    const std::uint32_t before = first->Db.FrameCount();
    CHECK(SharedMotionDatabase::Get(base) == first); // cached

    takes.pop_back(); // a rebake with one take fewer
    CHECK(save(takes));
    CHECK(SharedMotionDatabase::Get(base) == first); // still the cached load until told
    const std::uint64_t generation = SharedMotionDatabase::Generation();
    // Another spelling of the same base (as the editor's file watcher reports it) drops it too.
    std::string other = base;
    for (char& c : other) c = c == '\\' ? '/' : (char)std::toupper((unsigned char)c);
    SharedMotionDatabase::Invalidate(other);
    CHECK(SharedMotionDatabase::Generation() == generation + 1);
    auto second = SharedMotionDatabase::Get(base);
    CHECK(second != nullptr && second != first);
    CHECK(second && second->Db.FrameCount() < before);
    CHECK(first->Db.FrameCount() == before); // the old holder's copy is untouched

    // An unrelated base leaves the cache alone.
    SharedMotionDatabase::Invalidate(base + "_other");
    CHECK(SharedMotionDatabase::Get(base) == second);
    first.reset();

    // Kept after its last holder lets go (a later Play reuses it) ...
    std::weak_ptr<const SharedMotionDatabase> keptWeak = second;
    second.reset();
    CHECK(!keptWeak.expired());
    CHECK(SharedMotionDatabase::Get(base) == keptWeak.lock());
    // ... until its files are rewritten (no Invalidate: a bake outside the editor's watcher) ...
    const std::uint32_t twoFewer = keptWeak.lock()->Db.FrameCount();
    takes.pop_back();
    CHECK(save(takes));
    std::filesystem::last_write_time(base + ".mmdb", std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5), ec);
    auto rewritten = SharedMotionDatabase::Get(base);
    CHECK(rewritten && rewritten->Db.FrameCount() < twoFewer);
    CHECK(keptWeak.expired()); // the stale load was let go
    // ... or released (ReleaseKept) or invalidated.
    std::weak_ptr<const SharedMotionDatabase> rewrittenWeak = rewritten;
    rewritten.reset();
    CHECK(!rewrittenWeak.expired());
    SharedMotionDatabase::ReleaseKept();
    CHECK(rewrittenWeak.expired());
    auto fresh = SharedMotionDatabase::Get(base);
    rewrittenWeak = fresh;
    fresh.reset();
    SharedMotionDatabase::Invalidate(base);
    CHECK(rewrittenWeak.expired());
    // A kept load also watches the rig's model file and its .meta (import settings change the measured frames).
    CHECK(SharedMotionDatabase::WatchedFiles("a/Manny", "") == std::vector<std::string>({"a/Manny.mmanim", "a/Manny.mmdb"}));
    CHECK(SharedMotionDatabase::WatchedFiles("a/Manny", "b/Rig.fbx") ==
          std::vector<std::string>({"a/Manny.mmanim", "a/Manny.mmdb", "b/Rig.fbx", "b/Rig.fbx.meta"}));
    std::filesystem::remove_all(dir, ec);

    // What the editor's watcher hands the rebake.
    std::string got;
    CHECK(MotionRebake::IsConfig("assets/Animations/MotionMatching/Manny.mmconfig") && MotionRebake::IsConfig("X.MMCONFIG"));
    CHECK(!MotionRebake::IsConfig("Manny.mmdb") && !MotionRebake::IsBakedDatabase("Manny.mmconfig"));
    CHECK(MotionRebake::IsBakedDatabase("a/Manny.mmdb", &got) && got == "a/Manny");
    CHECK(MotionRebake::IsBakedDatabase("a/Manny.mmanim", &got) && got == "a/Manny");
}
} // namespace

void RegisterMotionMatchingRuntimeTests(UnitTestSupport::TestList& tests) {
    tests.push_back({"Motion matching: shared database invalidate (live rebake)", TestSharedDatabaseInvalidate});
    tests.push_back({"Motion matching: shared database prefetch ahead of Play", TestSharedDatabasePrefetch});
    tests.push_back({"Motion matching: shared database prefetch measured on the driver rig", TestSharedDatabasePrefetchRig});
    tests.push_back({"Motion matching: database build and boxes", TestDatabaseBuild});
    tests.push_back({"Motion matching: search matches brute force", TestSearchMatchesBruteForce});
    tests.push_back({"Motion matching: search filters and ties", TestSearchFiltersAndTies});
    tests.push_back({"Motion matching: 16-bit features against float", TestQuantizedSearch});
    tests.push_back({"Motion matching: the style cost", TestStyleSearch});
    tests.push_back({"Motion matching: fingers in the dead blend, the database cache key", TestFingersDeadBlend});
    tests.push_back({"Motion matching: search schedule", TestSearchSchedule});
    tests.push_back({"Motion matching: dead blending", TestInertialization});
    tests.push_back({"Motion matching: trajectory springs and collision", TestTrajectoryPredictor});
    tests.push_back({"Motion matching: trajectory along a route (NPCs)", TestPredictAlong});
    tests.push_back({"Motion matching: predicted start and stop match the capsule's", TestPredictStartStop});
    tests.push_back({"Motion matching: gait speed table", TestGaitTable});
    tests.push_back({"Motion matching: query root space", TestQueryConvention});
    tests.push_back({"Motion matching: matcher end to end", TestMatcherEndToEnd});
    tests.push_back({"Motion matching: player hooks (play rate, lock, gait speeds)", TestPlayerHooks});
    tests.push_back({"Motion matching: player exclusions, the capped aim and its fade", TestPlayerExclusionsAndAimCap});
    tests.push_back({"Motion matching: dead blend across a frame-time change", TestDeadBlendFrameTime});
    tests.push_back({"Motion matching: the facing model (simulation and character)", TestFacingModel});
    tests.push_back({"Motion matching: reselect history and the idle hold", TestReselectAndIdleHold});
    tests.push_back({"Motion matching: cooked database search", TestCookedDatabaseSearch});
    tests.push_back({"Motion matching: glitch scan (|dot| test == angle test)", TestGlitchScanMatchesAngle});
}
