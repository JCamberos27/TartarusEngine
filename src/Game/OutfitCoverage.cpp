#include "OutfitCoverage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace OutfitCoverage {
namespace {

// Segment p->p+d (t in [0,1]) against a triangle, Möller-Trumbore, two-sided.
bool SegmentHitsTriangle(const glm::vec3& p, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                         float* at = nullptr) {
    const glm::vec3 e1 = b - a, e2 = c - a;
    const glm::vec3 h = glm::cross(d, e2);
    const float det = glm::dot(e1, h);
    if (std::abs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const glm::vec3 s = p - a;
    const float u = glm::dot(s, h) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = glm::dot(e2, q) * inv;
    if (at) *at = t;
    return t >= 0.0f && t <= 1.0f;
}

// The cloth's triangles binned in a uniform grid.
struct Grid {
    glm::vec3 Min{0.0f};
    float Cell = 0.03f;
    glm::ivec3 Size{0};
    std::vector<std::vector<int>> Cells;

    int Index(const glm::ivec3& c) const { return (c.z * Size.y + c.y) * Size.x + c.x; }
    glm::ivec3 CellOf(const glm::vec3& p) const {
        return glm::clamp(glm::ivec3(glm::floor((p - Min) / Cell)), glm::ivec3(0), Size - 1);
    }
};

Grid Build(const Mesh& cloth, float margin) {
    Grid g;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const auto& p : cloth.Positions) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    g.Min = lo - glm::vec3(margin);
    const glm::vec3 extent = hi - lo + glm::vec3(2.0f * margin);
    // About 3 cm cells, but never more than ~64^3 of them.
    g.Cell = std::max({0.03f, extent.x / 64.0f, extent.y / 64.0f, extent.z / 64.0f});
    g.Size = glm::max(glm::ivec3(glm::ceil(extent / g.Cell)), glm::ivec3(1));
    g.Cells.assign((size_t)g.Size.x * g.Size.y * g.Size.z, {});
    for (size_t t = 0; t + 2 < cloth.Indices.size(); t += 3) {
        const glm::vec3& a = cloth.Positions[cloth.Indices[t]];
        const glm::vec3& b = cloth.Positions[cloth.Indices[t + 1]];
        const glm::vec3& c = cloth.Positions[cloth.Indices[t + 2]];
        const glm::ivec3 c0 = g.CellOf(glm::min(a, glm::min(b, c))), c1 = g.CellOf(glm::max(a, glm::max(b, c)));
        for (int z = c0.z; z <= c1.z; ++z)
            for (int y = c0.y; y <= c1.y; ++y)
                for (int x = c0.x; x <= c1.x; ++x) g.Cells[(size_t)g.Index({x, y, z})].push_back((int)(t / 3));
    }
    return g;
}

} // namespace

std::vector<glm::vec3> VertexNormals(const Mesh& mesh) {
    std::vector<glm::vec3> n(mesh.Positions.size(), glm::vec3(0.0f));
    for (size_t t = 0; t + 2 < mesh.Indices.size(); t += 3) {
        const unsigned i0 = mesh.Indices[t], i1 = mesh.Indices[t + 1], i2 = mesh.Indices[t + 2];
        if (i0 >= n.size() || i1 >= n.size() || i2 >= n.size()) continue;
        const glm::vec3 face = glm::cross(mesh.Positions[i1] - mesh.Positions[i0], mesh.Positions[i2] - mesh.Positions[i0]);
        n[i0] += face; n[i1] += face; n[i2] += face; // length = 2x area: area-weighted
    }
    for (auto& v : n) {
        const float l = glm::length(v);
        v = l > 1e-12f ? v / l : glm::vec3(0.0f);
    }
    return n;
}

