#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

// The maths behind the decal editor tools (docs/DECALS.md, "Editor tools"): the Decal Brush's
// stroke stepper and the auto-grime pass's room analysis. No ImGui, no World: the editor feeds
// them cursor samples and a ray-cast callback, so both are deterministic (a seed plus the same
// input gives the same decals) and the unit tests drive them with synthetic rooms.
namespace DecalTools {

// A tiny deterministic generator (SplitMix32): the same on every platform, unlike the standard
// distributions, so a seed reproduces a stroke or a grime pass exactly.
struct Rng {
    uint32_t S;
    explicit Rng(uint32_t seed = 1u) : S(seed) {}
    uint32_t Next() {
        uint32_t z = (S += 0x9E3779B9u);
        z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
        z = (z ^ (z >> 13)) * 0xC2B2AE35u;
        return z ^ (z >> 16);
    }
    float Unit() { return (float)(Next() >> 8) * (1.0f / 16777216.0f); } // [0, 1)
    float Signed() { return Unit() * 2.0f - 1.0f; }                      // [-1, 1)
    float Range(float lo, float hi) { return lo + (hi - lo) * Unit(); }
};
uint32_t Mix(uint32_t a, uint32_t b); // a stable hash of two values (a per-dab or per-site seed)

// --- Decal Brush -------------------------------------------------------------------------------
struct BrushSettings {
    float Radius = 0.40f;          // metres: how far from the cursor a decal may land
    float Spacing = 0.50f;         // metres of stroke between dabs
    float Density = 1.0f;          // decals per dab; the fraction is a chance of one more
    float Size = 1.0f;             // times the picked decal's size
    float SizeJitter = 0.30f;      // +- fraction of Size
    float RotationJitter = 180.0f; // +- degrees of spin
    float Opacity = 1.0f;
    bool AlignToSurface = true;    // +Z follows the surface under the cursor; off, the stroke's first surface
    bool FollowStroke = false;     // the spin follows the stroke's direction, the jitter on top
};

struct BrushDab {
    glm::vec3 Point{0.0f};     // on the stroke's surface plane, already scattered
    glm::vec3 Normal{0, 1, 0}; // surface normal there; the editor re-casts onto the real surface
    glm::vec3 Right{1, 0, 0};  // the in-plane axis Spin is measured from
    glm::vec3 Direction{0.0f}; // the stroke's heading (unit; zero on the first dab)
    float Spin = 0.0f;         // radians about +Z
    float Scale = 1.0f;        // times the picked decal's size
    float PickRoll = 0.0f;     // [0, 1) for DecalPalette::Pick
    float Opacity = 1.0f;
    uint32_t Seed = 0;         // the decal's own seed
};

// Walks a polyline of cursor samples and drops a dab every Spacing metres of it. The dabs depend on
// the path, not on how often it was sampled: feeding a straight stroke in one step or in a hundred
// gives the same ones. Allocation-free.
class BrushStroke {
public:
    static constexpr int kMaxPerAdvance = 32; // a sample that jumps a long way yields at most this many

    void Begin(uint32_t seed, const BrushSettings& settings);
    // One cursor sample on the surface (point, unit normal). Appends the dabs it completes to `out`
    // (room for `cap`) and returns how many. The first sample drops the first dab.
    int Advance(const glm::vec3& point, const glm::vec3& normal, BrushDab* out, int cap);
    int Placed() const { return m_Placed; }
    bool Started() const { return m_Started; }

private:
    int Emit(const glm::vec3& point, const glm::vec3& normal, const glm::vec3& dir, BrushDab* out, int cap);
    BrushSettings m_S;
    uint32_t m_Seed = 1u;
    uint32_t m_Index = 0;
    int m_Placed = 0;
    bool m_Started = false;
    float m_Carry = 0.0f; // stroke length since the last dab
    glm::vec3 m_Last{0.0f}, m_LastNormal{0, 1, 0}, m_FirstNormal{0, 1, 0};
};

// --- Auto-grime --------------------------------------------------------------------------------
struct GrimeHit {
    glm::vec3 Point{0.0f};
    glm::vec3 Normal{0, 1, 0};
    float T = 0.0f;
};
// A ray cast against the scene's geometry (the editor's RaycastRenderables).
struct GrimeProbe {
    bool (*Cast)(void* user, const glm::vec3& origin, const glm::vec3& dir, float maxT, GrimeHit& out) = nullptr;
    void* User = nullptr;
    bool operator()(const glm::vec3& o, const glm::vec3& d, float maxT, GrimeHit& out) const {
        return Cast && Cast(User, o, d, maxT, out);
    }
};

struct GrimeSettings {
    float Spacing = 0.8f;   // metres between probe columns along a wall
    float Density = 1.0f;   // scales how many sites survive (0..2)
    int MaxSites = 160;
    uint32_t Kinds = 0xFFu; // DecalSites bits to place
};

// A place that wants grime. The decal's +Z is Normal, out of the surface.
struct GrimeSite {
    glm::vec3 Point{0.0f};
    glm::vec3 Normal{0, 1, 0};
    glm::vec3 Along{1, 0, 0}; // a horizontal direction along the surface
    uint32_t Kind = 0;        // one DecalSites bit
    float Height = 0.0f;      // metres above the floor
};

struct GrimeSummary {
    float FloorY = 0.0f, CeilingY = 0.0f;
    int Rays = 0;
};

// Probes the room inside [boundsMin, boundsMax] and appends the sites to `out` (cleared first):
//  - walls: rays from the room's centre line out to each of the four sides, in columns Spacing
//    apart, at every quarter metre of height. A column's depth profile against the wall's own
//    depth tells what it is: open to the floor = a door, open higher up = a window (the site is
//    the sill below), a shallow bump = a pipe (the site is the drip below it); the wall beside
//    a door gets the frame;
//  - wall bases (floor contact), the ceiling line, open wall;
//  - corners, from rays along each wall's ends that hit a wall at an angle;
//  - ceiling and floor patches, from rays up and down on a coarse grid.
// Same bounds, scene, settings and seed give the same sites.
GrimeSummary PlanGrime(const GrimeProbe& probe, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                       const GrimeSettings& settings, uint32_t seed, std::vector<GrimeSite>& out);

} // namespace DecalTools
