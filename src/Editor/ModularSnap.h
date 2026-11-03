#pragma once
// Modular kit snapping (docs/PROJECT_ASSETS.md "Asset Library"): while a kit piece moves, its
// world box is pulled flush against nearby pieces' faces, and the axes nothing is near snap to
// the kit's module grid. Pure functions on boxes, no scene or ImGui - EditorLayer feeds them.

#include "AABB.h"

#include <glm/glm.hpp>

#include <cmath>
#include <vector>

namespace ModularSnap {

// Pull distance as a share of the module: within it a face snaps, beyond it the piece is free.
constexpr float kReach = 0.25f;

struct Result {
    glm::vec3 Delta{0.0f};  // add to the moving piece's position
    bool Snapped = false;   // at least one face is now flush with a neighbour
    AABB Face{glm::vec3(0.0f), glm::vec3(0.0f)}; // the touching face (zero thickness on its axis), for the highlight
};

inline bool Overlaps(float aMin, float aMax, float bMin, float bMax, float slack) {
    return aMin <= bMax + slack && aMax >= bMin - slack;
}

// `moving` is the piece's world box, `module` the kit's grid size in metres (> 0), `near` the
// world boxes of every other piece close enough to matter. Y faces snap too (stacked floors),
// but only X/Z fall back to the grid, so a piece dropped on uneven ground keeps its height.
inline Result Solve(const AABB& moving, float module, const std::vector<AABB>& near) {
    Result r;
    if (module <= 0.0f) return r;
    const float reach = kReach * module;
    for (int a = 0; a < 3; ++a) {
        const int b = (a + 1) % 3, c = (a + 2) % 3;
        float best = reach;
        bool found = false, contact = false;
        const AABB* hit = nullptr;
        for (const AABB& n : near) {
            if (!Overlaps(moving.Min[b], moving.Max[b], n.Min[b], n.Max[b], reach) ||
                !Overlaps(moving.Min[c], moving.Max[c], n.Min[c], n.Max[c], reach))
                continue;
            // Back-to-back faces (side by side), then same-side faces (fronts lined up in a row).
            const float cands[4] = {n.Max[a] - moving.Min[a], n.Min[a] - moving.Max[a],
                                    n.Min[a] - moving.Min[a], n.Max[a] - moving.Max[a]};
            for (int k = 0; k < 4; ++k) {
                if (std::fabs(cands[k]) < best) {
                    best = std::fabs(cands[k]);
                    r.Delta[a] = cands[k];
                    found = true;
                    contact = k < 2;
                    hit = &n;
                }
            }
        }
        if (found) {
            if (contact && !r.Snapped) {
                // The shared rectangle on the plane where the two boxes meet.
                const float plane = r.Delta[a] == hit->Max[a] - moving.Min[a] ? hit->Max[a] : hit->Min[a];
                r.Face.Min[a] = r.Face.Max[a] = plane;
                r.Face.Min[b] = std::max(moving.Min[b], hit->Min[b]);
                r.Face.Max[b] = std::min(moving.Max[b], hit->Max[b]);
                r.Face.Min[c] = std::max(moving.Min[c], hit->Min[c]);
                r.Face.Max[c] = std::min(moving.Max[c], hit->Max[c]);
            }
            r.Snapped = r.Snapped || contact;
        } else if (a != 1) {
            r.Delta[a] = std::round(moving.Min[a] / module) * module - moving.Min[a];
        }
    }
    return r;
}

// Yaw rounded to the nearest quarter turn: kit pieces only ever meet square.
inline float SnapYawDegrees(float yaw) { return std::round(yaw / 90.0f) * 90.0f; }

} // namespace ModularSnap