std::vector<std::uint8_t> Covered(const Mesh& body, const Mesh& cloth, const Settings& settings) {
    std::vector<std::uint8_t> out(body.Positions.size(), 0);
    if (body.Positions.empty() || cloth.Positions.empty() || cloth.Indices.size() < 3) return out;
    const Grid grid = Build(cloth, settings.Outward + settings.Inward);
    const std::vector<glm::vec3> normals = VertexNormals(body);
    std::vector<int> stamp(cloth.Indices.size() / 3, -1);
    for (size_t v = 0; v < body.Positions.size(); ++v) {
        const glm::vec3& n = normals[v];
        if (n == glm::vec3(0.0f)) continue;
        const glm::vec3 from = body.Positions[v] - n * settings.Inward;
        const glm::vec3 d = n * (settings.Inward + settings.Outward);
        const glm::ivec3 c0 = grid.CellOf(glm::min(from, from + d)), c1 = grid.CellOf(glm::max(from, from + d));
        bool hit = false;
        for (int z = c0.z; z <= c1.z && !hit; ++z)
            for (int y = c0.y; y <= c1.y && !hit; ++y)
                for (int x = c0.x; x <= c1.x && !hit; ++x)
                    for (int tri : grid.Cells[(size_t)grid.Index({x, y, z})]) {
                        if (stamp[(size_t)tri] == (int)v) continue;
                        stamp[(size_t)tri] = (int)v;
                        const size_t t = (size_t)tri * 3;
                        if (SegmentHitsTriangle(from, d, cloth.Positions[cloth.Indices[t]], cloth.Positions[cloth.Indices[t + 1]],
                                                cloth.Positions[cloth.Indices[t + 2]])) { hit = true; break; }
                    }
        out[v] = hit ? 1 : 0;
    }
    return out;
}

namespace {

// The first triangle of `mesh` the segment p->p+d meets (t in (minT, 1]), skipping those with vertex
// `skip` (its own); its t and how far its face turns toward `n`. False when there's none.
bool FirstHit(const Mesh& mesh, const Grid& grid, const glm::vec3& p, const glm::vec3& d, const glm::vec3& n, float minT,
              unsigned skip, float& outT, float& outFacing) {
    const glm::ivec3 c0 = grid.CellOf(glm::min(p, p + d)), c1 = grid.CellOf(glm::max(p, p + d));
    outT = 2.0f;
    for (int z = c0.z; z <= c1.z; ++z)
        for (int y = c0.y; y <= c1.y; ++y)
            for (int x = c0.x; x <= c1.x; ++x)
                for (int tri : grid.Cells[(size_t)grid.Index({x, y, z})]) {
                    const size_t t = (size_t)tri * 3;
                    const unsigned i0 = mesh.Indices[t], i1 = mesh.Indices[t + 1], i2 = mesh.Indices[t + 2];
                    if (i0 == skip || i1 == skip || i2 == skip) continue;
                    const glm::vec3 &a = mesh.Positions[i0], &b = mesh.Positions[i1], &c = mesh.Positions[i2];
                    float hit;
                    if (!SegmentHitsTriangle(p, d, a, b, c, &hit) || hit <= minT || hit >= outT) continue;
                    outT = hit;
                    const glm::vec3 fn = glm::cross(b - a, c - a);
                    const float l = glm::length(fn);
                    outFacing = l > 1e-12f ? glm::dot(fn / l, n) : 0.0f;
                }
    return outT <= 1.0f;
}

} // namespace

