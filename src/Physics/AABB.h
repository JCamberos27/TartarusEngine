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

    // Minimum translation vector to push `this` out of `other` (assumes overlap).
    glm::vec3 MTV(const AABB& other) const {
        float overlapX = std::min(Max.x, other.Max.x) - std::max(Min.x, other.Min.x);
        float overlapY = std::min(Max.y, other.Max.y) - std::max(Min.y, other.Min.y);
        float overlapZ = std::min(Max.z, other.Max.z) - std::max(Min.z, other.Min.z);

        glm::vec3 centerThis = (Min + Max) * 0.5f;
        glm::vec3 centerOther = (other.Min + other.Max) * 0.5f;

        if (overlapX < overlapY && overlapX < overlapZ) {
            float dir = centerThis.x < centerOther.x ? -1.0f : 1.0f;
            return glm::vec3(overlapX * dir, 0, 0);
        } else if (overlapY < overlapZ) {
            float dir = centerThis.y < centerOther.y ? -1.0f : 1.0f;
            return glm::vec3(0, overlapY * dir, 0);
        } else {
            float dir = centerThis.z < centerOther.z ? -1.0f : 1.0f;
            return glm::vec3(0, 0, overlapZ * dir);
        }
    }

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
