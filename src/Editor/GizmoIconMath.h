#pragma once
// The pure maths behind the Scene view's entity icons (EditorLayer::DrawEntityIcons): how far they
// fade and shrink with distance, and which of a crowd of overlapping icons get drawn. No ImGui or
// scene types, so the unit tests cover it directly.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace GizmoIconMath {

constexpr float kFadeNear = 12.0f; // metres: full alpha and full size up to here
constexpr float kFadeFar  = 45.0f; // metres: invisible from here on
constexpr float kMinSize  = 0.7f;  // size multiplier reached at kFadeFar

inline float Smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 1 up to kFadeNear, easing to 0 at kFadeFar. A selected icon never fades.
inline float DistanceAlpha(float dist, bool selected) {
    if (selected) return 1.0f;
    return 1.0f - Smoothstep((dist - kFadeNear) / (kFadeFar - kFadeNear));
}

// 1 up to kFadeNear, shrinking to kMinSize at kFadeFar. A selected icon never shrinks.
inline float DistanceScale(float dist, bool selected) {
    if (selected) return 1.0f;
    const float t = std::clamp((dist - kFadeNear) / (kFadeFar - kFadeNear), 0.0f, 1.0f);
    return 1.0f + (kMinSize - 1.0f) * t;
}

// One icon that wants drawing. Priority: smaller draws first (0 selected, 1 light, 2 camera, 3 other).
struct IconCandidate {
    float X = 0.0f, Y = 0.0f; // screen centre, pixels
    int   Id = 0;             // caller bookkeeping: survives the sort
    int   Priority = 3;
    float Distance = 0.0f;    // from the camera, metres
    bool  Keep = false;       // out: survived the declutter
};

// Sorts `icons` by priority then distance, then marks Keep on each one whose centre is at least
// `minSpacing` pixels from every icon already kept (callers pass ~0.6 * icon size).
inline void Declutter(std::vector<IconCandidate>& icons, float minSpacing) {
    std::stable_sort(icons.begin(), icons.end(), [](const IconCandidate& a, const IconCandidate& b) {
        if (a.Priority != b.Priority) return a.Priority < b.Priority;
        return a.Distance < b.Distance;
    });
    const float minSq = minSpacing * minSpacing;
    // Kept icons bucketed in minSpacing-sized cells: an icon only needs the kept ones in its own
    // and the 8 surrounding cells (anything farther is >= minSpacing away). Same greedy order and
    // result as testing every earlier kept icon, without the O(n * kept) walk (P-23: thousands of
    // icons in All mode on Forest).
    if (minSpacing <= 0.0f) {
        for (auto& ic : icons) ic.Keep = true;
        return;
    }
    const float inv = 1.0f / minSpacing;
    auto cellOf = [inv](float v) { return (std::int64_t)std::floor(v * inv); };
    auto key = [](std::int64_t cx, std::int64_t cy) { return (std::uint64_t)(cx * 73856093LL) ^ (std::uint64_t)(cy * 19349663LL); };
    std::unordered_multimap<std::uint64_t, size_t> kept;
    kept.reserve(icons.size());
    for (size_t i = 0; i < icons.size(); ++i) {
        const std::int64_t cx = cellOf(icons[i].X), cy = cellOf(icons[i].Y);
        bool ok = true;
        for (std::int64_t dy = -1; dy <= 1 && ok; ++dy)
            for (std::int64_t dx = -1; dx <= 1 && ok; ++dx) {
                auto range = kept.equal_range(key(cx + dx, cy + dy));
                for (auto it = range.first; it != range.second && ok; ++it) {
                    const IconCandidate& o = icons[it->second];
                    const float ddx = icons[i].X - o.X, ddy = icons[i].Y - o.Y;
                    if (ddx * ddx + ddy * ddy < minSq) ok = false;
                }
            }
        icons[i].Keep = ok;
        if (ok) kept.emplace(key(cx, cy), i);
    }
}

} // namespace GizmoIconMath