std::vector<float> PokeDepth(const Mesh& body, const Mesh& cloth, float maxDepth) {
    std::vector<float> out(body.Positions.size(), 0.0f);
    if (body.Positions.empty() || cloth.Positions.empty() || cloth.Indices.size() < 3) return out;
    const float selfReach = maxDepth + kFarWallSlack; // the far side is looked for a little past the cloth's reach
    const Grid grid = Build(cloth, maxDepth), self = Build(body, selfReach);
    const std::vector<glm::vec3> normals = VertexNormals(body);
    const float minSelf = 0.001f / selfReach; // closer than a millimetre is the vertex's own surface
    for (size_t v = 0; v < body.Positions.size(); ++v) {
        const glm::vec3& n = normals[v];
        if (n == glm::vec3(0.0f)) continue;
        const glm::vec3 p = body.Positions[v], d = -n * maxDepth;
        float t, facing;
        // The cloth just behind the vertex, facing out the way it does: the vertex is outside it...
        if (!FirstHit(cloth, grid, p, d, n, 0.0f, ~0u, t, facing) || facing <= 0.3f) continue;
        // ... unless the look back left the piece's own volume first (through a head to a hood's far side), or
        // all but, deep in: cloth just short of a far wall kFarWallDepth or more away is cloth sunk into the
        // far side of a limb (the Hawaiian shirt's loose sleeve dips ~1 cm into the underside of the arm in the
        // bind pose, ~9.5 cm in), not the skin outside it - the whole arm read as poking through, and the
        // hidden skin past the hem opened a hole. Not near: an ear is thinner than the slack, and one poking
        // through a balaclava must still go.
        float selfT, selfFacing;
        if (FirstHit(body, self, p, -n * selfReach, n, minSelf, (unsigned)v, selfT, selfFacing) && selfFacing < -0.3f) {
            const float clothAt = t * maxDepth, wallAt = selfT * selfReach;
            if (wallAt < clothAt || (clothAt >= kFarWallDepth && wallAt < clothAt + kFarWallSlack)) continue;
        }
        out[v] = t * maxDepth;
    }
    return out;
}

void Erode(const Mesh& body, std::vector<std::uint8_t>& covered, int rings) {
    for (int r = 0; r < rings; ++r) {
        std::vector<std::uint8_t> next = covered;
        for (size_t t = 0; t + 2 < body.Indices.size(); t += 3) {
            const unsigned i[3] = {body.Indices[t], body.Indices[t + 1], body.Indices[t + 2]};
            if (i[0] >= covered.size() || i[1] >= covered.size() || i[2] >= covered.size()) continue;
            if (covered[i[0]] && covered[i[1]] && covered[i[2]]) continue;
            next[i[0]] = next[i[1]] = next[i[2]] = 0; // a triangle on the edge uncovers all its corners
        }
        covered.swap(next);
    }
}

namespace {

// Points bucketed in cells `radius` wide: Near asks whether any lies within `radius` of p.
struct PointGrid {
    float Radius;
    std::unordered_map<std::uint64_t, std::vector<glm::vec3>> Cells;
    explicit PointGrid(float radius) : Radius(radius) {}
    glm::ivec3 CellOf(const glm::vec3& p) const { return glm::ivec3(glm::floor(p / Radius)); }
    static std::uint64_t Key(const glm::ivec3& c) {
        return ((std::uint64_t)(std::uint32_t)(c.x + (1 << 20)) << 42) ^ ((std::uint64_t)(std::uint32_t)(c.y + (1 << 20)) << 21) ^
               (std::uint64_t)(std::uint32_t)(c.z + (1 << 20));
    }
    void Add(const glm::vec3& p) { Cells[Key(CellOf(p))].push_back(p); }
    bool Empty() const { return Cells.empty(); }
    bool Near(const glm::vec3& p) const {
        const glm::ivec3 c = CellOf(p);
        const float r2 = Radius * Radius;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto it = Cells.find(Key(c + glm::ivec3(dx, dy, dz)));
                    if (it == Cells.end()) continue;
                    for (const glm::vec3& q : it->second)
                        if (glm::dot(q - p, q - p) < r2) return true;
                }
        return false;
    }
};

} // namespace

void ErodeWithin(const Mesh& body, std::vector<std::uint8_t>& covered, float radius, const std::vector<std::uint8_t>* only) {
    if (radius <= 0.0f) return;
    // The uncovered vertices of the triangles on the edge are the band's inner line.
    PointGrid edge(radius);
    for (size_t t = 0; t + 2 < body.Indices.size(); t += 3) {
        const unsigned i[3] = {body.Indices[t], body.Indices[t + 1], body.Indices[t + 2]};
        if (i[0] >= covered.size() || i[1] >= covered.size() || i[2] >= covered.size()) continue;
        const int c = covered[i[0]] + covered[i[1]] + covered[i[2]];
        if (c == 0 || c == 3) continue;
        for (unsigned v : i)
            if (!covered[v]) edge.Add(body.Positions[v]);
    }
    if (edge.Empty()) return;
    std::vector<std::uint8_t> next = covered;
    for (size_t v = 0; v < covered.size(); ++v)
        if (covered[v] && (!only || (v < only->size() && (*only)[v])) && edge.Near(body.Positions[v])) next[v] = 0;
    covered.swap(next);
}

