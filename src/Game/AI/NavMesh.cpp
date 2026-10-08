#include "NavMesh.h"

#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <unordered_map>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <set>

namespace {

constexpr unsigned short kWalkFlag = 1;
constexpr int kMaxPath = 256;

// RAII for the Recast build intermediates.
struct RecastBuild {
    rcHeightfield* hf = nullptr;
    rcCompactHeightfield* chf = nullptr;
    rcContourSet* cset = nullptr;
    rcPolyMesh* pmesh = nullptr;
    rcPolyMeshDetail* dmesh = nullptr;
    ~RecastBuild() {
        rcFreeHeightField(hf);
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
    }
};

glm::vec3 V(const float* p) { return glm::vec3(p[0], p[1], p[2]); }

std::uint64_t Fnv(std::uint64_t h, const void* data, size_t n) {
    const auto* b = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

thread_local std::mt19937 t_NavRng{12345u};
float NavRand() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(t_NavRng); }

constexpr char kMagic[8] = {'T', 'N', 'A', 'V', 'M', 'S', 'H', '1'};

} // namespace

NavMesh::NavMesh() : m_Filter(std::make_unique<dtQueryFilter>()) {
    m_Filter->setIncludeFlags(kWalkFlag);
    m_Filter->setExcludeFlags(0);
}

NavMesh::~NavMesh() { Clear(); }

void NavMesh::Clear() {
    dtFreeNavMeshQuery(m_Query);
    dtFreeNavMesh(m_Mesh);
    m_Query = nullptr;
    m_Mesh = nullptr;
    m_Data.clear();
}

std::uint64_t NavMesh::HashInput(const std::vector<float>& verts, const std::vector<int>& tris, const NavBuildSettings& s) {
    std::uint64_t h = 1469598103934665603ull;
    h = Fnv(h, verts.data(), verts.size() * sizeof(float));
    h = Fnv(h, tris.data(), tris.size() * sizeof(int));
    h = Fnv(h, &s, sizeof(s));
    return h;
}

