#pragma once

#include <glm/glm.hpp>

#include <unordered_map>
#include <vector>

class NavMesh;

// Where a soldier can take cover: points along the navigation mesh's open edges where something
// solid stands just beyond - low cover (a crate, a low wall: crouch behind, rise to shoot over it) or
// high cover (a wall, a pillar: stand behind it and step out past its end to shoot). Found once
// when Play starts, by sampling the edges and probing outward at knee and head height.
struct CoverPoint {
    glm::vec3 Pos{0.0f};          // where the soldier stands (feet, on the mesh)
    glm::vec3 Normal{0.0f};       // flat, from the soldier toward the cover
    bool High = false;            // stands taller than the soldier's head
    bool Peek[2] = {false, false};// high cover: can step out on its left [0] / right [1] (facing the cover)
    glm::vec3 PeekPos[2]{};       // where to step to for that
    int ClaimedBy = -1;           // NPC index holding it
    float LastUsed = -1e9f;       // when it was last given up
};

// What the probes found at one sample (the pure part of the classification, for tests).
struct CoverProbe {
    bool BlockedKnee = false;     // something within reach at 0.85 m
    bool BlockedHead = false;     // ... and at 1.55 m
    bool ClearPast[2] = {false, false}; // head height is clear a step to the left / right
};
// Whether the probe is cover, and which kind; `out` gets High / Peek.
bool ClassifyCover(const CoverProbe& probe, CoverPoint& out);

class CoverSystem {
public:
    // Samples `nav`'s open edges (PhysicsWorld solid-world rays). Returns the points found.
    int Build(const NavMesh& nav);
    void Clear();
    const std::vector<CoverPoint>& Points() const { return m_Points; }
    CoverPoint& Point(int i) { return m_Points[(size_t)i]; }
    // Indices of points within `radius` of `centre` (flat distance).
    void Query(const glm::vec3& centre, float radius, std::vector<int>& out) const;
    bool Claim(int index, int npc, float now);
    void Release(int npc, float now);
    int ClaimOf(int npc) const;

    // Whether standing / crouching at `p` (cover facing `normal`) hides `height` metres up from `threatEye`:
    // a solid-world ray from the eye toward that point is stopped short of it.
    static bool Shielded(const glm::vec3& feet, float height, const glm::vec3& threatEye);

private:
    std::vector<CoverPoint> m_Points;
    std::unordered_map<long long, std::vector<int>> m_Grid; // 4 m cells
    static long long Cell(float x, float z);
};
