#include "CoverSystem.h"

#include "GameModuleAPI.h" // RaycastHit, QueryFilter
#include "NavMesh.h"
#include "PhysicsWorld.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kCell = 4.0f;

bool SolidRay(const glm::vec3& from, const glm::vec3& dir, float dist, RaycastHit* out = nullptr) {
    const float o[3] = {from.x, from.y, from.z}, d[3] = {dir.x, dir.y, dir.z};
    QueryFilter f;
    f.HitTriggers = 0;
    RaycastHit hit;
    const bool h = PhysicsWorld::RaycastSolid(o, d, dist, f, hit) && hit.Hit;
    if (out) *out = hit;
    return h;
}

} // namespace

bool ClassifyCover(const CoverProbe& probe, CoverPoint& out) {
    if (!probe.BlockedKnee) return false; // nothing there, or too low to hide behind
    out.High = probe.BlockedHead;
    out.Peek[0] = out.High && probe.ClearPast[0];
    out.Peek[1] = out.High && probe.ClearPast[1];
    // High cover nobody can shoot round is only somewhere to hide, still worth having.
    return true;
}

long long CoverSystem::Cell(float x, float z) {
    const long long cx = (long long)std::floor(x / kCell), cz = (long long)std::floor(z / kCell);
    return (cx << 32) ^ (cz & 0xffffffffll);
}

void CoverSystem::Clear() {
    m_Points.clear();
    m_Grid.clear();
}

int CoverSystem::Build(const NavMesh& nav, const CoverTuning& tune) {
    Clear();
    std::vector<NavMesh::Edge> edges;
    nav.BoundaryEdges(edges);
    for (const NavMesh::Edge& e : edges) {
        glm::vec3 along = e.B - e.A;
        along.y = 0.0f;
        const float len = glm::length(along);
        if (len < 0.5f) continue;
        along /= len;
        const int n = std::max(1, (int)std::floor(len / tune.Spacing));
        for (int i = 0; i < n; ++i) {
            const float t = (i + 0.5f) / (float)n;
            const glm::vec3 p = e.A + (e.B - e.A) * t;
            CoverProbe probe;
            probe.BlockedKnee = SolidRay(p + glm::vec3(0, tune.Knee, 0), e.Out, tune.Reach);
            probe.BlockedHead = SolidRay(p + glm::vec3(0, tune.Head, 0), e.Out, tune.Reach);
            // Facing the cover (Out), its left is Out rotated +90 about up: (Out.z, 0, -Out.x) is the right.
            const glm::vec3 right(-e.Out.z, 0.0f, e.Out.x);
            const glm::vec3 side[2] = {-right, right};
            glm::vec3 peekPos[2];
            if (probe.BlockedHead) {
                for (int s = 0; s < 2; ++s) {
                    const glm::vec3 q = p + side[s] * tune.Step;
                    glm::vec3 snapped;
                    const bool onMesh = nav.Closest(q, snapped, glm::vec3(0.3f, 0.6f, 0.3f)) &&
                                        glm::length(glm::vec2(snapped.x - q.x, snapped.z - q.z)) < 0.25f;
                    // Clear past the end, and nothing in the way of the step itself.
                    probe.ClearPast[s] = onMesh && !SolidRay(q + glm::vec3(0, tune.Head, 0), e.Out, tune.Reach + 0.6f) &&
                                         !SolidRay(p + glm::vec3(0, tune.Head, 0), side[s], tune.Step);
                    peekPos[s] = onMesh ? snapped : q;
                }
            }
            CoverPoint c;
            if (!ClassifyCover(probe, c)) continue;
            c.Pos = p;
            c.Normal = e.Out;
            c.PeekPos[0] = peekPos[0];
            c.PeekPos[1] = peekPos[1];
            // Two samples this close facing the same way are one place to stand.
            bool dup = false;
            for (const CoverPoint& o : m_Points)
                if (glm::length(o.Pos - c.Pos) < 0.7f && glm::dot(o.Normal, c.Normal) > 0.7f) { dup = true; break; }
            if (!dup) m_Points.push_back(c);
        }
    }
    for (int i = 0; i < (int)m_Points.size(); ++i) m_Grid[Cell(m_Points[(size_t)i].Pos.x, m_Points[(size_t)i].Pos.z)].push_back(i);
    return (int)m_Points.size();
}

void CoverSystem::Query(const glm::vec3& centre, float radius, std::vector<int>& out) const {
    out.clear();
    const int r = (int)std::ceil(radius / kCell);
    const long long cx = (long long)std::floor(centre.x / kCell), cz = (long long)std::floor(centre.z / kCell);
    for (long long x = cx - r; x <= cx + r; ++x)
        for (long long z = cz - r; z <= cz + r; ++z) {
            const auto it = m_Grid.find((x << 32) ^ (z & 0xffffffffll));
            if (it == m_Grid.end()) continue;
            for (int i : it->second) {
                const glm::vec3& p = m_Points[(size_t)i].Pos;
                if ((p.x - centre.x) * (p.x - centre.x) + (p.z - centre.z) * (p.z - centre.z) <= radius * radius) out.push_back(i);
            }
        }
}

bool CoverSystem::Claim(int index, int npc, float now) {
    if (index < 0 || index >= (int)m_Points.size()) return false;
    CoverPoint& c = m_Points[(size_t)index];
    if (c.ClaimedBy >= 0 && c.ClaimedBy != npc) return false;
    Release(npc, now);
    c.ClaimedBy = npc;
    // Neighbours within a body's width are taken too (two soldiers don't share a crate's corner).
    return true;
}

void CoverSystem::Release(int npc, float now) {
    for (CoverPoint& c : m_Points)
        if (c.ClaimedBy == npc) { c.ClaimedBy = -1; c.LastUsed = now; }
}

int CoverSystem::ClaimOf(int npc) const {
    for (int i = 0; i < (int)m_Points.size(); ++i)
        if (m_Points[(size_t)i].ClaimedBy == npc) return i;
    return -1;
}

bool CoverSystem::Shielded(const glm::vec3& feet, float height, const glm::vec3& threatEye) {
    const glm::vec3 target = feet + glm::vec3(0.0f, height, 0.0f);
    glm::vec3 d = target - threatEye;
    const float dist = glm::length(d);
    if (dist < 0.5f) return false;
    d /= dist;
    RaycastHit hit;
    return SolidRay(threatEye, d, dist, &hit) && hit.Distance < dist - 0.25f;
}