bool NavMesh::Build(const std::vector<float>& verts, const std::vector<int>& tris, const NavBuildSettings& s, std::string* error) {
    Clear();
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    const int nverts = (int)verts.size() / 3, ntris = (int)tris.size() / 3;
    if (nverts < 3 || ntris < 1) return fail("no geometry");

    rcContext ctx(false);
    rcConfig cfg{};
    cfg.cs = s.CellSize;
    cfg.ch = s.CellHeight;
    cfg.walkableSlopeAngle = s.AgentMaxSlope;
    cfg.walkableHeight = (int)std::ceil(s.AgentHeight / cfg.ch);
    cfg.walkableClimb = (int)std::floor(s.AgentClimb / cfg.ch);
    cfg.walkableRadius = (int)std::ceil(s.AgentRadius / cfg.cs);
    cfg.maxEdgeLen = (int)(s.EdgeMaxLen / cfg.cs);
    cfg.maxSimplificationError = s.EdgeMaxError;
    cfg.minRegionArea = (int)(s.RegionMinSize * s.RegionMinSize);
    cfg.mergeRegionArea = (int)(s.RegionMergeSize * s.RegionMergeSize);
    cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    cfg.detailSampleDist = s.DetailSampleDist < 0.9f ? 0.0f : cfg.cs * s.DetailSampleDist;
    cfg.detailSampleMaxError = cfg.ch * s.DetailSampleMaxError;
    rcCalcBounds(verts.data(), nverts, cfg.bmin, cfg.bmax);
    // Room overhead so a walker on the highest floor still has its height inside the bounds.
    cfg.bmax[1] += s.AgentHeight;
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    if (cfg.width <= 0 || cfg.height <= 0 || (long long)cfg.width * cfg.height > 4000ll * 4000ll) return fail("bad bounds");

    RecastBuild b;
    b.hf = rcAllocHeightfield();
    if (!b.hf || !rcCreateHeightfield(&ctx, *b.hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        return fail("heightfield");
    std::vector<unsigned char> areas((size_t)ntris, 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris, areas.data());
    if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), areas.data(), ntris, *b.hf, cfg.walkableClimb))
        return fail("rasterize");
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *b.hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *b.hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *b.hf);
    b.chf = rcAllocCompactHeightfield();
    if (!b.chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *b.hf, *b.chf))
        return fail("compact heightfield");
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *b.chf)) return fail("erode");
    if (!rcBuildDistanceField(&ctx, *b.chf)) return fail("distance field");
    if (!rcBuildRegions(&ctx, *b.chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) return fail("regions");
    b.cset = rcAllocContourSet();
    if (!b.cset || !rcBuildContours(&ctx, *b.chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *b.cset))
        return fail("contours");
    b.pmesh = rcAllocPolyMesh();
    if (!b.pmesh || !rcBuildPolyMesh(&ctx, *b.cset, cfg.maxVertsPerPoly, *b.pmesh)) return fail("poly mesh");
    b.dmesh = rcAllocPolyMeshDetail();
    if (!b.dmesh || !rcBuildPolyMeshDetail(&ctx, *b.pmesh, *b.chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *b.dmesh))
        return fail("detail mesh");
    if (b.pmesh->npolys == 0) return fail("nothing walkable");
    for (int i = 0; i < b.pmesh->npolys; ++i) b.pmesh->flags[i] = b.pmesh->areas[i] == RC_WALKABLE_AREA ? kWalkFlag : 0;

    dtNavMeshCreateParams p{};
    p.verts = b.pmesh->verts;
    p.vertCount = b.pmesh->nverts;
    p.polys = b.pmesh->polys;
    p.polyAreas = b.pmesh->areas;
    p.polyFlags = b.pmesh->flags;
    p.polyCount = b.pmesh->npolys;
    p.nvp = b.pmesh->nvp;
    p.detailMeshes = b.dmesh->meshes;
    p.detailVerts = b.dmesh->verts;
    p.detailVertsCount = b.dmesh->nverts;
    p.detailTris = b.dmesh->tris;
    p.detailTriCount = b.dmesh->ntris;
    p.walkableHeight = s.AgentHeight;
    p.walkableRadius = s.AgentRadius;
    p.walkableClimb = s.AgentClimb;
    rcVcopy(p.bmin, b.pmesh->bmin);
    rcVcopy(p.bmax, b.pmesh->bmax);
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    unsigned char* data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&p, &data, &size) || !data) return fail("navmesh data");
    m_Data.assign(data, data + size);
    if (!InitFromData(data, size)) return fail("navmesh init");
    return true;
}

bool NavMesh::InitFromData(unsigned char* data, int size) {
    m_Mesh = dtAllocNavMesh();
    if (!m_Mesh || dtStatusFailed(m_Mesh->init(data, size, DT_TILE_FREE_DATA))) {
        dtFree(data);
        dtFreeNavMesh(m_Mesh);
        m_Mesh = nullptr;
        return false;
    }
    m_Query = dtAllocNavMeshQuery();
    if (!m_Query || dtStatusFailed(m_Query->init(m_Mesh, 4096))) {
        Clear();
        return false;
    }
    const dtMeshTile* tile = static_cast<const dtNavMesh*>(m_Mesh)->getTile(0);
    if (tile && tile->header) {
        m_BMin = V(tile->header->bmin);
        m_BMax = V(tile->header->bmax);
    }
    return true;
}

bool NavMesh::Save(const std::string& path, std::uint64_t hash) const {
    if (m_Data.empty()) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const std::uint32_t n = (std::uint32_t)m_Data.size();
    f.write(kMagic, sizeof kMagic);
    f.write(reinterpret_cast<const char*>(&hash), sizeof hash);
    f.write(reinterpret_cast<const char*>(&n), sizeof n);
    f.write(reinterpret_cast<const char*>(m_Data.data()), n);
    return (bool)f;
}

