#include "OutfitCoverage.h"

#include <algorithm>
#include <cmath>

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
    const Grid grid = Build(cloth, maxDepth), self = Build(body, maxDepth);
    const std::vector<glm::vec3> normals = VertexNormals(body);
    const float minSelf = 0.001f / maxDepth; // closer than a millimetre is the vertex's own surface
    for (size_t v = 0; v < body.Positions.size(); ++v) {
        const glm::vec3& n = normals[v];
        if (n == glm::vec3(0.0f)) continue;
        const glm::vec3 p = body.Positions[v], d = -n * maxDepth;
        float t, facing;
        // The cloth just behind the vertex, facing out the way it does: the vertex is outside it...
        if (!FirstHit(cloth, grid, p, d, n, 0.0f, ~0u, t, facing) || facing <= 0.3f) continue;
        // ... unless the look back left the piece's own volume first (through a head to a hood's far side).
        float selfT, selfFacing;
        if (FirstHit(body, self, p, d, n, minSelf, (unsigned)v, selfT, selfFacing) && selfT < t && selfFacing < -0.3f) continue;
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

std::vector<std::uint8_t> Hidden(const Mesh& under, const Mesh& over) {
    // Covered from outside: the edge ring stays, so a hem never opens a hole. Poking out: all of it
    // goes - the cloth is right behind it.
    std::vector<std::uint8_t> covered = Covered(under, over);
    Erode(under, covered, 1);
    const std::vector<float> poke = PokeDepth(under, over, kPokeReach);
    for (size_t v = 0; v < covered.size(); ++v)
        if (poke[v] > 0.0f) covered[v] = 1;
    return covered;
}

std::vector<std::uint32_t> Pack(const std::vector<std::uint8_t>& covered) {
    std::vector<std::uint32_t> bits((covered.size() + 31) / 32, 0u);
    for (size_t i = 0; i < covered.size(); ++i)
        if (covered[i]) bits[i >> 5] |= 1u << (i & 31);
    return bits;
}

} // namespace OutfitCoverage
