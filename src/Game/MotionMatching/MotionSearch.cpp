#include "MotionSearch.h"

#include "MotionMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kDims = MotionDatabase::kDims;
constexpr float kInf = std::numeric_limits<float>::infinity();

void DimWeights(const MotionSearchParams& p, float* w) {
    for (int d = 0; d < kDims; ++d) w[d] = std::max(0.0f, p.GroupWeight[MotionFeature::GroupOf(d)]);
}

float GaitCost(const MotionSearchParams& p, int gait) {
    return p.DesiredGait < 0 ? 0.0f : p.GaitWeight * (float)std::abs(gait - p.DesiredGait);
}

// The gait cost's lower bound over a box's gait range.
float GaitCostBound(const MotionSearchParams& p, int lo, int hi) {
    if (p.DesiredGait < 0) return 0.0f;
    const int d = p.DesiredGait < lo ? lo - p.DesiredGait : p.DesiredGait > hi ? p.DesiredGait - hi : 0;
    return p.GaitWeight * (float)d;
}

// Weighted squared distance, stopping once it reaches `stop` (the sum so far is returned, already >= stop).
inline float Distance(const float* q, const float* x, const float* w, float stop) {
    float c = 0.0f;
    for (int d = 0; d < kDims; ++d) {
        const float e = q[d] - x[d];
        c += w[d] * e * e;
        if (c >= stop) return c;
    }
    return c;
}

// The query's weighted squared distance to a box (0 along dims where the query is inside it).
inline float BoxDistance(const float* q, const MotionDatabase::Box& b, const float* w) {
    float c = 0.0f;
    for (int d = 0; d < kDims; ++d) {
        const float e = q[d] < b.Min[d] ? b.Min[d] - q[d] : q[d] > b.Max[d] ? q[d] - b.Max[d] : 0.0f;
        c += w[d] * e * e;
    }
    return c;
}

bool BoxMayPass(const MotionDatabase::Box& b, const MotionSearchParams& p) {
    const std::uint8_t forbid = (std::uint8_t)(p.Forbid | MotionSearch::kAlwaysForbid);
    return (b.FlagOr & p.Require) == p.Require && (b.FlagAnd & forbid) == 0;
}

// Frames of the playing clip near the playing frame are not candidates (they'd just be the same motion, a step off).
struct Skip {
    std::int64_t Current = -1, From = 0, To = -1; // [From, To] skipped
    Skip(const MotionDatabase& db, std::int64_t current, int near) : Current(current) {
        if (current < 0 || current >= (std::int64_t)db.FrameCount()) { Current = -1; return; }
        const auto& c = db.Clip(db.ClipOf((std::uint32_t)current));
        From = std::max<std::int64_t>(c.First, current - near);
        To = std::min<std::int64_t>((std::int64_t)c.First + c.Count - 1, current + near);
    }
    bool operator()(std::int64_t i) const { return i == Current || (i >= From && i <= To); }
};

MotionSearchResult Start(const MotionDatabase& db, const float* query, const MotionSearchParams& p, std::int64_t current) {
    MotionSearchResult r;
    r.Cost = kInf;
    if (current >= 0 && current < (std::int64_t)db.FrameCount()) {
        const float c = MotionSearch::Cost(db, query, (std::uint32_t)current, p);
        if (c < kInf) {
            r.Frame = current;
            r.Cost = r.CurrentCost = c;
        }
    }
    return r;
}

} // namespace

bool MotionSearch::Passes(std::uint8_t flags, const MotionSearchParams& p) {
    return (flags & p.Require) == p.Require && (flags & (p.Forbid | kAlwaysForbid)) == 0;
}

void MotionSearch::BuildQuery(const MotionDatabase& db, const float* poseRaw15, const float* trajRaw12, float* out) {
    float raw[kDims];
    for (int d = 0; d < 15; ++d) raw[d] = poseRaw15[d];
    for (int d = 0; d < 12; ++d) raw[15 + d] = trajRaw12[d];
    db.Normalize(raw, out);
}

float MotionSearch::Cost(const MotionDatabase& db, const float* query, std::uint32_t frame, const MotionSearchParams& p) {
    if (frame >= db.FrameCount() || !Passes(db.Flags(frame), p)) return kInf;
    float w[kDims];
    DimWeights(p, w);
    return Distance(query, db.Normalized(frame), w, kInf) + GaitCost(p, db.Gait(frame));
}