bool NavMesh::Load(const std::string& path, std::uint64_t hash) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[8] = {};
    std::uint64_t h = 0;
    std::uint32_t n = 0;
    f.read(magic, sizeof magic);
    f.read(reinterpret_cast<char*>(&h), sizeof h);
    f.read(reinterpret_cast<char*>(&n), sizeof n);
    if (!f || std::memcmp(magic, kMagic, sizeof kMagic) != 0 || h != hash || n == 0 || n > (64u << 20)) return false;
    std::vector<unsigned char> bytes(n);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return false;
    Clear();
    auto* data = static_cast<unsigned char*>(dtAlloc((size_t)n, DT_ALLOC_PERM));
    if (!data) return false;
    std::memcpy(data, bytes.data(), n);
    m_Data = std::move(bytes);
    return InitFromData(data, (int)n);
}

bool NavMesh::Closest(const glm::vec3& p, glm::vec3& out, const glm::vec3& extents) const {
    if (!m_Query) return false;
    dtPolyRef ref = 0;
    float nearest[3];
    const float c[3] = {p.x, p.y, p.z}, e[3] = {extents.x, extents.y, extents.z};
    if (dtStatusFailed(m_Query->findNearestPoly(c, e, m_Filter.get(), &ref, nearest)) || !ref) return false;
    out = V(nearest);
    return true;
}

bool NavMesh::FindPath(const glm::vec3& a, const glm::vec3& b, std::vector<glm::vec3>& out, bool* partial) const {
    out.clear();
    if (partial) *partial = false;
    if (!m_Query) return false;
    const float ext[3] = {1.0f, 2.0f, 1.0f};
    const float pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z};
    dtPolyRef ra = 0, rb = 0;
    float sa[3], sb[3];
    if (dtStatusFailed(m_Query->findNearestPoly(pa, ext, m_Filter.get(), &ra, sa)) || !ra) return false;
    if (dtStatusFailed(m_Query->findNearestPoly(pb, ext, m_Filter.get(), &rb, sb)) || !rb) return false;
    dtPolyRef path[kMaxPath];
    int n = 0;
    const dtStatus st = m_Query->findPath(ra, rb, sa, sb, m_Filter.get(), path, &n, kMaxPath);
    if (dtStatusFailed(st) || n == 0) return false;
    float end[3];
    dtVcopy(end, sb);
    if (path[n - 1] != rb) {
        if (partial) *partial = true;
        bool posOverPoly = false;
        m_Query->closestPointOnPoly(path[n - 1], sb, end, &posOverPoly);
    }
    float straight[kMaxPath * 3];
    int ns = 0;
    if (dtStatusFailed(m_Query->findStraightPath(sa, end, path, n, straight, nullptr, nullptr, &ns, kMaxPath)) || ns == 0)
        return false;
    for (int i = 0; i < ns; ++i) out.push_back(V(straight + 3 * i));
    return true;
}

float NavMesh::PathLength(const glm::vec3& a, const glm::vec3& b) const {
    static thread_local std::vector<glm::vec3> pts; // reused: cover searches call this in a loop
    bool partial = false;
    if (!FindPath(a, b, pts, &partial) || partial) return -1.0f;
    float len = 0.0f;
    for (size_t i = 1; i < pts.size(); ++i) len += glm::length(pts[i] - pts[i - 1]);
    return len;
}

bool NavMesh::Walkable(const glm::vec3& a, const glm::vec3& b) const {
    if (!m_Query) return false;
    const float ext[3] = {0.5f, 2.0f, 0.5f};
    const float pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z};
    dtPolyRef ra = 0;
    float sa[3];
    if (dtStatusFailed(m_Query->findNearestPoly(pa, ext, m_Filter.get(), &ra, sa)) || !ra) return false;
    float t = 0.0f, normal[3];
    dtPolyRef path[64];
    int n = 0;
    if (dtStatusFailed(m_Query->raycast(ra, sa, pb, m_Filter.get(), &t, normal, path, &n, 64))) return false;
    return t >= 1.0f; // FLT_MAX when the ray reached its end
}

