#pragma once
#include <glm/glm.hpp>
#include <array>
#include "AABB.h"

// Camera view frustum as 6 planes (left/right/bottom/top/near/far), each stored as (normal, d)
// such that a point P is on the visible side when dot(normal, P) + d >= 0. Extracted directly
// from a combined view-projection matrix via the standard Gribb-Hartmann method - no separate
// FOV/aspect/near/far bookkeeping to keep in sync, it falls out of the matrix itself, so it's
// automatically correct for both the perspective and orthographic editor camera modes.
struct Frustum {
    struct Plane {
        glm::vec3 Normal{0.0f};
        float D = 0.0f;
        float SignedDistance(const glm::vec3& p) const { return glm::dot(Normal, p) + D; }
    };
    std::array<Plane, 6> Planes;

    static Frustum FromViewProj(const glm::mat4& viewProj) {
        Frustum f;
        auto setPlane = [](Plane& plane, const glm::vec4& coeffs) {
            plane.Normal = glm::vec3(coeffs);
            plane.D = coeffs.w;
            float len = glm::length(plane.Normal);
            if (len > 1e-8f) {
                plane.Normal /= len;
                plane.D /= len;
            }
        };

        // GLM stores column-major (viewProj[col][row]); Gribb-Hartmann's derivation is stated in
        // terms of the matrix's ROWS, so each row here is gathered across all four columns at a
        // fixed row index.
        glm::vec4 row0(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
        glm::vec4 row1(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
        glm::vec4 row2(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);
        glm::vec4 row3(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);

        setPlane(f.Planes[0], row3 + row0); // left
        setPlane(f.Planes[1], row3 - row0); // right
        setPlane(f.Planes[2], row3 + row1); // bottom
        setPlane(f.Planes[3], row3 - row1); // top
        setPlane(f.Planes[4], row3 + row2); // near
        setPlane(f.Planes[5], row3 - row2); // far
        return f;
    }

    // Conservative test: only returns false when the box is fully outside some one plane (its
    // single most-positive corner against that plane still fails). A box that's actually outside
    // the frustum but straddles a corner/edge in a way that passes every plane's check anyway
    // (rare, only right at the frustum's silhouette) is treated as visible - a false positive
    // costs one wasted draw call, whereas a false negative would incorrectly hide something the
    // camera can actually see, which is the failure mode that must never happen.
    bool Intersects(const AABB& box) const {
        for (const Plane& plane : Planes) {
            glm::vec3 p(
                plane.Normal.x >= 0.0f ? box.Max.x : box.Min.x,
                plane.Normal.y >= 0.0f ? box.Max.y : box.Min.y,
                plane.Normal.z >= 0.0f ? box.Max.z : box.Min.z);
            if (plane.SignedDistance(p) < 0.0f) return false;
        }
        return true;
    }
};