namespace {

// The grid's cells the segment p->p+d passes through, in order (Amanatides-Woo), each handed to `visit`
// (its index) until that returns true. The segment is clipped to the grid first.
template <class Visit>
void Walk(const Grid& g, const glm::vec3& p, const glm::vec3& d, Visit&& visit) {
    const glm::vec3 lo = g.Min, hi = g.Min + glm::vec3(g.Size) * g.Cell;
    float t0 = 0.0f, t1 = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-12f) {
            if (p[i] < lo[i] || p[i] > hi[i]) return;
            continue;
        }
        float a = (lo[i] - p[i]) / d[i], b = (hi[i] - p[i]) / d[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    if (t0 > t1) return;
    glm::ivec3 c = g.CellOf(p + d * t0), step(0);
    glm::vec3 tMax(1e30f), tDelta(1e30f);
    for (int i = 0; i < 3; ++i) {
        if (d[i] > 0.0f) {
            step[i] = 1;
            tMax[i] = (g.Min[i] + (float)(c[i] + 1) * g.Cell - p[i]) / d[i];
            tDelta[i] = g.Cell / d[i];
        } else if (d[i] < 0.0f) {
            step[i] = -1;
            tMax[i] = (g.Min[i] + (float)c[i] * g.Cell - p[i]) / d[i];
            tDelta[i] = -g.Cell / d[i];
        }
    }
    for (;;) {
        if (visit(g.Index(c))) return;
        const int a = tMax.x < tMax.y ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
        if (tMax[a] > t1) return;
        c[a] += step[a];
        if (c[a] < 0 || c[a] >= g.Size[a]) return;
        tMax[a] += tDelta[a];
    }
}

// Whether p->p+d meets a triangle of `mesh` (past minT, not one of vertex `skip`'s own); with `facing`, only
// one whose front turns that way (a back face isn't drawn).
bool AnyHit(const Mesh& mesh, const Grid& grid, const glm::vec3& p, const glm::vec3& d, float minT, unsigned skip,
            const glm::vec3* facing = nullptr) {
    bool hit = false;
    Walk(grid, p, d, [&](int cell) {
        for (int tri : grid.Cells[(size_t)cell]) {
            const size_t t = (size_t)tri * 3;
            const unsigned i0 = mesh.Indices[t], i1 = mesh.Indices[t + 1], i2 = mesh.Indices[t + 2];
            if (i0 == skip || i1 == skip || i2 == skip) continue;
            const glm::vec3 &a = mesh.Positions[i0], &b = mesh.Positions[i1], &c = mesh.Positions[i2];
            if (facing && glm::dot(glm::cross(b - a, c - a), *facing) <= 0.0f) continue;
            float at;
            if (SegmentHitsTriangle(p, d, a, b, c, &at) && at > minT) {
                hit = true;
                return true;
            }
        }
        return false;
    });
    return hit;
}

} // namespace