bool NavMesh::RandomPointNear(const glm::vec3& around, float radius, float r1, float r2, glm::vec3& out) const {
    if (!m_Query) return false;
    t_NavRng.seed((unsigned)(r1 * 1e6f) ^ ((unsigned)(r2 * 1e6f) << 1));
    const float ext[3] = {1.0f, 2.0f, 1.0f};
    const float c[3] = {around.x, around.y, around.z};
    dtPolyRef ref = 0, rref = 0;
    float s[3], pt[3];
    if (dtStatusFailed(m_Query->findNearestPoly(c, ext, m_Filter.get(), &ref, s)) || !ref) return false;
    if (dtStatusFailed(m_Query->findRandomPointAroundCircle(ref, s, radius, m_Filter.get(), NavRand, &rref, pt))) return false;
    out = V(pt);
    return true;
}

void NavMesh::BoundaryEdges(std::vector<Edge>& out) const {
    out.clear();
    if (!m_Mesh) return;
    const dtNavMesh* nm = m_Mesh;
    for (int t = 0; t < nm->getMaxTiles(); ++t) {
        const dtMeshTile* tile = nm->getTile(t);
        if (!tile || !tile->header) continue;
        for (int i = 0; i < tile->header->polyCount; ++i) {
            const dtPoly& p = tile->polys[i];
            if (p.getType() != DT_POLYTYPE_GROUND || !(p.flags & kWalkFlag)) continue;
            glm::vec3 centre(0.0f);
            for (int j = 0; j < p.vertCount; ++j) centre += V(&tile->verts[p.verts[j] * 3]);
            centre /= (float)std::max<int>(1, p.vertCount);
            for (int j = 0; j < p.vertCount; ++j) {
                if (p.neis[j] != 0) continue; // an internal edge, or a link to another tile
                const glm::vec3 a = V(&tile->verts[p.verts[j] * 3]);
                const glm::vec3 b = V(&tile->verts[p.verts[(j + 1) % p.vertCount] * 3]);
                glm::vec3 along = b - a;
                along.y = 0.0f;
                if (glm::length(along) < 1e-3f) continue;
                along = glm::normalize(along);
                glm::vec3 n(along.z, 0.0f, -along.x);
                const glm::vec3 mid = 0.5f * (a + b);
                if (glm::dot(n, glm::vec3(mid.x - centre.x, 0.0f, mid.z - centre.z)) < 0.0f) n = -n;
                out.push_back({a, b, n});
            }
        }
    }
    // Merge runs of collinear edges that share an end (Recast splits long walls into short pieces). The first
    // pair in (i, j) order that joins is merged, then the search starts again: done with the candidate lists
    // (edges whose end meets another's start, facing the same way) kept per edge and only the touched edges
    // refreshed after a merge - the plain restart-from-scratch double loop was cubic (~70 ms on the Sandbox).
    const size_t n = out.size();
    std::vector<char> alive(n, 1);
    std::vector<std::vector<int>> cand(n);
    std::vector<int> best(n, -1);
    std::unordered_map<long long, std::vector<int>> starts; // edge start points in 5 cm cells
    auto cellKey = [](const glm::vec3& p) {
        const long long x = (long long)std::floor(p.x * 20.0f), y = (long long)std::floor(p.y * 20.0f), z = (long long)std::floor(p.z * 20.0f);
        return x * 73856093ll ^ y * 19349663ll ^ z * 83492791ll;
    };
    for (size_t i = 0; i < n; ++i) starts[cellKey(out[i].A)].push_back((int)i);
    std::vector<std::vector<int>> rev(n); // rev[j]: the edges that list j as a candidate (stale entries are harmless)
    std::set<int> ready;                  // edges that have a merge waiting, smallest first
    auto findCands = [&](size_t i) {
        cand[i].clear();
        const glm::vec3& e = out[i].B;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto it = starts.find(cellKey(e + glm::vec3((float)dx, (float)dy, (float)dz) * 0.05f));
                    if (it == starts.end()) continue;
                    for (int j : it->second)
                        if ((size_t)j != i && !(glm::length(e - out[(size_t)j].A) > 0.02f) && !(glm::dot(out[i].Out, out[(size_t)j].Out) < 0.999f))
                            cand[i].push_back(j);
                }
        std::sort(cand[i].begin(), cand[i].end());
        cand[i].erase(std::unique(cand[i].begin(), cand[i].end()), cand[i].end());
        for (int j : cand[i]) rev[(size_t)j].push_back((int)i);
    };
    auto findBest = [&](size_t i) {
        best[i] = -1;
        ready.erase((int)i);
        for (int j : cand[i]) {
            if (!alive[(size_t)j]) continue;
            const glm::vec3 d0 = glm::normalize(out[i].B - out[i].A), d1 = glm::normalize(out[(size_t)j].B - out[(size_t)j].A);
            if (glm::dot(d0, d1) < 0.999f) continue;
            best[i] = j;
            ready.insert((int)i);
            return;
        }
    };
    for (size_t i = 0; i < n; ++i) { findCands(i); findBest(i); }
    while (!ready.empty()) {
        const size_t i = (size_t)*ready.begin();
        const int j = best[i];
        out[i].B = out[(size_t)j].B;
        alive[(size_t)j] = 0;
        ready.erase((int)j);
        findCands(i);
        // Whoever listed i (its direction changed) or j (gone) may now pair differently, and so may i itself.
        std::vector<int> touched = rev[i];
        touched.insert(touched.end(), rev[(size_t)j].begin(), rev[(size_t)j].end());
        touched.push_back((int)i);
        for (int x : touched)
            if (alive[(size_t)x]) findBest((size_t)x);
    }
    size_t w = 0;
    for (size_t i = 0; i < n; ++i)
        if (alive[i]) out[w++] = out[i];
    out.resize(w);
}

