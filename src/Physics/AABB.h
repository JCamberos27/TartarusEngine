#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

struct AABB {
    glm::vec3 Min;
    glm::vec3 Max;

    static AABB FromCenterSize(const glm::vec3& center, const glm::vec3& size) {
        glm::vec3 h = size * 0.5f;
        return AABB{center - h, center + h};
    }

    bool Intersects(const AABB& other) const {
        return Min.x <= other.Max.x && Max.x >= other.Min.x &&
               Min.y <= other.Max.y && Max.y >= other.Min.y &&
               Min.z <= other.Max.z && Max.z >= other.Min.z;
    }

    // (AABB::MTV — the min-translation push-out the old Play-mode Player collision used — was
    // removed in #185 PR 3 when that moved to a PxCapsuleController. Re-add from git history if
    // a cheap AABB depenetration is ever needed again.)

    // World-space AABB enclosing this local-space box after a (possibly rotating) transform.
    // Rotation means the result isn't tight, but it's exact for the axis-aligned case and a
    // safe conservative bound otherwise — good enough for picking/dropping/snapping queries.
    AABB Transformed(const glm::mat4& transform) const {
        glm::vec3 newMin(1e30f), newMax(-1e30f);
        for (int c = 0; c < 8; ++c) {
            glm::vec3 corner((c & 1) ? Max.x : Min.x, (c & 2) ? Max.y : Min.y, (c & 4) ? Max.z : Min.z);
            glm::vec3 worldCorner = glm::vec3(transform * glm::vec4(corner, 1.0f));
            newMin = glm::min(newMin, worldCorner);
            newMax = glm::max(newMax, worldCorner);
        }
        return AABB{newMin, newMax};
    }

    // Ray/AABB slab test. Returns true and sets tHit (distance along ray) on hit.
    bool RayIntersect(const glm::vec3& origin, const glm::vec3& dir, float& tHit) const {
        float tmin = 0.0f, tmax = 1e30f;
        for (int i = 0; i < 3; ++i) {
            float o = origin[i], d = dir[i];
            float mn = Min[i], mx = Max[i];
            if (std::abs(d) < 1e-8f) {
                if (o < mn || o > mx) return false;
            } else {
                float t1 = (mn - o) / d;
                float t2 = (mx - o) / d;
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                if (tmin > tmax) return false;
            }
        }
        tHit = tmin;
        return true;
    }
};
