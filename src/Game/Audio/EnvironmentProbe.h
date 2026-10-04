#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <vector>

// What kind of space a shooter stands in, for choosing the gunshot tail.
// A handful of raycasts around the shooter (up, a diagonal ring and a horizontal ring) say how covered the space is
// (ceiling, overhead), how far its walls are and how much of the horizon is built up; those become four class weights
// that move smoothly between spaces, so walking through a doorway is a crossfade of tails, not a switch.

enum class SpaceClass { OutdoorOpen = 0, OutdoorUrban = 1, IndoorSmall = 2, IndoorLarge = 3 };
constexpr int kSpaceClassCount = 4;
const char* SpaceClassName(SpaceClass c);        // "outdoor_open", "outdoor_urban", "indoor_small", "indoor_large"
bool ParseSpaceClass(const char* name, SpaceClass& out);

// Tunables (the Weapon Audio component's "Environment" group fills these).
struct EnvironmentSettings {
    bool Enabled = true;               // off: every shot plays the generic tail (fire_tail)
    int RayCount = 12;                 // rays per probe: 1 up, a diagonal ring (45 degrees up) and a horizontal ring (>= 6)
    float MaxDistance = 40.0f;         // metres a ray looks; a miss counts as this far
    float IndoorCover = 0.7f;          // overhead cover (0..1) at which the space turns indoor (centre of the crossfade)
    float UrbanWall = 0.35f;           // share of the horizon with a wall within UrbanDistance at which outdoor turns urban
    float UrbanDistance = 25.0f;       // metres: a wall farther than this is not a building slap
    float LargeRoomDistance = 8.0f;    // mean wall distance (m) at which an indoor space turns from small to large
    float BlendFraction = 0.15f;       // crossfade half-width around the cover and wall thresholds (in 0..1 units)
    float BlendDistance = 0.3f;        // crossfade half-width around LargeRoomDistance (share of it)
    float RefreshInterval = 0.25f;     // seconds between probes of one shooter
    float RefreshMoveDistance = 1.0f;  // metres moved that probe again at once
    float MatchRadius = 2.0f;          // shooters the game does not name are told apart by position: shots this close are one shooter
    float TailGain[kSpaceClassCount] = {1.0f, 1.0f, 1.0f, 1.0f}; // per space class, on top of the tail layer's gain
    bool DebugDraw = false;            // keep the rays for EnvironmentProbe::DebugLines
};

struct EnvironmentRay {
    enum class Kind { Up, Diagonal, Horizontal };
    glm::vec3 Dir{0.0f, 1.0f, 0.0f};
    Kind Type = Kind::Up;
    bool Hit = false;
    float Distance = 0.0f;             // to the hit (only meaningful when Hit)
};

struct EnvironmentReading {
    bool Valid = false;
    float Cover = 0.0f;                // overhead cover: half the up ray, half the share of diagonal rays that hit
    float Wall = 0.0f;                 // share of horizontal rays that hit a wall within UrbanDistance
    float MeanWallDistance = 0.0f;     // mean horizontal ray length (a miss counts as MaxDistance)
    float Enclosure = 0.0f;            // share of all rays that hit
    float MeanDistance = 0.0f;         // mean length of all rays (a miss counts as MaxDistance)
    bool CeilingHit = false;
    float Weights[kSpaceClassCount] = {1.0f, 0.0f, 0.0f, 0.0f}; // indexed by SpaceClass; sum to 1, continuous in the features
    SpaceClass Dominant = SpaceClass::OutdoorOpen;
};

class EnvironmentProbe {
public:
    // Casts one ray against the solid world: true and the hit distance when it hits within maxDistance.
    using RayFn = std::function<bool(const glm::vec3& origin, const glm::vec3& dir, float maxDistance, float& hitDistance)>;
    // The PhysicsWorld's solid raycast (statics and kinematic bodies: not props, ragdolls or triggers).
    static RayFn PhysicsRays();

    // --- pure pieces (tested on their own) ---
    // The ray set for a probe: up, then the diagonal ring, then the horizontal ring (RayCount clamped to 6..64).
    static std::vector<EnvironmentRay> Rays(int rayCount);
    // Features and class weights from rays that have been cast (Hit / Distance filled in).
    static EnvironmentReading Classify(const std::vector<EnvironmentRay>& rays, const EnvironmentSettings& s);
    // The class weights of a feature set (the four of them sum to 1; each is continuous in every feature).
    static void Weights(float cover, float wall, float meanWallDistance, const EnvironmentSettings& s, float out[kSpaceClassCount]);

    void SetRayFn(RayFn fn) { m_Ray = std::move(fn); }
    // Probes now: casts the rays from `origin` and classifies.
    EnvironmentReading Probe(const glm::vec3& origin, const EnvironmentSettings& s, std::vector<EnvironmentRay>* raysOut = nullptr);
    // The shooter's cached reading, refreshed when it is older than RefreshInterval, the shooter moved more than
    // RefreshMoveDistance since the last probe, or it has none. `shooter` 0 = not named: matched by position (MatchRadius).
    // `now` is sim time in seconds. `refreshed` says whether this call probed.
    const EnvironmentReading& Query(std::uint32_t shooter, const glm::vec3& pos, double now, const EnvironmentSettings& s, bool* refreshed = nullptr);
    void Clear();

    // Overlay lines (7 floats per vertex: xyz rgba, two vertices per line) for the shooters whose last probe had DebugDraw on.
    void DebugLines(std::vector<float>& out) const;

    struct Stats {
        int Refreshes = 0;
        int Rays = 0;
        double TotalMicros = 0.0, MaxMicros = 0.0;
    };
    const Stats& GetStats() const { return m_Stats; }
    int Shooters() const { return (int)m_Entries.size(); }

private:
    struct Entry {
        std::uint32_t Id = 0;
        glm::vec3 LastSeen{0.0f}, ProbedAt{0.0f};
        double ProbeTime = -1e9, LastUsed = 0.0;
        EnvironmentReading Reading;
        std::vector<EnvironmentRay> Rays;
        bool Debug = false;
        float MaxDistance = 0.0f;
    };
    RayFn m_Ray;
    std::vector<Entry> m_Entries;
    Stats m_Stats;
};