void NavMesh::DebugTriangles(std::vector<float>& out) const {
    out.clear();
    if (!m_Mesh) return;
    const dtNavMesh* nm = m_Mesh;
    for (int t = 0; t < nm->getMaxTiles(); ++t) {
        const dtMeshTile* tile = nm->getTile(t);
        if (!tile || !tile->header) continue;
        for (int i = 0; i < tile->header->polyCount; ++i) {
            const dtPoly& p = tile->polys[i];
            if (p.getType() != DT_POLYTYPE_GROUND || !(p.flags & kWalkFlag)) continue;
            for (int j = 2; j < p.vertCount; ++j)
                for (int k : {0, j - 1, j}) {
                    const float* v = &tile->verts[p.verts[k] * 3];
                    out.insert(out.end(), {v[0], v[1], v[2]});
                }
        }
    }
}

int NavMesh::PolyCount() const {
    if (!m_Mesh) return 0;
    const dtNavMesh* nm = m_Mesh;
    int n = 0;
    for (int t = 0; t < nm->getMaxTiles(); ++t)
        if (const dtMeshTile* tile = nm->getTile(t); tile && tile->header) n += tile->header->polyCount;
    return n;
}

int NavMesh::KeepReachable(const std::vector<glm::vec3>& seeds, const glm::vec3& extents) {
    if (!m_Mesh || !m_Query) return 0;
    std::set<dtPolyRef> reached;
    std::vector<dtPolyRef> open;
    for (const glm::vec3& s : seeds) {
        dtPolyRef ref = 0;
        float nearest[3];
        if (dtStatusSucceed(m_Query->findNearestPoly(&s.x, &extents.x, m_Filter.get(), &ref, nearest)) && ref && reached.insert(ref).second)
            open.push_back(ref);
    }
    if (open.empty()) return 0;
    while (!open.empty()) {
        const dtPolyRef ref = open.back();
        open.pop_back();
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        m_Mesh->getTileAndPolyByRefUnsafe(ref, &tile, &poly);
        for (unsigned int l = poly->firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
            const dtPolyRef next = tile->links[l].ref;
            if (next && reached.insert(next).second) open.push_back(next);
        }
    }
    int cut = 0;
    const dtNavMesh* nm = m_Mesh;
    for (int t = 0; t < nm->getMaxTiles(); ++t) {
        const dtMeshTile* tile = nm->getTile(t);
        if (!tile || !tile->header) continue;
        const dtPolyRef base = nm->getPolyRefBase(tile);
        for (int i = 0; i < tile->header->polyCount; ++i)
            if (!reached.count(base | (dtPolyRef)i) && (tile->polys[i].flags & kWalkFlag)) {
                m_Mesh->setPolyFlags(base | (dtPolyRef)i, 0);
                ++cut;
            }
    }
    return cut;
}

