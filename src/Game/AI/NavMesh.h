#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
class dtCrowd;
class dtQueryFilter;

// Where the enemy AI can walk: a Recast navigation mesh built from the scene's solid geometry
// (PhysicsWorld::CollectStaticGeometry) when Play starts, and Detour queries on it - paths, the
// nearest walkable point, straight-line walkability, and the mesh's open edges (where walls and
// cover stand, for CoverSystem). No PhysX or GL in here, so it builds and runs in unit tests.
struct NavBuildSettings {
    float CellSize = 0.15f;      // metres, horizontal voxel size
    float CellHeight = 0.1f;     // metres, vertical
    float AgentHeight = 1.85f;
    float AgentRadius = 0.35f;
    float AgentClimb = 0.32f;    // a step the agent walks up (the capsule's step offset)
    float AgentMaxSlope = 48.0f; // degrees
    float RegionMinSize = 8.0f;  // cells^(1/2): smaller islands are dropped
    float RegionMergeSize = 20.0f;
    float EdgeMaxLen = 12.0f;    // metres
    float EdgeMaxError = 1.3f;   // cells
    float DetailSampleDist = 6.0f;
    float DetailSampleMaxError = 1.0f;
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // `verts` xyz, `tris` index triples, world space.
    bool Build(const std::vector<float>& verts, const std::vector<int>& tris, const NavBuildSettings& settings,
               std::string* error = nullptr);
    bool Valid() const { return m_Query != nullptr; }
    void Clear();
    // Save / load the built mesh, keyed by `hash` (of the input geometry and settings): Load fails
    // for any other hash, so a changed scene rebuilds.
    bool Save(const std::string& path, std::uint64_t hash) const;
    bool Load(const std::string& path, std::uint64_t hash);
    static std::uint64_t HashInput(const std::vector<float>& verts, const std::vector<int>& tris, const NavBuildSettings& s);

    // The walkable point nearest `p` within `extents` (half-size box); false when there is none.
    bool Closest(const glm::vec3& p, glm::vec3& out, const glm::vec3& extents = glm::vec3(1.0f, 2.0f, 1.0f)) const;
    // A walkable path from `a` to `b` (both snapped to the mesh) as corner points, `a`'s snap first.
    // False when either end is off the mesh or there's no route; `partial` true when the route ends
    // short of `b` (the nearest reachable point).
    bool FindPath(const glm::vec3& a, const glm::vec3& b, std::vector<glm::vec3>& out, bool* partial = nullptr) const;
    // The path's length, or a negative value when there is none.
    float PathLength(const glm::vec3& a, const glm::vec3& b) const;
    // Whether a walker can go straight from `a` to `b` along the mesh (no wall or drop between).
    bool Walkable(const glm::vec3& a, const glm::vec3& b) const;
    // A random walkable point within `radius` of `around` (reachable from it); false when none.
    bool RandomPointNear(const glm::vec3& around, float radius, float r1, float r2, glm::vec3& out) const;

    // The mesh's open edges - where it ends at a wall, a ledge or an obstacle - with the outward
    // normal (from the mesh toward what ends it), for cover. Merged collinear neighbours.
    struct Edge { glm::vec3 A, B, Out; };
    void BoundaryEdges(std::vector<Edge>& out) const;
    // The mesh's polygons as triangles (world xyz, 9 floats each), for the debug view.
    void DebugTriangles(std::vector<float>& out) const;
    int PolyCount() const;
    glm::vec3 BoundsMin() const { return m_BMin; }
    glm::vec3 BoundsMax() const { return m_BMax; }

    dtNavMesh* Detour() const { return m_Mesh; }
    dtNavMeshQuery* Query() const { return m_Query; }
    const dtQueryFilter& Filter() const { return *m_Filter; }

private:
    bool InitFromData(unsigned char* data, int size);
    dtNavMesh* m_Mesh = nullptr;
    dtNavMeshQuery* m_Query = nullptr;
    std::unique_ptr<dtQueryFilter> m_Filter;
    std::vector<unsigned char> m_Data; // the tile, for Save (Detour owns its own copy)
    glm::vec3 m_BMin{0.0f}, m_BMax{0.0f};
};

// Steering for every soldier at once (DetourCrowd): each follows its path corridor and steers
// round the others (and the player, an agent this code moves) by local avoidance. Positions are
// the characters' feet; the caller moves each capsule by Velocity() and hands the result back with
// Sync, so the capsule (which collides) stays the authority.
class NavCrowd {
public:
    NavCrowd();
    ~NavCrowd();
    NavCrowd(const NavCrowd&) = delete;
    NavCrowd& operator=(const NavCrowd&) = delete;

    bool Init(const NavMesh& mesh, int maxAgents = 12, float maxRadius = 0.6f);
    void Clear();
    bool Valid() const { return m_Crowd != nullptr; }
    // -1 on failure. `steer` false: an obstacle others avoid that this code moves (the player).
    int Add(const glm::vec3& feet, float radius, float height, float maxSpeed, bool steer = true);
    void Remove(int agent);
    bool SetTarget(int agent, const glm::vec3& target);
    void Stop(int agent);
    void SetMaxSpeed(int agent, float speed);
    void Update(float dt);
    glm::vec3 Velocity(int agent) const;      // what the crowd wants, m/s
    glm::vec3 Position(int agent) const;
    // Where the agent's capsule really is now (and for a non-steering agent, how fast it moves).
    void Sync(int agent, const glm::vec3& feet, const glm::vec3& velocity = glm::vec3(0.0f));
    bool HasTarget(int agent) const;
    // Distance left along the corridor to the target (straight-line from the last corner).
    float RemainingDistance(int agent) const;
    // The next corner the agent steers for (its position when none).
    glm::vec3 NextCorner(int agent) const;

private:
    dtCrowd* m_Crowd = nullptr;
    const NavMesh* m_Mesh = nullptr;
};