MotionSearchResult MotionSearch::SearchBrute(const MotionDatabase& db, const float* query, const MotionSearchParams& p,
                                             std::int64_t current) {
    MotionSearchResult r = Start(db, query, p, current);
    float w[kDims];
    DimWeights(p, w);
    const Skip skip(db, current, p.IgnoreNear);
    for (std::uint32_t i = 0; i < db.FrameCount(); ++i) {
        if (skip(i) || !Passes(db.Flags(i), p)) continue;
        ++r.FramesScored;
        const float c = Distance(query, db.Normalized(i), w, kInf) + GaitCost(p, db.Gait(i)) + p.TransitionCost;
        if (c < r.Cost) {
            r.Cost = c;
            r.Frame = i;
        }
    }
    r.Switched = r.Frame >= 0 && r.Frame != current;
    if (r.Frame < 0) r.Cost = 0.0f;
    return r;
}

MotionSearchResult MotionSearch::Search(const MotionDatabase& db, const float* query, const MotionSearchParams& p,
                                        std::int64_t current) {
    MotionSearchResult r = Start(db, query, p, current);
    float w[kDims];
    DimWeights(p, w);
    const Skip skip(db, current, p.IgnoreNear);
    const auto& large = db.LargeBoxes();
    const auto& small = db.SmallBoxes();
    const std::uint32_t n = db.FrameCount();
    constexpr int kPer = MotionDatabase::kLargeBox / MotionDatabase::kSmallBox;
    for (size_t L = 0; L < large.size(); ++L) {
        const MotionDatabase::Box& lb = large[L];
        if (!BoxMayPass(lb, p)) continue;
        if (BoxDistance(query, lb, w) + GaitCostBound(p, lb.GaitMin, lb.GaitMax) + p.TransitionCost >= r.Cost) continue;
        ++r.LargeBoxesVisited;
        for (size_t S = L * kPer; S < std::min(small.size(), (L + 1) * kPer); ++S) {
            const MotionDatabase::Box& sb = small[S];
            if (!BoxMayPass(sb, p)) continue;
            if (BoxDistance(query, sb, w) + GaitCostBound(p, sb.GaitMin, sb.GaitMax) + p.TransitionCost >= r.Cost) continue;
            ++r.SmallBoxesVisited;
            const std::uint32_t from = (std::uint32_t)S * MotionDatabase::kSmallBox;
            const std::uint32_t to = std::min(n, from + (std::uint32_t)MotionDatabase::kSmallBox);
            for (std::uint32_t i = from; i < to; ++i) {
                if (skip(i) || !Passes(db.Flags(i), p)) continue;
                ++r.FramesScored;
                const float d = Distance(query, db.Normalized(i), w, r.Cost);
                if (d >= r.Cost) continue; // (the extra costs are >= 0)
                const float c = d + GaitCost(p, db.Gait(i)) + p.TransitionCost;
                if (c < r.Cost) {
                    r.Cost = c;
                    r.Frame = i;
                }
            }
        }
    }
    r.Switched = r.Frame >= 0 && r.Frame != current;
    if (r.Frame < 0) r.Cost = 0.0f;
    return r;
}

bool MotionSearchSchedule::Tick(float dt, const glm::vec3& desiredVelocity, float desiredYaw, std::uint32_t filterKey,
                                bool outOfFrames) {
    m_Timer += dt;
    bool due = m_First || m_Timer >= Interval - 1e-4f;
    if (!due && outOfFrames) {
        due = true;
        ++ForcedByEnd;
    }
    if (!due) {
        const bool changed = glm::length(desiredVelocity - m_Vel) > VelocityChange ||
                             std::fabs(MotionMath::WrapPi(desiredYaw - m_Yaw)) > FacingChangeDeg * MotionMath::kPi / 180.0f ||
                             filterKey != m_Key;
        if (changed) {
            due = true;
            ++ForcedByInput;
        }
    }
    if (!due) return false;
    m_First = false;
    m_Timer = 0.0f;
    m_Vel = desiredVelocity;
    m_Yaw = desiredYaw;
    m_Key = filterKey;
    ++Searches;
    return true;
}
