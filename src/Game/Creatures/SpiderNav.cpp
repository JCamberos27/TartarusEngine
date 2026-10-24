#include "SpiderNav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <queue>
#include <unordered_set>

namespace Spider {

namespace {

enum class Surface { Floor, Wall, Ceiling };
Surface KindOf(const glm::vec3& n) { return n.y > 0.7f ? Surface::Floor : n.y < -0.7f ? Surface::Ceiling : Surface::Wall; }

float CostOf(const PathCosts& c, const glm::vec3& n) {
    switch (KindOf(n)) {
        case Surface::Floor: return std::max(1.0f, c.Floor);
        case Surface::Ceiling: return std::max(1.0f, c.Ceiling);
        default: return std::max(1.0f, c.Wall);
    }
}

// A lattice point's identity: where it is (to a quarter of the spacing) and which way its surface faces (one of six).
long long PointKey(const glm::vec3& p, const glm::vec3& n, float spacing) {
    const float q = spacing * 0.25f;
    // Rounded, not floored: a lattice point sits exactly on a bin's middle, and two triangles' arithmetic may land it a
    // hair either side.
    const long long x = (long long)std::floor(p.x / q + 0.5f) & 0xFFFFF, y = (long long)std::floor(p.y / q + 0.5f) & 0xFFFFF,
                    z = (long long)std::floor(p.z / q + 0.5f) & 0xFFFFF;
    int axis = 0;
    for (int a = 1; a < 3; ++a)
        if (std::fabs(n[a]) > std::fabs(n[axis])) axis = a;
    const long long face = axis * 2 + (n[axis] < 0.0f ? 1 : 0);
    return (x << 43) ^ (y << 23) ^ (z << 3) ^ face;
}

} // namespace

glm::vec3 RouteAim(const std::vector<glm::vec3>& route, const std::vector<glm::vec3>& normals, int& at, const glm::vec3& pos,
                   const glm::vec3& up, const glm::vec3& goal) {
    const int n = (int)std::min(route.size(), normals.size());
    if (n == 0) return goal;
    at = std::clamp(at, 0, n - 1);
    while (at < n - 1 && glm::length(pos - route[(size_t)at]) < 0.35f) ++at;
    int aim = at;
    for (int k = at; k < n && k < at + 8; ++k) {
        if (glm::dot(normals[(size_t)k], up) < 0.95f) break;
        aim = k;
    }
    return aim >= n - 1 ? goal : route[(size_t)aim];
}

void SurfaceGraph::Block(int i, int delta) {
    if (i < 0 || i >= (int)m_Pos.size()) return;
    if (m_Blocked.size() != m_Pos.size()) m_Blocked.assign(m_Pos.size(), 0);
    m_Blocked[(size_t)i] = std::max(0, m_Blocked[(size_t)i] + delta);
}

void SurfaceGraph::AddPenalty(int i, float metres) {
    if (i < 0 || i >= (int)m_Pos.size()) return;
    if (m_Penalty.size() != m_Pos.size()) m_Penalty.assign(m_Pos.size(), 0.0f);
    m_Penalty[(size_t)i] = std::max(0.0f, m_Penalty[(size_t)i] + metres);
}

void SurfaceGraph::Clear() {
    m_Pos.clear();
    m_Normal.clear();
    m_First.clear();
    m_Links.clear();
    m_Grid.clear();
    m_Comp.clear();
    m_Blocked.clear();
    m_Penalty.clear();
}

long long SurfaceGraph::CellKey(const glm::vec3& p) const {
    const long long x = (long long)std::floor(p.x / m_Cell) & 0x1FFFFF, y = (long long)std::floor(p.y / m_Cell) & 0x1FFFFF,
                    z = (long long)std::floor(p.z / m_Cell) & 0x1FFFFF;
    return (x << 42) | (y << 21) | z;
}

void SurfaceGraph::BuildGrid() {
    m_Grid.clear();
    m_Grid.reserve(m_Pos.size());
    for (int i = 0; i < (int)m_Pos.size(); ++i) m_Grid.push_back({CellKey(m_Pos[(size_t)i]), i});
    std::sort(m_Grid.begin(), m_Grid.end());
}

bool SurfaceGraph::Build(const std::vector<float>& verts, const std::vector<int>& tris, const SurfaceQuery& q, const NavSettings& s,
                         const std::vector<glm::vec3>* seeds, float seedRadius) {
    Clear();
    if (tris.size() < 3 || s.Spacing <= 0.01f) return false;
    const float sp = s.Spacing;
    auto vert = [&](int i) { return glm::vec3(verts[(size_t)i * 3], verts[(size_t)i * 3 + 1], verts[(size_t)i * 3 + 2]); };
    auto nearSeed = [&](const glm::vec3& p) {
        if (!seeds || seeds->empty()) return true;
        for (const glm::vec3& sd : *seeds)
            if (glm::length(p - sd) <= seedRadius) return true;
        return false;
    };

    // Nodes: each triangle's points on the world lattice of its dominant axis, so coplanar triangles share points.
    std::unordered_set<long long> seen;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        const glm::vec3 a = vert(tris[t]), b = vert(tris[t + 1]), c = vert(tris[t + 2]);
        glm::vec3 n = glm::cross(b - a, c - a);
        const float area2 = glm::length(n);
        if (area2 < 1e-8f) continue;
        n /= area2;
        if (seeds && !seeds->empty()) {
            const glm::vec3 lo = glm::min(a, glm::min(b, c)), hi = glm::max(a, glm::max(b, c));
            bool any = false;
            for (const glm::vec3& sd : *seeds)
                any = any || glm::length(glm::clamp(sd, lo, hi) - sd) <= seedRadius;
            if (!any) continue;
        }
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (std::fabs(n[k]) > std::fabs(n[axis])) axis = k;
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const glm::vec2 A(a[u], a[v]), B(b[u], b[v]), C(c[u], c[v]);
        const float det = (B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y);
        if (std::fabs(det) < 1e-9f) continue;
        const glm::vec2 lo = glm::min(A, glm::min(B, C)), hi = glm::max(A, glm::max(B, C));
        for (float pu = (std::floor(lo.x / sp) + 0.5f) * sp; pu <= hi.x; pu += sp)
            for (float pv = (std::floor(lo.y / sp) + 0.5f) * sp; pv <= hi.y; pv += sp) {
                // Barycentric in the projection, a hair outside the edges allowed so shared edges aren't missed.
                const float w1 = ((B.y - C.y) * (pu - C.x) + (C.x - B.x) * (pv - C.y)) / det;
                const float w2 = ((C.y - A.y) * (pu - C.x) + (A.x - C.x) * (pv - C.y)) / det;
                const float w3 = 1.0f - w1 - w2;
                if (w1 < -1e-4f || w2 < -1e-4f || w3 < -1e-4f) continue;
                const glm::vec3 p = a * w1 + b * w2 + c * w3;
                if (!nearSeed(p)) continue;
                // The open side: a box's faces, a wall's two sides, a slab's top and underside each count once.
                glm::vec3 face = n;
                if (q.Occupied(p + face * s.Room, s.RoomRadius)) {
                    face = -n;
                    if (q.Occupied(p + face * s.Room, s.RoomRadius)) continue;
                }
                // Footing: the same surface on at least three sides of it.
                {
                    const glm::vec3 t1 = glm::normalize(glm::cross(face, std::fabs(face.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
                    const glm::vec3 t2 = glm::cross(face, t1);
                    const glm::vec3 offs[4] = {t1 * s.Footing, -t1 * s.Footing, t2 * s.Footing, -t2 * s.Footing};
                    int feet = 0;
                    for (const glm::vec3& o : offs) {
                        SurfaceHit fh;
                        if (q.Ray(p + face * 0.08f + o, -face, 0.16f, fh) && std::fabs(fh.Distance - 0.08f) < 0.03f) ++feet;
                    }
                    if (feet < 3) continue;
                }
                if (!seen.insert(PointKey(p, face, sp)).second) continue;
                m_Pos.push_back(p);
                m_Normal.push_back(face);
            }
    }
    if (m_Pos.empty()) return false;

    // Links: neighbours within reach whose lifted line is clear (across a surface, or round a corner or an edge).
    m_Cell = s.LinkRadius;
    BuildGrid();
    std::vector<std::vector<int>> adj(m_Pos.size());
    for (int i = 0; i < (int)m_Pos.size(); ++i) {
        const glm::vec3 pi = m_Pos[(size_t)i];
        const glm::ivec3 c(glm::floor(pi / m_Cell));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const long long key = CellKey((glm::vec3(c + glm::ivec3(dx, dy, dz)) + 0.5f) * m_Cell);
                    auto it = std::lower_bound(m_Grid.begin(), m_Grid.end(), std::make_pair(key, -1));
                    for (; it != m_Grid.end() && it->first == key; ++it) {
                        const int j = it->second;
                        if (j <= i) continue;
                        const glm::vec3 pj = m_Pos[(size_t)j];
                        const float d = glm::length(pj - pi);
                        if (d > s.LinkRadius || d < 1e-4f) continue;
                        const glm::vec3 ni = m_Normal[(size_t)i], nj = m_Normal[(size_t)j];
                        if (glm::dot(ni, nj) < -0.2f) continue; // back to back: a wall's two sides
                        const glm::vec3 la = pi + ni * s.LinkLift, lb = pj + nj * s.LinkLift;
                        const float ll = glm::length(lb - la);
                        // Clear both ways: a mesh collider is one-sided, so a thin ceiling or floor plane stops a ray from
                        // its front only - checked one way, a wall's face linked up past the ceiling into the void above.
                        SurfaceHit h;
                        if (ll > 1e-4f && (q.Ray(la, (lb - la) / ll, ll, h) || q.Ray(lb, (la - lb) / ll, ll, h))) continue;
                        adj[(size_t)i].push_back(j);
                        adj[(size_t)j].push_back(i);
                    }
                }
    }
    m_First.assign(m_Pos.size() + 1, 0);
    for (size_t i = 0; i < adj.size(); ++i) m_First[i + 1] = m_First[i] + (int)adj[i].size();
    m_Links.reserve((size_t)m_First.back());
    for (const auto& a : adj) m_Links.insert(m_Links.end(), a.begin(), a.end());
    LabelComponents();
    return true;
}

void SurfaceGraph::LabelComponents() {
    m_Comp.assign(m_Pos.size(), -1);
    std::vector<int> open;
    int next = 0;
    for (int s = 0; s < (int)m_Pos.size(); ++s) {
        if (m_Comp[(size_t)s] >= 0) continue;
        m_Comp[(size_t)s] = next;
        open.push_back(s);
        while (!open.empty()) {
            const int i = open.back();
            open.pop_back();
            for (int k = m_First[(size_t)i]; k < m_First[(size_t)i + 1]; ++k)
                if (m_Comp[(size_t)m_Links[(size_t)k]] < 0) {
                    m_Comp[(size_t)m_Links[(size_t)k]] = next;
                    open.push_back(m_Links[(size_t)k]);
                }
        }
        ++next;
    }
}

int SurfaceGraph::Nearest(const glm::vec3& p, float maxDistance, const glm::vec3* up, int component) const {
    if (m_Pos.empty()) return -1;
    int best = -1;
    float bestD = maxDistance;
    const int reach = (int)std::ceil(maxDistance / m_Cell);
    const glm::ivec3 c(glm::floor(p / m_Cell));
    for (int dx = -reach; dx <= reach; ++dx)
        for (int dy = -reach; dy <= reach; ++dy)
            for (int dz = -reach; dz <= reach; ++dz) {
                const long long key = CellKey((glm::vec3(c + glm::ivec3(dx, dy, dz)) + 0.5f) * m_Cell);
                auto it = std::lower_bound(m_Grid.begin(), m_Grid.end(), std::make_pair(key, -1));
                for (; it != m_Grid.end() && it->first == key; ++it) {
                    const int i = it->second;
                    if (up && glm::dot(m_Normal[(size_t)i], *up) < 0.0f) continue;
                    if (component >= 0 && m_Comp[(size_t)i] != component) continue;
                    const float d = glm::length(m_Pos[(size_t)i] - p);
                    if (d < bestD) {
                        bestD = d;
                        best = i;
                    }
                }
            }
    return best;
}

bool SurfaceGraph::FindPath(int from, int to, const PathCosts& costs, std::vector<int>& out, int maxExpand) const {
    out.clear();
    const int n = (int)m_Pos.size();
    if (from < 0 || to < 0 || from >= n || to >= n) return false;
    if (from == to) {
        out.push_back(from);
        return true;
    }
    if (m_G.size() != (size_t)n) {
        m_G.assign((size_t)n, 0.0f);
        m_From.assign((size_t)n, -1);
        m_Stamp.assign((size_t)n, 0u);
    }
    if (++m_Search == 0) { // the stamp wrapped: every node fresh again
        std::fill(m_Stamp.begin(), m_Stamp.end(), 0u);
        m_Search = 1;
    }
    const glm::vec3 goal = m_Pos[(size_t)to];
    using Entry = std::pair<float, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    m_Stamp[(size_t)from] = m_Search;
    m_G[(size_t)from] = 0.0f;
    m_From[(size_t)from] = -1;
    open.push({glm::length(goal - m_Pos[(size_t)from]), from});
    int expanded = 0;
    while (!open.empty()) {
        const auto [f, cur] = open.top();
        open.pop();
        if (cur == to) break;
        const float g = m_G[(size_t)cur];
        if (f - glm::length(goal - m_Pos[(size_t)cur]) > g + 1e-4f) continue; // a stale entry
        if (++expanded > maxExpand) return false;
        for (int k = m_First[(size_t)cur]; k < m_First[(size_t)cur + 1]; ++k) {
            const int nb = m_Links[(size_t)k];
            if (nb != to && Blocked(nb)) continue;
            const float ng = g + glm::length(m_Pos[(size_t)nb] - m_Pos[(size_t)cur]) * CostOf(costs, m_Normal[(size_t)nb]) + Penalty(nb);
            if (m_Stamp[(size_t)nb] == m_Search && m_G[(size_t)nb] <= ng) continue;
            m_Stamp[(size_t)nb] = m_Search;
            m_G[(size_t)nb] = ng;
            m_From[(size_t)nb] = cur;
            open.push({ng + glm::length(goal - m_Pos[(size_t)nb]), nb});
        }
    }
    if (m_Stamp[(size_t)to] != m_Search) return false;
    for (int at = to; at >= 0; at = m_From[(size_t)at]) out.push_back(at);
    std::reverse(out.begin(), out.end());
    return true;
}

std::uint64_t SurfaceGraph::HashInput(const std::vector<float>& verts, const std::vector<int>& tris, const NavSettings& s,
                                      std::uint32_t buildVersion) {
    std::uint64_t h = 1469598103934665603ull; // FNV-1a over the input's bytes
    auto mix = [&](const void* data, size_t bytes) {
        const unsigned char* b = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(verts.data(), verts.size() * sizeof(float));
    mix(tris.data(), tris.size() * sizeof(int));
    mix(&s, sizeof s);
    mix(&buildVersion, sizeof buildVersion);
    return h;
}

std::uint64_t SurfaceGraph::HashBarriers(std::uint64_t h, const std::vector<Barrier>& barriers) {
    for (const Barrier& b : barriers) {
        const float f[10] = {b.Centre.x, b.Centre.y, b.Centre.z, b.Rotation.w, b.Rotation.x, b.Rotation.y, b.Rotation.z,
                             b.Half.x,   b.Half.y,   b.Half.z};
        const unsigned char* p = reinterpret_cast<const unsigned char*>(f);
        for (size_t i = 0; i < sizeof f; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

bool SurfaceGraph::Save(const std::string& path, std::uint64_t hash) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const std::uint32_t magic = 0x564E5053; // "SPNV"
    const std::uint32_t nodes = (std::uint32_t)m_Pos.size(), links = (std::uint32_t)m_Links.size();
    f.write((const char*)&magic, 4);
    f.write((const char*)&hash, 8);
    f.write((const char*)&m_Cell, 4);
    f.write((const char*)&nodes, 4);
    f.write((const char*)&links, 4);
    f.write((const char*)m_Pos.data(), (std::streamsize)(nodes * sizeof(glm::vec3)));
    f.write((const char*)m_Normal.data(), (std::streamsize)(nodes * sizeof(glm::vec3)));
    f.write((const char*)m_First.data(), (std::streamsize)((nodes + 1) * sizeof(int)));
    f.write((const char*)m_Links.data(), (std::streamsize)(links * sizeof(int)));
    return (bool)f;
}

bool SurfaceGraph::Load(const std::string& path, std::uint64_t hash) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::uint32_t magic = 0, nodes = 0, links = 0;
    std::uint64_t h = 0;
    float cell = 0.0f;
    f.read((char*)&magic, 4);
    f.read((char*)&h, 8);
    f.read((char*)&cell, 4);
    f.read((char*)&nodes, 4);
    f.read((char*)&links, 4);
    if (!f || magic != 0x564E5053 || h != hash || nodes == 0 || cell <= 0.0f) return false;
    Clear();
    m_Cell = cell;
    m_Pos.resize(nodes);
    m_Normal.resize(nodes);
    m_First.resize(nodes + 1);
    m_Links.resize(links);
    f.read((char*)m_Pos.data(), (std::streamsize)(nodes * sizeof(glm::vec3)));
    f.read((char*)m_Normal.data(), (std::streamsize)(nodes * sizeof(glm::vec3)));
    f.read((char*)m_First.data(), (std::streamsize)((nodes + 1) * sizeof(int)));
    f.read((char*)m_Links.data(), (std::streamsize)(links * sizeof(int)));
    if (!f || m_First.back() != (int)links) {
        Clear();
        return false;
    }
    BuildGrid();
    LabelComponents();
    return true;
}

} // namespace Spider
