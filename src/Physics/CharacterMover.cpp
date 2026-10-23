#include "CharacterMover.h"

#include <algorithm>
#include <cmath>

namespace CharacterMover {
namespace {

constexpr glm::vec3 kUp(0.0f, 1.0f, 0.0f);

// Whether `hit`, touched by the capsule standing at `feet`, holds it up. A shallow enough surface that isn't a character
// does, while it touches within LedgeTolerance of the axis; touched further out (the capsule's rim over an edge) only if
// there is floor straight under the axis within a step. `surfaceY`: the height of the surface it stands on.
bool HoldsUp(IMoverWorld& world, const MoverCapsule& cap, const glm::vec3& feet, const MoverHit& hit, const MoverSettings& s,
             bool& perched, float* surfaceY = nullptr) {
    perched = false;
    if (hit.Character) return false;
    const glm::vec2 off(hit.Point.x - feet.x, hit.Point.z - feet.z);
    if (surfaceY) *surfaceY = hit.Point.y;
    if (hit.Normal.y < s.WalkableCos) {
        // The rounded bottom on an edge (a step's nose) reports the edge's normal, tilted toward the capsule: what decides
        // is the surface itself, a hair past the edge.
        const float ol = glm::length(off);
        const glm::vec3 out = ol > 1e-4f ? glm::vec3(off.x, 0.0f, off.y) / ol : glm::vec3(0.0f);
        MoverHit surface;
        if (!world.Raycast(hit.Point + out * 0.01f + kUp * 0.05f, -kUp, 0.1f, surface) || surface.Normal.y < s.WalkableCos) return false;
        if (surfaceY) *surfaceY = surface.Point.y;
    }
    if (glm::length(off) <= s.LedgeTolerance * cap.Radius) return true;
    // A needle down the axis, its foot where the capsule's is.
    const float r = std::min(0.05f, cap.Radius);
    const MoverCapsule needle{r, cap.HalfHeight + cap.Radius - r};
    MoverHit under;
    if (world.Sweep(needle, feet, -kUp, s.StepHeight + s.Skin, under) && !under.Character && under.Normal.y >= s.WalkableCos)
        return true;
    perched = true;
    return false;
}

// A climb onto whatever stopped the horizontal move `rem` at `p`: up as far as the room above allows (no higher than a
// step), on, then down onto it. Succeeds only onto floor that holds the capsule up; leaves `p` and `rem` alone otherwise.
bool TryStepUp(IMoverWorld& world, const MoverCapsule& cap, const MoverSettings& s, glm::vec3& p, glm::vec3& rem,
               float& stepDelta, float& headroom) {
    glm::vec3 fwd(rem.x, 0.0f, rem.z);
    const float len = glm::length(fwd);
    headroom = 0.0f;
    if (len < 1e-5f) return false;
    fwd /= len;
    MoverHit h;
    // Up: a door's header over the step clips the climb rather than calling it off.
    float climb = s.StepHeight;
    if (world.Sweep(cap, p, kUp, s.StepHeight + s.Skin, h)) climb = std::max(0.0f, h.Distance - s.Skin);
    headroom = climb;
    if (climb < 0.02f) return false;
    glm::vec3 q = p + kUp * climb;
    // On: it must get somewhere (a wall stays a wall at any height).
    float adv = len;
    if (world.Sweep(cap, q, fwd, len + s.Skin, h)) adv = std::clamp(h.Distance - s.Skin, 0.0f, len);
    if (adv < std::min(len, 0.005f) || adv <= 0.0f) return false;
    q += fwd * adv;
    // Down onto it.
    const float down = climb + 2.0f * s.Skin;
    if (!world.Sweep(cap, q, -kUp, down, h)) return false;
    glm::vec3 land = q - kUp * std::clamp(h.Distance - s.Skin, 0.0f, down);
    bool perched = false;
    float top = land.y;
    if (!HoldsUp(world, cap, land, h, s, perched, &top)) return false;
    if (top - p.y > s.StepHeight + 1e-3f) return false; // landed on the rim of something taller than a step
    // On the rim of the step's nose the feet are still below its top: lifted level with it (the room was just swept clear),
    // so the climb is one step, not a step and then a slide up over the edge.
    land.y = std::min(q.y, std::max(land.y, top + s.Skin));
    const float rise = land.y - p.y;
    if (rise > 0.01f) stepDelta += rise;
    p = land;
    rem = fwd * (len - adv);
    return true;
}

} // namespace

glm::vec3 ClipToPlanes(const glm::vec3& v, const glm::vec3* normals, int count) {
    constexpr float kEps = 1e-5f;
    glm::vec3 out = v;
    for (int i = 0; i < count; ++i) {
        const glm::vec3& ni = normals[i];
        if (glm::dot(out, ni) >= -kEps) continue;
        glm::vec3 slid = out - ni * glm::dot(out, ni);
        for (int j = 0; j < count; ++j) {
            if (j == i || glm::dot(slid, normals[j]) >= -kEps) continue;
            // Into a second plane as well: along the crease between the two, or stopped where there is none.
            glm::vec3 crease = glm::cross(ni, normals[j]);
            const float cl = glm::length(crease);
            if (cl < 1e-4f) return glm::vec3(0.0f);
            crease /= cl;
            slid = crease * glm::dot(out, crease);
            for (int k = 0; k < count; ++k)
                if (k != i && k != j && glm::dot(slid, normals[k]) < -kEps) return glm::vec3(0.0f); // a third: stopped
            break;
        }
        out = slid;
    }
    if (glm::dot(out, v) <= 0.0f) return glm::vec3(0.0f); // never back the way it came
    return out;
}

MoverFloor FindFloor(IMoverWorld& world, const MoverCapsule& cap, const glm::vec3& feet, float probe, const MoverSettings& s) {
    MoverFloor f;
    // The capsule itself (a wall alongside is a skin away, never met going straight down): the gap found is the one the feet
    // can be snapped across.
    MoverHit h;
    if (!world.Sweep(cap, feet, -kUp, std::max(probe, 0.0f), h)) return f;
    f.Found = true;
    f.Gap = h.Distance;
    f.Hit = h;
    f.Walkable = HoldsUp(world, cap, feet, h, s, f.Perched);
    return f;
}

MoverResult Move(IMoverWorld& world, const MoverCapsule& cap, const glm::vec3& feet, const glm::vec3& velocity,
                 const glm::vec3& delta, const MoverSettings& s, bool wasGrounded, bool snapToGround, MoverDebug* debug) {
    MoverResult res;
    if (debug) debug->Clear();
    auto record = [&](const MoverHit& h) {
        if (res.HitCount < MoverResult::kMaxHits) res.Hits[res.HitCount++] = h;
    };
    glm::vec3 p = feet;
    // Out of anything it was left inside (a door swung into it, a teleport, a resize).
    for (int i = 0; i < s.MaxDepenetrations; ++i) {
        glm::vec3 push;
        if (!world.Penetration(cap, p, push)) break;
        p += push;
    }

    // Across: slide along whatever is in the way.
    constexpr int kMaxPlanes = 8;
    glm::vec3 planes[kMaxPlanes];
    int planeCount = 0;
    const glm::vec3 across(delta.x, 0.0f, delta.z);
    glm::vec3 rem = across;
    for (int it = 0; it < s.MaxSlides; ++it) {
        const float len = glm::length(rem);
        if (len < 1e-6f) break;
        const glm::vec3 dir = rem / len;
        MoverHit hit;
        if (!world.Sweep(cap, p, dir, len + s.Skin, hit)) {
            p += rem;
            rem = glm::vec3(0.0f);
            break;
        }
        const float adv = std::clamp(hit.Distance - s.Skin, 0.0f, len);
        p += dir * adv;
        rem = dir * (len - adv);
        record(hit);
        if (!hit.Character && hit.Normal.y >= s.WalkableCos) {
            // A slope (or a step's rounded nose): up it, as far across as asked.
            if (debug) debug->Planes.push_back({hit.Point, hit.Normal, 1});
            if (wasGrounded && hit.Normal.y > 1e-3f) rem.y = -(hit.Normal.x * rem.x + hit.Normal.z * rem.z) / hit.Normal.y;
            else rem -= hit.Normal * glm::dot(rem, hit.Normal);
            continue;
        }
        if (wasGrounded && !hit.Character && hit.Normal.y > -0.1f) {
            float headroom = 0.0f;
            const glm::vec3 at = p;
            const bool taken = TryStepUp(world, cap, s, p, rem, res.StepDelta, headroom);
            if (debug) debug->Steps.push_back({at, headroom, taken});
            if (taken) continue;
        }
        // A wall: stood straight up, so nothing too steep is ever climbed by walking into it.
        glm::vec3 n(hit.Normal.x, 0.0f, hit.Normal.z);
        const float nl = glm::length(n);
        n = nl > 1e-4f ? n / nl : -glm::vec3(dir.x, 0.0f, dir.z);
        if (debug) debug->Planes.push_back({hit.Point, n, hit.Character ? 2 : 0});
        if (planeCount < kMaxPlanes) planes[planeCount++] = n;
        res.Flags |= MoverSides;
        const glm::vec3 clipped = ClipToPlanes(rem, planes, planeCount);
        if (debug && planeCount >= 2 && glm::length(clipped) > 1e-5f) {
            debug->Creases.push_back(p);
            debug->Creases.push_back(p + glm::normalize(clipped) * 0.3f);
        }
        if (glm::dot(clipped, across) <= 0.0f) break;
        rem = clipped;
    }

    // Up or down.
    const float vy = delta.y;
    if (vy > 1e-6f) {
        MoverHit h;
        if (world.Sweep(cap, p, kUp, vy + s.Skin, h)) {
            p.y += std::clamp(h.Distance - s.Skin, 0.0f, vy);
            res.Flags |= MoverUp;
            record(h);
        } else {
            p.y += vy;
        }
    } else if (vy < -1e-6f) {
        glm::vec3 r(0.0f, vy, 0.0f);
        for (int it = 0; it < 3; ++it) {
            const float len = glm::length(r);
            if (len < 1e-6f) break;
            const glm::vec3 dir = r / len;
            MoverHit h;
            if (!world.Sweep(cap, p, dir, len + s.Skin, h)) {
                p += r;
                break;
            }
            const float adv = std::clamp(h.Distance - s.Skin, 0.0f, len);
            p += dir * adv;
            r = dir * (len - adv);
            record(h);
            bool perched = false;
            if (HoldsUp(world, cap, p, h, s, perched)) {
                res.Flags |= MoverDown;
                break;
            }
            // Too steep, someone's head, or only the rim on an edge: down and off it.
            r -= h.Normal * glm::dot(r, h.Normal);
        }
    }

    // The floor: followed down a step or a slope while walking, and whether it holds the capsule up.
    const bool follow = snapToGround && wasGrounded && vy <= 0.0f && !(res.Flags & MoverUp);
    MoverFloor floor = FindFloor(world, cap, p, (follow ? s.StepDownSnap : 0.0f) + 2.0f * s.Skin, s);
    if (floor.Found && floor.Walkable) {
        const float drop = floor.Gap - s.Skin;
        if (follow && drop > 0.0f) {
            p.y -= drop;
            if (drop > 0.05f) res.StepDelta -= drop; // a step down (not a slope's every frame) eases the eye
            floor.Gap = s.Skin;
        }
        res.Grounded = floor.Gap <= 2.0f * s.Skin + 0.01f;
    }
    if (res.Grounded) res.Flags |= MoverDown;
    res.Floor = floor;
    res.Feet = p;

    // What's left of the velocity: none of it into anything it ran into.
    glm::vec3 across_v(velocity.x, 0.0f, velocity.z);
    across_v = ClipToPlanes(across_v, planes, planeCount);
    res.Velocity = glm::vec3(across_v.x, velocity.y, across_v.z);
    if ((res.Flags & MoverUp) && res.Velocity.y > 0.0f) res.Velocity.y = 0.0f;

    if (debug) {
        debug->Feet = p;
        debug->Floor = floor;
    }
    return res;
}

} // namespace CharacterMover