// --- NavCrowd ------------------------------------------------------------------------------------

NavCrowd::NavCrowd() = default;
NavCrowd::~NavCrowd() { Clear(); }

void NavCrowd::Clear() {
    dtFreeCrowd(m_Crowd);
    m_Crowd = nullptr;
    m_Mesh = nullptr;
}

bool NavCrowd::Init(const NavMesh& mesh, int maxAgents, float maxRadius) {
    Clear();
    if (!mesh.Valid()) return false;
    m_Crowd = dtAllocCrowd();
    if (!m_Crowd || !m_Crowd->init(maxAgents, maxRadius, mesh.Detour())) { Clear(); return false; }
    m_Mesh = &mesh;
    m_Crowd->getEditableFilter(0)->setIncludeFlags(kWalkFlag);
    // Good-quality avoidance (DetourCrowd's sample "high" preset).
    dtObstacleAvoidanceParams ap = *m_Crowd->getObstacleAvoidanceParams(0);
    ap.velBias = 0.5f;
    ap.adaptiveDivs = 7;
    ap.adaptiveRings = 3;
    ap.adaptiveDepth = 3;
    m_Crowd->setObstacleAvoidanceParams(0, &ap);
    return true;
}

int NavCrowd::Add(const glm::vec3& feet, float radius, float height, float maxSpeed, bool steer) {
    if (!m_Crowd) return -1;
    dtCrowdAgentParams p{};
    p.radius = radius;
    p.height = height;
    p.maxAcceleration = 14.0f;
    p.maxSpeed = maxSpeed;
    p.collisionQueryRange = radius * 12.0f;
    p.pathOptimizationRange = radius * 30.0f;
    p.separationWeight = 1.5f;
    p.updateFlags = steer ? (unsigned char)(DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO |
                                            DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION)
                          : (unsigned char)0;
    p.obstacleAvoidanceType = 0;
    p.queryFilterType = 0;
    const float pos[3] = {feet.x, feet.y, feet.z};
    return m_Crowd->addAgent(pos, &p);
}

void NavCrowd::Remove(int agent) {
    if (m_Crowd && agent >= 0) m_Crowd->removeAgent(agent);
}

bool NavCrowd::SetTarget(int agent, const glm::vec3& target) {
    if (!m_Crowd || agent < 0 || !m_Mesh) return false;
    const float ext[3] = {1.0f, 2.0f, 1.0f};
    const float c[3] = {target.x, target.y, target.z};
    dtPolyRef ref = 0;
    float near[3];
    if (dtStatusFailed(m_Mesh->Query()->findNearestPoly(c, ext, &m_Mesh->Filter(), &ref, near)) || !ref) return false;
    return m_Crowd->requestMoveTarget(agent, ref, near);
}

void NavCrowd::Stop(int agent) {
    if (m_Crowd && agent >= 0) m_Crowd->resetMoveTarget(agent);
}

void NavCrowd::SetMaxSpeed(int agent, float speed) {
    if (!m_Crowd || agent < 0) return;
    const dtCrowdAgent* ag = m_Crowd->getAgent(agent);
    if (!ag || !ag->active) return;
    dtCrowdAgentParams p = ag->params;
    if (std::abs(p.maxSpeed - speed) < 1e-3f) return;
    p.maxSpeed = speed;
    m_Crowd->updateAgentParameters(agent, &p);
}

void NavCrowd::Update(float dt) {
    if (m_Crowd && dt > 0.0f) m_Crowd->update(dt, nullptr);
}

