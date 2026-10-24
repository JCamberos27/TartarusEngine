#pragma once

#include "SpiderLocomotion.h" // SurfaceQuery

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Where a spider can go: a graph over every surface of the level - floors, walls, ceilings, the tops and sides of
// furniture - not Recast's walkable floor. Nodes sit on a world-aligned lattice over the static collision's triangles
// (one every Spacing metres on each surface), each with its surface's normal and room above it for the body; links join
// neighbours a spider can crawl between in a straight line, across a surface or round a corner or edge onto the next.
// A* over it routes a spider to any point on any surface (floor to ceiling goes up the nearest wall); the mover
// (SpiderLocomotion.h) follows the route waypoint by waypoint.
namespace Spider {

struct NavSettings {
    float Spacing = 0.3f;       // metres between nodes on a surface
    float Room = 0.2f;          // the free space a node needs over its surface (a sphere this far out ...)
    float RoomRadius = 0.11f;   // ... of this radius must touch nothing)
    float LinkRadius = 0.5f;    // nodes this close may be linked
    float LinkLift = 0.1f;      // a link is clear when the line between its nodes, lifted this far off their surfaces, is
    float Footing = 0.14f;      // the surface must carry on this far round a node (at least three ways of four): not a
                                // floor slab's thin edge, through which a route would wrap into the void under it
};

// What a route prefers: each link costs its length times its far end's surface's weight (>= 1 keeps A* exact).
struct PathCosts {
    float Floor = 1.0f;         // normal within ~45 degrees of up
    float Wall = 1.0f;
    float Ceiling = 1.0f;       // normal within ~45 degrees of down
};

// Following a route (the graph's nodes' positions and normals, in order): drops the waypoints already reached (`at`
// advances), then aims as far along the route as it stays on the surface the body is on - a straight line across a
// floor, not a zigzag from node to node. A waypoint on the next surface is aimed at as it is: heading for it from this
// surface leads to the foot of the climb. On the route's last stretch it aims at `goal` itself.
glm::vec3 RouteAim(const std::vector<glm::vec3>& route, const std::vector<glm::vec3>& normals, int& at, const glm::vec3& pos,
                   const glm::vec3& up, const glm::vec3& goal);

class SurfaceGraph {
public:
    // Builds from world-space triangles (xyz `verts`, `tris` indices). `q` answers rays (is a link clear?) and occupancy
    // (is there room over a node?). With `seeds`, only the surfaces within `seedRadius` of one are kept.
    bool Build(const std::vector<float>& verts, const std::vector<int>& tris, const SurfaceQuery& q, const NavSettings& s,
               const std::vector<glm::vec3>* seeds = nullptr, float seedRadius = 0.0f);
    void Clear();
    bool Empty() const { return m_Pos.empty(); }

    int NodeCount() const { return (int)m_Pos.size(); }
    int LinkCount() const { return (int)m_Links.size() / 2; }
    const glm::vec3& Pos(int i) const { return m_Pos[(size_t)i]; }
    const glm::vec3& Normal(int i) const { return m_Normal[(size_t)i]; }
    // Node i's neighbours are Neighbour(k) for k in [FirstLink(i), FirstLink(i + 1)).
    int FirstLink(int i) const { return m_First[(size_t)i]; }
    int Neighbour(int k) const { return m_Links[(size_t)k]; }

    // The node nearest `p` within `maxDistance` (-1: none). With `up`, nodes whose surface faces away from it (a wall's
    // far side) are passed over.
    int Nearest(const glm::vec3& p, float maxDistance = 1.0f, const glm::vec3* up = nullptr) const;
    // A* from node `from` to node `to`: the nodes in order, both ends included. False when unreachable or when it would
    // expand more than `maxExpand` nodes.
    bool FindPath(int from, int to, const PathCosts& costs, std::vector<int>& out, int maxExpand = 80000) const;

    // A cache on disk, keyed by the input's hash (HashInput): a level's graph is baked once.
    static std::uint64_t HashInput(const std::vector<float>& verts, const std::vector<int>& tris, const NavSettings& s);
    bool Save(const std::string& path, std::uint64_t hash) const;
    bool Load(const std::string& path, std::uint64_t hash);

private:
    void BuildGrid();
    long long CellKey(const glm::vec3& p) const;

    float m_Cell = 0.5f;
    std::vector<glm::vec3> m_Pos, m_Normal;
    std::vector<int> m_First; // CSR: NodeCount + 1 entries
    std::vector<int> m_Links;
    // Spatial hash for Nearest: node ids sorted by cell.
    std::vector<std::pair<long long, int>> m_Grid;
    // A* scratch, reused between searches.
    mutable std::vector<float> m_G;
    mutable std::vector<int> m_From;
    mutable std::vector<std::uint32_t> m_Stamp;
    mutable std::uint32_t m_Search = 0;
};

} // namespace Spider
