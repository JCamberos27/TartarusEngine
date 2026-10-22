#pragma once
// The pure maths behind the Scene view's entity icons (EditorLayer::DrawEntityIcons): how far they
// fade and shrink with distance, and which of a crowd of overlapping icons get drawn. No ImGui or
// scene types, so the unit tests cover it directly.

#include <algorithm>
#include <cmath>
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
    for (size_t i = 0; i < icons.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < i && ok; ++j) {
            if (!icons[j].Keep) continue;
            const float dx = icons[i].X - icons[j].X, dy = icons[i].Y - icons[j].Y;
            if (dx * dx + dy * dy < minSq) ok = false;
        }
        icons[i].Keep = ok;
    }
}

} // namespace GizmoIconMath