std::vector<std::uint8_t> Backed(const Mesh& body, const Mesh& cloth, const std::vector<std::uint8_t>& only, float reach) {
    std::vector<std::uint8_t> out(body.Positions.size(), 0);
    if (body.Positions.empty() || cloth.Positions.empty() || cloth.Indices.size() < 3) return out;
    const Grid clothGrid = Build(cloth, reach), bodyGrid = Build(body, reach);
    const std::vector<glm::vec3> normals = VertexNormals(body);
    // The looks, spread evenly over a hemisphere about +Z (a Fibonacci spiral), out to ~78 degrees off the
    // normal: past that the view catches only a sliver of the skin, edge on.
    std::vector<glm::vec3> looks;
    for (int k = 0; k < kBackedRays; ++k) {
        const float z = 1.0f - 0.8f * ((float)k + 0.5f) / (float)kBackedRays; // cos from ~1 down to 0.2
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z)), a = 2.39996323f * (float)k; // golden angle
        looks.emplace_back(r * std::cos(a), r * std::sin(a), z);
    }
    const float minT = 0.001f / reach; // closer than a millimetre is the vertex's own surface
    for (size_t v = 0; v < body.Positions.size(); ++v) {
        if (!only.empty() && (v >= only.size() || !only[v])) continue;
        const glm::vec3& n = normals[v];
        if (n == glm::vec3(0.0f)) continue;
        const glm::vec3 t = glm::normalize(glm::cross(n, std::abs(n.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0)));
        const glm::vec3 b = glm::cross(n, t), p = body.Positions[v];
        bool backed = true;
        for (const glm::vec3& l : looks) {
            const glm::vec3 dir = l.x * t + l.y * b + l.z * n, d = dir * reach;
            // Seen from out along `dir` only if nothing's in the way...
            if (AnyHit(cloth, clothGrid, p, d, 0.0f, ~0u) || AnyHit(body, bodyGrid, p, d, minT, (unsigned)v)) continue;
            // ... and then, without it, the view goes on through: cloth (drawn both sides) or the body's
            // outside must be right there to see instead - found far down inside a shirt, it's a dark hole.
            const glm::vec3 behind = -dir * kBackedBehind;
            // Only a surface facing the view counts: past a collar's rim the view meets the collar's inside
            // (drawn, but dark) - a hole all the same.
            if (AnyHit(cloth, clothGrid, p, behind, 0.0f, ~0u, &dir) || AnyHit(body, bodyGrid, p, behind, 0.001f / kBackedBehind, (unsigned)v, &dir))
                continue;
            backed = false;
            break;
        }
        out[v] = backed ? 1 : 0;
    }
    return out;
}

std::vector<std::uint8_t> Hidden(const Mesh& under, const Mesh& over, bool exposed, bool rigid) {
    if (rigid) {
        const std::vector<float> poke = PokeDepth(under, over, kPokeReach);
        std::vector<std::uint8_t> out(poke.size(), 0);
        for (size_t v = 0; v < poke.size(); ++v) out[v] = poke[v] > 0.0f;
        return out;
    }
    // Covered from outside: a band inside the edge stays, so a hem never opens a hole. Poking out: all of it
    // goes - the cloth is right behind it.
    // The band only draws skin that's under the cloth (it's there outward of it): skin a little outside it
    // counts as covered too (Settings::Inward), and drawn, it shows. Not on the head (`exposed`): what's worn
    // there rides the head bone and can't swing off it, and the head is full of small uncovered spots (ear
    // canals, eye sockets, the mouth) - a band round each drew the ears, out through the balaclava.
    std::vector<std::uint8_t> covered = Covered(under, over);
    Erode(under, covered, 1);
    if (!exposed) {
        Settings outwardOnly;
        outwardOnly.Inward = 0.0f;
        const std::vector<std::uint8_t> underCloth = Covered(under, over, outwardOnly);
        ErodeWithin(under, covered, kEdgeBand, &underCloth);
    }
    const std::vector<float> poke = PokeDepth(under, over, kPokeReach);
    for (size_t v = 0; v < covered.size(); ++v)
        if (poke[v] > 0.0f) covered[v] = 1;
    if (exposed) {
        const std::vector<std::uint8_t> backed = Backed(under, over, covered);
        for (size_t v = 0; v < covered.size(); ++v) covered[v] &= backed[v];
    }
    return covered;
}

std::vector<std::uint32_t> Pack(const std::vector<std::uint8_t>& covered) {
    std::vector<std::uint32_t> bits((covered.size() + 31) / 32, 0u);
    for (size_t i = 0; i < covered.size(); ++i)
        if (covered[i]) bits[i >> 5] |= 1u << (i & 31);
    return bits;
}

} // namespace OutfitCoverage