glm::vec3 NavCrowd::Velocity(int agent) const {
    if (!m_Crowd || agent < 0) return glm::vec3(0.0f);
    const dtCrowdAgent* ag = m_Crowd->getAgent(agent);
    return ag && ag->active ? V(ag->vel) : glm::vec3(0.0f);
}

glm::vec3 NavCrowd::Position(int agent) const {
    if (!m_Crowd || agent < 0) return glm::vec3(0.0f);
    const dtCrowdAgent* ag = m_Crowd->getAgent(agent);
    return ag && ag->active ? V(ag->npos) : glm::vec3(0.0f);
}

void NavCrowd::Sync(int agent, const glm::vec3& feet, const glm::vec3& velocity) {
    if (!m_Crowd || agent < 0 || !m_Mesh) return;
    dtCrowdAgent* ag = m_Crowd->getEditableAgent(agent);
    if (!ag || !ag->active) return;
    const float pos[3] = {feet.x, feet.y, feet.z};
    if (ag->params.updateFlags == 0) {
        // Not steered (the player): put it where it is, moving as it moves, for the others to avoid.
        dtVcopy(ag->npos, pos);
        const float v[3] = {velocity.x, velocity.y, velocity.z};
        dtVcopy(ag->vel, v);
        dtVcopy(ag->nvel, v);
        dtVcopy(ag->dvel, v);
        return;
    }
    // Follow the capsule along the corridor (collision may have held it back or pushed it aside).
    if (dtVdist2DSqr(ag->npos, pos) > 1.0f) {
        const float ext[3] = {1.0f, 2.0f, 1.0f};
        dtPolyRef ref = 0;
        float near[3];
        if (dtStatusSucceed(m_Mesh->Query()->findNearestPoly(pos, ext, &m_Mesh->Filter(), &ref, near)) && ref) {
            ag->corridor.reset(ref, near);
            dtVcopy(ag->npos, near);
            if (ag->targetState == DT_CROWDAGENT_TARGET_VALID) {
                const float tgt[3] = {ag->targetPos[0], ag->targetPos[1], ag->targetPos[2]};
                m_Crowd->requestMoveTarget(agent, ag->targetRef, tgt);
            }
        }
        return;
    }
    ag->corridor.movePosition(pos, m_Mesh->Query(), &m_Mesh->Filter());
    dtVcopy(ag->npos, ag->corridor.getPos());
}

bool NavCrowd::HasTarget(int agent) const {
    if (!m_Crowd || agent < 0) return false;
    const dtCrowdAgent* ag = m_Crowd->getAgent(agent);
    return ag && ag->active && ag->targetState != DT_CROWDAGENT_TARGET_NONE && ag->targetState != DT_CROWDAGENT_TARGET_FAILED;
}

float NavCrowd::RemainingDistance(int agent) const {
    if (!m_Crowd || agent < 0) return 0.0f;
    const dtCrowdAgent* ag = m_Crowd->getAgent(agent);
    if (!ag || !ag->active || ag->targetState == DT_CROWDAGENT_TARGET_NONE) return 0.0f;
    float d = 0.0f;
    glm::vec3 prev = V(ag->npos);
    for (int i = 0; i < ag->ncorners; ++i) {
        const glm::vec3 c = V(&ag->cornerVerts[i * 3]);
        d += glm::length(glm::vec2(c.x - prev.x, c.z - prev.z));
        prev = c;
    }
    const glm::vec3 t = V(ag->targetPos);
    d += glm::length(glm::vec2(t.x - prev.x, t.z - prev.z));
    return d;
}

glm::vec3 NavCrowd::NextCorner(int agent) const {
    if (!m_Crowd || agent < 0) return glm::vec3(0.0f);
    const dtCrowdAgent* ag = m_Crowd->getAgent(agent);
    if (!ag || !ag->active) return glm::vec3(0.0f);
    return ag->ncorners > 0 ? V(&ag->cornerVerts[0]) : V(ag->npos);
}
