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
    // component >= 0: only nodes in that connected piece of the graph (Component), so a pick is somewhere it can get to.
    int Nearest(const glm::vec3& p, float maxDistance = 1.0f, const glm::vec3* up = nullptr, int component = -1) const;
    // Which connected piece of the graph node i is in: the rooms a spider can crawl between are one; a void sealed off
    // behind a wall, above a ceiling or under a floor is another.
    int Component(int i) const { return m_Comp[(size_t)i]; }
    // Nodes something movable stands on now (an open door's leaf against the wall): routes don't pass them. Counted, so
    // two leaves over one node both have to leave. Runtime only - never baked, saved or hashed.
    void Block(int i, int delta);
    bool Blocked(int i) const { return !m_Blocked.empty() && m_Blocked[(size_t)i] > 0; }
    // Extra route cost (metres) for passing node `i`, added to: a shut door's doorway costs about what pushing it open
    // takes, so a route takes an open way round when there is a fair one. Runtime only, like Block.
    void AddPenalty(int i, float metres);
    float Penalty(int i) const { return m_Penalty.empty() ? 0.0f : m_Penalty[(size_t)i]; }
    // A* from node `from` to node `to`: the nodes in order, both ends included. False when unreachable or when it would
    // expand more than `maxExpand` nodes.
    bool FindPath(int from, int to, const PathCosts& costs, std::vector<int>& out, int maxExpand = 80000) const;

    // A cache on disk, keyed by the input's hash (HashInput): a level's graph is baked once. The key holds the builder's
    // version too - bump kBuildVersion with any change to what Build makes, or every level keeps the graph it baked before.
    // (3: baked against the static world alone; before, a door, the player or a spider at the bake changed the graph.)
    // (4: a link is clear only if clear both ways - a one-sided ceiling or floor plane let links through from behind.)
    static constexpr std::uint32_t kBuildVersion = 4;
    static std::uint64_t HashInput(const std::vector<float>& verts, const std::vector<int>& tris, const NavSettings& s,
                                   std::uint32_t buildVersion = kBuildVersion);
    // `hash` with the level's creature barriers mixed in: they cut links, so a graph baked without them isn't this one.
    static std::uint64_t HashBarriers(std::uint64_t hash, const std::vector<Barrier>& barriers);
    bool Save(const std::string& path, std::uint64_t hash) const;
    bool Load(const std::string& path, std::uint64_t hash);

private:
    void BuildGrid();
    void LabelComponents();
    long long CellKey(const glm::vec3& p) const;

    float m_Cell = 0.5f;
    std::vector<glm::vec3> m_Pos, m_Normal;
    std::vector<int> m_First; // CSR: NodeCount + 1 entries
    std::vector<int> m_Links;
    std::vector<int> m_Comp; // per node, its connected piece (LabelComponents)
    std::vector<int> m_Blocked; // per node, how many movable things stand on it now (Block)
    std::vector<float> m_Penalty; // per node, extra route cost now (AddPenalty)
    // Spatial hash for Nearest: node ids sorted by cell.
    std::vector<std::pair<long long, int>> m_Grid;
    // A* scratch, reused between searches.
    mutable std::vector<float> m_G;
    mutable std::vector<int> m_From;
    mutable std::vector<std::uint32_t> m_Stamp;
    mutable std::uint32_t m_Search = 0;
};

} // namespace Spider
